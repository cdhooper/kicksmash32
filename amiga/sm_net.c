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
    msg->hm_hdr.km_tag    = host_tag_alloc();

    rc = host_msg(msg, msglen, (void **) &rdata, &rlen);
    if (rc == KM_STATUS_OK) {
        /* Open succeeded */
    }
    host_tag_free(msg->hm_hdr.km_tag);
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
    msg->hm_hdr.km_tag    = host_tag_alloc();

    rc = host_msg(msg, msglen, (void **) &rdata, &rlen);
    if (rc == KM_STATUS_OK) {
        /* Close succeeded */
    }
    host_tag_free(msg->hm_hdr.km_tag);
    free(msg);

    if (rc == KS_STATUS_NODATA)
        sm_nservice();  // Check if file service is still active

    return (rc);
}

uint
sm_nwrite(ethhdr_t *ehdr, void *buf, uint writelen, uint padded_header)
{
    uint msglen;
    uint rlen;
    uint rc;
    uint ehdr_len = (ehdr != NULL) ? sizeof (*ehdr) : 0;
    uint8_t *dptr;
    hm_nreadwrite_t *msg;
    hm_nreadwrite_t *rdata;

    if ((sm_net_active == 0) && (sm_nservice() == 0))
        return (KM_STATUS_UNAVAIL);

    if (padded_header)
        msg = buf;
    else
        msg = (hm_nreadwrite_t *) sm_net_packet_send_buf;

    msg->hm_hdr.km_op     = KM_OP_NWRITE;
    msg->hm_hdr.km_status = 0;
    msg->hm_hdr.km_tag    = host_tag_alloc();
    msg->hm_length        = writelen + ehdr_len;
    dptr = (uint8_t *) (msg + 1);  // Start of ethetnet packet
    if (ehdr != NULL)
        memcpy(dptr, ehdr, sizeof (*ehdr));

    if (padded_header) {
        /* Send entire message in one shot */
        msglen = sizeof (*msg) + ehdr_len + writelen;
        rc = host_msg(msg, msglen, (void **) &rdata, &rlen);
    } else {
        memcpy(dptr + ehdr_len, buf, writelen);
        msglen = sizeof (*msg) + ehdr_len + writelen;
        rc = host_msg(msg, msglen, (void **) &rdata, &rlen);
    }
    host_tag_free(msg->hm_hdr.km_tag);

    if (rc == KS_STATUS_NODATA)
        sm_nservice();  // Check if file service is still active
    return (rc);
}

uint
sm_nread(void **data, uint *readlen)
{
    uint rc;
    uint rlen;
    hm_nreadwrite_t msg;
    hm_nreadwrite_t *rdata;

    if ((sm_net_active == 0) && (sm_nservice() == 0))
        return (KM_STATUS_UNAVAIL);

    msg.hm_hdr.km_op     = KM_OP_NREAD;
    msg.hm_hdr.km_status = 0;
    msg.hm_hdr.km_tag    = host_tag_alloc();
    msg.hm_length        = 0;

    rc = host_msg(&msg, sizeof (msg), (void **) &rdata, &rlen);
    if (rc != KM_STATUS_OK) {
        rlen = 0;
        goto sm_recv_fail;
    } else if (rlen > sizeof (msg)) {
        rlen -= sizeof (msg);
    } else {
        rlen = 0;
    }

    *data = (void *) (rdata + 1);

sm_recv_fail:
    if (readlen != NULL)
        *readlen = rlen;

    host_tag_free(msg.hm_hdr.km_tag);

    if (rc == KS_STATUS_NODATA)
        sm_nservice();  // Check if file service is still active

    return (rc);
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
    msg.hm_hdr.km_tag    = host_tag_alloc();

    rc = host_msg(&msg, sizeof (msg), (void **) &rdata, &rlen);
    if (rc == KM_STATUS_OK) {
        memcpy(mac, rdata->hm_mac, sizeof (rdata->hm_mac));
    }

    host_tag_free(msg.hm_hdr.km_tag);

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
    msg.hm_hdr.km_tag    = host_tag_alloc();
    memcpy(msg.hm_mac, mac, sizeof (msg.hm_mac));

    rc = host_msg(&msg, sizeof (msg), (void **) &rdata, &rlen);
    host_tag_free(msg.hm_hdr.km_tag);

    if (rc == KS_STATUS_NODATA)
        sm_nservice();  // Check if file service is still active

    return (rc);
}
