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

#include <stdint.h>
#include "board.h"
#include "main.h"
#include "gpio.h"
#include "printf.h"
#include "timer.h"
#include "kbrst.h"
#include "config.h"
#include "pin_tests.h"
#include "m29f160xt.h"

uint8_t         amiga_not_in_reset     = 0xff;
static uint8_t  amiga_powered_off      = 0;
static uint64_t amiga_reset_timer      = 0;  // Timer to take Amiga out of reset
static uint64_t amiga_long_reset_timer = 0;  // Timer to detect long reset
static uint64_t amiga_reboot_detect_timeout = 0;
static uint8_t  amiga_long_reset_done  = 0;  // Long reset already handled

/*
 * Some keyboard adapters (such as the Sum Amiga keyboard adapter) can't
 * hold the Amiga in reset long enough for a long reset. As an alternative,
 * three resets within RAPID_RESET_MSEC are treated as a long reset.
 * amiga_rapid_reset_expire[] holds the expiration ticks of the previous
 * resets, oldest first. A value of 0 is an unused entry.
 */
#define RAPID_RESET_COUNT 3
#define RAPID_RESET_MSEC  4000
static uint64_t amiga_rapid_reset_expire[RAPID_RESET_COUNT - 1];

/*
 * amiga_rapid_reset_check
 * -----------------------
 * Record that the Amiga has entered reset. Returns 1 if this is the
 * third reset within RAPID_RESET_MSEC.
 */
static uint
amiga_rapid_reset_check(void)
{
    uint pos;
    uint count = 0;

    if (config.flags & CF_NO_RAPID_RESET)
        return (0);

    /* Discard expired entries, keeping the rest in order */
    for (pos = 0; pos < (RAPID_RESET_COUNT - 1); pos++) {
        uint64_t expire = amiga_rapid_reset_expire[pos];
        if ((expire != 0) && !timer_tick_has_elapsed(expire))
            amiga_rapid_reset_expire[count++] = expire;
    }

    if (count == (RAPID_RESET_COUNT - 1)) {
        /* Enough recent resets: start counting again after this one */
        for (pos = 0; pos < (RAPID_RESET_COUNT - 1); pos++)
            amiga_rapid_reset_expire[pos] = 0;
        return (1);
    }
    amiga_rapid_reset_expire[count++] = timer_tick_plus_msec(RAPID_RESET_MSEC);
    for (; count < (RAPID_RESET_COUNT - 1); count++)
        amiga_rapid_reset_expire[count] = 0;
    return (0);
}

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

    gpio_setmode(SOCKET_D31_PORT, SOCKET_D31_PIN,
                 GPIO_SETMODE_INPUT_PULLUPDOWN);
    for (count = 0; count < 100; count++) {
        got = gpio_get(SOCKET_D31_PORT, SOCKET_D31_PIN);
        if (got)
            saw_1++;
        else
            saw_0++;
        if (saw_0 && saw_1) {
            printf("Unexpected: D31 is changing state\n");
            return (1);  // Amiga is running (not expected)
            saw_1 = 1;
            break;
        }
    }
    gpio_setmode(SOCKET_D31_PORT, SOCKET_D31_PIN, GPIO_SETMODE_INPUT);
    return (saw_1 ? 1 : 0);
}

void
kbrst_poll(void)
{
    uint8_t kbrst;

    if (!board_is_standalone && !kbrst_in_amiga) {
        /* Board is in Amiga and KBRST is not connected */
        return;  // Leave at default state of "not in reset"
    }
    if ((amiga_reset_timer != 0) && timer_tick_has_elapsed(amiga_reset_timer)) {
        amiga_reset_timer = 0;
        gpio_setmode(KBRST_PORT, KBRST_PIN, GPIO_SETMODE_INPUT_PULLUPDOWN);
    }

    kbrst = !!gpio_get(KBRST_PORT, KBRST_PIN);
    if (amiga_not_in_reset != kbrst) {
        if (amiga_not_in_reset == 0xff) {
            amiga_not_in_reset = kbrst;
            return;
        }
        amiga_not_in_reset = kbrst;
        /* Amiga reset state change has occurred */

        if (kbrst == 0) {
            /* In reset: update ROM bank if requested by user (at reset) */
            printf("Amiga in reset\n");
            ee_update_bank_at_reset();
            if (amiga_long_reset_timer == 0)
                amiga_long_reset_timer = timer_tick_plus_msec(2000);
            amiga_long_reset_done = 0;
            if (amiga_rapid_reset_check()) {
                /* Third reset in a short time: treat it as a long reset */
                printf("Amiga rapid reset\n");
                ee_update_bank_at_longreset();
                amiga_long_reset_done = 1;  // Don't advance again if held
            }
            board_state |= BOARD_STATE_IN_RESET;
        } else {
            /* Out of reet */
            if (amiga_powered_off) {
                amiga_powered_off = 0;
                printf("Amiga powered on\n");
            } else {
                printf("Amiga out of reset\n");
            }
            amiga_long_reset_timer = 0;
            board_state &= ~BOARD_STATE_IN_RESET;
        }
        amiga_reboot_detect_timeout = timer_tick_plus_msec(5000);
    } else {
        if ((amiga_long_reset_timer != 0) &&
            timer_tick_has_elapsed(amiga_long_reset_timer)) {
            amiga_long_reset_timer = 0;
            if (amiga_not_in_reset == 0) {
                if (amiga_is_powered_on()) {
                    /* Still in reset at timer expiration */
                    if (!amiga_long_reset_done)
                        ee_update_bank_at_longreset();
                } else {
                    printf("Amiga powered off\n");
                    amiga_powered_off++;
                    ee_update_bank_at_poweron();
                }
            }
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
        amiga_long_reset_timer = 0xffffffffffffffff;
    } else {
        if (longreset)
            amiga_reset_timer = timer_tick_plus_msec(2500);
        else
            amiga_reset_timer = timer_tick_plus_msec(400);
        amiga_long_reset_timer = 0;
    }
}
