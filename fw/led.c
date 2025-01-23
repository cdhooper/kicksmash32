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
#include <libopencm3/stm32/timer.h>


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
#if 1
    gpio_setmode(LED_POWER_PORT, LED_POWER_PIN, GPIO_SETMODE_OUTPUT_PPULL_2);
#else
    /*
     * Experimental code for PWM-driven LED
     *
     * Too slow because TIM4 is bits 16-31 of timer tick. Might be
     * made to work by finding a different timer to replace TIM4 in the
     * timer tick.
     */
    gpio_setmode(LED_POWER_PORT, LED_POWER_PIN, GPIO_SETMODE_OUTPUT_AF_PPULL_2);
//  gpio_set_af(LED_POWER_PORT, GPIO_AF2, LED_POWER_PIN);

    timer_disable_oc_output(TIM4, TIM_OC3);
    timer_set_oc_mode(TIM4, TIM_OC3, TIM_OCM_PWM1);  // Alt TIM_OCM_PWM2
    timer_set_oc_value(TIM4, TIM_OC3, 16384);
    timer_enable_oc_output(TIM4, TIM_OC3);
//  timer_generate_event(TIM4, TIM_EGR_UG);

    timer_disable_counter(TIM4);  // Hopefully not needed
    timer_enable_counter(TIM4);   // Hopefully not needed

    timer_set_oc_value(TIM4, TIM_OC3, 20);
#endif
}
