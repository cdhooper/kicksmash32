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

#ifndef _POWER_H
#define _POWER_H

void power_init(void);
void power_poll(void);
void power_set(uint state);  // POWER_STATE_ ON, OFF, or CYCLE
void power_show(void);

#define POWER_STATE_INIT         0  // Needs initialization

/* Transitional state (will end in another state) */
#define POWER_STATE_CYCLE        1  // Cycling power

/* Resting power states */
#define POWER_STATE_ON           2  // Power supply is on
#define POWER_STATE_OFF          3  // Power supply is off

extern uint8_t power_state;

#endif /* _POWER_H */
