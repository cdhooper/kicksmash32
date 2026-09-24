/*
 * smashnet_prg.c -- command-line program which hosts smashnet.device.
 *
 * The executable installs a transient Exec device named "smashnet.device".
 * It remains resident only while this process is running.  Device entry
 * points enqueue debug records; the CLI owner task prints them, avoiding
 * stdio calls from arbitrary client tasks.
 *
 * The device itself lives in smashnet.c; smashnet_drv.c is the alternative
 * front end which builds it as a disk-loadable driver.
 *
 * Copyright: public-domain-style example; use at your own risk.
 */

#include <intuition/intuition.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/dos.h>
#include <proto/intuition.h>

#include "smashnet.h"

#ifdef SMASHNET_DEVICE
#error "Do not build smashnet_prg.c with -DSMASHNET_DEVICE"
#endif

extern struct DosLibrary *DOSBase;
extern struct IntuitionBase *IntuitionBase;

static BOOL
allocate_signals(struct SmashNetContext *context)
{
    context->log_signal_bit = AllocSignal(-1);
    context->state_signal_bit = AllocSignal(-1);

    if (context->log_signal_bit < 0 || context->state_signal_bit < 0) {
        return (FALSE);
    }
    return (TRUE);
}

static void
free_signals(struct SmashNetContext *context)
{
    if (context->log_signal_bit >= 0) {
        FreeSignal(context->log_signal_bit);
    }
    if (context->state_signal_bit >= 0) {
        FreeSignal(context->state_signal_bit);
    }
}


static BOOL
device_already_present(void)
{
    struct Node *node;
    BOOL found = FALSE;

    Forbid();
    node = SysBase->DeviceList.lh_Head;
    while (node->ln_Succ != NULL) {
        const char *name = node->ln_Name;
        if ((name != NULL) && strcmp(name, SMASHNET_NAME) == 0) {
            found = TRUE;
            break;
        }
        node = node->ln_Succ;
    }
    Permit();
    return (found);
}


/*
 * create_device
 * -------------
 * Allocate the device with MakeLibrary() and initialize it.  The device is
 * added to the system by main().  (The driver instead lets Exec allocate
 * it from the RTF_AUTOINIT table; see smashnet_drv.c.)
 */
static struct SmashNetDevice *
create_device(struct SmashNetContext *context)
{
    struct SmashNetDevice *device;

    device = (struct SmashNetDevice *) MakeLibrary(
                        (APTR) smashnet_device_vectors, NULL, NULL,
                        sizeof (*device), 0);
    if (device == NULL)
        return (NULL);
    smashnet_device_setup(device, context);
    device->seglist = 0;  /* Code belongs to the program, not to DOS */
    return (device);
}

static BOOL
ask_to_wait_for_close(ULONG open_count)
{
    struct EasyStruct easy;
    ULONG arguments[1];

    if (IntuitionBase == NULL) {
        printf("smashnet.device has %lu open client(s). Shut down the network "
               "interface, then press Ctrl-C again after it closes.\n",
               open_count);
        return (FALSE);
    }

    easy.es_StructSize = sizeof (easy);
    easy.es_Flags = 0;
    easy.es_Title = (STRPTR)SMASHNET_NAME;
    easy.es_TextFormat = (STRPTR)
        "smashnet.device still has %ld open client(s).\n\n"
        "Shut down the network interface, then choose Wait for close.\n";
    easy.es_GadgetFormat = (STRPTR)"Wait for close|Keep running";
    arguments[0] = open_count;
    return (EasyRequestArgs(NULL, &easy, NULL, (APTR)arguments) != 0);
}

static void
wait_for_open_count_zero(struct SmashNetContext *context)
{
    ULONG mask = smashnet_signal_mask(context->log_signal_bit) |
                 smashnet_signal_mask(context->state_signal_bit);

    while ((context->device != NULL) &&
           smashnet_current_open_count(context) != 0) {
        smashnet_drain_debug(context);
        Wait(mask);
    }
    smashnet_drain_debug(context);
}


static void
remove_installed_device(struct SmashNetContext *context)
{
    struct SmashNetDevice *device;

    Forbid();
    device = (struct SmashNetDevice *)context->device;
    Permit();
    if (device != NULL) {
        RemDevice((struct Device *)device);
    }
}

static void
wait_for_device_removed(struct SmashNetContext *context)
{
    ULONG mask = smashnet_signal_mask(context->log_signal_bit) |
                 smashnet_signal_mask(context->state_signal_bit);

    while (context->device != NULL) {
        ULONG signals = Wait(mask);
        if ((signals & smashnet_signal_mask(context->log_signal_bit)) != 0) {
            smashnet_drain_debug(context);
        }
    }

    /*
     * RemDevice() calls the device's Expunge vector directly and
     * synchronously -- it does not defer this to some later point.
     * That means device_expunge() may already have run (and already
     * set context->device = NULL and Signal()'d log_signal_bit /
     * state_signal_bit) before this function was ever called, in
     * which case the Wait() loop above never executes and those
     * signals are never consumed. Freeing a signal bit while it is
     * still pending is not safe, so explicitly clear both bits here
     * before free_signals() releases them.
     */
    SetSignal(0, mask);
    smashnet_drain_debug(context);
}

static int
smash_message_init(void)
{
    cpu_control_init();  // cpu_type, SysBase
    if (sm_nservice() == 0) {
        printf("Network service not up\n");
    }
    return (0);
}


int
main(void)
{
    struct SmashNetContext *context = NULL;
    struct SmashNetDevice *device = NULL;
    ULONG wait_mask;
    ULONG signals;
    ULONG open_count;
    BYTE receive_priority;
    BOOL exit_requested = FALSE;
    BOOL device_installed = FALSE;
    int result = RETURN_FAIL;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds="
    SysBase = *(struct ExecBase **)4UL;
#pragma GCC diagnostic pop
    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 36);
    if (DOSBase == NULL) {
        return (RETURN_FAIL);
    }
    if (smash_message_init())
        goto cleanup;

    IntuitionBase = (struct IntuitionBase *)
                    OpenLibrary("intuition.library", 36);
    if (IntuitionBase == NULL) {
        printf("Unable to open intuition.library V36.\n");
        goto cleanup;
    }

    if (device_already_present()) {
        printf("%s is already installed.\n", SMASHNET_NAME);
        goto cleanup;
    }

    context = smashnet_context_alloc();
    if (context == NULL) {
        printf("Unable to allocate smashnet context.\n");
        goto cleanup;
    }
    context->owner_task = FindTask(NULL);

    if (!allocate_signals(context)) {
        printf("Unable to allocate Exec signals.\n");
        goto cleanup;
    }

    receive_priority = context->owner_task->tc_Node.ln_Pri;
    if (receive_priority > -128) {
        --receive_priority;
    }
    if (!smashnet_rx_start(context, receive_priority)) {
        printf("Unable to start receive task (memory or timer.device).\n");
        goto cleanup;
    }

    device = create_device(context);
    if (device == NULL) {
        printf("MakeLibrary() could not allocate the device.\n");
        goto cleanup;
    }
    context->device = device;
    AddDevice((struct Device *)device);
    device_installed = TRUE;

    printf("%s installed as unit 0. Press Ctrl-C to shut down.\n",
           SMASHNET_NAME);
    fflush(stdout);

    wait_mask = SIGBREAKF_CTRL_C |
                smashnet_signal_mask(context->log_signal_bit) |
                smashnet_signal_mask(context->state_signal_bit);

    while (!exit_requested) {
        signals = Wait(wait_mask);
        if ((signals & smashnet_signal_mask(context->log_signal_bit)) != 0) {
            smashnet_drain_debug(context);
        }
        if (context->device == NULL) {
            printf("%s was removed externally; shutting down host task.\n",
                   SMASHNET_NAME);
            exit_requested = TRUE;
        }
        if ((signals & SIGBREAKF_CTRL_C) != 0) {
            open_count = smashnet_current_open_count(context);
            if (open_count == 0 || ask_to_wait_for_close(open_count)) {
                smashnet_begin_shutdown(context);
                exit_requested = TRUE;
            }
        }
    }

    open_count = smashnet_current_open_count(context);
    if (open_count != 0) {
        printf("Waiting for %lu open client(s) to close...\n", open_count);
        wait_for_open_count_zero(context);
    }

    result = RETURN_OK;

cleanup:
    if (context != NULL) {
        /* Stop the receive task before closing the remote network */
        smashnet_begin_shutdown(context);
        smashnet_rx_stop(context);
    }
    if (sm_net_open) {
        sm_nclose();
    }
    if (context != NULL) {
        if (context->device != NULL) {
            if (smashnet_current_open_count(context) != 0) {
                /*
                 * Initialization failures can still race with a client
                 * that opened the device immediately after AddDevice().
                 */
                wait_for_open_count_zero(context);
            }
            remove_installed_device(context);
            wait_for_device_removed(context);
        } else if (device != NULL && !device_installed) {
            /* Device was allocated but never added. */
            smashnet_free_device_memory(device);
            device = NULL;
        }
        smashnet_drain_debug(context);

        /*
         * Belt-and-suspenders: make sure none of our signal bits are
         * left pending before free_signals() releases them back to
         * the task's pool. Freeing a signal bit while it is still
         * set is not safe (see wait_for_device_removed()).
         */
        SetSignal(0, smashnet_signal_mask(context->log_signal_bit) |
                     smashnet_signal_mask(context->state_signal_bit));

        free_signals(context);
        smashnet_context_free(context);
    }

    if (IntuitionBase != NULL) {
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = NULL;
    }
    if (DOSBase != NULL) {
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
    }
    return (result);
}
