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

#include "printf.h"
#include "board.h"
#include "main.h"
#include <stdbool.h>
#include "timer.h"
#include "clock.h"

#include <libopencm3/stm32/timer.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/cm3/nvic.h>

/*
 * STM32F1 timer usage
 *   TIM4 - bits 16-31 of tick timer (bits 32-63 are in global timer_high)
 *   TIM1 - bits 0-15 of tick timer, OVF trigger to TIM4
 *   TIM5 CH1 - ROM OE (PA0) trigger to DMA2 CH5 capture of address lo
 *   TIM2 CH1 - ROM OE (PA0) trigger to DMA1 CH5 capture of address hi
 */

/*
 * Timer trigger possibilities
 *
 *       TS 000   001    010    011
 * Slave   ITR0   ITR1   ITR2   ITR3
 * ---------------------------------
 *  TIM1 | TIM5 | TIM2 | TIM3 | TIM4
 *  TIM2 | TIM1 | TIM8 | TIM3 | TIM4
 *  TIM3 | TIM1 | TIM2 | TIM5 | TIM4
 *  TIM4 | TIM1 | TIM2 | TIM3 | TIM8
 *  TIM5 | TIM2 | TIM3 | TIM4 | TIM8
 *
 *  Timer Triggers in use:
 *      TIM1 -> TIM4 ITR0
 */

/*
 * STM32F4 TIM2 implements a 32-bit counter. This allows us to very easily
 * implement a 64-bit clock tick value by software incrementing the top
 * 32 bits on the 32-bit rollover every ~72 seconds.
 *
 * STM32F1 does not have a 32-bit counter on any timer, but two timers can
 * be chained to form a 32-bit counter. Because of this capability, we can
 * still implement a 64-bit clock tick value, but the code is a bit more
 * complicated. For that reason, the low level routines must be slightly
 * different.
 */

static volatile uint32_t timer_high = 0;

/**
 * __dmb() implements a Data Memory Barrier.
 *
 * This function requires no arguments.
 *
 * @return      None.
 */
__attribute__((always_inline))
static inline void __dmb(void)
{
    __asm__ volatile("dmb");
}

#ifdef STM32F1
void
tim4_isr(void)
{
    uint32_t flags = TIM_SR(TIM4) & TIM_DIER(TIM4);

    TIM_SR(TIM4) = ~flags;  // Clear observed flags

    if (flags & TIM_SR_UIF)
        timer_high++;  // Increment upper bits of 64-bit timer value

    if (flags & ~TIM_SR_UIF) {
        TIM_DIER(TIM4) &= ~(flags & ~TIM_SR_UIF);
        printf("Unexpected TIM4 IRQ: %04lx\n", flags & ~TIM_SR_UIF);
    }
}

uint64_t
timer_tick_get(void)
{
    /*
     * TIM1       is high speed tick 72 MHz RCC_PLK2
     * TIM4       is cascaded tick, 72 MHz / 65536 = ~1098.6 Hz
     * timer_high is cascaded globak, 72 MHz / 2^32 = ~0.0168 Hz
     */
    uint32_t high   = timer_high;
    uint32_t high16 = TIM_CNT(TIM4);
    uint32_t low16  = TIM_CNT(TIM1);

    /*
     * A Data Memory Barrier (ARM "dmb") here is necessary to prevent
     * a pipeline fetch of timer_high before the TIM4 CNT fetch has
     * completed. Without it, a timer update interrupt happening at
     * this point could potentially exhibit a non-monotonic clock.
     */
    __dmb();

    /*
     * Check for unhandled timer rollover. Note this must be checked
     * twice due to an ARM pipelining race with interrupt context.
     */
    if ((TIM_SR(TIM4) & TIM_SR_UIF) && (TIM_SR(TIM4) & TIM_SR_UIF)) {
        high++;
        if ((low16 > TIM_CNT(TIM1)) || (high16 > TIM_CNT(TIM4))) {
            /* timer wrapped */
            high16 = TIM_CNT(TIM4);
            low16 = TIM_CNT(TIM1);
        }
    } else if ((high16 != TIM_CNT(TIM4)) || (high != timer_high)) {
        /* TIM1 or interrupt rollover */
        high   = timer_high;
        high16 = TIM_CNT(TIM4);
        low16  = TIM_CNT(TIM1);
    }
    return (((uint64_t) high << 32) | (high16 << 16) | low16);
}

void
timer_init(void)
{
    /*
     * TIM1 is the low 16 bits of the 32-bit counter.
     * TIM4 is the high 16 bits of the 32-bit counter.
     * We chain a rollover of TIM1 to increment TIM4.
     * TIM4 rollover causes an interrupt, which software
     * uses to then increment the upper 32-bit part of
     * the 64-bit system tick counter.
     */

    /* Enable and reset TIM1 and TIM4 */
    RCC_APB2ENR  |=  RCC_APB2ENR_TIM1EN;
    RCC_APB2RSTR |=  RCC_APB2RSTR_TIM1RST;
    RCC_APB2RSTR &= ~(RCC_APB2RSTR_TIM1RST);
    RCC_APB1ENR  |=  RCC_APB1ENR_TIM4EN;
    RCC_APB1RSTR |=  RCC_APB1RSTR_TIM4RST;
    RCC_APB1RSTR &= ~(RCC_APB1RSTR_TIM4RST);

    /* Set timer CR1 mode (No clock division, Edge, Dir Up) */
    TIM_CR1(TIM4) &= ~(TIM_CR1_CKD_CK_INT_MASK | TIM_CR1_CMS_MASK | TIM_CR1_DIR_DOWN);
    TIM_CR1(TIM1) &= ~(TIM_CR1_CKD_CK_INT_MASK | TIM_CR1_CMS_MASK | TIM_CR1_DIR_DOWN);

    TIM_ARR(TIM4)  = 0xffff;       // Set period (rollover at 2^16)
    TIM_ARR(TIM1)  = 0xffff;       // Set period (rollover at 2^16)
    TIM_CR1(TIM1) |= TIM_CR1_URS;  // Update on overflow
    TIM_CR1(TIM1) &= ~TIM_CR1_OPM; // Continuous mode

    /* TIM1 is master - generate TRGO to TIM4 on rollover (UEV) */
    TIM_CR2(TIM1) &= TIM_CR2_MMS_MASK;
    TIM_CR2(TIM1) |= TIM_CR2_MMS_UPDATE;

    /* TIM4 is slave of TIM1 (ITR0) per Table 86 */
    TIM_SMCR(TIM4) = 0;
    TIM_SMCR(TIM4) |= TIM_SMCR_TS_ITR0;

    /* TIM4 has External Clock Mode 1 (increment on rising edge of TRGI) */
    TIM_SMCR(TIM4) |= TIM_SMCR_SMS_ECM1;

    /* Enable counters */
    TIM_CR1(TIM4)  |= TIM_CR1_CEN;
    TIM_CR1(TIM1)  |= TIM_CR1_CEN;

    /* Enable TIM4 rollover interrupt, but not TIE (interrupt on trigger) */
    TIM_DIER(TIM4) |= TIM_DIER_UIE | TIM_DIER_TDE;
    nvic_set_priority(NVIC_TIM4_IRQ, 0x11);
    nvic_enable_irq(NVIC_TIM4_IRQ);
}

void
timer_shutdown(void)
{
    TIM_DIER(TIM4) = 0;
}

void
timer_delay_ticks(uint32_t ticks)
{
    uint32_t start = TIM_CNT(TIM1);
    while ((uint16_t) (TIM_CNT(TIM1) - start) < ticks) {
        /* Empty */
    }
}

#else  /* STM32F407 */
void
tim2_isr(void)
{
    uint32_t flags = TIM_SR(TIM2) & TIM_DIER(TIM2);

    TIM_SR(TIM2) = ~flags;  // Clear observed flags

    if (flags & TIM_SR_UIF)
        timer_high++;  // Increment upper bits of 64-bit timer value

    if (flags & ~TIM_SR_UIF) {
        TIM_DIER(TIM2) &= ~(flags & ~TIM_SR_UIF);
        printf("Unexpected TIM2 IRQ: %04lx\n", flags & ~TIM_SR_UIF);
    }
}

/* STM32F407 */
uint64_t
timer_tick_get(void)
{
    uint32_t high = timer_high;
    uint32_t low  = TIM_CNT(TIM2);

    /*
     * A Data Memory Barrier (ARM "dmb") here is necessary to prevent
     * a pipeline fetch of timer_high before the TIM2 CNT fetch has
     * completed. Without it, a timer update interrupt happening at
     * this point could potentially exhibit a non-monotonic clock.
     */
    __dmb();

    /*
     * Check for unhandled timer rollover. Note this must be checked
     * twice due to an ARM pipelining race with interrupt context.
     */
    if ((TIM_SR(TIM2) & TIM_SR_UIF) && (TIM_SR(TIM2) & TIM_SR_UIF)) {
        high++;
        if (low > TIM_CNT(TIM2))
            low = TIM_CNT(TIM2);
    } else if (high != timer_high) {
        low = TIM_CNT(TIM2);
        high = timer_high;
    }
    return (((uint64_t) high << 32) | low);
}

/* STM32F407 */
void
timer_init(void)
{
    /* Enable and reset 32-bit TIM2 */
    RCC_APB1ENR  |=  RCC_APB1ENR_TIM2EN;
    RCC_APB1RSTR |=  RCC_APB1RSTR_TIM2RST;
    RCC_APB1RSTR &= ~RCC_APB1RSTR_TIM2RST;

    /* Set TIM2 CR1 mode (CK INT, Edge, Dir Up) */
    TIM_CR1(TIM2) &= ~(TIM_CR1_CKD_CK_INT_MASK | TIM_CR1_CMS_MASK | TIM_CR1_DIR_DOWN);

    TIM_ARR(TIM2)  = 0xffffffff;   // Set period (rollover at 2^32)
    TIM_CR1(TIM2) |= TIM_CR1_URS;  // Update on overflow
    TIM_CR1(TIM2) &= ~TIM_CR1_OPM; // Continuous mode
    TIM_CR1(TIM2) |= TIM_CR1_CEN;  // Enable counter

    /* Enable TIM2 rollover interrupt */
    TIM_DIER(TIM2) |= TIM_DIER_TIE | TIM_DIER_UIE | TIM_DIER_TDE;
    nvic_set_priority(NVIC_TIM2_IRQ, 0x11);
    nvic_enable_irq(NVIC_TIM2_IRQ);
}

/* STM32F407 */
void
timer_delay_ticks(uint32_t ticks)
{
    uint32_t start = TIM_CNT(TIM2);
    while ((uint16_t) (TIM_CNT(TIM2) - start) < ticks) {
        /* Empty */
    }
}
#endif

/**
 * timer_usec_to_tick() converts the specified number of microseconds to an
 *                      equivalent number of timer ticks.
 *
 * @param [in]  usec - The number of microseconds to convert.
 *
 * @return      The equivalent number of timer ticks.
 */
uint64_t
timer_usec_to_tick(uint usec)
{
    uint64_t ticks_per_usec = rcc_pclk2_frequency / 1000000;  // nominal 60 MHz
    return (ticks_per_usec * usec);
}

/*
 * timer_nsec_to_tick() converts the specified number of nanoseconds to an
 *                      equivalent number of timer ticks.
 *
 * @param [in]  nsec - The number of nanoseconds to convert.
 *
 * @return      The equivalent number of timer ticks.
 */
uint32_t
timer_nsec_to_tick(uint nsec)
{
    return (rcc_pclk2_frequency / 1000000 * nsec / 1000);
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
 *     start = timer_tick_get();
 *     measure_func();
 *     end = timer_tick_get();
 *     printf("diff=%lu us\n", (uint32_t)timer_tick_to_usec(end - start));
 */
uint64_t
timer_tick_to_usec(uint64_t value)
{
    return (value / (rcc_pclk2_frequency / 1000000));
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
bool
timer_tick_has_elapsed(uint64_t value)
{
    uint64_t now = timer_tick_get();

    if (now >= value)
        return (true);

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
    uint64_t ticks_per_msec = rcc_pclk2_frequency / 1000;
    return (timer_tick_get() + ticks_per_msec * msec);
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
    uint64_t ticks_per_usec = rcc_pclk2_frequency / 1000000;
    return (timer_tick_get() + ticks_per_usec * usec);
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

#include <time.h>
time_t
time(time_t *ptr)
{
    return (timer_tick_get());
}
