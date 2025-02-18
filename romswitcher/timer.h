/*
 * Timer functions.
 *
 * This header file is part of the code base for a simple Amiga ROM
 * replacement sufficient to allow programs using some parts of GadTools
 * to function.
 *
 * Copyright 2025 Chris Hooper. This program and source may be used
 * and distributed freely, for any purpose which benefits the Amiga
 * community. All redistributions must retain this Copyright notice.
 *
 * DISCLAIMER: THE SOFTWARE IS PROVIDED "AS-IS", WITHOUT ANY WARRANTY.
 * THE AUTHOR ASSUMES NO LIABILITY FOR ANY DAMAGE ARISING OUT OF THE USE
 * OR MISUSE OF THIS UTILITY OR INFORMATION REPORTED BY THIS UTILITY.
 */
#ifndef _TIMER_H
#define _TIMER_H

uint64_t timer_tick_get(void);
void     timer_delay_msec(uint msec);
void     timer_delay_usec(uint usec);
void     timer_delay_ticks(uint32_t ticks);
uint     timer_tick_has_elapsed(uint64_t value);
uint64_t timer_tick_plus_msec(uint msec);
uint64_t timer_tick_plus_usec(uint usec);
uint64_t timer_tick_to_usec(uint64_t value);
void     timer_init(void);
uint     eclk_ticks(void);      // 16-bit ECLK tick value

#define VID_NTSC 0
#define VID_PAL  1

extern uint     eclk_ticks_per_sec; // 715909 (NTSC) or 709379 (PAL),
extern uint     vblank_hz;          // 60 (NTSC typical) or 50 (PAL typical)
extern uint     vid_type;           // VID_NTSC or VID_PAL
extern volatile uint64_t timer_tick_base;  // timer last vblank interrupt
extern volatile uint16_t eclk_last_update; // ECLK value at last vblank IRQ

#endif /* _TIMER_H */
