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

uint8_t amiga_not_in_reset = 0xff;

void
kbrst_poll(void)
{
    uint8_t kbrst = !!gpio_get(KBRST_PORT, KBRST_PIN);
    if (amiga_not_in_reset == 0xff) {
        amiga_not_in_reset = kbrst;
    } else {
        if (amiga_not_in_reset != kbrst) {
            amiga_not_in_reset = kbrst;
            printf("Amiga KBRST=%u\n", kbrst);
        }
    }
}

void
kbrst_amiga(uint hold)
{
    gpio_setv(KBRST_PORT, KBRST_PIN, 0);
    gpio_setmode(KBRST_PORT, KBRST_PIN, GPIO_SETMODE_OUTPUT_PPULL_2);
    if (!hold) {
        timer_delay_msec(500);
        gpio_setmode(KBRST_PORT, KBRST_PIN, GPIO_SETMODE_INPUT_PULLUPDOWN);
    }
}
