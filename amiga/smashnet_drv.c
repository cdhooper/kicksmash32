/*
 * smashnet_drv.c -- disk-loadable smashnet.device (copy to DEVS:).
 *
 * This is the driver front end for the SANA-II device implemented in
 * smashnet.c. It must be built with -DSMASHNET_DEVICE, without startup
 * code (see the Makefile). It provides:
 *
 *   - The entry stub and ROMTag structure which is used by OpenDevice()
 *     to load a device driver. The ROMTag structure includes a pointer
 *     to a driver init function and standard device driver vectors
 *     (open, close, expungs, rsvd, begin_io, abort_io).
 *   - the few C runtime pieces the shared code needs, since there is no
 *     startup code: SysBase, DOSBase, malloc(), free(), atexit()
 *
 * Unlike the smashnet program, there is no owner task: client tasks call
 * the device directly, and a private receive task (started at init,
 * stopped at Expunge) polls Kicksmash while the device is open.
 */

#ifndef SMASHNET_DEVICE
#error "smashnet_drv.c must be built with -DSMASHNET_DEVICE"
#endif

#include <stdlib.h>
#include <exec/resident.h>
#include "smashnet.h"

/*
 * Optional init tracing (-DSMASHNET_DRV_TRACE): one raw character per stage
 * on the serial port via exec RawPutChar(), usable in any context, to find
 * where initialization stops. A..E are the drv_init stages.
 */
#ifdef SMASHNET_DRV_TRACE
#define DPUTCHAR(c) RawPutChar(c)
#else
#define DPUTCHAR(c) do { } while (0)
#endif

/*
 * Runtime pieces normally provided by startup code / libc
 */
struct ExecBase   *SysBase;
struct DosLibrary *DOSBase;  // Never opened: printf() must not use dos.library

void *
malloc(size_t size)
{
    return (AllocVec(size, MEMF_PUBLIC));
}

void
free(void *ptr)
{
    if (ptr != NULL) {
        FreeVec(ptr);
    }
}

/*
 * sm_msg.c registers host_msg_exit() with atexit(). A device never
 * "exits"; the receive task calls host_msg_exit() itself at Expunge.
 */
int
atexit(void (*func)(void))
{
    (void) func;
    return (0);
}

/*
 * drv_init
 * --------
 * Called by Exec (RTF_AUTOINIT, via MakeLibrary) with d0 = device base and
 * a0 = segment list. Returns the device base in d0 on success, in which
 * case Exec adds the device; 0 if initialization failed.
 *
 * SysBase is taken from location 4 rather than a register parameter, and
 * the result is returned as a ULONG so that it is unambiguously in d0
 * (GCC may return pointers in a0).
 */
static ULONG
drv_init(register struct SmashNetDevice *device __asm("d0"),
         register BPTR seglist __asm("a0"))
{
    struct SmashNetContext *context;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds="
    SysBase = *(struct ExecBase **)4UL;
#pragma GCC diagnostic pop
    DPUTCHAR('A');
    cpu_control_init();  /* cpu_type */

    context = smashnet_context_alloc();
    if (context == NULL) {
        goto fail;
    }
    DPUTCHAR('B');
    context->rx_drains_debug = TRUE;  /* No owner task to print records */

    smashnet_device_setup(device, context);
    device->seglist = seglist;
    context->device = device;
    DPUTCHAR('C');

    if (!smashnet_rx_start(context, SMASHNET_RX_PRIORITY)) {
        context->device = NULL;
        smashnet_context_free(context);
        goto fail;
    }
    DPUTCHAR('D');
    return ((ULONG) device);

fail:
    DPUTCHAR('F');
    /* Exec does not free the device base when init fails */
    smashnet_free_device_memory(device);
    return (0);
}

static const char drv_name[] = SMASHNET_NAME;
static const char drv_id[]   = SMASHNET_IDSTRING;

/* Exec MakeLibrary() parameters: size, vectors, InitStruct table, init */
static const APTR drv_init_table[4] = {
    (APTR) sizeof (struct SmashNetDevice),
    (APTR) smashnet_device_vectors,
    (APTR) NULL,
    (APTR) drv_init
};

/*
 * Entry stub and ROMTag as a single object in .text, so the layout does not
 * depend on how GCC orders things: if this object lands first in the hunk,
 * running the file executes "moveq #-1,d0 / rts" (never the ROMTag's
 * RTC_MATCHWORD, which is the ILLEGAL opcode); if something else is first,
 * that is ordinary code. ramlib finds the ROMTag by scanning for the match
 * word, which must be word aligned.
 */
static const struct {
    UWORD           code[2];  /* 0x70ff moveq #-1,d0; 0x4e75 rts */
    struct Resident rt;
} drv_entry __attribute__((used, section(".text"))) = {
    { 0x70ff, 0x4e75 },
    {
        RTC_MATCHWORD,
        (struct Resident *) &drv_entry.rt,
        (APTR) (&drv_entry.rt + 1),
        RTF_AUTOINIT,
        SMASHNET_VERSION,
        NT_DEVICE,
        0,
        (char *) drv_name,
        (char *) drv_id,
        (APTR) drv_init_table
    }
};
