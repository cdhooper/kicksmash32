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
#include "crc32.h"
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
 *        0x0117, 0x0119, 0x1017, 0x0204
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


#define MSG_POOL_COUNT    (4)
#define MSG_POOL_BUF_SIZE (4200)
static void            *msg_pool[4];
static volatile uint8_t msg_pool_cur;

#ifdef STANDALONE
typedef struct {
    uint16_t          ms_tag;         // Next available tag
    uint16_t          ms_tag_count;   // Outstanding tags
} msg_semaphore_t;

static msg_semaphore_t *msg_sem;
static msg_semaphore_t  msg_sem_data;

static void
host_msg_init(void)
{
    /* Standalone (ROM Switcher) never uses more than one buffer */
    static uint8_t buf[4200];
    uint cur;

    if (msg_sem != NULL)
        return;
    msg_sem = &msg_sem_data;

    for (cur = 0; cur < MSG_POOL_COUNT; cur++)
        msg_pool[cur] = buf;
}

#else  /* !STANDALONE */

#define TAG_QUEUE_ANY ((uint)-1)  // Give any queued message

typedef struct SignalSemaphore SignalSemaphore_t;
typedef struct MinNode         MinNode_t;
typedef struct MinList         MinList_t;
typedef struct {
    MinNode_t  mrq_Node;    // Embedded node for list management
    void      *mrg_buf;     // Buffer (might be exchanged)
    uint16_t   mrg_len;     // Length of message (NOT buffer size)
    uint16_t   mrg_tag;     // Desired reply tag
} msg_read_queue_t;

typedef struct {
    SignalSemaphore_t ms_sem;         // Semaphore MUST be the first member
    uint16_t          ms_version;     // Struct version
    uint16_t          ms_tag;         // Next available tag
    uint16_t          ms_tag_count;   // Outstanding tags
    uint16_t          ms_refcount;    // Number of tasks using this struct
    uint16_t          ms_busy;        // Reader busy
    uint16_t          ms_allocsize;   // Size of struct
    SignalSemaphore_t ms_queue_lock;  // Pending reply queue lock
    MinList_t         ms_queue_list;  // Pending reply queue head
    void             *ms_free_msgbuf; // Spare message buffer for exchange
} msg_semaphore_t;

static msg_semaphore_t *msg_sem;
static const char       semaphore_name[] = "smashmsg";

/*
 * tag_queue_add() adds a node to the end of the message queue.
 *                 This function is thread-safe.
 */
static void
tag_queue_add(uint16_t tag, void *buf, uint len)
{
    msg_read_queue_t *node = AllocMem(sizeof (*node), MEMF_PUBLIC);

    if (node == NULL) {
        /* No memoory: Discard message to avoid leaking memory */
        printf("Out of mem\n");
        FreeMem(buf, MSG_POOL_BUF_SIZE);
        return;
    }
    node->mrg_tag = tag;  // Message tag
    node->mrg_buf = buf;  // Message header
    node->mrg_len = len;  // Message size

    ObtainSemaphore(&msg_sem->ms_queue_lock);

    /* AddTail operates on MinList/MinNode via typecasting */
    AddTail((struct List *)&msg_sem->ms_queue_list, (struct Node *)node);

    ReleaseSemaphore(&msg_sem->ms_queue_lock);
}

/*
 * tag_queue_remove() will search for and remove a node by its tag field
 *                    Old tags will be automatically expunged.
 *                    This function is thread-safe.
 */
static void *
tag_queue_remove(uint target_tag, uint *rxlen)
{
    msg_read_queue_t *node;
    msg_read_queue_t *next_node;
    msg_read_queue_t *found_node = NULL;

    ObtainSemaphore(&msg_sem->ms_queue_lock);

    /* Safe iteration macro using MinNode pointers */
    for (node = (msg_read_queue_t *)msg_sem->ms_queue_list.mlh_Head;
         (next_node = (msg_read_queue_t *)node->mrq_Node.mln_Succ);
         node = next_node)
    {
        if ((target_tag == TAG_QUEUE_ANY) ||  // Remove any
            (node->mrg_tag == target_tag)) {
            /* Remove node from the list */
            Remove((struct Node *)node);
            found_node = node;
            break;
        }
        if ((target_tag != TAG_QUEUE_ANY) &&
            ((uint16_t) (target_tag - node->mrg_tag) > 0x30)) {
            /* This node's tag is "too old" -- do garbage collection */
            printf("Expunge tag %x (current: %x)\n", node->mrg_tag, target_tag);
            Remove((struct Node *)node);
        }
    }

    ReleaseSemaphore(&msg_sem->ms_queue_lock);
    if (found_node != NULL) {
        void *found_buf = found_node->mrg_buf;
        *rxlen = found_node->mrg_len;
        FreeMem(found_node, sizeof (*found_node));
        return (found_buf);
    } else {
        return (NULL);
    }
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

        Permit();  // Allow multitasking before possibly blocking
        ObtainSemaphore(sem);
        hdr->ms_refcount++;
        ReleaseSemaphore(sem);
    } else {
        /* Not found: allocate new message semaphore */
        hdr = (msg_semaphore_t *)
              AllocMem(sizeof (msg_semaphore_t), MEMF_PUBLIC | MEMF_CLEAR);

        if (hdr != NULL) {
            /* Initialize main SignalSemaphore */
            InitSemaphore(&hdr->ms_sem);

            /* Initialize tag queue SignalSemaphore */
            InitSemaphore(&hdr->ms_queue_lock);
            NewList((struct List *)&hdr->ms_queue_list);

            /* Structure may survive this program, so can't use constant */
            char **name = &hdr->ms_sem.ss_Link.ln_Name;
            *name = AllocMem(sizeof (semaphore_name), MEMF_PUBLIC);
            strcpy(*name, semaphore_name);

            hdr->ms_sem.ss_Link.ln_Pri  = 0;

            /* Initialize custom header fields */
            hdr->ms_allocsize   = sizeof (*hdr);
            hdr->ms_version     = 1;
            hdr->ms_refcount    = 1;
            hdr->ms_tag         = 0;
            hdr->ms_free_msgbuf = NULL;

            /* Publish semaphore globally */
            AddSemaphore(&hdr->ms_sem);
        }
        Permit(); // Re-enable multitasking
    }

    msg_sem = hdr;
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
         * 4. Release any unclaimed tag data
         * 5. Free the allocated memory.
         */
        ReleaseSemaphore(&hdr->ms_sem);
        RemSemaphore(&hdr->ms_sem);
        Permit();
        void *data;
        uint len;
        while ((data = tag_queue_remove(TAG_QUEUE_ANY, &len)) != NULL) {
            km_msg_hdr_t *hdr = data;
            printf("Removed %x tag %x op %x len %x\n",
                   data, hdr->km_tag, hdr->km_op, len);
            FreeMem(data, MSG_POOL_BUF_SIZE);
        }
        if (hdr->ms_free_msgbuf != NULL)
            FreeMem(hdr->ms_free_msgbuf, MSG_POOL_BUF_SIZE);
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
 * host_msg_exit() releases the message pool when the program is exiting.
 */
void
host_msg_exit(void)
{
    uint cur;

    if (msg_sem == NULL)
        return;

    release_msg_semaphore();
    for (cur = 0; cur < MSG_POOL_COUNT; cur++) {
        if (msg_pool[cur] != NULL) {
            FreeMem(msg_pool[cur], MSG_POOL_BUF_SIZE);
            msg_pool[cur] = NULL;
        }
    }
}

/*
 * host_msg_init() allocates the message pool when the program starts.
 */
static void
host_msg_init(void)
{
    uint cur;
    if (msg_sem != NULL)
        return;

    for (cur = 0; cur < MSG_POOL_COUNT; cur++) {
        msg_pool[cur] = AllocMem(MSG_POOL_BUF_SIZE, MEMF_PUBLIC);
        if (msg_pool[cur] == NULL) {
            while (cur > 0) {
                cur--;
                FreeMem(msg_pool[cur], MSG_POOL_BUF_SIZE);
                msg_pool[cur] = NULL;
            }
            return;
        }
    }

    atexit(host_msg_exit);
    get_msg_semaphore();

    if (msg_sem != NULL) {
        hm_version_t msg;
        hm_version_t *rmsg;
        uint8_t      tag = host_tag_alloc();
        uint         rlen;
        uint         rc;

        msg.hm_hdr.km_op     = KM_OP_VERSION;
        msg.hm_hdr.km_status = 0;
        msg.hm_hdr.km_tag    = host_tag_alloc();
        msg.hm_version       = HOST_INTERFACE_VERSION;
        msg.hm_unused[0]     = 0;
        msg.hm_unused[1]     = 0;
        msg.hm_unused[2]     = 0;
        rc = host_msg(&msg, sizeof (msg), (void **) &rmsg, &rlen);
        if (rc == KM_STATUS_OK) {
            host_interface_version = rmsg->hm_version;
        }
        host_tag_free(tag);
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

/*
 * host_recv_msg
 * -------------
 * Receive a single message from the USB host, returning a pointer to the
 * buffer containing the message content.
 *
 * tag is the unique message tag for this transaction; see host_tag_alloc().
 * rdata is a pointer which will be assigned the address where the received
 *     message will be returned.
 * rlen is a pointer to the received data length which will be returned.
 */
uint
host_recv_msg(uint tag, void **rdata, uint *rlen)
{
    km_msg_hdr_t *msg;
    uint rc;
    uint rxlen;
    uint timeout;
    uint msg_pool_pos;

    /* Acquire the next message buffer */
    Forbid();
    msg_pool_pos = msg_pool_cur;
    msg = (km_msg_hdr_t *) msg_pool[msg_pool_pos];
    msg_pool_cur = (msg_pool_cur + 1) & (MSG_POOL_COUNT - 1);
    Permit();

    for (timeout = 50; timeout > 0; timeout--) {
#ifndef STANDALONE
        km_msg_hdr_t *nmsg;
        ObtainSemaphore(&msg_sem->ms_sem);
        nmsg = tag_queue_remove(tag, &rxlen);
        if (nmsg != NULL) {
            /* Already received a message matching the expected tag */

            /* Release the local buffer */
            if (msg_sem->ms_free_msgbuf == NULL) {
                msg_sem->ms_free_msgbuf = msg;
            } else {
                FreeMem(msg, MSG_POOL_BUF_SIZE);
            }

            /* Store the retrieved message buffer in the local pool */
            msg_pool[msg_pool_pos] = nmsg;
            msg = nmsg;
            rc = msg->km_status;
        } else
#endif
        {
            rc = recv_msg(msg, MSG_POOL_BUF_SIZE, &rxlen, 25);  // 25 ms timeout
            if (rc == KS_STATUS_NODATA) {
                ReleaseSemaphore(&msg_sem->ms_sem);
                if (timeout > 1)
                    continue;  // Try again, letting other tasks progress
            }
            if (rc != KM_STATUS_OK) {
                printf("Get message tag %x failed: (%s)\n", tag, smash_err(rc));
#ifndef ROMFS
                if (flag_debug > 2)
                    dump_memory(msg, 0x40, DUMP_VALUE_UNASSIGNED);
#endif
                return (rc);
            }
        }
        if (tag == msg->km_tag) {
            if ((rc != KM_STATUS_OK) && (rc != KM_STATUS_EOF)) {
                ReleaseSemaphore(&msg_sem->ms_sem);
                return (rc);
            }
            /* Got desired message */
            if (rxlen > MSG_POOL_BUF_SIZE) {
                printf("BUG: Rx message op=%x stat=%x too large (%u > %u)\n",
                       msg->km_op, msg->km_status, rxlen, MSG_POOL_BUF_SIZE);
                rxlen = MSG_POOL_BUF_SIZE;
            }
            *rlen = rxlen;
            *rdata = msg;
            if (rc == KM_STATUS_OK)
                rc = msg->km_status;
            ReleaseSemaphore(&msg_sem->ms_sem);
            return (rc);
        }
        /* Hand off this message as it's not for the current caller */

#ifndef STANDALONE
        /* Allocate a new message buffer */
        if (msg_sem->ms_free_msgbuf != NULL) {
            nmsg = msg_sem->ms_free_msgbuf;
            msg_sem->ms_free_msgbuf = NULL;
        } else {
            nmsg = AllocMem(MSG_POOL_BUF_SIZE, MEMF_PUBLIC);
        }
        if (nmsg != NULL) {
            /* Hand off this message buffer */
            msg_pool[msg_pool_pos] = nmsg;
            tag_queue_add(msg->km_tag, msg, rxlen);
            msg = nmsg;
        }
        ReleaseSemaphore(&msg_sem->ms_sem);
#endif
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
                    printf("Message tag=%x sequence skipped: %x to %x\n",
                           tag, seq, rdata->km_op);
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
 * host_msg
 * --------
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
host_msg(void *smsg, uint slen, void **rdata, uint *rlen)
{
    km_msg_hdr_t *smsg_h = (km_msg_hdr_t *) smsg;
    uint rc = host_send_msg(smsg, slen);
    if (rc != 0)
        return (rc);
    return (host_recv_msg(smsg_h->km_tag, rdata, rlen));
}


static const char *const ks_status_s[] = {
    "OK",                               // KS_STATUS_OK
    "KS Failure",                       // KS_STATUS_FAIL
    "KS reports CRC bad",               // KS_STATUS_CRC
    "KS detected unknown command",      // KS_STATUS_UNKCMD
    "KS reports bad command argument",  // KS_STATUS_BADARG
    "KS reports bad length",            // KS_STATUS_BADLEN
    "KS reports no data available",     // KS_STATUS_NODATA
    "KS reports resource locked",       // KS_STATUS_LOCKED
};
STATIC_ASSERT(ARRAY_SIZE(ks_status_s) == (KS_STATUS_LAST_ENT >> 8));

static const char *const km_status_s[] = {
    "OK",                               // KM_STATUS_OK
    "FAIL",                             // KM_STATUS_FAIL
    "EOF",                              // KM_STATUS_EOF
    "UNKCMD",                           // KM_STATUS_UNKCMD
    "PERM",                             // KM_STATUS_PERM
    "INVALID",                          // KM_STATUS_INVALID
    "NOTEMPTY",                         // KM_STATUS_NOTEMPTY
    "NOEXIST",                          // KM_STATUS_NOEXIST
    "EXIST",                            // KM_STATUS_EXIST
    "UNAVAIL"                           // KM_STATUS_UNAVAIL
};
STATIC_ASSERT(ARRAY_SIZE(km_status_s) == KM_STATUS_LAST_ENTRY);

static const char *const msg_status_s[] = {
    "Msg Failure",                      // MSG_STATUS_FAIL
    "Msg No Reply",                     // MSG_STATUS_NO_REPLY
    "Msg detected bad length",          // MSG_STATUS_BAD_LENGTH
    "Msg detected bad CRC",             // MSG_STATUS_BAD_CRC
    "Msg Invalid data",                 // MSG_STATUS_BAD_DATA
    "Msg Program/erase timeout",        // MSG_STATUS_PRG_TMOUT
    "Msg Program/erase failure",        // MSG_STATUS_PRG_FAIL
    "Msg Insufficient memory",          // MSG_STATUS_NO_MEM
    "Msg Data mismatch",                // MSG_STATUS_MISMATCH
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
