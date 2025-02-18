/*
 * CPU cache control.
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
#ifndef _CACHE_H
#define _CACHE_H

#define CACRF_EnableI           BIT(0)  // Enable instruction cache
#define CACRF_FreezeI           BIT(1)  // Freeze instruction cache
#define CACRF_ClearI            BIT(3)  // Clear instruction cache
#define CACRF_IBE               BIT(4)  // Enable Instruction burst
#define CACRF_EnableD           BIT(8)  // 68030 Enable data cache
#define CACRF_FreezeD           BIT(9)  // 68030 Freeze data cache
#define CACRF_ClearD            BIT(11) // 68030 Clear data cache
#define CACRF_DBE               BIT(12) // 68030 Data burst enable
#define CACRF_WriteAllocate     BIT(13) // 68030 Write-Allocate mode: leave on
#define CACRF_EnableE           BIT(30) // Master enable for external caches

uint32_t CacheControl(uint32_t cache_bits, uint32_t cache_mask);

void cache_init(void);

#endif /* _CACHE_H */
