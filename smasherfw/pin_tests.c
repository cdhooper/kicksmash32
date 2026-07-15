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
#include "ee_kicksmash.h"
#include "timer.h"
#include "utils.h"
#include "config.h"
#include "usb.h"
#include "cmdline.h"
#include "led.h"
#include "power.h"
#include "smash.h"

#include <libopencm3/stm32/timer.h>

uint8_t  board_is_standalone = 0;


#define PIN_EXT_PULLDOWN 0  // Pin is externally pulled down
#define PIN_EXT_PULLUP   1  // Pin is externally pulled up
#define PIN_INPUT        2  // Pin should float (nothing else drives it)

#define FS_PD            0  // Final state: Pull down
#define FS_PU            1  // Final state: Pull up
#define FS_IN            2  // Final state: Input
#define FS_0             3  // Final state: Drive to 0
#define FS_1             4  // Final state: Drive to 1

static void
print_addr_pins(uint32_t value)
{
    uint pin;
    for (pin = 0; pin < 32; pin++)
        if ((value & BIT(pin)) != 0)
            printf(" A%u", pin);
    printf("\n");
}

static void
print_data_pins(uint32_t value)
{
    uint pin;
    for (pin = 0; pin < 32; pin++)
        if ((value & BIT(pin)) != 0)
            printf(" D%u", pin);
    printf("\n");
}

typedef struct {
    char     name[12];
    uint32_t port;
    uint16_t pin;
    uint8_t  final_state;
    uint8_t  type;
} pin_config_t;

static const pin_config_t pin_config[] =
{
    { "LED_STATUS",  GPIOB,           GPIO9,          FS_1,  PIN_INPUT },
    { "DFU_BUTTON",  GPIOB,           GPIO5,          FS_PD, PIN_EXT_PULLDOWN },
    { "USR_BUTTON",  GPIOA,           GPIO0,          FS_PD, PIN_INPUT },
    { "SOCKET_CE",   GPIOB,           GPIO14,         FS_PU, PIN_INPUT },
    { "SOCKET_BYTE", GPIOB,           GPIO15,         FS_PU, PIN_INPUT },
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
        *port = SOCKET_D0_PORT;
        *pin  = BIT(pos);
        sprintf(buf, "D%u", pos);
        return (buf);
    }
    pos -= 16;
    if (pos < 16) {
        /* D16-D31 */
        *port = SOCKET_D16_PORT;
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

static uint32_t
data_input(void)
{
    return (GPIO_IDR(SOCKET_D0_PORT) | (GPIO_IDR(SOCKET_D16_PORT) << 16));
}

static uint32_t
address_input(void)
{
    uint32_t addr = GPIO_IDR(SOCKET_A0_PORT);
    addr |= ((GPIO_IDR(SOCKET_A16_PORT) & 0x00f0) << (16 - 4));
    return (addr);
}

static uint
pin_standalone_tests(uint verbose, uint force)
{
    return (0);
}

static uint
pin_test_ks_oe(void)
{
    uint     errs = 0;
    uint32_t datadiff;
    uint32_t value;

    /* Set Kicksmash to not drive data pins */
    gpio_setv(SOCKET_OE_PORT, SOCKET_OE_PIN, 1);
    gpio_setv(SOCKET_CE_PORT, SOCKET_CE_PIN, 1);

    data_output(0xffffffff);
    timer_delay_msec(1);

    power_set(POWER_STATE_ON);  // Power on Kicksmash
    timer_delay_msec(100);

    /* With OE and CE high, no data lines should be driven */
    value = ~data_input();
    if (value != 0x00000000) {
        printf("Data lines stuck low (%08x):", (uint) value);
        print_data_pins(value);
        errs++;
    }
    data_output(0x00000000);
    timer_delay_msec(1);
    value = data_input();
    if (value == BIT(15) || (value == BIT(31))) {
        /*
         * D15 and D31 are special in that Kicksmash might in a state where
         * it's pulling one or the other high
         *
         * Try again, driving instead of pulling these pins high
         */
        gpio_setmode(SOCKET_D0_PORT, 0x8000, GPIO_SETMODE_OUTPUT_PPULL_2);
        gpio_setmode(SOCKET_D16_PORT, 0x8000, GPIO_SETMODE_OUTPUT_PPULL_2);
        timer_delay_msec(1);
        value = data_input();
        gpio_setmode(SOCKET_D0_PORT, 0x8000, GPIO_SETMODE_INPUT_PULLUPDOWN);
        gpio_setmode(SOCKET_D16_PORT, 0x8000, GPIO_SETMODE_INPUT_PULLUPDOWN);
    }
    if (value != 0x00000000) {
        printf("Data lines stuck high (%08x):", (uint) value);
        print_data_pins(value);
        errs++;
    }
    data_output(0xffffffff);
#define ADDR_BITS     (BIT(20) - 1)
#define ADDR_BITS_MIN (BIT(17) - 1)
    address_output(0x000fffff);
    timer_delay_msec(1);

    value = ~address_input() & ADDR_BITS_MIN;  // Ignore A17 and higher for now
    if (value != 0x00000000) {
        printf("Address lines stuck low (%05x):", (uint) value);
        print_addr_pins(value);
        errs++;
    }

    address_output(0x00000000);
    timer_delay_msec(1);
    value = address_input() & ADDR_BITS_MIN;  // Ignore A17 and higher for now
    if (value != 0x00000000) {
        printf("Address lines stuck high (%05x):", (uint) value);
        print_addr_pins(value);
        errs++;
    }
    address_output(0x000fffff);
    gpio_setv(SOCKET_OE_PORT, SOCKET_OE_PIN, 0);
    gpio_setv(SOCKET_CE_PORT, SOCKET_CE_PIN, 0);
    gpio_setmode(SOCKET_OE_PORT, SOCKET_OE_PIN, GPIO_SETMODE_OUTPUT_PPULL_2);
    gpio_setmode(SOCKET_CE_PORT, SOCKET_CE_PIN, GPIO_SETMODE_OUTPUT_PPULL_2);
    timer_delay_msec(1);

    /* Verify that Kicksmash-driven data lines can't be changed */
    datadiff = 0;
    data_output(0x00000000);
    timer_delay_msec(10);
    datadiff |= data_input();
    data_output(0xffffffff);
    timer_delay_msec(10);
    datadiff |= ~data_input();
    if (datadiff != 0xffffffff) {
        printf("Floating data pins (%08x):", (uint) ~datadiff);
        print_data_pins(~datadiff);
        errs++;
    }

    /* Release Kicksmash output enable */
    gpio_setv(SOCKET_OE_PORT, SOCKET_OE_PIN, 1);
    gpio_setv(SOCKET_CE_PORT, SOCKET_CE_PIN, 1);
    gpio_setmode(SOCKET_OE_PORT, SOCKET_OE_PIN, GPIO_SETMODE_INPUT_PULLUPDOWN);
    gpio_setmode(SOCKET_CE_PORT, SOCKET_CE_PIN, GPIO_SETMODE_INPUT_PULLUPDOWN);
    return (errs);
}


/*
 * pin_test_ks_data_addr_shorts
 * ----------------------------
 * Test Kicksmash pins with SOCKET_OE disabled to verify no pins affect
 * any others.
 */
static uint
pin_test_ks_data_addr_shorts(void)
{
    uint     errs = 0;
    uint     cur;
    uint     pass;
    char     buf0[8];
    char     buf1[8];
    uint32_t curport;
    uint16_t curpin;
    const char *curname;

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
            if ((pass == 0) &&
                (curport == SOCKET_OE_PORT) && (curpin == SOCKET_OE_PIN))
                continue;  // Don't set SOCKET_OE low
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
                    if (errs++ == 0)
                        printf("FAIL pin short tests\n");
                    printf("%-4s %s has external pull-%s but state is %u\n",
                           gpio_to_str(curport, curpin), curname,
                           (type == PIN_EXT_PULLUP) ? "up" : "down",
                           state);
                    continue;
                }
            }
            if ((curport == SOCKET_A16_PORT) &&
                ((curpin == SOCKET_A17_PIN) ||
                 (curpin == SOCKET_A18_PIN) ||
                 (curpin == SOCKET_A19_PIN))) {
                /*
                 * Don't bother checking pins which might be driven
                 * by Kicksmash in normal operation (bank select).
                 */
                continue;
            }
            if (state != !pass) {
                if (errs++ == 0) {
                    gpio_show(-1, 0xffff);
                    printf("FAIL pin short tests\n");
                }
                printf("  %-4s %s could not pull %s (%u)\n",
                       gpio_to_str(curport, curpin),
                       curname, !pass ? "high" : "low", !pass);
            }
        }

        timer_delay_usec(1);
        for (cur = 0; cur < ARRAY_SIZE(pin_config) + 32 + 20; cur++) {
            curname = pin_config_get(cur, &curport, &curpin, buf0);
            usb_poll();

            /* Set one pin the opposite of the others */
            gpio_setv(curport, curpin, pass);
            gpio_setmode(curport, curpin, GPIO_SETMODE_OUTPUT_PPULL_2);

            timer_delay_msec(1);

            /* Verify that pin made it to the desired state */
            if (!!gpio_get(curport, curpin) != pass) {
                if (errs++ == 0) {
                    gpio_show(-1, 0xffff);
                    printf("FAIL pin short tests\n");
                }
                printf("  %-4s %s could not drive %s (%u)\n",
                       gpio_to_str(curport, curpin),
                       curname, pass ? "high" : "low", pass);
            }

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
                    if (((checkport == SOCKET_D0_PORT) ||
                         (checkport == SOCKET_D16_PORT)) &&
                        (gpio_get(SOCKET_OE_PORT, SOCKET_OE_PIN) == 0)) {
                        /*
                         * SOCKET_OE will cause Kicksmash to drive data pins.
                         */
                        continue;
                    }
                    if ((state == 1) &&
                        (((checkport == SOCKET_D15_PORT) &&
                          (checkpin == SOCKET_D15_PIN)) ||
                         ((checkport == SOCKET_D31_PORT) &&
                          (checkpin == SOCKET_D31_PIN)))) {
                        /*
                         * D31 (Kicksmash32) or D15 (Kicksmash1200) may
                         * be unexpectedly high because Kicksmash firmware
                         * may pull this pin high
                         */
                        continue;
                    }

                    if ((checkport == SOCKET_A16_PORT) &&
                        ((checkpin == SOCKET_A17_PIN) ||
                         (checkpin == SOCKET_A18_PIN) ||
                         (checkpin == SOCKET_A19_PIN))) {
                        /*
                         * Don't bother checking pins which might be driven
                         * by Kicksmash in normal operation (bank select).
                         */
                        continue;
                    }
                    if (config.flags & CF_OE_GATE) {
                        /*
                         * SOCKET_OE follow OR gate inputs,
                         * so don't bother checking these.
                         */
                         if ((checkport == SOCKET_OE_PORT) &&
                             (checkpin == SOCKET_OE_PIN)) {
                            continue;
                        }
                    }

                    if (errs++ == 0)
                        printf("FAIL pin short tests\n");
                    printf("  %-4s %s=%u caused ",
                           gpio_to_str(curport, curpin), curname, pass);
                    printf("%-4s %s=%u\n",
                           gpio_to_str(checkport, checkpin), checkname, state);
                    if (errs == 1)
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
        uint final = FS_PD;  // default to pull-down
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

    return (errs);
}

static uint
message_test_kicksmash(void)
{
    uint pass;
    for (pass = 0; pass < 5; pass++) {
        usb_poll();
        ee_enable();
        if (smash_test_only())
            return (1);
    }
    return (0);
}


/*
 * pin_tests
 * ---------
 * Performs Kicksmash external pin tests.
 */
uint
pin_tests(uint verbose, uint force)
{
    uint errs = 0;

    led_status(0);
    led_alert(0);

    if (power_state == POWER_STATE_ON) {
        power_set(POWER_STATE_OFF);  // Power off Kicksmash
        timer_delay_msec(400);
    }
    power_set(POWER_STATE_ON);   // Power on Kicksmash

    /* Pull down OE and pull up all data pins */
    gpio_setv(KBRST_PORT, KBRST_PIN, 1);
    gpio_setv(SOCKET_OE_PORT, SOCKET_OE_PIN, 0);
    gpio_setv(SOCKET_CE_PORT, SOCKET_CE_PIN, 0);
    gpio_setv(SOCKET_BYTE_PORT, SOCKET_BYTE_PIN, 1);
    data_output(0xffffffff);
    address_output(0x000fffff);
    gpio_setmode(SOCKET_A0_PORT, 0xffff, GPIO_SETMODE_INPUT_PULLUPDOWN);
    gpio_setmode(SOCKET_A16_PORT, SOCKET_A16_PIN * 0xf,
                 GPIO_SETMODE_INPUT_PULLUPDOWN);
    gpio_setmode(SOCKET_D0_PORT, 0xffff, GPIO_SETMODE_INPUT_PULLUPDOWN);
    gpio_setmode(SOCKET_D16_PORT, 0xffff, GPIO_SETMODE_INPUT_PULLUPDOWN);
    gpio_setmode(KBRST_PORT, KBRST_PIN, GPIO_SETMODE_INPUT_PULLUPDOWN);
    gpio_setmode(SOCKET_OE_PORT, SOCKET_OE_PIN, GPIO_SETMODE_INPUT_PULLUPDOWN);
    gpio_setmode(SOCKET_CE_PORT, SOCKET_CE_PIN, GPIO_SETMODE_INPUT_PULLUPDOWN);
    gpio_setmode(SOCKET_BYTE_PORT, SOCKET_BYTE_PIN,
                 GPIO_SETMODE_INPUT_PULLUPDOWN);
    timer_delay_msec(20);

    /* If the data lines are not high, then a device is connected */
    board_is_standalone = 1;  // Assume standalone
    if (__builtin_popcount(data_input()) < 27) {
        /* More than 5 data pins are low */
        board_is_standalone = 0;
        printf("Kicksmash detected (data pins low)\n");
    } else if (__builtin_popcount(address_input()) < 15) {
        /* More than 4 address pins are low */
        board_is_standalone = 0;
        printf("Kicksmash detected (address pins low)\n");
    }

    gpio_setv(SOCKET_OE_PORT, SOCKET_OE_PIN, 1);
    gpio_setv(SOCKET_CE_PORT, SOCKET_CE_PIN, 1);
    timer_delay_msec(1);
    if (!gpio_get(SOCKET_CE_PORT, SOCKET_CE_PIN) +
        !gpio_get(SOCKET_OE_PORT, SOCKET_OE_PIN) +
        !gpio_get(SOCKET_BYTE_PORT, SOCKET_BYTE_PIN) > 1) {
        /* More than 1 control pin is low */
        board_is_standalone = 0;
        printf("Kicksmash detected (OE CE BYTE)\n");
    }

    if (board_is_standalone) {
        /* Further tests */
        data_output(0x00000000);
        gpio_setmode(SOCKET_OE_PORT, SOCKET_OE_PIN,
                     GPIO_SETMODE_OUTPUT_PPULL_2);
        gpio_setmode(SOCKET_CE_PORT, SOCKET_CE_PIN,
                     GPIO_SETMODE_OUTPUT_PPULL_2);
        gpio_setv(SOCKET_OE_PORT, SOCKET_OE_PIN, 0);
        gpio_setv(SOCKET_CE_PORT, SOCKET_CE_PIN, 0);
        timer_delay_msec(10);
        if (__builtin_popcount(data_input()) > 4) {
            /* More than 4 data pins are high */
            board_is_standalone = 0;
            printf("Kicksmash detected (data pins high)\n");
        }
        gpio_setv(SOCKET_OE_PORT, SOCKET_OE_PIN, 1);
        gpio_setv(SOCKET_CE_PORT, SOCKET_CE_PIN, 1);
        gpio_setmode(SOCKET_OE_PORT, SOCKET_OE_PIN,
                     GPIO_SETMODE_INPUT_PULLUPDOWN);
        gpio_setmode(SOCKET_CE_PORT, SOCKET_CE_PIN,
                     GPIO_SETMODE_INPUT_PULLUPDOWN);
    }

    if (board_is_standalone) {
        /* Board is standalone */
        printf("No Kicksmash detected\n");
        errs = pin_standalone_tests(verbose, force);
        goto finish;
    }

    /* Board is not standalone */
    errs += pin_test_ks_oe();

    /* Generic tests are done. Check individual pins for short */
    errs += pin_test_ks_data_addr_shorts();

    /* Do high level protocol tests */
    if (errs == 0)
        errs += message_test_kicksmash();

    if (errs == 0)
        printf("No errors detected during Kicksmash pin tests\n");

finish:
    if (errs == 0)
        led_status(1);
    else
        led_alert(1);

    power_set(config.leave_on ? POWER_STATE_ON : POWER_STATE_OFF);

    return (errs);
}
