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
#include "pin_tests.h"

uint8_t         amiga_not_in_reset     = 0xff;
static uint8_t  amiga_reset_by_smasher = 0;
// static uint8_t  amiga_powered_off      = 0;
static uint64_t amiga_reset_timer      = 0;  // Timer to take Amiga out of reset
// static uint64_t amiga_reboot_detect_timeout = 0;

void
kbrst_poll(void)
{
    uint8_t kbrst;

    if ((amiga_reset_timer != 0) && timer_tick_has_elapsed(amiga_reset_timer)) {
        amiga_reset_timer = 0;
        gpio_setv(KBRST_PORT, KBRST_PIN, 1);
        gpio_setmode(KBRST_PORT, KBRST_PIN, GPIO_SETMODE_INPUT_PULLUPDOWN);
    }

    kbrst = !!gpio_get(KBRST_PORT, KBRST_PIN);
    if (amiga_not_in_reset != kbrst) {
        if (amiga_not_in_reset == 0xff) {
            amiga_not_in_reset = kbrst;
            return;
        }
        amiga_not_in_reset = kbrst;
        if (amiga_reset_by_smasher)
            printf("Smasher ");
        else
            printf("Kicksmash ");
        if (kbrst == 0) {
            printf("put Amiga in reset\n");
        } else {
            printf("released Amiga from reset\n");
            amiga_reset_by_smasher = 0;
        }
    }
}

void
kbrst_amiga(uint hold, uint longreset)
{
    gpio_setv(KBRST_PORT, KBRST_PIN, 0);
    gpio_setmode(KBRST_PORT, KBRST_PIN, GPIO_SETMODE_OUTPUT_PPULL_50);
    if (hold) {
        amiga_reset_timer = 0;
    } else {
        if (longreset)
            amiga_reset_timer = timer_tick_plus_msec(2500);
        else
            amiga_reset_timer = timer_tick_plus_msec(400);
    }
    amiga_reset_by_smasher = 1;
}
