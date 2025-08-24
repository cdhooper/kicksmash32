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

#ifdef STANDALONE
#include "util.h"
#include "cache.h"
#else
#include <devices/cd.h>
#include <inline/timer.h>
#include <inline/exec.h>
#include <inline/dos.h>
#endif

#define BIT(x) (1U << (x))

#ifdef STANDALONE
/* Standalone is always in supervisor state */
#define SUPERVISOR_STATE_ENTER()
#define SUPERVISOR_STATE_EXIT()
#else
#define SUPERVISOR_STATE_ENTER()    { \
                                      APTR old_stack = SuperState()
#define SUPERVISOR_STATE_EXIT()       UserState(old_stack); \
                                    }
#endif
#define CACHE_DISABLE_DATA() \
        { \
            uint32_t oldcachestate = \
            CacheControl(0L, CACRF_EnableD) & (CACRF_EnableD | CACRF_DBE)
#define CACHE_RESTORE_STATE() \
            CacheControl(oldcachestate, CACRF_EnableD | CACRF_DBE | CACRF_ClearD); \
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
            __asm volatile("nop"); \
        }

#define CACHE_FLUSH() \
        switch (cpu_type) { \
            case 68030: \
                cpu_set_cacr(cpu_get_cacr() | CACRF_ClearI | CACRF_ClearD); \
                break; \
            case 68040: \
            case 68060: \
                cpu_cache_flush_040_both(); \
                break; \
        } \
        __asm volatile("nop");

#define MMU_FLUSH() \
        switch (cpu_type) { \
            case 68030: \
                flush_tlb_030(); \
                break; \
            case 68040: \
            case 68060: \
                flush_tlb_040(); \
                break; \
        }

#define INTERRUPTS_DISABLE() if (irq_disabled++ == 0) \
                                 Disable()  /* Disable interrupts */
#define INTERRUPTS_ENABLE()  if (--irq_disabled == 0) \
                                 Enable()   /* Enable Interrupts */

#define CIA_USEC(x)      (((x) * 715909 + 1000000 - 715909) / 1000000)
#define CIA_USEC_LONG(x) (((x) * 7159   + 10000   - 7159)   / 10000)

#ifndef ADDR8
#define ADDR8(x)    ((volatile uint8_t *)  ((uintptr_t)(x)))
#define ADDR16(x)   ((volatile uint16_t *) ((uintptr_t)(x)))
#define ADDR32(x)   ((volatile uint32_t *) ((uintptr_t)(x)))
#endif
#ifndef VADDR8
#define VADDR8(x)    ((volatile uint8_t *)  ((uintptr_t)(x)))
#define VADDR16(x)   ((volatile uint16_t *) ((uintptr_t)(x)))
#define VADDR32(x)   ((volatile uint32_t *) ((uintptr_t)(x)))
#endif

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

__attribute__((unused))
static inline uint32_t
cpu_get_cacr(void)
{
    uint32_t cacr;
    __asm volatile("movec.l cacr, %0" : "=r" (cacr)::);
    return (cacr);
}

__attribute__((unused))
static inline void
cpu_set_cacr(uint32_t cacr)
{
    __asm volatile("movec.l %0, cacr" :: "r" (cacr):);
}

__attribute__((unused))
static inline void
cpu_cache_flush_040_both(void)
{
    __asm volatile("nop\n\t"
                   "cpusha %bc");
}

__attribute__((unused))
static inline void
cpu_cache_flush_040_data(void)
{
    __asm volatile("nop\n\t"
                   ".word 0xf478");  // Flush Data Cache
}

__attribute__((unused))
static inline void
cpu_cache_flush_040_inst(void)
{
    __asm volatile("nop\n\t"
                   ".word 0xf4b8");  // Flush Inst Cache
}

__attribute__((unused))
static inline void
cpu_cache_flush_040_instdata(void)
{
    __asm volatile("nop\n\t"
                   ".word 0xf4f8");  // Flush Inst and Data Cache
}

__attribute__((unused))
static inline void
cpu_cache_invalidate_040(void)
{
    __asm volatile("nop\n\t"
                   "cinva %bc");
}

static __inline void __attribute__((__unused__))
flush_tlb_030(void)
{
    /* Using exact opcode because 68030 pflusha differs from 68040 pflusha */
    __asm volatile(".word 0xf000 \n\r"  // 68030 pflusha
                   ".word 0x2400");
}

static __inline void __attribute__((__unused__))
flush_tlb_040(void)  // Also 68060
{
    /* Using exact opcode because 68040 pflusha differs from 68030 pflusha */
    __asm volatile(".word 0xf518");  // 68040 pflusha
}

void cpu_cache_flush(void);

extern unsigned int cpu_type;
extern unsigned int irq_disabled;

#ifndef _DCC
extern struct ExecBase *SysBase;
#endif

#endif /* _CPU_CONTROL_H */
