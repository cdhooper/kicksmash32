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

#define MEM_FAULT_CAPTURE   mem_fault_ok = TRUE
#define MEM_FAULT_RESTORE   mem_fault_ok = FALSE

#if defined(AMIGA)
const bool_t  no_unaligned_access = TRUE; // 68000 can't handle unaligned access
#else
const bool_t  no_unaligned_access = FALSE;
#endif

uint8_t       mem_fault_ok    = FALSE;
volatile uint mem_fault_count = 0;

rc_t
mem_read(uint64_t addr, uint width, void *bufp)
{
    uint8_t *buf = (uint8_t *) bufp;

    MEM_FAULT_CAPTURE;
    mem_fault_count = 0;

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
#else /* AMIGA */
            case 8:
rmode_8:
                if (no_unaligned_access && (addr & 7))
                    goto rmode_4;
                mode = 8;
                mem_copy8(buf, (void *)(uintptr_t)addr);
                break;
            case 16:
rmode_16:
                if (no_unaligned_access && (addr & 0xf))
                    goto rmode_8;
                mode = 16;
                mem_copy16(buf, (void *)(uintptr_t)addr);
                break;
            case 32:
                if (no_unaligned_access && (addr & 0x1f))
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

#ifdef HAVE_SPACE_PHYS
rc_t
phys_mem_read(uint64_t addr, uint width, void *bufp)
{
    rc_t rc;
#ifdef AMIGA
    SUPERVISOR_STATE_ENTER();
    INTERRUPTS_DISABLE();
    CACHE_DISABLE_DATA();
    MMU_DISABLE();
#endif
    rc = mem_read(addr, width, bufp);
#ifdef AMIGA
    CACHE_FLUSH();
    MMU_RESTORE();
    CACHE_RESTORE_STATE();
    INTERRUPTS_ENABLE();
    SUPERVISOR_STATE_EXIT();
#endif
    return (rc);
}

rc_t
phys_mem_write(uint64_t addr, uint width, void *bufp)
{
    rc_t rc;
#ifdef AMIGA
    SUPERVISOR_STATE_ENTER();
    INTERRUPTS_DISABLE();
    CACHE_DISABLE_DATA();
    MMU_DISABLE();
#endif
    rc = mem_write(addr, width, bufp);
#ifdef AMIGA
    CACHE_FLUSH();
    MMU_RESTORE();
    CACHE_RESTORE_STATE();
    INTERRUPTS_ENABLE();
    SUPERVISOR_STATE_EXIT();
#endif
    return (rc);
}
#endif

#if defined (AMIGAOS) && !defined(_DCC)
/*
 * void trap_handler(cpu frame, uint32_t trap#)
 *
 * Trap handling code (leaves D0 intact).  Entered in supervisor
 * mode with the following on the supervisor stack:
 *    0(sp).l = trap#
 *    4(sp) Processor dependent exception frame
 */
__asm("_trap_handler:");

__asm("cmpi.l  #2,(sp)  \n"            // is this bus error?
      "beq.s   bus_err  \n"
      "cmpi.l  #3,(sp)  \n"            // is this address error?
      "beq.s   addr_err \n"
      "cmpi.l  #4,(sp)  \n"            // is this illegal instruction?
      "beq.s   ill_inst \n"
      "cmpi.l  #5,(sp)  \n"            // is this a divide by zero?
      "beq.s   div0_err \n"

      "tst.l   _old_TrapCode  \n"      // is there another trap handler ?
      "beq.s   endtrap        \n"      // no, so we'll exit
      "move.l  _old_TrapCode,-(sp) \n" // yes, go on to old TrapCode
      "rts                    \n"      // jumps to old TrapCode

      "endtrap:               \n"
      "addq    #4,sp          \n"      // remove exception number from SSP
      "rte                    \n"      // return from exception

      "addr_err:              \n"
      "bus_err:               \n"
      "add.l   #1,_mem_fault_count \n" // increment fault count
      "addq.l  #6,sp               \n" // remove exception number from SSP
      "addq.l  #2,(sp)             \n" // Skip bad instruction
      "subq.l  #2,sp               \n"
      "addq    #4,sp               \n" // remove exception number from SSP
      "rte                         \n" // return from exception

      "ill_inst:                   \n"
      "add.l   #1,_mem_fault_count");  // increment fault count

__asm("addq.l  #2,6(sp)            \n" // Skip bad instruction
      "addq.l  #4,sp");                // Remove exception number from SSP

#if 0
     /* Alternative to the above */
__asm("subq.l  #2,sp               \n" // remove exception number from SSP
      "addq.l  #2,8(sp)            \n" // Skip bad instruction
      "addq.l  #6,sp");
#endif
__asm("rte                         \n" // return from exception

      "div0_err:                   \n"
      "add.l   #1,_mem_fault_count \n" // increment fault count
      "addq    #4,sp               \n" // remove exception number from SSP
      "rte");
#endif /* AMIGAOS && !_DCC */
