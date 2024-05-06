/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in August 2024.
 *
 * ---------------------------------------------------------------------
 *
 * Smash remote host file transfer and management functions.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "cpu_control.h"
#include "smash_cmd.h"
#include "host_cmd.h"
#include "sm_msg.h"
#include "sm_file.h"

#define VALUE_UNASSIGNED 0xffffffff

#ifndef RC_SUCCESS
#define RC_SUCCESS 0
#define RC_FAILURE 1
#endif

#ifndef UNUSED
#define UNUSED(x) (void)(x)
#endif

extern struct ExecBase *DOSBase;
extern uint flag_debug;

static char
printable_ascii(uint8_t ch)
{
    if (ch >= ' ' && ch <= '~')
        return (ch);
    if (ch == '\t' || ch == '\r' || ch == '\n' || ch == '\0')
        return (' ');
    return ('.');
}

static void
dump_memory(void *buf, uint len, uint dump_base)
{
    uint pos;
    uint strpos;
    char str[20];
    uint32_t *src = buf;

    len = (len + 3) / 4;
    if (dump_base != VALUE_UNASSIGNED)
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
            if ((dump_base != VALUE_UNASSIGNED) && ((pos + 1) < len)) {
                dump_base += 16;
                printf("%05x:", dump_base);
            }
        }
    }
    if ((pos & 3) != 0) {
        str[strpos] = '\0';
        printf("%*s%s\n", (4 - (pos & 3)) * 5, "", str);
    }
}

static uint
recv_msg(void *buf, uint len, uint *rlen, uint timeout_ms)
{
    uint rc;
    rc = send_cmd(KS_CMD_MSG_RECEIVE, NULL, 0, buf, len, rlen);
    while (rc == KS_STATUS_NODATA) {
        cia_spin(CIA_USEC(100));
        rc = send_cmd(KS_CMD_MSG_RECEIVE, NULL, 0, buf, len, rlen);
        if (timeout_ms-- == 0)
            break;
    }
    if (rc == KS_CMD_MSG_SEND)
        rc = RC_SUCCESS;
    if (rc != RC_SUCCESS) {
        printf("Get message failed: %d (%s)\n", rc, smash_err(rc));
        if (flag_debug)
            dump_memory(buf, 0x40, VALUE_UNASSIGNED);
    }
    return (rc);
}


// relocate to host_cmd.c
static uint
host_tag_alloc(void)
{
    /* XXX: Fake "allocator" for now */
    static uint16_t tag = 0;
    return (tag++);
}

static void
host_tag_free(uint tag)
{
    /* XXX: Fake "allocator" for now */
    UNUSED(tag);
}

// relocate to host_cmd.c
uint host_send_msg(void *msg, uint len);

uint
host_send_msg(void *smsg, uint slen)
{
    uint32_t rbuf[16];
    uint rc = send_cmd(KS_CMD_MSG_SEND, smsg, slen, rbuf, sizeof (rbuf), NULL);
    if (rc != 0) {
        printf("Send message l=%u failed: %d (%s)\n",
               slen, rc, smash_err(rc));
        if (flag_debug)
            dump_memory(rbuf, sizeof (rbuf), VALUE_UNASSIGNED);
    }
    return (rc);
}

uint
host_recv_msg(uint tag, void **rdata, uint *rlen)
{
    static uint8_t buf[4200];
    km_msg_hdr_t *msg = (km_msg_hdr_t *)buf;
    uint rc;
    uint rxlen;
    uint count;

    for (count = 0; count < 50; count++) {
        rc = recv_msg(buf, sizeof (buf), &rxlen, 1000);
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
            return (KM_STATUS_OK);
        }
        if (rc == KM_STATUS_EOF)
            return (rc);
        // XXX: Need to save this message as it's not for the current caller
        printf("Discarded message op=%02x status=%02x tag=%04x\n",
               msg->km_op, msg->km_status, msg->km_tag);
        Delay(1);
    }
    printf("Message receive timeout\n");
    return (KM_STATUS_FAIL);
}

uint
host_msg(void *smsg, uint slen, void **rdata, uint *rlen)
{
    km_msg_hdr_t *smsg_h = (km_msg_hdr_t *) smsg;
    uint rc = host_send_msg(smsg, slen);
    if (rc != 0)
        return (rc);
    return (host_recv_msg(smsg_h->km_tag, rdata, rlen));
}

uint
sm_open(handle_t parent_handle, const char *name, uint mode, uint *type,
        uint create_perms, handle_t *handle)
{
    uint msglen;
    uint rc;
    uint rlen;
    uint namelen = strlen(name) + 1;
    hm_fopenhandle_t *msg;
    hm_fopenhandle_t *rdata;

    *handle = 0;

    if (namelen > 2000) {
        printf("Path \"%s\" too long\n", name);
        return (KM_STATUS_FAIL);
    }
    msglen = sizeof (*msg) + namelen;
    msg = malloc(msglen);
    if (msg == NULL) {
        printf("Failed to allocate %u bytes\n", msglen);
        return (KM_STATUS_FAIL);
    }

    msg->hm_hdr.km_op     = KM_OP_FOPEN;
    msg->hm_hdr.km_status = 0;
    msg->hm_hdr.km_tag    = host_tag_alloc();
    msg->hm_handle        = parent_handle;  // parent directory handle
    msg->hm_mode          = mode;           // open mode
    msg->hm_aperms        = create_perms;   // Amiga permissions

    strcpy((char *)(msg + 1), name);  // Name follows message header

    rc = host_msg(msg, msglen, (void **) &rdata, &rlen);
    if (rc == RC_SUCCESS) {
        rc = rdata->hm_hdr.km_status;
        *handle = rdata->hm_handle;
    }
    if (type != NULL)
        *type = rdata->hm_type;
    host_tag_free(rdata->hm_hdr.km_tag);
    free(msg);
    return (rc);
}

uint
sm_close(handle_t handle)
{
    uint rc;
    uint rlen;
    hm_fopenhandle_t *rdata;
    hm_fopenhandle_t msg;
    msg.hm_hdr.km_op     = KM_OP_FCLOSE;
    msg.hm_hdr.km_status = 0;
    msg.hm_hdr.km_tag    = host_tag_alloc();
    msg.hm_handle        = handle;

    if (host_msg(&msg, sizeof (msg), (void **) &rdata, &rlen) == RC_SUCCESS)
        rc = RC_SUCCESS;
    else
        rc = RC_FAILURE;
    host_tag_free(rdata->hm_hdr.km_tag);
    return (rc);
}

/*
 * sm_read
 * -------
 * Returns data contents from the USB host's file handle, which could
 * be from the contents of a file or directory entries.
 *
 * handle is the remote file handle: see sm_open().
 * readsize is the maximum size of data to acquire.
 * data is a pointer which is returned by this function.
 *      Note that data is from a static buffer not allocated by the caller.
 * rlen is the size of the received content (pointed to by data).
 */
uint
sm_read(handle_t handle, uint readsize, void **data, uint *rlen)
{
    uint rc;
    hm_freadwrite_t msg;
    hm_freadwrite_t *rdata;
    uint rcvlen;

    msg.hm_hdr.km_op     = KM_OP_FREAD;
    msg.hm_hdr.km_status = 0;
    msg.hm_hdr.km_tag    = host_tag_alloc();
    msg.hm_handle        = handle;
    msg.hm_length        = readsize;

    if (host_msg(&msg, sizeof (msg), (void **) &rdata, &rcvlen) == RC_SUCCESS) {
        rc = RC_SUCCESS;
    } else {
        rc = RC_FAILURE;
        goto sm_read_fail;
    }
    rc = rdata->hm_hdr.km_status;
//  printf("read km_status=%d rlen=%x\n", rc, rcvlen);
    if ((rc != KM_STATUS_OK) && (rc != KM_STATUS_EOF)) {
        rcvlen = 0;
        goto sm_read_fail;
    }

    if (rcvlen > readsize + sizeof (msg)) {
        printf("bad rcvlen %x\n", rcvlen);
        rcvlen = readsize + sizeof (msg);
    }

    if (rcvlen >= sizeof (*rdata))
        rcvlen -= sizeof (*rdata);
    else
        rcvlen = 0;
    *data = (void *) (rdata + 1);

sm_read_fail:
    if (rlen != NULL)
        *rlen = rcvlen;
    if ((rc != RC_SUCCESS) && flag_debug)
        dump_memory(rdata, rcvlen, VALUE_UNASSIGNED);

    // XXX: In the future, only free the tag once all data is received
    host_tag_free(msg.hm_hdr.km_tag);
    return (rc);
}

uint
sm_write(handle_t handle, void *buf, uint buflen)
{
    hm_freadwrite_t *msg = buf;
    hm_freadwrite_t *rdata;
    uint msglen = sizeof (*msg) + buflen;
    uint rcvlen;
    uint rc;

    msg->hm_hdr.km_op     = KM_OP_FWRITE;
    msg->hm_hdr.km_status = 0;
    msg->hm_hdr.km_tag    = host_tag_alloc();
    msg->hm_handle        = handle;
    msg->hm_length        = buflen;

    rc = host_msg(msg, msglen, (void **) &rdata, &rcvlen);
    if (rc == RC_SUCCESS)
        rc = rdata->hm_hdr.km_status;

    host_tag_free(msg->hm_hdr.km_tag);
    return (rc);
}

uint
sm_path(handle_t handle, char **name)
{
    uint rc;
    uint rlen;
    hm_fhandle_t *rdata;
    hm_fhandle_t msg;

    msg.hm_hdr.km_op     = KM_OP_FPATH;
    msg.hm_hdr.km_status = 0;
    msg.hm_hdr.km_tag    = host_tag_alloc();
    msg.hm_handle        = handle;

    rc = host_msg(&msg, sizeof (msg), (void **) &rdata, &rlen);
    if (rc == RC_SUCCESS) {
        *name = (char *)(rdata + 1);
        rc = rdata->hm_hdr.km_status;
    }

    host_tag_free(rdata->hm_hdr.km_tag);
    return (rc);
}

uint
sm_delete(handle_t handle, const char *name)
{
    uint rc;
    uint namelen = strlen(name) + 1;
    uint rlen;
    uint msglen;
    hm_fhandle_t *rdata;
    hm_fhandle_t *msg;

    if (namelen > 2000) {
        printf("Path \"%s\" too long\n", name);
        return (RC_FAILURE);
    }
    msglen = sizeof (*msg) + namelen;
    msg = malloc(msglen);
    if (msg == NULL) {
        printf("Failed to allocate %u bytes\n", msglen);
        return (RC_FAILURE);
    }

    msg->hm_hdr.km_op     = KM_OP_FDELETE;
    msg->hm_hdr.km_status = 0;
    msg->hm_hdr.km_tag    = host_tag_alloc();
    msg->hm_handle        = handle;
    strcpy((char *)(msg + 1), name);  // Name follows message header

    if (host_msg(msg, msglen, (void **) &rdata, &rlen) == RC_SUCCESS) {
        rc = RC_SUCCESS;
        if (rdata->hm_hdr.km_status != KM_STATUS_OK) {
            printf("Failed to delete %s (%x)\n", name, rdata->hm_hdr.km_status);
            rc = RC_FAILURE;
        }
    } else {
        rc = RC_FAILURE;
        printf("Failed to delete %s\n", name);
    }

    host_tag_free(rdata->hm_hdr.km_tag);
    free(msg);
    return (rc);
}

uint
sm_rename(handle_t handle, const char *name_old, const char *name_new)
{
    uint rc;
    uint len_from  = strlen(name_old) + 1;
    uint len_to    = strlen(name_new) + 1;
    uint len_total = len_from + len_to;
    uint rlen;
    uint msglen;
    hm_fhandle_t *rdata;
    hm_fhandle_t *msg;

    if (len_total > 2000) {
        printf("Path \"%s\" plus \"%s\" too long\n", name_old, name_new);
        return (RC_FAILURE);
    }
    msglen = sizeof (*msg) + len_total;
    msg = malloc(msglen);
    if (msg == NULL) {
        printf("Failed to allocate %u bytes\n", msglen);
        return (RC_FAILURE);
    }
    msg->hm_hdr.km_op     = KM_OP_FRENAME;
    msg->hm_hdr.km_status = 0;
    msg->hm_hdr.km_tag    = host_tag_alloc();
    msg->hm_handle        = handle;
    strcpy((char *)(msg + 1), name_old);  // From name follows message header
    strcpy((char *)(msg + 1) + len_from, name_new);  // To name follows that

    if ((rc = host_msg(msg, msglen, (void **) &rdata, &rlen)) == RC_SUCCESS) {
        rc = RC_SUCCESS;
        if (rdata->hm_hdr.km_status != KM_STATUS_OK) {
            printf("Failed to rename %s to %s (%x)\n",
                   name_old, name_new, rdata->hm_hdr.km_status);
            rc = RC_FAILURE;
        }
    } else {
        rc = RC_FAILURE;
        printf("Failed to rename %s to %s\n", name_old, name_new);
    }

    host_tag_free(rdata->hm_hdr.km_tag);
    free(msg);
    return (rc);
}

uint
sm_create(handle_t parent_handle, const char *name, const char *tgt_name,
          uint hm_type)
{
    uint rc;
    uint msglen;
    uint rlen;
    uint create_perms = 0;
    uint namelen = strlen(name) + 1;
    uint tgtlen = strlen(tgt_name) + 1;
    hm_fopenhandle_t *msg;
    hm_fhandle_t     *rdata;

    if (namelen + tgtlen > 2000) {
        printf("Path \"%s\" too long\n", name);
        return (RC_FAILURE);
    }
    msglen = sizeof (*msg) + namelen + tgtlen;
    msg = malloc(msglen);
    if (msg == NULL) {
        printf("Failed to allocate %u bytes\n", msglen);
        return (RC_FAILURE);
    }

    msg->hm_hdr.km_op     = KM_OP_FCREATE;
    msg->hm_hdr.km_status = 0;
    msg->hm_hdr.km_tag    = host_tag_alloc();
    msg->hm_handle        = parent_handle;
    msg->hm_mode          = 0;
    msg->hm_type          = hm_type;
    msg->hm_aperms        = create_perms;   // Amiga permissions

    strcpy((char *)(msg + 1), name);  // Name follows message header
    strcpy((char *)(msg + 1) + namelen, tgt_name);  // Symlink target follows

    rc = host_msg(msg, msglen, (void **) &rdata, &rlen);
    if (rc == RC_SUCCESS) {
        rc = rdata->hm_hdr.km_status;
    } else {
        printf("Failed to create %s\n", name);
    }

    host_tag_free(rdata->hm_hdr.km_tag);
    free(msg);
    return (rc);
}

uint
sm_set_perms(handle_t parent_handle, const char *name, uint perms)
{
    uint rc;
    uint msglen;
    uint rlen;
    uint namelen = strlen(name) + 1;
    hm_fopenhandle_t *msg;
    hm_fhandle_t     *rdata;

    if (namelen > 2000) {
        printf("Path \"%s\" too long\n", name);
        return (RC_FAILURE);
    }
    msglen = sizeof (*msg) + namelen;
    msg = malloc(msglen);
    if (msg == NULL) {
        printf("Failed to allocate %u bytes\n", msglen);
        return (RC_FAILURE);
    }

    msg->hm_hdr.km_op     = KM_OP_FSETPERMS;
    msg->hm_hdr.km_status = 0;
    msg->hm_hdr.km_tag    = host_tag_alloc();
    msg->hm_handle        = parent_handle;  // parent directory handle
    msg->hm_mode          = 0;              // unused
    msg->hm_aperms        = perms;          // Amiga permissions

    if (host_msg(msg, msglen, (void **) &rdata, &rlen) == RC_SUCCESS) {
        rc = rdata->hm_hdr.km_status;
    } else {
        rc = KM_STATUS_FAIL;
        printf("Failed to set perms 0x%x for %s\n", perms, name);
    }

    host_tag_free(rdata->hm_hdr.km_tag);
    free(msg);
    return (rc);
}
