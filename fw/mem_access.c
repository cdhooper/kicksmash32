/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2020.
 *
 * ---------------------------------------------------------------------
 *
 * Generic physical memory access code.
 */

#ifdef EMBEDDED_CMD
#include "printf.h"
#endif
#include "cmdline.h"
#include "main.h"
#include "mem_access.h"

#if defined(AMIGA)
/* 68000 Can not handle unaligned access */
const bool_t no_unaligned_access = TRUE;
#else
const bool_t no_unaligned_access = FALSE;
#endif

uint8_t mem_fault_ok    = 0;
uint    mem_fault_count = 0;

rc_t
mem_read(uint64_t addr, uint width, void *bufp)
{
    uint8_t *buf = (uint8_t *) bufp;

    mem_fault_count = 0;
    mem_fault_ok    = TRUE;

    while (width > 0) {
        uint mode = width;

        /* Handle unaligned source address */
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
                if (no_unaligned_access && (addr & 1))
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
                if (no_unaligned_access && (addr & 3))
                    goto rmode_2;
                mode = 4;
                *(uint32_t *)buf = *(uint32_t *)(uintptr_t)addr;
                break;
#ifndef AMIGA
            case 8:
                if (no_unaligned_access && (addr & 7))
                    goto rmode_4;
                mode = 8;
                *(uint64_t *)buf = *(uint64_t *)(uintptr_t)addr;
                break;
#endif
        }
        buf   += mode;
        addr  += mode;
        width -= mode;
    }

    mem_fault_ok = FALSE;
    if (mem_fault_count != 0)
        return (RC_FAILURE);

    return (RC_SUCCESS);
}

rc_t
mem_write(uint64_t addr, uint width, void *bufp)
{
    uint8_t *buf = (uint8_t *) bufp;

    mem_fault_count = 0;
    mem_fault_ok    = TRUE;

    while (width > 0) {
        uint mode = width;

        /* Handle unaligned source address */
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
#endif
        }
        buf   += mode;
        addr  += mode;
        width -= mode;
    }

    mem_fault_ok = FALSE;
    if (mem_fault_count != 0)
        return (RC_FAILURE);

    return (RC_SUCCESS);
}
