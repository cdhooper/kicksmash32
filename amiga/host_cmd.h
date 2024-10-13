/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2024.
 *
 * ---------------------------------------------------------------------
 *
 * USB Host command interface
 */

#ifndef _HOST_CMD_H
#define _HOST_CMD_H

/* Operations which apply to message payload header km_op */
#define KM_OP_NULL            0x00  // Do nothing (discard message)
#define KM_OP_NOP             0x01  // Do nothing but reply
#define KM_OP_ID              0x02  // Report app ID and configuration
#define KM_OP_LOOPBACK        0x06  // Message loopback
#define KM_OP_FOPEN           0x10  // File storage open
#define KM_OP_FCLOSE          0x11  // File storage close
#define KM_OP_FREAD           0x12  // File storage read
#define KM_OP_FWRITE          0x13  // File storage write
#define KM_OP_FSEEK           0x14  // File storage seek
#define KM_OP_FCREATE         0x15  // File storage create
#define KM_OP_FDELETE         0x16  // File storage delete
#define KM_OP_FRENAME         0x17  // File storage rename
#define KM_OP_FPATH           0x18  // File storage get path to handle
#define KM_OP_FSETPERMS       0x19  // File storage set permissions
#define KM_OP_FSETOWN         0x1a  // File storage set owner / group
#define KM_OP_FSETDATE        0x1b  // File storage set date

#define KM_OP_REPLY           0x80  // Reply message flag to remote request

#define KM_STATUS_OK          0x00  // Success
#define KM_STATUS_FAIL        0x01  // General failure
#define KM_STATUS_EOF         0x02  // End of file (or directory) reached
#define KM_STATUS_UNKCMD      0x03  // Unknown command
#define KM_STATUS_PERM        0x04  // Permission failure
#define KM_STATUS_INVALID     0x05  // Invalid mode for operation
#define KM_STATUS_NOTEMPTY    0x06  // Directory not empty
#define KM_STATUS_NOEXIST     0x07  // Object does not exist
#define KM_STATUS_EXIST       0x08  // Object already exists
#define KM_STATUS_LAST_ENTRY  0x09  // Fake status: must always be last + 1

#define HM_TYPE_ANY         0x0000  // Any type of file (for open)
#define HM_TYPE_UNKNOWN     0x0000  // Unknown (for reported type)
#define HM_TYPE_FILE        0x0001  // Regular file
#define HM_TYPE_DIR         0x0002  // Directory
#define HM_TYPE_LINK        0x0003  // Symbolic (soft) link
#define HM_TYPE_HLINK       0x0004  // Hard link
#define HM_TYPE_BDEV        0x0005  // Block device
#define HM_TYPE_CDEV        0x0006  // Character device
#define HM_TYPE_FIFO        0x0007  // FIFO
#define HM_TYPE_SOCKET      0x0008  // Socket
#define HM_TYPE_WHTOUT      0x0009  // Whiteout entry
#define HM_TYPE_VOLUME      0x000a  // Disk volume
#define HM_TYPE_VOLDIR      0x000b  // Volume directory
#define HM_TYPE_LAST_ENTRY  0x000c  // Fake type: must always be last + 1

#define HM_MODE_READ        0x0001  // Read
#define HM_MODE_WRITE       0x0002  // Write
#define HM_MODE_RDWR        0x0003  // Read/write
#define HM_MODE_APPEND      0x0004  // Append to file
#define HM_MODE_CREATE      0x0100  // Create file if it doesn't exist
#define HM_MODE_TRUNC       0x0200  // Truncate file at open
#define HM_MODE_DIR         0x0800  // Read directory entry in parent (stat)
#define HM_MODE_READDIR     0x0801  // Read directory (composite)
#define HM_MODE_NOFOLLOW    0x1000  // Do not follow symlink on READDIR
#define HM_MODE_LINK        0x2000  // Symlink
#define HM_MODE_READLINK    0x2001  // Read symlink (composite)

#define HM_FLAG_SEEK0       0x0001  // Seek the start of file before read

typedef uint32_t handle_t;

typedef struct {
    km_msg_hdr_t hm_hdr;     // Standard message header
    handle_t     hm_handle;  // Handle or parent dir handle
} hm_fhandle_t;

typedef struct {
    km_msg_hdr_t hm_hdr;     // Standard message header
    handle_t     hm_handle;  // Parent dir handle on open, new handle on reply
    uint16_t     hm_type;    // File or directory type (from USB host)
    uint16_t     hm_mode;    // File mode for open
    uint32_t     hm_aperms;  // Amiga file permissions for create
    /* For open, the filename immediately follows this struct */
} hm_fopenhandle_t;

typedef struct {
    km_msg_hdr_t hm_hdr;     // Standard message header
    handle_t     hm_handle;  // File handle for request
    uint32_t     hm_length;  // Length of request or reply data size
    uint16_t     hm_flag;    // Read / write operation flags
    uint16_t     hm_unused;  // Unused
} hm_freadwrite_t;

typedef struct {
    km_msg_hdr_t hm_hdr;     // Standard message header
    handle_t     hm_shandle; // Source parent dir handle
    handle_t     hm_dhandle; // Destination parent dir handle
    /* Source and destination filenames immediately follow this struct */
} hm_frename_t;

typedef struct {
    km_msg_hdr_t hm_hdr;     // Standard message header
    handle_t     hm_handle;  // File handle for request
    uint32_t     hm_off_hi;  // New file offset upper 32 bits (signed)
    uint32_t     hm_off_lo;  // New file offset lower 32 bits
    uint32_t     hm_old_hi;  // Reply: Prev file offset upper 32 bits (signed)
    uint32_t     hm_old_lo;  // Reply: Prev file offset lower 32 bits
    int8_t       hm_seek;    // -1=from beginning, 0=from current, 1=from end
    uint8_t      hm_unused1; // Unused
    uint16_t     hm_unused2; // Unused
} hm_fseek_t;

typedef struct {
    km_msg_hdr_t hm_hdr;     // Standard message header
    handle_t     hm_handle;  // Parent dir handle
    uint8_t      hm_which;   // Which timestamp(s) to update
    uint8_t      hm_unused0; // Unused
    uint16_t     hm_unused1; // Unused
    uint32_t     hm_time;    // Time secs since Jan 1, 1970
    uint32_t     hm_time_ns; // Time nanoseconds
    /* The filename immediately follows this struct */
} hm_fsetdate_t;

typedef struct {
    km_msg_hdr_t hm_hdr;     // Standard message header
    handle_t     hm_handle;  // Parent dir handle
    uint32_t     hm_oid;     // New owner ID
    uint32_t     hm_gid;     // New group ID
    /* The filename immediately follows this struct */
} hm_fsetown_t;

typedef struct {
    uint16_t     hmd_type;    // File or directory type
    uint16_t     hmd_elen;    // Entry length
    uint32_t     hmd_size_hi; // file size upper 32 bits
    uint32_t     hmd_size_lo; // file size lower 32 bits
    uint32_t     hmd_blksize; // disk block size
    uint32_t     hmd_blks;    // disk blocks consumed lower 32 bits
    uint32_t     hmd_atime;   // Access time   (secs since Jan 1, 1970)
    uint32_t     hmd_ctime;   // Creation time (secs since Jan 1, 1970)
    uint32_t     hmd_mtime;   // Modify time   (secs since Jan 1, 1970)
    uint32_t     hmd_aperms;  // Amiga-style file permissions
    uint32_t     hmd_ino;     // Unique file number per-filesystem
    uint32_t     hmd_ouid;    // Owner userid
    uint32_t     hmd_ogid;    // Owner groupid
    uint32_t     hmd_mode;    // Unix disk mode (permissions)
    uint32_t     hmd_nlink;   // Filesystem links to file
    uint32_t     hmd_rdev;    // Filesystem block and char devices
    uint32_t     hmd_rsvd[2]; // Reserved space for future expansion
    /* Name and comment follows... */
} hm_fdirent_t;
/*
 * The filename immediately follows hm_fdirent_t in the reply
 * message, and the file comment immediately follows the filename
 * (each NIL-terminated). The next struct in the directory list
 * will be two-byte aligned following the comment.
 */

#endif /* _HOST_CMD_H */
