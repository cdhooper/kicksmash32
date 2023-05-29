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

/*
 * EE_MODE_32      = 32-bit flash
 * EE_MODE_16_LOW  = 16-bit flash low device (bits 0-15)
 * EE_MODE_16_HIGH = 16-bit flash high device (bits 16-31)
 */
uint            ee_mode = EE_MODE_16_LOW;
static uint32_t ee_cmd_mask;
static uint32_t ee_status = EE_STATUS_NORMAL;  // Status from program/erase

static uint32_t ticks_per_15_nsec;
static uint32_t ticks_per_20_nsec;
static uint32_t ticks_per_30_nsec;
static uint64_t ee_last_access = 0;
static bool     ee_enabled = false;
uint8_t         board_is_standalone = 0;

/*
 * check_board_standalone
 * ----------------------
 * Checks whether this board is installed in an Amiga and sets
 * board_is_standalone to FALSE if it is.
 */
void
check_board_standalone(void)
{
    uint32_t got;

    /*
     * Stand-alone test:
     *  Pull all SOCKET_A0-A15 high, wait 1 ms
     *  If all SOCKET_A0-A15 signals are not high, we are in a system
     *  Pull all SOCKET_A0-A15 low, wait 1 ms
     *  If all SOCKET_A0-A15 signals are not low, we are in a system
     */
    gpio_setmode(SOCKET_A0_PORT, 0xffff, GPIO_SETMODE_INPUT_PULLUPDOWN);

    /* Set pullup and test */
    gpio_setv(SOCKET_A0_PORT, 0xffff, 0xffff);
#if BOARD_REV > 1
    gpio_setv(SOCKET_A13_PORT, 0x000e, 0x000e);  // PA1-PA3 = A13-A15
#endif
    timer_delay_msec(1);
    got = gpio_get(SOCKET_A0_PORT, 0xffff);
    if (got != 0xffff) {
        printf("A0-A15 pullup got %04lx\n", got);
        goto in_amiga;
    }

    /* Set pulldown and test */
    gpio_setv(SOCKET_A0_PORT, 0xffff, 0x0000);
#if BOARD_REV > 1
    gpio_setv(SOCKET_A13_PORT, 0x000e, 0x0000);  // PA1-PA3 = A13-A15
#endif
    timer_delay_msec(1);
    got = gpio_get(SOCKET_A0_PORT, 0xffff);
    if (got != 0x0000) {
        printf("A0-A15 pulldown got %04lx\n", got);
        goto in_amiga;
    }

    board_is_standalone = true;
    printf("DEBUG: not acting as standalone\n");
//  return;  // Leave socket address lines with weak pulldown
in_amiga:
    /* Return to floating input */
    gpio_setmode(SOCKET_A0_PORT, 0xffff, GPIO_SETMODE_INPUT);
#if BOARD_REV > 1
    gpio_setmode(SOCKET_A13_PORT, 0x00fe, GPIO_SETMODE_INPUT);
#endif
    board_is_standalone = false;
}

/*
 * address_output
 * --------------
 * Writes the specified value to the address output pins.
 */
static void
address_output(uint32_t addr)
{
#if BOARD_REV == 1
    /*
     * The address bits are split across two port registers (more than 16 bits)
     */
    GPIO_ODR(SOCKET_A0_PORT)   = addr & 0xffff;           // Set A0-A15
    GPIO_BSRR(SOCKET_A16_PORT) = 0x03c00000 |             // Clear A16-A19
                                 ((addr >> 10) & 0x03c0); // Set A16-A19
#else
    /* BOARD_REV 2+ */
    GPIO_ODR(SOCKET_A0_PORT)   = addr & 0xffff;           // Set A0-A12
    GPIO_BSRR(SOCKET_A13_PORT) = 0x00fe0000 |             // Clear A13-A19
                                 ((addr >> 12) & 0x00fe); // Set A13-A19
#endif
}

/*
 * address_input
 * -------------
 * Returns the current value present on the address pins.
 */
static uint32_t
address_input(void)
{
#if BOARD_REV == 1
    uint32_t addr = GPIO_IDR(SOCKET_A0_PORT);
    addr |= ((GPIO_IDR(SOCKET_A16_PORT) & 0x03c0) << (16 - 6));
#elif 0
    /* BOARD_REV 2+ */
    uint32_t addr = GPIO_IDR(SOCKET_A0_PORT) & 0x1fff;
    addr |= ((GPIO_IDR(SOCKET_A13_PORT) & 0x00fe) << (13 - 1));
#else
    /* Alternative to above for BOARD_REV 2+ */
    uint32_t addr = GPIO_IDR(SOCKET_A0_PORT);
    addr |= ((GPIO_IDR(SOCKET_A16_PORT) & 0x00f0) << (16 - 4));
#endif
    return (addr);
}

/*
 * address_override
 * ----------------
 * Override the Flash A18 and A19 address
 *
 * override = 0  Temporarily disable override
 * override = 1  Record new override
 * override = 2  Restore previous override
 */
static void
address_override(uint8_t bits, uint override)
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

    if (val & BIT(3)) {
        uint shift = (val & BIT(7)) ? 0 : 16;
        GPIO_BSRR(FLASH_A19_PORT) = FLASH_A19_PIN << shift;
        gpio_setmode(FLASH_A19_PORT, FLASH_A19_PIN,
                     GPIO_SETMODE_OUTPUT_PPULL_2);
        printf(" A18=%d", !shift);
    } else {
        gpio_setmode(FLASH_A19_PORT, FLASH_A19_PIN, GPIO_SETMODE_INPUT);
        printf(" !A19");
    }
    if (val & BIT(2)) {
        uint shift = (val & 4) ? 0 : 16;
        GPIO_BSRR(FLASH_A18_PORT) = FLASH_A18_PIN << shift;
        gpio_setmode(FLASH_A18_PORT, FLASH_A18_PIN,
                     GPIO_SETMODE_OUTPUT_PPULL_2);
        printf(" A18=%d", !shift);
    } else {
        gpio_setmode(FLASH_A18_PORT, FLASH_A18_PIN, GPIO_SETMODE_INPUT);
        printf(" !A18");
    }
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
#if BOARD_REV == 1
    /*
     * Each register manages up to 8 GPIOs
     *  A0...A7  are in the first port CRL register
     *  A8...A15 are in the first port CRH register
     * A16...A19 are in the second port CRL register bottom half / middle
     */
    GPIO_CRL(SOCKET_A0_PORT)  = 0x11111111;  // Output Push-Pull
    GPIO_CRH(SOCKET_A0_PORT)  = 0x11111111;

    /* PC6...PC9 */
    GPIO_CRL(SOCKET_A16_PORT) = (GPIO_CRL(SOCKET_A16_PORT) & 0x00ffffff) |
                                0x11000000;
    GPIO_CRH(SOCKET_A16_PORT) = (GPIO_CRH(SOCKET_A16_PORT) & 0xffffff00) |
                                0x00000011;
#else
    /* BOARD_REV 2+ */
    /* A0-A12=PC0-PC12 A13-A19=PA1-PA7 */
    GPIO_CRL(SOCKET_A0_PORT)  = 0x11111111;  // Output Push-Pull
    GPIO_CRH(SOCKET_A0_PORT)  = 0x00011111;
    GPIO_CRL(SOCKET_A13_PORT) = 0x11111118;  // PA0=SOCKET_OE = Input
    address_override(0, 0);  // Suspend A19-A18 override
#endif
}

/*
 * address_output_disable
 * ----------------------
 * Reverts the address pins back to input (don't drive).
 */
static void
address_output_disable(void)
{
#if BOARD_REV == 1
    /*
     * Each register manages up to 8 GPIOs
     *  A0...A7  are in the first port CRL register
     *  A8...A15 are in the first port CRH register
     * A16...A19 straddle the second port CRL and CRH
     */
    GPIO_CRL(SOCKET_A0_PORT)   = 0x88888888; // Input Pull-Up / Pull-Down
    GPIO_CRH(SOCKET_A0_PORT)   = 0x88888888;
    /* PC6...PC9 */
    GPIO_CRL(SOCKET_A16_PORT)  = (GPIO_CRL(SOCKET_A16_PORT) & 0x00ffffff) |
                                 0x88000000;
    GPIO_CRH(SOCKET_A16_PORT)  = (GPIO_CRH(SOCKET_A16_PORT) & 0xffffff00) |
                                 0x00000088;
    GPIO_ODR(SOCKET_A0_PORT)   = 0x00000000;  // Pull down A0-A15
    GPIO_ODR(SOCKET_A16_PORT) &= 0xfffffc3f;  // Pull down A16-A19
#else
    /* BOARD_REV 2+ */
    /* A0-A12=PC0-PC12 A13-A19=PA1-PA7 */
    GPIO_CRL(SOCKET_A0_PORT)   = 0x44444444;  // Input
    GPIO_CRH(SOCKET_A0_PORT)   = 0x44444444;
    GPIO_CRL(SOCKET_A13_PORT)  = 0x44444448;  // PA0=SOCKET_OE = Input PU
    address_override(0, 2);  // Restore previous A19-A18 override
#endif
}

/*
 * data_output
 * -----------
 * Writes the specified value to the data output pins.
 */
static void
data_output(uint32_t data)
{
#if BOARD_REV == 1
    GPIO_ODR(SOCKET_D0_PORT)   = data;                     // Set D15-D0

    GPIO_BSRR(SOCKET_D16_PORT) = 0x00ff0000 |              // Clear D16-D23
                                 ((data >> 16) & 0x00ff);  // Set D16-D23
    GPIO_BSRR(SOCKET_D24_PORT) = 0x000f0000 |              // Clear D24-D27
                                 ((data >> 24) & 0x000f);  // Set D24-D27
    GPIO_BSRR(SOCKET_D28_PORT) = 0x04000000 |              // Clear D28
                                 ((data >> 18) & 0x0400);  // Set D28
    GPIO_BSRR(SOCKET_D29_PORT) = 0x03200000 |              // Clear D29-D31
                                 ((data >> 24) & 0x0020) | // Set D29
                                 ((data >> 22) & 0x0300);  // Set D30-D31
#else
    /* BOARD_REV 2+ */
    GPIO_ODR(SOCKET_D0_PORT)  = data;                      // Set D0-D15
    GPIO_ODR(SOCKET_D16_PORT) = (data >> 16);              // Set D16-D31
#endif
}

/*
 * data_input
 * ----------
 * Returns the current value present on the data pins.
 */
static uint32_t
data_input(void)
{
#if BOARD_REV == 1
    /*
     * Board Rev 1
     *
     * D0-D15  = PD0-PD15
     * D16-D23 = PA0-PA7
     * D24-D27 = PC0-PC3
     * D28     = PC10
     * D29     = PB5
     * D30-D31 = PB8-PB9
     */
    return (GPIO_IDR(SOCKET_D0_PORT) |                      // D0-D15
            ((GPIO_IDR(SOCKET_D16_PORT) & 0x00ff) << 16) |  // D16-D23
            ((GPIO_IDR(SOCKET_D24_PORT) & 0x000f) << 24) |  // D24-D27
            ((GPIO_IDR(SOCKET_D28_PORT) & 0x0400) << 18) |  // D28
            ((GPIO_IDR(SOCKET_D29_PORT) & 0x0020) << 24) |  // D29
            ((GPIO_IDR(SOCKET_D30_PORT) & 0x0300) << 22));  // D30-D31
#else
    /*
     * Board Rev 2+
     *
     * D0-D15  = PD0-PD15
     * D16-D15 = PE0-PE15
     */
    return (GPIO_IDR(SOCKET_D0_PORT) | (GPIO_IDR(SOCKET_D16_PORT) << 16));
#endif
}

/*
 * data_output_enable
 * ---------------------
 * Enables the data pins for output.
 */
static void
data_output_enable(void)
{
#if BOARD_REV == 1
    GPIO_CRL(SOCKET_D0_PORT)  = 0x11111111; // Output Push-Pull
    GPIO_CRH(SOCKET_D0_PORT)  = 0x11111111;
    GPIO_CRL(SOCKET_D16_PORT) = 0x11111111;

    GPIO_CRL(SOCKET_D24_PORT) = 0x00001111 |
                                (GPIO_CRL(SOCKET_D24_PORT) & ~0x0000ffff);
    GPIO_CRH(SOCKET_D28_PORT) = 0x00000100 |
                                (GPIO_CRH(SOCKET_D28_PORT) & ~0x00000f00);
    GPIO_CRL(SOCKET_D29_PORT) = 0x00100000 |
                                (GPIO_CRL(SOCKET_D29_PORT) & ~0x00f00000);
    GPIO_CRH(SOCKET_D30_PORT) = 0x00000011 |
                                (GPIO_CRH(SOCKET_D30_PORT) & ~0x000000ff);
#else
    /* BOARD_REV 2+ */
    GPIO_CRL(SOCKET_D0_PORT)  = 0x11111111; // Output Push-Pull
    GPIO_CRH(SOCKET_D0_PORT)  = 0x11111111;
    GPIO_CRL(SOCKET_D16_PORT) = 0x11111111;
    GPIO_CRH(SOCKET_D16_PORT) = 0x11111111;
#endif
}

/*
 * data_output_disable
 * -------------------
 * Reverts the data pins back to input (don't drive).
 */
static void
data_output_disable(void)
{
#if BOARD_REV == 1
    GPIO_CRL(SOCKET_D0_PORT)  = 0x88888888; // Input Pull-Up / Pull-Down
    GPIO_CRH(SOCKET_D0_PORT)  = 0x88888888;
    GPIO_CRL(SOCKET_D16_PORT) = 0x88888888;
    GPIO_CRL(SOCKET_D24_PORT) = 0x00008888 |
                                (GPIO_CRL(SOCKET_D24_PORT) & ~0x0000ffff);
    GPIO_CRH(SOCKET_D28_PORT) = 0x00000800 |
                                (GPIO_CRH(SOCKET_D28_PORT) & ~0x00000f00);
    GPIO_CRL(SOCKET_D29_PORT) = 0x00800000 |
                                (GPIO_CRL(SOCKET_D29_PORT) & ~0x00f00000);
    GPIO_CRH(SOCKET_D30_PORT) = 0x00000088 |
                                (GPIO_CRH(SOCKET_D30_PORT) & ~0x000000ff);
#else
    /* BOARD_REV 2+ */
    /* D0-D15 = PD0-PD15, D16-D31=PE0-PE15 */
    GPIO_CRL(SOCKET_D0_PORT)  = 0x44444444; // Input
    GPIO_CRH(SOCKET_D0_PORT)  = 0x44444444;
    GPIO_CRL(SOCKET_D16_PORT) = 0x44444444;
    GPIO_CRH(SOCKET_D16_PORT) = 0x44444444;
#endif
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
 * oe_output
 * ---------
 * Drives the OE# (flash output enable) pin with the specified value.
 */
static void
oe_output(uint value)
{
#ifdef DEBUG_SIGNALS
    printf(" OE=%d", value);
#endif
    gpio_setv(FLASH_OE_PORT, FLASH_OE_PIN, value);
#if 0
    if (board_is_standalone)
        gpio_setv(SOCKET_OE_PORT, SOCKET_OE_PIN, value);
#endif
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
static void
oe_output_enable(void)
{
#if 0
    gpio_setmode(FLASH_OE_PORT, FLASH_OE_PIN, GPIO_SETMODE_OUTPUT_PPULL_50);
#else
    /* FLASH_OE = PB13 */
    GPIO_CRH(FLASH_OE_PORT) = (GPIO_CRH(FLASH_OE_PORT) & 0xff0fffff) |
                              0x00100000;  // Output
#endif
}

/*
 * oe_output_disable
 * -----------------
 * Disable drive of the FLASH_OE pin, which is flash output enable OE#.
 */
static void
oe_output_disable(void)
{
#if 0
    gpio_setmode(FLASH_OE_PORT, FLASH_OE_PIN, GPIO_SETMODE_INPUT);
#else
    /* FLASH_OE = PB13 */
    GPIO_CRH(FLASH_OE_PORT) = (GPIO_CRH(FLASH_OE_PORT) & 0xff0fffff) |
                              0x00400000;  // Input
#endif
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
    ticks_per_15_nsec  = timer_nsec_to_tick(15);
    ticks_per_20_nsec  = timer_nsec_to_tick(20);
    ticks_per_30_nsec  = timer_nsec_to_tick(30);
    address_output(0);
    address_output_enable();
    we_output(1);
    oe_output(1);
    oe_output_enable();
    data_output_disable();
    ee_enabled = true;
    ee_read_mode();
#ifdef DEBUG_SIGNALS
    printf("GPIOA=%x GPIOB=%x GPIOC=%x GPIOD=%x GPIOE=%x\n",
           GPIOA, GPIOB, GPIOC, GPIOD, GPIOE);
#endif

    if (ee_mode == 0)
        ee_cmd_mask = 0xffffffff;  // 32-bit
    else if (ee_mode == 1)
        ee_cmd_mask = 0x0000ffff;  // 16-bit low
    else
        ee_cmd_mask = 0xffff0000;  // 16-bit high
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
    uint32_t status = 0;
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
        if (status == lstatus) {
            if (same_count++ >= 1) {
                /* Same for 2 tries */
                if (verbose)
                    printf("    Done\n");
                ee_status = EE_STATUS_NORMAL;
                return (0);
            }
        } else {
            if (same_count)
                printf("S");
            same_count = 0;
            lstatus = status;
        }

        if (status & (BIT(5) | BIT(5 + 16)))  // Program / erase failure
            if (see_fail_count++ > 5)
                break;

        if (verbose) {
            if (report_time < usecs / 1000000) {
                report_time = usecs / 1000000;
                printf("\r%08lx %u", status, report_time);
            }
        }
    }
    if (verbose) {
        report_time = usecs / 1000000;
        printf("\r%08lx %u.%03u sec", status, report_time,
               (uint) ((usecs - report_time * 1000000) / 1000));
    }

    if (status & (BIT(5) | BIT(5 + 16))) {
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
                printf("Program failed -- trying again at 0x%lx\n", addr);
#endif
                goto try_again;
            }
            printf("  Program failed at 0x%lx\n", addr << 1);
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
                printf("Program mismatch -- trying again at 0x%lx\n", addr);
#endif
                goto try_again;
            }
            printf("  Program mismatch at 0x%lx\n", addr << 1);
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
    uint32_t cb_chipid;
    uint8_t  cb_bbnum;   // Boot block number (0=Bottom boot)
    uint8_t  cb_bsize;   // Common block size in Kwords (typical 32K)
    uint8_t  cb_ssize;   // Boot block sector size in Kwords (typical 4K)
    uint8_t  cb_map;     // Boot block sector erase map
} chip_blocks_t;

static const chip_blocks_t chip_blocks[] = {
    { 0x000122D2, 31, 32, 4, 0x71 },  // M29F160FT  01110001 16K 4K 4K 8K
    { 0x000122D8,  0, 32, 4, 0x1d },  // M29F160FB  00011101 8K 4K 4K 16K
    { 0x000122D6, 15, 32, 4, 0x71 },  // M29F800FT  01110001 16K 4K 4K 8K
    { 0x00012258,  0, 32, 4, 0x1d },  // M29F800FB  00011101 8K 4K 4K 16K
    { 0x00c222D6, 15, 32, 4, 0x71 },  // MX29F800CT 01110001 16K 4K 4K 8K
    { 0x00c22258,  0, 32, 4, 0x1d },  // MX29F800CB 00011101 8K 4K 4K 16K
    { 0x00000000,  0, 32, 4, 0x1d },  // No match: default to bottom boot 2MB
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
    uint pos;
    for (pos = 0; pos < ARRAY_SIZE(chip_blocks) - 1; pos++)
        if (chip_blocks[pos].cb_chipid == chipid)
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

    /* Need to figure out if this is a top boot or bottom boot part */
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
        ee_write_word(0x002aa, 0x00559055);

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
ee_snoop(void)
{
    uint     last_oe = 1;
    uint     cons = 0;
    uint     prod = 0;
    uint     iters = 0;
    uint     no_data = 0;
    uint32_t captures[32];
    uint32_t laddr = 0xffffffff;

    address_output_disable();
    while (1) {
        if (oe_input() == 0) {
            /* Only capture on falling edge of OE */
            if (last_oe == 1) {
                uint32_t addr = address_input();
                if (addr != laddr) {
                    uint nprod = prod + 1;
                    if (nprod >= ARRAY_SIZE(captures))
                        nprod = 0;
                    if (nprod != cons) {
                        /* FIFO has space */
                        captures[prod] = addr;
                        prod = nprod;
                        laddr = addr;
                        no_data = 0;
                        if (iters++ >= 30) {
                            if (getchar() > 0)
                                goto abort;
                            iters = 0;
                        }
                        continue;
                    }
                }
                last_oe = 0;
            }
        } else {
            last_oe = 1;
        }
        if (no_data++ < 100)
            continue;
        if (cons != prod) {
            while (cons != prod) {
                printf(" %lx", captures[cons]);
                if (++cons >= ARRAY_SIZE(captures))
                    cons = 0;
            }
            printf("\n");
        }
        if (getchar() > 0)
            break;
    }
abort:
    printf("\n");
}


/* Buffer to store the results of the ADC conversion */
#define ADDR_BUF_COUNT 64
volatile uint16_t addr_buffer[ADDR_BUF_COUNT];
uint addr_cons = 0;
uint8_t reply_buffer[256];

#define KS_CMD_ID       0x01  // Reply with software ID
#define KS_CMD_TESTPATT 0x02  // Reply with bit test pattern
#define KS_CMD_EEPROM   0x03  // Issue command to EEPROM
#define KS_CMD_ROMSEL   0x04  // Force or release A18 and A19
#define KS_CMD_NOP      0x12  // Do nothing

#define KS_STATUS_OK    0x0000  // Success
#define KS_STATUS_FAIL  0x0001  // Failure
#define KS_STATUS_CRC   0x0002  // CRC failure

#define KS_FLAG_WE      0x0100  // Issue command to EEPROM (given with CMD)

#define KS_ROMSEL_SAVE  0x0001  // Save setting in NVRAM
#define KS_ROMSEL_SET   0x0f00  // ROM select bits to force
#define KS_ROMSEL_BITS  0xf000  // ROM select bit values (A19 is high bit)

static const uint16_t ks_magic[] = { 0x0011 };

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
    uint pos = 0;
    uint tlen = (ee_mode == EE_MODE_32) ? 4 : 2;
//    uint64_t start;

    for (count = 0; oe_input() == 0; count++) {
        if (count > 100000) {
            printf("OE timeout 01\n");
            return;
        }
    }
    if (count > 0)
        printf("<%u>", count);

    oe_output_enable();
    if (ee_mode == EE_MODE_16_HIGH)
        pos -= 2;  /* Change position so data is in upper 16 bits */

    while ((int)len > 0) {
        uint32_t dval = 0;
        dval = *(uint32_t *) ((uint8_t *)reply_buf + pos);
#if 0
        if (ee_mode == EE_MODE_16_HIGH)
            dval <<= 16;
#endif
//   printf(",%x,", dval);
        pos += tlen;
        len -= tlen;
        data_output(dval);
        for (count = 0; oe_input() != 0; count++) {
            if (count > 100000) {
                printf("OE timeout 0\n");
                goto oe_reply_end;
            }
        }
        data_output_enable();  // Drive data
//        start = timer_tick_get();
        if (hold_we)
            we_output(0);
        for (count = 0; oe_input() == 0; count++) {
            if (count > 100000) {
                if (hold_we)
                    we_output(1);
                printf("OE timeout 1\n");
                goto oe_reply_end;
            }
        }
        data_output_disable();
//        printf("%u ", (unsigned int) (timer_tick_get() - start));
//      printf("%u ", count);
        if (hold_we)
            we_output(1);
    }
oe_reply_end:
    oe_output_disable();
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
    if (prod >= ARRAY_SIZE(addr_buffer))
        return;
    while (cons != prod) {
//      printf(" %x:%04x", cons, addr_buffer[cons]);
        if (magic_pos++ < ARRAY_SIZE(ks_magic)) {
            /* Magic phase */
            if (addr_buffer[cons] != ks_magic[magic_pos - 1])
                magic_pos = 0;  // No match
        } else if (magic_pos == ARRAY_SIZE(ks_magic) + 1) {
            /* Command phase */
            cmd = addr_buffer[cons];
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
                case KS_CMD_TESTPATT: {
                    static const uint32_t reply[] = {
                        0xaaaa5555, 0xcccc3333, 0xeeee1111, 0x66669999,
                        0x00020001, 0x00080004, 0x00200010, 0x00800040,
                        0x02000100, 0x08000400, 0x20001000, 0x80004000,
                        0xfffdfffe, 0xfff7fffb, 0xffdfffef, 0xff7fffbf,
                        0xfdfffeff, 0xf7fffbff, 0xdfffefff, 0x7fffbfff
                    };
                    oe_reply(0, sizeof (reply), &reply);
                    magic_pos = 0;
                    printf("TP\n");
                    break;
                }
                case KS_CMD_EEPROM:
                case KS_CMD_ROMSEL:
                    break;
                case KS_CMD_NOP:
                    magic_pos = 0;  // Do nothing
                    break;
                default:
printf("unk %x\n", cmd);
                    magic_pos = 0;  // Unknown command
                    break;
            }
            crc = crc32(0, (void *) &addr_buffer[cons], sizeof (uint16_t));
        } else if (magic_pos == ARRAY_SIZE(ks_magic) + 2) {
            /* Length phase */
            len = cmd_len = addr_buffer[cons];
            crc = crc32(crc, (void *) &addr_buffer[cons], sizeof (uint16_t));
        } else if (len-- > 0) {
            /* Data in phase */
            if (len == 0) {
                crc = crc32(crc, (void *) &addr_buffer[cons], 1);
            } else {
                len--;
                crc = crc32(crc, (void *) &addr_buffer[cons], 2);
            }
        } else if ((uint16_t) crc != addr_buffer[cons]) {
            /* CRC failed */
            uint16_t error = KS_STATUS_CRC;
            oe_reply(0, sizeof (error), &error);
printf("%04x CRC %04x != exp %04x %x %x\n", cmd, addr_buffer[cons], (uint16_t)crc, (uint16_t)crc << 1, (uint16_t)crc << 2);
            magic_pos = 0;  // Unknown command
        } else {
            /* Execution phase */
            switch ((uint8_t) cmd) {
                case KS_CMD_EEPROM: {
                    const uint we = cmd & KS_FLAG_WE;
                    uint8_t *buf1;
                    uint8_t *buf2;
                    uint len1;
                    uint len2;
                    uint cons_s = cons;
                    if ((int) cons_s <= 0)
                        cons_s += ARRAY_SIZE(addr_buffer);
                    if (cons_s * 2 >= cmd_len) {
                        /* Send data doesn't wrap */
                        len1 = cmd_len;
                        buf1 = ((uint8_t *) &addr_buffer[cons_s]) - len1;
// don't write
// dnw prom 22 1;dwn prom 6 1;dwn prom 8 1;dwn prom 1234 4;dwn prom e5d8 1;dw prom 20 10
// MAGIC 0x11
// CMD 0x3
// LEN 0x4
// DATA 0x091a 0x091b
// CRC 72ec
//
// Array id mode
// dnw prom 22 1;dwn prom 206 1;dwn prom 4 1;dwn prom 120 2;dwn prom 36ae 1;dw prom aaa 1;dw prom 0 10
// DATA 0x0090
// ADDR 0x0555
                        oe_reply(we, len1, buf1);
if (we)
    printf("we\n");
                    } else {
                        /* Send data from beginning and end of buffer */
                        len1 = cons_s * 2;
                        buf1 = (void *) addr_buffer;
                        len2 = cmd_len - len1;
                        buf2 = ((uint8_t *) addr_buffer) +
                               sizeof (addr_buffer) - len2;
                        if (len2 != 0)
                            oe_reply(we, len2, buf2);
                        oe_reply(we, len1, buf1);
                    }
// printf("%04x %04x %04x ", *(uint16_t *)buf1, *(uint16_t *)(buf1 + 2), *(uint16_t *)(buf1 + 4));
                    break;
                }
                case KS_CMD_ROMSEL: {
                    uint16_t status = KS_STATUS_OK;
                    uint cons_s = cons - 1;
                    if ((int) cons_s <= 0)
                        cons_s += ARRAY_SIZE(addr_buffer);
printf("%04x %04x ", addr_buffer[cons_s], addr_buffer[cons_s + 1]);
                    oe_reply(0, sizeof (status), &status);
                    address_override(addr_buffer[cons_s] >> 8, 1);
// 11
// dnw prom 22 1;dwn prom 8 1;dwn prom 4 1;dwn prom 19800 1;dwn prom 2d88 1; dw prom 20 8
// ~
// dnw prom 22 1;dwn prom 8 1;dwn prom 4 1;dwn prom 0 1;dwn prom 3a72 1; dw prom 20 8
                    break;
                }
                default:
                    magic_pos = 0;  // Unknown command
                    break;
            }
            magic_pos = 0;  // Unknown command
        }
        if (++cons >= ARRAY_SIZE(addr_buffer))
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
    producer = ARRAY_SIZE(addr_buffer) - dma_left;
    process_addresses(producer);
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
     * -        SPI1_RX   SPI1_TX   SPI2_Tx   SPI2       -         -
     *                              I2S2_Tx   I2S2_T
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
     * DMA1 Channel 1 is used by ADC
     * DMA2 Channel 5 is used by ROM OE DMA
     */
    uint32_t dma     = DMA2;
    uint32_t channel = DMA_CHANNEL5;

    memset((void *) addr_buffer, 0, sizeof (addr_buffer));
    printf("ABUF 0x%x\n", (unsigned int) addr_buffer);

    rcc_periph_clock_enable(RCC_DMA2);

    dma_disable_channel(dma, channel);

    dma_channel_reset(dma, channel);
    dma_set_peripheral_address(dma, channel,
                               (uintptr_t)&GPIO_IDR(SOCKET_A0_PORT));
    dma_set_memory_address(dma, channel, (uintptr_t)addr_buffer);
    dma_set_read_from_peripheral(dma, channel);
    dma_set_number_of_data(dma, channel, ADDR_BUF_COUNT);
    dma_disable_peripheral_increment_mode(dma, channel);
    dma_enable_memory_increment_mode(dma, channel);
    dma_set_peripheral_size(dma, channel, DMA_CCR_PSIZE_16BIT);
    dma_set_memory_size(dma, channel, DMA_CCR_MSIZE_16BIT);
    dma_enable_circular_mode(dma, channel);
    dma_set_priority(dma, channel, DMA_CCR_PL_MEDIUM);

    dma_disable_transfer_error_interrupt(dma, channel);
    dma_disable_half_transfer_interrupt(dma, channel);
    dma_disable_transfer_complete_interrupt(dma, channel);
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

    dma_enable_channel(dma, channel);

    /* Set up TIM5 CH1 to trigger DMA based on external PA0 pin */
    rcc_periph_clock_enable(RCC_TIM5);
    rcc_periph_reset_pulse(RST_TIM5);

    timer_disable_counter(TIM5);
    timer_disable_oc_output(TIM5, TIM_OC1);

    /* Enable capture compare CC1 DMA and interrupt */
    timer_enable_irq(TIM5, TIM_DIER_CC1DE | TIM_DIER_CC1IE);
//    timer_update_on_overflow(TIM5);

//    timer_set_dma_on_compare_event(TIM5);  // DMA request when CCx event occurs
    timer_set_ti1_ch1(TIM5);               // Capture input from channel 1 only
//  timer_slave_set_polarity(TIM5, 0);     // Slave Polarity falling edge

    timer_continuous_mode(TIM5);
    timer_set_oc_polarity_low(TIM5, TIM_OC1);
    timer_set_oc_value(TIM5, TIM_OC1, 1);

    /* Select the Input and set the filter */
    TIM5_CCMR1 &= ~(TIM_CCMR1_CC1S_MASK | TIM_CCMR1_IC1F_MASK);
    TIM5_CCMR1 |= TIM_CCMR1_CC1S_IN_TI1 | TIM_CCMR1_IC1F_OFF;

    timer_enable_oc_output(TIM5, TIM_OC1);
    timer_enable_counter(TIM5);

    timer_clear_flag(TIM5, TIM_SR(TIM5) & TIM_DIER(TIM5));
#if 0
    nvic_set_priority(NVIC_DMA2_CHANNEL5_IRQ, 0x20);
    nvic_enable_irq(NVIC_DMA2_CHANNEL5_IRQ);
    nvic_set_pending_irq(NVIC_DMA2_CHANNEL5_IRQ);  // Debug
#endif
    nvic_set_priority(NVIC_TIM5_IRQ, 0x20);
    nvic_enable_irq(NVIC_TIM5_IRQ);
//  nvic_set_pending_irq(NVIC_TIM5_IRQ);  // Debug

#if 0
    timer_generate_event(TIM5, TIM_EGR_TG): // Generate fake Trigger event
    timer_generate_event(TIM5, TIM_EGR_UG): // Generate fake Update event
#endif

    /*
     * DMA/interrupt enable register (TIMx_DIER)
     *     UDE: Update DMA request enable
     *     CC1DE: Capture/Compare 1 DMA request enable
     *     CC2DE: Capture/Compare 2 DMA request enable
     *     CC3DE: Capture/Compare 3 DMA request enable
     *     CC4DE: Capture/Compare 4 DMA request enable
     *     COMDE: COM DMA request enable
     *     TDE: trigger DMA request enable
     */
    // TIM_DIER_UDE
    // timer_enable_irq(TIM2, TIM_DIER_UDE);
}
