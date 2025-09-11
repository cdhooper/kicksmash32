/*
 * Timer functions.
 *
 * This source file is part of the code base for a simple Amiga ROM
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
#include <stdint.h>
#include <time.h>
#include <stdbool.h>
#include "amiga_chipset.h"
#include "printf.h"
#include "timer.h"
#include "util.h"

uint eclk_ticks_per_sec;   // 715909 (NTSC) or 709379 (PAL),
uint vblank_hz;            // 60 (NTSC typical) or 50 (PAL typical)
uint vid_type;             // VID_NTSC or VID_PAL
volatile uint64_t timer_tick_base;  // timer last vblank interrupt
volatile uint16_t eclk_last_update; // ECLK value at last vblank interrupt

static uint64_t timer_tick_get_eclk(void);
uint64_t (*timer_tick_get)(void) = &timer_tick_get_eclk;

uint
eclk_ticks(void)
{
    uint8_t hi1;
    uint8_t hi2;
    uint8_t lo;

    hi1 = *CIAA_TBHI;
    lo  = *CIAA_TBLO;
    hi2 = *CIAA_TBHI;

    /*
     * The below operation will provide the same effect as:
     *     if (hi2 != hi1)
     *         lo = 0xff;  // rollover occurred
     */
    lo |= (hi2 - hi1);  // rollover of hi forces lo to 0xff value

    return (lo | (hi2 << 8));
}

/*
 * timer_tick_get() returns the current tick timer. It also updates the tick
 *                  base from the running counters, so timer functions should
 *                  be usable even in interrupt context.
 */
uint64_t
timer_tick_get_eclk(void)
{
    uint32_t sr = irq_disable();
    uint16_t cur = eclk_ticks();
    uint16_t diff = eclk_last_update - cur;
    timer_tick_base += diff;
    eclk_last_update = cur;
    irq_restore(sr);
    return (timer_tick_base);
}

uint64_t
timer_tick_get_dummy(void)
{
    (void) *CIAA_TBHI;
    return (timer_tick_base += 19);
}

/**
 * timer_tick_to_usec() converts a tick timer count to microseconds.
 *                      This function is useful for reporting time
 *                      difference measurements.
 *
 * @param [in]  value - The tick timer count.
 *
 * @return      The converted microseconds value.
 *
 * Example usage:
 *     uint64_t start, end;
 *     tart = timer_tick_get();
 *     measure_func();
 *     end = timer_tick_get();
 *     printf("diff=%lu us\n", (uint32_t)timer_tick_to_usec(end - start));
 */
uint64_t
timer_tick_to_usec(uint64_t value)
{
    if ((value >> 44) != 0) {
        /* Would overflow if multiplied by 1 million */
        return (value * 1000 / eclk_ticks_per_sec * 1000);
    }
    return (value * 1000000 / eclk_ticks_per_sec);
}

/**
 * timer_tick_has_elapsed() indicates whether the specified tick timer value
 *                          has already elapsed.
 *
 * @param [in]  value - The value to compare against the current tick timer.
 *
 * @return      true  - The specified value has elapsed.
 * @return      false - The specified value has not yet elapsed.
 *
 * Example usage: See timer_tick_plus_msec()
 */
uint
timer_tick_has_elapsed(uint64_t value)
{
    uint64_t now = timer_tick_get();
#undef TIMER_DEBUG
#ifdef TIMER_DEBUG
    if ((now < value) && ((value - now) > 1000000)) {
        /* Sleep greater than 1 sec */
        printf("Sleep > 1 sec: %llx %llx\n", now, value);
        return (true);
    }
    static uint64_t timer_last;
    static uint     not_advancing;
    if (timer_last == now) {
        if (not_advancing++ > 100) {
            printf("Timer not advancing: %llx\n", now);
            return (true);
        }
    } else {
        not_advancing = 0;
    }
#endif

    if (now >= value)
        return (true);

    __asm("nop");
    return (false);
}

/**
 * timer_tick_plus_msec() returns what the tick timer value will be when the
 *                        specified number of milliseconds have elapsed.
 *                        This function is useful for computing timeouts.
 *
 * @param [in]  msec - The number of milliseconds to add to the current
 *                     tick timer value.
 *
 * @return      The value of the tick timer when the specified number of
 *              milliseconds have elapsed.
 *
 * Example usage:
 *     uint64_t timeout = timer_tick_plus_msec(1000);  // Expire in 1 second
 *
 *     while (wait_for_condition() == false)
 *         if (timer_tick_has_elapsed(timeout)) {
 *             printf("Condition timeout\n");
 *             return (RC_TIMEOUT);
 *         }
 */
uint64_t
timer_tick_plus_msec(uint msec)
{
    uint ticks = ((uint64_t) msec) * eclk_ticks_per_sec / 1000;
    return (timer_tick_get() + ticks);
}

/**
 * timer_tick_plus_usec() returns what the tick timer value will be when the
 *                        specified number of microseconds have elapsed. This
 *                        function is useful for computing timeouts.
 *
 * @param [in]  usec - The number of microseconds to add to the current
 *                     tick timer value.
 *
 * @return      The value of the tick timer when the specified number of
 *              microseconds have elapsed.
 */
uint64_t
timer_tick_plus_usec(uint usec)
{
    uint ticks = ((uint64_t) usec) * eclk_ticks_per_sec / 1000000;
    return (timer_tick_get() + ticks);
}

void
timer_delay_ticks(uint32_t ticks)
{
    uint64_t end = timer_tick_get() + ticks;
    while (timer_tick_has_elapsed(end) == false) {
        /* Empty */
    }
}

/**
 * timer_delay_msec() delays the specified number of milliseconds.
 *
 * @param [in]  msec - The number of milliseconds to delay.
 *
 * @return      None.
 */
void
timer_delay_msec(uint msec)
{
    uint64_t end = timer_tick_plus_msec(msec);
    while (timer_tick_has_elapsed(end) == false) {
        /* Empty */
    }
}

/**
 * timer_delay_usec() delays the specified number of microseconds.
 *
 * @param [in]  usec - The number of microseconds to delay.
 *
 * @return      None.
 */
void
timer_delay_usec(uint usec)
{
    uint64_t end = timer_tick_plus_usec(usec);
    while (timer_tick_has_elapsed(end) == false) {
        /* Empty */
    }
}

time_t
time(time_t *ptr)
{
    (void) (ptr);
    return (timer_tick_get());
}

/*
 * Derived table
 * ECLK is either 715909 (NTSC) or 709379 (PAL), depending on the
 * video crystal oscillator installed on the motherboard.
 *
 * The expected ticks may be calculated as ECLK / Hz = ticks
 *
 *      VidFreq  Hz   Computed  Measured ECLK ticks
 * NTSC 715909   60   11932     11928 - 11937
 *  PAL 709379   50   14187     14209 - 14213
 * NTSC 715909   50   14318
 *  PAL 709379   60   11823
 */
static const struct {
    uint32_t eclk;
    uint8_t  tick_hz;
    uint8_t  vid_type;
    uint16_t eclk_per_tick;
} eclk_to_hz_table[] = {
    { 715909, 60, VID_NTSC, 11932 },  // NTSC 60 Hz
    { 709379, 50, VID_PAL,  14187 },  // PAL 50 Hz
    { 715909, 50, VID_NTSC, 14318 },  // NTSC 50 Hz
    { 709379, 60, VID_PAL,  11823 },  // PAL 60 Hz
};

void
timer_init(void)
{
    uint     cur;
    uint     timeout;
    uint8_t  hz_tick;
    uint8_t  hz_stick;
    uint8_t  eclk_tbhi;
    uint     eclk_total;
    uint16_t eclk_etick;
    uint16_t eclk_stick;

    timer_tick_get = timer_tick_get_dummy;

    /*
     * The Amiga 8520 CIA chips have E clock input connected to ECLK.
     * In the A3000, Gary drives ECLK as 7M / 10, which means that
     * ECLK is either 715909 (NTSC) or 709379 (PAL).
     *
     * In addition, the TICK input to CIA-B comes from either the power
     * supply 50 Hz / 60 Hz or from the video hardware. We can measure
     * the tick of these two clocks relative to each other and make a
     * guess as to whether the main clock is NTSC or PAL, and whether
     * the tick is 50 Hz or 60 Hz.
     *
     * We will set up CIA A Event Counter to count power supply ticks
     * and CIA A Timer B to count E clock
     *
     * Note regarding the 8520 doc is that TOD pin may be mentioned.
     * This is actually TICK (pin 19) in the Amiga schematics.
     */

    /* Stop timers */
    *CIAA_CRA  = 0x00;
    *CIAA_CRB  = 0x00;

    /* Start event counter */
    *CIAA_EMSB = 0;
    *CIAA_EMID = 0;
    *CIAA_ELSB = 0;  // This should start the event counter ticking

    /* Configure timer to be single shot and start it */
    *CIAA_CRB  = CIA_CRB_RUNMODE;
    *CIAA_TBLO = 0xff;
    *CIAA_TBHI = 0xff;
    *CIAA_CRB = CIA_CRB_START | CIA_CRB_RUNMODE | CIA_CRB_LOAD;

    /*
     * At this point, both timers should be ticking.
     *
     * Watch CIA A TBHI (ECLK). If it reaches 0 before CIAA_ELSB
     * (HZ tick) increments, then something is broken (no tick).
     */
    eclk_tbhi = *CIAA_TBHI;
    hz_tick   = *CIAA_ELSB;

    timeout = 10000;
    while (eclk_tbhi == *CIAA_TBHI)
        if (--timeout == 0) {
            if (eclk_tbhi == 0)
                printf("CIA-E ECLK done too early\n");
            else
                printf("CIA-A ECLK timeout\n");

fail_use_defaults:
            /* Use NTSC 60 Hz as defaults */
            *CIAA_CRB = CIA_CRB_START;
            vblank_hz = eclk_to_hz_table[0].tick_hz + 1;
            eclk_ticks_per_sec = eclk_to_hz_table[0].eclk;
            vid_type = eclk_to_hz_table[0].vid_type;
            return;
        }

    while (hz_tick == *CIAA_ELSB) {
        if (*CIAA_TBHI == 0) {
            printf("CIA-A Hz tick timeout\n");
            goto fail_use_defaults;
        }
    }

    /* Configure ECLK timer to be continuous */
    *CIAA_CRB = CIA_CRB_START;

    /*
     * Now that we are sure both timers are ticking:
     * 1. Sync to the edge of a Hz tick
     * 2. Capture ECLK
     * 3. Wait for the edge of a Hz tick
     * 4. Capture ECLK
     * 5. Discard overhead of capturing ECLK
     *
     * The number of ECLK ticks should allow an assumption to be
     * made as to the speed of both ECLK and Hz.
     */

    /* 1. Sync to the edge of a Hz tick */
    hz_tick = *CIAA_ELSB;
    while (hz_tick == (hz_stick = *CIAA_ELSB))
        ;

    /* 2. Capture ECLK */
    eclk_stick = eclk_ticks();

    /* 3. Wait for the edge of a Hz tick */
    while (hz_stick == (hz_tick = *CIAA_ELSB))
        ;

    /* 4. Capture ECLK */
    eclk_etick = eclk_ticks();
    eclk_total = eclk_stick - eclk_etick;  // countdown

    /* 5. Discard overhead of capturing ECLK */
    eclk_stick = eclk_ticks();
    eclk_etick = eclk_ticks();
    eclk_total -= (eclk_stick - eclk_etick);

    for (cur = 0; cur < ARRAY_SIZE(eclk_to_hz_table); cur++) {
        if ((eclk_total > eclk_to_hz_table[cur].eclk_per_tick - 15) &&
            (eclk_total < eclk_to_hz_table[cur].eclk_per_tick + 15)) {
            break;
        }
    }
    if (cur >= ARRAY_SIZE(eclk_to_hz_table))
        cur = 0;  // Default to NTSC 60 Hz

#undef DEBUG_TIMER_INIT
#ifdef DEBUG_TIMER_INIT
    printf("ECLK ticks=%u\n", eclk_total);
    printf("%u Hz %s ECLK=%u\n", eclk_to_hz_table[cur].tick_hz,
           eclk_to_hz_table[cur].vid_type ? "PAL" : "NTSC",
           eclk_to_hz_table[cur].eclk);
#endif
    vblank_hz = eclk_to_hz_table[cur].tick_hz;
    eclk_ticks_per_sec = eclk_to_hz_table[cur].eclk;
    vid_type = eclk_to_hz_table[cur].vid_type;

    /*
     * Dump CIAA registers
     *      loop 16 dbA bfe$a01 1
     * Start CIAA event counter
     *      cb 00bfea01 0; cb 00bfe901 0; cb 00bfe801 0
     * Read CIAA event counter
     *      dbA 00bfea01 1;dbA 00bfe901 1;dbA 00bfe801 1
     *
     * Dump CIAB registers
     *      loop 16 dbA bfd$a00 1
     * Start CIAB event counter
     *      cb 00bfda00 0; cb 00bfd900 0; cb 00bfd800 0
     * Read CIAB event counter
     *      dbA 00bfda00 1;dbA 00bfd900 1;dbA 00bfd800 1
     */

    timer_tick_get = timer_tick_get_eclk;
    eclk_last_update = eclk_ticks();
}
