/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2020.
 *
 * ---------------------------------------------------------------------
 *
 * STM32 timer and timing handling.
 */

#ifndef _TIMER_H
#define _TIMER_H

void     timer_init(void);
void     timer_shutdown(void);
uint64_t timer_tick_get(void);
void     timer_delay_msec(uint msec);
void     timer_delay_usec(uint usec);
void     timer_delay_ticks(uint32_t ticks);
bool     timer_tick_has_elapsed(uint64_t value);
uint64_t timer_tick_plus_msec(uint msec);
uint64_t timer_tick_plus_usec(uint usec);
uint64_t timer_tick_to_usec(uint64_t value);
uint64_t timer_usec_to_tick(uint usec);
uint32_t timer_nsec_to_tick(uint nsec);

#endif /* _TIMER_H */
