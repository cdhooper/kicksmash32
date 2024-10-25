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
#include <exec/memory.h>
#include <inline/exec.h>
#include "smash_cmd.h"
#include "host_cmd.h"
#include "sm_msg.h"
#include "sm_file.h"

static uint     sm_mbuf_size = 0;
static uint8_t *sm_mbuf      = NULL;

/*
 * sm_fservice
 * -----------
 * Returns non-zero if the host is connected and providing file service.
 */
uint
sm_fservice(void)
{
    uint16_t states[2];
    uint rc;
    uint rxlen;
    rc = send_cmd(KS_CMD_MSG_STATE, 0, 0, states, sizeof (states), &rxlen);
    if ((rc == 0) &&
        ((states[1] & (MSG_STATE_SERVICE_UP | MSG_STATE_HAVE_FILE)) ==
                      (MSG_STATE_SERVICE_UP | MSG_STATE_HAVE_FILE))) {
        sm_file_active = 1;
        return (1);
    }
    sm_file_active = 0;
    return (0);
}

/*
 * sm_fopen
 * --------
 * Open the specified file, returning a file handle.
 *
 * parent_handle is the parent directory for file names which do not
 *     specify an absolute path to the file. If the file name begins
 *     with "::" then it is a fully specified absolute path; the
 *     parent handle will be ignored in that case. If the file name
 *     begins with ":" then the parent handle will be used only to
 *     reference the appropriate volume as a starting point. If the
 *     parent handle has a value of 0, the Volume Directory will be
 *     used as the file name starting point. If the parent handle has
 *     a value of -1 (0xffffffff), the default volume will be used as
 *     the file name starting point. If hostsmash is not started with
 *     a -M option, then the Volume Directory will be used as the
 *     default volume.
 * name specifies the file name path to open.
 * mode is a combination of HM_MODE_*
 *      HM_MODE_READ opens the file or directory for read access
 *      HM_MODE_WRITE opens the file for write access
 *      HM_MODE_RDWR opens the file for read and write access
 *      HM_MODE_APPEND opens the file for write access, appending to
 *              existing content.
 *      HM_MODE_CREATE opens a file for write, creating the file if it
 *              does not already exist. create_perms are then applied
 *              to the file's permissions. These permissions are
 *              specified as Amiga fib_Protection bits (FIBF_READ, etc).
 *      HM_MODE_TRUNC opens a file for write, truncating all content
 *              beyond the current seek position.
 *      HM_MODE_DIR opens a directory or file for read of file STAT
 *              information. See the hm_fdirent_t data structure for
 *              data format returned from reads.
 *      HM_MODE_READDIR is a short-hand for HM_MODE_READ and HM_MODE_DIR.
 * hm_type is a pointer to the file type which was successfully opened. It
 *      will be one of HM_TYPE_*.
 * create_perms are the permissions to apply to the created file (see
 *      HM_MODE_CREATE above). See sm_fsetprotect() for more information
 *      on file permissions.
 * handle is a pointer to the new file handle which will be returned if
 *     the open is successful.
 */
uint
sm_fopen(handle_t parent_handle, const char *name, uint mode, uint *hm_type,
         uint create_perms, handle_t *handle)
{
    uint msglen;
    uint rc;
    uint rlen;
    uint namelen = strlen(name) + 1;
    hm_fopenhandle_t *msg;
    hm_fopenhandle_t *rdata;

    *handle = 0;

    if ((sm_file_active == 0) && (sm_fservice() == 0))
        return (KM_STATUS_UNAVAIL);

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
    msg->hm_type          = 0;              // unused
    msg->hm_aperms        = create_perms;   // Amiga permissions

    strcpy((char *)(msg + 1), name);  // Name follows message header

    rc = host_msg(msg, msglen, (void **) &rdata, &rlen);
    if (rc == KM_STATUS_OK)
        *handle = rdata->hm_handle;

    if (hm_type != NULL)
        *hm_type = rdata->hm_type;
    host_tag_free(msg->hm_hdr.km_tag);
    free(msg);

    if (rc == KS_STATUS_NODATA)
        sm_fservice();  // Check if file service is still active

    return (rc);
}

/*
 * sm_fclose
 * ---------
 * Close a previously opened file handle.
 *
 * handle is the remote file handle: see sm_fopen().
 */
uint
sm_fclose(handle_t handle)
{
    uint rc;
    uint rlen;
    hm_fopenhandle_t *rdata;
    hm_fopenhandle_t msg;

    if ((sm_file_active == 0) && (sm_fservice() == 0))
        return (KM_STATUS_UNAVAIL);

    msg.hm_hdr.km_op     = KM_OP_FCLOSE;
    msg.hm_hdr.km_status = 0;
    msg.hm_hdr.km_tag    = host_tag_alloc();
    msg.hm_handle        = handle;

    rc = host_msg(&msg, sizeof (msg), (void **) &rdata, &rlen);
    host_tag_free(msg.hm_hdr.km_tag);

    if (sm_mbuf != NULL) {
        free(sm_mbuf);
        sm_mbuf = NULL;
    }

    if (rc == KS_STATUS_NODATA)
        sm_fservice();  // Check if file service is still active

    return (rc);
}

/*
 * sm_fread
 * --------
 * Returns data contents from the USB host's file handle, which could
 * be from the contents of a file or directory entries.
 *
 * handle is the remote file handle: see sm_fopen().
 * readsize is the maximum size of data to acquire.
 * data is a pointer which is returned by this function.
 *      Note that data is from a static buffer not allocated by the caller.
 * rlen is the size of the received content (pointed to by data).
 */
uint
sm_fread(handle_t handle, uint readsize, void **data, uint *rlen, uint flags)
{
    uint rc;
    hm_freadwrite_t msg;
    hm_freadwrite_t *rdata;
    uint rcvlen;

    if ((sm_file_active == 0) && (sm_fservice() == 0))
        return (KM_STATUS_UNAVAIL);

    msg.hm_hdr.km_op     = KM_OP_FREAD;
    msg.hm_hdr.km_status = 0;
    msg.hm_hdr.km_tag    = host_tag_alloc();
    msg.hm_handle        = handle;
    msg.hm_length        = readsize;
    msg.hm_flag          = flags;
    msg.hm_unused        = 0;

    rc = host_msg(&msg, sizeof (msg), (void **) &rdata, &rcvlen);

    if ((rc != KM_STATUS_OK) && (rc != KM_STATUS_EOF)) {
        rcvlen = 0;
        goto sm_read_fail;
    }

#if 0
    // Need to remove this so that single dirents can be read
    if (rcvlen > readsize + sizeof (msg)) {
        printf("bad rcvlen %x\n", rcvlen);
        rcvlen = readsize + sizeof (msg);
    }
#endif

    if (rcvlen >= sizeof (*rdata))
        rcvlen -= sizeof (*rdata);
    else
        rcvlen = 0;
    *data = (void *) (rdata + 1);

    if (rcvlen != rdata->hm_length) {
        /* More packets are inbound */
        uint total_len = rdata->hm_length;
        uint tag = msg.hm_hdr.km_tag;

        if ((sm_mbuf == NULL) || (total_len >= sm_mbuf_size))  {
            if (sm_mbuf != NULL)
                free(sm_mbuf);
            sm_mbuf      = malloc(total_len);
            sm_mbuf_size = total_len;
        }
        if (sm_mbuf == NULL) {
            printf("malloc(%u) failed\n", total_len);
            rc = MSG_STATUS_NO_MEM;
            goto sm_read_fail;
        }
        memcpy(sm_mbuf, (rdata + 1), rcvlen);
        rc = host_recv_msg_cont(tag, sm_mbuf + rcvlen, total_len - rcvlen);
        if (rc != KM_STATUS_OK)
            goto sm_read_fail;
        rcvlen = total_len;
        *data = (void *) sm_mbuf;
    }
sm_read_fail:
    if (rlen != NULL)
        *rlen = rcvlen;

    host_tag_free(msg.hm_hdr.km_tag);

    if (rc == KS_STATUS_NODATA)
        sm_fservice();  // Check if file service is still active
    return (rc);
}

/*
 * sm_fwrite
 * ---------
 * Sends data to be written to the USB host's specified file handle.
 *
 * handle is the remote file handle: see sm_fopen().
 * buf is the data to be written, following uninitialized space reserved
 *     for a hm_freadwrite_t message header (12 bytes). This may be a bit
 *     odd for a file API, but it helps reduce the number of data copies
 *     for a message to be sent.
 * buflen is the number of bytes to write, which does not include the
 *     space reserved for the message header.
 * padded_header is a flag which indicates 12 bytes of extra space has
 *     been allocated in the buffer for header data. This allows more
 *     efficient data send, but may not be convenient for the caller.
 */
uint
sm_fwrite(handle_t handle, void *buf, uint writelen, uint padded_header,
          uint flags)
{
    hm_freadwrite_t *msg;
    hm_freadwrite_t *rdata;
    uint msglen;
    uint rcvlen;
    uint rc;
    uint8_t chunk_header[sizeof (*msg) + 32];

    if ((sm_file_active == 0) && (sm_fservice() == 0))
        return (KM_STATUS_UNAVAIL);

    if (padded_header)
        msg = buf;
    else
        msg = (hm_freadwrite_t *) &chunk_header;

    msg->hm_hdr.km_op     = KM_OP_FWRITE;
    msg->hm_hdr.km_status = 0;
    msg->hm_hdr.km_tag    = host_tag_alloc();
    msg->hm_handle        = handle;
    msg->hm_length        = writelen;
    msg->hm_flag          = flags;
    msg->hm_unused        = 0;

    if (padded_header) {
        /* Send entire message in one shot */
        msglen = sizeof (*msg) + writelen;
        rc = host_msg(msg, msglen, (void **) &rdata, &rcvlen);
    } else {
        /* Send an initial chunk, then use the sent space to insert header */
        uint copylen = sizeof (chunk_header) - sizeof (*msg);
        if (copylen > writelen)
            copylen = writelen;

        msg->hm_length = copylen;
        memcpy(msg + 1, buf, copylen);

        msglen = copylen + sizeof (*msg);
        rc = host_msg(msg, msglen, (void **) &rdata, &rcvlen);
        if ((rc == 0) && (copylen < writelen)) {
            hm_freadwrite_t *msg2 = buf + copylen - sizeof (*msg);

            /* Save original data */
            memcpy(msg + 1, msg2, sizeof (*msg));  // Save original data
            memcpy(msg2, msg, sizeof (*msg));      // Install header

            /* Send the rest of the message */
            msg2->hm_length = writelen - copylen;
            msglen = sizeof (*msg) + writelen - copylen;
            rc = host_msg(msg2, msglen, (void **) &rdata, &rcvlen);

            /* Restore original data */
            memcpy(msg2, msg + 1, sizeof (*msg));
        }
    }
    host_tag_free(msg->hm_hdr.km_tag);

    if (rc == KS_STATUS_NODATA)
        sm_fservice();  // Check if file service is still active
    return (rc);
}

/*
 * sm_fpath
 * --------
 * Provides the path name to access the specified handle.
 *
 * handle is the remote file handle: see sm_fopen().
 * name is a pointer which will be assigned by this function to
 *     point to the full file path of the specified file.
 */
uint
sm_fpath(handle_t handle, char **name)
{
    uint rc;
    uint rlen;
    hm_fhandle_t *rdata;
    hm_fhandle_t msg;

    if ((sm_file_active == 0) && (sm_fservice() == 0))
        return (KM_STATUS_UNAVAIL);

    msg.hm_hdr.km_op     = KM_OP_FPATH;
    msg.hm_hdr.km_status = 0;
    msg.hm_hdr.km_tag    = host_tag_alloc();
    msg.hm_handle        = handle;

    rc = host_msg(&msg, sizeof (msg), (void **) &rdata, &rlen);
    if (rc == KM_STATUS_OK)
        *name = (char *)(rdata + 1);

    host_tag_free(msg.hm_hdr.km_tag);
    return (rc);
}

/*
 * sm_fdelete
 * ----------
 * Remove a file on the USB Host.
 *
 * handle is the remote parent directory handle: see sm_fopen().
 * name is the filename to delete. If the name refers to a directory,
 *     that directory must be empty or the deletion will fail.
 */
uint
sm_fdelete(handle_t handle, const char *name)
{
    uint rc;
    uint namelen = strlen(name) + 1;
    uint rlen;
    uint msglen;
    hm_fhandle_t *rdata;
    hm_fhandle_t *msg;

    if (namelen > 2000) {
        printf("Path \"%s\" too long\n", name);
        return (KM_STATUS_FAIL);
    }
    msglen = sizeof (*msg) + namelen;
    msg = malloc(msglen);
    if (msg == NULL) {
        printf("malloc(%u) fail\n", msglen);
        return (MSG_STATUS_NO_MEM);
    }

    msg->hm_hdr.km_op     = KM_OP_FDELETE;
    msg->hm_hdr.km_status = 0;
    msg->hm_hdr.km_tag    = host_tag_alloc();
    msg->hm_handle        = handle;
    strcpy((char *)(msg + 1), name);  // Name follows message header

    rc = host_msg(msg, msglen, (void **) &rdata, &rlen);
    if (rc != KM_STATUS_OK)
        printf("Failed to delete %s: %s\n", name, smash_err(rc));

    host_tag_free(msg->hm_hdr.km_tag);
    free(msg);
    return (rc);
}

/*
 * sm_frename
 * ----------
 * Rename or move a file on the USB Host. The old and new file name
 * path may be relative to the handle or specify an abolute path.
 * Absolute paths may cross volume boundaries, so long as the USB
 * Host permits the move. Unix hosts may reject moves across different
 * filesystems.
 *
 * handle is the remote parent directory handle for both the old name
 * and the new name: see sm_fopen().
 * name_old is the filename to be renamed.
 * name_new is the filename to be renamed.
 */
uint
sm_frename(handle_t shandle, const char *name_old,
           handle_t dhandle, const char *name_new)
{
    uint rc;
    uint len_from  = strlen(name_old) + 1;
    uint len_to    = strlen(name_new) + 1;
    uint len_total = len_from + len_to;
    uint rlen;
    uint msglen;
    hm_fhandle_t *rdata;
    hm_frename_t *msg;

    if (len_total > 2000) {
        printf("Path \"%s\" plus \"%s\" too long\n", name_old, name_new);
        return (MSG_STATUS_BAD_LENGTH);
    }
    msglen = sizeof (*msg) + len_total;
    msg = malloc(msglen);
    if (msg == NULL) {
        printf("malloc(%u) fail\n", msglen);
        return (MSG_STATUS_NO_MEM);
    }

    msg->hm_hdr.km_op     = KM_OP_FRENAME;
    msg->hm_hdr.km_status = 0;
    msg->hm_hdr.km_tag    = host_tag_alloc();
    msg->hm_shandle        = shandle;
    msg->hm_dhandle        = dhandle;
    strcpy((char *)(msg + 1), name_old);  // From name follows message header
    strcpy((char *)(msg + 1) + len_from, name_new);  // To name follows that

    rc = host_msg(msg, msglen, (void **) &rdata, &rlen);
    if (rc != KM_STATUS_OK) {
        printf("Failed to rename %s to %s: %s\n",
               name_old, name_new, smash_err(rc));
    }

    host_tag_free(msg->hm_hdr.km_tag);
    free(msg);
    return (rc);
}

/*
 * sm_fcreate
 * ----------
 * Create the specified directory, file, or special file.
 *
 * parent_handle is the parent directory for file names which do not
 *     specify an absolute path to the file. If the file name begins
 *     with "::" then it is a fully specified absolute path; the
 *     parent handle will be ignored in that case. If the file name
 *     begins with ":" then the parent handle will be used only to
 *     reference the appropriate volume as a starting point. If the
 *     parent handle has a value of 0, the Volume Directory will be
 *     used as the file name starting point. If the parent handle has
 *     a value of -1 (0xffffffff), the default volume will be used as
 *     the file name starting point. If hostsmash is not started with
 *     a -M option, then the Volume Directory will be used as the
 *     default volume.
 * name specifies the file name path to create.
 * tgt_name is only used in the case of creating a symbolic link. It
 *     refers to the name of an existing file to which the new link
 *     should point. There are some special limitations when creating
 *     a symbolic link. One is that name must refer to a path which
 *     is relative to tgt_name. The other is that name must not
 *     already exist. Symbolic link creation may be disabled during
 *     the USB host software compile.
 * hm_type is the file type to create. It will be one of HM_TYPE_*.
 *     HM_TYPE_FILE   is a regular file
 *     HM_TYPE_DIR    is a Directory.
 *     HM_TYPE_LINK   is a Symbolic (soft) Link. This type may be
 *                       specifically disabled on the host.
 *     HM_TYPE_HLINK  is a Hard Link. This type may be spoecifically
 *                       disabled on the host.
 *     HM_TYPE_BDEV   is a Block device.
 *     HM_TYPE_CDEV   is a Character device.
 *     HM_TYPE_FIFO   is a FIFO (also known as a pipe).
 *     HM_TYPE_SOCKET is a TCP socket.
 *     HM_TYPE_WHTOUT is a whiteout entry for overlay filesystems. This
 *                       type may not be implemented on the Host.
 *     HM_TYPE_VOLUME is not permitted as it refers to a drive volume.
 *     HM_TYPE_VOLDIR is not permitted as it refers to the volume directory.
 * create_perms are the Amiga protection bits to apply to the created
 *     file (FIBF_READ, etc). See sm_fsetprotect() for more information
 *     on file permissions.
 */
uint
sm_fcreate(handle_t parent_handle, const char *name, const char *tgt_name,
           uint hm_type, uint create_perms)
{
    uint rc;
    uint msglen;
    uint rlen;
    uint namelen = strlen(name) + 1;
    uint tgtlen = strlen(tgt_name) + 1;
    hm_fopenhandle_t *msg;
    hm_fhandle_t     *rdata;

    if (namelen + tgtlen > 2000) {
        printf("Path \"%s\" too long\n", name);
        return (MSG_STATUS_BAD_LENGTH);
    }
    msglen = sizeof (*msg) + namelen + tgtlen;
    msg = malloc(msglen);
    if (msg == NULL) {
        printf("malloc(%u) fail\n", msglen);
        return (MSG_STATUS_NO_MEM);
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
    if (rc != KM_STATUS_OK)
        printf("Failed to create %s: %s\n", name, smash_err(rc));

    host_tag_free(msg->hm_hdr.km_tag);
    free(msg);
    return (rc);
}

/*
 * sm_fseek
 * --------
 * Move to a specific position in a file or directory on the USB host.
 *
 * handle is the handle of the file / directory.
 * seek_mode is one of
 *      OFFSET_BEGINNING (-1) to seek from the start of file
 *      OFFSET_CURRENT (0) to seek from the current file position
 *      OFFSET_END (1) to seek from the end of file, signed offset in that case
 * offset is the offset from the start of the file / directory.
 * new_pos is an optional pointer to the value of the new file position.
 * prev_pos is an optional pointer to the value of the previous file position.
 */
uint
sm_fseek(handle_t handle, int seek_mode, uint64_t offset,
         uint64_t *new_pos, uint64_t *prev_pos)
{
    uint rc;
    uint rlen;
    hm_fseek_t  msg;
    hm_fseek_t *rmsg;

    if ((seek_mode < -1) || (seek_mode > 1)) {
        printf("\nODD seek_mode %d\n\n", seek_mode);
    }
    msg.hm_hdr.km_op     = KM_OP_FSEEK;
    msg.hm_hdr.km_status = 0;
    msg.hm_hdr.km_tag    = host_tag_alloc();
    msg.hm_handle        = handle;
    msg.hm_off_hi        = offset >> 32;
    msg.hm_off_lo        = offset;
    msg.hm_seek          = seek_mode;
    msg.hm_unused1       = 0;
    msg.hm_unused2       = 0;

    rc = host_msg(&msg, sizeof (msg), (void **) &rmsg, &rlen);
    if (rc != KM_STATUS_OK)
        printf("Failed to seek: %s\n", smash_err(rc));

    if (new_pos != NULL)
        *new_pos = ((uint64_t) (rmsg->hm_off_hi) << 32) | rmsg->hm_off_lo;
    if (prev_pos != NULL)
        *prev_pos = ((uint64_t) (rmsg->hm_old_hi) << 32) | rmsg->hm_old_lo;

    host_tag_free(msg.hm_hdr.km_tag);
    return (rc);
}

/*
 * sm_fsetown
 * ----------
 * Set owner and group of the specified file.
 *
 * parent_handle is the parent directory handle of the file. If the
 *     file name specifies a fully qualified path (beginning with a
 *     volume name), then a value of 0 or -1 may be specified here.
 *     See sm_fopen() for more information on the parent handle.
 * name specifies the file name to owner and group information should
 *     be changed. The file must already exist.
 * which is which operation to perform.
 *     0 sets the modify date/time
 *     1 gets the modify date/time
 *     2 sets the change date/time
 *     3 gets the change date/time
 *     4 sets the access date/time
 *     5 gets the access date/time
 * sec is seconds since 1970.
 * nsec is nanoseconds
 */
uint
sm_fsetdate(handle_t parent_handle, const char *name,
            uint which, uint *sec, uint *nsec)
{
    uint rc;
    uint msglen;
    uint rlen;
    uint namelen = strlen(name) + 1;
    hm_fsetdate_t *msg;
    hm_fhandle_t  *rdata;

    if (namelen > 2000) {
        printf("Path \"%s\" too long\n", name);
        return (MSG_STATUS_BAD_LENGTH);
    }
    msglen = sizeof (*msg) + namelen;
    msg = malloc(msglen);
    if (msg == NULL) {
        printf("malloc(%u) fail\n", msglen);
        return (MSG_STATUS_NO_MEM);
    }

    msg->hm_hdr.km_op     = KM_OP_FSETDATE;
    msg->hm_hdr.km_status = 0;
    msg->hm_hdr.km_tag    = host_tag_alloc();
    msg->hm_handle        = parent_handle;  // parent directory handle
    msg->hm_which         = which;
    msg->hm_unused0       = 0;
    msg->hm_unused1       = 0;
    msg->hm_time          = *sec;
    msg->hm_time_ns       = *nsec;

    strcpy((char *)(msg + 1), name);  // Name follows message header

    rc = host_msg(msg, msglen, (void **) &rdata, &rlen);
    if (rc != KM_STATUS_OK) {
        printf("Failed to set date %u.%u %s: %s\n",
               *sec, *nsec, name, smash_err(rc));
    }

    host_tag_free(msg->hm_hdr.km_tag);

    *sec  = msg->hm_time;
    *nsec = msg->hm_time_ns;
    free(msg);

    return (rc);
}

/*
 * sm_fsetown
 * ----------
 * Set owner and group of the specified file.
 *
 * parent_handle is the parent directory handle of the file. If the
 *     file name specifies a fully qualified path (beginning with a
 *     volume name), then a value of 0 or -1 may be specified here.
 *     See sm_fopen() for more information on the parent handle.
 * name specifies the file name to owner and group information should
 *     be changed. The file must already exist.
 * oid is the owner ID.
 * gid is the group ID.
 */
uint
sm_fsetown(handle_t parent_handle, const char *name, uint oid, uint gid)
{
    uint rc;
    uint msglen;
    uint rlen;
    uint namelen = strlen(name) + 1;
    hm_fsetown_t *msg;
    hm_fhandle_t *rdata;

    if (namelen > 2000) {
        printf("Path \"%s\" too long\n", name);
        return (MSG_STATUS_BAD_LENGTH);
    }
    msglen = sizeof (*msg) + namelen;
    msg = malloc(msglen);
    if (msg == NULL) {
        printf("malloc(%u) fail\n", msglen);
        return (MSG_STATUS_NO_MEM);
    }

    msg->hm_hdr.km_op     = KM_OP_FSETOWN;
    msg->hm_hdr.km_status = 0;
    msg->hm_hdr.km_tag    = host_tag_alloc();
    msg->hm_handle        = parent_handle;  // parent directory handle
    msg->hm_oid           = oid;            // file new owner id
    msg->hm_gid           = gid;            // file new group id

    strcpy((char *)(msg + 1), name);  // Name follows message header

    rc = host_msg(msg, msglen, (void **) &rdata, &rlen);
    if (rc != KM_STATUS_OK) {
        printf("Failed to set owner %u.%u for %s: %s\n",
               oid, gid, name, smash_err(rc));
    }

    host_tag_free(msg->hm_hdr.km_tag);
    free(msg);
    return (rc);
}

/*
 * sm_fsetprotect
 * --------------
 * Set access permissions and other attributes on the specified file.
 *
 * parent_handle is the parent directory handle of the file. If the
 *     file name specifies a fully qualified path (beginning with a
 *     volume name), then a value of 0 or -1 may be specified here.
 *     See sm_fopen() for more information on the parent handle.
 * name specifies the file name to which permissions should be applied.
 *     The file must already exist.
 * perms are the Amiga protection bits to apply to the specified file.
 *     FIBF_DELETE      file may be deleted by Owner. Note that this
 *                      permission may not be applicable to remote Unix
 *                      USB Hosts, as the permission to delete there is
 *                      specified in the parent directory permissions.
 *     FIBF_EXECUTE     file may be executed by Owner
 *     FIBF_WRITE       file may be written by Owner
 *     FIBF_READ        file may be read by Owner
 *     FIBF_ARCHIVE     file has been archived (not implemented by the
 *                      current USB Host software).
 *     FIBF_PURE        file is a module which should be kept resident.
 *     FIBF_SCRIPT      file is an Executable script
 *     FIBF_HOLD        file is Reentrant and re-executable
 *     FIBF_GRP_DELETE  file may be deleted by Group users (see FIBF_DELETE).
 *     FIBF_GRP_EXECUTE file may be executed by Group users.
 *     FIBF_GRP_WRITE   file may be written by Group users.
 *     FIBF_GRP_READ    file may be read by Group users.
 *     FIBF_OTR_DELETE  file may be deleted by Other users (see FIBF_DELETE).
 *     FIBF_OTR_EXECUTE file may be executed by Other users.
 *     FIBF_OTR_WRITE   file may be written by Other users.
 *     FIBF_OTR_READ    file may be read by Other users.
 */
uint
sm_fsetprotect(handle_t parent_handle, const char *name, uint perms)
{
    uint rc;
    uint msglen;
    uint rlen;
    uint namelen = strlen(name) + 1;
    hm_fopenhandle_t *msg;
    hm_fhandle_t     *rdata;

    if (namelen > 2000) {
        printf("Path \"%s\" too long\n", name);
        return (MSG_STATUS_BAD_LENGTH);
    }
    msglen = sizeof (*msg) + namelen;
    msg = malloc(msglen);
    if (msg == NULL) {
        printf("malloc(%u) fail\n", msglen);
        return (MSG_STATUS_NO_MEM);
    }

    msg->hm_hdr.km_op     = KM_OP_FSETPERMS;
    msg->hm_hdr.km_status = 0;
    msg->hm_hdr.km_tag    = host_tag_alloc();
    msg->hm_handle        = parent_handle;  // parent directory handle
    msg->hm_mode          = 0;              // unused
    msg->hm_type          = 0;              // unused
    msg->hm_aperms        = perms;          // Amiga permissions

    strcpy((char *)(msg + 1), name);  // Name follows message header

    rc = host_msg(msg, msglen, (void **) &rdata, &rlen);
    if (rc != KM_STATUS_OK) {
        printf("Failed to set perms 0x%x for %s: %s\n",
               perms, name, smash_err(rc));
    }

    host_tag_free(msg->hm_hdr.km_tag);
    free(msg);
    return (rc);
}
