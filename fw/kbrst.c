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
#include "kbrst.h"

uint8_t kbrst_last = 0xff;

void
kbrst_poll(void)
{
    uint8_t kbrst = !!gpio_get(KBRST_PORT, KBRST_PIN);
    if (kbrst_last == 0xff) {
        kbrst_last = kbrst;
    } else {
        if (kbrst_last != kbrst) {
            kbrst_last = kbrst;
            printf("Amiga KBRST=%u\n", kbrst);
        }
    }
}
