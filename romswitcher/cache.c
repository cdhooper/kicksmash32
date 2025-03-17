/*
 * CPU cache control.
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
#include "util.h"
#include "cpu_control.h"

static inline uint32_t
get_cacr(void)
{
    uint32_t cacr;
    __asm volatile("movec.l cacr, %0" : "=r" (cacr)::);
    return (cacr);
}

static inline void
set_cacr(uint32_t cacr)
{
    __asm volatile("movec.l %0, cacr" :: "r" (cacr):);
}


uint32_t
CacheControl(uint32_t cache_bits, uint32_t cache_mask)
{
    uint32_t old_cacr;
    uint32_t new_cacr;

    old_cacr = get_cacr();
*ADDR32(0x1010) = old_cacr;
return (old_cacr);
    new_cacr = (old_cacr & ~cache_mask) | cache_bits;
    set_cacr(new_cacr);

    return (old_cacr); // Return the previous CACR value
}

void
cache_init(void)
{
#if 0
    CacheControl(CACRF_EnableD | CACRF_DBE | CACRF_ClearD |
                 CACRF_EnableI | CACRF_IBE | CACRF_ClearI,
                 CACRF_EnableD | CACRF_DBE | CACRF_ClearD |
                 CACRF_EnableI | CACRF_IBE | CACRF_ClearI);
#endif
}
