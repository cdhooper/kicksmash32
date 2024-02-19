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
#include "crc32.h"
#include "smash_cmd.h"
#include "pin_tests.h"
#include "config.h"
#include "kbrst.h"
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

#define CAPTURE_SW       0
#define CAPTURE_ADDR     1
#define CAPTURE_DATA_LO  2
#define CAPTURE_DATA_HI  3

#define SWAP16(x) __builtin_bswap16(x)
#define SWAP32(x) __builtin_bswap32(x)

#define LOG_DMA_CONTROLLER DMA2
#define LOG_DMA_CHANNEL    DMA_CHANNEL5

/* Inline speed-critical code */
#define dma_get_number_of_data(x, y) DMA_CNDTR(x, y)

static void config_tim2_ch1_dma(bool verbose);
static void config_tim5_ch1_dma(bool verbose);
static void config_dma(uint32_t dma, uint32_t channel, uint to_periph,
                       uint mode, volatile void *dst, volatile void *src,
                       uint32_t wraplen);

static const uint16_t sm_magic[] = { 0x0117, 0x0119, 0x1017, 0x0204 };

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
static uint32_t ticks_per_200_nsec;
static uint64_t ee_last_access = 0;
static bool     ee_enabled = false;
static uint8_t  capture_mode = CAPTURE_ADDR;
static uint     consumer_wrap;
static uint     consumer_spin;
static uint     rx_consumer = 0;
static uint     message_count = 0;

static uint32_t address_input(void);
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
static void
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

void
ee_set_mode(uint new_mode)
{
    ee_mode = new_mode;
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
    static uint consumer_spin_last = 0;
    if (ee_last_access != 0) {
        uint64_t usec = timer_tick_to_usec(timer_tick_get() - ee_last_access);
        if (usec > 1000000) {
            ee_disable();
            ee_last_access = 0;
        }
    }
    if (consumer_spin_last != consumer_spin) {
        consumer_spin_last = consumer_spin;
        /*
         * Re-enable message interrupt if it was disabled during
         * interrupt processing due to excessive time.
         */
        timer_enable_irq(TIM5, TIM_DIER_CC1IE);
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
        printf("Bank select %u %s\n", bank, config.bi.bi_desc[bank]);
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


/* Buffers for DMA from/to GPIOs and Timer event generation registers */
#define ADDR_BUF_COUNT 512
#define ALIGN  __attribute__((aligned(16)))
ALIGN volatile uint16_t buffer_rxa_lo[ADDR_BUF_COUNT];
ALIGN volatile uint16_t buffer_rxd[ADDR_BUF_COUNT];
ALIGN volatile uint16_t buffer_txd_lo[ADDR_BUF_COUNT * 2];
ALIGN volatile uint16_t buffer_txd_hi[ADDR_BUF_COUNT];

static void
configure_oe_capture_rx(bool verbose)
{
    consumer_wrap = 0;
    rx_consumer = 0;
    config_tim2_ch1_dma(verbose);
    config_tim5_ch1_dma(verbose);

    disable_irq();
// Not enough memory bandwidth on at least one CPU I have to have both
// DMAs active and STM32 keep up with the Amiga. That particular STM32 does
// not have DFU, so it might be a remarked STM32F103 or something else.
    TIM_CCER(TIM2) |= TIM_CCER_CC1E;  // timer_enable_oc_output()
    TIM_CCER(TIM5) |= TIM_CCER_CC1E;
    enable_irq();
}

#ifdef CAPTURE_GPIOS
ALIGN uint16_t buffer_a[ADDR_BUF_COUNT];
ALIGN uint16_t buffer_b[ADDR_BUF_COUNT];
ALIGN uint16_t buffer_c[ADDR_BUF_COUNT];
ALIGN uint16_t buffer_d[ADDR_BUF_COUNT];

static uint
gpio_watch(void)
{
    uint pos = 0;
    uint16_t gpioa;
    uint16_t gpiob;
    uint16_t gpioc;
    uint16_t gpiod;
    uint16_t l_gpioa = 0;
    uint16_t l_gpiob = 0;
    uint16_t l_gpioc = 0;
    uint16_t l_gpiod = 0;

    while (1) {
        gpioa = GPIO_IDR(GPIOA);  // PA0 + A13-A19
        gpiob = GPIO_IDR(GPIOB);  // WE=PB14 OEWE=PB9
        gpioc = GPIO_IDR(GPIOC);  // A0-A15
        gpiod = GPIO_IDR(GPIOD);  // D0-D15
        if ((gpioa != l_gpioa) ||
            (gpiob != l_gpiob) ||
            (gpioc != l_gpioc) ||
            (gpiod != l_gpiod)) {
            buffer_a[pos] = gpioa;
            buffer_b[pos] = gpiob;
            buffer_c[pos] = gpioc;
            buffer_d[pos] = gpiod;
            if (pos++ > 400)
                break;
            l_gpioa = gpioa;
            l_gpiob = gpiob;
            l_gpioc = gpioc;
            l_gpiod = gpiod;
        }
    }
    return (pos);
}

static void
gpio_showbuf(uint count)
{
    uint     pos;
    uint16_t last_a = 0;
    uint16_t last_b = 0;
    uint16_t last_c = 0;
    uint16_t last_d = 0;
    uint16_t diff;

    last_a = ~buffer_a[0];
    last_b = ~buffer_b[0];
    last_c = ~buffer_c[0];
    last_d = ~buffer_d[0];
    for (pos = 0; pos < count; pos++) {
        uint printed_a = 0;
        uint16_t a = buffer_a[pos];
        uint16_t b = buffer_b[pos];
        uint16_t c = buffer_c[pos];
        uint16_t d = buffer_d[pos];
        printf(" %04x %04x %04x %04x", a, b, c, d);
        if (a != last_a) {
            diff = a ^ last_a;
            if (diff & SOCKET_OE_PIN)
                printf(" S_OE=%u", !!(a & SOCKET_OE_PIN));
            if (diff & 0x00f0) {
                /* A16-A19 */
                printf(" A=%05x", c | ((a & 0xf0) << (16 - 4)));
                printed_a++;
            }
        }
        if (b != last_b) {
            diff = b ^ last_b;
            if (diff & FLASH_OE_PIN)
                printf(" F_OE=%u", !!(b & FLASH_OE_PIN));
            if (diff & FLASH_WE_PIN)
                printf(" WE=%u", !!(b & FLASH_WE_PIN));
            if (diff & FLASH_OEWE_PIN)
                printf(" OEWE=%u", !!(b & FLASH_OEWE_PIN));
        }
        if (c != last_c) {
            if (!printed_a)
                printf(" A=%05x", c | ((a & 0xf0) << (16 - 4)));
        }
        if (d != last_d) {
            diff = d ^ last_d;
            printf(" D=%x", d);
        }
        printf("\n");
        last_a = buffer_a[pos];
        last_b = buffer_b[pos];
        last_c = buffer_c[pos];
        last_d = buffer_d[pos];
    }
}
#endif

/*
 * ks_reply
 * --------
 * This function sends a reply message to the host operating system.
 * It will disable flash output and drive data lines directly from the STM32.
 * This routine is called from interrupt context.
 */
static void
ks_reply(uint hold_we, uint len, const void *reply_buf, uint status)
{
    uint      count = 0;
    uint      tlen = (ee_mode == EE_MODE_32) ? 4 : 2;
    uint      dma_left;
    uint      dma_last;

    /* FLASH_OE=1 disables flash from driving data pins */
    oe_output(1);
    oe_output_enable();

    /*
     * Board rev 3 and higher have external bus tranceiver, so STM32 can
     * always drive data bus so long as FLASH_OE is disabled.
     */
    data_output_enable();  // Drive data pins
    if (hold_we) {
        we_output(1);
// XXX: remove once stronger pull-up resistor (1K?) is in place
#define STM32_HARD_DRIVE_WE_ON_SHARED_WRITE
#ifdef STM32_HARD_DRIVE_WE_ON_SHARED_WRITE
        we_enable(1);      // Drive high
#else
        we_enable(0);      // Pull up
#endif
        oewe_output(1);
    }

    /*
     * Configure DMA hardware to drive data pins from RAM when OE goes high
     *
     * In 32-bit mode, we need to copy the 16-bit high word of each 32-bit
     * long to a buffer and then DMA that to the D16-D31 output data register.
     * The D0-D15 values can be DMA as 32-bit values from the source buffer,
     * as the output data register does nothing with writes to the upper
     * 16 bits.
     *
     * In 16-bit low mode, we just need to do 16-bit DMA from the source
     * data to the D0-D15 data register.
     *
     * In 16-bit high mode, we need to do 16-bit DMA from the source data
     * to the D16-D31 data register.
     */

    /* Stop timer DMA triggers */
    TIM_CCER(TIM2) = 0;  // Disable everything
    TIM_CCER(TIM5) = 0;
    timer_disable_irq(TIM5, TIM_DIER_CC1IE);

#if 0
    /* Stop DMA engines */
    timer_disable_oc_output(TIM2, TIM_OC1);
    timer_disable_oc_output(TIM5, TIM_OC1);
#endif

    if (ee_mode == EE_MODE_32) {
        /*
         * For 32-bit mode, we need to separate low and high 16 bits as
         * they are driven out by separate DMA engines (TIM5 and TIM2).
         */
        uint pos;
        if (hold_we) {
            for (pos = 0; pos < len / 4; pos++) {
                uint32_t val = ((uint32_t *)reply_buf)[pos];
                buffer_txd_hi[pos] = val >> 16;
                buffer_txd_lo[pos] = (uint16_t) val;
            }
        } else {
            uint32_t crc;
            for (pos = 0, count = 0; count < ARRAY_SIZE(sm_magic); pos++) {
                buffer_txd_hi[pos] = sm_magic[count++];
                buffer_txd_lo[pos] = sm_magic[count++];
            }
            buffer_txd_hi[pos] = len;
            buffer_txd_lo[pos] = status;
            pos++;
            crc = crc32r(0, &len, 2);
            crc = crc32r(crc, &status, 2);
            crc = crc32(crc, reply_buf, len);
            len /= tlen;
            for (count = 0; count < len; count++, pos++) {
                uint32_t val = ((uint32_t *)reply_buf)[count];
                buffer_txd_hi[pos] = (val << 8)  | ((val >> 8) & 0x00ff);
                buffer_txd_lo[pos] = (val >> 24) | ((val >> 8) & 0xff00);
            }
            buffer_txd_hi[pos] = crc >> 16;
            buffer_txd_lo[pos] = (uint16_t) crc;
            pos++;  // Include CRC
        }

        TIM5_SMCR = TIM_SMCR_ETP      | // falling edge detection
                    TIM_SMCR_ECE;       // external clock mode 2 (ETR input)

        /* TIM5 DMA drives low 16 bits */
        dma_disable_channel(DMA2, DMA_CHANNEL5);  // TIM5
        dma_set_peripheral_address(DMA2, DMA_CHANNEL5,
                                   (uintptr_t) &GPIO_ODR(FLASH_D0_PORT));
        dma_set_memory_address(DMA2, DMA_CHANNEL5, (uintptr_t)buffer_txd_lo);
        dma_set_read_from_memory(DMA2, DMA_CHANNEL5);
        dma_set_number_of_data(DMA2, DMA_CHANNEL5, pos + 1);
        dma_set_peripheral_size(DMA2, DMA_CHANNEL5, DMA_CCR_PSIZE_16BIT);
        dma_set_memory_size(DMA2, DMA_CHANNEL5, DMA_CCR_MSIZE_16BIT);
        DMA_CCR(DMA2, DMA_CHANNEL5) &= ~DMA_CCR_CIRC;
        dma_enable_channel(DMA2, DMA_CHANNEL5);

        /* TIM2 DMA drives high 16 bits */
        dma_disable_channel(DMA1, DMA_CHANNEL5);  // TIM2
        dma_set_peripheral_address(DMA1, DMA_CHANNEL5,
                                   (uintptr_t) &GPIO_ODR(FLASH_D16_PORT));
        dma_set_memory_address(DMA1, DMA_CHANNEL5, (uintptr_t)buffer_txd_hi);
        dma_set_read_from_memory(DMA1, DMA_CHANNEL5);
        dma_set_number_of_data(DMA1, DMA_CHANNEL5, pos + 1);
        dma_set_peripheral_size(DMA1, DMA_CHANNEL5, DMA_CCR_PSIZE_16BIT);
        dma_set_memory_size(DMA1, DMA_CHANNEL5, DMA_CCR_MSIZE_16BIT);
        DMA_CCR(DMA1, DMA_CHANNEL5) &= ~DMA_CCR_CIRC;
        dma_enable_channel(DMA1, DMA_CHANNEL5);

        /* Send out first data and enable capture */
        disable_irq();
        TIM2_EGR = TIM_EGR_CC1G;
        TIM5_EGR = TIM_EGR_CC1G;
        TIM_CCER(TIM2) = TIM_CCER_CC1E;  // Enable, rising edge
        TIM_CCER(TIM5) = TIM_CCER_CC1E;  // Enable, rising edge
        enable_irq();
    } else {
        /* For 16-bit mode, a single DMA engine can be used */
        uint pos;
        if (hold_we) {
            memcpy((void *)buffer_txd_lo, reply_buf, len);
            pos = len / 2;
        } else {
            uint32_t crc;
            memcpy((void *)buffer_txd_lo, sm_magic, sizeof (sm_magic));
            pos = sizeof (sm_magic);
            buffer_txd_lo[pos++] = len;
            buffer_txd_lo[pos++] = status;
            crc = crc32r(0, &len, 2);
            crc = crc32r(crc, &status, 2);
            crc = crc32(crc, reply_buf, len);
            if (len > 0) {
                memcpy((void *)&buffer_txd_lo[pos], reply_buf, len);
                pos += len / 2;
            }
            buffer_txd_lo[pos++] = crc >> 16;
            buffer_txd_lo[pos++] = (uint16_t) crc;
        }

        TIM5_SMCR = TIM_SMCR_ETP      | // falling edge detection
                    TIM_SMCR_ECE;       // external clock mode 2 (ETR input)

        /* TIM5 DMA drives low 16 bits */
        dma_disable_channel(DMA2, DMA_CHANNEL5);  // TIM5
        dma_set_peripheral_address(DMA2, DMA_CHANNEL5,
                                   (uintptr_t) &GPIO_ODR(FLASH_D0_PORT));
        dma_set_memory_address(DMA2, DMA_CHANNEL5, (uintptr_t)buffer_txd_lo);
        dma_set_read_from_memory(DMA2, DMA_CHANNEL5);
        dma_set_number_of_data(DMA2, DMA_CHANNEL5, pos + 1);
        dma_set_peripheral_size(DMA2, DMA_CHANNEL5, DMA_CCR_PSIZE_16BIT);
        dma_set_memory_size(DMA2, DMA_CHANNEL5, DMA_CCR_MSIZE_16BIT);
        DMA_CCR(DMA2, DMA_CHANNEL5) &= ~DMA_CCR_CIRC;
        dma_enable_channel(DMA2, DMA_CHANNEL5);

        dma_disable_channel(DMA1, DMA_CHANNEL5);  // TIM2

        /* Send out first data and enable capture */
        disable_irq();
        TIM5_EGR = TIM_EGR_CC1G;
        TIM_CCER(TIM5) = TIM_CCER_CC1E;  // Enable, rising edge
        enable_irq();
    }

//  enable_oe_capture_tx();

#ifdef CAPTURE_GPIOS
    if (hold_we) {
        count = gpio_watch();
    } else {
#endif
    dma_last = dma_get_number_of_data(DMA2, DMA_CHANNEL5);

    while (dma_last != 0) {
        dma_left = dma_get_number_of_data(DMA2, DMA_CHANNEL5);
        while (dma_last == dma_left) {
            for (count = 0; dma_last == dma_left; count++) {
                if (count > 100000) {
                    printf(" KS timeout 0: %u reads left\n", dma_left);
                    goto oe_reply_end;
                }
                __asm__ volatile("dmb");
                dma_left = dma_get_number_of_data(DMA1, DMA_CHANNEL5);
            }
        }
        dma_last = dma_left;
    }
    timer_delay_ticks(ticks_per_200_nsec);
//  timer_delay_usec(1);  // probably not necessary
#ifdef CAPTURE_GPIOS
}
#endif

oe_reply_end:
    if (hold_we) {
        oewe_output(0);
#ifndef STM32_HARD_DRIVE_WE_ON_SHARED_WRITE
        we_output(1);      // Pull up
#endif
    }

    data_output_disable();
    oe_output_disable();

    configure_oe_capture_rx(false);
    timer_enable_irq(TIM5, TIM_DIER_CC1IE);
    data_output(0xffffffff);    // Return to pull-up of data pins

#ifdef CAPTURE_GPIOS
    if (hold_we) {
        gpio_showbuf(count);
    }
#endif
}

static void
execute_cmd(uint16_t cmd, uint16_t cmd_len)
{
    uint cons_s;

    switch ((uint8_t) cmd) {
        case KS_CMD_NULL:
            /* Do absolutely nothing (dscard command) */
            break;
        case KS_CMD_NOP:
            /* Do nothing but reply */
            ks_reply(0, 0, NULL, KS_STATUS_OK);
            break;
        case KS_CMD_ID: {
            /* Send Kickflash identification and configuration */
            static const uint32_t reply[] = {
                0x12091610,  // Matches USB ID
                0x00000001,  // Protocol version 0.1
                0x00000001,  // Features
                0x00000000,  // Unused
                0x00000000,  // Unused
            };
            ks_reply(0, sizeof (reply), &reply, KS_STATUS_OK);
            break;  // End processing
        }
        case KS_CMD_TESTPATT: {
            /* Send special data pattern (for test / diagnostic) */
            static const uint32_t reply[] = {
                0x54534554, 0x54544150, 0x53202d20, 0x54524154,
                0xaaaa5555, 0xcccc3333, 0xeeee1111, 0x66669999,
                0x00020001, 0x00080004, 0x00200010, 0x00800040,
                0x02000100, 0x08000400, 0x20001000, 0x80004000,
                0xfffdfffe, 0xfff7fffb, 0xffdfffef, 0xff7fffbf,
                0xfdfffeff, 0xf7fffbff, 0xdfffefff, 0x7fffbfff,
                0x54534554, 0x54544150, 0x444e4520, 0x68646320,
            };
            ks_reply(0, sizeof (reply), &reply, KS_STATUS_OK);
            break;  // End processing
        }
        case KS_CMD_LOOPBACK: {
            /* Send loopback data (for test / diagnostic) */
            const uint we = cmd & KS_EEPROM_WE;
            uint8_t *buf1;
            uint8_t *buf2;
            uint len1;
            uint len2;
            cons_s = rx_consumer;
            if ((int) cons_s <= 0)
                cons_s += ARRAY_SIZE(buffer_rxa_lo);
            if (cons_s * 2 >= cmd_len) {
                /* Send data doesn't wrap */
                len1 = cmd_len;
                buf1 = ((uint8_t *) &buffer_rxa_lo[cons_s]) - len1;
                ks_reply(we, len1, buf1, KS_STATUS_OK);
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
                buf1 = (void *) buffer_rxa_lo;
                len2 = cmd_len - len1;
                buf2 = ((uint8_t *) buffer_rxa_lo) +
                       sizeof (buffer_rxa_lo) - len2;
                if (len2 != 0)
                    ks_reply(we, len2, buf2, KS_STATUS_OK);
                ks_reply(we, len1, buf1, KS_STATUS_OK);
// XXX: This won't work. Maybe need to modify ks_reply() to handle
//      two buffer pointers and lengths. A better option may be
//      to give a flag to ks_reply() which says whether to not
//      send header/do CRC. Then that can be used for the flash
//      write.
            }
            break;  // End processing
        }
        case KS_CMD_ROMSEL: {
            uint16_t bank;
            cons_s = rx_consumer - (cmd_len + 1) / 2 - 1;
            if ((int) cons_s < 0)
                cons_s += ARRAY_SIZE(buffer_rxa_lo);
            bank = buffer_rxa_lo[cons_s];

            if (cmd_len != 2) {
                ks_reply(0, 0, NULL, KS_STATUS_BADLEN);
                break;
            }
            ks_reply(0, 0, NULL, KS_STATUS_OK);
printf("RS l=%u b=%04x\n", cmd_len, bank);
//          ee_address_override(buffer_rxa_lo[cons_s] >> 8, 0);
            break;
        }
        case KS_CMD_FLASH_READ: {
            /* Send command sequence for flash read array command */
            uint32_t addr = SWAP32(0x00555);
            ks_reply(0, sizeof (addr), &addr, KS_STATUS_OK);
            if (ee_mode == EE_MODE_32) {
                uint32_t data = 0x00f000f0;
                ks_reply(1, sizeof (data), &data, 0);  // WE will be set
            } else {
                uint16_t data = 0x00f0;
                ks_reply(1, sizeof (data), &data, 0);  // WE will be set
            }
            break;
        }
        case KS_CMD_FLASH_ID: {
            /* Send command sequence to put it in identify mode */
            static const uint32_t addr[] = {
                SWAP32(0x00555), SWAP32(0x002aa), SWAP32(0x00555)
            };
            ks_reply(0, sizeof (addr), &addr, KS_STATUS_OK);
            if (ee_mode == EE_MODE_32) {
                static const uint32_t data32[] = {
                    0x00aa00aa, 0x00550055, 0x00900090
                };
                ks_reply(1, sizeof (data32), &data32, 0);  // WE will be set
            } else {
                static const uint16_t data16[] = {
                    0x00aa, 0x0055, 0x0090
                };
                ks_reply(1, sizeof (data16), &data16, 0);  // WE will be set
            }
            break;
        }
        case KS_CMD_FLASH_WRITE: {
            /* Send command sequence to perform flash write */
            static const uint32_t addr[] = {
                SWAP32(0x00555), SWAP32(0x002aa), SWAP32(0x00555)
            };
            uint32_t data;

            cons_s = rx_consumer - (cmd_len + 1) / 2 - 1;
            if ((int) cons_s < 0)
                cons_s += ARRAY_SIZE(buffer_rxa_lo);

            if (ee_mode == EE_MODE_32) {
                if (cmd_len != 4) {
                    ks_reply(0, 0, NULL, KS_STATUS_BADLEN);
                    break;
                }
                data = buffer_rxa_lo[cons_s] << 16;
                cons_s++;
                if (cons_s == ARRAY_SIZE(buffer_rxa_lo))
                    cons_s = 0;
                data |= buffer_rxa_lo[cons_s];
            } else {
                if (cmd_len != 2) {
                    ks_reply(0, 0, NULL, KS_STATUS_BADLEN);
                    break;
                }
                data = buffer_rxa_lo[cons_s];
            }

            ks_reply(0, sizeof (addr), &addr, KS_STATUS_OK);
            if (ee_mode == EE_MODE_32) {
                static uint32_t data32[] = {
                    0x00aa00aa, 0x00550055, 0x00a000a0, 0
                };
                data32[3] = data;
                ks_reply(1, sizeof (data32), &data32, 0);  // WE will be set
            } else {
                static uint16_t data16[] = {
                    0x00aa, 0x0055, 0x00a0, 0
                };
                data16[3] = data;
                ks_reply(1, sizeof (data16), &data16, 0);  // WE will be set
            }
            break;
        }
        case KS_CMD_FLASH_ERASE: {
            static const uint32_t addr[] = {
                SWAP32(0x00555), SWAP32(0x002aa), SWAP32(0x00555),
                SWAP32(0x00555), SWAP32(0x002aa),
            };
            if (cmd_len != 0) {
                ks_reply(0, 0, NULL, KS_STATUS_BADLEN);
                break;
            }
            ks_reply(0, sizeof (addr), &addr, KS_STATUS_OK);
            if (ee_mode == EE_MODE_32) {
                static const uint32_t data32[] = {
                    0x00aa00aa, 0x00550055, 0x00800080,
                    0x00aa00aa, 0x00550055, 0x00300030,
                };
                ks_reply(1, sizeof (data32), &data32, 0);  // WE will be set
            } else {
                static const uint16_t data16[] = {
                    0x00aa, 0x0055, 0x00a0,
                    0x00aa, 0x0055, 0x0030
                };
                ks_reply(1, sizeof (data16), &data16, 0);  // WE will be set
            }
            break;
        }
        case KS_CMD_BANK_INFO:
            /* Get bank info */
            ks_reply(0, sizeof (config.bi), &config.bi, KS_STATUS_OK);
            break;
        case KS_CMD_BANK_SET: {
            /* Set ROM bank (options in high bits of command) */
            uint16_t bank;
            cons_s = rx_consumer - (cmd_len + 1) / 2 - 1;
            if ((int) cons_s < 0)
                cons_s += ARRAY_SIZE(buffer_rxa_lo);
            bank = buffer_rxa_lo[cons_s];

            if (cmd_len != 2) {
                ks_reply(0, 0, NULL, KS_STATUS_BADLEN);
                break;
            }
            if (bank >= ROM_BANKS) {
                ks_reply(0, 0, NULL, KS_STATUS_BADARG);
                break;
            }
            ks_reply(0, 0, NULL, KS_STATUS_OK);
            if (cmd & KS_BANK_SETCURRENT) {
                ee_set_bank(bank);
            }
            if (cmd & KS_BANK_SETTEMP) {
                ee_address_override((bank << 4) | 0x7, 0);
            }
            if (cmd & KS_BANK_UNSETTEMP) {
                ee_set_bank(config.bi.bi_bank_current);
            }
            if (cmd & KS_BANK_SETRESET) {
                config.bi.bi_bank_nextreset = bank;
            }
            if (cmd & KS_BANK_SETPOWERON) {
                config.bi.bi_bank_poweron = bank;
                config_updated();
            }
            if (cmd & KS_BANK_REBOOT) {
                kbrst_amiga(0, 0);
            }
            break;
        }
        case KS_CMD_BANK_MERGE: {
            /* Merge or unmerge multiple ROM banks */
            uint     bank;
            uint8_t  bank_start;  // first bank number
            uint8_t  bank_end;    // last bank number
            uint     banks_add;   // additional banks past first
            cons_s = rx_consumer - (cmd_len + 1) / 2 - 1;
            if ((int) cons_s < 0)
                cons_s += ARRAY_SIZE(buffer_rxa_lo);
            bank = buffer_rxa_lo[cons_s];

            if (cmd_len != 2) {
                ks_reply(0, 0, NULL, KS_STATUS_BADLEN);
                break;
            }
            bank_start = (uint8_t) bank;
            bank_end   = bank >> 8;
            banks_add  = bank_end - bank_start;

            /*
             * Bank range must be within 0 to max bank.
             * Bank sizes must be a power of 2 (1, 2, 4, 8).
             * Two-bank ranges must start with an even bank number.
             * Four-bank ranges must start with either bank 0 or 4.
             * Eight-bank ranges must start with bank 0.
             */
            if ((bank_start > bank_end) || (bank_end >= ROM_BANKS) ||
                ((banks_add != 0) && (banks_add != 1) &&
                 (banks_add != 3) && (banks_add != 7)) ||
                ((banks_add == 1) && (bank_start & 1)) ||
                ((banks_add == 3) && (bank_start != 0) && (bank_start != 4)) ||
                ((banks_add == 7) && (bank_start != 0))) {
                ks_reply(0, 0, NULL, KS_STATUS_BADARG);
                break;
            }
            for (bank = bank_start; bank <= bank_end; bank++) {
                if ((((cmd & KS_BANK_UNMERGE) == 0) &&
                     (config.bi.bi_merge[bank] != 0)) ||
                    (((cmd & KS_BANK_UNMERGE) != 0) &&
                     (config.bi.bi_merge[bank] == 0))) {
                    break;  // Stop early -- invalid bank choice
                }
            }
            if (bank < bank_end) {
                ks_reply(0, 0, NULL, KS_STATUS_FAIL);
                break;
            }
            ks_reply(0, 0, NULL, KS_STATUS_OK);
            for (bank = bank_start; bank <= bank_end; bank++) {
                if (cmd & KS_BANK_UNMERGE) {
                    config.bi.bi_merge[bank] = 0;
                } else {
                    config.bi.bi_merge[bank] = (banks_add << 4) |
                                               (bank - bank_start);
                }
            }
            config_updated();
            break;
        }
        case KS_CMD_BANK_COMMENT: {
            /* Add comment (description) to the specified bank */
            uint16_t bank;
            uint     slen;
            uint     pos = 0;
            uint8_t *ptr;
            cons_s = rx_consumer - (cmd_len + 1) / 2 - 1;
            if ((int) cons_s < 0)
                cons_s += ARRAY_SIZE(buffer_rxa_lo);
            bank = buffer_rxa_lo[cons_s];
            slen = cmd_len - 2;
            if (bank >= ROM_BANKS) {
                ks_reply(0, 0, NULL, KS_STATUS_BADARG);
                break;
            }
            if (slen > sizeof (config.bi.bi_desc[0])) {
                ks_reply(0, 0, NULL, KS_STATUS_BADLEN);
                break;
            }
            ks_reply(0, 0, NULL, KS_STATUS_OK);
            while (slen > 0) {
                if (++cons_s >= ARRAY_SIZE(buffer_rxa_lo))
                    cons_s = 0;
                ptr = (uint8_t *) &buffer_rxa_lo[cons_s];
                config.bi.bi_desc[bank][pos++] = ptr[1];
                config.bi.bi_desc[bank][pos++] = ptr[0];

                if (slen == 1)
                    slen = 0;
                else
                    slen -= 2;
            }
            config_updated();
            break;
        }
        case KS_CMD_BANK_LRESET: {
            /* Update long reset sequence of banks */
            uint    bank;
            uint8_t banks[ROM_BANKS];

            cons_s = rx_consumer - (cmd_len + 1) / 2 - 1;
            if ((int) cons_s < 0)
                cons_s += ARRAY_SIZE(buffer_rxa_lo);
            if (cmd_len != sizeof (config.bi.bi_longreset_seq)) {
                /*
                 * All bytes of sequence must be specified.
                 * Unused sequence bytes at the end are 0xff.
                 */
                ks_reply(0, 0, NULL, KS_STATUS_BADLEN);
                break;
            }
            for (bank = 0; bank < ROM_BANKS; bank += 2) {
                banks[bank + 0] = buffer_rxa_lo[cons_s] >> 8;
                banks[bank + 1] = (uint8_t) buffer_rxa_lo[cons_s];
                if (++cons_s >= ARRAY_SIZE(buffer_rxa_lo))
                    cons_s = 0;
            }
            for (bank = 0; bank < ROM_BANKS; bank++) {
                if ((banks[bank] >= ROM_BANKS) && (banks[bank] != 0xff))
                    break;  // Invalid position
                if ((config.bi.bi_merge[banks[bank]] & 0x0f) != 0)
                    break;  // Not start of bank
            }
            if (bank < ROM_BANKS) {
                ks_reply(0, 0, NULL, KS_STATUS_BADARG);
                break;
            }
            ks_reply(0, 0, NULL, KS_STATUS_OK);
            memcpy(config.bi.bi_longreset_seq, banks, ROM_BANKS);
            config_updated();
            break;
        }
        default:
            /* Unknown command */
            ks_reply(0, 0, NULL, KS_STATUS_UNKCMD);
            printf("KS cmd %x?\n", cmd);
            break;
    }
}

/*
 * process_addresses
 * -----------------
 * Walk the ring of captured ROM addresses to detect and act upon commands
 * from the running operating system. This routine is called from interrupt
 * context.
 */
static inline void
process_addresses(void)
{
    static uint     magic_pos = 0;
    static uint16_t len = 0;
    static uint16_t cmd = 0;
    static uint16_t cmd_len = 0;
    static uint32_t crc;
    static uint32_t crc_rx;
    uint            start_wrap = consumer_wrap;
    uint            dma_left;
    uint            prod;

new_cmd:
    dma_left = dma_get_number_of_data(LOG_DMA_CONTROLLER, LOG_DMA_CHANNEL);
    prod     = ARRAY_SIZE(buffer_rxa_lo) - dma_left;

    while (rx_consumer != prod) {
        switch (magic_pos) {
            case 0:
                /* Look for start of Magic sequence (needs to be fast) */
                if (buffer_rxa_lo[rx_consumer] != sm_magic[0])
                    break;
                magic_pos = 1;
                break;
            case 1:
            case 2:
            case 3:  // 1 ... ARRAY_SIZE(sm_magic) - 1
                /* Magic phase */
                if (buffer_rxa_lo[rx_consumer] != sm_magic[magic_pos]) {
                    magic_pos = 0;  // No match
                    break;
                }
                magic_pos++;
                break;
            case ARRAY_SIZE(sm_magic):
                /* Length phase */
                message_count++;
                len = cmd_len = buffer_rxa_lo[rx_consumer];
                crc = crc32r(0, &len, sizeof (uint16_t));
                magic_pos++;
                break;
            case ARRAY_SIZE(sm_magic) + 1:
                /* Command phase */
                cmd = buffer_rxa_lo[rx_consumer];
                crc = crc32r(crc, &cmd, sizeof (uint16_t));
                if (len == 0)
                    magic_pos++;  // Skip following Data Phase
                magic_pos++;
                break;
            case ARRAY_SIZE(sm_magic) + 2:
                /* Data phase */
                len--;
                if (len != 0) {
                    crc = crc32r(crc, (void *) &buffer_rxa_lo[rx_consumer], 2);
                    len--;
                } else {
                    /* Special case -- odd byte at end */
                    crc = crc32r(crc, (void *) &buffer_rxa_lo[rx_consumer], 1);
                }
                if (len == 0)
                    magic_pos++;
                break;
            case ARRAY_SIZE(sm_magic) + 3:
                /* Top half of CRC */
                crc_rx = buffer_rxa_lo[rx_consumer] << 16;
                magic_pos++;
                break;
            case ARRAY_SIZE(sm_magic) + 4:
                /* Bottom half of CRC */
                crc_rx |= buffer_rxa_lo[rx_consumer];
                if (crc_rx != crc) {
                    uint16_t error[2];
                    error[0] = KS_STATUS_CRC;
                    error[1] = crc;
{
// XXX: not fast enough to deal with log
    static uint16_t tempcap[16];
    uint c = rx_consumer;
    int pos;
    for (pos = ARRAY_SIZE(tempcap) - 1; pos > 0; pos--) {
        tempcap[pos] = buffer_rxa_lo[c];
        if (--c == 0)
            c = ARRAY_SIZE(buffer_rxa_lo) - 1;
    }
                    ks_reply(0, sizeof (error), &error, KS_STATUS_CRC);
                    magic_pos = 0;  // Reset magic sequencer

                    printf("cmd=%x l=%04x CRC %08lx != calc %08lx\n",
                           cmd, cmd_len, crc_rx, crc);
    for (pos = 0; pos < ARRAY_SIZE(tempcap); pos++) {
        printf(" %04x", tempcap[pos]);
        if (((pos & 0xf) == 0xf) && (pos != ARRAY_SIZE(tempcap) - 1))
            printf("\n");
    }
    printf("\n");
}
                    magic_pos = 0;  // Restart magic detection
                    break;
                }

                /* Execution phase */
                execute_cmd(cmd, cmd_len);
                magic_pos = 0;  // Restart magic detection
                goto new_cmd;
            default:
                printf("?");
                magic_pos = 0;  // Restart magic detection
                goto new_cmd;
        }

        if (++rx_consumer == ARRAY_SIZE(buffer_rxa_lo)) {
            rx_consumer = 0;
            consumer_wrap++;

            if (consumer_wrap - start_wrap > 10) {
                /*
                 * Spinning too much in interrupt context.
                 * Disable interrupt -- it will be re-enabled in ee_poll().
                 */
                timer_disable_irq(TIM5, TIM_DIER_CC1IE);
                consumer_spin++;
                return;
            }
        }
    }

    dma_left = dma_get_number_of_data(LOG_DMA_CONTROLLER, LOG_DMA_CHANNEL);
    prod = ARRAY_SIZE(buffer_rxa_lo) - dma_left;
    if (rx_consumer != prod)
        goto new_cmd;
}

void
tim5_isr(void)
{
#if 0
    uint flags = TIM_SR(TIM5) & TIM_DIER(TIM5);
    TIM_SR(TIM5) = ~flags;  /* Clear interrupt */
#else
    TIM_SR(TIM5) = 0;  /* Clear all TIM5 interrupt status */
#endif

    process_addresses();
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
    dma_set_priority(dma, channel, DMA_CCR_PL_VERY_HIGH);

    dma_enable_channel(dma, channel);
}

static void
config_tim5_ch1_dma(bool verbose)
{
    if (verbose) {
        memset((void *) buffer_rxa_lo, 0, sizeof (buffer_rxa_lo));
//      printf("Addr lo capture %08x (t5c1) 16-bit\n",
//             (uintptr_t) buffer_rxa_lo);
    }

    /* DMA from address GPIOs A0-A15 to memory */
    config_dma(DMA2, DMA_CHANNEL5, 0, 16,
               &GPIO_IDR(SOCKET_A0_PORT),
               buffer_rxa_lo, ADDR_BUF_COUNT);

    /* Set up TIM5 CH1 to trigger DMA based on external PA0 pin */
    timer_disable_oc_output(TIM5, TIM_OC1);

    /* Enable capture compare CC1 DMA and interrupt */
    timer_enable_irq(TIM5, TIM_DIER_CC1DE | TIM_DIER_CC1IE);

    timer_set_ti1_ch1(TIM5);               // Capture input from channel 1 only

    timer_set_oc_polarity_low(TIM5, TIM_OC1);
    timer_set_oc_value(TIM5, TIM_OC1, 0);

    /* Select the Input and set the filter off */
    TIM5_CCMR1 &= ~(TIM_CCMR1_CC1S_MASK | TIM_CCMR1_IC1F_MASK);
    TIM5_CCMR1 |= TIM_CCMR1_CC1S_IN_TI1 | TIM_CCMR1_IC1F_OFF;

    TIM5_SMCR = TIM_SMCR_ECE;       // external clock mode 2 (ETR input)

    /*
     * TIM5
     * PA0  TIM5_CH1      Filter    Polarity   Trigger  Clock
     *      CC1S_IN_TI1   IC1F_OFF  CCER_CC1P  TI1FP1   ECE
     *                    None      High       TI1      ETR(2)
     *
     * TIM2
     * PA0  TIM2_CH1_ETR  Filter    Polarity   Trigger  Clock
     *      CC1S_IN_TI1   IC1F_OFF  !CCER_CC1P          ECE
     *                    None      Low                 ETR(2)
     *
     * TIM3
     *                            ITR1|ITR2    ECM1
     *
     * Ext clock mode 1 = external input pin (TIx)
     * Ext clock mode 2 = external trigger input (ETR)
     */

//  timer_enable_oc_output(TIM5, TIM_OC1);
}

static void
config_tim2_ch1_dma(bool verbose)
{
    volatile void *src;

    if (verbose) {
        memset((void *) buffer_rxd, 0, sizeof (buffer_rxd));
//      printf("Addr hi capture %08x (t2c1) 16-bit\n",
//             (uintptr_t) buffer_rxd);
    }

    timer_disable_oc_output(TIM2, TIM_OC1);

    /* Word-wide DMA from data GPIOs D0-D15 to memory */
    switch (capture_mode) {
        default:
        case CAPTURE_ADDR:
            src = &GPIO_IDR(SOCKET_A16_PORT);
            break;
        case CAPTURE_DATA_LO:
            src = &GPIO_IDR(FLASH_D0_PORT);
            break;
        case CAPTURE_DATA_HI:
            src = &GPIO_IDR(FLASH_D16_PORT);
            break;
    }
    config_dma(DMA1, DMA_CHANNEL5, 0, 16, src, buffer_rxd, ADDR_BUF_COUNT);

    timer_set_ti1_ch1(TIM2);        // Capture input from channel 1 only

    if (capture_mode == CAPTURE_ADDR)
        timer_set_oc_polarity_low(TIM2, TIM_OC1);
    else
        timer_set_oc_polarity_high(TIM2, TIM_OC1);

    /* Select the Input and set the filter off */
    TIM2_CCMR1 &= ~(TIM_CCMR1_CC1S_MASK | TIM_CCMR1_IC1F_MASK);
    TIM2_CCMR1 |= TIM_CCMR1_CC1S_IN_TI1 | TIM_CCMR1_IC1F_OFF;

    TIM2_SMCR = // TIM_SMCR_ETP      | // falling edge detection
                TIM_SMCR_ECE      | // external clock mode 2 (ETR input)
                TIM_SMCR_ETPS_OFF | // no prescaler
                TIM_SMCR_ETF_OFF;   // no filter
    TIM2_DIER = 0;
    timer_enable_irq(TIM2, TIM_DIER_CC1DE); // DMA on capture/compare event

    timer_set_dma_on_compare_event(TIM2);  // DMA on CCx event occurs
}

void
ee_snoop(uint mode)
{
    uint     last_oe = 1;
    uint     count = 0;
    uint     cons = 0;
    uint     prod = 0;
    uint     oprod = 0;
    uint     no_data = 0;
    uint32_t cap_addr[32];
    uint32_t cap_data[32];

    if (mode != CAPTURE_SW)
        printf("Press any key to exit\n");

    address_output_disable();
    if (mode != CAPTURE_SW) {
        /* Use hardware DMA for capture */
        uint dma_left;
        capture_mode = mode;
        configure_oe_capture_rx(false);
        dma_left = dma_get_number_of_data(LOG_DMA_CONTROLLER, LOG_DMA_CHANNEL);
        prod = ARRAY_SIZE(buffer_rxa_lo) - dma_left;
        if (prod > ARRAY_SIZE(buffer_rxa_lo))
            prod = 0;
        cons = prod;

        while (1) {
            if (((count++ & 0xff) == 0) && getchar() > 0)
                break;
            dma_left = dma_get_number_of_data(LOG_DMA_CONTROLLER,
                                              LOG_DMA_CHANNEL);
            prod = ARRAY_SIZE(buffer_rxa_lo) - dma_left;
            if (prod > ARRAY_SIZE(buffer_rxa_lo))
                prod = 0;
            if (cons != prod) {
                while (cons != prod) {
                    uint addr = buffer_rxa_lo[cons];
                    uint data = buffer_rxd[cons];
                    if (mode == CAPTURE_ADDR) {
                        addr |= ((data & 0xf0) << (16 - 4));
                        printf(" %05x", addr);
                    } else {
                        printf(" %04x[%04x]", addr, data);
                    }
                    cons++;
                    if (cons >= ARRAY_SIZE(buffer_rxa_lo))
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
                    /* FIFO has space, capture address */
                    cap_addr[prod] = addr;
                    oprod = prod;
                    prod = nprod;
                    no_data = 0;
                    continue;
                }
                last_oe = 0;
            }
            continue;
        } else {
            /* Capture data on rising edge of OE */
            if (last_oe == 0) {
                cap_data[oprod] = data_input();
                last_oe = 1;
                continue;
            }
        }
        if ((no_data++ & 0x1ff) != 0)
            continue;
        if (cons != prod) {
            while (cons != prod) {
                printf(" %lx[%08lx]", cap_addr[cons], cap_data[cons]);
                if (++cons >= ARRAY_SIZE(cap_addr))
                    cons = 0;
            }
            printf("\n");
        }
        if ((no_data & 0xffff) != 1)
            continue;
        if (getchar() > 0)
            break;
        no_data = 0;
    }
    timer_enable_irq(TIM5, TIM_DIER_CC1IE);
    printf("\n");
}

int
address_log_replay(uint max)
{
    uint dma_left;
    uint prod;
    uint cons;
    uint addr;
    uint data;
    uint count = 0;
#if 0
    uint flags = TIM_SR(TIM5) & TIM_DIER(TIM5);
    TIM_SR(TIM5) = ~flags;  /* Clear interrupt */
#endif

    dma_left = dma_get_number_of_data(LOG_DMA_CONTROLLER, LOG_DMA_CHANNEL);
    prod = ARRAY_SIZE(buffer_rxa_lo) - dma_left;

    if (prod >= ARRAY_SIZE(buffer_rxa_lo)) {
        printf("Invalid producer=%x left=%x\n", prod, dma_left);
        return (1);
    }
    if (max >= 999) {
        printf("T2C1=%04x %08x\n"
               "T5C1=%04x %08x\n"
               "Wrap=%u\n"
               "Spin=%u\n"
               "Msgs=%u\n",
               (uint) DMA_CNDTR(DMA1, DMA_CHANNEL5), (uintptr_t)buffer_rxd,
               (uint) DMA_CNDTR(DMA2, DMA_CHANNEL5), (uintptr_t)buffer_rxa_lo,
               consumer_wrap, consumer_spin, message_count);
        consumer_wrap = 0;
        consumer_spin = 0;
        message_count = 0;
        return (0);
    }
    if (max > ARRAY_SIZE(buffer_rxa_lo) - 1)
        max = ARRAY_SIZE(buffer_rxa_lo) - 1;

    cons = prod - max;
    if (cons >= ARRAY_SIZE(buffer_rxa_lo)) {
        if (consumer_wrap == 0) {
            cons = 0;
            if (prod == 0) {
                printf("No log entries\n");
                return (1);
            }
        } else {
            cons += ARRAY_SIZE(buffer_rxa_lo);  // Fix negative wrap
        }
    }

    printf("Ent ROMAddr AmigaAddr");
    if (capture_mode == CAPTURE_DATA_LO)
        printf(" DataLo");
    else if (capture_mode == CAPTURE_DATA_HI)
        printf(" DataHi");
    printf("\n");

    while (cons != prod) {
        addr = buffer_rxa_lo[cons];
        data = buffer_rxd[cons];
        if (capture_mode == CAPTURE_ADDR) {
            addr |= ((data & 0xf0) << (16 - 4));
            printf("%3u %05x   %05x\n", cons, addr,
                   (ee_mode == EE_MODE_32) ? (addr << 2) : (addr << 1));
        } else {
            printf("%3u _%04x   %05x     %04x\n", cons, addr,
                   (ee_mode == EE_MODE_32) ? (addr << 2) : (addr << 1), data);
        }
        cons++;
        if (cons >= ARRAY_SIZE(buffer_rxa_lo))
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
     * DMA2 Channel 6 used by TIM3     (slave of TIM2 or TIM5)
     *
     * Only one channel may be active per stream.
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

    rcc_periph_clock_enable(RCC_TIM2);
    rcc_periph_clock_enable(RCC_TIM5);

    rcc_periph_reset_pulse(RST_TIM2);
    rcc_periph_reset_pulse(RST_TIM5);

//  timer_clear_flag(TIM5, TIM_SR(TIM5) & TIM_DIER(TIM5));
    nvic_set_priority(NVIC_TIM5_IRQ, 0x20);
    nvic_enable_irq(NVIC_TIM5_IRQ);

    capture_mode = CAPTURE_ADDR;
    configure_oe_capture_rx(true);

    ticks_per_15_nsec  = timer_nsec_to_tick(15);
    ticks_per_20_nsec  = timer_nsec_to_tick(20);
    ticks_per_30_nsec  = timer_nsec_to_tick(30);
    ticks_per_200_nsec = timer_nsec_to_tick(200);

    /* XXX: These values for bank override should come from NVRAM */
    ee_address_override(0x7, 0);

    ee_set_mode(ee_mode);
}
