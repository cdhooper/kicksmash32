/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in August 2026.
 *
 * ---------------------------------------------------------------------
 *
 * Smash remote host network transfer and management functions.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <exec/memory.h>
#include <inline/exec.h>
#include <clib/dos_protos.h>
#include <inline/dos.h>
#include "smash_cmd.h"
#include "host_cmd.h"
#include "sm_msg.h"
#include "sm_net.h"

#define ETHERNET_FRAME_MAX 1522
#define NET_SEND_MAX (ETHERNET_FRAME_MAX + sizeof (hm_nreadwrite_t))

uint8_t         sm_net_active = 0;
static uint8_t  sm_net_packet_send_buf[NET_SEND_MAX];

/*
 * Received packets are sent by hostsmash asynchronously, using the tag of
 * the most recent KM_OP_NREAD request (see sm_nreadtag()). Up to two tags
 * are outstanding: the current one, and the previous one until packets
 * are seen with the current tag.
 */
uint16_t        sm_nread_tag_cur;
uint16_t        sm_nread_tag_prev;
uint8_t         sm_nread_tag_cur_valid  = 0;
uint8_t         sm_nread_tag_prev_valid = 0;

/*
 * sm_nread_tags_publish() copies the tag state to the shared message
 * structure, where every task's tag_queue_remove() uses it to avoid
 * expunging queued packets for these tags.
 */
static void
sm_nread_tags_publish(void)
{
    host_msg_set_nread_tags(sm_nread_tag_cur, sm_nread_tag_cur_valid,
                            sm_nread_tag_prev, sm_nread_tag_prev_valid);
}

/*
 * sm_nread_retire_prev() retires the previous tag, which hostsmash no
 * longer uses.
 */
static void
sm_nread_retire_prev(void)
{
    if (sm_nread_tag_prev_valid) {
        sm_nread_tag_prev_valid = 0;
        sm_nread_tags_publish();
        host_tag_free(sm_nread_tag_prev);
    }
}

/*
 * sm_nservice
 * -----------
 * Returns non-zero if the host is connected and providing network service.
 */
uint
sm_nservice(void)
{
    uint16_t states[2];
    uint rc;
    uint rxlen;
    rc = send_cmd_retry(KS_CMD_MSG_STATE, 0, 0, states, sizeof (states),
                        &rxlen);
    if ((rc == 0) &&
        ((states[1] & (MSG_STATE_SERVICE_UP | MSG_STATE_HAVE_NET)) ==
                      (MSG_STATE_SERVICE_UP | MSG_STATE_HAVE_NET))) {
        sm_net_active = 1;
        return (1);
    }
    sm_net_active = 0;
    return (0);
}

uint
sm_nopen(void)
{
    uint rc = 0;
    uint msglen;
    uint rlen;
    hm_nopenhandle_t *msg;
    hm_nopenhandle_t *rdata;

    if ((sm_net_active == 0) && (sm_nservice() == 0))
        return (KM_STATUS_UNAVAIL);

    msglen = sizeof (*msg);
    msg = malloc(msglen);
    msg->hm_hdr.km_op     = KM_OP_NOPEN;
    msg->hm_hdr.km_status = 0;

    rc = host_msg(msg, msglen, (void **) &rdata, &rlen);
    if (rc == KM_STATUS_OK) {
        /* Open succeeded */
    }
    free(msg);

    if (rc == KS_STATUS_NODATA)
        sm_nservice();  // Check if file service is still active

    return (rc);
}

uint
sm_nclose(void)
{
    uint rc = 0;
    uint msglen;
    uint rlen;
    hm_nopenhandle_t *msg;
    hm_fopenhandle_t *rdata;

    if ((sm_net_active == 0) && (sm_nservice() == 0))
        return (KM_STATUS_UNAVAIL);

    msglen = sizeof (*msg);
    msg = malloc(msglen);
    msg->hm_hdr.km_op     = KM_OP_NCLOSE;
    msg->hm_hdr.km_status = 0;

    rc = host_msg(msg, msglen, (void **) &rdata, &rlen);
    if (rc == KM_STATUS_OK) {
        /* Close succeeded */
    }
    free(msg);

    if (rc == KS_STATUS_NODATA)
        sm_nservice();  // Check if file service is still active

    return (rc);
}

static uint
host_send_msg_autotag(void *smsg, uint slen)
{
    uint rc;
    uint16_t tag = host_tag_alloc();
    ((km_msg_hdr_t *) smsg)->km_tag = tag;
    rc = host_send_msg(smsg, slen);
    host_tag_free(tag);
    return (rc);
}

uint
sm_nwrite(ethhdr_t *ehdr, void *buf, uint writelen, uint padded_header)
{
    uint msglen;
    uint rc;
    uint ehdr_len = (ehdr != NULL) ? sizeof (*ehdr) : 0;
    uint8_t *dptr;
    hm_nreadwrite_t *msg;

    if ((sm_net_active == 0) && (sm_nservice() == 0))
        return (KM_STATUS_UNAVAIL);

    if (padded_header)
        msg = buf;
    else
        msg = (hm_nreadwrite_t *) sm_net_packet_send_buf;

    msg->hm_hdr.km_op     = KM_OP_NWRITE;
    msg->hm_hdr.km_status = 0;
    msg->hm_length        = writelen + ehdr_len;
    dptr = (uint8_t *) (msg + 1);  // Start of ethetnet packet
    if (ehdr != NULL)
        memcpy(dptr, ehdr, sizeof (*ehdr));

    if (padded_header) {
        /* Send entire message in one shot */
        msglen = sizeof (*msg) + ehdr_len + writelen;
        rc = host_send_msg_autotag(msg, msglen);
    } else {
        memcpy(dptr + ehdr_len, buf, writelen);
        msglen = sizeof (*msg) + ehdr_len + writelen;
        rc = host_send_msg_autotag(msg, msglen);
    }

    if (rc == KS_STATUS_NODATA)
        sm_nservice();  // Check if file service is still active
    return (rc);
}

#if 0
void queue_debug_simple(const char *msg);
#endif

/*
 * sm_nreadtag
 * -----------
 * Allocate a new tag and send it to hostsmash with KM_OP_NREAD. Hostsmash
 * sends all received packets with the tag of the most recent KM_OP_NREAD,
 * and expects a new one every 10 seconds.
 *
 * At most two tags are kept outstanding. If two are already outstanding,
 * the oldest is retired before the new tag is allocated. If sending fails,
 * the new tag is freed and the existing tags are kept, so a valid tag
 * remains outstanding.
 */
uint
sm_nreadtag(void)
{
    hm_nreadwrite_t msg;
    uint            tag;

    if ((sm_net_active == 0) && (sm_nservice() == 0))
        return (KM_STATUS_UNAVAIL);

    if (sm_nread_tag_prev_valid && sm_nread_tag_cur_valid) {
        /* Two tags already outstanding: retire the oldest now */
        sm_nread_retire_prev();
    }

    tag = host_tag_alloc();

    /*
     * Publish the new tag before sending it, so packets which another
     * task receives for it are never expunged.
     */
    if (sm_nread_tag_cur_valid) {
        sm_nread_tag_prev       = sm_nread_tag_cur;
        sm_nread_tag_prev_valid = 1;
    }
    sm_nread_tag_cur       = tag;
    sm_nread_tag_cur_valid = 1;
    sm_nread_tags_publish();

    msg.hm_hdr.km_op     = KM_OP_NREAD;
    msg.hm_hdr.km_status = 0;
    msg.hm_hdr.km_tag    = tag;
    msg.hm_length        = 0;
#if 0
    static char buf[32];
    sprintf(buf, "[new rx tag %x]", tag);
    queue_debug_simple(buf);
#endif

    /* This message (KM_OP_NREAD) does not expect a reply */
    return (host_send_msg(&msg, sizeof (msg)));
}

/*
 * sm_nreadtag_free
 * ----------------
 * Retire all outstanding KM_OP_NREAD tags. Call this before
 * host_msg_exit() when the network service is shut down.
 */
void
sm_nreadtag_free(void)
{
    uint8_t cur_valid  = sm_nread_tag_cur_valid;
    uint8_t prev_valid = sm_nread_tag_prev_valid;

    sm_nread_tag_cur_valid  = 0;
    sm_nread_tag_prev_valid = 0;
    sm_nread_tags_publish();
    if (prev_valid)
        host_tag_free(sm_nread_tag_prev);
    if (cur_valid)
        host_tag_free(sm_nread_tag_cur);
}

/*
 * sm_nread
 * --------
 * Acquire the oldest received packet sent by hostsmash on a KM_OP_NREAD
 * tag, without waiting. Returns KS_STATUS_NODATA if no packet is
 * available.
 */
uint
sm_nread(void **data, uint *readlen)
{
    uint rc;
    uint rlen = 0;
    hm_nreadwrite_t *rdata;

    if (readlen != NULL)
        *readlen = 0;

    if ((sm_net_active == 0) && (sm_nservice() == 0))
        return (KM_STATUS_UNAVAIL);

    /* host_recv_msg_noblock() needs an outstanding tag on which to receive */
    if (!sm_nread_tag_cur_valid) {
        rc = sm_nreadtag();
        if (rc != 0)
            return (rc);
    }

    /* Packets on the current tag mean hostsmash is done with the old one */
    if (sm_nread_tag_prev_valid && host_msg_tag_queued(sm_nread_tag_cur))
        sm_nread_retire_prev();

    if (sm_nread_tag_prev_valid) {
        /* Until hostsmash switches tags, packets arrive on the old one */
        rc = host_recv_msg_noblock(sm_nread_tag_prev, (void **) &rdata, &rlen);
#if 0
        if (rc != KS_STATUS_NODATA) {
            static char buf[64];
            sprintf(buf, "req ptag %04x got rc=%x cmd %02x tag %04x",
                    sm_nread_tag_prev, rc, rdata->hm_hdr.km_op,
                    rdata->hm_hdr.km_tag);
            queue_debug_simple(buf);
        }
#endif
        if ((rc != KM_STATUS_OK) && host_msg_tag_queued(sm_nread_tag_cur)) {
            /*
             * Hostsmash has switched tags. Packets received meanwhile
             * were queued under the new tag; take one of those.
             */
            sm_nread_retire_prev();
            rc = host_recv_msg_noblock(sm_nread_tag_cur, (void **) &rdata,
                                       &rlen);
#if 0
        if (rc != KS_STATUS_NODATA) {
            static char buf[64];
            sprintf(buf, "req ctag %04x got rc=%x cmd %02x tag %04x",
                    sm_nread_tag_cur, rc, rdata->hm_hdr.km_op,
                    rdata->hm_hdr.km_tag);
            queue_debug_simple(buf);
        }
#endif
        }
    } else {
        rc = host_recv_msg_noblock(sm_nread_tag_cur, (void **) &rdata, &rlen);
    }

    if ((rc == KS_STATUS_NODATA) || (rc == KS_STATUS_CRC))
        return (KS_STATUS_NODATA);  // Nothing waiting
    if (rc != KM_STATUS_OK)
        return (rc);

    if (rlen > sizeof (*rdata))
        rlen -= sizeof (*rdata);
    else
        rlen = 0;

    *data = (void *) (rdata + 1);
    if (readlen != NULL)
        *readlen = rlen;
    return (KM_STATUS_OK);
}

uint
sm_ngetmac(uint8_t *mac)
{
    uint rc = 0;
    uint rlen;
    hm_nmac_t msg;
    hm_nmac_t *rdata;

    if ((sm_net_active == 0) && (sm_nservice() == 0))
        return (KM_STATUS_UNAVAIL);

    msg.hm_hdr.km_op     = KM_OP_NGETMAC;
    msg.hm_hdr.km_status = 0;

    rc = host_msg(&msg, sizeof (msg), (void **) &rdata, &rlen);
    if (rc == KM_STATUS_OK) {
        memcpy(mac, rdata->hm_mac, sizeof (rdata->hm_mac));
    }

    if (rc == KS_STATUS_NODATA)
        sm_nservice();  // Check if file service is still active

    return (rc);
}

uint
sm_nsetmac(uint8_t *mac)
{
    uint rc = 0;
    uint rlen;
    hm_nmac_t msg;
    hm_nmac_t *rdata;

    if ((sm_net_active == 0) && (sm_nservice() == 0))
        return (KM_STATUS_UNAVAIL);

    msg.hm_hdr.km_op     = KM_OP_NSETMAC;
    msg.hm_hdr.km_status = 0;
    memcpy(msg.hm_mac, mac, sizeof (msg.hm_mac));

    rc = host_msg(&msg, sizeof (msg), (void **) &rdata, &rlen);

    if (rc == KS_STATUS_NODATA)
        sm_nservice();  // Check if file service is still active

    return (rc);
}
