/*
 * cpu_control
 * -----------
 * Functions to control the CPU state from AmigaOS.
 *
 * Copyright 2024 Chris Hooper. This program and source may be used
 * and distributed freely, for any purpose which benefits the Amiga
 * community. Commercial use of the binary, source, or algorithms requires
 * prior written approval from Chris Hooper <amiga@cdh.eebugs.com>.
 * All redistributions must retain this Copyright notice.
 *
 * DISCLAIMER: THE SOFTWARE IS PROVIDED "AS-IS", WITHOUT ANY WARRANTY.
 * THE AUTHOR ASSUMES NO LIABILITY FOR ANY DAMAGE ARISING OUT OF THE USE
 * OR MISUSE OF THIS UTILITY OR INFORMATION REPORTED BY THIS UTILITY.
 */

#include <stdio.h>
#ifndef STANDALONE
#include <exec/execbase.h>
#endif
#include "cpu_control.h"

#define CIAA_TBLO        ADDR8(0x00bfe601)
#define CIAA_TBHI        ADDR8(0x00bfe701)

unsigned int irq_disabled = 0;
unsigned int cpu_type = 0;

extern struct ExecBase *SysBase;

static uint
cia_ticks(void)
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

void
cia_spin(unsigned int ticks)
{
    uint16_t start = cia_ticks();
    uint16_t now;
    uint16_t diff;

    while (ticks != 0) {
        now = cia_ticks();
        diff = start - now;
        if (diff >= ticks)
            break;
        ticks -= diff;
        start = now;
        __asm__ __volatile__("nop");
        __asm__ __volatile__("nop");
    }
}

#ifdef STANDALONE
static unsigned int
get_cpu(void)
{
    unsigned int cpu_type = 0;

    __asm__ volatile(
        "move.l #68000, %0\n"      // Default to 68000
        "move.l #0x80000100, d1\n" // Enable 68020, 68030 and 68040+ D cache
        "movec.l cacr, d0\n"       // Save current CACR
        "movec.l d1, cacr\n"       // Check if CACR bit can be written
        "movec.l cacr, d1\n"
        "movec.l d0, cacr\n"       // Restore CACR
        "cmp.l #0, d1\n"
        "bne 1f\n"                 // If CACR is nonzero, it's 68020+
        "movec.l  sfc, d1\n"       // Check for 68010
        "cmp.l #0x00008000, d1\n"
        "bne 3f\n"                 // If different, it's 68000
        "move.l #68010, %0\n"
        "bra 3f\n"

        // 68020+
        "1: move.l #0x8000, d1\n"  // CACR.IE (68040 and 68060)
        "movec.l d1, cacr\n"       // Check if CACR bit can be written
        "movec.l cacr, d1\n"
        "movec.l d0, cacr\n"       // Restore CACR
        "cmp.l #0, d1\n"
        "beq 2f\n"                 // Doesn't have CACR.IE (ICache Enable)

        // 68040 or 68060
        "move.l #68040, %0\n"      // 68040+
        "move.l #0x4000, d1\n"     // CACR.NAI (68060)
        "movec.l d1, cacr\n"       // Check if CACR bit can be written
        "movec.l cacr, d1\n"
        "movec.l d0, cacr\n"       // Restore CACR
        "cmp.l #0, d1\n"
        "beq 3f\n"                 // No CACR.NAI; 68040 detected
        "move.l #68060, %0\n"
        "bra 3f\n"

        // 68020 or 68030
        "2: move.l #68030, %0\n"   // 68020 or 68030 detected
        "move.l #0x0200, d1\n"     // CACR.FD (68030)
        "movec.l d1, cacr\n"       // Check if CACR bit can be written
        "movec.l cacr, d1\n"
        "movec.l d0, cacr\n"       // Restore CACR
        "cmp.l #0, d1\n"
        "bne 3f\n"                 // Has CACR.FD (Freeze Data); 68030 detected
        "move.l #68020, %0\n"      // 68020
        "3: nop\n"
        : "=r" (cpu_type)          // CPU type as integer
        :                          // No input operands
        : "d0", "d1"               // Modified registers
    );
    return (cpu_type);
}
#else
static unsigned int
get_cpu(void)
{
    UWORD attnflags = SysBase->AttnFlags;

    if (attnflags & 0x80)
        return (68060);
    if (attnflags & AFF_68040)
        return (68040);
    if (attnflags & AFF_68030)
        return (68030);
    if (attnflags & AFF_68020)
        return (68020);
    if (attnflags & AFF_68010)
        return (68010);
    return (68000);
}
#endif

/*
 * mmu_get_tc_030
 * --------------
 * This function only works on the 68030.
 * 68040 and 68060 have different MMU instructions.
 */

__attribute__ ((noinline)) uint32_t
mmu_get_tc_030(void)
{
    register uint32_t result;
    __asm__ __volatile__("subq.l #4,sp \n\t"
                         ".long 0xf0174200 \n\t"  // pmove tc,(sp)
                         "move.l (sp)+,d0 \n\t"
                         : "=d" (result));
    return (result);
}

/*
 * mmu_set_tc_030
 * --------------
 * This function only works on the 68030.
 */
__attribute__ ((noinline)) void
mmu_set_tc_030(register uint32_t tc asm("%d0"))
{
    __asm__ __volatile__("move.l %0,-(sp) \n\t"
                         ".long 0xf0174000 \n\t"  // pmove.l (sp),tc
                         "adda.l #4,sp \n\t"
                         : "=d" (tc));
}

/*
 * mmu_get_tc_040
 * --------------
 * This function only works on 68040 and 68060.
 */
__attribute__ ((noinline)) uint32_t
mmu_get_tc_040(void)
{
    register uint32_t result;
    __asm__ __volatile__(".long 0x4e7a0003 \n\t"  // movec.l tc,d0
                         "rts \n\t"
                         : "=d" (result));
    return (result);
}

/*
 * mmu_set_tc_040
 * --------------
 * This function only works on 68040 and 68060.
 */
__attribute__ ((noinline)) void
mmu_set_tc_040(register uint32_t tc asm("%d0"))
{
    __asm__ __volatile__(".long 0x4e7b0003 \n\t"  // movec.l d0,tc
                         "rts \n\t"
                         : "=d" (tc));
}

#if 0
__attribute__ ((noinline)) uint32_t
cpu_get_cacr(void)
{
    register uint32_t result;
    __asm__ __volatile__("subq.l #4,sp \n\t"
                         ".long 0xf0174200 \n\t"  // pmove tc,(sp)
                         "move.l (sp)+,d0"
                         : "=d" (result));
    return (result);
}
#endif

#if 0
static inline uint32_t
cpu_get_cacr(void)
{
    uint32_t cacr;
    __asm volatile("movec.l cacr, %0" : "=r" (cacr)::);
    return (cacr);
}

static inline void
cpu_set_cacr(uint32_t cacr)
{
    __asm volatile("movec.l %0, cacr" :: "r" (cacr):);
}

static void
cpu_cache_flush_040_both(void)
{
    __asm volatile("nop\n\t"
                   "cpusha %bc");
}

void
cpu_cache_flush(void)
{
    switch (cpu_type) {
        case 68030:
            cpu_set_cacr(cpu_get_cacr() | CACRF_ClearD);
            break;
        case 68040:
        case 68060:
            cpu_cache_flush_040_both();
            break;
    }
}
#endif


void
cpu_control_init(void)
{
#if !defined(_DCC) && !defined(STANDALONE)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds="
    SysBase = *(struct ExecBase **)4UL;
#pragma GCC diagnostic pop
#endif

    cpu_type = get_cpu();
}
