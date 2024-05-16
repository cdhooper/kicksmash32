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
#define KM_OP_FSETPERMS       0x19  // File storage set file perms

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
#define HM_MODE_DIR         0x0200  // Read directory entry in parent (stat)
#define HM_MODE_READDIR     0x0201  // Read directory (composite)

typedef uint32_t handle_t;

typedef struct {
    km_msg_hdr_t hm_hdr;     // standard message header
    handle_t     hm_handle;  // handle or parent dir handle
} hm_fhandle_t;

typedef struct {
    km_msg_hdr_t hm_hdr;     // standard message header
    handle_t     hm_handle;  // parent dir handle on open, new handle on reply
    uint16_t     hm_type;    // file or directory type (from USB host)
    uint16_t     hm_mode;    // file mode for open
    uint32_t     hm_aperms;  // Amiga file permissions for create
    /* For open, the filename immediately follows this struct */
} hm_fopenhandle_t;

typedef struct {
    km_msg_hdr_t hm_hdr;     // standard message header
    handle_t     hm_handle;  // file handle for request
    uint32_t     hm_length;  // length of request or reply data size
} hm_freadwrite_t;

typedef struct {
    km_msg_hdr_t hm_hdr;     // standard message header
    handle_t     hm_handle;
    uint32_t     hm_offset_hi;
    uint32_t     hm_offset_lo;
} hm_fseek_t;

typedef struct {
    uint16_t     hmd_type;    // File or directory type
    uint16_t     hmd_elen;    // Entry length
    uint32_t     hmd_size_hi; // file size upper 32 bits
    uint32_t     hmd_size_lo; // file size lower 32 bits
    uint32_t     hmd_blks;    // disk blocks consumed
    uint32_t     hmd_atime;   // Access time   (secs since Jan 1, 1978)
    uint32_t     hmd_ctime;   // Creation time (secs since Jan 1, 1978)
    uint32_t     hmd_mtime;   // Modify time   (secs since Jan 1, 1978)
    uint32_t     hmd_aperms;  // Amiga-style file permissions
    uint32_t     hmd_ino;     // Unique file number per-filesystem
    uint32_t     hmd_ouid;    // Owner userid
    uint32_t     hmd_ogid;    // Owner groupid
    uint32_t     hmd_rsvd[2]; // Reserved space for future expansion
    /* Name and comment follows... */
} hm_fdirent_t;
/*
 * The filename immediately follows hm_fdirent_t in the reply
 * message, and the file comment immediately follows the filename
 * (each NIL-terminated). The next struct in the directory list
 * will be two-byte aligned following the comment.
 */


#if 0
/* Host file commands -- these probably won't be used */
#define HF_CMD_DIE            0x01  // Terminate host command handler
#define HF_CMD_LOCATE_OBJECT  0x02  //
#define HF_CMD_FINDINPUT      0x03  //
#define HF_CMD_FINDOUTPUT     0x04  //
#define HF_CMD_FINDUPDATE     0x05  //
#define HF_CMD_READ           0x06  //
#define HF_CMD_WRITE          0x07  //
#define HF_CMD_END            0x08  //
#define HF_CMD_SEEK           0x09  //

#define HF_CMD_EXAMINE_OBJECT 0x00  //
#define HF_CMD_EXAMINE_NEXT   0x00  //
#define HF_CMD_EXAMINE_FH     0x00  //

#define HF_CMD_CURRENT_VOLUME 0x00  //
#define HF_CMD_SAME_LOCK      0x00  //
#define HF_CMD_FREE_LOCK      0x00  //
#define HF_CMD_CREATE_OBJECT  0x00  //
#define HF_CMD_RENAME_OBJECT  0x00  //
#define HF_CMD_DELETE_OBJECT  0x00  //
#define HF_CMD_CREATE_DIR     0x00  //
#define HF_CMD_COPY_DIR       0x00  //
#define HF_CMD_SET_PROTECT    0x00  //
#define HF_CMD_DISK_INFO      0x00  // Disk info of current volume
#define HF_CMD_INFO           0x00  // DIsk info of volume holding lock ARG1
#define HF_CMD_FLUSH          0x00  // Write all cached data to disk
#define HF_CMD_PARENT         0x00  //
#define HF_CMD_INHIBIT        0x00  //
#define HF_CMD_DISK_CHANGE    0x00  //
#define HF_CMD_SET_DATE       0x00  //

#define HF_CMD_READ_LINK      0x00  //
#define HF_CMD_MAKE_LINK      0x00  //
#define HF_CMD_SET_FILE_SIZE  0x00  //
#define HF_CMD_SET_OWNER      0x00  //
#define HF_CMD_SET_DATES      0x00  //
#define HF_CMD_SET_TIMES      0x00  //
#define HF_CMD_GET_PERMS      0x00  //
#define HF_CMD_SET_PERMS      0x00  //
#define HF_CMD_IS_FILESYSTEM  0x00  //
#define HF_CMD_GET_DISK_FSSM  0x00  //
#define HF_CMD_FREE_DISK_FSSM 0x00  //
#define HF_CMD_FS_STATS       0x00  //
#define HF_CMD_WRITE_PROTECT  0x00  //
#define HF_CMD_DISK_CHANGE    0x00  // Re-scan drive
#define HF_CMD_RENAME_DISK    0x00  // Change volume name
#define HF_CMD_FORMAT         0x00  // Format disk
#define HF_CMD_FH_FROM_LOCK   0x00  //
#define HF_CMD_COPY_DIR_FH    0x00  //
#define HF_CMD_PARENT_FH      0x00  //
#define HF_CMD_EXAMINE_ALL    0x00  //
#define HF_CMD_ADD_NOTIFY     0x00  //
#define HF_CMD_REMOVE_NOTIFY  0x00  //

#endif

#endif /* _HOST_CMD_H */
