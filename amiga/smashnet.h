/*
 * smashnet.h -- definitions shared by the three smashnet source files.
 *
 *   smashnet.c      SANA-II device implementation common to both builds
 *   smashnet_prg.c  command-line program: installs a transient device
 *   smashnet_drv.c  disk-loadable smashnet.device (ROMTag, DEVS: driver)
 *
 * The program is built normally.  The driver is built from the same
 * sources with -DSMASHNET_DEVICE.
 */

#ifndef _SMASHNET_H
#define _SMASHNET_H

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <exec/devices.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <exec/tasks.h>
#include <exec/memory.h>
#include <exec/errors.h>
#include <devices/timer.h>
#include <devices/newstyle.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <clib/alib_protos.h>

#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include "smash_cmd.h"
#include "host_cmd.h"
#include "sm_msg.h"
#include "sm_net.h"
#include "cpu_control.h"
#include "sana2.h"

#define SMASHNET_NAME            "smashnet.device"
#define SMASHNET_VERSION         1
#define SMASHNET_REVISION        0
#define SMASHNET_IDSTRING        "smashnet.device 1.0 (06.08.2026)\r\n"
#define SMASHNET_UNIT            0
#define SMASHNET_MTU             1500UL
#define SMASHNET_RAW_MTU         1514UL
#define SMASHNET_BPS             10000000UL
#define SMASHNET_MAX_OPENERS     16
#define SMASHNET_DEBUG_SLOTS     128
#define SMASHNET_POLL_US         20000UL
#define SMASHNET_TASK_STACK      16384UL

/* Signals delivered to the receive task */
#define SMASHNET_RX_SIGNAL       SIGBREAKF_CTRL_F  /* Stop and exit */
#define SMASHNET_RX_WAKE         SIGBREAKF_CTRL_E  /* Re-evaluate state */

/*
 * Build-specific behavior
 *
 * SMASHNET_POLL_IDLE      1: poll Kicksmash even when nobody has the device
 *                            open (program).
 *                         0: poll only while the device is open (driver).
 * SMASHNET_RX_PRIORITY    Priority of the driver's receive task.
 * SMASHNET_FLAG_OUTPUT     Initial flag_output for the driver. printf() in
 *                         the driver must never use the calling task's
 *                         Output(), so it is 0 (silent) or 2 (serial port;
 *                         enable with -DSMASHNET_DRV_SERIAL_DEBUG).
 */
#ifdef SMASHNET_DEVICE
#define SMASHNET_POLL_IDLE       0
#ifndef SMASHNET_RX_PRIORITY
#define SMASHNET_RX_PRIORITY     0
#endif
#ifdef SMASHNET_DRV_SERIAL_DEBUG
#define SMASHNET_FLAG_OUTPUT     2
#else
#define SMASHNET_FLAG_OUTPUT     0
#endif
#else
#define SMASHNET_POLL_IDLE       1
#define SMASHNET_FLAG_OUTPUT     1
#endif

#ifndef SANA2IOF_RAW
#define SANA2IOF_RAW             (1UL << 7)
#endif

#ifndef SANA2IOF_BCAST
#define SANA2IOF_BCAST           (1UL << 6)
#endif

#ifndef SANA2IOF_MCAST
#define SANA2IOF_MCAST           (1UL << 5)
#endif

#ifndef S2_DEVICEQUERY
#error "devices/sana2.h is required"
#endif

struct SmashNetDevice;

struct SmashNetDebugRecord {
    const char *function_name;
    struct IOSana2Req *request;
    ULONG command;
    ULONG io_flags;
    ULONG packet_type;
    ULONG data_length;
    APTR data;
    APTR stat_data;
    APTR buffer_management;
    ULONG unit_number;
    ULONG open_flags;
    ULONG open_count;
    BYTE io_error;
    ULONG wire_error;
    UBYTE source[6];
    UBYTE destination[6];
};

struct SmashNetContext {
    struct Task *owner_task;        /* Program: main task. Driver: NULL */
    volatile struct SmashNetDevice *device;
    volatile struct Task *rx_task;
    volatile LONG rx_start_error;
    volatile BOOL shutting_down;
    BOOL rx_drains_debug;           /* rx task prints queued debug records */
    struct Task *rx_waiter;         /* Task waiting on rx start/stop */
    ULONG rx_wait_mask;             /* Signal mask that task waits on */
    BYTE log_signal_bit;            /* Program only */
    BYTE state_signal_bit;          /* Program only */
    volatile UWORD debug_read;
    volatile UWORD debug_write;
    volatile ULONG debug_dropped;
    struct SmashNetDebugRecord debug[SMASHNET_DEBUG_SLOTS];
};

struct SmashNetOpener {
    BOOL in_use;
    struct IORequest *open_request;
    ULONG open_flags;
    APTR copy_to_buff;
    APTR copy_from_buff;
    APTR packet_filter;
};

struct SmashNetDevice {
    struct Library library;
    struct SmashNetContext *context;
    BPTR seglist;                   /* Driver: returned by Expunge */
    struct Unit unit;
    struct List read_queue;
    struct List orphan_queue;
    struct List event_queue;
    BOOL configured;
    BOOL online;
    BOOL promiscuous;
    BOOL exclusive;
    UBYTE station_address[6];
    struct SmashNetOpener openers[SMASHNET_MAX_OPENERS];
};

extern struct ExecBase *SysBase;

extern uint    flag_debug;
extern uint8_t flag_output;
extern uint    sm_net_open;   /* 0=not open, 1=open, 2=open failed */

/* Exec device vector table (Open, Close, Expunge, Reserved, BeginIO, Abort) */
extern const ULONG smashnet_device_vectors[];

/* Common code: smashnet.c */
struct SmashNetContext *smashnet_context_alloc(void);
void   smashnet_context_free(struct SmashNetContext *context);
void   smashnet_fetch_mac(struct SmashNetDevice *device);
void   smashnet_device_setup(struct SmashNetDevice *device,
                             struct SmashNetContext *context);
void   smashnet_free_device_memory(struct SmashNetDevice *device);
BOOL   smashnet_rx_start(struct SmashNetContext *context, BYTE priority);
void   smashnet_rx_stop(struct SmashNetContext *context);
void   smashnet_drain_debug(struct SmashNetContext *context);
ULONG  smashnet_signal_mask(BYTE bit);
ULONG  smashnet_current_open_count(struct SmashNetContext *context);
void   smashnet_begin_shutdown(struct SmashNetContext *context);

#endif /* _SMASHNET_H */
