/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in August 2020.
 *
 * ---------------------------------------------------------------------
 *
 * Generic physical memory access code.
 */

#ifdef EMBEDDED_CMD
#include "printf.h"
#endif
#include "med_cmdline.h"
#include "mem_access.h"

#if 0
#if defined(AMIGA)
#define AMIGA_BERR_DSACK 0x00de0000  // Bit7=1 for BERR on timeout, else DSACK

typedef unsigned int uintptr_t;

void mem_copy8(void *dst, void *src);
void mem_copy16(void *dst, void *src);
void mem_copy32(void *dst, void *src);

/* 68000 Can not handle unaligned access */
const bool_t no_unaligned_access = TRUE;

#define ADDR8(x) ((uint8_t *)(x))
#define BIT(x) (1U << (x))
#define MEM_FAULT_CAPTURE                                   \
    struct Task *thistask = (struct Task *) FindTask(NULL); \
    uint8_t old_berr_dsack = *ADDR8(AMIGA_BERR_DSACK);      \
    *ADDR8(AMIGA_BERR_DSACK) &= ~BIT(7);                    \
    old_TrapCode = thistask->tc_TrapCode;                   \
    thistask->tc_TrapCode = trap_handler
#define MEM_FAULT_RESTORE                                   \
    thistask->tc_TrapCode = old_TrapCode;                   \
    *ADDR8(AMIGA_BERR_DSACK) = old_berr_dsack
void trap_handler(void);
APTR old_TrapCode;
#endif
#endif

const bool_t no_unaligned_access = FALSE;
#define MEM_FAULT_CAPTURE \
    mem_fault_ok = TRUE
#define MEM_FAULT_RESTORE \
    mem_fault_ok = FALSE
uint8_t mem_fault_ok = FALSE;
volatile uint mem_fault_count = 0;

uint64_t mem_copy8(void *dst, void *src);
uint64_t mem_copy16(void *dst, void *src);
uint64_t mem_copy32(void *dst, void *src);

// XXX: rewrite as C functions

/*
 * void mem_copy8(void *dst, void *src)
 *           Copy 8 bytes from src to dst address
 *
 */
__asm("_mem_copy8: \n"
      "move.l 8(sp),a0 \n" //       ; a0 = src
      "move.l 4(sp),a1 \n" //       ; a1 = dst
      "movem.l (a0),d0-d1 \n"
      "movem.l d0-d1,(a1) \n"
      "rts");

/*
 * void mem_copy16(void *dst, void *src)
 *           Copy 16 bytes from src to dst address
 */
__asm("_mem_copy16: \n"
      "move.l 8(sp),a0 \n"
      "move.l 4(sp),a1 \n"
      "movem.l d2-d3,-(sp) \n"
      "movem.l (a0),d0-d3 \n"
      "movem.l d0-d3,(a1) \n"
      "movem.l (sp)+,d2-d3 \n"
      "rts");

/*
 * void mem_copy32(void *dst, void *src)
 *           Copy 32 bytes from src to dst address
 */
__asm("_mem_copy32: \n"
      "move.l 8(sp),a0 \n"
      "move.l 4(sp),a1 \n"
      "movem.l d2-d7,-(sp) \n"
      "movem.l (a0),d0-d7 \n"
      "movem.l d0-d7,(a1) \n"
      "movem.l (sp)+,d2-d7 \n"
      "rts");

rc_t
mem_read(uint64_t addr, uint width, void *bufp)
{
    uint8_t *buf = (uint8_t *) bufp;
    bool_t   no_unaligned = no_unaligned_access;

    MEM_FAULT_CAPTURE;
    mem_fault_count = 0;

    while (width > 0) {
        uint mode = width;

        /* Handle unaligned source adddress */
        if (addr & 1)
            mode = 1;
        else if ((mode > 2) && (addr & 2))
            mode = 2;
        else if ((mode > 4) && (addr & 4))
            mode = 4;

        switch (mode) {
            case 0:
                return (RC_FAILURE);
            case 1:
rmode_1:
                mode = 1;
                *buf = *(uint8_t *)(uintptr_t)addr;
                break;
            case 2:
            case 3:
rmode_2:
                if (no_unaligned && (addr & 1))
                    goto rmode_1;
                mode = 2;
                *(uint16_t *)buf = *(uint16_t *)(uintptr_t)addr;
                break;
            default:
            case 4:
            case 5:
            case 6:
            case 7:
rmode_4:
                if (no_unaligned && (addr & 3))
                    goto rmode_2;
                mode = 4;
                *(uint32_t *)buf = *(uint32_t *)(uintptr_t)addr;
                break;
#ifndef AMIGA
            case 8:
                if (no_unaligned && (addr & 7))
                    goto rmode_4;
                mode = 8;
                *(uint64_t *)buf = *(uint64_t *)(uintptr_t)addr;
                break;
#else /* AMIGA */
            case 8:
rmode_8:
                if (no_unaligned && (addr & 7))
                    goto rmode_4;
                mode = 8;
                mem_copy8(buf, (void *)(uintptr_t)addr);
                break;
            case 16:
rmode_16:
                if (no_unaligned && (addr & 0xf))
                    goto rmode_8;
                mode = 16;
                mem_copy16(buf, (void *)(uintptr_t)addr);
                break;
            case 32:
// rmode_32:
                if (no_unaligned && (addr & 0x1f))
                    goto rmode_16;
                mem_copy32(buf, (void *)(uintptr_t)addr);
                break;
#endif
        }
        buf   += mode;
        addr  += mode;
        width -= mode;
    }
    MEM_FAULT_RESTORE;
    if (mem_fault_count != 0)
        return (RC_FAILURE);

    return (RC_SUCCESS);
}

rc_t
mem_write(uint64_t addr, uint width, void *bufp)
{
    uint8_t *buf = (uint8_t *) bufp;

    MEM_FAULT_CAPTURE;
    mem_fault_count = 0;

    while (width > 0) {
        uint mode = width;

        /* Handle unaligned source adddress */
        if (addr & 1)
            mode = 1;
        else if ((mode > 2) && (addr & 2))
            mode = 2;
        else if ((mode > 4) && (addr & 4))
            mode = 4;

        switch (mode) {
            case 0:
                return (RC_FAILURE);
            case 1:
wmode_1:
                *(uint8_t *)(uintptr_t)addr = *buf;
                break;
            case 2:
            case 3:
wmode_2:
                if (no_unaligned_access && (addr & 1))
                    goto wmode_1;
                mode = 2;
                *(uint16_t *)(uintptr_t)addr = *(uint16_t *)buf;
                break;
            default:
            case 4:
            case 5:
            case 6:
            case 7:
wmode_4:
                if (no_unaligned_access && (addr & 3))
                    goto wmode_2;
                mode = 4;
                *(uint32_t *)(uintptr_t)addr = *(uint32_t *)buf;
                break;
#ifndef AMIGA
            case 8:
                if (no_unaligned_access && (addr & 7))
                    goto wmode_4;
                mode = 8;
                *(uint64_t *)(uintptr_t)addr = *(uint64_t *)buf;
                break;
#else /* AMIGA */
            case 8:
wmode_8:
                if (no_unaligned_access && (addr & 7))
                    goto wmode_4;
                mode = 8;
                mem_copy8((void *)(uintptr_t)addr, buf);
                break;
            case 16:
wmode_16:
                if (no_unaligned_access && (addr & 0xf))
                    goto wmode_8;
                mode = 16;
                mem_copy16((void *)(uintptr_t)addr, buf);
                break;
            case 32:
// wmode_32:
                if (no_unaligned_access && (addr & 0x1f))
                    goto wmode_16;
                mem_copy32((void *)(uintptr_t)addr, buf);
                break;
#endif
        }
        buf   += mode;
        addr  += mode;
        width -= mode;
    }

    MEM_FAULT_RESTORE;
    if (mem_fault_count != 0)
        return (RC_FAILURE);

    return (RC_SUCCESS);
}
