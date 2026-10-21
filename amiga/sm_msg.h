/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2024.
 *
 * ---------------------------------------------------------------------
 *
 * Amiga-to-Kicksmash message interface functions.
 */

#ifndef _SM_MSG_H
#define _SM_MSG_H

/* Status codes from local message handling */
#define MSG_STATUS_SUCCESS    0           //   0 No error
#define MSG_STATUS_FAIL       0xfffffffa  //  -6 Generic failure
#define MSG_STATUS_NO_REPLY   0xfffffff9  //  -7 No reply from Kicksmash
#define MSG_STATUS_BAD_LENGTH 0xfffffff8  //  -8 Bad length detected
#define MSG_STATUS_BAD_CRC    0xfffffff7  //  -9 CRC failure detected
#define MSG_STATUS_BAD_DATA   0xfffffff6  // -10 Invalid data
#define MSG_STATUS_PRG_TMOUT  0xfffffff5  // -11 Programming timeout
#define MSG_STATUS_PRG_FAIL   0xfffffff4  // -12 Programming failure
#define MSG_STATUS_NO_MEM     0xfffffff3  // -13 No memory available
#define MSG_STATUS_MISMATCH   0xfffffff2  // -14 Data mismatch
#define MSG_STATUS_LAST_ENTRY 0xfffffff1  // Fake ent: must always be last - 1

#define DUMP_VALUE_UNASSIGNED 0xffffffff

typedef unsigned int uint;

void msg_init(void);

#ifdef ROMFS
extern const uint32_t lcrc32_table[];
#endif
extern void (*ecia_spin)(unsigned int ticks);
extern uint (*esend_cmd_core)(uint16_t cmd, void *arg, uint16_t arglen,
                              void *reply, uint replymax, uint *replyalen);
uint send_cmd_core(uint16_t cmd, void *arg, uint16_t arglen,
                   void *reply, uint replymax, uint *replyalen);
void send_cmd_core_begin(void);
void send_cmd_core_end(void);

uint send_cmd(uint16_t cmd, void *arg, uint16_t arglen,
              void *reply, uint replymax, uint *replyalen);
uint send_cmd_retry(uint16_t cmd, void *arg, uint16_t arglen,
                    void *reply, uint replymax, uint *replyalen);

uint host_msg(void *smsg, uint slen, void **rdata, uint *rlen);
uint host_msg_tag(void *smsg, uint slen, void **rdata, uint *rlen);
uint host_send_msg(void *smsg, uint slen);
uint host_recv_msg(uint tag, void **rdata, uint *rlen);
uint host_recv_msg_noblock(uint tag, void **rdata, uint *rlen);
uint host_recv_msg_cont(uint tag, void *buf, uint buf_len);

uint host_tag_alloc(void);
void host_tag_free(uint tag);
void host_msg_exit(void);
uint host_msg_tag_queued(uint tag);
void host_msg_set_nread_tags(uint cur, uint cur_valid,
                             uint prev, uint prev_valid);

void dump_memory(void *buf, uint len, uint dump_base);

#define MSG_BUF_SIZE 4200  // Maximum received message size (multiple of 4)

#ifndef STANDALONE
#include <exec/nodes.h>
#include <exec/lists.h>
#include <exec/semaphores.h>

/*
 * Shared "smashmsg" message semaphore structure (created by sm_msg.c).
 * Visible here so that debug tools (smash debugq) can display it.
 * Change MSG_SEM_VERSION whenever the msg_semaphore_t or msg_buf_t
 * layout changes.
 */
#define MSG_SEM_VERSION 3
typedef struct SignalSemaphore SignalSemaphore_t;
typedef struct MinNode         MinNode_t;
typedef struct MinList         MinList_t;

/*
 * Message buffer. Every buffer is on exactly one of the shared lists
 * (ms_free_list, ms_queue_list, or ms_held_list), except while a task is
 * actively receiving into it. The list node is part of the buffer, so a
 * buffer can never be referenced from two places at once.
 */
typedef struct {
    MinNode_t    mb_node;     // Free, reply queue, or held list linkage
    struct Task *mb_owner;    // Holding task (ms_held_list only)
    void        *mb_prog;     // Holding program (ms_held_list only)
    uint16_t     mb_len;      // Length of message (NOT buffer size)
    uint16_t     mb_tag;      // Message tag (ms_queue_list only)
    uint32_t     mb_data[MSG_BUF_SIZE / 4];  // Message (km_msg_hdr_t first)
} msg_buf_t;

typedef struct {
    SignalSemaphore_t ms_sem;         // Semaphore MUST be the first member
    uint16_t          ms_version;     // Struct version
    uint16_t          ms_tag;         // Next available tag
    uint16_t          ms_tag_count;   // Outstanding tags
    uint16_t          ms_refcount;    // Number of tasks using this struct
    uint16_t          ms_busy;        // Reader busy
    uint16_t          ms_allocsize;   // Size of struct
    /*
     * ms_queue_lock protects all three buffer lists, ms_free_count, and
     * the network receive tags below.
     */
    SignalSemaphore_t ms_queue_lock;  // Buffer list lock
    MinList_t         ms_queue_list;  // Received messages awaiting their tag
    MinList_t         ms_free_list;   // Spare message buffers
    MinList_t         ms_held_list;   // Buffers delivered to tasks (in use)
    uint16_t          ms_free_count;  // Number of buffers on ms_free_list
    /*
     * Network receive (KM_OP_NREAD) tags, used by every task's
     * tag_queue_remove() so that queued packets for these tags are not
     * expunged. Protected by ms_queue_lock; see host_msg_set_nread_tags().
     */
    uint16_t          ms_nread_tag_cur;        // Current network rx tag
    uint16_t          ms_nread_tag_prev;       // Previous network rx tag
    uint8_t           ms_nread_tag_cur_valid;  // ms_nread_tag_cur is in use
    uint8_t           ms_nread_tag_prev_valid; // ms_nread_tag_prev is in use
} msg_semaphore_t;
#endif /* !STANDALONE */

const char *smash_err(uint status);

extern uint     smash_cmd_shift;
extern uint8_t  host_interface_version;

#endif /* _SM_MSG_H */
