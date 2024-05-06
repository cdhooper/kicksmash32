/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2024.
 *
 * ---------------------------------------------------------------------
 *
 * Functions to control the CPU state from AmigaOS.
 */

#ifndef _CPU_CONTROL_H
#define _CPU_CONTROL_H

#ifndef _DCC
#include <devices/cd.h>
#include <inline/timer.h>
#include <inline/exec.h>
#include <inline/dos.h>
#endif

#define BIT(x) (1U << (x))

#define SUPERVISOR_STATE_ENTER()    { \
                                      APTR old_stack = SuperState()
#define SUPERVISOR_STATE_EXIT()       UserState(old_stack); \
                                    }
#define CACHE_DISABLE_DATA() \
        { \
            uint32_t oldcachestate = \
            CacheControl(0L, CACRF_EnableD) & (CACRF_EnableD | CACRF_DBE)
#define CACHE_RESTORE_STATE() \
            CacheControl(oldcachestate, CACRF_EnableD | CACRF_DBE); \
        }

/* MMU_DISABLE() and MMU_RESTORE() must be called from Supervisor state */
#define MMU_DISABLE() \
        { \
            uint32_t oldmmustate = 0; \
            if (cpu_type == 68030) { \
                oldmmustate = mmu_get_tc_030(); \
                mmu_set_tc_030(oldmmustate & ~BIT(31)); \
            } else if ((cpu_type == 68040) || (cpu_type == 68060)) { \
                oldmmustate = mmu_get_tc_040(); \
                mmu_set_tc_040(oldmmustate & ~BIT(15)); \
            }
#define MMU_RESTORE() \
            if (cpu_type == 68030) { \
                mmu_set_tc_030(oldmmustate); \
            } else if ((cpu_type == 68040) || (cpu_type == 68060)) { \
                mmu_set_tc_040(oldmmustate); \
            } \
        }

#define INTERRUPTS_DISABLE() if (irq_disabled++ == 0) \
                                 Disable()  /* Disable interrupts */
#define INTERRUPTS_ENABLE()  if (--irq_disabled == 0) \
                                 Enable()   /* Enable Interrupts */

#define CIA_USEC(x)      (x * 715909 / 1000000)
#define CIA_USEC_LONG(x) (x * 7159 / 10000)

#define ADDR8(x)    ((volatile uint8_t *)  ((uintptr_t)(x)))
#define ADDR16(x)   ((volatile uint16_t *) ((uintptr_t)(x)))
#define ADDR32(x)   ((volatile uint32_t *) ((uintptr_t)(x)))

/*
 * Define compile-time assert. This macro relies on gcc's built-in
 * static assert checking which is available in C11.
 */
#include <assert.h>
#define STATIC_ASSERT(test_for_true) \
    static_assert((test_for_true), "(" #test_for_true ") failed")

#define ARRAY_SIZE(x) ((sizeof (x) / sizeof ((x)[0])))

void cpu_control_init(void);
void cia_spin(unsigned int ticks);

__attribute__ ((noinline)) uint32_t mmu_get_tc_030(void);
__attribute__ ((noinline)) uint32_t mmu_get_tc_040(void);
__attribute__ ((noinline)) void mmu_set_tc_030(register uint32_t tc asm("%d0"));
__attribute__ ((noinline)) void mmu_set_tc_040(register uint32_t tc asm("%d0"));

extern unsigned int cpu_type;
extern unsigned int irq_disabled;

#ifndef _DCC
extern struct ExecBase *SysBase;
#endif

#endif /* _CPU_CONTROL_H */
