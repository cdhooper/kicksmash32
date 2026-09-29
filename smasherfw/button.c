/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2026.
 *
 * ---------------------------------------------------------------------
 *
 * Abort button handling.
 */

#include <stdint.h>
#include "board.h"
#include "button.h"
#include "main.h"
#include "gpio.h"
#include "printf.h"
#include "config.h"
#include "power.h"
#include "pin_tests.h"
#include "timer.h"
#include "led.h"

/**
 * button_poll() polls the buttons for state changes.
 *
 * @param [in]  None.
 * @return      None.
 */
void
button_poll(void)
{
    static bool pressed_dfu  = false;

    if (gpio_get(BOOT0_PORT, BOOT0_PIN)) {
        if (pressed_dfu == false) {
            pressed_dfu = true;
            printf("Pressed DFU\n");
            config.leave_on = !config.leave_on;
            power_set(config.leave_on ? POWER_STATE_ON : POWER_STATE_OFF);
        }
    } else {
        if (pressed_dfu) {
            printf("Released DFU\n");
        }
        pressed_dfu = false;
    }

    if (gpio_get(USER_BUTTON_PORT, USER_BUTTON_PIN)) {
        uint toggle = 0;
        printf("Pressed user: ");
        if (smash_test_running) {
            if (smash_test_passes != 0xffffffff) {
                smash_test_passes = 0xffffffff;
                printf("Running test indefinitely\n");
            } else {
                smash_test_passes = 0;  // Stop test
                printf("Stopping test\n");
            }
        } else {
            /* Initiate a Kicksmash test */
            printf("Running test\n");
        }

        /* Wait for release */
        while (gpio_get(USER_BUTTON_PORT, USER_BUTTON_PIN)) {
            led_status(toggle);
            toggle ^= 1;
            if (toggle)
                timer_delay_msec(100);
            else
                timer_delay_msec(30);
        }

        led_status(0);
        if (smash_test_running == 0) {
            /* Initiate a Kicksmash test */
            pin_tests(1, 1);
        }
    }
}

void
button_init(void)
{
    gpio_setv(USER_BUTTON_PORT, USER_BUTTON_PIN, 0);
    gpio_setmode(USER_BUTTON_PORT, USER_BUTTON_PIN,
                 GPIO_SETMODE_INPUT_PULLUPDOWN);

    gpio_setv(BOOT0_PORT, BOOT0_PIN, 0);
    gpio_setmode(BOOT0_PORT, BOOT0_PIN, GPIO_SETMODE_INPUT_PULLUPDOWN);
}
