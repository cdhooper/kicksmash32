/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2022.
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
#include "adc.h"
#include "utils.h"
#include "timer.h"
#include "gpio.h"
#include "usb.h"
#include "crc32.h"
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

#define GPIO_MODE_OUTPUT_PP GPIO_MODE_OUTPUT_10_MHZ

#define CAPTURE_SW      0
#define CAPTURE_ADDR    1
#define CAPTURE_DATA_LO 2
#define CAPTURE_DATA_HI 3

static void config_tim2_ch1_dma(uint mode);
static void config_tim5_ch1_dma(bool verbose);

/*
 * EE_MODE_32      = 32-bit flash
 * EE_MODE_16_LOW  = 16-bit flash low device (bits 0-15)
 * EE_MODE_16_HIGH = 16-bit flash high device (bits 16-31)
 */
uint            ee_mode = EE_MODE_32;
uint            ee_default_mode = EE_MODE_32;
static uint32_t ee_cmd_mask;
static uint32_t ee_addr_shift;
static uint32_t ee_status = EE_STATUS_NORMAL;  // Status from program/erase

static uint32_t ticks_per_15_nsec;
static uint32_t ticks_per_20_nsec;
static uint32_t ticks_per_30_nsec;
static uint64_t ee_last_access = 0;
static bool     ee_enabled = false;
static uint8_t  capture_mode = CAPTURE_ADDR;

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
 * Override the Flash A18 and A19 address
 *
 * override = 0  Temporarily disable override
 * override = 1  Record new override
 * override = 2  Restore previous override
 *
 * bits
 *   0    1=Drive A18
 *   1    A18 driven value
 *   2    1=Drive A19
 *   3    A19 driven value
 *   4-7  Unused
 */
void
ee_address_override(uint8_t bits, uint override)
{
    static uint8_t old  = 0;
    static uint8_t last = 0;
    uint8_t        val;

    switch (override) {
        default:
        case 0:
            val = 0;
            break;
        case 1:
            val = old = bits;
            break;
        case 2:
            val = old;
            break;
    }
    if (val == last)
        return;
    last = val;

    if (val & BIT(0)) {
        /* Drive A18 */
        uint shift = (val & BIT(1)) ? 0 : 16;
        GPIO_BSRR(FLASH_A18_PORT) = FLASH_A18_PIN << shift;
        gpio_setmode(FLASH_A18_PORT, FLASH_A18_PIN,
                     GPIO_SETMODE_OUTPUT_PPULL_2);
        if (override == 1)
            printf(" A18=%d", !shift);
    } else {
        /* Disable: Weak pull down A18 */
        GPIO_BSRR(FLASH_A18_PORT) = FLASH_A18_PIN << 16;
        gpio_setmode(FLASH_A18_PORT, FLASH_A18_PIN, GPIO_SETMODE_INPUT_PULLUPDOWN);
        if (override == 1)
            printf(" !A18");
    }
    if (val & BIT(2)) {
        /* Drive A19 */
        uint shift = (val & BIT(3)) ? 0 : 16;
        GPIO_BSRR(FLASH_A19_PORT) = FLASH_A19_PIN << shift;
        gpio_setmode(FLASH_A19_PORT, FLASH_A19_PIN,
                     GPIO_SETMODE_OUTPUT_PPULL_2);
        if (override == 1)
            printf(" A19=%d", !shift);
    } else {
        /* Disable: Weak pull down A19 */
        GPIO_BSRR(FLASH_A19_PORT) = FLASH_A19_PIN << 16;
        gpio_setmode(FLASH_A19_PORT, FLASH_A19_PIN, GPIO_SETMODE_INPUT_PULLUPDOWN);
        if (override == 1)
            printf(" !A19");
    }
    if (override == 1)
        printf("\n");
}

/*
 * address_output_enable
 * ---------------------
 * Enables the address pins for output.
 */
static void
address_output_enable(void)
{
    /* A0-A12=PC0-PC12 A13-A19=PA1-PA7 */
    GPIO_CRL(SOCKET_A0_PORT)  = 0x11111111;  // Output Push-Pull
    GPIO_CRH(SOCKET_A0_PORT)  = 0x00011111;
    GPIO_CRL(SOCKET_A13_PORT) = 0x11111118;  // PA0=SOCKET_OE = Input
    ee_address_override(0, 0);  // Suspend A19-A18 override
}

/*
 * address_output_disable
 * ----------------------
 * Reverts the address pins back to input (don't drive).
 */
static void
address_output_disable(void)
{
    /* A0-A12=PC0-PC12 A13-A19=PA1-PA7 */
    GPIO_CRL(SOCKET_A0_PORT)   = 0x44444444;  // Input
    GPIO_CRH(SOCKET_A0_PORT)   = 0x44444444;
    GPIO_CRL(SOCKET_A13_PORT)  = 0x44444448;  // PA0=SOCKET_OE = Input PU
    ee_address_override(0, 2);  // Restore previous A19-A18 override
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
uint32_t
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
static void
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
 * oewe_output
 * -----------
 * Drives the OEWE (flash write enable on output enable) pin with the specified value.
 * If this pin is high, when the host drives OE# low, it will result in WE# being
 * driven low.
 */
static void
oewe_output(uint value)
{
#ifdef DEBUG_SIGNALS
    printf(" WE=%d", value);
#endif
    gpio_setv(FLASH_OEWE_PORT, FLASH_OEWE_PIN, value);
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
 * oe_input
 * --------
 * Return the current value of the SOCKET_OE pin (either 0 or non-zero).
 */
static uint
oe_input(void)
{
    return (GPIO_IDR(SOCKET_OE_PORT) & SOCKET_OE_PIN);
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

static void
update_ee_cmd_mask(void)
{
    if (ee_mode == EE_MODE_32) {
        ee_cmd_mask = 0xffffffff;  // 32-bit
        ee_addr_shift = 2;
    } else if (ee_mode == EE_MODE_16_LOW) {
        ee_cmd_mask = 0x0000ffff;  // 16-bit low
        ee_addr_shift = 1;
    } else {
        ee_cmd_mask = 0xffff0000;  // 16-bit high
        ee_addr_shift = 1;
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
#ifdef DEBUG_SIGNALS
    printf("ee_enable\n");
#endif
    address_output(0);
    address_output_enable();
    we_output(1);  // WE# disabled
    oe_output(1);
    oe_output_enable();
    data_output_disable();
    ee_enabled = true;
    ee_read_mode();
#ifdef DEBUG_SIGNALS
    printf("GPIOA=%x GPIOB=%x GPIOC=%x GPIOD=%x GPIOE=%x\n",
           GPIOA, GPIOB, GPIOC, GPIOD, GPIOE);
#endif
    update_ee_cmd_mask();
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

        usb_mask_interrupts();
        while (count-- > 0)
            ee_read_word(addr++, data++);
        usb_unmask_interrupts();
    } else {
        uint16_t *data = datap;

        usb_mask_interrupts();
        while (count-- > 0) {
            uint32_t value;
            ee_read_word(addr++, &value);
            if (ee_mode == EE_MODE_16_LOW)
                (*data++) = (uint16_t) value;
            else
                (*data++) = value >> 16;
        }
        usb_unmask_interrupts();
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

    /* Check for a command which doens't require an unlock sequence */
    switch (cmd & 0xffff) {
        case 0x98:  // Read CFI Query
        case 0xf0:  // Read/Reset
        case 0xb0:  // Erase Suspend
        case 0x30:  // Erase Resume
            ee_write_word(addr, cmd);
            goto finish;
    }

    usb_mask_interrupts();

    ee_write_word(0x00555, 0x00aa00aa);
    ee_write_word(0x002aa, 0x00550055);
    ee_write_word(addr, cmd);

    usb_unmask_interrupts();
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
    usb_mask_interrupts();
    ee_write_word(0x00555, 0x00aa00aa);
    ee_write_word(0x002aa, 0x00550055);
    ee_write_word(0x00555, 0x00a000a0);
    ee_write_word(addr, word);
    usb_unmask_interrupts();

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

/*
 * ee_status_read
 * --------------
 * Acquires and converts to readable text the current value of the status
 * register from the EEPROM.
 *
 * @param [out] status     - A buffer where to store the status string.
 * @param [in]  status_len - The buffer length
 *
 * @return      status value read from the EEPROM.
 *
 * XXX: This code does not yet support the M29F160xT
 */
uint16_t
ee_status_read(char *status, uint status_len)
{
    uint32_t data;
    const char *sstr;

    ee_cmd(0x00555, 0x00700070);
    ee_read_word(0x00000, &data);
    ee_read_mode();

    switch (ee_status) {
        case EE_STATUS_NORMAL:
            sstr = "Normal";
            break;
        case EE_STATUS_ERASE_TIMEOUT:
            sstr = "Erase Timeout";
            break;
        case EE_STATUS_PROG_TIMEOUT:
            sstr = "Program Timeout";
            break;
        case EE_STATUS_ERASE_FAILURE:
            sstr = "Erase Failure";
            break;
        case EE_STATUS_PROG_FAILURE:
            sstr = "Program Failure";
            break;
        default:
            sstr = "Unknown";
            break;
    }
    snprintf(status, status_len, sstr);

    return (ee_status);
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
    { 0x22D2, 31, 32, 4, 0x71 },  // 01110001 16K 4K 4K 8K (top)
    { 0x22D8,  0, 32, 4, 0x1d },  // 00011101 8K 4K 4K 16K (bottom)
    { 0x22D6, 15, 32, 4, 0x71 },  // 01110001 16K 4K 4K 8K (top)
    { 0x2258,  0, 32, 4, 0x1d },  // 00011101 8K 4K 4K 16K (bottom)
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

        usb_mask_interrupts();

        ee_write_word(0x00555, 0x00aa00aa);
        ee_write_word(0x002aa, 0x00550055);
        ee_write_word(0x00555, 0x00800080);
        ee_write_word(0x00555, 0x00aa00aa);
        ee_write_word(0x002aa, 0x00550055);

        if (mode == MX_ERASE_MODE_CHIP) {
            ee_write_word(0x00555, 0x00100010);
            timeout = 32000000;  // 32 seconds
            len = 0;
        } else {
            /* Block erase (supports multiple blocks) */
//          int bcount = 0;
            timeout = 1000000;  // 1 second
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

/* Buffers for DMA from/to GPIOs and Timer event generation registers */
#define ADDR_BUF_COUNT 512
#define ALIGN  __attribute__((aligned(16)))
ALIGN volatile uint16_t tim2_buffer[ADDR_BUF_COUNT];
ALIGN volatile uint16_t addr_buffer_lo[ADDR_BUF_COUNT];
ALIGN volatile uint8_t *addr_buffer_hi = (uint8_t *)tim2_buffer;
uint addr_cons = 0;
uint8_t reply_buffer[256];

void
ee_snoop(uint mode)
{
    uint     last_oe = 1;
    uint     cons = 0;
    uint     prod = 0;
    uint     oprod = 0;
    uint     no_data = 0;
    uint32_t cap_addr[32];
    uint32_t cap_data[32];

    if (mode != CAPTURE_SW)
        printf("Press any key to exit\n");

    address_output_disable();
    if ((mode == CAPTURE_ADDR) ||
        (mode == CAPTURE_DATA_LO) ||
        (mode == CAPTURE_DATA_HI)) {
        uint dma_left;
        config_tim2_ch1_dma(mode);
        config_tim5_ch1_dma(false);
        dma_left = dma_get_number_of_data(DMA2, DMA_CHANNEL5);
        prod = ARRAY_SIZE(addr_buffer_lo) - dma_left;
        if (prod > ARRAY_SIZE(addr_buffer_lo))
            prod = 0;
        cons = prod;

        while (1) {
            if (getchar() > 0)
                break;
            dma_left = dma_get_number_of_data(DMA2, DMA_CHANNEL5);
            prod = ARRAY_SIZE(addr_buffer_lo) - dma_left;
            if (prod > ARRAY_SIZE(addr_buffer_lo))
                prod = 0;
            if (cons != prod) {
                while (cons != prod) {
                    if (mode == CAPTURE_ADDR) {
                        printf(" %x%04x",
                               addr_buffer_hi[cons] >> 4, addr_buffer_lo[cons]);
                    } else {
                        printf(" %04x[%04x]",
                               addr_buffer_lo[cons], tim2_buffer[cons]);
                    }
                    cons++;
                    if (cons >= ARRAY_SIZE(addr_buffer_lo))
                        cons = 0;
                }
                printf("\n");
            }
        }
        return;
    }

    timer_disable_irq(TIM5, TIM_DIER_CC1IE);
    while (1) {
        if (oe_input() == 0) {
            /* Capture address on falling edge of OE */
            if (last_oe == 1) {
                uint32_t addr = address_input();
                uint nprod = prod + 1;
                if (nprod >= ARRAY_SIZE(cap_addr))
                    nprod = 0;
                if (nprod != cons) {
                    /* FIFO has space */
                    cap_addr[prod] = addr;
                    oprod = prod;
                    prod = nprod;
                    no_data = 0;
                    continue;
                }
                last_oe = 0;
            }
        } else {
            /* Capture data on rising edge of OE */
            if (last_oe == 0) {
                cap_data[oprod] = data_input();
                last_oe = 1;
            }
        }
        if (no_data++ < 400)
            continue;
        if (cons != prod) {
            while (cons != prod) {
                printf(" %lx[%08lx]", cap_addr[cons], cap_data[cons]);
                if (++cons >= ARRAY_SIZE(cap_addr))
                    cons = 0;
            }
            printf("\n");
        }
        if (no_data++ < 40000)
            continue;
        if (getchar() > 0)
            break;
        no_data = 0;
    }
    timer_enable_irq(TIM5, TIM_DIER_CC1IE);
    printf("\n");
}


#define KS_CMD_ID       0x01    // Reply with software ID
#define KS_CMD_TESTPATT 0x02    // Reply with bit test pattern
#define KS_CMD_EEPROM   0x03    // Issue low level command to EEPROM
#define KS_CMD_ROMSEL   0x04    // Force or release A18 and A19
#define KS_CMD_LOOPBACK 0x05    // Reply with sent message
#define KS_CMD_NOP      0x12     // Do nothing

#define KS_STATUS_OK    0x0000  // Success
#define KS_STATUS_FAIL  0x0001  // Failure
#define KS_STATUS_CRC   0x0002  // CRC failure

#define KS_EEPROM_WE    0x0100  // Drive WE# to EEPROM (given with CMD)

#define KS_ROMSEL_SAVE  0x0100  // Save setting in NVRAM (given with CMD)
#define KS_ROMSEL_SET   0x0f00  // ROM select bits to force
#define KS_ROMSEL_BITS  0xf000  // ROM select bit values (A19 is high bit)

static const uint16_t ks_magic[] = { 0x0119, 0x1970 };

#if 0
static uint8_t
get_dma_interrupt_flags(uint32_t dma, uint channel)
{
    return ((DMA_ISR(dma) >> DMA_FLAG_OFFSET(channel)) & 0x1f);
}

// DMA_IFCR_CTCIF1
void
dma2_channel5_isr(void)
{
    uint32_t dma     = DMA2;
    uint32_t channel = DMA_CHANNEL5;

    uint flags = get_dma_interrupt_flags(dma, channel);
    printf("[%x]", flags);
    dma_clear_interrupt_flags(dma, channel, flags);
}
#endif

static void
oe_reply(uint hold_we, uint len, const void *reply_buf)
{
    uint count = 0;
    uint tlen = (ee_mode == EE_MODE_32) ? 4 : 2;
    uint8_t *dptr;
#undef MEASURE_OE_W
#ifdef MEASURE_OE_W
    uint64_t start;
#endif

    /* Wait for OE to go high */
    for (count = 0; oe_input() == 0; count++) {
        if (count > 100000) {
            printf("OE timeout 01\n");
            return;
        }
    }
    if (count > 0)
        printf("<%u>", count);

    oe_output_enable();
    dptr = (void *) reply_buf;
    if (ee_mode == EE_MODE_16_HIGH)
        dptr -= 2;  /* Change position so data is in upper 16 bits */

    /* Board rev 3 and higher have external bus tranceiver, so STM32 can always drive */
    data_output_enable();  // Drive data pins
    if (hold_we) {
        we_enable(0);      // Pull up
        oewe_output(1);
    }

    while ((int)len > 0) {
        uint32_t dval = *(uint32_t *) dptr;
#if 0
        if (ee_mode == EE_MODE_16_HIGH)
            dval <<= 16;
#endif
        data_output(dval);
        dptr += tlen;
        len  -= tlen;

        /* Wait for OE to go low (start of this data cycle) */
        for (count = 0; oe_input() != 0; count++) {
            if (count > 100000) {
                printf("OE timeout 0\n");
                goto oe_reply_end;
            }
        }
#ifdef MEASURE_OE_W
        start = timer_tick_get();
#endif
        /*
         * Not enough time! By the point that this loop is entered, OE has already
         * gone high, so the address lines have already been released before we
         * can drive WE high.
         * The only option I can see is if DMA hardware can be set up to:
         * 1) On OE=0, drive data output, then drive WE=0
         * 1) On OE=1, drive WE=1, load next data, maybe load next data
         *
         * If WE from STM32 instead drives a FET gate which connects the external
         * OE to WE, then we could have a k
         */
        /* Wait for OE to go high */
        for (count = 0; oe_input() == 0; count++) {
            if (count > 100000) {
                printf("OE timeout 1\n");
                goto oe_reply_end;
            }
        }
#ifdef MEASURE_OE_W
        printf("%u,%u ", (unsigned int) (timer_tick_get() - start), count);
#endif
    }
oe_reply_end:
    oe_output_disable();
    data_output_disable();
    if (hold_we) {
        oewe_output(0);
        we_enable(1);      // Drive high
    }
}

static void
process_addresses(uint prod)
{
    static uint cons = 0;
    static uint magic_pos = 0;
    static uint16_t len = 0;
    static uint16_t cmd = 0;
    static uint16_t cmd_len = 0;
    static uint32_t crc;
    if (prod >= ARRAY_SIZE(addr_buffer_lo))
        return;
    while (cons != prod) {
//      printf(" %x:%04x", cons, addr_buffer_lo[cons]);
        if (magic_pos++ < ARRAY_SIZE(ks_magic)) {
            /* Magic phase */
            if (addr_buffer_lo[cons] != ks_magic[magic_pos - 1])
                magic_pos = 0;  // No match
        } else if (magic_pos == ARRAY_SIZE(ks_magic) + 1) {
            /* Command phase */
            cmd = addr_buffer_lo[cons];
//          printf("cmd=%x\n", cmd);
            switch ((uint8_t) cmd) {
                case KS_CMD_ID: {
                    static const uint32_t reply[] = {
                        0x12091610,  // Matches USB ID
                        0x00000001,  // Protocol version 0.1
                        0x00000001,  // Features
                        0x00000000,  // Unused
                        0x00000000,  // Unused
                    };
                    oe_reply(0, sizeof (reply), &reply);
                    magic_pos = 0;
                    break;
                }
                case KS_CMD_TESTPATT: {  // Test pattern used to verify link
                    static const uint32_t reply[] = {
                        0x54534554, 0x54544150, 0x53202d20, 0x54524154,
                        0xaaaa5555, 0xcccc3333, 0xeeee1111, 0x66669999,
                        0x00020001, 0x00080004, 0x00200010, 0x00800040,
                        0x02000100, 0x08000400, 0x20001000, 0x80004000,
                        0xfffdfffe, 0xfff7fffb, 0xffdfffef, 0xff7fffbf,
                        0xfdfffeff, 0xf7fffbff, 0xdfffefff, 0x7fffbfff,
                        0x54534554, 0x54544150, 0x444e4520, 0x68646320,
                    };
                    oe_reply(0, sizeof (reply), &reply);
                    magic_pos = 0;
                    printf("TP\n");
                    break;
                }
                case KS_CMD_LOOPBACK:
                case KS_CMD_EEPROM:
                case KS_CMD_ROMSEL:
                    break;
                case KS_CMD_NOP:
                    magic_pos = 0;  // Do nothing
                    break;
                default:
                    printf("Unknown cmd %x\n", cmd);
                    magic_pos = 0;  // Unknown command
                    break;
            }
            crc = crc32(0, (void *) &addr_buffer_lo[cons], sizeof (uint16_t));
        } else if (magic_pos == ARRAY_SIZE(ks_magic) + 2) {
            /* Length phase */
            len = cmd_len = addr_buffer_lo[cons];
            crc = crc32(crc, (void *) &addr_buffer_lo[cons], sizeof (uint16_t));
        } else if (len-- > 0) {
            /* Data in phase */
            if (len == 0) {
                crc = crc32(crc, (void *) &addr_buffer_lo[cons], 1);
            } else {
                len--;
                crc = crc32(crc, (void *) &addr_buffer_lo[cons], 2);
            }
        } else if ((uint16_t) crc != addr_buffer_lo[cons]) {
            /* CRC failed */
            uint16_t error[2];
            error[0] = KS_STATUS_CRC;
            error[1] = crc;
            oe_reply(0, sizeof (error), &error);
printf("c=%04x l=%02x CRC %04x != exp %04x %x %x\n", cmd, cmd_len, addr_buffer_lo[cons], (uint16_t)crc, (uint16_t)crc << 1, (uint16_t)crc << 2);
            magic_pos = 0;  // Unknown command
        } else {
            /* Execution phase */
            switch ((uint8_t) cmd) {
                case KS_CMD_LOOPBACK:
                case KS_CMD_EEPROM: {
                    const uint we = cmd & KS_EEPROM_WE;
                    uint8_t *buf1;
                    uint8_t *buf2;
                    uint len1;
                    uint len2;
                    uint cons_s = cons;
                    if ((int) cons_s <= 0)
                        cons_s += ARRAY_SIZE(addr_buffer_lo);
                    if (cons_s * 2 >= cmd_len) {
                        /* Send data doesn't wrap */
                        len1 = cmd_len;
                        buf1 = ((uint8_t *) &addr_buffer_lo[cons_s]) - len1;
                        oe_reply(we, len1, buf1);
                        if (we) {
                            uint pos;
                            printf("we l=%x b=", len1);
                            for (pos = 0; pos < len1 && pos < 10; pos++) {
                                printf("%02x ", buf1[pos]);
                            }
                            printf("\n");
                        }
                    } else {
                        /* Send data from end and beginning of buffer */
                        len1 = cons_s * 2;
                        buf1 = (void *) addr_buffer_lo;
                        len2 = cmd_len - len1;
                        buf2 = ((uint8_t *) addr_buffer_lo) +
                               sizeof (addr_buffer_lo) - len2;
                        if (len2 != 0)
                            oe_reply(we, len2, buf2);
                        oe_reply(we, len1, buf1);
                    }
                    break;
                }
                case KS_CMD_ROMSEL: {
                    uint16_t status = KS_STATUS_OK;
                    uint cons_s = cons - 1;
                    if ((int) cons_s <= 0)
                        cons_s += ARRAY_SIZE(addr_buffer_lo);
printf("RS %04x %04x ", addr_buffer_lo[cons_s], addr_buffer_lo[cons_s + 1]);
                    oe_reply(0, sizeof (status), &status);
                    ee_address_override(addr_buffer_lo[cons_s] >> 8, 1);
                    break;
                }
                default:
                    magic_pos = 0;  // Unknown command
                    break;
            }
            magic_pos = 0;  // Unknown command
        }
        if (++cons >= ARRAY_SIZE(addr_buffer_lo))
            cons = 0;
    }
}

void
tim5_isr(void)
{
    uint dma_left;
    uint producer;
    uint flags = TIM_SR(TIM5) & TIM_DIER(TIM5);
    TIM_SR(TIM5) = ~flags;  /* Clear interrupt */

    dma_left = dma_get_number_of_data(DMA2, DMA_CHANNEL5);
    producer = ARRAY_SIZE(addr_buffer_lo) - dma_left;
    process_addresses(producer);
}

static void
config_dma(uint32_t dma, uint32_t channel, uint to_periph, uint mode,
           volatile void *dst, volatile void *src, uint32_t wraplen)
{
    dma_disable_channel(dma, channel);
    dma_channel_reset(dma, channel);
    dma_set_peripheral_address(dma, channel, (uintptr_t)dst);
    dma_set_memory_address(dma, channel, (uintptr_t)src);
    if (to_periph)
        dma_set_read_from_memory(dma, channel);
    else
        dma_set_read_from_peripheral(dma, channel);
    dma_set_number_of_data(dma, channel, wraplen);
    dma_disable_peripheral_increment_mode(dma, channel);
    dma_enable_memory_increment_mode(dma, channel);
    switch (mode) {
        case 8:
            dma_set_peripheral_size(dma, channel, DMA_CCR_PSIZE_8BIT);
            dma_set_memory_size(dma, channel, DMA_CCR_MSIZE_8BIT);
            break;
        case 16:
            dma_set_peripheral_size(dma, channel, DMA_CCR_PSIZE_16BIT);
            dma_set_memory_size(dma, channel, DMA_CCR_MSIZE_16BIT);
            break;
        default: // 32
            dma_set_peripheral_size(dma, channel, DMA_CCR_PSIZE_32BIT);
            dma_set_memory_size(dma, channel, DMA_CCR_MSIZE_32BIT);
            break;
    }
    dma_enable_circular_mode(dma, channel);
    dma_set_priority(dma, channel, DMA_CCR_PL_MEDIUM);

    dma_disable_transfer_error_interrupt(dma, channel);
    dma_disable_half_transfer_interrupt(dma, channel);
    dma_disable_transfer_complete_interrupt(dma, channel);

    dma_enable_channel(dma, channel);
}

static void
config_tim5_ch1_dma(bool verbose)
{
    /* Capture address when OE goes low */
    memset((void *) addr_buffer_lo, 0, sizeof (addr_buffer_lo));
    if (verbose) {
        printf("Addr lo capture %08x (t5c1) 16-bit\n",
               (uintptr_t) addr_buffer_lo);
    }

    /* DMA from address GPIOs A0-A15 to memory */
    config_dma(DMA2, DMA_CHANNEL5, 0, 16,
               &GPIO_IDR(SOCKET_A0_PORT),
               addr_buffer_lo, ADDR_BUF_COUNT);

#if 0
    /*
     * Can't use DMA interrupt because that only occurs when the
     * DMA buffer has wrapped.
     */
    dma_clear_interrupt_flags(dma, channel,
                              get_dma_interrupt_flags(dma, channel));
    dma_disable_transfer_error_interrupt(dma, channel);
    dma_disable_half_transfer_interrupt(dma, channel);
    dma_enable_transfer_complete_interrupt(dma, channel);
    printf("DMA2_ISR = %p\n", &DMA2_ISR);
    printf("DMA2_CCR(5) = %p\n", &DMA2_CCR(channel));
#endif

    /* Set up TIM5 CH1 to trigger DMA based on external PA0 pin */
    timer_disable_oc_output(TIM5, TIM_OC1);

    /* Enable capture compare CC1 DMA and interrupt */
    timer_enable_irq(TIM5, TIM_DIER_CC1DE | TIM_DIER_CC1IE);

    timer_set_ti1_ch1(TIM5);               // Capture input from channel 1 only

//  timer_continuous_mode(TIM5);
    timer_set_oc_polarity_low(TIM5, TIM_OC1);
    timer_set_oc_value(TIM5, TIM_OC1, 0);

    /* Select the Input and set the filter */
    TIM5_CCMR1 &= ~(TIM_CCMR1_CC1S_MASK | TIM_CCMR1_IC1F_MASK);
    TIM5_CCMR1 |= TIM_CCMR1_CC1S_IN_TI1 | TIM_CCMR1_IC1F_OFF;
    TIM5_SMCR = TIM_SMCR_ETP      | // falling edge detection
                TIM_SMCR_ECE;       // external clock mode 2 (ETR input)

    timer_enable_oc_output(TIM5, TIM_OC1);
}

static void
config_tim2_ch1_dma(uint mode)
{
    capture_mode = mode;

    /* Set up DMA Write to TIM5 Event Generation register */
    memset((void *) tim2_buffer, 0, sizeof (tim2_buffer));
    if (mode == CAPTURE_ADDR) {
        printf("Addr hi capture %08x (t2c1) 8-bit\n",
               (uintptr_t) addr_buffer_hi);
        /* Byte-wide DMA from address GPIOs A16-A19 to memory */
        config_dma(DMA1, DMA_CHANNEL5, 0, 8,
                   &GPIO_IDR(SOCKET_A16_PORT),
                   addr_buffer_hi, ADDR_BUF_COUNT);
    } else if (mode == CAPTURE_DATA_LO) {
        /* Word-wide DMA from data GPIOs D0-D15 to memory */
        config_dma(DMA1, DMA_CHANNEL5, 0, 16,
                   &GPIO_IDR(FLASH_D0_PORT), tim2_buffer, ADDR_BUF_COUNT);
    } else if (mode == CAPTURE_DATA_HI) {
        /* Word-wide DMA from data GPIOs D16-D31 to memory */
        config_dma(DMA1, DMA_CHANNEL5, 0, 16,
                   &GPIO_IDR(FLASH_D16_PORT), tim2_buffer, ADDR_BUF_COUNT);
    }

    timer_disable_oc_output(TIM2, TIM_OC1);
    timer_set_ti1_ch1(TIM2);        // Capture input from channel 1 only

    if (mode == CAPTURE_ADDR) {
        timer_set_oc_polarity_low(TIM2, TIM_OC1);
    } else {
        timer_set_oc_polarity_high(TIM2, TIM_OC1);
    }

    /* Select the Input and set the filter */
    TIM2_CCMR1 &= ~(TIM_CCMR1_CC1S_MASK | TIM_CCMR1_IC1F_MASK);
    TIM2_CCMR1 |= TIM_CCMR1_CC1S_IN_TI1 | TIM_CCMR1_IC1F_OFF;

    timer_enable_oc_output(TIM2, TIM_OC1);

    TIM2_SMCR = TIM_SMCR_ETP      | // falling edge detection
                TIM_SMCR_ECE      | // external clock mode 2 (ETR input)
                TIM_SMCR_ETPS_OFF | // no prescaler
                TIM_SMCR_ETF_OFF;   // no filter
    TIM2_DIER = 0;
    timer_enable_irq(TIM2, TIM_DIER_CC1DE); // DMA on capture/compare event

/*
 * Enable the following only if I want updates at every wrap of the counter,
 * and that only works if the max value is at least 1.
 *      timer_set_dma_on_update_event(TIM2);   // DMA on update event occurs
 *      TIM2_DIER |= TIM_DIER_UDE;             // DMA on update event
 * This seems to limit DMA on all channels except the trigger channel
 */
    timer_set_dma_on_compare_event(TIM2);  // DMA on CCx event occurs

//  timer_generate_event(TIM2, TIM_EGR_TG); // Generate fake Trigger event
//  timer_generate_event(TIM2, TIM_EGR_UG); // Generate fake Update event
}

int
address_log_replay(uint max)
{
    uint dma_left;
    uint prod;
    uint cons;
    uint addr;
    uint count = 0;
    uint flags = TIM_SR(TIM5) & TIM_DIER(TIM5);
    TIM_SR(TIM5) = ~flags;  /* Clear interrupt */

    dma_left = dma_get_number_of_data(DMA2, DMA_CHANNEL5);
    prod = ARRAY_SIZE(addr_buffer_lo) - dma_left;
    process_addresses(prod);

    if (prod >= ARRAY_SIZE(addr_buffer_lo)) {
        printf("Invalid producer\n");
        return (1);
    }
    if (max > ARRAY_SIZE(addr_buffer_lo) - 1)
        max = ARRAY_SIZE(addr_buffer_lo) - 1;

    cons = prod - max;
    if (cons >= ARRAY_SIZE(addr_buffer_lo))
        cons += ARRAY_SIZE(addr_buffer_lo);  // Unsigned negative wrap

    printf("Ent ROMAddr AmigaAddr");
    if (capture_mode == CAPTURE_DATA_LO)
        printf(" DataLo");
    else if (capture_mode == CAPTURE_DATA_HI)
        printf(" DataHi");
    printf("\n");

    while (cons != prod) {
        addr = addr_buffer_lo[cons];
        if (capture_mode == CAPTURE_ADDR)
            addr |= ((addr_buffer_hi[cons] & 0xf0) << 16);
        printf("%3u %05x   %06x", cons,
               addr, (ee_mode == EE_MODE_32) ? (addr << 2) : (addr << 1));
        if ((capture_mode == CAPTURE_DATA_LO) ||
            (capture_mode == CAPTURE_DATA_HI))
            printf("    %04x", tim2_buffer[cons]);
        printf("\n");
        cons++;
        if (cons >= ARRAY_SIZE(addr_buffer_lo))
            cons = 0;
        if (count++ > max) {
            printf("bug: count=%u cons=%x prod=%x\n", count, cons, prod);
            break;
        }
    }
    return (0);
}

void
ee_init(void)
{
    /*
     * Configure DMA on SOCKET_OE going low
     *
     * ---------------- STM32F1 Table 78 lists DMA1 channels ----------------
     * CH1      CH2       CH3       CH4       CH5        CH6       CH7
     * ADC1     -         -         -         -          -         -
     * -        SPI1_RX   SPI1_TX   SPI2_RX   SPI2_TX    -         -
     *                              I2S2_RX   I2S2_TX
     * -        USART3_TX USART3_RX USART1_TX USART1_RX  USART2_RX USART2_TX
     * -        -         -         I2C2_TX   I2C2_RX    I2C1_TX   I2C1_RX
     * -        TIM1_CH1  -         TIM1_CH4  TIM1_UP    TIM1_CH3  -
     *                              TIM1_COM
     *                              TIM1_TRIG
     * TIM2_CH3 TIM2_UP   -         -         TIM2_CH1   -         TIM2_CH2
     *                                                             TIM2_CH4
     * -        TIM3_CH3  TIM3_CH4  -         -          TIM3_CH1  -
     *                    TIM3_UP                        TIM3_TRIG
     * TIM4_CH1 -         -         TIM4_CH2  TIM4_CH3   -         TIM4_UP
     *
     *
     * ---------------- STM32F1 Table 79 lists DMA2 channels ----------------
     * CH1       CH2       CH3       CH4       CH5
     * -         -         -         -         -
     * SPI_RX    SPI_TX    -         -         -
     * I2S3_RX   I2S3_TX   -         -         -
     * -         -         UART4_RX  -         UART4_TX
     * -         -         -         SDIO      -
     * TIM5_CH4  TIM5_CH3  -         TIM5_CH2  TIM5_CH1
     * TIM5_TRIG TIM5_UP
     * -         -         TIM6_UP   -         -
     *                     DAC_CH1
     * -         -         -         TIM7_UP   -
     *                               DAC_CH2
     * TIM8_CH3  TIM8_CH4  TIM8_CH1  -         TIM8_CH2
     *           TIM8_UP   TIM8_TRIG
     *                     TIM8_COM
     *
     * PA0: WKUP/USART2_CTS, ADC12_IN0/TIM2_CH1_ETR, TIM5_CH1/ETH_MII_CRS_WKUP
     *
     * DMA1 Channel 1 used by ADC1
     * DMA1 Channel 5 used by TIM2_TRG (ROM OE DMA from ext pin)
     * DMA2 Channel 5 used by TIM5_CH1 (ROM OE DMA from ext pin)
     *
     * Only one channel may be active per stream.
     *
     * TIM5 CH1 is input trigger for OE
     */

    /*
     * 2023-12-22
     * Read-from-STM32 sequence:
     *      Setup (as soon as OE high after command determined)
     *              Set flash WE high
     *              Set flash OE high
     *              Set data pins to output
     *              Set up DMA from reply buffer
     *      OE high
     *              loop_count--
     *              If loop_count == 0
     *                  Disable DMA
     *                  Set data pins to input
     *                  Flash OE = input
     *              else
     *                  write data pins
     * Write to flash sequence
     *      Setup (as soon as OE high after command determined)
     *              Set flash WE input pullup
     *              Set flash OEWE high
     *              Set flash OE high
     *              Set data pins to output
     *              Set up DMA from reply buffer
     *      OE high
     *              loop_count--
     *              If loop_count == 0
     *                  Disable DMA
     *                  Set data pins to input
     *                  Flash OEWE = low
     *                  Flash OE = input
     *              else
     *                  write data pins
     */
    rcc_periph_clock_enable(RCC_DMA1);
    rcc_periph_clock_enable(RCC_DMA2);

    rcc_periph_clock_enable(RCC_TIM5);
    rcc_periph_reset_pulse(RST_TIM5);
//  timer_set_period(TIM5, 0xffff);

    timer_clear_flag(TIM5, TIM_SR(TIM5) & TIM_DIER(TIM5));
    nvic_set_priority(NVIC_TIM5_IRQ, 0x20);
    nvic_enable_irq(NVIC_TIM5_IRQ);

    rcc_periph_clock_enable(RCC_TIM2);
    rcc_periph_reset_pulse(RST_TIM2);
//  timer_set_period(TIM2, 0x0001);

    config_tim2_ch1_dma(CAPTURE_ADDR);  // PA0 to TIM2_TRIG (CH1)
    config_tim5_ch1_dma(true);          // PA0 to TIM5 CC1

    timer_enable_irq(TIM2, TIM_DIER_TDE);
    timer_enable_irq(TIM5, TIM_DIER_TDE);

//  timer_enable_counter(TIM5);
//  timer_enable_counter(TIM2);

    ticks_per_15_nsec  = timer_nsec_to_tick(15);
    ticks_per_20_nsec  = timer_nsec_to_tick(20);
    ticks_per_30_nsec  = timer_nsec_to_tick(30);

    /* These values for bank override should come from NVRAM */
    ee_address_override(0x5, 0x1);

    update_ee_cmd_mask();
}
