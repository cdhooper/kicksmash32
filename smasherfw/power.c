/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2025.
 *
 * ---------------------------------------------------------------------
 *
 * Power management functions
 */

#include "printf.h"
#include "main.h"
#include "power.h"
#include "gpio.h"
#include "config.h"
#include "timer.h"
#include "utils.h"

#define POWER_CYCLE_OFF_PERIOD 1000 // msec to hold power off during cycle

uint8_t power_state;
uint8_t leave_ks_power_on;

static uint64_t power_timer;

#define PSON_SET_ON  1  // Turn power supply on
#define PSON_SET_OFF 0  // Turn power supply off

/*
 * power_set_on() sets the ENABLE_V5 pin to either turn on or turn off
 *                power to the Kicksmash board.
 */
static void
power_set_on(unsigned int enable)
{
    gpio_setv(ENABLE_V5_PORT, ENABLE_V5_PIN, !enable);
}

/*
 * power_poll
 * ----------
 * Manage power supply state machine
 */
void
power_poll(void)
{
    if (power_state == POWER_STATE_CYCLE) {
        /* Waiting for power off to initiate power on */
        if (timer_tick_has_elapsed(power_timer)) {
            printf("Powering on\n");
            power_set_on(PSON_SET_ON);
            power_state = POWER_STATE_ON;
        }
    }
}

/*
 * power_set
 * ---------
 * Set the desired state for the power supply
 */
void
power_set(uint state)
{
    if (power_state != state) {
        switch (state) {
            case POWER_STATE_CYCLE:
                printf("Powering off\n");
                power_set_on(PSON_SET_OFF);
                power_timer = timer_tick_plus_msec(POWER_CYCLE_OFF_PERIOD);
                break;
            case POWER_STATE_OFF:
                printf("Powering off\n");
                power_set_on(PSON_SET_OFF);
                break;
            case POWER_STATE_ON:
                printf("Powering on\n");
                power_set_on(PSON_SET_ON);
                break;
        }
        power_state = state;
    }
}

/*
 * power_show
 * ----------
 * Display current power status
 */
void
power_show(void)
{
    const char *state;
    switch (power_state) {
        case POWER_STATE_CYCLE:
            state = "Cycling Power";
            break;
        case POWER_STATE_ON:
            state = "On";
            break;
        case POWER_STATE_OFF:
            state = "Off";
            break;
        default:
            state = "Unknown";
            break;
    }
    printf("Power state     %s\n", state);
}

/*
 * power_init
 * ----------
 * Initialize power management functions
 */
void
power_init(void)
{
    gpio_setv(ENABLE_V5_PORT, ENABLE_V5_PIN, 1);  // Disable power
    gpio_setmode(ENABLE_V5_PORT, ENABLE_V5_PIN, GPIO_SETMODE_OUTPUT_ODRAIN_2);
    if (power_state == POWER_STATE_INIT)
        power_state = POWER_STATE_OFF;
}
