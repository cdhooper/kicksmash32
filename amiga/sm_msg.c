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
#ifndef STANDALONE
#include <exec/execbase.h>
#endif
#include <memory.h>
#include "crc32.h"
#include "sm_msg.h"
#include "smash_cmd.h"
#include "host_cmd.h"
#include "cpu_control.h"

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
    rc = send_cmd(KS_CMD_MSG_RECEIVE, NULL, 0, buf, len, rlen);
    timeout_ms /= 2;
    while (rc == KS_STATUS_NODATA) {
        cia_spin(CIA_USEC(600));
        rc = send_cmd(KS_CMD_MSG_RECEIVE, NULL, 0, buf, len, rlen);
        if (timeout_ms-- == 0)
            break;
    }
    if (rc == KS_CMD_MSG_SEND)
        rc = KM_STATUS_OK;
    if (rc != KM_STATUS_OK) {
        printf("Get message failed: (%s)\n", smash_err(rc));
#ifndef ROMFS
        if (flag_debug > 2)
            dump_memory(buf, 0x40, DUMP_VALUE_UNASSIGNED);
#endif
    }
    return (rc);
}


/*
 * host_tag_alloc
 * --------------
 * Allocate and return a new host message tag. See host_tag_free().
 */
uint
host_tag_alloc(void)
{
    /* XXX: Fake "allocator" for now */
    static uint16_t tag = 0;
    return (tag++);
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
    /* XXX: Fake "allocator" for now */
    (void) tag;
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
    uint pos;
    uint rc;

    if (sendlen > SEND_MSG_MAX)
        sendlen = SEND_MSG_MAX;

    rc = send_cmd(KS_CMD_MSG_SEND, smsg, sendlen, rbuf, sizeof (rbuf), NULL);
    if ((rc == 0) && (sendlen < len)) {
        uint timeout = 0;
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
            rc = send_cmd(KS_CMD_MSG_SEND, smsg + pos, sendlen, NULL, 0, NULL);
            memcpy(smsg + pos, savebuf, sizeof (km_msg_hdr_t));

            if (rc == KS_STATUS_BADLEN) {
                /* Not wasn't enough space in the KS buffer; try again. */
                if (timeout++ < 20) {
                    cia_spin(CIA_USEC(1000));
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
    static uint8_t buf[4200];
    km_msg_hdr_t *msg = (km_msg_hdr_t *)buf;
    uint rc;
    uint rxlen;
    uint count;

    for (count = 0; count < 50; count++) {
        rc = recv_msg(buf, sizeof (buf), &rxlen, 500);  // 500 ms timeout
        if ((rc != KM_STATUS_OK) && (rc != KM_STATUS_EOF))
            return (rc);
        if (tag == msg->km_tag) {
            /* Got desired message */
            if (rxlen > sizeof (buf)) {
                printf("BUG: Rx message op=%x stat=%x too large (%u > %u)\n",
                       msg->km_op, msg->km_status, rxlen, sizeof (buf));
                rxlen = sizeof (buf);
            }
            *rlen = rxlen;
            *rdata = buf;
            if (rc == KM_STATUS_OK)
                rc = msg->km_status;
            return (rc);
        }
        // XXX: Need to save this message as it's not for the current caller
        printf("Discarded message op=%02x status=%02x tag=%04x (want %04x)\n",
               msg->km_op, msg->km_status, msg->km_tag, tag);
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
    uint8_t *rdata;
    uint     rcvlen;
    uint     cur_len = 0;
    uint     rc;

    while (cur_len < buf_len) {
        rc = host_recv_msg(tag, (void **) &rdata, &rcvlen);
        if (rc == KM_STATUS_EOF)
            rc = KM_STATUS_OK;
        if (rc != KM_STATUS_OK) {
            printf("next pkt failed at %u of %u: %s\n",
                   cur_len, buf_len, smash_err(rc));
            return (rc);
        }
        if (rcvlen + cur_len > buf_len + sizeof (km_msg_hdr_t)) {
            printf("next pkt bad rcvlen %x\n", rcvlen);
            return (KM_STATUS_FAIL);
        }
        if (rcvlen >= sizeof (km_msg_hdr_t))
            rcvlen -= sizeof (km_msg_hdr_t);
        else
            rcvlen = 0;

        memcpy(buf + cur_len, rdata + sizeof (km_msg_hdr_t), rcvlen);
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
