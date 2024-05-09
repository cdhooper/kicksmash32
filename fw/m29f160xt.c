/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2024.
 *
 * ---------------------------------------------------------------------
 *
 * M29F160xT / MX29F800x specific code (read, write, erase, status, etc).
 */

#include "board.h"
#include "main.h"
#include "printf.h"
#include "uart.h"
#include <stdbool.h>
#include <string.h>
#include "m29f160xt.h"
#include "irq.h"
#include "utils.h"
#include "timer.h"
#include "gpio.h"
#include "usb.h"
#include "smash_cmd.h"
#include "pin_tests.h"
#include "config.h"
#include "kbrst.h"
#include "msg.h"
#include <libopencm3/stm32/dma.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/timer.h>
#include <libopencm3/cm3/nvic.h>

#undef  DEBUG_SIGNALS

#define EE_DEVICE_SIZE          (1 << 20)   // 1M words (16-bit words)
#define MX_ERASE_SECTOR_SIZE    (32 << 10)  // 32K-word blocks

#define MX_STATUS_FAIL_PROGRAM  0x10  // Status code - failed to program
#define MX_STATUS_FAIL_ERASE    0x20  // Status code - failed to erase
#define MX_STATUS_COMPLETE      0x80  // Status code - operation complete

#define EE_MODE_ERASE           0     // Waiting for erase to complete
#define EE_MODE_PROGRAM         1     // Waiting for program to complete

#define EE_STATUS_NORMAL        0     // Normal status
#define EE_STATUS_ERASE_TIMEOUT 1     // Erase timeout
#define EE_STATUS_PROG_TIMEOUT  2     // Program timeout
#define EE_STATUS_ERASE_FAILURE 3     // Erase failure
#define EE_STATUS_PROG_FAILURE  4     // Program failure

/*
 * EE_MODE_32      = 32-bit flash
 * EE_MODE_16_LOW  = 16-bit flash low device (bits 0-15)
 * EE_MODE_16_HIGH = 16-bit flash high device (bits 16-31)
 */
uint            ee_mode         = EE_MODE_32;
uint            ee_default_mode = EE_MODE_32;
static uint32_t ee_cmd_mask;
static uint32_t ee_addr_shift;
static uint32_t ee_status = EE_STATUS_NORMAL;  // Status from program/erase

static uint32_t ticks_per_15_nsec;
static uint32_t ticks_per_20_nsec;
static uint32_t ticks_per_30_nsec;
static uint64_t ee_last_access = 0;
static bool     ee_enabled = false;

/*
 * address_output
 * --------------
 * Writes the specified value to the address output pins.
 */
static void
address_output(uint32_t addr)
{
    GPIO_ODR(SOCKET_A0_PORT)   = addr & 0xffff;           // Set A0-A12
    GPIO_BSRR(SOCKET_A13_PORT) = 0x00fe0000 |             // Clear A13-A19
                                 ((addr >> 12) & 0x00fe); // Set A13-A19
}

/*
 * address_input
 * -------------
 * Returns the current value present on the address pins.
 */
static uint32_t
address_input(void)
{
    uint32_t addr = GPIO_IDR(SOCKET_A0_PORT);
    addr |= ((GPIO_IDR(SOCKET_A16_PORT) & 0x00f0) << (16 - 4));
    return (addr);
}

/*
 * ee_address_override
 * -------------------
 * Override the Flash A17, A18 and A19 address
 *
 * override = 0  Record new override
 * override = 1  Temporarily disable override
 * override = 2  Restore previous override
 *
 * bits
 *   0    1=Drive A17
 *   1    1=Drive A18
 *   2    1=Drive A19
 *   3    Unused
 *   4    A17 driven value
 *   5    A18 driven value
 *   6    A19 driven value
 *   7    Unused
 */
void
ee_address_override(uint8_t bits, uint override)
{
    uint           bit;
    static uint8_t old  = 0xff;
    static uint8_t last = 0;
    static const uint32_t ports[] =
                          { FLASH_A17_PORT, FLASH_A18_PORT, FLASH_A19_PORT };
    static const uint32_t pins[]  =
                          { FLASH_A17_PIN,  FLASH_A18_PIN,  FLASH_A19_PIN };

    switch (override) {
        default:
        case 0:  // Record new override
            break;
        case 1:  // Temporily disable override
            if (old == 0xff)
                old = last;
            bits = 0;
            break;
        case 2:  // Restore previous override
            if (old == 0xff)
                return;
            bits = old;
            old = 0xff;
            break;
    }
    if (bits == last)
        return;
    last = bits;

    for (bit = 0; bit < 3; bit++) {
        uint32_t port  = ports[bit];
        uint16_t pin   = pins[bit];
        if (bits & BIT(bit)) {
            /* Drive address pin */
            uint     shift = (bits & BIT(bit + 4)) ? 0 : 16;
            gpio_setmode(port, pin, GPIO_SETMODE_OUTPUT_PPULL_2);
            GPIO_BSRR(port) = pin << shift;
#undef ADDR_OVERRIDE_DEBUG
#ifdef ADDR_OVERRIDE_DEBUG
            if (override == 0)             // XXX: Remove output when debugged
                printf(" A%u=%u", 17 + bit, !shift);
#endif
        } else {
            /* Disable drive: Weak pull down pin */
            gpio_setmode(port, pin, GPIO_SETMODE_INPUT_PULLUPDOWN);
            GPIO_BSRR(port) = pin << 16;
#ifdef ADDR_OVERRIDE_DEBUG
            if (override == 0)             // XXX: Remove output when debugged
                printf(" !A%u", 17 + bit);
#endif
        }
    }
#ifdef ADDR_OVERRIDE_DEBUG
    if (override == 0)  // XXX: Remove output when debugged
        printf("\n");
#endif
}

/*
 * address_output_enable
 * ---------------------
 * Enables the address pins for output.
 */
static void
address_output_enable(void)
{
    ee_address_override(0, 1);  // Suspend A19-A18-A17 override

    /* A0-A12=PC0-PC12 A13-A19=PA1-PA7 */
    GPIO_CRL(SOCKET_A0_PORT)  = 0x11111111;  // Output Push-Pull
    GPIO_CRH(SOCKET_A0_PORT)  = 0x00011111;  // Not PC13-PC15 (weak drive)
    GPIO_CRL(SOCKET_A13_PORT) = 0x11111118;  // PA0=SOCKET_OE = Input PU
}

/*
 * address_output_disable
 * ----------------------
 * Reverts the address pins back to input (don't drive).
 */
void
address_output_disable(void)
{
    /* A0-A12=PC0-PC12 A13-A19=PA1-PA7 */
    if (board_is_standalone) {
        GPIO_CRL(SOCKET_A0_PORT)   = 0x88888888;  // Input Pull-Up / Pull-Down
        GPIO_CRH(SOCKET_A0_PORT)   = 0x44488888;  // Not PC13-PC15
        GPIO_CRL(SOCKET_A13_PORT)  = 0x88888888;  // PA0=SOCKET_OE = Input PU
        GPIO_ODR(SOCKET_A0_PORT)   = 0xffff;      // Pull up A0-A12
        GPIO_BSRR(SOCKET_A13_PORT) = 0x0000003e;  // Pull up A13-A17
    } else {
        GPIO_CRL(SOCKET_A0_PORT)   = 0x44444444;  // Input
        GPIO_CRH(SOCKET_A0_PORT)   = 0x44444444;
        GPIO_CRL(SOCKET_A13_PORT)  = 0x44444448;  // PA0=SOCKET_OE = Input PU
    }
    ee_address_override(0, 2);  // Restore previous A19-A18-A17 override
}

/*
 * data_output
 * -----------
 * Writes the specified value to the data output pins.
 */
void
data_output(uint32_t data)
{
    GPIO_ODR(FLASH_D0_PORT)  = data;                      // Set D0-D15
    GPIO_ODR(FLASH_D16_PORT) = (data >> 16);              // Set D16-D31
}

/*
 * data_input
 * ----------
 * Returns the current value present on the data pins.
 */
static uint32_t
data_input(void)
{
    /*
     * Board Rev 2+
     *
     * D0-D15  = PD0-PD15
     * D16-D15 = PE0-PE15
     */
    return (GPIO_IDR(FLASH_D0_PORT) | (GPIO_IDR(FLASH_D16_PORT) << 16));
}

/*
 * data_output_enable
 * ------------------
 * Enables the data pins for output.
 */
void
data_output_enable(void)
{
    GPIO_CRL(FLASH_D0_PORT)  = 0x11111111; // Output Push-Pull
    GPIO_CRH(FLASH_D0_PORT)  = 0x11111111;
    GPIO_CRL(FLASH_D16_PORT) = 0x11111111;
    GPIO_CRH(FLASH_D16_PORT) = 0x11111111;
}

/*
 * data_output_disable
 * -------------------
 * Reverts the data pins back to input (don't drive).
 */
void
data_output_disable(void)
{
    /* D0-D15 = PD0-PD15, D16-D31=PE0-PE15 */
    GPIO_CRL(FLASH_D0_PORT)  = 0x88888888; // Input Pull-Up / Pull-Down
    GPIO_CRH(FLASH_D0_PORT)  = 0x88888888;
    GPIO_CRL(FLASH_D16_PORT) = 0x88888888;
    GPIO_CRH(FLASH_D16_PORT) = 0x88888888;
}

/*
 * we_output
 * ---------
 * Drives the WE# (flash write enable) pin with the specified value.
 */
static void
we_output(uint value)
{
#ifdef DEBUG_SIGNALS
    printf(" WE=%d", value);
#endif
    gpio_setv(FLASH_WE_PORT, FLASH_WE_PIN, value);
}

/*
 * we_enable
 * ---------
 * Enables or disables WE# pin output
 */
static void
we_enable(uint value)
{
    gpio_setmode(FLASH_WE_PORT, FLASH_WE_PIN,
                 (value != 0) ? GPIO_SETMODE_OUTPUT_PPULL_50 :
                                GPIO_SETMODE_INPUT_PULLUPDOWN);
}

/*
 * oe_output
 * ---------
 * Drives the OE# (flash output enable) pin with the specified value.
 */
void
oe_output(uint value)
{
#ifdef DEBUG_SIGNALS
    printf(" OE=%d", value);
#endif
    gpio_setv(FLASH_OE_PORT, FLASH_OE_PIN, value);
}

/*
 * oe_output_enable
 * ----------------
 * Enable drive of the FLASH_OE pin, which is flash output enable OE#.
 */
void
oe_output_enable(void)
{
    /* FLASH_OE = PB13 */
    GPIO_CRH(FLASH_OE_PORT) = (GPIO_CRH(FLASH_OE_PORT) & 0xff0fffff) |
                              0x00100000;  // Output

//  gpio_setmode(FLASH_OE_PORT, FLASH_OE_PIN, GPIO_SETMODE_OUTPUT_PPULL_50);
}

/*
 * oe_output_disable
 * -----------------
 * Disable drive of the FLASH_OE pin, which is flash output enable OE#.
 */
void
oe_output_disable(void)
{
    /* FLASH_OE = PB13 */
    GPIO_CRH(FLASH_OE_PORT) = (GPIO_CRH(FLASH_OE_PORT) & 0xff0fffff) |
                              0x00400000;  // Input

//  gpio_setmode(FLASH_OE_PORT, FLASH_OE_PIN, GPIO_SETMODE_INPUT_PULLUPDOWN);
}

void
ee_set_mode(uint new_mode)
{
    ee_mode = new_mode;
    if (ee_mode == EE_MODE_32) {
        ee_cmd_mask = 0xffffffff;  // 32-bit
        ee_addr_shift = 2;
        msg_mode(32);
    } else if (ee_mode == EE_MODE_16_LOW) {
        ee_cmd_mask = 0x0000ffff;  // 16-bit low
        ee_addr_shift = 1;
        msg_mode(16);
    } else {
        ee_cmd_mask = 0xffff0000;  // 16-bit high
        ee_addr_shift = 1;
        msg_mode(16);
    }
}

/*
 * ee_enable
 * ---------
 * Enables drivers to the EEPROM device, including OE# and WE#.
 * Data lines are left floating.
 */
void
ee_enable(void)
{
    if (ee_enabled)
        return;
    address_output(0);
    address_output_enable();
    we_output(1);  // WE# disabled
    oe_output(1);
    oe_output_enable();
    data_output_disable();
    ee_enabled = true;
    ee_read_mode();
    ee_last_access = timer_tick_get();
    ee_set_mode(ee_mode);
}

/*
 * ee_disable
 * ----------
 * Tri-states all address and data lines to the device.
 */
void
ee_disable(void)
{
    we_output(1);
    oe_output_disable();
    address_output_disable();
    data_output_disable();
    timer_delay_usec(50);
    ee_enabled = false;
}

/*
 * ee_read_word
 * ------------
 * Performs a single address read, with appropriate timing.
 *
 * M29F160xT read timing waveform
 *
 *   Address  ####<------Address Stable------->#########
 *            ________                        __________
 *   CE#              \______________________/
 *            ______________                  __________
 *   OE#                    \________________/
 *
 *            High-Z                               High-Z
 *   DATA-OUT ~~~~~~~~~~~~~~~~~~~<-Data Out Valid->~~~~~~
 *
 * M29F160xT timing notes
 *   tRC  - Address valid to Next Address valid  (min 55ns)
 *   tACC - Address stable to Data Out Valid     (max 55ns)
 *   tLZ  - CE Low to Output Transition          (max 0ns)
 *   tCE  - CE low to Data Out Valid             (max 55ns)
 *   tOLZ - OE low to Output Transition          (min 0ns)
 *   tOE  - OE low to Data Out Valid             (max 20ns)
 *   tHZ  - CE high to Data OUT High-Z           (max 15ns)
 *   tDF  - OE high to Data OUT High-Z           (max 15ns)
 *   tOH  - OE high to Data Out no longer valid  (min 0ns)
 */
static void
ee_read_word(uint32_t addr, uint32_t *data)
{
    address_output(addr);
    address_output_enable();
    oe_output(0);
    oe_output_enable();
    timer_delay_ticks(ticks_per_20_nsec);  // Wait for tOE
    *data = data_input();
    oe_output(1);
    oe_output_disable();
    timer_delay_ticks(ticks_per_15_nsec);  // Wait for tDF
#ifdef DEBUG_SIGNALS
    printf(" RWord[%lx]=%08lx", addr, *data);
#endif
}

/*
 * ee_read
 * -------
 * Reads the specified number of words from the EEPROM device.
 */
int
ee_read(uint32_t addr, void *datap, uint count)
{
    if (addr + count > EE_DEVICE_SIZE)
        return (1);

    if (ee_mode == EE_MODE_32) {
        uint32_t *data = datap;

        disable_irq();
        while (count-- > 0)
            ee_read_word(addr++, data++);
        enable_irq();
    } else {
        uint16_t *data = datap;

        disable_irq();
        while (count-- > 0) {
            uint32_t value;
            ee_read_word(addr++, &value);
            if (ee_mode == EE_MODE_16_LOW)
                (*data++) = (uint16_t) value;
            else
                (*data++) = value >> 16;
        }
        enable_irq();
    }

    return (0);
}

/*
 * ee_write_word
 * -------------
 * Performs a single address write, with appropriate timing.
 *
 * M29F160xT write timing waveform
 *
 *   Address  ####<------Address Stable----->##########
 *            ___                                     _
 *   CE#         \___________________________________/
 *            ________                        _________
 *   WE#              \______________________/
 *            _________________________________________
 *   OE#
 *   ADDR     High-Z<~~~~~~Valid~~~~~~~~~~~~~~~~>High-Z
 *   DATA     ~~~~~~~~~~~~~<----Data In Valid--->~~~~~~
 *
 * Address is latched on the falling edge of CE/WE.
 * Data is latched on the rising edge of CE/WE
 *
 * M29F160xT timing notes
 *   tWC   - Address Valid to Next Address Valid  (min 55ns)
 *   tCS   - CE Low to WE Low                     (min 0ns)
 *   tWP   - WE Low to WE High                    (min 30ns)
 *   tDS   - Input Valid to WE High               (min 20ns)
 *   tDH   - WE High to Input Transition          (min 0ns)
 *   tCH   - WE High to CE High                   (min 0us)
 *   tWPH  - WE High to WE Low                    (min 15us)
 *   tAS   - Address Valid to WE Low              (min 0us)
 *   tAH   - WE Low to address invalid            (min 30us)
 *   tGHWL - OE High to WE Low                    (min 0us)
 *   tOEH  - WE High to OE Low                    (min 0us)
 *   tBUSY - Program/Erase Valid to RB# Low       (max 20us)
 *
 * M29F160xT
 *   Address lines are latched at the falling edge of WE#
 *   Data lines are latched at the rising edge of WE#
 *   OE# must remain high during the entire bus operation
 */
static void
ee_write_word(uint32_t addr, uint32_t data)
{
#ifdef DEBUG_SIGNALS
    printf(" WWord[%lx]=%08lx", addr, data);
#endif
    address_output(addr);
    oe_output(1);
    oe_output_enable();

    we_enable(1);
    we_output(0);
    data_output(data & ee_cmd_mask);
    data_output_enable();

    timer_delay_ticks(ticks_per_30_nsec);  // tWP=30ns tDS=20ns
    we_output(1);
    data_output_disable();
    oe_output_disable();
}

/*
 * ee_cmd
 * ------
 * Sends a command to the EEPROM device.
 */
void
ee_cmd(uint32_t addr, uint32_t cmd)
{
    uint32_t ccmd = cmd;

    ee_last_access = timer_tick_get();

    switch (ee_mode) {
        case EE_MODE_32:
            if ((cmd >> 16) == 0)
                cmd |= (cmd << 16);
            break;
        case EE_MODE_16_LOW:
            break;
        case EE_MODE_16_HIGH:
            if ((cmd >> 16) == 0)
                cmd |= (cmd << 16);
            break;
    }

    if ((ccmd & 0xffff) == 0)
        ccmd >>= 16;

    /* Check for a command which doesn't require an unlock sequence */
    switch (ccmd & 0xffff) {
        case 0x98:  // Read CFI Query
        case 0xf0:  // Read/Reset
        case 0xb0:  // Erase Suspend
        case 0x30:  // Erase Resume
            ee_write_word(addr, cmd);
            goto finish;
    }

    disable_irq();
    ee_write_word(0x00555, 0x00aa00aa);
    ee_write_word(0x002aa, 0x00550055);
    ee_write_word(addr, cmd);
    enable_irq();

finish:
    timer_delay_usec(2);   // Wait for command to complete
}

/*
 * ee_status_clear
 * ---------------
 * Resets any error status on the MX29F160xT part(s), returning the
 * flash array to normal read mode.
 */
void
ee_status_clear(void)
{
    ee_cmd(0x00000, 0x00f000f0);
    ee_read_mode();
}

/*
 * ee_wait_for_done_status
 * -----------------------
 * Will poll the EEPROM part waiting for an erase or programming cycle to
 * complete. For the MX29F160xT, this is done by watching whether Q6
 * continues to toggle. Indefinite toggling indicates timeout.
 */
static int
ee_wait_for_done_status(uint32_t timeout_usec, int verbose, int mode)
{
    uint     report_time = 0;
    uint64_t start;
    uint64_t now;
    uint32_t status;
    uint32_t cstatus;
    uint32_t lstatus = 0;
    uint64_t usecs = 0;
    int      same_count = 0;
    int      see_fail_count = 0;

    start = timer_tick_get();
    while (usecs < timeout_usec) {
        now = timer_tick_get();
        usecs = timer_tick_to_usec(now - start);
        ee_read_word(0x000000000, &status);
        status &= ee_cmd_mask;

        cstatus = status;
        /* Filter out checking of status which is already done */
        if (((cstatus ^ lstatus) & 0x0000ffff) == 0)
            cstatus &= ~0x0000ffff;
        if (((cstatus ^ lstatus) & 0xffff0000) == 0)
            cstatus &= ~0xffff0000;

        if (status == lstatus) {
            if (same_count++ >= 1) {
                /* Same for 2 tries */
                if (verbose) {
                    report_time = usecs / 1000000;
                    printf("\r%08lx %c%c %u sec", status,
                           ((cstatus & 0xffff0000) == 0) ? '.' : '?',
                           ((cstatus & 0x0000ffff) == 0) ? '.' : '?',
                           report_time);
                    printf("    Done\n");
                }
                ee_status = EE_STATUS_NORMAL;
                return (0);
            }
        } else {
            if (same_count)
                printf("S");
            same_count = 0;
            lstatus = status;
        }

        if (cstatus & (BIT(5) | BIT(5 + 16)))  // Program / erase failure
            if (see_fail_count++ > 5)
                break;

        if (verbose) {
            /* Update once a second */
            if (report_time < usecs / 1000000) {
                report_time = usecs / 1000000;
                printf("\r%08lx %c%c %u sec", status,
                       ((cstatus & 0xffff0000) == 0) ? '.' : '?',
                       ((cstatus & 0x0000ffff) == 0) ? '.' : '?',
                       report_time);
            }
        }
    }
    if (verbose) {
        report_time = usecs / 1000000;
        printf("\r%08lx %c%c %u.%03u sec", status,
               ((cstatus & 0xffff0000) == 0) ? '.' : '?',
               ((cstatus & 0x0000ffff) == 0) ? '.' : '?',
               report_time, (uint) ((usecs - report_time * 1000000) / 1000));
    }

    if (cstatus & (BIT(5) | BIT(5 + 16))) {
        /* Program / erase failure */
        ee_status = (mode == EE_MODE_ERASE) ? EE_STATUS_ERASE_FAILURE :
                                              EE_STATUS_PROG_FAILURE;
        printf("    %s Failure\n",
               (mode == EE_MODE_ERASE) ? "Erase" : "Program");
        ee_status_clear();
        return (1);
    }

    ee_status = (mode == EE_MODE_ERASE) ? EE_STATUS_ERASE_TIMEOUT :
                                          EE_STATUS_PROG_TIMEOUT;
    printf("    %s Timeout\n",
           (mode == EE_MODE_ERASE) ? "Erase" : "Program");
    ee_status_clear();
    return (1);
}

/*
 * ee_program_word
 * ---------------
 * Writes a single word to the EEPROM.
 */
static int
ee_program_word(uint32_t addr, uint32_t word)
{
    disable_irq();
    ee_write_word(0x00555, 0x00aa00aa);
    ee_write_word(0x002aa, 0x00550055);
    ee_write_word(0x00555, 0x00a000a0);
    ee_write_word(addr, word);
    enable_irq();

    return (ee_wait_for_done_status(360, 0, EE_MODE_PROGRAM));
}

/*
 * ee_write() will program <count> words to EEPROM, starting at the
 *            specified address. It automatically uses page program
 *            to speed up programming. After each page is written,
 *            it is read back to verify that programming was successful.
 */
int
ee_write(uint32_t addr, void *datap, uint count)
{
    int      rc;
    uint     wordsize = (ee_mode == EE_MODE_32) ? 4 : 2;
    uint8_t *data = datap;
    uint32_t value;
    uint32_t rvalue;
    uint32_t xvalue;

    if (addr + count > EE_DEVICE_SIZE)
        return (1);

    while (count > 0) {
        int try_count = 0;
try_again:
        switch (ee_mode) {
            default:
            case EE_MODE_32:
                value = *(uint32_t *) data;
                break;
            case EE_MODE_16_LOW:
                value = *(uint16_t *) data;
                break;
            case EE_MODE_16_HIGH:
                value = (*(uint16_t *) data) << 16;
                break;
        }
        rc = ee_program_word(addr, value);
        if (rc != 0) {
            if (try_count++ < 2) {
#ifdef DEBUG_SIGNALS
                printf("Program failed -- trying again at 0x%lx\n",
                       addr << ee_addr_shift);
#endif
                goto try_again;
            }
            printf("  Program failed at 0x%lx\n", addr << ee_addr_shift);
            return (3);
        }

        /* Verify write was successful */
        ee_read_word(addr, &rvalue);
        xvalue = (value ^ rvalue) & ee_cmd_mask;
        if (xvalue != 0) {
            /* Mismatch */
            if ((try_count++ < 2) && ((xvalue & ~rvalue) == 0)) {
                /* Can try again -- bits are not 0 which need to be 1 */
#ifdef DEBUG_SIGNALS
                printf("Program mismatch -- trying again at 0x%lx\n",
    1                  addr << ee_addr_shift);
#endif
                goto try_again;
            }
            printf("  Program mismatch at 0x%lx\n", addr << ee_addr_shift);
            printf("      wrote=%08lx read=%08lx\n", value, rvalue);
            return (4);
        }

        count--;
        addr++;
        data += wordsize;
    }

    ee_read_mode();
    return (0);
}

/*
 * ee_read_mode
 * ------------
 * Sends a command to put the EEPROM chip back in the startup read mode.
 */
void
ee_read_mode(void)
{
    ee_cmd(0x00555, 0x00f000f0);
}

typedef struct {
    uint16_t cv_id;       // Vendor code
    char     cv_vend[12]; // Vendor string
} chip_vendors_t;

static const chip_vendors_t chip_vendors[] = {
    { 0x0001, "AMD" },      // AMD, Alliance, ST, Micron, others
    { 0x0004, "Fujitsu" },
    { 0x00c2, "Macronix" }, // MXIC
    { 0x0000, "Unknown" },  // Must remain last
};

typedef struct {
    uint32_t ci_id;       // Vendor code
    char     ci_dev[16];  // ID string for display
} chip_ids_t;
static const chip_ids_t chip_ids[] = {
    { 0x000122D2, "M29F160FT" },   // AMD+others 2MB top boot
    { 0x000122D8, "M29F160FB" },   // AMD+others 2MB bottom boot
    { 0x000122D6, "M29F800FT" },   // AMD+others 1MB top boot
    { 0x00012258, "M29F800FB" },   // AMD+others 1MB bottom boot
    { 0x00012223, "M29F400FT" },   // AMD+others 512K top boot
    { 0x000122ab, "M29F400FB" },   // AMD+others 512K bottom boot
    { 0x000422d2, "M29F160TE" },   // Fujitsu 2MB top boot
    { 0x00c222D6, "MX29F800CT" },  // Macronix 2MB top boot
    { 0x00c22258, "MX29F800CB" },  // Macronix 2MB bottom boot
    { 0x00000000, "Unknown" },     // Must remain last
};

typedef struct {
    uint16_t cb_chipid;   // Chip id code
    uint8_t  cb_bbnum;    // Boot block number (0=Bottom boot)
    uint8_t  cb_bsize;    // Common block size in Kwords (typical 32K)
    uint8_t  cb_ssize;    // Boot block sector size in Kwords (typical 4K)
    uint8_t  cb_map;      // Boot block sector erase map
} chip_blocks_t;

static const chip_blocks_t chip_blocks[] = {
    { 0x22D2, 31, 32, 4, 0x71 },  // 01110001 8K 4K 4K 16K (top)
    { 0x22D8,  0, 32, 4, 0x1d },  // 00011101 16K 4K 4K 8K (bottom)
    { 0x22D6, 15, 32, 4, 0x71 },  // 01110001 8K 4K 4K 16K (top)
    { 0x2258,  0, 32, 4, 0x1d },  // 00011101 16K 4K 4K 8K (bottom)
    { 0x0000,  0, 32, 4, 0x1d },  // Default to bottom boot
};

/*
 * get_chip_block_info
 * -------------------
 * Returns a pointer to the chip erase block information for the specified
 * chipid. This function will never return NULL. If no match is found, the
 * last (default) element in the structure above is returned.
 */
static const chip_blocks_t *
get_chip_block_info(uint32_t chipid)
{
    uint16_t cid = (uint16_t) chipid;
    uint pos;

    /* Search for exact match */
    for (pos = 0; pos < ARRAY_SIZE(chip_blocks) - 1; pos++)
        if (chip_blocks[pos].cb_chipid == cid)
            break;

    return (&chip_blocks[pos]);
}

/*
 * ee_erase
 * --------
 * Will erase the entire chip, individual blocks, or sequential groups
 * of blocks.
 *
 * A non-zero length erases all sectors making up the address range
 * of addr to addr + length - 1. This means that it's possible that
 * more than the specified length will be erased, but that never too
 * few sectors will be erased. A minimum of one sector will always
 * be erased.
 *
 * EEPROM erase MX_ERASE_MODE_CHIP erases the entire device.
 * EEPROM erase MX_ERASE_MODE_SECTOR erases a 32K-word (64KB) sector.
 *
 * Return values
 *  0 = Success
 *  1 = Erase Timeout
 *  2 = Erase failure
 *  3 = Erase rejected by device (low VPP?)
 *
 * M29F160
 *     Block erase time:      8 sec max
 *     Word Program time:     256 us max
 *     Erase/Program Cycles:  100000 per block
 *
 * MX29F800
 *     Chip erase time:       32 sec max
 *     Block erase time:      15 sec max
 *     Word Program time:     360 us max
 *     Chip programming time: 3 sec typical
 *     Erase/Program Cycles:  100000 min
 *
 * @param [in]  mode=MX_ERASE_MODE_CHIP   - The entire chip is to be erased.
 * @param [in]  mode=MX_ERASE_MODE_SECTOR - One or multiple sectors are to be
 *                                          erased.
 * @param [in]  addr    - The address to erased (if MX_ERASE_MODE_SECTOR).
 * @param [in]  len     - The length to erased (if MX_ERASE_MODE_SECTOR). Note
 *                        that the erase area is always rounded up to the next
 *                        full sector size. A length of 0 will still erase a
 *                        single sector.
 * @param [in]  verbose - Report the accumulated time as the erase progresses.
 */
int
ee_erase(uint mode, uint32_t addr, uint32_t len, int verbose)
{
    int rc = 0;
    int timeout;
    uint32_t part1;
    uint32_t part2;
    const chip_blocks_t *cb;

    if (mode > MX_ERASE_MODE_SECTOR) {
        printf("BUG: Invalid erase mode %d\n", mode);
        return (1);
    }
    if ((len == 0) || (mode == MX_ERASE_MODE_CHIP))
        len = 1;

    /* Figure out if this is a top boot or bottom boot part */
    ee_id(&part1, &part2);
    cb = get_chip_block_info(part1);

    ee_status_clear();
    while (len > 0) {
        if (addr >= EE_DEVICE_SIZE) {
            /* Exceeded the address range of the EEPROM */
            rc = 1;
            break;
        }

        disable_irq();
        ee_write_word(0x00555, 0x00aa00aa);
        ee_write_word(0x002aa, 0x00550055);
        ee_write_word(0x00555, 0x00800080);
        ee_write_word(0x00555, 0x00aa00aa);
        ee_write_word(0x002aa, 0x00550055);

        if (mode == MX_ERASE_MODE_CHIP) {
            ee_write_word(0x00555, 0x00100010);
            usb_mask_interrupts();
            enable_irq();
            timeout = 32000000;  // 32 seconds
            len = 0;
        } else {
            /* Block erase (supports multiple blocks) */
//          int bcount = 0;
            timeout = 1000000;  // 1 second
            enable_irq();
            while (len > 0) {
                uint32_t addr_mask;
                uint32_t bsize = cb->cb_bsize << 10;
                uint     bnum  = addr / bsize;
                if (bnum == cb->cb_bbnum) {
                    /* Boot block has variable block size */
                    uint soff = addr - bnum * bsize;
                    uint snum = soff / (cb->cb_ssize << 10);
                    uint smap = cb->cb_map;
#ifdef ERASE_DEBUG
                    printf("bblock soff=%x snum=%x s_map=%x\n", soff, snum, smap);
#endif
                    bsize = 0;
                    do {
                        bsize += (cb->cb_ssize << 10);
                        snum++;
                        if (smap & BIT(snum))
                            break; // At next block
#ifdef ERASE_DEBUG
                        printf("   smap=%x bsize=%lx\n", smap, bsize);
#endif
                    } while (snum < 8);
#ifdef ERASE_DEBUG
                    printf(" bb sector %lx\n", bsize);
#endif
                }
#ifdef ERASE_DEBUG
                else {
                    printf(" normal block %lx\n", bsize);
                }
#endif

                addr_mask = ~(bsize - 1);
#ifdef ERASE_DEBUG
                printf("->ee_erase %lx %x\n", addr & addr_mask, bsize);
#else
                ee_write_word(addr & addr_mask, 0x00300030);
#endif

                timeout += 1000000;  // Add 1 second per block

                if (len < bsize) {
                    /* Nothing left to do -- allow erase to start */
                    len = 0;
                    break;
                }
                len  -= bsize;
                addr += bsize;  // Advance to the next sector
#if 0
                if (bcount++ > 8)  // Limit count of simultaneous block erases
                    break;
#endif
            }
        }
        usb_unmask_interrupts();

        timer_delay_usec(100);  // tBAL (Word Access Load Time)

        rc = ee_wait_for_done_status(timeout, verbose, EE_MODE_ERASE);
        if (rc != 0)
            break;
    }

    ee_read_mode();
    return (rc);
}

/*
 * ee_id
 * -----
 * Queries and reports the current chip ID values.
 *
 * M29F160FB chip id should be 0x000122D8
 *         (0x0001=Micron and 0x22D8=M29F160 bottom boot)
 * M29F160FT chip id should be 0x000122D2
 *         (0x0001=Micron and 0x22D2=M29F160 top boot)
 * MX29F800CB chip id should be 0x00c22258
 *         (0x00c2=MXID and 0x2258=MX29F800 bottom boot)
 *
 * This function requires no arguments.
 *
 * @return      The manufacturer (high) and device code (low).
 *              Note that if there are two EEPROM devices, then the
 *              Upper 16 bits of the high and low values will contain
 *              the manufacturer and device code of the second part.
 */
void
ee_id(uint32_t *part1, uint32_t *part2)
{
    uint32_t low;
    uint32_t high;

    ee_cmd(0x00555, 0x00900090);
    ee_read_word(0x00000, &low);
    ee_read_word(0x00001, &high);
    ee_read_mode();

    switch (ee_mode) {
        case EE_MODE_32:
        case EE_MODE_16_LOW:
            *part1 = (low << 16) | ((uint16_t) high);
            *part2 = (low & 0xffff0000) | (high >> 16);
            break;
        case EE_MODE_16_HIGH:
            *part2 = (low << 16) | ((uint16_t) high);
            *part1 = (low & 0xffff0000) | (high >> 16);
            break;
    }
}

const char *
ee_vendor_string(uint32_t id)
{
    uint16_t vid = id >> 16;
    uint     pos;

    for (pos = 0; pos < ARRAY_SIZE(chip_vendors) - 1; pos++)
        if (chip_vendors[pos].cv_id == vid)
            break;
    return (chip_vendors[pos].cv_vend);
}

const char *
ee_id_string(uint32_t id)
{
    uint pos;

    for (pos = 0; pos < ARRAY_SIZE(chip_ids) - 1; pos++)
        if (chip_ids[pos].ci_id == id)
            break;

    if (pos == ARRAY_SIZE(chip_ids)) {
        uint16_t cid = id & 0xffff;
        for (pos = 0; pos < ARRAY_SIZE(chip_ids) - 1; pos++)
            if ((chip_ids[pos].ci_id & 0xffff) == cid)
                break;
    }
    return (chip_ids[pos].ci_dev);
}

/*
 * ee_poll() monitors the EEPROM for last access and automatically cuts
 *           drivers to it after being idle for more than 1 second.
 *
 * This function requires no arguments.
 *
 * @return      None.
 */
void
ee_poll(void)
{
    if (ee_last_access != 0) {
        uint64_t usec = timer_tick_to_usec(timer_tick_get() - ee_last_access);
        if (usec > 1000000) {
            ee_disable();
            ee_last_access = 0;
        }
    }
}

static void
ee_print_bits(uint32_t value, int high_bit, char *prefix)
{
    int bit;
    for (bit = high_bit; bit >= 0; bit--) {
        if (value & (1 << bit))
            printf("%s%d ", prefix, bit);
    }
}

/*
 * ee_verify() verifies pin connectivity to an installed EEPROM. This is done
 *             by a sequence of distinct tests.
 *
 * These are:
 *   1) Pull-down test (all address and data lines are weakly pulled
 *      down to verify no exernal power is present).
 *      including using STM32 internal
 *   2) VCC, CE, OE, and VPP are then applied in sequence to verify
 *      no address or data lines are pulled up.
 *   3) Pull-up test (all address and data lines are weakly pulled up, one
 *      at a time) to verify:
 *      A) Each line is pulled up in less than 1 ms
 *      B) No other line is affected by that pull-up
 *   4) Pins one row beyond where the EEPROM should be are tested to verify
 *      that they float.
 *   5) Power is applied
 */
int
ee_verify(int verbose)
{
    int         rc = 0;
    int         pass;
    uint32_t    value;
    uint32_t    expected;
    const char *when = "";

    if (verbose)
        printf("Test address and data pull-down: ");
    for (pass = 0; pass <= 1; pass++) {
        /* Set up next pass */
        switch (pass) {
            case 0:
                /* Start in an unpowered state, all I/Os input, pulldown */
                ee_disable();
                break;
            case 1:
                oe_output_enable();
                oe_output(1);
                when = " when OE high";
                break;
        }
        timer_delay_usec(100);  // Pull-downs should quickly drop voltage

        value = address_input();
        if (value != 0) {
            ee_print_bits(value, 19, "A");
            printf("addr stuck high: 0x%05lx%s\n", value, when);
            rc = 1;
            goto fail;
        }

        value = data_input();
        if (value != 0) {
            ee_print_bits(value, 15, "D");
            printf("data stuck high: 0x%08lx%s\n", value, when);
            rc = 1;
            goto fail;
        }
    }

    if (verbose) {
        printf("pass\n");
        printf("Test address pull-up: ");
    }

    /* Pull up and verify address lines, one at a time */
    for (pass = 0; pass <= 19; pass++) {
        /* STM32F1 pullup/pulldown is controlled by output data register */
        address_output((1 << (pass + 1)) - 1);

        uint64_t timeout = timer_tick_plus_msec(1);
        uint64_t start = timer_tick_get();
        uint64_t seen = 0;

        while (timer_tick_has_elapsed(timeout) == false) {
            value = data_input();
            if (value != 0) {
                ee_print_bits(value, 16, "D");
                printf("found high with A%d pull-up: %04lx\n", pass, value);
                rc = 1;
                break;
            }
            value = address_input();
            if (value & (1 << pass)) {
                if (seen == 0)
                    seen = timer_tick_get();
                expected = (1U << (pass + 1)) - 1;
                if (value != expected) {
                    printf("A%d pull-up caused incorrect ", pass);
                    ee_print_bits(value ^ expected, 19, "A");
                    printf("value: 0x%05lx\n", value);
                    rc = 1;
                    break;
                }
            }
        }
        if (seen == 0) {
            printf("A%d stuck low: 0x%05lx\n", pass, value);
            rc = 1;
        } else if (verbose > 1) {
            printf(" A%d: %lld usec\n",
                   pass, timer_tick_to_usec(seen - start));
        }
    }
    if (rc != 0)
        goto fail;

    if (verbose) {
        printf("pass\n");
        printf("Test data pull-up: ");
    }

    /* Pull up and verify data lines, one at a time */
    for (pass = 0; pass <= 15; pass++) {
        /* STM32F1 pullup/pulldown is controlled by output data register */
        data_output((1 << (pass + 1)) - 1);

        uint64_t timeout = timer_tick_plus_msec(1);
        uint64_t start = timer_tick_get();
        uint64_t seen = 0;

        while (timer_tick_has_elapsed(timeout) == false) {
            value = address_input();
            if (value != 0xfffff) {
                ee_print_bits(value ^ 0xffff, 19, "A");
                printf("found low with D%d pull-up: %05lx\n", pass, value);
                rc = 1;
                break;
            }
            value = data_input();
            if (value & (1 << pass)) {
                if (seen == 0)
                    seen = timer_tick_get();
                expected = (1U << (pass + 1)) - 1;
                if (value != expected) {
                    printf("D%d pull-up caused incorrect ", pass);
                    ee_print_bits(value ^ expected, 16, "D");
                    printf("value: 0x%04lx\n", value);
                    rc = 1;
                    break;
                }
            }
        }
        if (seen == 0) {
            printf("D%d stuck low: 0x%04lx\n", pass, value);
            rc = 1;
        } else if (verbose > 1) {
            printf(" D%d: %lld usec\n",
                   pass, timer_tick_to_usec(seen - start));
        }
    }
    if (rc != 0)
        goto fail;

    if (verbose) {
        printf("pass\n");
    }

fail:
    ee_disable();
    return (rc);
}

void
ee_set_bank(uint8_t bank)
{
    uint8_t oldbank = config.bi.bi_bank_current;
    /*
     * bi_merge tells what bits of A17, A18, and A19 to force.
     * If the width of the bank is 8, then none are forced.
     * If the width of the bank is 4, then A19 is forced.
     * If the width of the bank is 2, then A19 and A18 are forced.
     * If the width of the bank is 1, then A19, A18, and A17 are forced.
     */
    switch (config.bi.bi_merge[bank] >> 4) {
        case 0:  // 512KB bank (width 1)
            ee_address_override((bank << 4) | 0x7, 0);
            break;
        case 1:  // 1MB bank (width 2)
            ee_address_override((bank << 4) | 0x6, 0);
            break;
        case 2:  // 2MB bank (width 4)
            ee_address_override((bank << 4) | 0x8, 0);
            break;
        case 3:  // 4MB bank (width 8)
            ee_address_override((bank << 4) | 0x0, 0);
            break;
    }

    config.bi.bi_bank_current = bank;
    config.bi.bi_bank_nextreset = 0xff;
    if (bank != oldbank)
        printf("Bank select %u %s\n", bank, config.bi.bi_name[bank]);
}

void
ee_update_bank_at_reset(void)
{
    if ((config.bi.bi_bank_nextreset != 0xff) &&
        (config.bi.bi_bank_nextreset != config.bi.bi_bank_current)) {
        printf("nextreset bank %u\n", config.bi.bi_bank_nextreset);
        ee_set_bank(config.bi.bi_bank_nextreset);
    } else {
        ee_set_bank(config.bi.bi_bank_current);
    }
}

void
ee_update_bank_at_poweron(void)
{
    ee_set_bank(config.bi.bi_bank_poweron);
}

void
ee_update_bank_at_longreset(void)
{
    uint bank;
    uint cur = config.bi.bi_bank_current;
    for (bank = 0; bank < ROM_BANKS; bank++) {
        if (cur == config.bi.bi_longreset_seq[bank]) {
            /* Switch to the next bank in the sequence */
            uint nextbank = bank + 1;
            if ((nextbank == ROM_BANKS) ||
                (config.bi.bi_longreset_seq[nextbank] >= ROM_BANKS)) {
                nextbank = 0;
            }
            printf("longreset bank %u\n", config.bi.bi_longreset_seq[nextbank]);
            ee_set_bank(config.bi.bi_longreset_seq[nextbank]);
            break;
        }
    }
}

void
ee_init(void)
{
    ticks_per_15_nsec  = timer_nsec_to_tick(15);
    ticks_per_20_nsec  = timer_nsec_to_tick(20);
    ticks_per_30_nsec  = timer_nsec_to_tick(30);

    ee_set_mode(ee_mode);
}
