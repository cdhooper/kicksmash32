/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2020.
 *
 * ---------------------------------------------------------------------
 *
 * LED handling.
 */

#ifndef _LED_H
#define _LED_H

void led_init(void);
void led_power(int turn_on);
void led_busy(int turn_on);
void led_alert(int turn_on);

#endif /* _LED_H */
