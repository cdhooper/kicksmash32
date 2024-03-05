/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2023.
 *
 * ---------------------------------------------------------------------
 *
 * Amiga KBRST handling
 */

#include "board.h"
#include "main.h"
#include "gpio.h"
#include "printf.h"
#include "timer.h"
#include "kbrst.h"

uint            amiga_reboot_detect    = 0;
uint8_t         amiga_not_in_reset     = 0xff;
static uint8_t  amiga_powered_off      = 0;
static uint64_t amiga_reset_timer      = 0;  // Timer to take Amiga out of reset
static uint64_t amiga_long_reset_timer = 0;  // Timer to detect long reset
static uint64_t amiga_reboot_detect_timeout = 0;

/*
 * amiga_is_powered_on
 * -------------------
 * This function determines if the Amiga is poweered on or off based on
 * whether the D31 pin is high or low. The STM32 will be configured with
 * a weak pull-up on this pin, but Amiga pull-ups are significantly stronger
 * and will always override the STM32. If the Amiga is off, D31 should always
 * have a 0 value. If the Amiga is on, D31 should always be high so long
 * as the Amiga is in reset.
 *
 * It is expected this function will be called while the Amiga is in reset.
 */
static uint
amiga_is_powered_on(void)
{
    uint count;
    uint got;
    uint saw_0 = 0;
    uint saw_1 = 0;

    for (count = 0; count < 100; count++) {
        got = gpio_get(SOCKET_D31_PORT, SOCKET_D31_PIN);
        if (got)
            saw_1++;
        else
            saw_0++;
        if (saw_0 && saw_1) {
            printf("Unexpected: D31 is changing state\n");
            return (1);  // Amiga is running (not expected)
        }
    }
    return (saw_1 ? 1 : 0);
}

void
kbrst_poll(void)
{
    uint8_t kbrst;

    if ((amiga_reset_timer != 0) && timer_tick_has_elapsed(amiga_reset_timer)) {
        amiga_reset_timer = 0;
        gpio_setmode(KBRST_PORT, KBRST_PIN, GPIO_SETMODE_INPUT_PULLUPDOWN);
    }
    if (amiga_reboot_detect) {
        amiga_reboot_detect = 0;
        if (timer_tick_has_elapsed(amiga_reboot_detect_timeout)) {
            amiga_reboot_detect_timeout = timer_tick_plus_msec(5000);
            printf("Amiga reboot\n");
        }
    }

    kbrst = !!gpio_get(KBRST_PORT, KBRST_PIN);
    if (amiga_not_in_reset == 0xff) {
        amiga_not_in_reset = kbrst;
    } else {
        if (amiga_not_in_reset != kbrst) {
            amiga_not_in_reset = kbrst;
            if (kbrst == 0) {
                /* Update ROM bank if requested by user (at reset) */
                void ee_update_bank_at_reset(void);
                printf("Amiga in reset\n");
                ee_update_bank_at_reset();
                if (amiga_long_reset_timer == 0)
                    amiga_long_reset_timer = timer_tick_plus_msec(2000);
            } else {
                if (amiga_powered_off) {
                    amiga_powered_off = 0;
                    printf("Amiga powered on\n");
                } else {
                    printf("Amiga out of reset\n");
                }
            }
            amiga_reboot_detect_timeout = timer_tick_plus_msec(5000);
        } else {
            if ((amiga_long_reset_timer != 0) &&
                timer_tick_has_elapsed(amiga_long_reset_timer)) {
                amiga_long_reset_timer = 0;
                if (amiga_not_in_reset == 0) {
                    if (amiga_is_powered_on()) {
                        /* Still in reset at timer expiration */
                        void ee_update_bank_at_longreset(void);
                        ee_update_bank_at_longreset();
                    } else {
                        void ee_update_bank_at_poweron(void);
                        printf("Amiga powered off\n");
                        amiga_powered_off++;
                        ee_update_bank_at_poweron();
                    }
                }
            }
        }
    }
}

void
kbrst_amiga(uint hold, uint longreset)
{
    gpio_setv(KBRST_PORT, KBRST_PIN, 0);
    gpio_setmode(KBRST_PORT, KBRST_PIN, GPIO_SETMODE_OUTPUT_PPULL_2);
    if (hold) {
        amiga_reset_timer = 0;
        amiga_long_reset_timer = 0xffffffffffffffff;
    } else {
        if (longreset)
            amiga_reset_timer = timer_tick_plus_msec(2500);
        else
            amiga_reset_timer = timer_tick_plus_msec(400);
    }
}
