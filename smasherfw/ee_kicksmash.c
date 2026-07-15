/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2026.
 *
 * ---------------------------------------------------------------------
 *
 * Kicksmash-specific access code for reads and writes
 */

#include "board.h"
#include "main.h"
#include "printf.h"
#include "uart.h"
#include <stdbool.h>
#include <string.h>
#include "ee_kicksmash.h"
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
 * EE_MODE_32_SWAP = 32-bit flash low / high flash swapped
 * EE_MODE_16_LOW  = 16-bit flash low device (bits 0-15)
 * EE_MODE_16_HIGH = 16-bit flash high device (bits 16-31)
 */
uint            ee_mode = EE_MODE_32;
// static uint32_t ee_cmd_mask;
// static uint32_t ee_addr_shift;
// static uint32_t ee_status = EE_STATUS_NORMAL;  // Status from program/erase

static uint32_t ticks_per_20_nsec;
static uint32_t ticks_per_30_nsec;
static uint32_t ticks_per_35_nsec;
static uint64_t ee_last_access = 0;
static bool     ee_enabled = false;
// static bool     ee_write_bug = true;
void            smash_restore_bank(void);

/*
 * address_output
 * --------------
 * Writes the specified value to the address output pins.
 */
void
address_output(uint32_t addr)
{
    GPIO_ODR(SOCKET_A0_PORT)   = addr & 0xffff;           // Set A0-A12
    GPIO_BSRR(SOCKET_A13_PORT) = 0x00fe0000 |             // Clear A13-A19
                                 ((addr >> 12) & 0x00fe); // Set A13-A19
}

#if 0
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
#endif

/*
 * address_output_enable
 * ---------------------
 * Enables the address pins for output.
 */
static void
address_output_enable(void)
{
//  ee_address_override(0, 1);  // Suspend A19-A18-A17 override

    /* A0-A12=PC0-PC12 A13-A19=PA1-PA7 */
    GPIO_CRL(SOCKET_A0_PORT)  = 0x33333333;  // Output Push-Pull
    GPIO_CRH(SOCKET_A0_PORT)  = 0x44433333;  // Not PC13-PC15 (weak drive)
    GPIO_CRL(SOCKET_A13_PORT) = 0x33333338;  // PA0=SOCKET_OE = Input PU
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
    GPIO_CRL(SOCKET_A0_PORT)   = 0x44444444;  // Input
    GPIO_CRH(SOCKET_A0_PORT)   = 0x44444444;
    GPIO_CRL(SOCKET_A13_PORT)  = 0x44444448;  // PA0=SOCKET_OE = Input PU
}

/*
 * data_output
 * -----------
 * Writes the specified value to the data output pins.
 */
void
data_output(uint32_t data)
{
    if (ee_mode == EE_MODE_32_SWAP) {
        GPIO_ODR(SOCKET_D0_PORT)  = (data >> 16);              // Set D16-D31
        GPIO_ODR(SOCKET_D16_PORT) = data;                      // Set D0-D15
    } else {
        GPIO_ODR(SOCKET_D0_PORT)  = data;                      // Set D0-D15
        GPIO_ODR(SOCKET_D16_PORT) = (data >> 16);              // Set D16-D31
    }
}

/*
 * data_input
 * ----------
 * Returns the current value present on the data pins.
 */
static uint32_t
data_input(void)
{
    if (ee_mode == EE_MODE_32_SWAP) {
        /*
         * D0-D15  = PE0-PE15
         * D16-D15 = PD0-PD15
         */
        return ((GPIO_IDR(SOCKET_D0_PORT) << 16) | GPIO_IDR(SOCKET_D16_PORT));
    } else {
        /*
         * D0-D15  = PD0-PD15
         * D16-D15 = PE0-PE15
         */
        return (GPIO_IDR(SOCKET_D0_PORT) | (GPIO_IDR(SOCKET_D16_PORT) << 16));
    }
}

/*
 * data_output_enable
 * ------------------
 * Enables the data pins for output.
 */
void
data_output_enable(void)
{
    GPIO_CRL(SOCKET_D0_PORT)  = 0x33333333; // Output Push-Pull
    GPIO_CRH(SOCKET_D0_PORT)  = 0x33333333;
    GPIO_CRL(SOCKET_D16_PORT) = 0x33333333;
    GPIO_CRH(SOCKET_D16_PORT) = 0x33333333;
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
    GPIO_CRL(SOCKET_D0_PORT)  = 0x88888888; // Input Pull-Up / Pull-Down
    GPIO_CRH(SOCKET_D0_PORT)  = 0x88888888;
    GPIO_CRL(SOCKET_D16_PORT) = 0x88888888;
    GPIO_CRH(SOCKET_D16_PORT) = 0x88888888;
    data_output(0);
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
//  gpio_setv(SOCKET_OE_PORT, SOCKET_OE_PIN | SOCKET_CE_PIN, value);
    uint set = value ? SOCKET_OE_PIN : 0;
    GPIO_BSRR(SOCKET_OE_PORT) = ((SOCKET_OE_PIN | SOCKET_CE_PIN) << 16) | set;
}

/*
 * oe_output_enable
 * ----------------
 * Enable drive of the SOCKET_OE pin, which is flash output enable OE#.
 */
void
oe_output_enable(void)
{
    /* SOCKET_OE = PB13 */
    /* SOCKET_CE = PB14 */
    GPIO_CRH(SOCKET_OE_PORT) = (GPIO_CRH(SOCKET_OE_PORT) & 0xf00fffff) |
                              0x01100000;  // Output

//  gpio_setmode(SOCKET_OE_PORT, SOCKET_OE_PIN, GPIO_SETMODE_OUTPUT_PPULL_50);
}

/*
 * oe_output_disable
 * -----------------
 * Disable drive of the SOCKET_OE pin, which is flash output enable OE#.
 */
void
oe_output_disable(void)
{
    /* SOCKET_OE = PB13 */
    /* SOCKET_CE = PB14 */
    GPIO_CRH(SOCKET_OE_PORT) = (GPIO_CRH(SOCKET_OE_PORT) & 0xf00fffff) |
                              0x08800000;  // Input Pull Up/Down

//  gpio_setmode(SOCKET_OE_PORT, SOCKET_OE_PIN, GPIO_SETMODE_INPUT_PULLUPDOWN);
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
    oe_output(1);
    oe_output_enable();
    data_output_disable();
    ee_enabled = true;
//  ee_read_mode();
    ee_last_access = timer_tick_get();
}

/*
 * ee_disable
 * ----------
 * Tri-states all address and data lines to the device.
 */
void
ee_disable(void)
{
    if (ee_enabled == false)
        return;
    oe_output_disable();
    address_output_disable();
    data_output_disable();
    timer_delay_usec(50);
    ee_enabled = false;
    ee_last_access = 0;
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
 *
 * Timing comparison with M29F800CB
 *       MXIC        Micron
 *      M29F800CB   M29F160FT
 * tOE   30ns        20ns        Time to valid read data following OE low
 * tAA   70ns        55ns        Time to valid read data following address
 * tDF   20ns        15ns        Time to HiZ after OE high
 */
static void
ee_read_word(uint32_t addr, uint32_t *data)
{
    address_output(addr);
    address_output_enable();
    oe_output(0);
    oe_output_enable();
    timer_delay_ticks(ticks_per_30_nsec);  // Wait for tOE
    *data = data_input();
    oe_output(1);
    oe_output_disable();
    timer_delay_ticks(ticks_per_20_nsec);  // Wait for tDF
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

    uint32_t *data = datap;

    while (count-- > 0)
        ee_read_word(addr++, data++);

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
    timer_delay_usec(1);  // Wait for WE to rise
    ee_cmd(0x00555, 0x00f000f0);
}

#if 0
typedef struct {
    uint16_t cv_id;       // Vendor code
    char     cv_vend[12]; // Vendor string
} chip_vendors_t;

static const chip_vendors_t chip_vendors[] = {
    { 0x0001, "AMD" },      // AMD, Alliance, ST, Micron, others
    { 0x0004, "Fujitsu" },
    { 0x0020, "ST" },
    { 0x00c2, "Macronix" }, // MXIC
    { 0x0000, "Unknown" },  // Must remain last
};
#endif

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
    { 0x000422D8, "M29F160TB" },   // Fujitsu 2MB bottom boot
    { 0x00c222D6, "MX29F800CT" },  // Macronix 1MB top boot
    { 0x00c22258, "MX29F800CB" },  // Macronix 1MB bottom boot
    { 0x00c222c4, "MX29LV160CT" }, // Macronix 2MB top boot
    { 0x00c22249, "MX29LV160CB" }, // Macronix 2MB bottom boot
    { 0x002022cc, "M29F160BT" },   // ST-Micro 2MB top boot
    { 0x0020224b, "M29F160BB" },   // ST-Micro 2MB bottom boot
    { 0x002022c4, "M29W160ET" },   // ST-Micro 2MB top boot
    { 0x00202249, "M29W160EB" },   // ST-Micro 2MB bottom boot
    { 0x00000000, "Unknown" },     // Must remain last
};

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
        if (usec > 100000) {  // 100 ms
            smash_restore_bank();
            ee_disable();
        }
    }
}

#if 0
/*
 * ee_test() checks that all pins of flash parts are connected and that
 *           the flash parts can be identified.
 */
int
ee_test(void)
{
    uint32_t addr;
    uint     block;
    const chip_blocks_t *cb1;
    const chip_blocks_t *cb2;
    const char *id1;
    const char *id2;
    uint32_t part1;
    uint32_t part2;
    uint32_t val;
    uint64_t zerodata;
    uint pos;
    int rc = 0;

    /* Verify flash parts can be identified */
    ee_enable();
    ee_id(&part1, &part2);
    cb1 = get_chip_block_info(part1);
    cb2 = get_chip_block_info(part2);

    id1 = ee_id_string(part1);

    id2 = ee_id_string(part2);
    if ((strcmp(id1, "Unknown") == 0) ||
        (strcmp(id1, "Unknown") == 0) ||
        (cb1->cb_chipid == 0) ||
        (cb2->cb_chipid == 0)) {
        printf("FAIL: ");
        rc = 1;
    }
    printf("Prom %08lx %08lx %s %s\n", part1, part2, id1, id2);
    if (rc != 0)
        return (rc);

    /*
     * Read Autoselect address 0x3 while pulling high all data pins.
     * The value should always be 0x0000000 unless a flash part is not
     * populated or pins are not making contact.
     */
    ee_cmd(0x00555, 0x00900090);
    data_output_disable();    // set pull-up or pull-down
    data_output(0xffffffff);  // pull high
    ee_read_word(0x3, &val);
    ee_read_mode();

    /* ST M29W160Ex parts have a non-zero value at word 3 */
    if ((part1 == 0x002022c4) || (part1 == 0x00202249))  //  M29W160Ex
        val &= ~0x00000001;
    if ((part2 == 0x002022c4) || (part2 == 0x00202249))  //  M29W160Ex
        val &= ~0x00010000;

    if (val != 0x00000000) {
        printf("Flash data %08lx should be 00000000.\n"
               "Bits floating or stuck:", val);
        for (pos = 0; pos < 32; pos++)
            if (val & BIT(pos)) {
                uint npos;
                printf(" %u", pos);
                for (npos = pos; npos < 32; npos++)
                    if ((val & BIT(npos + 1)) == 0)
                        break;
                if (pos != npos) {
                    printf("-%u", npos);
                    pos = npos;
                }
            }
        printf("\n");
        return (1);
    }

    /* Verify that no blocks are locked */
    ee_cmd(0x00555, 0x00900090);
    for (block = 0, addr = 0; addr < EE_DEVICE_SIZE; ) {
        uint bsize = cb1->cb_bsize << 10;
        uint bnum  = addr / bsize;

        if (bnum == cb1->cb_bbnum) {
            /* Boot block has variable block size */
            uint soff = addr - bnum * bsize;

            uint snum = soff / (cb1->cb_ssize << 10);
            uint smap = cb1->cb_map;
            bsize = 0;
            do {
                bsize += (cb1->cb_ssize << 10);
                snum++;
                if (smap & BIT(snum))
                    break; // At next block
            } while (snum < 8);
        }
        ee_read_word(addr + 2, &val);
        if ((val == 0x00000001) ||
            (val == 0x00010000) ||
            (val == 0x00010001)) {
            if (rc++ == 0)
                printf("Flash blocks locked: ");
            printf(" 0x%x:", block);
            if (val == 0x00000001)
                printf("01");
            else if (val == 0x00010000)
                printf("10");
            else
                printf("11");
            rc++;
        } else if (val != 0x0000) {
            printf("Invalid flash block lock status addr=%06lx block=%x "
                   "status=%08lx\n",
                   addr, block, val);
            ee_read_mode();
            return (1);
        }

        addr += bsize;
        block++;
    }
    if (rc != 0)
        printf("\n");

    /*
     * Put the flash in CFI Query mode. In this mode, the first
     * 0x400 bytes should not shadow address 0x0. This allows code
     * to test A1-A8. Maybe A1-A7 on some flash parts.
     */
    ee_cmd(0x55, 0x98);
    ee_read(0, &zerodata, sizeof (zerodata));
    for (pos = 1; pos < 8; pos++) {
        uint64_t data;
        ee_read(BIT(pos), &data, sizeof (data));
        if (data == zerodata) {
            printf("FAIL: CFI wrap at A%x\n", pos);
        }
    }

    ee_read_mode();
    return (rc);
}
#endif

void
ee_init(void)
{
    ticks_per_20_nsec  = timer_nsec_to_tick(20);
    ticks_per_30_nsec  = timer_nsec_to_tick(30);
    ticks_per_35_nsec  = timer_nsec_to_tick(35);
    ee_mode = config.ee_mode;
}
