/*
 * Random utility functions.
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
#ifndef _UTIL
#define _UTIL

#define ADDR8(x)    ((uint8_t *)  ((uintptr_t)(x)))
#define ADDR16(x)   ((uint16_t *) ((uintptr_t)(x)))
#define ADDR32(x)   ((uint32_t *) ((uintptr_t)(x)))

#define VADDR8(x)    ((volatile uint8_t *)  ((uintptr_t)(x)))
#define VADDR16(x)   ((volatile uint16_t *) ((uintptr_t)(x)))
#define VADDR32(x)   ((volatile uint32_t *) ((uintptr_t)(x)))

#define BIT(x) (1U << (x))
#define ARRAY_SIZE(x) ((sizeof (x) / sizeof ((x)[0])))

typedef unsigned int uint;

#undef isdigit
#undef isxdigit
#undef isalnum
#undef isspace

#define isdigit(x) (((x) >= '0') && ((x) <= '9'))
#define isxdigit(x) (isdigit(x) || \
                     (((x) >= 'a') && ((x) <= 'f')) || \
                     (((x) >= 'A') && ((x) <= 'F')))
#define isalnum(x) (((x) >= ' ') || ((x) <= 'z'))
#define isspace(x) (((x) == ' ') || ((x) == '\t'))

#define SAVE_A4()         __asm("move.l a4,-(sp)")
#define RESTORE_A4()      __asm("move.l (sp)+,a4")
#define GET_GLOBALS_PTR() __asm("move.l 0x100,a4")

#include <stddef.h>

#define MEMF_PUBLIC (1UL<<0)
#define MEMF_CHIP   (1UL<<1)

void *AllocVec(uint byteSize, uint requirements);
void *AllocMem(uint byteSize, uint requirements);

void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void free(void *addr);
char *strdup(const char *s);
void *malloc_chipmem(size_t size);
void free_chipmem(void *addr);

static inline uint32_t
get_sp(void)
{
    uint32_t sp;
    __asm volatile("move.l sp, %0" : "=d" (sp)::);
    return (sp);
}

static inline uint32_t
get_sr(void)
{
    uint32_t sr;
    __asm volatile("move.w sr, %0" : "=d" (sr)::);
    return (sr);
}

/* Disable interrupts */
static inline uint32_t
irq_disable(void)
{
    uint32_t sr;
    __asm volatile("move.w sr, %0 \n"
                   "or.w #0x0700, sr" : "=d" (sr)::);
    return (sr);
}

#define Enable()  irq_enable()
#define Disable() irq_disable()

/* Enable interrupts */
static inline uint32_t
irq_enable(void)
{
    uint32_t sr;
    __asm volatile("move.w sr, %0 \n"
                   "and.w #0xf8ff, sr" : "=d" (sr)::);
    return (sr);
}

/* Restore interrupts to previous state */
static inline void
irq_restore(uint32_t sr)
{
    __asm volatile("move.w %0, sr" :: "d" (sr));
}

void checknull(uintptr_t addr, const char *text);

#endif /* _UTIL */
