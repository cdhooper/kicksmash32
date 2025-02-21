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
#include "clock.h"

#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/timer.h>
#include <libopencm3/stm32/rcc.h>

uint8_t         led_alert_state = 0;
static uint8_t  led_brightness = 0;  // Percent value (0 to 100)
static uint64_t led_power_timer;

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

static void
led_brightness_set_hw(unsigned int value)
{
    /*
     * The LED brightness scales non-linearly with power, so there needs
     * to be a conversion that applies an exponential to the input.
     * The PWM has been set up to use a scale from 0 - 1000. This
     * algorithm will convert a value from 0 - 100 to this scale.
     *
     * The below seem reasonable:
     *     0:0     1:0     2:0     3:0      4:1
     *     5:2     6:3     7:4     8:6      9:8
     *    10:10   20:40   30:90   40:160   50:250
     *    60:360  70:490  80:640  90:810  100:1000
     */
    value = value * value / 10;
    if (value > 1000)
        value = 1000;
    timer_set_oc_value(TIM4, TIM_OC3, value);
}

void
led_power(int turn_on)
{
    if (turn_on)
        led_brightness_set_hw(led_brightness);
    else
        led_brightness_set_hw(0);
//  gpio_setv(LED_POWER_PORT, LED_POWER_PIN, !turn_on);
}

/*
 * Set Power LED brightness level (0-100)
 */
void
led_set_brightness(unsigned int value)
{
    led_brightness = value;
    led_brightness_set_hw(value);
}

/*
 * Manage LED state such as error blink blinking
 */
void
led_poll(void)
{
    if (led_alert_state && (timer_tick_has_elapsed(led_power_timer))) {
        /* Blink Power LED */
        led_alert_state ^= 2;
        if (led_alert_state & 2)
            led_brightness_set_hw(0);
        else
            led_brightness_set_hw(led_brightness);
        led_power_timer = timer_tick_plus_msec(250);
    }
}

void
led_init(void)
{
    /*
     * Experimental code for PWM-driven LED
     */
    uint freq;
    uint period;

    rcc_periph_clock_enable(RCC_TIM4);
    rcc_periph_reset_pulse(RST_TIM4);
//  timer_disable_counter(TIM4);  // Not needed

    freq = rcc_pclk2_frequency;
    period = 1000;
    timer_set_mode(TIM4, TIM_CR1_CKD_CK_INT, TIM_CR1_CMS_EDGE, TIM_CR1_DIR_UP);
    timer_set_prescaler(TIM4, freq * 2 / 1000000 - 1);
    timer_set_period(TIM4, period);
    timer_set_repetition_counter(TIM4, 0);
    timer_enable_preload(TIM4);
    timer_continuous_mode(TIM4);

//  timer_disable_oc_output(TIM4, TIM_OC3);  // Not needed
    timer_set_oc_polarity_low(TIM4, TIM_OC3);
    timer_set_oc_mode(TIM4, TIM_OC3, TIM_OCM_PWM1);  // Alt TIM_OCM_PWM2
    timer_set_oc_value(TIM4, TIM_OC3, 10);
    timer_enable_oc_output(TIM4, TIM_OC3);
    timer_generate_event(TIM4, TIM_EGR_UG);  // Load from shadow registers

    timer_enable_counter(TIM4);

    /*
     *
     * Addresses and quick commands:
     *   PSC   cl 40000828 4000
     *   ARR   cl 4000082c 2000
     *   T-DIS cl 40000800 80
     *   T-EN  cl 40000800 81
     *
     *   CCR3  cl 4000083c 1000
     *   C-DIS cl 40000820 0
     *   C-EN  cl 40000820 100
     *
     * Show tick
     *   loop 10 dr 40000824 1
     *
     *   gpio pb8=ao2
     *                            PSC              ARR
     * cl 40000800 0;cl 40000828 1000;cl 4000082c 400;cl 40000800 1
     * cl 40000820 0; cl 4000083c 400; cl 40000820 100
     * cl 40000820 0; cl 4000083c 20; cl 40000820 100
     */

    gpio_set_mode(LED_POWER_PORT, GPIO_MODE_OUTPUT_2_MHZ,
                  GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, LED_POWER_PIN);

    led_set_brightness(100);
}
