/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2024.
 *
 * ---------------------------------------------------------------------
 *
 * Pin tests for board connectivity and soldering issues.
 */

#include "board.h"
#include "main.h"
#include "printf.h"
#include "uart.h"
#include "gpio.h"
#include "pin_tests.h"
#include "m29f160xt.h"
#include "timer.h"
#include "utils.h"

uint8_t  board_is_standalone = 0;
uint8_t  kbrst_in_amiga = 0;


#define PIN_EXT_PULLDOWN 0  // Pin is externally pulled down
#define PIN_EXT_PULLUP   1  // Pin is externally pulled up
#define PIN_INPUT        2  // Pin should float (nothing else drives it)

#define FS_PD            0  // Final state: Pull down
#define FS_PU            1  // Final state: Pull up
#define FS_IN            1  // Final state: Input

typedef struct {
    char     name[12];
    uint32_t port;
    uint16_t pin;
    uint8_t  final_state;
    uint8_t  type;
} pin_config_t;

static const pin_config_t pin_config[] =
{
    { "KBRST",      KBRST_PORT,      KBRST_PIN,      FS_PD, PIN_INPUT },
    { "FLASH_RP",   FLASH_RP_PORT,   FLASH_RP_PIN,   FS_PU, PIN_INPUT },
    { "FLASH_RB1",  FLASH_RB1_PORT,  FLASH_RB1_PIN,  FS_PU, PIN_INPUT },
    { "FLASH_RB2",  FLASH_RB2_PORT,  FLASH_RB2_PIN,  FS_PU, PIN_INPUT },
    { "FLASH_CE",   FLASH_CE_PORT,   FLASH_CE_PIN,   FS_PD, PIN_INPUT },
    { "FLASH_WE",   FLASH_WE_PORT,   FLASH_WE_PIN,   FS_PU, PIN_EXT_PULLUP },
    { "FLASH_OE",   FLASH_OE_PORT,   FLASH_OE_PIN,   FS_PU, PIN_INPUT },
    { "FLASH_A18",  FLASH_A18_PORT,  FLASH_A18_PIN,  FS_PD, PIN_INPUT },
    { "FLASH_A19",  FLASH_A19_PORT,  FLASH_A19_PIN,  FS_PD, PIN_INPUT },
    { "SOCKET_OE",  SOCKET_OE_PORT,  SOCKET_OE_PIN,  FS_PU, PIN_INPUT },
    { "FLASH_OEWE", FLASH_OEWE_PORT, FLASH_OEWE_PIN, FS_PD, PIN_EXT_PULLDOWN },
    { "USB_CC1",    USB_CC1_PORT,    USB_CC1_PIN,    FS_IN, PIN_EXT_PULLDOWN },
    { "USB_CC2",    USB_CC2_PORT,    USB_CC2_PIN,    FS_IN, PIN_EXT_PULLDOWN },
    { "BOOT1",      GPIOB,           GPIO2,          FS_IN, PIN_EXT_PULLDOWN },
};

static const char *
pin_config_get(uint pos, uint32_t *port, uint16_t *pin, char *buf)
{
    if (pos < ARRAY_SIZE(pin_config)) {
        *port = pin_config[pos].port;
        *pin  = pin_config[pos].pin;
        return (pin_config[pos].name);
    }
    pos -= ARRAY_SIZE(pin_config);
    if (pos < 16) {
        /* D0-D15 */
        *port = FLASH_D0_PORT;
        *pin  = BIT(pos);
        sprintf(buf, "D%u", pos);
        return (buf);
    }
    pos -= 16;
    if (pos < 16) {
        /* D16-D31 */
        *port = FLASH_D16_PORT;
        *pin  = BIT(pos);
        sprintf(buf, "D%u", pos + 16);
        return (buf);
    }
    pos -= 16;
    if (pos < 16) {
        /* A0-A15 */
        *port = SOCKET_A0_PORT;
        *pin  = BIT(pos);
        sprintf(buf, "A%u", pos);
        return (buf);
    }
    pos -= 16;
    if (pos < 4) {
        /* A16-A19 */
        *port = SOCKET_A16_PORT;
        *pin  = SOCKET_A16_PIN << pos;
        sprintf(buf, "A%u", pos + 16);
        return (buf);
    }
    printf("BUG: pin_config_get(%u)\n", ARRAY_SIZE(pin_config) + 32 + 16 + pos);
    *port = 0;
    *pin = 0;
    return (NULL);
}

/*
 * check_board_standalone
 * ----------------------
 * Checks whether this board is installed in an Amiga and sets
 * board_is_standalone to FALSE if it is.
 */
void
check_board_standalone(void)
{
    uint     pass;
    uint     cur;
    uint64_t timeout;
    uint16_t got;
    uint16_t got2;
    uint16_t saw;
    uint16_t conn;
    uint32_t curport;
    uint16_t curpin;
    const char *curname;
    char     buf0[8];
    char     buf1[8];
    uint     fail = 0;

    /*
     * Test if KBRST is connected. If connected, pin should be high
     * regardless of STM32 pull-down.
     */
    gpio_setv(KBRST_PORT, KBRST_PIN, 0);
    gpio_setmode(KBRST_PORT, KBRST_PIN, GPIO_SETMODE_INPUT_PULLUPDOWN);
    timer_delay_msec(1);
    got = gpio_get(KBRST_PORT, KBRST_PIN);
    if (got != 0) {
        /*
         * Pin is high even though it's being pulled down -- there must
         * be an external pull-up.
         */
        kbrst_in_amiga = true;
    } else {
        /* Pin is low -- try pulling up */
        gpio_setv(KBRST_PORT, KBRST_PIN, 1);
        timer_delay_msec(1);
        got = gpio_get(KBRST_PORT, KBRST_PIN);
        if (got == 0) {
            printf("Amiga in reset\n");
            kbrst_in_amiga = true;
        } else {
            kbrst_in_amiga = false;
        }
        gpio_setv(KBRST_PORT, KBRST_PIN, 0);
    }

    /*
     * Test whether A18 and A19 are connected to the Amiga.
     *
     * In the first test (0), set both pins high and wait for up to
     * 1 ms for the pins to go high. If a pin was not seen high or
     * last read was not high, then the pin is connected.
     *
     * In the second pass (1), set both pins low and wait for up to
     * 1 ms for the pins to go low. If a pin was not seen low or
     * last read was not low, then the pin is connected.
     *
     * In the final step, wait up to 1ms for pins to change state,
     * which would also indicate they are connected.
     */
    gpio_setmode(SOCKET_A16_PORT, SOCKET_A18_PIN | SOCKET_A19_PIN,
                 GPIO_SETMODE_INPUT_PULLUPDOWN);
    conn = 0;

    for (pass = 0; pass <= 1; pass++) {
        gpio_setv(SOCKET_A16_PORT, SOCKET_A18_PIN | SOCKET_A19_PIN, !pass);
        saw = 0;
        timeout = timer_tick_plus_msec(10);
        while (timer_tick_has_elapsed(timeout) == false) {
            got = gpio_get(SOCKET_A16_PORT,
                           SOCKET_A18_PIN | SOCKET_A19_PIN);
            if (pass == 1)
                got = ~got;
            saw |= got;
            if ((saw & (SOCKET_A18_PIN | SOCKET_A19_PIN)) ==
                       (SOCKET_A18_PIN | SOCKET_A19_PIN)) {
                break;
            }
        }
        conn |= ~saw | ~got;
    }
    printf("Connected: ");
    if (!(conn & SOCKET_A18_PIN))
        putchar('!');
    printf("A18 ");
    if (!(conn & SOCKET_A19_PIN))
        putchar('!');
    printf("A19 ");
    if (!kbrst_in_amiga)
        putchar('!');
    printf("KBRST");

    /*
     * Detect which flash parts are installed (default bus mode)
     *
     * This is done by applying a weak pull-down (pass 0) or
     * pull-up (pass 1) to the data pins and then briefly driving
     * flash output enable. If any pins differ from the pull-up
     * or pull-down value, then it is assumed there is a flash
     * part on those pins. This is not foolproof; if there is a
     * board fault, this can cause false detection.
     */
    gpio_setmode(FLASH_D0_PORT, 0xffff, GPIO_SETMODE_INPUT_PULLUPDOWN);
    saw = 0;
    for (pass = 0; pass <= 1; pass++) {
        gpio_setv(FLASH_D0_PORT, 0xffff, pass);
        gpio_setv(FLASH_D16_PORT, 0xffff, pass);
        timer_delay_msec(1);
        oe_output(0);
        oe_output_enable();
        got  = gpio_get(FLASH_D0_PORT, 0xffff);
        got2 = gpio_get(FLASH_D16_PORT, 0xffff);
        if (pass == 1) {
            got = ~got;
            got2 = ~got2;
        }
        oe_output_disable();
        if (got != 0)
            saw |= BIT(0);
        if (got2 != 0)
            saw |= BIT(1);
    }
    oe_output(1);

    if (saw & BIT(0)) {
        if (saw & BIT(1)) {
            printf(" Flash0 Flash1 (32-bit)\n");
            ee_default_mode = EE_MODE_32;
        } else {
            printf(" Flash0 !Flash1 (16-bit)\n");
            ee_default_mode = EE_MODE_16_LOW;
        }
    } else if (saw & BIT(1)) {
        printf(" !Flash0 Flash1 (16-bit) NOT NORMAL\n");
        ee_default_mode = EE_MODE_16_HIGH;
    } else {
        ee_default_mode = EE_MODE_32;
        printf(" !Flash0 !Flash1 NO FLASH DETECTED\n");
    }

    /*
     * XXX: Might modify this in the future so that ee_mode is read from
     *      NVRAM, and only assigned if it's not been written to the NVRAM.
     */
    ee_mode = ee_default_mode;

    /*
     * Detect Amiga bus mode?
     * This is probably not possible now that there are bus
     * tranceivers (unidirectional) in between the STM32 data
     * pins and the host.
     */

    /*
     * Stand-alone test:
     *  Pull all SOCKET_A0-A15 high, wait 1 ms
     *  If all SOCKET_A0-A15 signals are not high, we are in a system
     *  Pull all SOCKET_A0-A15 low, wait 1 ms
     *  If all SOCKET_A0-A15 signals are not low, we are in a system
     */
    gpio_setmode(SOCKET_A0_PORT, 0xffff, GPIO_SETMODE_INPUT_PULLUPDOWN);

    /* Set pullup and test */
    gpio_setv(SOCKET_A0_PORT, 0xffff, 1);
    gpio_setv(SOCKET_A13_PORT, 0x000e, 1);  // PA1-PA3 = A13-A15
    timer_delay_msec(1);
    got = gpio_get(SOCKET_A0_PORT, 0xffff);
    if (got != 0xffff) {
        printf("A0-A15 pullup got %04x\n", got);
        goto in_amiga;
    }

    /* Set pulldown and test */
    gpio_setv(SOCKET_A0_PORT, 0xffff, 0);
    gpio_setv(SOCKET_A13_PORT, 0x000e, 0);  // PA1-PA3 = A13-A15
    timer_delay_msec(1);
    got = gpio_get(SOCKET_A0_PORT, 0xffff);
    if (got != 0x0000) {
        printf("A0-A15 pulldown got %04x\n", got);
in_amiga:
        /* Set address lines as floating input */
        gpio_setmode(SOCKET_A0_PORT, 0xffff, GPIO_SETMODE_INPUT);
        gpio_setmode(SOCKET_A13_PORT, 0x00fe, GPIO_SETMODE_INPUT);
        board_is_standalone = false;
        return;
    }

    board_is_standalone = true;
    /* Perform data bus tests */

    /* If board is standalone, can test data and address pins */
    /* Set all data pins PU and test */
    /* Set all data pins PD and test */

#if 0
    /* Test OE, WE, and OEWE interaction */
    gpio_setv(SOCKET_OE_PORT, SOCKET_OE_PIN, 1);
    gpio_setmode(SOCKET_OE_PORT, SOCKET_OE_PIN, GPIO_SETMODE_INPUT_PULLUPDOWN);
    gpio_setv(FLASH_OE_PORT, FLASH_OE_PIN, 1);
    gpio_setmode(FLASH_OE_PORT, FLASH_OE_PIN, GPIO_SETMODE_INPUT_PULLUPDOWN);
    gpio_setv(FLASH_WE_PORT, FLASH_WE_PIN, 1);
    gpio_setmode(FLASH_WE, FLASH_WE_PIN, GPIO_SETMODE_INPUT_PULLUPDOWN);
#endif

    /*
     * Set one pin at a time drive high or drive low and verify that no
     * other pins are affected.
     *
     * This test is driven partially by the pin_config[] table, which
     * specifies the pins to test and expected behavior of those pins.
     * All 32 FLASH_D* pins and all 20 SOCKET_A* pins are also verified.
     * This is accomplished in the loops by adding 32 and 19 to the
     */
    for (pass = 0; pass < 2; pass++) {
        uint     state;
        uint     check;
        uint32_t checkport;
        uint16_t checkpin;
        const char *checkname;

        /* Set all pins as input pull-up or pull-down */
        for (cur = 0; cur < ARRAY_SIZE(pin_config) + 32 + 20; cur++) {
            curname = pin_config_get(cur, &curport, &curpin, buf0);
            gpio_setv(curport, curpin, !pass);
            gpio_setmode(curport, curpin, GPIO_SETMODE_INPUT_PULLUPDOWN);
        }

        /* Verify pins made it to the expected state */
        for (cur = 0; cur < ARRAY_SIZE(pin_config) + 32 + 20; cur++) {
            curname = pin_config_get(cur, &curport, &curpin, buf0);
            state = !!gpio_get(curport, curpin);
            if (cur < ARRAY_SIZE(pin_config)) {
                uint type = pin_config[cur].type;
                if (((state == 0) && type == PIN_EXT_PULLDOWN) ||
                    ((state == 1) && type == PIN_EXT_PULLUP)) {
                    /* Okay to ignore */
                    continue;
                }
                if (((state == 1) && type == PIN_EXT_PULLDOWN) ||
                    ((state == 0) && type == PIN_EXT_PULLUP)) {
                    /*
                     * External pull-up or pull-down is always stronger than
                     * STM32 internal pull-up or pull-down (~30k).
                     */
                    if (fail++ == 0)
                        printf("FAIL pin short tests\n");
                    printf("%-4s %s has external pull-%s but state is %u\n",
                           gpio_to_str(curport, curpin), curname,
                           (type == PIN_EXT_PULLUP) ? "up" : "down",
                           state);
                    continue;
                }
            }
            if (state != !pass) {
                if (fail++ == 0)
                    printf("FAIL pin short tests\n");
                printf("  %-4s %s did not go %s (%u)\n",
                       gpio_to_str(curport, curpin),
                       curname, !pass ? "high" : "low", !pass);
            }
        }

        for (cur = 0; cur < ARRAY_SIZE(pin_config) + 32 + 20; cur++) {
            curname = pin_config_get(cur, &curport, &curpin, buf0);

            /* Set one pin the opposite of the others */
            gpio_setv(curport, curpin, pass);
            gpio_setmode(curport, curpin, GPIO_SETMODE_OUTPUT_PPULL_2);

            /* Check other pins for wrong state */
            for (check = 0; check < ARRAY_SIZE(pin_config) + 32 + 20; check++) {
                if (check == cur)
                    continue;

                checkname = pin_config_get(check, &checkport, &checkpin, buf1);

                state = !!gpio_get(checkport, checkpin);
                if (state != !pass) {
                    if (check < ARRAY_SIZE(pin_config)) {
                        uint type = pin_config[check].type;
                        if (((state == 0) && type == PIN_EXT_PULLDOWN) ||
                            ((state == 1) && type == PIN_EXT_PULLUP)) {
                            /* Okay to ignore */
                            continue;
                        }
                    }
                    if ((pass == 0) &&
                        (curport == FLASH_RP_PORT) &&
                        (curpin == FLASH_RP_PIN) &&
                        (checkport == FLASH_RB1_PORT) &&
                        ((checkpin == FLASH_RB1_PIN) ||
                         (checkpin == FLASH_RB2_PIN))) {
                        /*
                         * Okay to ignore
                         * FLASH_RP=0 causes FLASH_RB1=0 and FLASH_RB2=0
                         */
                        continue;
                    }
                    if (((curport == FLASH_OE_PORT) &&
                         (curpin == FLASH_OE_PIN) &&
                         (checkport == SOCKET_OE_PORT) &&
                         (checkpin == SOCKET_OE_PIN)) ||
                        ((curport == SOCKET_OE_PORT) &&
                         (curpin == SOCKET_OE_PIN) &&
                         (checkport == FLASH_OE_PORT) &&
                         (checkpin == FLASH_OE_PIN))) {
                        /* OE pins connected by resistor */
                        continue;
                    }
                    if (((curport == FLASH_A18_PORT) &&
                         (curpin == FLASH_A18_PIN) &&
                         (checkport == SOCKET_A16_PORT) &&
                         (checkpin == SOCKET_A18_PIN)) ||
                        ((curport == SOCKET_A16_PORT) &&
                         (curpin == SOCKET_A18_PIN) &&
                         (checkport == FLASH_A18_PORT) &&
                         (checkpin == FLASH_A18_PIN))) {
                        /* A18 pins connected by resistor */
                        continue;
                    }
                    if (((curport == FLASH_A19_PORT) &&
                         (curpin == FLASH_A19_PIN) &&
                         (checkport == SOCKET_A16_PORT) &&
                         (checkpin == SOCKET_A19_PIN)) ||
                        ((curport == SOCKET_A16_PORT) &&
                         (curpin == SOCKET_A19_PIN) &&
                         (checkport == FLASH_A19_PORT) &&
                         (checkpin == FLASH_A19_PIN))) {
                        /* A19 pins connected by resistor */
                        continue;
                    }
                    if ((pass == 1) &&
                        (curport == FLASH_RP_PORT) &&
                        (curpin == FLASH_RP_PIN) &&
                        ((checkport = FLASH_D0_PORT) ||
                         (checkport == FLASH_D16_PORT))) {
                        /* FLASH_RP=1 drives data pins */
                        continue;
                    }
                    if ((pass == 0) &&
                        (curport == FLASH_WE_PORT) &&
                        (curpin == FLASH_WE_PIN) &&
                        (((checkport = FLASH_OE_PORT) &&
                          (checkpin == FLASH_OE_PIN)) ||
                         ((checkport = SOCKET_OE_PORT) &&
                          (checkpin == SOCKET_OE_PIN)))) {
                        /*
                         * FLASH_WE causes flash to drive FLASH_OE,
                         * which is connected to SOCKET_OE
                         */
                        continue;
                    }
                    if ((pass == 1) &&
                        (curport == FLASH_OEWE_PORT) &&
                        (curpin == FLASH_OEWE_PIN) &&
                        (checkport == SOCKET_OE_PORT) &&
                        (checkpin == SOCKET_OE_PIN)) {
                        /*
                         * FLASH_OEWE high enables MOSFET which can allow
                         * FLASH_WE (pulled high by LED) to strongly pull
                         * SOCKET_OE high.
                         */
                        continue;
                    }
                    if (fail++ == 0)
                        printf("FAIL pin short tests\n");
                    printf("  %-4s %s=%u caused ",
                           gpio_to_str(curport, curpin), curname, pass);
                    printf("%-4s %s=%u\n",
                           gpio_to_str(checkport, checkpin), checkname, state);
                }
            }

            /* Restore pin back to pull-up / pull-down */
            gpio_setv(curport, curpin, !pass);
            gpio_setmode(curport, curpin, GPIO_SETMODE_INPUT_PULLUPDOWN);
        }
    }

    /* Restore all pins to input pull-up/pull-down and final state */
    for (cur = 0; cur < ARRAY_SIZE(pin_config) + 32 + 20; cur++) {
        uint final = FS_PU;  // default to pull-up
        uint mode = GPIO_SETMODE_INPUT_PULLUPDOWN;
        curname = pin_config_get(cur, &curport, &curpin, buf0);
        if (cur < ARRAY_SIZE(pin_config)) {
            final = pin_config[cur].final_state;
            if (final == FS_IN) {
                final = FS_PD;
                mode = GPIO_SETMODE_INPUT;
            }
        }
        gpio_setv(curport, curpin, final);
        gpio_setmode(curport, curpin, mode);
    }
}
