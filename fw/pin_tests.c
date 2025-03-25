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
#include "config.h"
#include "usb.h"
#include "cmdline.h"
#include "prom_access.h"
#include "led.h"

uint8_t  board_is_standalone = 0;
uint8_t  kbrst_in_amiga = 0;


#define PIN_EXT_PULLDOWN 0  // Pin is externally pulled down
#define PIN_EXT_PULLUP   1  // Pin is externally pulled up
#define PIN_INPUT        2  // Pin should float (nothing else drives it)

#define FS_PD            0  // Final state: Pull down
#define FS_PU            1  // Final state: Pull up
#define FS_IN            2  // Final state: Input
#define FS_0             3  // Final state: Drive to 0

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
    { "FLASH_RP",   FLASH_RP_PORT,   FLASH_RP_PIN,   FS_PU, PIN_EXT_PULLUP },
    { "FLASH_RB",   FLASH_RB_PORT,   FLASH_RB_PIN,   FS_PU, PIN_INPUT },
    { "FLASH_WE",   FLASH_WE_PORT,   FLASH_WE_PIN,   FS_PU, PIN_EXT_PULLUP },
    { "FLASH_OE",   FLASH_OE_PORT,   FLASH_OE_PIN,   FS_PU, PIN_INPUT },
    { "FLASH_A18",  FLASH_A18_PORT,  FLASH_A18_PIN,  FS_PD, PIN_INPUT },
    { "FLASH_A19",  FLASH_A19_PORT,  FLASH_A19_PIN,  FS_PD, PIN_INPUT },
    { "SOCKET_D31", SOCKET_D31_PORT, SOCKET_D31_PIN, FS_PU, PIN_INPUT },
    { "SOCKET_OE",  SOCKET_OE_PORT,  SOCKET_OE_PIN,  FS_PU, PIN_INPUT },
    { "FLASH_OEWE", FLASH_OEWE_PORT, FLASH_OEWE_PIN, FS_0,  PIN_EXT_PULLDOWN },
    { "BOOT1",      GPIOB,           GPIO2,          FS_IN, PIN_EXT_PULLDOWN },
    /* USB-C host may pull these low */
//  { "USB_CC1",    USB_CC1_PORT,    USB_CC1_PIN,    FS_IN, PIN_EXT_PULLDOWN },
//  { "USB_CC2",    USB_CC2_PORT,    USB_CC2_PIN,    FS_IN, PIN_EXT_PULLDOWN },
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
    uint     d31_conn;
    int      rc;
    uint64_t timeout;
    uint16_t got;
    uint16_t got2;
    uint16_t saw;
    uint     armed;
    uint16_t conn;

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
     * Test whether D31 is connected to the Amiga.
     *
     * In the first test (0), pull D31 pin low and wait for up to
     * 2 ms for the pin to go high. If the pin was not seen low,
     * then the pin is connected.
     *
     * In the second pass (1), set D31 pin high and wait for up to
     * 2 ms for the pin to go high. If the pin was not seen high,
     * then the pin is connected.
     *
     * The test will finish sooner (armed) if the state is first the
     * same and then opposite of whether the pin is pulled low or high.
     */
    gpio_setmode(SOCKET_D31_PORT, SOCKET_D31_PIN,
                 GPIO_SETMODE_INPUT_PULLUPDOWN);
    conn = 0;

    usb_poll();
    for (pass = 0; pass <= 1; pass++) {
        gpio_setv(SOCKET_D31_PORT, SOCKET_D31_PIN, pass);
        timeout = timer_tick_plus_msec(2);
        armed = 0;
        while (timer_tick_has_elapsed(timeout) == false) {
            got = gpio_get(SOCKET_D31_PORT, SOCKET_D31_PIN);
            if (pass == 1)
                got ^= SOCKET_D31_PIN;
            if (got) {
                if (armed) {
                    conn = 1;  // pin is opposite of pull-up or pull-down
                    break;     // can stop now
                }
            } else {
                /* In the set pull-up or pull-down state */
                armed = 1;
            }
        }
        if (armed == 0) {
            /* Didn't arm within timeout period -- must be connected */
            conn = 1;
        }
        if (conn)
            break;
    }
    d31_conn = conn ? 1 : 0;

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
     * The test will finish sooner (armed) if the state is first the
     * same and then opposite of whether the pin is pulled low or high.
     */
    gpio_setmode(FLASH_A18_PORT, FLASH_A18_PIN | FLASH_A19_PIN,
                 GPIO_SETMODE_INPUT);
    gpio_setmode(SOCKET_A16_PORT,
                 FLASH_A17_PIN | SOCKET_A18_PIN | SOCKET_A19_PIN,
                 GPIO_SETMODE_INPUT_PULLUPDOWN);
    conn = 0;

    for (pass = 0; pass <= 1; pass++) {
        gpio_setv(SOCKET_A16_PORT,
                  FLASH_A17_PIN | SOCKET_A18_PIN | SOCKET_A19_PIN, pass);
        armed = 0;
        timeout = timer_tick_plus_msec(10);
        while (timer_tick_has_elapsed(timeout) == false) {
            uint igot;
            got = gpio_get(SOCKET_A16_PORT,
                           FLASH_A17_PIN | SOCKET_A18_PIN | SOCKET_A19_PIN);
            if (pass == 0) {
                igot = got;
                got ^= (FLASH_A17_PIN | SOCKET_A18_PIN | SOCKET_A19_PIN);
            } else {
                igot = got ^ (FLASH_A17_PIN | SOCKET_A18_PIN | SOCKET_A19_PIN);
            }

            /* Arm if got is same as pull-up or pull-down */
            armed |= got;

            /* Mark connected if armed and got is opposite of expected */
            conn |= (armed & igot);

            /* If all are connected, then stop early */
            if ((conn & (FLASH_A17_PIN | SOCKET_A18_PIN | SOCKET_A19_PIN)) ==
                        (FLASH_A17_PIN | SOCKET_A18_PIN | SOCKET_A19_PIN)) {
                break;
            }
        }
        /* Anything which didn't arm within timeout period is connected */
        conn |= (armed ^ ((FLASH_A17_PIN | SOCKET_A18_PIN | SOCKET_A19_PIN)));

        if (kbrst_in_amiga)
            break;  // Only run pass 0
    }
    printf("Connected: ");
    if (!(conn & FLASH_A17_PIN))
        putchar('!');
    printf("A17 ");
    if (!(conn & SOCKET_A18_PIN))
        putchar('!');
    printf("A18 ");
    if (!(conn & SOCKET_A19_PIN))
        putchar('!');
    printf("A19 ");
    if (!d31_conn)
        putchar('!');
    printf("D31 ");
    if (!kbrst_in_amiga)
        putchar('!');
    printf("KBRST");
    usb_poll();

    if (kbrst_in_amiga) {
        printf("\n");
        goto in_amiga;  // Can't do further tests in a running Amiga
    }

    if (d31_conn ||
        (conn & (FLASH_A17_PIN | SOCKET_A18_PIN | SOCKET_A19_PIN))) {
        printf("\n");
        if (d31_conn)
            printf("%s connected but KBRST is not\n", "D31");
        if (conn & FLASH_A17_PIN)
            printf("%s connected but KBRST is not\n", "A17");
        if (conn & FLASH_A18_PIN)
            printf("%s connected but KBRST is not\n", "A18");
        if (conn & FLASH_A19_PIN)
            printf("%s connected but KBRST is not\n", "A19");
        led_alert(1);
        goto in_amiga;
    }

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
            printf(" Flash0 Flash1\n");
            ee_default_mode = EE_MODE_32;
        } else {
            printf(" Flash0 !Flash1\n");
            ee_default_mode = EE_MODE_16_LOW;
        }
    } else if (saw & BIT(1)) {
        printf(" !Flash0 Flash1 (NOT NORMAL)\n");
        ee_default_mode = EE_MODE_16_HIGH;
    } else {
        ee_default_mode = EE_MODE_32;
        printf(" !Flash0 !Flash1 NO FLASH DETECTED\n");
        led_alert(1);
    }

    if (kbrst_in_amiga) {
        /*
         * If upper data lines are not connected to Amiga, then set the
         * flash default mode to 16-bit.
         */
        if (!d31_conn) {
            /* D31 floats -- probably a 16-bit Amiga */
            if (ee_default_mode == EE_MODE_32)
                ee_default_mode = EE_MODE_16_LOW;
        }
    }

    if (config.ee_mode != EE_MODE_AUTO) {
        ee_set_mode(config.ee_mode);
    } else {
        ee_set_mode(ee_default_mode);
    }

    /* Set pullup and test */
    gpio_setmode(SOCKET_A0_PORT, 0xffff, GPIO_SETMODE_INPUT_PULLUPDOWN);
    gpio_setv(SOCKET_A0_PORT, 0xffff, 1);
    gpio_setv(SOCKET_A13_PORT, 0x000e, 1);  // PA1-PA3 = A13-A15
    timer_delay_msec(1);
    got = gpio_get(SOCKET_A0_PORT, 0xffff);
    if (got != 0xffff) {
        printf("A0-A15 pullup got %04x\n", got);
        led_alert(1);
        goto in_amiga;
    }

    /* Set pulldown and test */
    gpio_setv(SOCKET_A0_PORT, 0xffff, 0);
    gpio_setv(SOCKET_A13_PORT, 0x000e, 0);  // PA1-PA3 = A13-A15
    timer_delay_msec(1);
    got = gpio_get(SOCKET_A0_PORT, 0xffff);
    if (got != 0x0000) {
        printf("A0-A15 pulldown got %04x\n", got);
        led_alert(1);
in_amiga:
        /* Set address lines as floating input */
        gpio_setmode(SOCKET_A0_PORT, 0xffff, GPIO_SETMODE_INPUT);
        gpio_setmode(SOCKET_A13_PORT, 0x00fe, GPIO_SETMODE_INPUT);
        board_is_standalone = false;
        return;
    }

    board_is_standalone = true;
    rc = pin_tests();
    if (rc == 0)
        rc = prom_test();
    if (rc != 0)
        led_alert(1);
}

/*
 * pin_tests
 * ---------
 * Performs stand-alone board pin tests.
 */
uint
pin_tests(void)
{
    uint     pass;
    uint     cur;
    char     buf0[8];
    char     buf1[8];
    uint     fail = 0;
    uint32_t curport;
    uint16_t curpin;
    const char *curname;

    /* Perform data bus tests */
    if (board_is_standalone == false) {
        printf("This test may only be performed on a stand-alone board\n");
        return (RC_FAILURE);
    }

    /* Set alternate PA13 | PA14 | PA15 to be input */
    gpio_setmode(SOCKET_A13_PORT, GPIO1 | GPIO2 | GPIO3, GPIO_SETMODE_INPUT);

    /*
     * Stand-alone test:
     *  Pull all SOCKET_A0-A15 high, wait 1 ms
     *  If all SOCKET_A0-A15 signals are not high, we are in a system
     *  Pull all SOCKET_A0-A15 low, wait 1 ms
     *  If all SOCKET_A0-A15 signals are not low, we are in a system
     */
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
     * number of elements in the pin_config[] table.
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
            gpio_setmode(curport, curpin, GPIO_SETMODE_INPUT_PULLUPDOWN);
#if 1
            if ((pass == 0) &&
                (((curport == FLASH_OE_PORT) && (curpin == FLASH_OE_PIN)) ||
                 ((curport == SOCKET_OE_PORT) && (curpin == SOCKET_OE_PIN))))
                continue;  // Don't set FLASH_OE or SOCKET_OE low
#endif
            gpio_setv(curport, curpin, !pass);
        }

        usb_poll();

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
            if ((pass == 1) &&
                (((cur >= ARRAY_SIZE(pin_config)) &&
                  (cur < ARRAY_SIZE(pin_config) + 32)) ||
                 ((curport == SOCKET_D31_PORT) &&
                  (curpin == SOCKET_D31_PIN)))) {
                /* Don't bother checking data pins when they are driven */
                continue;
            }

            if (state != !pass) {
                if (fail++ == 0)
                    printf("FAIL pin short tests\n");
                printf("  %-4s %s did not go %s (%u)\n",
                       gpio_to_str(curport, curpin),
                       curname, !pass ? "high" : "low", !pass);
            }
        }

        timer_delay_usec(1);
        for (cur = 0; cur < ARRAY_SIZE(pin_config) + 32 + 20; cur++) {
            curname = pin_config_get(cur, &curport, &curpin, buf0);
            if ((curport == FLASH_A18_PORT) && (curpin == FLASH_A18_PIN)) {
                /*
                 * I don't know why this is necessary. SOCKET_OE=1 briefly
                 * when FLASH_A18=1 otherwise.
                 */
                timer_delay_usec(1);
            }

            usb_poll();

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
                        (checkport == FLASH_RB_PORT) &&
                        (checkpin == FLASH_RB_PIN)) {
                        /*
                         * Okay to ignore
                         * FLASH_RP=0 causes FLASH_RB=0
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
                        /* FLASH_OE and SOCKET_OE connected by resistor */
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
                        /* FLASH_A18 and SOCKET_A18 connected by resistor */
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
                        /* FLASH_A19 and SOCKET_A19 connected by resistor */
                        continue;
                    }
                    if ((pass == 1) &&
                        (curport == FLASH_RP_PORT) &&
                        (curpin == FLASH_RP_PIN) &&
                        ((checkport == FLASH_D0_PORT) ||
                         (checkport == FLASH_D16_PORT))) {
                        /* FLASH_RP=1 drives data pins */
                        continue;
                    }
                    if ((pass == 0) &&
                        (curport == FLASH_WE_PORT) &&
                        (curpin == FLASH_WE_PIN) &&
                        (((checkport == FLASH_OE_PORT) &&
                          (checkpin == FLASH_OE_PIN)) ||
                         ((checkport == SOCKET_OE_PORT) &&
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

                    if (((checkport == FLASH_D0_PORT) ||
                         (checkport == FLASH_D16_PORT)) &&
                        (gpio_get(FLASH_OE_PORT, FLASH_OE_PIN) == 0)) {
                        /*
                         * FLASH_OE will cause flash to drive data pins.
                         */
                        continue;
                    }
                    if (((checkport == SOCKET_D31_PORT) &&
                          (checkpin == SOCKET_D31_PIN)) &&
                        (gpio_get(SOCKET_OE_PORT, SOCKET_OE_PIN) == 0)) {
                        /*
                         * SOCKET_OE will cause buffers to drive SOCKET_D31
                         */
                        continue;
                    }

                    if ((curport == FLASH_D31_PORT) &&
                        (curpin == FLASH_D31_PIN) &&
                        (checkport == SOCKET_D31_PORT) &&
                        (checkpin == SOCKET_D31_PIN)) {
                        /*
                         * FLASH_D31 can affect SOCKET_D31 if SOCKET_OE=0
                         */
                        continue;
                    }

                    if (fail++ == 0)
                        printf("FAIL pin short tests\n");
                    printf("  %-4s %s=%u caused ",
                           gpio_to_str(curport, curpin), curname, pass);
                    printf("%-4s %s=%u\n",
                           gpio_to_str(checkport, checkpin), checkname, state);
                    if (fail == 1)
                        gpio_show(-1, 0xffff);
                }
            }

            /* Restore pin back to pull-up / pull-down */
            gpio_setv(curport, curpin, !pass);
            gpio_setmode(curport, curpin, GPIO_SETMODE_INPUT_PULLUPDOWN);
        }
    }

    usb_poll();

    /* Restore all pins to input pull-up/pull-down and final state */
    for (cur = 0; cur < ARRAY_SIZE(pin_config) + 32 + 20; cur++) {
        uint final = FS_PU;  // default to pull-up
        uint mode = GPIO_SETMODE_INPUT_PULLUPDOWN;
        curname = pin_config_get(cur, &curport, &curpin, buf0);
        if (cur < ARRAY_SIZE(pin_config)) {
            final = pin_config[cur].final_state;
            switch (final) {
                case FS_IN:
                    final = 0;
                    mode = GPIO_SETMODE_INPUT;
                    break;
                case FS_0:
                    final = 0;
                    mode = GPIO_SETMODE_OUTPUT_PPULL_2;
                    break;
            }
        }
        gpio_setv(curport, curpin, final);
        gpio_setmode(curport, curpin, mode);
    }

    return (fail ? RC_FAILURE : RC_SUCCESS);
}
