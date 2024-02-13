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

uint8_t  amiga_not_in_reset     = 0xff;
uint64_t amiga_reset_timer      = 0;  // Timer to take Amiga out of reset
uint64_t amiga_long_reset_timer = 0;  // Timer to detect long reset in effect

void
kbrst_poll(void)
{
    uint8_t kbrst;

    if ((amiga_reset_timer != 0) && timer_tick_has_elapsed(amiga_reset_timer)) {
        amiga_reset_timer = 0;
        gpio_setmode(KBRST_PORT, KBRST_PIN, GPIO_SETMODE_INPUT_PULLUPDOWN);
    }

    kbrst = !!gpio_get(KBRST_PORT, KBRST_PIN);
    if (amiga_not_in_reset == 0xff) {
        amiga_not_in_reset = kbrst;
    } else {
        if (amiga_not_in_reset != kbrst) {
            amiga_not_in_reset = kbrst;
            printf("Amiga KBRST=%u\n", kbrst);
            if (kbrst == 0) {
                /* Update ROM bank if requested by user (at reset) */
                void ee_update_bank_at_reset(void);
                ee_update_bank_at_reset();
                if (amiga_long_reset_timer == 0)
                    amiga_long_reset_timer = timer_tick_plus_msec(2000);
            }
        } else {
            if ((amiga_long_reset_timer != 0) &&
                timer_tick_has_elapsed(amiga_long_reset_timer)) {
                if (amiga_not_in_reset == 0) {
                    /* Still in reset at timer expiration */
                    void ee_update_bank_at_longreset(void);
                    ee_update_bank_at_longreset();
                }
                amiga_long_reset_timer = 0;
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
