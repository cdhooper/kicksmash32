/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2020.
 *
 * ---------------------------------------------------------------------
 *
 * STM32 LED control
 */

#include "board.h"
#include "main.h"
#include <stdint.h>
#include "led.h"
#include "gpio.h"
#include "timer.h"

#include <libopencm3/stm32/gpio.h>

static uint8_t  led_alert_state = 0;
static uint64_t led_power_timer;

void
led_poll(void)
{
    if (led_alert_state && (timer_tick_has_elapsed(led_power_timer))) {
        /* Blink Power LED */
        gpio_setv(LED_POWER_PORT, LED_POWER_PIN, !(led_alert_state & 2));
        led_alert_state ^= 2;
        led_power_timer = timer_tick_plus_msec(250);
    }
}

void
led_alert(int turn_on)
{
    led_alert_state = turn_on;
    led_poll();
//  gpio_setv(LED_ALERT_PORT, LED_ALERT_PIN, turn_on);
}

void
led_busy(int turn_on)
{
//  gpio_setv(LED_BUSY_PORT, LED_BUSY_PIN, turn_on);
}

void
led_power(int turn_on)
{
    gpio_setv(LED_POWER_PORT, LED_POWER_PIN, !turn_on);
}

void
led_init(void)
{
    gpio_setmode(LED_POWER_PORT, LED_POWER_PIN, GPIO_SETMODE_OUTPUT_PPULL_2);
}
