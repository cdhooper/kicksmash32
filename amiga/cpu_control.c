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
#include <exec/execbase.h>
#include "cpu_control.h"

#define CIAA_TBLO        ADDR8(0x00bfe601)
#define CIAA_TBHI        ADDR8(0x00bfe701)

unsigned int irq_disabled = 0;
unsigned int cpu_type = 0;

struct ExecBase *SysBase;

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
                         "move.l (sp)+,d0"
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
#if 0
    __asm__ __volatile__("adda.l #4,sp \n\t"
                         ".long 0xf0174000 \n\t"  // pmove.l (sp),tc
                         "suba.l #4,sp"
                         : "=d" (tc));
#elif 1
    __asm__ __volatile__("move.l %0,-(sp) \n\t"
                         ".long 0xf0174000 \n\t"  // pmove.l (sp),tc
                         "adda.l #4,sp \n\t"
                         : "=d" (tc));
#endif
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

void
cpu_control_init(void)
{
#ifndef _DCC
    SysBase = *(struct ExecBase **)4UL;
#endif

    cpu_type = get_cpu();
}
