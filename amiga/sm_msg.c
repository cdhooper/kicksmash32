/*
 * msg
 * ---
 * Functions for AmigaOS to send messages to and receive messages from
 * Kicksmash.
 *
 * Copyright 2024 Chris Hooper. This program and source may be used
 * and distributed freely, for any purpose which benefits the Amiga
 * community. Commercial use of the binary, source, or algorithms requires
 * prior written approval from Chris Hooper <amiga@cdh.eebugs.com>.
 * All redistributions must retain this Copyright notice.
 *
 * DISCLAIMER: THE SOFTWARE IS PROVIDED "AS-IS", WITHOUT ANY WARRANTY.
 * THE AUTHOR ASSUMES NO LIABILITY FOR ANY DAMAGE ARISING OUT OF THE USE
 * OR MISUSE OF THIS UTILITY OR INFORMATION REPORTED BY THIS UTILITY.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#ifndef STANDALONE
#include <exec/execbase.h>
#include <exec/types.h>
#include <exec/nodes.h>
#include <exec/lists.h>
#include <exec/semaphores.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <clib/alib_protos.h>
#endif
#include <memory.h>
#include "sm_msg.h"
#include "smash_cmd.h"
#include "host_cmd.h"
#include "cpu_control.h"

#define HOST_INTERFACE_VERSION 1
uint8_t host_interface_version = 0;

#define ARRAY_SIZE(x) ((sizeof (x) / sizeof ((x)[0])))

#define ROM_BASE         0x00f80000  /* Base address of Kickstart ROM */

extern uint flag_debug;

#ifdef ROMFS
#define send_cmd_core esend_cmd_core

#else /* ! ROMFS */

static char
printable_ascii(uint8_t ch)
{
    if (ch >= ' ' && ch <= '~')
        return (ch);
    if (ch == '\t' || ch == '\r' || ch == '\n' || ch == '\0')
        return (' ');
    return ('.');
}

/*
 * dump_memory
 * -----------
 * Display hex and ASCII dump of data at the specified memory location.
 *
 * buf is address of the data.
 * len is the number of bytes to display.
 * dump_base is either an address/offset of DUMP_VALUE_UNASSIGNED if
 *     it should not be printed.
 */
void
dump_memory(void *buf, uint len, uint dump_base)
{
    uint pos;
    uint strpos;
    char str[20];
    uint32_t *src = buf;

    len = (len + 3) / 4;
    if (dump_base != DUMP_VALUE_UNASSIGNED)
        printf("%05x:", dump_base);
    for (strpos = 0, pos = 0; pos < len; pos++) {
        uint32_t val = src[pos];
        printf(" %08x", val);
        str[strpos++] = printable_ascii(val >> 24);
        str[strpos++] = printable_ascii(val >> 16);
        str[strpos++] = printable_ascii(val >> 8);
        str[strpos++] = printable_ascii(val);
        if ((pos & 3) == 3) {
            str[strpos] = '\0';
            strpos = 0;
            printf(" %s\n", str);
            if ((dump_base != DUMP_VALUE_UNASSIGNED) && ((pos + 1) < len)) {
                dump_base += 16;
                printf("%05x:", dump_base);
            }
        }
    }
    if ((pos & 3) != 0) {
        str[strpos] = '\0';
        printf("%*s%s\n", (4 - (pos & 3)) * 9 + 1, "", str);
    }
}
#endif

/*
 * send_cmd
 * --------
 * Sends a command to the STM32 ARM CPU running on Kicksmash.
 * All messages are protected by CRC. Message format:
 *     Magic (64 bits)
 *        0x0204, 0x1017, 0x0119, 0x0117
 *     Length (16 bits)
 *        The length specifies the number of payload bytes (not including
 *        magic, length, command, or CRC bytes at end). This number may be
 *        zero (0) if only a command is present.
 *     Command or status code (16 bits)
 *        KS_CMD_*
 *     Additional data (if any)
 *     CRC (32 bits)
 *        CRC is over all content except magic (includes length and command)
 *
 * cmd is the message command to send.
 * arg is a pointer to optional data to send.
 * arglen is the length of optional data to send.
 * reply is a pointer to a buffer for optional reply data.
 *     If reply is NULL, reply data will be received and discarded.
 * replymax is the length of the reply buffer.
 * replyalen is the actual length of reply data received, filled in
 *     by this function.
 */
uint
send_cmd(uint16_t cmd, void *arg, uint16_t arglen,
         void *reply, uint replymax, uint *replyalen)
{
    uint rc;

    SUPERVISOR_STATE_ENTER();
    INTERRUPTS_DISABLE();
    CACHE_DISABLE_DATA();
    MMU_DISABLE();

    rc = send_cmd_core(cmd, arg, arglen, reply, replymax, replyalen);

    CACHE_FLUSH();
    MMU_RESTORE();
    CACHE_RESTORE_STATE();
    INTERRUPTS_ENABLE();
    SUPERVISOR_STATE_EXIT();
    return (rc);
}

/*
 * send_cmd_retry
 * --------------
 * Send a request to Kicksmash, retrying up to 5 times on error.
 */
uint
send_cmd_retry(uint16_t cmd, void *arg, uint16_t arglen,
               void *reply, uint replymax, uint *replyalen)
{
    uint tries = 5;
    uint rc;

    do {
        rc = send_cmd(cmd, arg, arglen, reply, replymax, replyalen);
        if ((rc != MSG_STATUS_BAD_CRC) &&
            (rc != MSG_STATUS_NO_REPLY) &&
            (rc != KS_STATUS_CRC)) {
            break;
        }
    } while (--tries > 0);
    return (rc);
}

/*
 * msg_init
 * --------
 * Initializes the KickSmash message interface.
 */
void
msg_init(void)
{
    cpu_control_init();
}

/*
 * recv_msg
 * --------
 * Receives a message from the remote USB Host via KickSmash.
 * buf is a pointer to a buffer where the received message will be stored.
 * len is the length of the receive message buffer.
 * rlen is the actual received message length, filled in by this function.
 * timeout_ms is the number of milliseconds to wait for a message to
 *     arrive before returning with a timeout failure.
 *
 * This function will return KS_STATUS_NODATA on timeout.
 */
uint
recv_msg(void *buf, uint len, uint *rlen, uint timeout_ms)
{
    uint rc;
    rc = send_cmd_retry(KS_CMD_MSG_RECEIVE, NULL, 0, buf, len, rlen);
    if (timeout_ms > 3000)
        timeout_ms = 3000;  // cap at 3 seconds
    while (rc == KS_STATUS_NODATA) {
        cia_spin(CIA_USEC(300));
        rc = send_cmd_retry(KS_CMD_MSG_RECEIVE, NULL, 0, buf, len, rlen);
        if (timeout_ms-- == 0)
            break;
    }
    if (rc == KS_CMD_MSG_SEND)
        rc = KM_STATUS_OK;
    return (rc);
}


#ifdef STANDALONE
typedef struct {
    uint16_t          ms_tag;         // Next available tag
    uint16_t          ms_tag_count;   // Outstanding tags
} msg_semaphore_t;

typedef struct {
    uint16_t mb_len;                     // Length of message
    uint16_t mb_tag;                     // Message tag
    uint32_t mb_data[MSG_BUF_SIZE / 4];  // Message (km_msg_hdr_t first)
} msg_buf_t;

static msg_semaphore_t *msg_sem;
static msg_semaphore_t  msg_sem_data;

/* Standalone is single-threaded: no locking required */
#define MSG_SEM_OBTAIN()
#define MSG_SEM_RELEASE()

/*
 * Standalone (ROM Switcher) never uses more than one buffer, and never
 * hands off messages for other tags.
 */
static msg_buf_t msg_standalone_buf;

static msg_buf_t *
msgbuf_get(void)
{
    return (&msg_standalone_buf);
}

static void
msgbuf_put(msg_buf_t *mb)
{
    (void) mb;
}

static void
msgbuf_hold(msg_buf_t *mb)
{
    (void) mb;
}

static void
host_msg_init(void)
{
    if (msg_sem != NULL)
        return;
    msg_sem = &msg_sem_data;
}

/* Standalone has no tag queue and no network receive tags */
uint
host_msg_tag_queued(uint tag)
{
    (void) tag;
    return (0);
}

void
host_msg_set_nread_tags(uint cur, uint cur_valid, uint prev, uint prev_valid)
{
    (void) cur;
    (void) cur_valid;
    (void) prev;
    (void) prev_valid;
}

#else  /* !STANDALONE */

#define TAG_QUEUE_ANY   ((uint)-1)  // Give any queued message
#define TAG_EXPUNGE_AGE 0x30        // Queued tags older than this are stale

/*
 * Each task may hold this many delivered message buffers. A message
 * returned by host_recv_msg() remains valid until the same task has
 * received this many more messages.
 */
#define MSG_HOLD_PER_TASK 4

/* Spare buffers kept on the shared free list (excess are freed) */
#define MSG_FREE_MAX      8

/* msg_buf_t and msg_semaphore_t are defined in sm_msg.h */

static msg_semaphore_t *msg_sem;
static const char       semaphore_name[] = "smashmsg";

/*
 * The address of msg_prog_id identifies this program (or device) in
 * mb_prog of held buffers, so that host_msg_exit() can return every
 * buffer held by any of this program's tasks.
 */
static uint8_t          msg_prog_id;
#define MSG_PROG_ID     ((void *) &msg_prog_id)

#define MSG_SEM_OBTAIN()  ObtainSemaphore(&msg_sem->ms_sem)
#define MSG_SEM_RELEASE() ReleaseSemaphore(&msg_sem->ms_sem)

#define LIST(x) ((struct List *) &(x))

/*
 * msgbuf_put_locked() returns a message buffer to the shared free list,
 *                     or frees it if the free list is full. Must be
 *                     called with ms_queue_lock held.
 */
static void
msgbuf_put_locked(msg_buf_t *mb)
{
    if (msg_sem->ms_free_count < MSG_FREE_MAX) {
        mb->mb_owner = NULL;
        mb->mb_prog  = NULL;
        AddHead(LIST(msg_sem->ms_free_list), (struct Node *) mb);
        msg_sem->ms_free_count++;
    } else {
        FreeMem(mb, sizeof (*mb));
    }
}

/*
 * msgbuf_put() returns a message buffer to the shared free list.
 *              This function is thread-safe.
 */
static void
msgbuf_put(msg_buf_t *mb)
{
    ObtainSemaphore(&msg_sem->ms_queue_lock);
    msgbuf_put_locked(mb);
    ReleaseSemaphore(&msg_sem->ms_queue_lock);
}

/*
 * msgbuf_get() takes a message buffer from the shared free list, or
 *              allocates a new one if the free list is empty.
 *              Returns NULL if no memory is available.
 *              This function is thread-safe.
 */
static msg_buf_t *
msgbuf_get(void)
{
    msg_buf_t *mb;

    ObtainSemaphore(&msg_sem->ms_queue_lock);
    mb = (msg_buf_t *) RemHead(LIST(msg_sem->ms_free_list));
    if (mb != NULL)
        msg_sem->ms_free_count--;
    ReleaseSemaphore(&msg_sem->ms_queue_lock);

    if (mb == NULL)
        mb = AllocMem(sizeof (*mb), MEMF_PUBLIC);
    return (mb);
}

/*
 * msgbuf_hold() records a message buffer as delivered to (held by) the
 *               current task. The task's oldest held buffers beyond
 *               MSG_HOLD_PER_TASK are returned to the free list.
 *               This function is thread-safe.
 */
static void
msgbuf_hold(msg_buf_t *mb)
{
    struct Task *me = FindTask(NULL);
    msg_buf_t   *node;
    msg_buf_t   *next_node;
    uint         count = 0;

    mb->mb_owner = me;
    mb->mb_prog  = MSG_PROG_ID;

    ObtainSemaphore(&msg_sem->ms_queue_lock);
    AddTail(LIST(msg_sem->ms_held_list), (struct Node *) mb);

    /* Count this task's held buffers (list is oldest first) */
    for (node = (msg_buf_t *) msg_sem->ms_held_list.mlh_Head;
         node->mb_node.mln_Succ != NULL;
         node = (msg_buf_t *) node->mb_node.mln_Succ) {
        if ((node->mb_owner == me) && (node->mb_prog == MSG_PROG_ID))
            count++;
    }

    /* Recycle the oldest ones beyond the limit */
    for (node = (msg_buf_t *) msg_sem->ms_held_list.mlh_Head;
         (count > MSG_HOLD_PER_TASK) &&
         ((next_node = (msg_buf_t *) node->mb_node.mln_Succ) != NULL);
         node = next_node) {
        if ((node->mb_owner == me) && (node->mb_prog == MSG_PROG_ID)) {
            Remove((struct Node *) node);
            msgbuf_put_locked(node);
            count--;
        }
    }
    ReleaseSemaphore(&msg_sem->ms_queue_lock);
}

/*
 * msgbuf_release_held() returns every buffer held by any task of this
 *                       program to the free list. Called when the
 *                       program is exiting.
 */
static void
msgbuf_release_held(void)
{
    msg_buf_t *node;
    msg_buf_t *next_node;

    ObtainSemaphore(&msg_sem->ms_queue_lock);
    for (node = (msg_buf_t *) msg_sem->ms_held_list.mlh_Head;
         (next_node = (msg_buf_t *) node->mb_node.mln_Succ) != NULL;
         node = next_node) {
        if (node->mb_prog == MSG_PROG_ID) {
            Remove((struct Node *) node);
            msgbuf_put_locked(node);
        }
    }
    ReleaseSemaphore(&msg_sem->ms_queue_lock);
}

/*
 * tag_queue_add() adds a received message buffer to the end of the
 *                 reply queue, for the task which owns the tag.
 *                 This function is thread-safe.
 */
static void
tag_queue_add(msg_buf_t *mb, uint16_t tag, uint len)
{
    mb->mb_tag = tag;  // Message tag
    mb->mb_len = len;  // Message size

    ObtainSemaphore(&msg_sem->ms_queue_lock);
    AddTail(LIST(msg_sem->ms_queue_list), (struct Node *) mb);
    ReleaseSemaphore(&msg_sem->ms_queue_lock);
}

/*
 * tag_is_nread() reports whether the tag is one of the outstanding network
 *                receive (KM_OP_NREAD) tags. Must be called with
 *                ms_queue_lock held.
 */
static int
tag_is_nread(uint16_t tag)
{
    return ((msg_sem->ms_nread_tag_cur_valid &&
             (tag == msg_sem->ms_nread_tag_cur)) ||
            (msg_sem->ms_nread_tag_prev_valid &&
             (tag == msg_sem->ms_nread_tag_prev)));
}

/*
 * tag_queue_remove() will search for and remove a message buffer by its
 *                    tag. Old tags will be automatically expunged, except
 *                    for the outstanding network receive tags, which live
 *                    much longer than other tags.
 *                    This function is thread-safe.
 */
static msg_buf_t *
tag_queue_remove(uint target_tag)
{
    msg_buf_t *node;
    msg_buf_t *next_node;
    msg_buf_t *found_node = NULL;

    ObtainSemaphore(&msg_sem->ms_queue_lock);

    for (node = (msg_buf_t *) msg_sem->ms_queue_list.mlh_Head;
         (next_node = (msg_buf_t *) node->mb_node.mln_Succ) != NULL;
         node = next_node) {
        if ((target_tag == TAG_QUEUE_ANY) ||  // Remove any
            (node->mb_tag == target_tag)) {
            /* Remove node from the list */
            Remove((struct Node *) node);
            found_node = node;
            break;
        }
        /*
         * Expunge only tags OLDER than the target by more than
         * TAG_EXPUNGE_AGE. A tag newer than the target (difference wraps
         * to 0x8000 or more) belongs to a transaction which started
         * after this one, so it must be kept. Otherwise a task searching
         * with a long-lived tag (such as a network receive tag) would
         * discard every other program's pending replies.
         */
        uint16_t age = (uint16_t) (target_tag - node->mb_tag);
        if ((target_tag != TAG_QUEUE_ANY) &&
            (age > TAG_EXPUNGE_AGE) && (age < 0x8000) &&
            !tag_is_nread(node->mb_tag)) {
            /* This node's tag is "too old" -- do garbage collection */
            printf("Expunge tag=%x (current: %x)\n", node->mb_tag, target_tag);
            Remove((struct Node *) node);
            msgbuf_put_locked(node);
        }
    }

    ReleaseSemaphore(&msg_sem->ms_queue_lock);
    return (found_node);
}

/*
 * host_msg_tag_queued() reports whether a message with the specified tag
 *                       is waiting in the shared reply queue (received
 *                       by some task, but not yet claimed).
 */
uint
host_msg_tag_queued(uint tag)
{
    msg_buf_t *node;
    uint       found = 0;

    if (msg_sem == NULL)
        return (0);

    ObtainSemaphore(&msg_sem->ms_queue_lock);
    for (node = (msg_buf_t *) msg_sem->ms_queue_list.mlh_Head;
         node->mb_node.mln_Succ != NULL;
         node = (msg_buf_t *) node->mb_node.mln_Succ) {
        if (node->mb_tag == (uint16_t) tag) {
            found = 1;
            break;
        }
    }
    ReleaseSemaphore(&msg_sem->ms_queue_lock);
    return (found);
}

/*
 * host_msg_set_nread_tags() publishes the network receive (KM_OP_NREAD)
 *                           tags in the shared structure, so that every
 *                           task's tag_queue_remove() keeps queued packets
 *                           for those tags instead of expunging them.
 */
void
host_msg_set_nread_tags(uint cur, uint cur_valid, uint prev, uint prev_valid)
{
    if (msg_sem == NULL)
        return;

    ObtainSemaphore(&msg_sem->ms_queue_lock);
    msg_sem->ms_nread_tag_cur        = cur;
    msg_sem->ms_nread_tag_cur_valid  = !!cur_valid;
    msg_sem->ms_nread_tag_prev       = prev;
    msg_sem->ms_nread_tag_prev_valid = !!prev_valid;
    ReleaseSemaphore(&msg_sem->ms_queue_lock);
}

/*
 * get_msg_semaphore() will locate or create the shared Kicksmash message
 *                     structure.
 */
static void
get_msg_semaphore(void)
{
    struct SignalSemaphore *sem;
    msg_semaphore_t *hdr = NULL;

    Forbid();  // Disable multitasking during discovery/creation

    sem = FindSemaphore((STRPTR) semaphore_name);

    if (sem != NULL) {
        /* Message semaphore already exists */
        hdr = (msg_semaphore_t *)sem;

        /*
         * Another program built with a different msg_semaphore_t layout
         * would corrupt this structure (or this program would corrupt
         * theirs). Refuse to share it. Change ms_version whenever the
         * structure layout changes.
         */
        if ((hdr->ms_version != MSG_SEM_VERSION) ||
            (hdr->ms_allocsize != sizeof (*hdr))) {
            Permit();
            printf("smashmsg version %u size %u incompatible with %u size %u; "
                   "rebuild all smash programs\n",
                   hdr->ms_version, hdr->ms_allocsize, MSG_SEM_VERSION,
                   (uint) sizeof (*hdr));
            msg_sem = NULL;
            return;
        }

        /*
         * Take the reference while still in Forbid(). Otherwise, the
         * last owner could free the structure before it is claimed here.
         * release_msg_semaphore() also adjusts refcount under Forbid().
         */
        hdr->ms_refcount++;
        Permit();
    } else {
        /* Not found: allocate new message semaphore */
        hdr = (msg_semaphore_t *)
              AllocMem(sizeof (msg_semaphore_t), MEMF_PUBLIC | MEMF_CLEAR);

        if (hdr != NULL) {
            /* Initialize main SignalSemaphore */
            InitSemaphore(&hdr->ms_sem);

            /* Initialize buffer list SignalSemaphore and lists */
            InitSemaphore(&hdr->ms_queue_lock);
            NewList(LIST(hdr->ms_queue_list));
            NewList(LIST(hdr->ms_free_list));
            NewList(LIST(hdr->ms_held_list));

            /* Structure may survive this program, so can't use constant */
            char *name = AllocMem(sizeof (semaphore_name), MEMF_PUBLIC);
            if (name == NULL) {
                FreeMem(hdr, sizeof (*hdr));
                Permit();
                msg_sem = NULL;
                return;
            }
            strcpy(name, semaphore_name);
            hdr->ms_sem.ss_Link.ln_Name = name;

            hdr->ms_sem.ss_Link.ln_Pri  = 0;

            /* Initialize custom header fields */
            hdr->ms_allocsize   = sizeof (*hdr);
            hdr->ms_version     = MSG_SEM_VERSION;
            hdr->ms_refcount    = 1;
            hdr->ms_tag         = 0;
            hdr->ms_free_count  = 0;

            /* Publish semaphore globally */
            AddSemaphore(&hdr->ms_sem);
        }
        Permit(); // Re-enable multitasking
    }

    msg_sem = hdr;
}

/*
 * free_msgbuf_list() frees every message buffer on the specified list.
 *                    Only used by the last owner of the shared structure.
 */
static void
free_msgbuf_list(MinList_t *list)
{
    msg_buf_t *mb;

    while ((mb = (msg_buf_t *) RemHead(LIST(*list))) != NULL)
        FreeMem(mb, sizeof (*mb));
}

/*
 * release_msg_semaphore() will release or remove the shared Kicksmash
 *                         message structure.
 */
static void
release_msg_semaphore(void)
{
    msg_semaphore_t *hdr = msg_sem;

    if (hdr == NULL)
        return;

    ObtainSemaphore(&hdr->ms_sem);
    Forbid();  // Disable multitasking before checking refcount

    if (hdr->ms_refcount > 0) {
        hdr->ms_refcount--;
    }

    if (hdr->ms_refcount == 0) {
        /*
         * Last owner exiting:
         * 1. Release local lock before removing from system lists.
         * 2. Remove the semaphore from Exec's public list.
         * 3. Allow multitasking to resume
         * 4. Release any unclaimed tag data and all other buffers
         * 5. Free the allocated memory.
         */
        ReleaseSemaphore(&hdr->ms_sem);
        RemSemaphore(&hdr->ms_sem);
        Permit();
        msg_buf_t *mb;
        while ((mb = tag_queue_remove(TAG_QUEUE_ANY)) != NULL) {
            km_msg_hdr_t *km = (km_msg_hdr_t *) mb->mb_data;
            printf("Removed %x tag=%x op=%x len=%x\n",
                   (uint) mb, km->km_tag, km->km_op, mb->mb_len);
            FreeMem(mb, sizeof (*mb));
        }
        free_msgbuf_list(&hdr->ms_free_list);
        free_msgbuf_list(&hdr->ms_held_list);
        char *name = hdr->ms_sem.ss_Link.ln_Name;
        FreeMem(name, strlen(name) + 1);
        FreeMem(hdr, sizeof (*hdr));
    } else {
        /* Other applications are still using the data structure */
        Permit();
        ReleaseSemaphore(&hdr->ms_sem);
    }

    msg_sem = NULL;  // This is a pointer to a static structure
}

/*
 * host_msg_exit() returns this program's message buffers and releases
 *                 the shared message structure when the program is
 *                 exiting. Safe to call more than once.
 */
void
host_msg_exit(void)
{
    if (msg_sem == NULL)
        return;  // Not acquired (or already released)

    msgbuf_release_held();
    release_msg_semaphore();
}

/*
 * host_msg_init() acquires the shared message structure when the program
 *                 starts.
 */
static void
host_msg_init(void)
{
    static uint8_t atexit_registered = 0;

    if (msg_sem != NULL)
        return;

    if (atexit_registered == 0) {
        atexit_registered = 1;
        atexit(host_msg_exit);
    }
    get_msg_semaphore();

    if (msg_sem != NULL) {
        hm_version_t msg;
        hm_version_t *rmsg;
        uint         rlen;
        uint         rc;

        msg.hm_hdr.km_op     = KM_OP_VERSION;
        msg.hm_hdr.km_status = 0;
        msg.hm_version       = HOST_INTERFACE_VERSION;
        msg.hm_unused[0]     = 0;
        msg.hm_unused[1]     = 0;
        msg.hm_unused[2]     = 0;
        rc = host_msg(&msg, sizeof (msg), (void **) &rmsg, &rlen);
        if (rc == KM_STATUS_OK) {
            host_interface_version = rmsg->hm_version;
        }
    }
}

#endif /* !STANDALONE */

/*
 * host_tag_alloc
 * --------------
 * Allocate and return a new host message tag. See host_tag_free().
 */
uint
host_tag_alloc(void)
{
    /* Allocator just uses a simple circular counter */
    uint16_t newtag;

    host_msg_init();
    if (msg_sem == NULL)
        return (0);  // Message interface unavailable (no memory)
    Forbid();
    newtag = (msg_sem->ms_tag)++;
    msg_sem->ms_tag_count++;      // Should be 0 when idle
    Permit();
    return (newtag);
}

/*
 * host_tag_free
 * -------------
 * Deallocate the specified host message tag.
 *
 * tag is the message tag to deallocate.
 */
void
host_tag_free(uint tag)
{
    /* Allocator just uses a circular counter, so no need to release */
    (void) tag;
    if (msg_sem == NULL)
        return;
    Forbid();
    msg_sem->ms_tag_count--;
    Permit();
}

#define SEND_MSG_MAX 2000

/*
 * host_send_msg
 * -------------
 * Send a message to the USB Host. If the message is larger than the
 * maximum message size (SEND_MAX_MAX), it will be automatically broken
 * and streamed in units of the maximum size. It's important to
 * understand that only messages where the receiving side will know the
 * size of the entire message should send messages larger than
 * SEND_MSG_MAX. This can be accomplished by including the complete
 * message length in the message header (for example hm_freadwrite_t).
 *
 * smsg is the message to send.
 * len is the length of the message to send.
 */
uint
host_send_msg(void *smsg, uint len)
{
    uint8_t savebuf[sizeof (km_msg_hdr_t)];
    uint32_t rbuf[16];
    uint sendlen = len;
    uint timeout = 0;
    uint pos;
    uint rc;

    host_msg_init();
    if (msg_sem == NULL)
        return (MSG_STATUS_NO_MEM);

    if (sendlen > SEND_MSG_MAX)
        sendlen = SEND_MSG_MAX;

try_send_again:
    rc = send_cmd_retry(KS_CMD_MSG_SEND, smsg, sendlen, rbuf, sizeof (rbuf),
                        NULL);
    if (rc == KS_STATUS_BADLEN) {
        /* Not enough space in the KS buffer; try again. */
        if (timeout++ < 500) {
            cia_spin(CIA_USEC(500));
            goto try_send_again;
        }
        pos = 0;
        printf("send msg buffer timeout at pos=%x of %x: %s\n",
               pos, len, smash_err(rc));
    }
    if ((rc == 0) && (sendlen < len)) {
        timeout = 0;
        pos = sendlen - sizeof (km_msg_hdr_t);

        while (pos < len - sizeof (km_msg_hdr_t)) {
            if (sendlen > len - pos)
                sendlen = len - pos;

            memcpy(savebuf, smsg + pos, sizeof (km_msg_hdr_t));
            memcpy(smsg + pos, smsg, sizeof (km_msg_hdr_t));

#undef DEBUG_SEND_MSG
#ifdef DEBUG_SEND_MSG
            printf("send %x pos=%x of %x\n", sendlen, pos, len);
#endif
            rc = send_cmd_retry(KS_CMD_MSG_SEND, smsg + pos, sendlen,
                                NULL, 0, NULL);
            memcpy(smsg + pos, savebuf, sizeof (km_msg_hdr_t));

            if (rc == KS_STATUS_BADLEN) {
                /* Not enough space in the KS buffer; try again. */
                if (timeout++ < 500) {
                    cia_spin(CIA_USEC(500));
                    continue;
                }
                printf("send msg buffer timeout at pos=%x of %x: %s\n",
                       pos, len, smash_err(rc));
                break;
            }
            if (rc != 0) {
                printf("send msg failed at pos=%x of %x: %s\n",
                       pos, len, smash_err(rc));
                break;
            }
            timeout = 0;
            pos += sendlen - sizeof (km_msg_hdr_t);
        }
    }
    if (rc != 0) {
        printf("Send message l=%u failed: (%s)\n",
               len, smash_err(rc));
#ifndef ROMFS
        if (flag_debug > 2)
            dump_memory(rbuf, sizeof (rbuf), DUMP_VALUE_UNASSIGNED);
#endif
    }
    return (rc);
}

#define MSG_RECV_HANDOFF_MAX 50  // Messages for other tags per attempt

/*
 * host_recv_msg_try
 * -----------------
 * Receive attempt for host_recv_msg() and host_recv_msg_noblock(). A
 * message already queued for the tag is taken first. Otherwise, a buffer
 * is taken from the shared free list and a message is received from
 * Kicksmash into it. A received message which is for a different tag is
 * handed off (the buffer itself is moved to the reply queue), and another
 * receive is attempted with a fresh buffer. This repeats only while
 * messages for other tags are being received.
 *
 * A message for the tag is delivered in a buffer which is then held by
 * the calling task (see msgbuf_hold()).
 *
 * Returns KS_STATUS_NODATA (or KS_STATUS_CRC) if no message for the tag
 * was received.
 */
static uint
host_recv_msg_try(uint tag, void **rdata, uint *rlen, uint timeout_ms)
{
    msg_buf_t    *mb = NULL;
    km_msg_hdr_t *msg;
    uint rc;
    uint rxlen;
    uint count;

    for (count = 0; count < MSG_RECV_HANDOFF_MAX; count++) {
        MSG_SEM_OBTAIN();
#ifndef STANDALONE
        msg_buf_t *qmb = tag_queue_remove(tag);
        if (qmb != NULL) {
            /* Already received a message matching the expected tag */
            if (mb != NULL)
                msgbuf_put(mb);  // Receive buffer is not needed
            mb    = qmb;
            rxlen = mb->mb_len;
            msg   = (km_msg_hdr_t *) mb->mb_data;
            rc    = msg->km_status;
        } else
#endif
        {
            if (mb == NULL) {
                mb = msgbuf_get();
                if (mb == NULL) {
                    MSG_SEM_RELEASE();
                    printf("Get message tag=%x: out of memory\n", tag);
                    return (MSG_STATUS_NO_MEM);
                }
            }
            msg = (km_msg_hdr_t *) mb->mb_data;
            rc = recv_msg(msg, MSG_BUF_SIZE, &rxlen, timeout_ms);
            if ((rc == KS_STATUS_NODATA) || (rc == KS_STATUS_CRC)) {
                /*
                 * Nothing received. Release here and never fall through
                 * to the code below, which also releases the semaphore
                 * (releasing twice gives Guru 81000008 AN_SemCorrupt).
                 */
                MSG_SEM_RELEASE();
                msgbuf_put(mb);
                return (rc);
            }
            if (((rc & 0x0000ff00) != KS_STATUS_OK) &&
                ((rc & 0xffff0000) == MSG_STATUS_SUCCESS)) {
                /*
                 * One of the following was received from Kicksmash (not host):
                 * KS_STATUS_FAIL, KS_STATUS_UNKCMD, KS_STATUS_BADARG,
                 * KS_STATUS_BADLEN, KS_STATUS_LOCKED.
                 * These are all fatal, except maybe KS_STATUS_LOCKED.
                 * Don't match its tag or queue it for another task.
                 */
                MSG_SEM_RELEASE();
                msgbuf_put(mb);
                printf("Get message tag=%x failed: (%s)\n", tag, smash_err(rc));
                return (rc);
            }
        }
        if (tag == msg->km_tag) {
            MSG_SEM_RELEASE();
            if ((rc != KM_STATUS_OK) && (rc != KM_STATUS_EOF)) {
                printf("Get message tag=%x failed: (%s)\n", tag, smash_err(rc));
#ifndef ROMFS
                if (flag_debug > 2)
                    dump_memory(msg, 0x40, DUMP_VALUE_UNASSIGNED);
#endif
                msgbuf_put(mb);
                return (rc);
            }
            /* Got desired message */
            if (rxlen > MSG_BUF_SIZE) {
                printf("BUG: Rx message op=%x stat=%x too large (%u > %u)\n",
                       msg->km_op, msg->km_status, rxlen, MSG_BUF_SIZE);
                rxlen = MSG_BUF_SIZE;
            }
            *rlen = rxlen;
            *rdata = msg;
            if (rc == KM_STATUS_OK)
                rc = msg->km_status;
            msgbuf_hold(mb);  // Valid until this task receives more messages
            return (rc);
        }
        /* Hand off this message as it's not for the current caller */

#ifndef STANDALONE
        /* The buffer now belongs to the reply queue; get a new one next pass */
        tag_queue_add(mb, msg->km_tag, rxlen);
        mb = NULL;
#endif
        MSG_SEM_RELEASE();
    }
    if (mb != NULL)
        msgbuf_put(mb);
    return (KS_STATUS_NODATA);  // Only messages for other tags arrived
}

/*
 * host_recv_msg_noblock
 * ---------------------
 * Receive a single message for the specified tag without waiting for one
 * to arrive. Messages received for other tags are queued for their owners.
 * Returns KS_STATUS_NODATA (or KS_STATUS_CRC) if no message for the tag
 * is available; otherwise the same as host_recv_msg().
 */
uint
host_recv_msg_noblock(uint tag, void **rdata, uint *rlen)
{
    if (msg_sem == NULL)
        return (MSG_STATUS_NO_MEM);

    return (host_recv_msg_try(tag, rdata, rlen, 0));
}

/*
 * host_recv_msg
 * -------------
 * Receive a single message from the USB host, returning a pointer to the
 * buffer containing the message content.
 *
 * tag is the unique message tag for this transaction; see host_tag_alloc().
 * rdata is a pointer which will be assigned the address where the received
 *     message will be returned. The caller does not free it. It remains
 *     valid until the calling task has received MSG_HOLD_PER_TASK more
 *     messages (or the program exits).
 * rlen is a pointer to the received data length which will be returned.
 */
uint
host_recv_msg(uint tag, void **rdata, uint *rlen)
{
    uint rc;
    uint timeout;

    if (msg_sem == NULL)
        return (MSG_STATUS_NO_MEM);

    for (timeout = 50; timeout > 0; timeout--) {
        rc = host_recv_msg_try(tag, rdata, rlen, 25);
        if ((rc != KS_STATUS_NODATA) && (rc != KS_STATUS_CRC))
            return (rc);
        /* Try again, letting other tasks progress */
    }
    printf("Message receive timeout\n");
    return (KM_STATUS_FAIL);
}

/*
 * host_recv_msg_cont
 * ------------------
 * Continue the previous message receive, stripping the header and just
 * copying data to the specified buffer.
 *
 * tag is the unique message tag for this transaction; see host_tag_alloc().
 *     It should be the same tag which was used to receive the lead message.
 * buf is a pointer to the buffer for the remaining message payload.
 * buf_len is the number of bytes to receive, across however many messages
 *     is takes to receive them.
 */
uint
host_recv_msg_cont(uint tag, void *buf, uint buf_len)
{
    uint          rcvlen;
    uint          cur_len = 0;
    uint          rc;
    uint          seq = 1;
    km_msg_hdr_t *rdata;

    while (cur_len < buf_len) {
        rc = host_recv_msg(tag, (void **) &rdata, &rcvlen);
        if (rc == KM_STATUS_EOF)
            rc = KM_STATUS_OK;
        if (rc != KM_STATUS_OK) {
            printf("next pkt failed at %u of %u: %s\n",
                   cur_len, buf_len, smash_err(rc));
            return (rc);
        }
        if (host_interface_version > 0) {
            if (rdata->km_op != seq) {
                if (rdata->km_op < seq) {
                    /* Repeat message can be dropped */
                    printf("Message tag=%x sequence %x dup: %x\n",
                           tag, seq, rdata->km_op);
                    continue;
                } else {
                    /* Skipped message(s) */
                    printf("Message tag=%x sequence skipped: %x to %x (%x)\n",
                           tag, seq, rdata->km_op, (uintptr_t) rdata);
                    return (KM_STATUS_FAIL);
                }
            }
            seq++;
        }
        if (rcvlen + cur_len > buf_len + sizeof (km_msg_hdr_t)) {
            printf("Message tag=%x next pkt bad rcvlen %x "
                   "curlen %x buflen %x\n",
                   tag, rcvlen, cur_len, buf_len);
            return (KM_STATUS_FAIL);
        }
        if (rcvlen >= sizeof (km_msg_hdr_t))
            rcvlen -= sizeof (km_msg_hdr_t);
        else
            rcvlen = 0;

        memcpy(buf + cur_len, rdata + 1, rcvlen);
        cur_len += rcvlen;
    }
    return (KM_STATUS_OK);
}

/*
 * host_msg_tag
 * ------------
 * Send a message and wait for a single reply message. This function will
 * only return the first message of a multiple message reply. If there is
 * further data pending, use host_recv_msg_cont() to receive the remaining
 * message data.
 *
 * smsg is the message to send.
 * slen is the length of the message to send.
 * rdata will be assigned a pointer to the received data. The caller is
 *     is not responsible for allocating or freeing the returned buffer.
 * rlen will be assigned the length of the received message.
 */
uint
host_msg_tag(void *smsg, uint slen, void **rdata, uint *rlen)
{
    km_msg_hdr_t *smsg_h = (km_msg_hdr_t *) smsg;
    uint rc = host_send_msg(smsg, slen);
    if (rc != 0)
        return (rc);
    return (host_recv_msg(smsg_h->km_tag, rdata, rlen));
}

/*
 * host_msg
 * --------
 * Send a message and wait for a simple reply message. This automatically
 * allocates and frees a message tag, so it does not handle messages
 * requiring continuation. Use host_msg_tag() to send and receive large
 * messages while repeatedly using the same tag.
 */
uint
host_msg(void *smsg, uint slen, void **rdata, uint *rlen)
{
    uint     rc;
    uint16_t tag = host_tag_alloc();

    ((km_msg_hdr_t *) smsg)->km_tag = tag;
    rc = host_msg_tag(smsg, slen, rdata, rlen);
    host_tag_free(tag);
    return (rc);
}

static const char *const ks_status_s[] = {
    "OK",                               // 0x00000000 KS_STATUS_OK
    "KS Failure",                       // 0x00000100 KS_STATUS_FAIL
    "KS reports CRC bad",               // 0x00000200 KS_STATUS_CRC
    "KS detected unknown command",      // 0x00000300 KS_STATUS_UNKCMD
    "KS reports bad command argument",  // 0x00000400 KS_STATUS_BADARG
    "KS reports bad length",            // 0x00000500 KS_STATUS_BADLEN
    "KS reports no data available",     // 0x00000600 KS_STATUS_NODATA
    "KS reports resource locked",       // 0x00000700 KS_STATUS_LOCKED
};
STATIC_ASSERT(ARRAY_SIZE(ks_status_s) == (KS_STATUS_LAST_ENT >> 8));

static const char *const km_status_s[] = {
    "OK",                               // 0x00000000 KM_STATUS_OK
    "FAIL",                             // 0x00000001 KM_STATUS_FAIL
    "EOF",                              // 0x00000002 KM_STATUS_EOF
    "UNKCMD",                           // 0x00000003 KM_STATUS_UNKCMD
    "PERM",                             // 0x00000004 KM_STATUS_PERM
    "INVALID",                          // 0x00000005 KM_STATUS_INVALID
    "NOTEMPTY",                         // 0x00000006 KM_STATUS_NOTEMPTY
    "NOEXIST",                          // 0x00000007 KM_STATUS_NOEXIST
    "EXIST",                            // 0x00000008 KM_STATUS_EXIST
    "UNAVAIL"                           // 0x00000009 KM_STATUS_UNAVAIL
};
STATIC_ASSERT(ARRAY_SIZE(km_status_s) == KM_STATUS_LAST_ENTRY);

static const char *const msg_status_s[] = {
    "Msg Failure",                      // 0xfffffffa MSG_STATUS_FAIL
    "Msg No Reply",                     // 0xfffffff9 MSG_STATUS_NO_REPLY
    "Msg detected bad length",          // 0xfffffff8 MSG_STATUS_BAD_LENGTH
    "Msg detected bad CRC",             // 0xfffffff7 MSG_STATUS_BAD_CRC
    "Msg Invalid data",                 // 0xfffffff6 MSG_STATUS_BAD_DATA
    "Msg Program/erase timeout",        // 0xfffffff5 MSG_STATUS_PRG_TMOUT
    "Msg Program/erase failure",        // 0xfffffff4 MSG_STATUS_PRG_FAIL
    "Msg Insufficient memory",          // 0xfffffff3 MSG_STATUS_NO_MEM
    "Msg Data mismatch",                // 0xfffffff2 MSG_STATUS_MISMATCH
};
STATIC_ASSERT(ARRAY_SIZE(msg_status_s) ==
              (MSG_STATUS_FAIL - MSG_STATUS_LAST_ENTRY));

/*
 * smash_err
 * ---------
 * Converts KS_STATUS, KM_STATUS, or MSG_STATUS value to a readable string
 *
 * status is the error status code to convert.
 */
const char *
smash_err(uint status)
{
    uint        ks_status_v  = status >> 8;
    uint        msg_status_v = (~status) - (~MSG_STATUS_FAIL);
    static char buf[64];
    const char *str = "Unknown";

    if (status < ARRAY_SIZE(km_status_s))
        str = km_status_s[status];
    else if (ks_status_v < ARRAY_SIZE(ks_status_s))
        str = ks_status_s[ks_status_v];
    else if (msg_status_v < ARRAY_SIZE(msg_status_s))
        str = msg_status_s[msg_status_v];
    sprintf(buf, "%d %s", status, str);
    return (buf);
}
