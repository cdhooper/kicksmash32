/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2020.
 *
 * ---------------------------------------------------------------------
 *
 * Interrupt handling.
 */

#ifndef _IRQ_H
#define _IRQ_H

#define LIBOPENCM3_IRQ_MASKING
#ifdef LIBOPENCM3_IRQ_MASKING

#include <libopencm3/cm3/cortex.h>
#define disable_irq() cm_disable_interrupts()
#define enable_irq()  cm_enable_interrupts()

/*
 * XXX: probably should use the below functions instead.
 *
 * #define disable_irq() (void) cm_mask_interrupts(x)
 * #define enable_irq(x) (void) cm_mask_interrupts(x)
 */

#else /* local IRQ masking */

#define disable_irq() __disable_irq()
#define enable_irq() __enable_irq()

__attribute__((always_inline))
static inline bool __disable_irq(void)
{
    register uint32_t primask_val;
    __asm__ volatile("MRS %0, primask" : "=r" (primask_val));
    __asm__ volatile("cpsid i");
    return ((primask_val & 0x1) == 0);
}

__attribute__((always_inline))
static inline void __enable_irq(bool enable)
{
    if (!enable)
        return;
    __asm__ volatile("cpsie i");
}
#endif /* local IRQ masking */

void fault_show_regs(const void *sp);
void fault_hard(void);

#endif /* _IRQ_H */
