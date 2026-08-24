/*
 * smashnet.device -- command-line hosted SANA-II device skeleton for AmigaOS.
 *
 * The executable installs a transient Exec device named "smashnet.device".
 * It remains resident only while this process is running.  Device entry points
 * enqueue debug records; the CLI owner task prints them, avoiding stdio calls
 * from arbitrary client tasks.
 *
 * Copyright: public-domain-style example; use at your own risk.
 */

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
// #include <devices/sana2.h>
#include <devices/newstyle.h>
#include <utility/tagitem.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/dos.h>
#include <proto/intuition.h>
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

// #include "smashnet_backend.h"

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
#define SMASHNET_TASK_STACK      8192UL
#define SMASHNET_RX_SIGNAL       SIGBREAKF_CTRL_F

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
    struct Task *owner_task;
    volatile struct SmashNetDevice *device;
    volatile struct Task *rx_task;
    volatile LONG rx_start_error;
    volatile BOOL shutting_down;
    BYTE log_signal_bit;
    BYTE state_signal_bit;
    BYTE rx_ready_signal_bit;
    BYTE rx_done_signal_bit;
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
extern struct DosLibrary *DOSBase;
extern struct IntuitionBase *IntuitionBase;

static struct SmashNetContext *g_context;

static BOOL smashnet_call_copy(APTR callback, APTR to, APTR from, ULONG length);

static LONG device_open(register struct SmashNetDevice *device __asm("a6"),
                        register struct IOSana2Req *request __asm("a1"),
                        register ULONG unit_number __asm("d0"),
                        register ULONG flags __asm("d1"));
static BPTR device_close(register struct SmashNetDevice *device __asm("a6"),
                         register struct IOSana2Req *request __asm("a1"));
static BPTR device_expunge(register struct SmashNetDevice *device __asm("a6"));
static ULONG device_reserved(void);
static void device_begin_io(register struct SmashNetDevice *device __asm("a6"),
                            register struct IOSana2Req *request __asm("a1"));
static LONG device_abort_io(register struct SmashNetDevice *device __asm("a6"),
                            register struct IOSana2Req *request __asm("a1"));

static const ULONG device_vectors[] = {
    (ULONG)device_open,
    (ULONG)device_close,
    (ULONG)device_expunge,
    (ULONG)device_reserved,
    (ULONG)device_begin_io,
    (ULONG)device_abort_io,
    (ULONG)-1
};

static const UWORD supported_commands[] = {
    CMD_RESET, CMD_READ, CMD_WRITE, CMD_UPDATE, CMD_CLEAR, CMD_STOP,
    CMD_START, CMD_FLUSH,
    S2_DEVICEQUERY, S2_GETSTATIONADDRESS, S2_CONFIGINTERFACE,
    S2_ADDMULTICASTADDRESS, S2_DELMULTICASTADDRESS, S2_MULTICAST,
    S2_BROADCAST, S2_TRACKTYPE, S2_UNTRACKTYPE, S2_GETTYPESTATS,
    S2_GETSPECIALSTATS, S2_GETGLOBALSTATS, S2_ONEVENT, S2_READORPHAN,
    S2_ONLINE, S2_OFFLINE,
#ifdef S2_ADDMULTICASTADDRESSES
    S2_ADDMULTICASTADDRESSES,
#endif
#ifdef S2_DELMULTICASTADDRESSES
    S2_DELMULTICASTADDRESSES,
#endif
    NSCMD_DEVICEQUERY,
    0
};

uint        flag_debug  = 0;
uint8_t     flag_output = 1;
static uint sm_net_open = 0;

static ULONG
signal_mask(BYTE bit)
{
    return (bit >= 0 ? (1UL << bit) : 0);
}

static void
copy_mac(UBYTE *to, const UBYTE *from)
{
    ULONG index;
    for (index = 0; index < 6; ++index) {
        to[index] = from[index];
    }
}

static BOOL
mac_is_zero(const UBYTE *address)
{
    ULONG index;
    for (index = 0; index < 6; ++index) {
        if (address[index] != 0) {
            return (FALSE);
        }
    }
    return (TRUE);
}

static BOOL
mac_equal(const UBYTE *left, const UBYTE *right)
{
    ULONG index;
    for (index = 0; index < 6; ++index) {
        if (left[index] != right[index]) {
            return (FALSE);
        }
    }
    return (TRUE);
}

static BOOL
mac_is_broadcast(const UBYTE *address)
{
    ULONG index;
    for (index = 0; index < 6; ++index) {
        if (address[index] != 0xff) {
            return (FALSE);
        }
    }
    return (TRUE);
}

static void
request_set_error(struct IOSana2Req *request, BYTE io_error, ULONG wire_error)
{
    request->ios2_Req.io_Error = io_error;
    request->ios2_WireError = wire_error;
}

static void
complete_request(struct IOSana2Req *request)
{
    if ((request->ios2_Req.io_Flags & IOF_QUICK) == 0) {
        ReplyMsg(&request->ios2_Req.io_Message);
    }
}

static void
fill_debug_record(struct SmashNetDebugRecord *record, const char *function_name,
                  struct IOSana2Req *request, ULONG unit_number,
                  ULONG open_flags, ULONG open_count)
{
    ULONG index;

    record->function_name = function_name;
    record->request = request;
    record->unit_number = unit_number;
    record->open_flags = open_flags;
    record->open_count = open_count;
    record->command = 0;
    record->io_flags = 0;
    record->packet_type = 0;
    record->data_length = 0;
    record->data = NULL;
    record->stat_data = NULL;
    record->buffer_management = NULL;
    record->io_error = 0;
    record->wire_error = 0;

    for (index = 0; index < 6; ++index) {
        record->source[index] = 0;
        record->destination[index] = 0;
    }

    if (request != NULL) {
        record->command = request->ios2_Req.io_Command;
        record->io_flags = request->ios2_Req.io_Flags;
        record->io_error = request->ios2_Req.io_Error;
        if (request->ios2_Req.io_Message.mn_Length >=
            sizeof(struct IOSana2Req)) {
            record->packet_type = request->ios2_PacketType;
            record->data_length = request->ios2_DataLength;
            record->data = request->ios2_Data;
            record->stat_data = request->ios2_StatData;
            record->buffer_management = request->ios2_BufferManagement;
            record->wire_error = request->ios2_WireError;
            copy_mac(record->source, request->ios2_SrcAddr);
            copy_mac(record->destination, request->ios2_DstAddr);
        } else if (request->ios2_Req.io_Message.mn_Length >=
                   sizeof(struct IOStdReq)) {
            struct IOStdReq *standard = (struct IOStdReq *)request;
            record->data_length = standard->io_Length;
            record->data = standard->io_Data;
        }
    }
}

static void
queue_debug(struct SmashNetContext *context, const char *function_name,
            struct IOSana2Req *request, ULONG unit_number,
            ULONG open_flags, ULONG open_count)
{
    UWORD write_index;
    UWORD next_index;

    if (context == NULL) {
        return;
    }

    Forbid();
    write_index = context->debug_write;
    next_index = (UWORD)((write_index + 1) % SMASHNET_DEBUG_SLOTS);
    if (next_index == context->debug_read) {
        ++context->debug_dropped;
    } else {
        fill_debug_record(&context->debug[write_index], function_name, request,
                          unit_number, open_flags, open_count);
        context->debug_write = next_index;
    }
    Permit();

    if (context->owner_task != NULL && context->log_signal_bit >= 0) {
        Signal(context->owner_task, signal_mask(context->log_signal_bit));
    }
}

static BOOL
pop_debug(struct SmashNetContext *context, struct SmashNetDebugRecord *record)
{
    BOOL available = FALSE;

    Forbid();
    if (context->debug_read != context->debug_write) {
        *record = context->debug[context->debug_read];
        context->debug_read = (UWORD)((context->debug_read + 1) %
                                      SMASHNET_DEBUG_SLOTS);
        available = TRUE;
    }
    Permit();
    return (available);
}

static const char *
command_name(ULONG command)
{
    switch (command) {
        case CMD_INVALID: return ("CMD_INVALID");
        case CMD_RESET: return ("CMD_RESET");
        case CMD_READ: return ("CMD_READ");
        case CMD_WRITE: return ("CMD_WRITE");
        case CMD_UPDATE: return ("CMD_UPDATE");
        case CMD_CLEAR: return ("CMD_CLEAR");
        case CMD_STOP: return ("CMD_STOP");
        case CMD_START: return ("CMD_START");
        case CMD_FLUSH: return ("CMD_FLUSH");
        case S2_DEVICEQUERY: return ("S2_DEVICEQUERY");
        case S2_GETSTATIONADDRESS: return ("S2_GETSTATIONADDRESS");
        case S2_CONFIGINTERFACE: return ("S2_CONFIGINTERFACE");
        case S2_ADDMULTICASTADDRESS: return ("S2_ADDMULTICASTADDRESS");
        case S2_DELMULTICASTADDRESS: return ("S2_DELMULTICASTADDRESS");
        case S2_MULTICAST: return ("S2_MULTICAST");
        case S2_BROADCAST: return ("S2_BROADCAST");
        case S2_TRACKTYPE: return ("S2_TRACKTYPE");
        case S2_UNTRACKTYPE: return ("S2_UNTRACKTYPE");
        case S2_GETTYPESTATS: return ("S2_GETTYPESTATS");
        case S2_GETSPECIALSTATS: return ("S2_GETSPECIALSTATS");
        case S2_GETGLOBALSTATS: return ("S2_GETGLOBALSTATS");
        case S2_ONEVENT: return ("S2_ONEVENT");
        case S2_READORPHAN: return ("S2_READORPHAN");
        case S2_ONLINE: return ("S2_ONLINE");
        case S2_OFFLINE: return ("S2_OFFLINE");
#ifdef S2_ADDMULTICASTADDRESSES
        case S2_ADDMULTICASTADDRESSES: return ("S2_ADDMULTICASTADDRESSES");
#endif
#ifdef S2_DELMULTICASTADDRESSES
        case S2_DELMULTICASTADDRESSES: return ("S2_DELMULTICASTADDRESSES");
#endif
#ifdef S2_GETPEERADDRESS
        case S2_GETPEERADDRESS: return ("S2_GETPEERADDRESS");
#endif
#ifdef S2_GETDNSADDRESS
        case S2_GETDNSADDRESS: return ("S2_GETDNSADDRESS");
#endif
#ifdef S2_GETEXTENDEDGLOBALSTATS
        case S2_GETEXTENDEDGLOBALSTATS: return ("S2_GETEXTENDEDGLOBALSTATS");
#endif
#ifdef S2_CONNECT
        case S2_CONNECT: return ("S2_CONNECT");
#endif
#ifdef S2_DISCONNECT
        case S2_DISCONNECT: return ("S2_DISCONNECT");
#endif
#ifdef S2_SAMPLE_THROUGHPUT
        case S2_SAMPLE_THROUGHPUT: return ("S2_SAMPLE_THROUGHPUT");
#endif
#ifdef S2_SANA2HOOK
        case S2_SANA2HOOK: return ("S2_SANA2HOOK");
#endif
        case NSCMD_DEVICEQUERY: return ("NSCMD_DEVICEQUERY");
        default: return ("UNKNOWN");
    }
}

static void
print_mac(const UBYTE *address)
{
    printf("%02x:%02x:%02x:%02x:%02x:%02x",
           address[0], address[1], address[2],
           address[3], address[4], address[5]);
}

static void
drain_debug(struct SmashNetContext *context)
{
    struct SmashNetDebugRecord record;
    ULONG dropped;

    while (pop_debug(context, &record)) {
        printf("DBG %s", record.function_name);
        if (record.request != NULL) {
            printf(" req=%p cmd=%s(%lu) flags=$%02lx"
                   " type=$%08lx len=%lu data=%p stat=%p bm=%p",
                   (void *)record.request,
                   command_name(record.command), record.command,
                   record.io_flags,
                   record.packet_type, record.data_length,
                   record.data, record.stat_data, record.buffer_management);
            printf(" src=");
            print_mac(record.source);
            printf(" dst=");
            print_mac(record.destination);
            printf(" ioErr=%ld wireErr=%lu",
                   (LONG)record.io_error, record.wire_error);
        }
        if (record.unit_number != (ULONG)-1) {
            printf(" unit=%lu openFlags=$%08lx", record.unit_number,
                   record.open_flags);
            printf(" openCount=%lu", record.open_count);
        }
        printf("\n");
    }

    Forbid();
    dropped = context->debug_dropped;
    context->debug_dropped = 0;
    Permit();
    if (dropped != 0) {
        printf("DBG %lu debug record(s) dropped\n", dropped);
    }
    fflush(stdout);
}

static struct SmashNetOpener *
find_free_opener(struct SmashNetDevice *device)
{
    ULONG index;
    for (index = 0; index < SMASHNET_MAX_OPENERS; ++index) {
        if (!device->openers[index].in_use) {
            return (&device->openers[index]);
        }
    }
    return (NULL);
}

static struct SmashNetOpener *
request_opener(struct IOSana2Req *request)
{
    return ((struct SmashNetOpener *)request->ios2_BufferManagement);
}

static struct SmashNetOpener *
find_cookie_opener(struct SmashNetDevice *device, struct IOSana2Req *request)
{
    APTR cookie;
    ULONG index;

    if (request == NULL ||
        request->ios2_Req.io_Message.mn_Length < sizeof(struct IOSana2Req)) {
        return (NULL);
    }

    cookie = request->ios2_BufferManagement;
    for (index = 0; index < SMASHNET_MAX_OPENERS; ++index) {
        if (cookie == (APTR)&device->openers[index] &&
            device->openers[index].in_use) {
            return (&device->openers[index]);
        }
    }
    return (NULL);
}

static struct SmashNetOpener *
find_request_opener(struct SmashNetDevice *device, struct IOSana2Req *request)
{
    struct SmashNetOpener *opener;
    ULONG index;

    opener = find_cookie_opener(device, request);
    if (opener != NULL) {
        return (opener);
    }

    for (index = 0; index < SMASHNET_MAX_OPENERS; ++index) {
        if (device->openers[index].in_use &&
            device->openers[index].open_request ==
                (struct IORequest *)request) {
            return (&device->openers[index]);
        }
    }
    return (NULL);
}

static void
read_buffer_tags(struct SmashNetOpener *opener, struct TagItem *tags)
{
    struct TagItem *tag = tags;

    opener->copy_to_buff = NULL;
    opener->copy_from_buff = NULL;
    opener->packet_filter = NULL;

    while (tag != NULL) {
        switch (tag->ti_Tag) {
            case TAG_DONE:
                return;
            case TAG_IGNORE:
                ++tag;
                break;
            case TAG_MORE:
                tag = (struct TagItem *)tag->ti_Data;
                break;
            case TAG_SKIP:
                tag += tag->ti_Data + 1;
                break;
            case S2_CopyToBuff:
                opener->copy_to_buff = (APTR)tag->ti_Data;
                ++tag;
                break;
            case S2_CopyFromBuff:
                opener->copy_from_buff = (APTR)tag->ti_Data;
                ++tag;
                break;
#ifdef S2_CopyToBuff16
            case S2_CopyToBuff16:
#endif
#ifdef S2_CopyToBuff32
            case S2_CopyToBuff32:
#endif
#ifdef S2_CopyFromBuff16
            case S2_CopyFromBuff16:
#endif
#ifdef S2_CopyFromBuff32
            case S2_CopyFromBuff32:
#endif
                /* Optional aligned callbacks are advisory.  This skeleton uses
                 * only the mandatory byte-oriented callbacks. */
                ++tag;
                break;
            case S2_PacketFilter:
                opener->packet_filter = (APTR)tag->ti_Data;
                ++tag;
                break;
            default:
                ++tag;
                break;
        }
    }
}

static BOOL
remove_request_from_queue(struct List *queue, struct IOSana2Req *request)
{
    struct Node *node = queue->lh_Head;

    while (node->ln_Succ != NULL) {
        if (node == &request->ios2_Req.io_Message.mn_Node) {
            Remove(node);
            return (TRUE);
        }
        node = node->ln_Succ;
    }
    return (FALSE);
}

static BOOL
remove_request_from_all_queues(struct SmashNetDevice *device,
                               struct IOSana2Req *request)
{
    if (remove_request_from_queue(&device->read_queue, request)) {
        return (TRUE);
    }
    if (remove_request_from_queue(&device->orphan_queue, request)) {
        return (TRUE);
    }
    return (remove_request_from_queue(&device->event_queue, request));
}


static void enqueue_request(struct List *queue, struct IOSana2Req *request)
{
    Forbid();
    request->ios2_Req.io_Flags &= (UBYTE)~IOF_QUICK;
    AddTail(queue, &request->ios2_Req.io_Message.mn_Node);
    Permit();
}

static void
abort_queued_for_opener(struct SmashNetDevice *device, struct List *queue,
                        struct SmashNetOpener *opener, BYTE error,
                        ULONG wire_error)
{
    struct Node *node;
    struct Node *next;
    struct IOSana2Req *request;

    Forbid();
    node = queue->lh_Head;
    while (node->ln_Succ != NULL) {
        next = node->ln_Succ;
        request = (struct IOSana2Req *)node;
        if (opener == NULL || request_opener(request) == opener) {
            Remove(node);
            request_set_error(request, error, wire_error);
            ReplyMsg(&request->ios2_Req.io_Message);
        }
        node = next;
    }
    Permit();

    (void)device;
}

static void
abort_all_for_opener(struct SmashNetDevice *device,
                     struct SmashNetOpener *opener)
{
    abort_queued_for_opener(device, &device->read_queue, opener,
                            IOERR_ABORTED, S2WERR_GENERIC_ERROR);
    abort_queued_for_opener(device, &device->orphan_queue, opener,
                            IOERR_ABORTED, S2WERR_GENERIC_ERROR);
    abort_queued_for_opener(device, &device->event_queue, opener,
                            IOERR_ABORTED, S2WERR_GENERIC_ERROR);
}

static void
abort_data_requests(struct SmashNetDevice *device, BYTE error, ULONG wire_error)
{
    abort_queued_for_opener(device, &device->read_queue, NULL,
                            error, wire_error);
    abort_queued_for_opener(device, &device->orphan_queue, NULL,
                            error, wire_error);
}

static ULONG
supported_event_mask(void)
{
    ULONG mask = S2EVENT_ERROR | S2EVENT_TX | S2EVENT_RX |
                 S2EVENT_ONLINE | S2EVENT_OFFLINE | S2EVENT_BUFF |
                 S2EVENT_HARDWARE | S2EVENT_SOFTWARE |
                 S2EVENT_CONFIGCHANGED;
#ifdef S2EVENT_CONNECT
    mask |= S2EVENT_CONNECT;
#endif
#ifdef S2EVENT_DISCONNECT
    mask |= S2EVENT_DISCONNECT;
#endif
    return (mask);
}

static void
trigger_events(struct SmashNetDevice *device, ULONG events)
{
    struct Node *node;
    struct Node *next;
    struct IOSana2Req *request;
    ULONG matched;

    Forbid();
    node = device->event_queue.lh_Head;
    while (node->ln_Succ != NULL) {
        next = node->ln_Succ;
        request = (struct IOSana2Req *)node;
        matched = request->ios2_WireError & events;
        if (matched != 0) {
            Remove(node);
            request->ios2_WireError = matched;
            request->ios2_Req.io_Error = 0;
            ReplyMsg(&request->ios2_Req.io_Message);
        }
        node = next;
    }
    Permit();
}

static void
free_device_memory(struct SmashNetDevice *device)
{
    ULONG negative_size = device->library.lib_NegSize;
    ULONG positive_size = device->library.lib_PosSize;
    UBYTE *allocation = ((UBYTE *)device) - negative_size;
    FreeMem(allocation, negative_size + positive_size);
}

static LONG
device_open(register struct SmashNetDevice *device __asm("a6"),
            register struct IOSana2Req *request __asm("a1"),
            register ULONG unit_number __asm("d0"),
            register ULONG flags __asm("d1"))
{
    struct SmashNetOpener *opener = NULL;
    struct TagItem *tags = NULL;
    BOOL full_sana_request;
    LONG result = 0;

    if (request == NULL) {
        return (IOERR_BADADDRESS);
    }

    queue_debug(device->context, "Open", request, unit_number, flags,
                device->library.lib_OpenCnt);
    request->ios2_Req.io_Error = 0;
    full_sana_request =
        request->ios2_Req.io_Message.mn_Length >= sizeof(struct IOSana2Req);
    if (full_sana_request) {
        request->ios2_WireError = 0;
        tags = (struct TagItem *)request->ios2_BufferManagement;
    }

    if (request->ios2_Req.io_Message.mn_Length < sizeof(struct IOStdReq)) {
        result = IOERR_OPENFAIL;
    } else if (unit_number != SMASHNET_UNIT) {
        result = IOERR_OPENFAIL;
    } else {
        Forbid();
        if (device->context != NULL && device->context->shutting_down) {
            result = IOERR_OPENFAIL;
        } else if (((flags & SANA2OPF_PROM) != 0 &&
             (flags & SANA2OPF_MINE) == 0) ||
            device->exclusive ||
            ((flags & SANA2OPF_MINE) != 0 &&
             device->library.lib_OpenCnt != 0)) {
            result = IOERR_UNITBUSY;
        } else if (device->library.lib_OpenCnt != 0 &&
                   (device->library.lib_Flags & LIBF_DELEXP) != 0) {
            result = IOERR_OPENFAIL;
        } else {
            opener = find_free_opener(device);
            if (opener == NULL) {
                result = IOERR_OPENFAIL;
            } else {
                read_buffer_tags(opener, tags);
                if (tags != NULL &&
                    (opener->copy_to_buff == NULL ||
                     opener->copy_from_buff == NULL)) {
                    /* A non-empty SANA-II tag list must provide both mandatory
                     * callbacks.  A NULL list is allowed for query/statistics
                     * opens; data commands then fail gracefully. */
                    opener->copy_to_buff = NULL;
                    opener->copy_from_buff = NULL;
                    opener->packet_filter = NULL;
                    result = S2ERR_BAD_ARGUMENT;
                } else {
                    opener->in_use = TRUE;
                    opener->open_request = (struct IORequest *)request;
                    opener->open_flags = flags;
                    if (full_sana_request) {
                        request->ios2_BufferManagement = opener;
                    }
                    request->ios2_Req.io_Unit = &device->unit;
                    request->ios2_Req.io_Device = (struct Device *)device;
                    ++device->library.lib_OpenCnt;
                    ++device->unit.unit_OpenCnt;
                    device->library.lib_Flags &= (UBYTE)~LIBF_DELEXP;
                    if ((flags & SANA2OPF_MINE) != 0) {
                        device->exclusive = TRUE;
                    }
                    if ((flags & SANA2OPF_PROM) != 0) {
                        device->promiscuous = TRUE;
                    }
                }
            }
        }
        Permit();
    }

    if (result != 0) {
        request->ios2_Req.io_Error = (BYTE)result;
        request->ios2_Req.io_Device = (struct Device *)-1;
        request->ios2_Req.io_Unit = (struct Unit *)-1;
    }

    queue_debug(device->context, "OpenResult", request, unit_number, flags,
                device->library.lib_OpenCnt);
    if (device->context != NULL && device->context->owner_task != NULL) {
        Signal(device->context->owner_task,
               signal_mask(device->context->state_signal_bit));
    }
    return (result);
}

static BPTR
device_close(register struct SmashNetDevice *device __asm("a6"),
             register struct IOSana2Req *request __asm("a1"))
{
    struct SmashNetOpener *opener;
    BPTR segment = 0;
    BOOL expunge = FALSE;

    Forbid();
    opener = find_request_opener(device, request);
    if (opener != NULL && opener->in_use) {
        /* Keep the opener active until every queued request using its callback
         * table has been detached and replied.  The outer Forbid() makes the
         * nested queue helpers atomic with the state change below. */
        abort_all_for_opener(device, opener);
        if ((opener->open_flags & SANA2OPF_MINE) != 0) {
            device->exclusive = FALSE;
        }
        if ((opener->open_flags & SANA2OPF_PROM) != 0) {
            device->promiscuous = FALSE;
        }
        opener->in_use = FALSE;
        opener->open_request = NULL;
        opener->copy_to_buff = NULL;
        opener->copy_from_buff = NULL;
        opener->packet_filter = NULL;
        opener->open_flags = 0;
        if (device->library.lib_OpenCnt != 0) {
            --device->library.lib_OpenCnt;
        }
        if (device->unit.unit_OpenCnt != 0) {
            --device->unit.unit_OpenCnt;
        }
        expunge = device->library.lib_OpenCnt == 0 &&
                  (device->library.lib_Flags & LIBF_DELEXP) != 0;
    }
    Permit();

    if (request != NULL) {
        request->ios2_Req.io_Device = (struct Device *)-1;
        request->ios2_Req.io_Unit = (struct Unit *)-1;
    }

    queue_debug(device->context, "Close", request, (ULONG)-1, 0, 0);
    if (expunge) {
        return (device_expunge(device));
    }

    if (device->context != NULL && device->context->owner_task != NULL) {
        Signal(device->context->owner_task,
               signal_mask(device->context->state_signal_bit));
    }
    return (segment);
}

static BPTR
device_expunge(register struct SmashNetDevice *device __asm("a6"))
{
    struct SmashNetContext *context;

    Forbid();
    if (device->library.lib_OpenCnt != 0) {
        device->library.lib_Flags |= LIBF_DELEXP;
        Permit();
        return (0);
    }

    context = device->context;
    queue_debug(context, "Expunge", NULL, (ULONG)-1, 0, 0);
    Remove(&device->library.lib_Node);
    if (context != NULL) {
        context->device = NULL;
        if (context->owner_task != NULL) {
            Signal(context->owner_task,
                   signal_mask(context->state_signal_bit));
        }
    }
    free_device_memory(device);
    Permit();
    return (0);
}

static ULONG
device_reserved(void)
{
    return (0);
}

static void
handle_nsd_device_query(struct IOStdReq *request)
{
    struct NSDeviceQueryResult *query;

    request->io_Actual = 0;
    if (request->io_Data == NULL) {
        request->io_Error = IOERR_BADADDRESS;
        return;
    }
    if (request->io_Length < sizeof(struct NSDeviceQueryResult)) {
        request->io_Error = IOERR_BADLENGTH;
        return;
    }

    query = (struct NSDeviceQueryResult *)request->io_Data;
    if (query->nsdqr_DevQueryFormat != 0) {
        request->io_Error = IOERR_NOCMD;
        return;
    }

    query->nsdqr_DevQueryFormat = 0;
    query->nsdqr_SizeAvailable = sizeof(*query);
    query->nsdqr_DeviceType = NSDEVTYPE_SANA2;
    query->nsdqr_DeviceSubType = 0;
    query->nsdqr_SupportedCommands = (UWORD *)supported_commands;
    request->io_Actual = sizeof(*query);
    request->io_Error = 0;
}

static void
fill_device_query(struct IOSana2Req *request)
{
    struct Sana2DeviceQuery *query;
    ULONG available;
    ULONG supplied;

    query = (struct Sana2DeviceQuery *)request->ios2_StatData;
    if (query == NULL) {
        request_set_error(request, S2ERR_BAD_ARGUMENT, S2WERR_GENERIC_ERROR);
        return;
    }

    available = query->SizeAvailable;
    if (available < (2 * sizeof(ULONG))) {
        request_set_error(request, S2ERR_BAD_ARGUMENT, S2WERR_BAD_STATDATA);
        return;
    }
    supplied = available;
    if (supplied > sizeof(*query)) {
        supplied = sizeof(*query);
    }

    if (available >= offsetof(struct Sana2DeviceQuery, DevQueryFormat) +
                     sizeof(query->DevQueryFormat)) {
        query->DevQueryFormat = 0;
    }
    if (available >= offsetof(struct Sana2DeviceQuery, DeviceLevel) +
                     sizeof(query->DeviceLevel)) {
        query->DeviceLevel = 0;
    }
    if (available >= offsetof(struct Sana2DeviceQuery, AddrFieldSize) +
                     sizeof(query->AddrFieldSize)) {
        query->AddrFieldSize = 48;
    }
    if (available >= offsetof(struct Sana2DeviceQuery, MTU) + sizeof(query->MTU)) {
        query->MTU = SMASHNET_MTU;
    }
    if (available >= offsetof(struct Sana2DeviceQuery, BPS) + sizeof(query->BPS)) {
        query->BPS = SMASHNET_BPS;
    }
    if (available >= offsetof(struct Sana2DeviceQuery, HardwareType) +
                     sizeof(query->HardwareType)) {
        query->HardwareType = S2WireType_Ethernet;
    }
    if (available >= offsetof(struct Sana2DeviceQuery, RawMTU) +
                     sizeof(query->RawMTU)) {
        query->RawMTU = SMASHNET_RAW_MTU;
    }
    query->SizeSupplied = supplied;
}

static void
zero_output(APTR data, ULONG length)
{
    UBYTE *bytes = (UBYTE *)data;
    ULONG index;

    if (bytes != NULL) {
        for (index = 0; index < length; ++index) {
            bytes[index] = 0;
        }
    }
}

static BOOL
handle_onevent(struct SmashNetDevice *device, struct IOSana2Req *request,
               ULONG requested)
{
    ULONG immediate = 0;

    if ((requested & ~supported_event_mask()) != 0 || requested == 0) {
        request_set_error(request, S2ERR_NOT_SUPPORTED, S2WERR_BAD_EVENT);
        return (FALSE);
    }

    if (device->online && (requested & S2EVENT_ONLINE) != 0) {
        immediate |= S2EVENT_ONLINE;
    }
    if (!device->online && (requested & S2EVENT_OFFLINE) != 0) {
        immediate |= S2EVENT_OFFLINE;
    }

    if (immediate != 0) {
        request->ios2_WireError = immediate;
        return (FALSE);
    }

    request->ios2_WireError = requested;
    enqueue_request(&device->event_queue, request);
    return (TRUE);
}

static void
handle_config_interface(struct SmashNetDevice *device,
                        struct IOSana2Req *request)
{
    BOOL was_online = device->online;

    if (!mac_is_zero(request->ios2_SrcAddr)) {
        copy_mac(device->station_address, request->ios2_SrcAddr);
        {
            static char buf[64];
            uint8_t *mac = device->station_address;
            sprintf(buf, "ConfigIF MAC %02x:%02x:%02x:%02x:%02x:%02x",
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            queue_debug(device->context, buf, NULL, (ULONG)-1, 0, 0);
            }
        } else
            queue_debug(device->context, "ConfigIF", NULL, (ULONG)-1, 0, 0);
    device->configured = TRUE;
    device->online = TRUE;
    copy_mac(request->ios2_SrcAddr, device->station_address);
    copy_mac(request->ios2_DstAddr, device->station_address);

    trigger_events(device, S2EVENT_CONFIGCHANGED |
                    (was_online ? 0 : S2EVENT_ONLINE));
}

static void
handle_online(struct SmashNetDevice *device)
{
    if (!device->online) {
        device->online = TRUE;
        trigger_events(device, S2EVENT_ONLINE);
    }
}

static void
handle_offline(struct SmashNetDevice *device)
{
    if (device->online) {
        device->online = FALSE;
        abort_data_requests(device, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);
        trigger_events(device, S2EVENT_OFFLINE);
    }
}

static void
unimplemented_command(struct IOSana2Req *request)
{
    request_set_error(request, IOERR_NOCMD, S2WERR_GENERIC_ERROR);
}

static void
dbgpkt(struct SmashNetDevice *device, const char *name,
       const uint8_t *data, uint datalen)
{
    static char buf[2048];
    uint pos;
    uint bufpos;
    uint maxpos;
    strcpy(buf, name);
    bufpos = strlen(name);
    maxpos = (sizeof (buf) - bufpos - 1) / 3;
    if (maxpos > datalen)
        maxpos = datalen;
    for (pos = 0; pos < maxpos; pos++) {
        if (bufpos >= sizeof (buf) - 4)
            break;
        buf[bufpos++] = ' ';
        buf[bufpos++] = "0123456789abcdef"[data[pos] >> 4];
        buf[bufpos++] = "0123456789abcdef"[data[pos] & 0xf];
    }
    buf[bufpos] = '\0';
    queue_debug(device->context, buf, NULL, (ULONG)-1, 0, 0);
}

static void
device_begin_io(register struct SmashNetDevice *device __asm("a6"),
                register struct IOSana2Req *request __asm("a1"))
{
    ULONG command;
    ULONG input_wire_error;
    struct SmashNetOpener *opener;
    BOOL queued = FALSE;

    if (request == NULL) {
        return;
    }

    command = request->ios2_Req.io_Command;
    if ((command != CMD_READ) && (command != CMD_WRITE))
        queue_debug(device->context, "BeginIO", request, (ULONG)-1, 0, 0);

    if (command == NSCMD_DEVICEQUERY) {
        if (request->ios2_Req.io_Message.mn_Length < sizeof(struct IOStdReq)) {
            request->ios2_Req.io_Error = IOERR_BADLENGTH;
        } else {
            handle_nsd_device_query((struct IOStdReq *)request);
        }
        if ((command != CMD_READ) && (command != CMD_WRITE)) {
            queue_debug(device->context, "CompleteIO",
                        request, (ULONG)-1, 0, 0);
        }
        complete_request(request);
        return;
    }

    if (request->ios2_Req.io_Message.mn_Length < sizeof(struct IOSana2Req)) {
        request->ios2_Req.io_Error = IOERR_BADLENGTH;
        complete_request(request);
        return;
    }

    Forbid();
    input_wire_error = request->ios2_WireError;
    request->ios2_Req.io_Error = 0;
    request->ios2_WireError = 0;
    opener = find_cookie_opener(device, request);

    switch (command) {
        case CMD_RESET:
        case CMD_UPDATE:
        case CMD_CLEAR:
        case CMD_STOP:
        case CMD_START:
            break;

        case CMD_FLUSH:
            if (opener == NULL) {
                request_set_error(request, S2ERR_BAD_ARGUMENT,
                                  S2WERR_GENERIC_ERROR);
            } else {
                abort_all_for_opener(device, opener);
            }
            break;

        case CMD_READ:
            if (!device->online) {
                request_set_error(request, S2ERR_OUTOFSERVICE,
                                  S2WERR_UNIT_OFFLINE);
            } else if (opener == NULL || !opener->in_use ||
                       opener->copy_to_buff == NULL) {
                request_set_error(request, S2ERR_BAD_ARGUMENT,
                                  S2WERR_BUFF_ERROR);
            } else {
                enqueue_request(&device->read_queue, request);
                queued = TRUE;
            }
            break;

        case CMD_WRITE:
        case S2_BROADCAST:
        case S2_MULTICAST:
            if (!device->online) {
                request_set_error(request, S2ERR_OUTOFSERVICE,
                                  S2WERR_UNIT_OFFLINE);
                break;
            } else if (((request->ios2_Req.io_Flags & SANA2IOF_RAW) != 0 &&
                        request->ios2_DataLength > SMASHNET_RAW_MTU) ||
                       ((request->ios2_Req.io_Flags & SANA2IOF_RAW) == 0 &&
                        request->ios2_DataLength > SMASHNET_MTU)) {
                request_set_error(request, S2ERR_MTU_EXCEEDED,
                                  S2WERR_GENERIC_ERROR);
                break;
            }
            /* Transmit to remote USB host */
            uint8_t txbuf[SMASHNET_RAW_MTU + sizeof (hm_nreadwrite_t)];
            ethhdr_t *hdr = (ethhdr_t *) (txbuf + sizeof (hm_nreadwrite_t));
            uint datalen;

            if (request->ios2_Req.io_Flags & SANA2IOF_RAW) {
                datalen = request->ios2_DataLength;
                if (opener == NULL || opener->copy_from_buff == NULL ||
                    !smashnet_call_copy(opener->copy_from_buff, (APTR) hdr,
                                        request->ios2_Data,
                                        datalen)) {
                    request_set_error(request, S2ERR_NO_RESOURCES,
                                      S2WERR_BUFF_ERROR);
                    break;
                }
                dbgpkt(device, "Tx", (uint8_t *) hdr, datalen);
                sm_nwrite(NULL, txbuf, datalen, 1);
            } else {
                datalen = request->ios2_DataLength + sizeof (ethhdr_t);
                // XXX: SRC MAC might need to be assigned by hardware,
                //      not the network stack.
                memcpy(hdr->dstmac, request->ios2_DstAddr, sizeof (hdr->dstmac));
                memcpy(hdr->srcmac, request->ios2_SrcAddr, sizeof (hdr->srcmac));
                hdr->type = request->ios2_PacketType;
                if (opener == NULL || opener->copy_from_buff == NULL ||
                    !smashnet_call_copy(opener->copy_from_buff,
                                        (APTR) (hdr + 1),
                                        request->ios2_Data,
                                        request->ios2_DataLength)) {
                    request_set_error(request, S2ERR_NO_RESOURCES,
                                      S2WERR_BUFF_ERROR);
                    break;
                }
//              dbgpkt(device, "Tx", (uint8_t *) hdr, datalen);
                sm_nwrite(NULL, txbuf, datalen, 1);
            }
            break;

        case S2_DEVICEQUERY:
            fill_device_query(request);
            break;

        case S2_GETSTATIONADDRESS:
            {
            uint rc;
            rc = sm_ngetmac(device->station_address);
            if (rc != 0) {
                static char buf[64];
                sprintf(buf, "ngetmac failed %d", rc);
                queue_debug(device->context, buf, NULL, (ULONG)-1, 0, 0);
            }
            }
            copy_mac(request->ios2_SrcAddr, device->station_address);
            copy_mac(request->ios2_DstAddr, device->station_address);
            {
                static char buf[64];
                uint8_t *mac = device->station_address;
                sprintf(buf, "GetStationAddr MAC %02x:%02x:%02x:%02x:%02x:%02x",
                        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                queue_debug(device->context, buf, NULL, (ULONG)-1, 0, 0);
            }
            break;

        case S2_CONFIGINTERFACE:
            handle_config_interface(device, request);
            break;

        case S2_ADDMULTICASTADDRESS:
        case S2_DELMULTICASTADDRESS:
#ifdef S2_ADDMULTICASTADDRESSES
        case S2_ADDMULTICASTADDRESSES:
#endif
#ifdef S2_DELMULTICASTADDRESSES
        case S2_DELMULTICASTADDRESSES:
#endif
        case S2_TRACKTYPE:
        case S2_UNTRACKTYPE:
            /* Accepted as a no-op so stacks can complete setup. */
            break;

        case S2_GETTYPESTATS:
            zero_output(request->ios2_StatData, sizeof(struct Sana2PacketTypeStats));
            break;

        case S2_GETSPECIALSTATS:
            if (request->ios2_StatData != NULL) {
                struct Sana2SpecialStatHeader *header;
                header = (struct Sana2SpecialStatHeader *)request->ios2_StatData;
                header->RecordCountSupplied = 0;
            }
            break;

        case S2_GETGLOBALSTATS:
            zero_output(request->ios2_StatData, sizeof(struct Sana2DeviceStats));
            break;

        case S2_ONEVENT:
            if (opener == NULL) {
                request_set_error(request, S2ERR_BAD_ARGUMENT,
                                  S2WERR_GENERIC_ERROR);
            } else {
                queued = handle_onevent(device, request, input_wire_error);
            }
            break;

        case S2_READORPHAN:
            if (!device->online) {
                request_set_error(request, S2ERR_OUTOFSERVICE,
                                  S2WERR_UNIT_OFFLINE);
            } else if (opener == NULL || !opener->in_use ||
                       opener->copy_to_buff == NULL) {
                request_set_error(request, S2ERR_BAD_ARGUMENT,
                                  S2WERR_BUFF_ERROR);
            } else {
                enqueue_request(&device->orphan_queue, request);
                queued = TRUE;
            }
            break;

        case S2_ONLINE:
            handle_online(device);
            break;

        case S2_OFFLINE:
            handle_offline(device);
            break;

#ifdef S2_GETPEERADDRESS
        case S2_GETPEERADDRESS:
#endif
#ifdef S2_GETDNSADDRESS
        case S2_GETDNSADDRESS:
#endif
#ifdef S2_GETEXTENDEDGLOBALSTATS
        case S2_GETEXTENDEDGLOBALSTATS:
#endif
#ifdef S2_CONNECT
        case S2_CONNECT:
#endif
#ifdef S2_DISCONNECT
        case S2_DISCONNECT:
#endif
#ifdef S2_SAMPLE_THROUGHPUT
        case S2_SAMPLE_THROUGHPUT:
#endif
#ifdef S2_SANA2HOOK
        case S2_SANA2HOOK:
#endif
            unimplemented_command(request);
            break;

        case CMD_INVALID:
        default:
            request_set_error(request, IOERR_NOCMD, S2WERR_GENERIC_ERROR);
            break;
    }
    Permit();

    if (!queued) {
        if ((command != CMD_READ) && (command != CMD_WRITE)) {
            queue_debug(device->context, "CompleteIO",
                        request, (ULONG)-1, 0, 0);
        }
        complete_request(request);
    }
}

static LONG
device_abort_io(register struct SmashNetDevice *device __asm("a6"),
                register struct IOSana2Req *request __asm("a1"))
{
    BOOL removed = FALSE;

    if (request == NULL) {
        return (IOERR_NOCMD);
    }

    Forbid();
    if (remove_request_from_all_queues(device, request)) {
        request_set_error(request, IOERR_ABORTED, S2WERR_GENERIC_ERROR);
        removed = TRUE;
    }
    queue_debug(device->context, "AbortIO", request, (ULONG)-1, 0, 0);
    if (removed) {
        ReplyMsg(&request->ios2_Req.io_Message);
    }
    Permit();

    return (removed ? 0 : IOERR_NOCMD);
}

static struct IOSana2Req *
detach_matching_read(struct SmashNetDevice *device, ULONG packet_type,
                     const UBYTE *destination)
{
    struct Node *node;
    struct IOSana2Req *request;
    struct SmashNetOpener *opener;

    node = device->read_queue.lh_Head;
    while (node->ln_Succ != NULL) {
        request = (struct IOSana2Req *)node;
        opener = request_opener(request);
        if (request->ios2_PacketType == packet_type && opener != NULL &&
            opener->in_use &&
            (device->promiscuous || mac_equal(destination, device->station_address) ||
             mac_is_broadcast(destination) || (destination[0] & 1) != 0)) {
            Remove(node);
            return (request);
        }
        node = node->ln_Succ;
    }
    return (NULL);
}

static struct IOSana2Req *
detach_orphan(struct SmashNetDevice *device)
{
    struct Node *node = RemHead(&device->orphan_queue);
    return ((struct IOSana2Req *)node);
}

/* Roadshow's copy function ABI expects arguments in registers */
typedef BOOL (*copyfunc_t)(APTR to __asm("a0"),
                           APTR from __asm("a1"),
                           ULONG n __asm("d0")) __asm("d0");

static BOOL
smashnet_call_copy(APTR callback, APTR to, APTR from, ULONG length)
{
    copyfunc_t func = (copyfunc_t)callback;
    if (callback == NULL)
        return (FALSE);
    return (func(to, from, length));
}

static BOOL
deliver_frame(struct SmashNetDevice *device, struct IOSana2Req *request,
              const UBYTE *frame, ULONG frame_length, ULONG packet_type)
{
    struct SmashNetOpener *opener;
    const UBYTE *payload;
    ULONG payload_length;
    BOOL copied;

    opener = request_opener(request);
    if (opener == NULL || !opener->in_use || opener->copy_to_buff == NULL) {
        request_set_error(request, S2ERR_BAD_ARGUMENT, S2WERR_BUFF_ERROR);
        complete_request(request);
        return (FALSE);
    }

    copy_mac(request->ios2_DstAddr, frame);
    copy_mac(request->ios2_SrcAddr, frame + 6);
    request->ios2_PacketType = packet_type;
    request->ios2_Req.io_Flags &= ~(SANA2IOF_BCAST | SANA2IOF_MCAST);
    if (mac_is_broadcast(frame)) {
        request->ios2_Req.io_Flags |= SANA2IOF_BCAST;
    } else if ((frame[0] & 1) != 0) {
        request->ios2_Req.io_Flags |= SANA2IOF_MCAST;
    }

    if ((request->ios2_Req.io_Flags & SANA2IOF_RAW) != 0) {
        payload = frame;
        payload_length = frame_length;
    } else {
        payload = frame + 14;
        payload_length = frame_length - 14;
    }

    request->ios2_DataLength = payload_length;
    copied = smashnet_call_copy(opener->copy_to_buff, request->ios2_Data,
                                (APTR)payload, payload_length);
    if (!copied) {
        request_set_error(request, S2ERR_NO_RESOURCES, S2WERR_BUFF_ERROR);
    } else {
        request_set_error(request, 0, 0);
    }

    (void) device;
//  queue_debug(device->context, "ReceiveToStack", request, (ULONG)-1, 0, 0);
    complete_request(request);
    return (copied);
}

static BOOL
SmashNet_ReceiveEthernetFrame(const UBYTE *frame, ULONG length)
{
    struct SmashNetContext *context = g_context;
    struct SmashNetDevice *device;
    struct IOSana2Req *request;
    ULONG packet_type;
    BOOL delivered = FALSE;

    if (context == NULL || frame == NULL || length < 14 ||
        length > SMASHNET_RAW_MTU) {
        return (FALSE);
    }

    Forbid();
    device = (struct SmashNetDevice *)context->device;
    if (device == NULL || !device->online) {
        Permit();
        return (FALSE);
    }

//  dbgpkt(device, "Rx", frame, length);

    packet_type = ((ULONG)frame[12] << 8) | frame[13];
    request = detach_matching_read(device, packet_type, frame);
    if (request == NULL) {
        request = detach_orphan(device);
#if 0
        queue_debug(device->context, "DO", NULL, (ULONG)-1, 0, 0);
        if (request == NULL) {
            static char buf[64];
            sprintf(buf, "Rx Detach %x NULL", packet_type);
            queue_debug(device->context, buf, NULL, (ULONG)-1, 0, 0);
        }
#endif
    }
    if (request != NULL) {
        /* SANA-II buffer callbacks are designed for driver context. Holding
         * Forbid() here also prevents Close/Expunge from invalidating the
         * opener, request, or device while the frame is copied and replied. */
        delivered = deliver_frame(device, request, frame, length, packet_type);
//      if ((frame[23] == 0x01) && (frame[34] == 0x00)) {
        if (frame[23] == 0x01) {
            // ICMP                ECHO Reply
//          dbgpkt(device, "ICMP Rx", frame, length);
            uint icmp_seq;
            static char buf[64];
            icmp_seq = (frame[40] << 8) | frame[41];
            sprintf(buf, "ICMP E %u", icmp_seq);
            queue_debug(device->context, buf, NULL, (ULONG)-1, 0, 0);
        }
//      queue_debug(device->context, "Rx Delivered", NULL, (ULONG)-1, 0, 0);
    }
    Permit();
    return (delivered);
}

static void
smashnet_poll_hardware(struct SmashNetDevice *device)
{
    if (sm_net_active == 0) {
        if (sm_nservice() != 0) {
            queue_debug(device->context, "net up", NULL, (ULONG)-1, 0, 0);
        }
        return;
    } else {
        if (sm_nservice() == 0) {
            queue_debug(device->context, "net down", NULL, (ULONG)-1, 0, 0);
        }
    }
    if (sm_net_active == 0)
        return;

    if (sm_net_open == 0) {
        uint rc = sm_nopen();
        if (rc == 0) {
            sm_net_open = 1;
            queue_debug(device->context, "net open", NULL, (ULONG)-1, 0, 0);
        } else {
            sm_net_open = 2;
            queue_debug(device->context, "net openfail", NULL, (ULONG)-1, 0, 0);
        }
    }

    if (sm_net_open == 1) {
        uint readlen;
        void *datap;
        /* Poll remote USB host */
        if (sm_nread(&datap, &readlen) == 0) {
            /* Received a complete ethernet frame */
#if 0
            static char buf[32];
            sprintf(buf, "Rx len=%u", readlen);
            queue_debug(device->context, buf, NULL, (ULONG)-1, 0, 0);
#endif
            SmashNet_ReceiveEthernetFrame(datap, readlen);
        }
    }
}

static void
rx_task_entry(void)
{
    struct SmashNetContext *context = g_context;
    struct MsgPort *timer_port = NULL;
    struct timerequest *timer_request = NULL;
    struct Task *owner_task;
    ULONG ready_signal;
    ULONG done_signal;
    BOOL timer_open = FALSE;
    BOOL timer_pending = FALSE;
    ULONG wait_mask;
    ULONG signals;

    if (context == NULL) {
        RemTask(NULL);
        return;
    }

    owner_task = context->owner_task;
    ready_signal = signal_mask(context->rx_ready_signal_bit);
    done_signal = signal_mask(context->rx_done_signal_bit);

    timer_port = CreateMsgPort();
    if (timer_port != NULL) {
        timer_request = (struct timerequest *)CreateIORequest(
            timer_port, sizeof(struct timerequest));
    }
    if (timer_request != NULL &&
        OpenDevice(TIMERNAME, UNIT_MICROHZ,
                   &timer_request->tr_node, 0) == 0) {
        timer_open = TRUE;
    }

    if (!timer_open) {
        context->rx_start_error = 1;
        if (timer_request != NULL) {
            DeleteIORequest(&timer_request->tr_node);
        }
        if (timer_port != NULL) {
            DeleteMsgPort(timer_port);
        }
        context->rx_task = NULL;
        Signal(owner_task, ready_signal | done_signal);
        RemTask(NULL);
        return;
    }

    context->rx_start_error = 0;
    Signal(owner_task, ready_signal);
    wait_mask = signal_mask(timer_port->mp_SigBit) | SMASHNET_RX_SIGNAL;

    for (;;) {
        timer_request->tr_node.io_Command = TR_ADDREQUEST;
        timer_request->tr_time.tv_secs = 0;
        timer_request->tr_time.tv_micro = SMASHNET_POLL_US;
        SendIO(&timer_request->tr_node);
        timer_pending = TRUE;

        signals = Wait(wait_mask);
        if ((signals & SMASHNET_RX_SIGNAL) != 0) {
            if (timer_pending) {
                AbortIO(&timer_request->tr_node);
                WaitIO(&timer_request->tr_node);
                timer_pending = FALSE;
            }
            break;
        }

        if ((signals & signal_mask(timer_port->mp_SigBit)) != 0) {
            WaitIO(&timer_request->tr_node);
            timer_pending = FALSE;
            smashnet_poll_hardware((struct SmashNetDevice *)context->device);
        }
    }

    CloseDevice(&timer_request->tr_node);
    DeleteIORequest(&timer_request->tr_node);
    DeleteMsgPort(timer_port);

    context->rx_task = NULL;
    Signal(owner_task, done_signal);
    RemTask(NULL);
}

static BOOL
allocate_signals(struct SmashNetContext *context)
{
    context->log_signal_bit = AllocSignal(-1);
    context->state_signal_bit = AllocSignal(-1);
    context->rx_ready_signal_bit = AllocSignal(-1);
    context->rx_done_signal_bit = AllocSignal(-1);

    if (context->log_signal_bit < 0 || context->state_signal_bit < 0 ||
        context->rx_ready_signal_bit < 0 || context->rx_done_signal_bit < 0) {
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
    if (context->rx_ready_signal_bit >= 0) {
        FreeSignal(context->rx_ready_signal_bit);
    }
    if (context->rx_done_signal_bit >= 0) {
        FreeSignal(context->rx_done_signal_bit);
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
        if (node->ln_Name != NULL && strcmp(node->ln_Name, SMASHNET_NAME) == 0) {
            found = TRUE;
            break;
        }
        node = node->ln_Succ;
    }
    Permit();
    return (found);
}

static struct SmashNetDevice *
create_device(struct SmashNetContext *context)
{
    struct SmashNetDevice *device;
    ULONG custom_offset = sizeof(struct Library);

    device = (struct SmashNetDevice *)MakeLibrary((APTR)device_vectors,
                                                   NULL, NULL,
                                                   sizeof(*device), 0);
    if (device == NULL) {
        return (NULL);
    }

    memset(((UBYTE *)device) + custom_offset, 0,
           sizeof(*device) - custom_offset);
    device->library.lib_Node.ln_Type = NT_DEVICE;
    device->library.lib_Node.ln_Name = (STRPTR)SMASHNET_NAME;
    device->library.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    device->library.lib_Version = SMASHNET_VERSION;
    device->library.lib_Revision = SMASHNET_REVISION;
    device->library.lib_IdString = (APTR)SMASHNET_IDSTRING;
    device->context = context;

    device->unit.unit_MsgPort.mp_Node.ln_Type = NT_MSGPORT;
    NewList(&device->unit.unit_MsgPort.mp_MsgList);
    NewList(&device->read_queue);
    NewList(&device->orphan_queue);
    NewList(&device->event_queue);

    /* Locally administered, unicast placeholder address. */
    uint rc;
    if ((rc = sm_ngetmac(device->station_address)) != 0) {
        /* MAC is not available (yet) */
        static char buf[64];
        device->station_address[0] = 0x02;  // Locally-administered
        device->station_address[1] = 0x80;  // OUI
        memcpy(&device->station_address[2], &SysBase->IdleCount, 4);
        sprintf(buf, "ngetmac fail %d", rc);
        queue_debug(device->context, buf, NULL, (ULONG)-1, 0, 0);
    }
    {
        static char buf[64];
        uint8_t *mac = device->station_address;
        sprintf(buf, "Use MAC %02x:%02x:%02x:%02x:%02x:%02x",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        queue_debug(device->context, buf, NULL, (ULONG)-1, 0, 0);
    }
#if 0
    device->station_address[0] = 0x02;
    device->station_address[1] = 0x08;
    device->station_address[2] = 0x22;
    device->station_address[3] = 0x10;
    device->station_address[4] = 0x37;
    device->station_address[5] = 0xe5;
// 02:08:22:10:37:e5
#endif
    return (device);
}

static ULONG
current_open_count(struct SmashNetContext *context)
{
    struct SmashNetDevice *device;
    ULONG count = 0;

    Forbid();
    device = (struct SmashNetDevice *)context->device;
    if (device != NULL) {
        count = device->library.lib_OpenCnt;
    }
    Permit();
    return (count);
}

static void
begin_shutdown(struct SmashNetContext *context)
{
    Forbid();
    context->shutting_down = TRUE;
    Permit();
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

    easy.es_StructSize = sizeof(easy);
    easy.es_Flags = 0;
    easy.es_Title = (STRPTR)SMASHNET_NAME;
    easy.es_TextFormat = (STRPTR)
        "smashnet.device still has %ld open client(s).\n\n"
        "Shut down the Roadshow/network interface, then choose Wait for close.\n"
        "The program will exit after the final CloseDevice().";
    easy.es_GadgetFormat = (STRPTR)"Wait for close|Keep running";
    arguments[0] = open_count;
    return (EasyRequestArgs(NULL, &easy, NULL, (APTR)arguments) != 0);
}

static void
wait_for_open_count_zero(struct SmashNetContext *context)
{
    ULONG mask = signal_mask(context->log_signal_bit) |
                 signal_mask(context->state_signal_bit);

    while (context->device != NULL && current_open_count(context) != 0) {
        drain_debug(context);
        Wait(mask);
    }
    drain_debug(context);
}

static void
stop_rx_task(struct SmashNetContext *context)
{
    struct Task *task;
    ULONG mask = signal_mask(context->rx_done_signal_bit) |
                 signal_mask(context->log_signal_bit);

    Forbid();
    task = (struct Task *)context->rx_task;
    Permit();
    if (task == NULL) {
        return;
    }

    Signal(task, SMASHNET_RX_SIGNAL);
    while (context->rx_task != NULL) {
        ULONG signals = Wait(mask);
        if ((signals & signal_mask(context->log_signal_bit)) != 0) {
            drain_debug(context);
        }
    }
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
    ULONG mask = signal_mask(context->log_signal_bit) |
                 signal_mask(context->state_signal_bit);

    while (context->device != NULL) {
        ULONG signals = Wait(mask);
        if ((signals & signal_mask(context->log_signal_bit)) != 0) {
            drain_debug(context);
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
    drain_debug(context);
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
    struct Task *rx_task = NULL;
    ULONG wait_mask;
    ULONG signals;
    ULONG open_count;
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

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 36);
    if (IntuitionBase == NULL) {
        printf("Unable to open intuition.library V36.\n");
        goto cleanup;
    }

    if (device_already_present()) {
        printf("%s is already installed.\n", SMASHNET_NAME);
        goto cleanup;
    }

    context = (struct SmashNetContext *)AllocMem(sizeof(*context),
                                                  MEMF_PUBLIC | MEMF_CLEAR);
    if (context == NULL) {
        printf("Unable to allocate smashnet context.\n");
        goto cleanup;
    }
    context->log_signal_bit = -1;
    context->state_signal_bit = -1;
    context->rx_ready_signal_bit = -1;
    context->rx_done_signal_bit = -1;
    context->owner_task = FindTask(NULL);

    if (!allocate_signals(context)) {
        printf("Unable to allocate Exec signals.\n");
        goto cleanup;
    }

    g_context = context;
    {
        BYTE receive_priority = context->owner_task->tc_Node.ln_Pri;
        if (receive_priority > -128) {
            --receive_priority;
        }
        Forbid();
        rx_task = CreateTask((STRPTR)"smashnet receive task",
                             receive_priority,
                             rx_task_entry, SMASHNET_TASK_STACK);
        if (rx_task != NULL) {
            context->rx_task = rx_task;
        }
        Permit();
    }
    if (rx_task == NULL) {
        printf("Unable to create receive task.\n");
        goto cleanup;
    }

    Wait(signal_mask(context->rx_ready_signal_bit));
    if (context->rx_start_error != 0) {
        printf("Receive task could not open timer.device.\n");
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
                signal_mask(context->log_signal_bit) |
                signal_mask(context->state_signal_bit) |
                signal_mask(context->rx_done_signal_bit);

    while (!exit_requested) {
        signals = Wait(wait_mask);
        if ((signals & signal_mask(context->log_signal_bit)) != 0) {
            drain_debug(context);
        }
        if (context->device == NULL) {
            printf("%s was removed externally; shutting down host task.\n",
                   SMASHNET_NAME);
            exit_requested = TRUE;
        }
        if ((signals & SIGBREAKF_CTRL_C) != 0) {
            open_count = current_open_count(context);
            if (open_count == 0 || ask_to_wait_for_close(open_count)) {
                begin_shutdown(context);
                exit_requested = TRUE;
            }
        }
    }

    open_count = current_open_count(context);
    if (open_count != 0) {
        printf("Waiting for %lu open client(s) to close...\n", open_count);
        wait_for_open_count_zero(context);
    }

    result = RETURN_OK;

cleanup:
    if (sm_net_open) {
        sm_nclose();
    }
    if (context != NULL) {
        begin_shutdown(context);
        stop_rx_task(context);
        if (context->device != NULL) {
            if (current_open_count(context) != 0) {
                /* Initialization failures can still race with a client that
                 * opened the device immediately after AddDevice(). */
                wait_for_open_count_zero(context);
            }
            remove_installed_device(context);
            wait_for_device_removed(context);
        } else if (device != NULL && !device_installed) {
            /* Device was allocated but never added. */
            free_device_memory(device);
            device = NULL;
        }
        drain_debug(context);
        g_context = NULL;

        /*
         * Belt-and-suspenders: make sure none of our signal bits are
         * left pending before free_signals() releases them back to
         * the task's pool. Freeing a signal bit while it is still
         * set is not safe (see wait_for_device_removed()).
         */
        SetSignal(0, signal_mask(context->log_signal_bit) |
                     signal_mask(context->state_signal_bit) |
                     signal_mask(context->rx_ready_signal_bit) |
                     signal_mask(context->rx_done_signal_bit));

        free_signals(context);
        FreeMem(context, sizeof(*context));
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
