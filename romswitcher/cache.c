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

#define CACR_68040_EDC  BIT(31)  // Enable data cache
#define CACR_68040_EIC  BIT(15)  // Enable instruction cache
#define CACR_68060_CABC BIT(22)  // Clear all entries in the branch cache

#define CACR_68030_CD  BIT(11) // Clear data cache
#define CACR_68030_ED  BIT(8)  // Enable data cache
#define CACR_68030_CI  BIT(3)  // Clear instruction cache
#define CACR_68030_EI  BIT(0)  // Enable instruction cache

#define TTR_E     BIT(15)       // Enable transparent translation
#define TTR_S_I   BIT(14)       // Supervisor mode -- Ignore
#define TTR_CM_NC BIT(6)|BIT(5) // Cache mode -- Noncachable

static uint32_t
convert_030_cacr_to_040_cacr(uint32_t cacr_030)
{
    uint32_t cacr_040 = 0;
    if (cacr_030 & CACR_68030_EI)
        cacr_040 |= CACR_68040_EIC;
    if (cacr_030 & CACR_68030_ED)
        cacr_040 |= CACR_68040_EDC;
    return (cacr_040);
}

static uint32_t
convert_040_cacr_to_030_cacr(uint32_t cacr_040)
{
    uint32_t cacr_030 = 0;
    if (cacr_040 & CACR_68040_EIC)
        cacr_030 |= CACR_68030_EI;
    if (cacr_040 & CACR_68040_EDC)
        cacr_030 |= CACR_68030_ED;
    return (cacr_030);
}

uint32_t
CacheControl(uint32_t cache_bits, uint32_t cache_mask)
{
    uint32_t old_cacr;
    uint32_t new_cacr;
    uint32_t cacr_mask;
    uint32_t cacr_bits;
    uint32_t return_cacr;

    old_cacr = cpu_get_cacr();
    switch (cpu_type) {
        default:
        case 68030:
            cacr_mask = cache_mask;
            cacr_bits = cache_bits;
            return_cacr = old_cacr;
            break;
        case 68060:
        case 68040:
            cacr_mask = 0;
            cacr_bits = 0;
            if (cache_bits & CACRF_ClearD)
                cpu_cache_flush_040_data();
            if (cache_bits & CACRF_ClearI)
                cpu_cache_flush_040_inst();

            cacr_mask = convert_030_cacr_to_040_cacr(cache_mask);
            cacr_bits = convert_030_cacr_to_040_cacr(cache_bits);
            return_cacr = convert_040_cacr_to_030_cacr(old_cacr);

            if (cache_mask & CACRF_IBE) {
                /* Instruction burst enabled in another register on 68040 and 68060 */
            }
            if (cache_mask & CACRF_DBE) {
                /* Data burst enabled in another register on 68040 and 68060 */
            }
            break;
    }
    new_cacr = (old_cacr & ~cacr_mask) | cacr_bits;
    cpu_set_cacr(new_cacr);

    return (return_cacr); // Return the previous CACR value
}

void
cache_init(void)
{
    switch (cpu_type) {
        case 68030:
            flush_tlb_030();
            cpu_set_cacr(CACR_68030_CD | CACR_68030_CI);
            cpu_set_cacr(CACR_68030_ED | CACR_68030_EI);
            break;
        case 68040:
        case 68060:
            flush_tlb_040();
            cpu_cache_invalidate_040();
            cpu_set_dttr0(TTR_E | TTR_S_I | TTR_CM_NC);
            if (cpu_type == 68060)
                cpu_set_cacr(CACR_68060_CABC);
            cpu_set_cacr(CACR_68040_EDC | CACR_68040_EIC);
            break;
    }
#if 0
    CacheControl(CACRF_EnableD | CACRF_DBE | CACRF_ClearD |
                 CACRF_EnableI | CACRF_IBE | CACRF_ClearI,
                 CACRF_EnableD | CACRF_DBE | CACRF_ClearD |
                 CACRF_EnableI | CACRF_IBE | CACRF_ClearI);
#endif
}
