#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <clib/dos_protos.h>
#include <clib/exec_protos.h>
#include <clib/alib_protos.h>
#include <fcntl.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <exec/ports.h>
#include <exec/nodes.h>
#include <exec/libraries.h>

#include <devices/timer.h>

#include <libraries/dos.h>
#include <libraries/dosextens.h>
#include <libraries/filehandler.h>
#include "printf.h"
#include "smash_cmd.h"
#include "cpu_control.h"
#include "host_cmd.h"
#include "sm_msg.h"
#include "sm_file.h"
#include "fs_hand.h"
#include "fs_vol.h"
#include "fs_packet.h"

#define GARG1 (gpack->dp_Arg1)
#define GARG2 (gpack->dp_Arg2)
#define GARG3 (gpack->dp_Arg3)
#define GARG4 (gpack->dp_Arg4)

/* Ralph Babel packets */
#define ACTION_GET_DISK_FSSM    4201
#define ACTION_FREE_DISK_FSSM   4202

/* Given to me by Michael B. Smith, AS225 packets */
#define ACTION_EX_OBJECT        50
#define ACTION_EX_NEXT          51

/* BFFS extended fib_DirEntryType values */
#define ST_BDEVICE      -10     /* block special device */
#define ST_CDEVICE      -11     /* char special device */
#define ST_SOCKET       -12     /* UNIX socket */
#define ST_FIFO         -13     /* named pipe (queue) */
#define ST_LIFO         -14     /* named pipe (stack) */
#define ST_WHITEOUT     -15     /* whiteout entry */

typedef struct FileInfoBlock FileInfoBlock_t;
typedef struct FileHandle FileHandle_t;

#define FL_FLAG_NEEDS_REWIND 0x01 /* EXAMINE_NEXT should rewind dir handle */
#define FL_FLAG_IS_DIR       0x02 /* Lock handle is a scannable directory */

#define MAX_NAMELEN          255

/*
 * Request size which holds exactly one directory entry of any name length:
 * name + NIL, comment NIL, and pad to even length. The host never packs a
 * second entry into a request this small, and it truncates a symlink's
 * comment (link target) to fit, so the reply never exceeds the request.
 */
#define DIRENT_READ_LEN      (sizeof (hm_fdirent_t) + MAX_NAMELEN + 3)

/* DOS FileLock with SmashFS extensions */
typedef struct fs_lock {
    BPTR            fl_Link;      /* next Dos Lock */
    LONG            fl_Key;       /* Kicksmash handle */
    LONG            fl_Access;    /* 0=shared */
    struct MsgPort *fl_Task;      /* this handler's DosPort */
    BPTR            fl_Volume;    /* volume node of this handler */
    /* Below are SmashFS-specific */
    handle_t        fl_PHandle;   /* Parent handle */
    uint            fl_Flags;     /* Flags for this lock */
} fs_lock_t;

typedef struct fh_private fh_private_t;
struct fh_private {
    fs_lock_t    *fp_lock;        /* Parent lock */
    FileHandle_t *fp_fh;          /* AmigaOS FileHandle */
    handle_t      fp_handle;      /* KS file handle */
    uint64_t      fp_pos_cur;     /* Current file position */
    uint64_t      fp_pos_max;     /* Maximum file position */
};

typedef struct {
    ULONG   fa_type;
    ULONG   fa_mode;
    ULONG   fa_nlink;
    ULONG   fa_uid;
    ULONG   fa_gid;
    ULONG   fa_size;
    ULONG   fa_blocksize;
    ULONG   fa_rdev;
    ULONG   fa_blocks;
    ULONG   fa_fsid;
    ULONG   fa_fileid;
    ULONG   fa_atime;
    ULONG   fa_atime_us;
    ULONG   fa_mtime;
    ULONG   fa_mtime_us;
    ULONG   fa_ctime;
    ULONG   fa_ctime_us;
} fileattr_t;

/*
 * Most of these "NFS" file attribute types types come from RFC1094.
 * NFFIFO and higher come from NetBSD's nfsproto.h header.
 */
typedef enum {
    NFNON = 0,
    NFREG = 1,
    NFDIR = 2,
    NFBLK = 3,
    NFCHR = 4,
    NFLNK = 5,
    NFSOCK = 6,
    NFFIFO = 7,
    NFATTRDIR = 8,
    NFNAMEDATTR = 9,
} fileattr_type_t;

struct DosPacket *gpack;  // current packet being processed

static uint
km_status_to_amiga_error(uint status)
{
    switch (status) {
        case KM_STATUS_OK:
            return (0);
        case KM_STATUS_FAIL:
            /* Host general failure: mostly a path which can't be resolved */
            return (ERROR_OBJECT_NOT_FOUND);
        case KM_STATUS_UNAVAIL:  // File service not available
        case KS_STATUS_NODATA:   // No reply from host
            return (ERROR_DEVICE_NOT_MOUNTED);
        case MSG_STATUS_NO_MEM:
            return (ERROR_NO_FREE_STORE);
        case MSG_STATUS_BAD_LENGTH:  // Name too long for a message
            return (ERROR_INVALID_COMPONENT_NAME);
        case KM_STATUS_EOF:
            return (ERROR_NO_MORE_ENTRIES);
        case KM_STATUS_UNKCMD:
            return (ERROR_NOT_IMPLEMENTED);
        case KM_STATUS_PERM:
            return (ERROR_WRITE_PROTECTED);
        case KM_STATUS_INVALID:
            return (ERROR_OBJECT_WRONG_TYPE);
        case KM_STATUS_NOTEMPTY:
            return (ERROR_DIRECTORY_NOT_EMPTY);
        case KM_STATUS_NOEXIST:
            return (ERROR_OBJECT_NOT_FOUND);
        case KM_STATUS_EXIST:
            return (ERROR_OBJECT_EXISTS);
        case KM_STATUS_INUSE:
            return (ERROR_OBJECT_IN_USE);
        case KM_STATUS_STALE:
            /* Host restarted: check for that and reopen volume handles */
            volume_refresh_soon();
            return (ERROR_INVALID_LOCK);
        case KM_STATUS_LAST_ENTRY:
            return (ERROR_NO_MORE_ENTRIES);
        default:
            /* Communication failure (CRC, timeout, bad reply, ...) */
            printf("Status %x mapped to read/write error\n", status);
            return (ABORT_DISK_ERROR);
    }
}

/*
 * fs_open
 * -------
 * sm_fopen() which also holds a shared (SHARED_LOCK) or exclusive
 * (EXCLUSIVE_LOCK) lock on the object for as long as the handle is open,
 * if the host enforces locks. The host refuses a conflicting lock with
 * KM_STATUS_INUSE. With a host which does not support locks, no lock
 * is enforced.
 */
static uint
fs_open(handle_t phandle, const char *name, uint mode, LONG lockmode,
        uint *type, uint create_perms, handle_t *handle)
{
    uint rc;

    if (sm_host_locks) {
        uint lockbits = (lockmode == EXCLUSIVE_LOCK) ? HM_MODE_LOCK_EX :
                                                       HM_MODE_LOCK;
        rc = sm_fopen(phandle, name, mode | lockbits, type, create_perms,
                      handle);
        if (rc != KM_STATUS_INVALID)
            return (rc);
        /*
         * An older host may have replaced the one which advertised
         * locks; it rejects lock bits in some open modes. Retry without.
         * The next successful open reply updates sm_host_locks.
         */
    }
    return (sm_fopen(phandle, name, mode, type, create_perms, handle));
}

/*
 * strip_volume
 * ------------
 * DOS passes names to the handler as the application gave them, so a
 * name may begin with a device, volume, or assign name ("Work:dir/file").
 * As with other handlers, the prefix is stripped and the remainder is
 * relative to the lock DOS passed: for a device or volume, DOS passes no
 * lock (the volume root); for an assign, it passes the lock of the
 * assigned directory. The prefix must not be sent to the host, which
 * can't resolve it: DOS names are case-insensitive, fsname() may have
 * renamed the volume (spaces, duplicates), and assigns are unknown to
 * the host.
 *
 * A name beginning with ":" (root of the volume) is left unchanged, as
 * the host resolves that relative to the volume of the parent handle.
 */
static char *
strip_volume(char *name)
{
    char *ptr;

    for (ptr = name; *ptr != '\0'; ptr++) {
        if (*ptr == '/')
            break;  // A colon after a slash is not a volume separator
        if (*ptr == ':')
            return ((ptr == name) ? name : ptr + 1);
    }
    return (name);
}

/* Returns non-zero if the HM_TYPE_* is a directory (which can be scanned) */
static int
hm_type_is_dir(uint type)
{
    return ((type == HM_TYPE_DIR) || (type == HM_TYPE_VOLDIR) ||
            (type == HM_TYPE_VOLUME));
}

/*
 * CreateLock
 * ----------
 * Creates a DOS lock for the open host handle. type is the HM_TYPE_* of
 * the object, which determines whether EXAMINE_NEXT may scan it.
 */
fs_lock_t *
CreateLock(handle_t handle, handle_t phandle, uint mode, uint type)
{
    fs_lock_t    *lock;
    DeviceList_t *volnode = gvol->vl_volnode;

    /*
     * Lock conflicts (shared vs exclusive) are enforced by the host when
     * the handle is opened: see fs_open().
     */
    if (volnode == NULL) {
        gpack->dp_Res2 = ERROR_DEVICE_NOT_MOUNTED;
        printf("device is not mounted\n");
        return (NULL);
    }

    /* C= said that if it's not EXCLUSIVE, it's SHARED */
    if (mode != EXCLUSIVE_LOCK)
        mode = SHARED_LOCK;

    lock = (fs_lock_t *) AllocMem(sizeof (fs_lock_t), MEMF_PUBLIC);
    if (lock == NULL) {
        gpack->dp_Res2 = ERROR_NO_FREE_STORE;
        return (NULL);
    }

    lock->fl_Key        = handle;
    lock->fl_Access     = mode;
    lock->fl_Task       = gvol->vl_msgport;
    lock->fl_Volume     = CTOB(volnode);
    lock->fl_PHandle    = phandle;
    lock->fl_Flags      = hm_type_is_dir(type) ? FL_FLAG_IS_DIR : 0;

#define CREATELOCK_DEBUG
#ifdef CREATELOCK_DEBUG
    printf("  CreateLock: handle=%x phandle=%x type=%s\n", handle, phandle,
           ((mode == EXCLUSIVE_LOCK) ? "exclusive" : "shared"));
#endif

    Forbid();
        lock->fl_Link = volnode->dl_LockList;
        volnode->dl_LockList = CTOB(lock);
    Permit();

    gvol->vl_use_count++;
    return (lock);
}

/*
 * FreeLock
 * --------
 * Removes the lock from the volume's lock list and frees it.
 * Returns 0 on success, or -1 if the lock was not found in the list
 * (in which case it is not freed).
 */
int
FreeLock(fs_lock_t *lock)
{
    DeviceList_t *volnode = gvol->vl_volnode;
    fs_lock_t *current;
    fs_lock_t *parent;

#ifndef FAST
    if (lock == NULL) {
        printf("** ERROR - FreeLock called with NULL lock\n");
        return (-1);
    }
#endif

#define FREELOCK_DEBUG
#ifdef FREELOCK_DEBUG
    printf("  FreeLock: handle=%x phandle=%x flags=%x\n",
           lock->fl_Key, lock->fl_PHandle, lock->fl_Flags);
#endif

    parent = NULL;
    Forbid();
        current = (fs_lock_t *) BTOC(volnode->dl_LockList);
        while (current != NULL) {
            if (current == lock) {
                if (parent == NULL)  /* at head */
                    volnode->dl_LockList = current->fl_Link;
                else
                    parent->fl_Link      = current->fl_Link;
                break;
            }
            parent  = current;
            current = (fs_lock_t *) BTOC(current->fl_Link);
        }
    Permit();

    if (current == NULL) {
        printf("Did not find lock in global locklist\n");
        return (-1);
    }
    FreeMem(current, sizeof (fs_lock_t));
    gvol->vl_use_count--;
    return (0);
}

static ULONG
action_copy_dir(void)
{
    fs_lock_t *lock = (fs_lock_t *) BTOC(GARG1);
    fs_lock_t *newlock;
    uint       rc;
    uint       type;
    handle_t   handle;
    handle_t   phandle  = (lock == NULL) ? gvol->vl_handle : lock->fl_Key;
    handle_t   pphandle = (lock == NULL) ? 0 : lock->fl_PHandle;

    printf("COPY_DIR %x %x = ", phandle, pphandle);
    rc = fs_open(phandle, "", 0, SHARED_LOCK, &type, 0, &handle);
    if (rc == 0) {
        printf("%x\n", handle);
        newlock = CreateLock(handle, pphandle, SHARED_LOCK, type);
        if (newlock == NULL)
            sm_fclose(handle);
        return (CTOB(newlock));
    }
    printf("FAIL %d\n", rc);
    gpack->dp_Res2 = km_status_to_amiga_error(rc);
    return (DOSFALSE);
}

static ULONG
action_create_dir(void)
{
    fs_lock_t *newlock;
    fs_lock_t *lock    = (fs_lock_t *) BTOC(GARG1);
    char      *bname   = (char *) BTOC(GARG2);
    char      *name    = bname + 1;
    handle_t   phandle = (lock == NULL) ? gvol->vl_handle : lock->fl_Key;
    handle_t   handle;
    char       cho;
    uint       rc;
    uint       type;

    /* Temporarily NIL-terminate name */
    bname = name + (uint8_t) *bname;
    cho = *bname;
    *bname = '\0';
    name = strip_volume(name);

    printf("CREATE_DIR p=%x %p '%s'\n", phandle, lock, name);
    rc = sm_fcreate(phandle, name, "", HM_TYPE_DIR, 0);
    if (rc == 0) {
        rc = fs_open(phandle, name, HM_MODE_READDIR, EXCLUSIVE_LOCK,
                     &type, 0, &handle);
        if (rc != 0) {
            /*
             * Without a lock to return, the packet must fail: remove the
             * new directory so that the failure leaves nothing behind.
             */
            printf("  open of new dir failed %d; removing it\n", rc);
            (void) sm_fdelete(phandle, name);
        }
    }
    *bname = cho;

    if (rc != 0) {
        gpack->dp_Res2 = km_status_to_amiga_error(rc);
        return (DOSFALSE);
    }
    printf("  dir handle %x\n", handle);
    newlock = CreateLock(handle, phandle, EXCLUSIVE_LOCK, HM_TYPE_DIR);
    if (newlock == NULL)
        sm_fclose(handle);
    return (CTOB(newlock));
}

static ULONG
action_current_volume(void)
{
    return (CTOB(gvol->vl_volnode));
}

static ULONG
action_delete_object(void)
{
    fs_lock_t *lock    = (fs_lock_t *) BTOC(GARG1);
    char      *bname   = (char *) BTOC(GARG2);
    char      *name    = bname + 1;
    handle_t   phandle = (lock == NULL) ? gvol->vl_handle : lock->fl_Key;
    char       cho;
    uint       rc;

    /* Temporarily NIL-terminate name */
    bname = name + (uint8_t) *bname;
    cho = *bname;
    *bname = '\0';
    name = strip_volume(name);

    printf("DELETEOBJECT p=%x %p '%s'\n", phandle, lock, name);

    rc = sm_fdelete(phandle, name);
    *bname = cho;

    if (rc != 0) {
        gpack->dp_Res2 = km_status_to_amiga_error(rc);
        return (DOSFALSE);
    }
    return (DOSTRUE);
}

static ULONG
action_die(void)
{
    printf("DIE\n");
    grunning = 0;
    return (DOSTRUE);
}

/*
 * dirent_valid
 * ------------
 * Returns non-zero if a directory entry reply of rlen bytes holds a
 * complete entry with a NIL-terminated name. The entry length is only
 * read once the reply is known to contain the entry header.
 */
static int
dirent_valid(hm_fdirent_t *dent, uint rlen)
{
    uint entlen;

    if (rlen < sizeof (*dent))
        return (0);
    entlen = dent->hmd_elen;
    if ((entlen > 1024) || (rlen < sizeof (*dent) + entlen) ||
        (memchr(dent + 1, '\0', entlen) == NULL))
        return (0);
    return (1);
}

static void
FillInfoBlock(FileInfoBlock_t *fib, fileattr_t *fattr, hm_fdirent_t *dent)
{
    char *dname = (char *) (dent + 1);
    uint namelen = strlen(dname);
    uint type = ST_FILE;
    uint attype = NFNON;

    printf("name=%s is ", dname);
    fib->fib_DiskKey = dent->hmd_ino;
    switch (dent->hmd_type) {
        case HM_TYPE_FILE:
            type   = ST_FILE;
            attype = NFREG;
            printf("file");
            break;
        case HM_TYPE_DIR:
            type   = ST_USERDIR;
            attype = NFDIR;
            printf("dir");
            break;
        case HM_TYPE_LINK:
            type   = ST_SOFTLINK;
            attype = NFLNK;
            printf("link");
            break;
        case HM_TYPE_HLINK:
            type   = ST_LINKFILE;
            attype = NFLNK;
            printf("hlink");
            break;
        case HM_TYPE_FIFO:
            type   = ST_PIPEFILE;
            attype = NFFIFO;
            printf("fifo");
            break;
        case HM_TYPE_SOCKET:
            type   = ST_SOCKET;
            attype = NFSOCK;
            printf("socket");
            break;
        case HM_TYPE_BDEV:
            type   = ST_BDEVICE;
            attype = NFBLK;
            printf("bdev");
            break;
        case HM_TYPE_CDEV:
            type   = ST_CDEVICE;
            attype = NFCHR;
            printf("cdev");
            break;
        case HM_TYPE_WHTOUT:
            type   = ST_WHITEOUT;
            attype = NFNON;
            printf("whtout");
            break;
        case HM_TYPE_VOLUME:
        case HM_TYPE_VOLDIR:
            type   = ST_ROOT;
            attype = NFDIR;
            printf("root");
            break;
        default:
            printf("unknown %x", dent->hmd_type);
            break;
    }
    printf("\n");
    fib->fib_DirEntryType = type;

    if (namelen > sizeof (fib->fib_FileName) - 2)
        namelen = sizeof (fib->fib_FileName) - 2;
    strncpy(fib->fib_FileName + 1, dname, namelen);
    fib->fib_FileName[namelen + 1] = '\0';
    fib->fib_FileName[0] = strlen(fib->fib_FileName + 1);

    fib->fib_Protection = dent->hmd_aperms;
    fib->fib_EntryType = fib->fib_DirEntryType;  // must be the same
    if (dent->hmd_size_hi != 0)
        fib->fib_Size = 0xffffffff;
    else
        fib->fib_Size = dent->hmd_size_lo;
    if (dent->hmd_type == HM_TYPE_VOLUME) {
        /* Volume entry: hmd_blks is already in units of hmd_blksize */
        fib->fib_NumBlocks = dent->hmd_blks;
    } else {
        /*
         * Count blocks of the size the host reports for the filesystem,
         * which is also what ACTION_INFO reports as id_BytesPerBlock.
         * The host's own count (hmd_blks) is in 512-byte units on UNIX.
         */
        uint64_t size = ((uint64_t) dent->hmd_size_hi << 32) |
                        dent->hmd_size_lo;
        uint     bsize = (dent->hmd_blksize != 0) ? dent->hmd_blksize : 512;
        uint64_t blks = (size + bsize - 1) / bsize;
        fib->fib_NumBlocks = (blks > 0xffffffff) ? 0xffffffff : (ULONG) blks;
    }

    fib->fib_Comment[0] = 0;
    fib->fib_Comment[1] = '\0';
    fib->fib_OwnerUID = dent->hmd_ouid;
    fib->fib_OwnerGID = dent->hmd_ogid;

    unix_time_to_amiga_datestamp(dent->hmd_mtime, 0, &fib->fib_Date);
    memset(fib->fib_Reserved, 0, sizeof (fib->fib_Reserved));

    if (fattr != NULL) {
        memset(fattr, 0, sizeof (*fattr));
        fattr->fa_type = attype;
        fattr->fa_mode = dent->hmd_mode;
        fattr->fa_nlink = dent->hmd_nlink;
        fattr->fa_uid = dent->hmd_ouid;
        fattr->fa_gid = dent->hmd_ogid;
        fattr->fa_size = dent->hmd_size_lo;
        fattr->fa_blocksize = dent->hmd_blksize;
        fattr->fa_rdev = dent->hmd_rdev;
        fattr->fa_blocks = dent->hmd_blks;
        fattr->fa_fsid = 0;
        fattr->fa_fileid = dent->hmd_ino;
        fattr->fa_atime = dent->hmd_atime;
        fattr->fa_atime_us = 0;
        fattr->fa_mtime = dent->hmd_mtime;
        fattr->fa_mtime_us = 0;
        fattr->fa_ctime = dent->hmd_ctime;
        fattr->fa_ctime_us = 0;
    }
}

/*
 * examine_common() will fill in a FileInfoBlock structure for the
 * specified file (the lock).  It will optionally fill the fattr
 * structure as well, if non-NULL. A NULL lock refers to the root of
 * the volume.
 *
 * RES1 = Success (DOSTRUE) / Failure (DOSFALSE)
 * RES2 = Failure code when RES1 == DOSFALSE
 *        Can be ERROR_IS_SOFT_LINK or ERROR_OBJECT_NOT_FOUND.
 */
static ULONG
examine_common(fs_lock_t *lock, FileInfoBlock_t *fib, fileattr_t *fattr)
{
    handle_t handle;
    hm_fdirent_t *dent;
    uint type;
    uint rc;
    uint rlen;
    uint len = DIRENT_READ_LEN;
    handle_t lhandle = (lock == NULL) ? gvol->vl_handle : lock->fl_Key;

    /*
     * Follow symlinks: the host already follows them for everything else
     * done with the lock, so Examine describes the same object (the link
     * target). As with FFS, only directory scans report soft links.
     */
    rc = sm_fopen(lhandle, "", HM_MODE_READDIR, &type, 0, &handle);
    if (rc != 0) {
        gpack->dp_Res2 = km_status_to_amiga_error(rc);
        return (DOSFALSE);
    }

    rc = sm_fread(handle, len, (void **) &dent, &rlen, 0);
    if (rc != 0) {
        sm_fclose(handle);
        gpack->dp_Res2 = km_status_to_amiga_error(rc);
        return (DOSFALSE);
    }

    if (!dirent_valid(dent, rlen)) {
        printf("Corrupt dirent rlen=%x for %x\n", rlen, handle);
        gpack->dp_Res2 = ERROR_BAD_TEMPLATE;
        sm_fclose(handle);
        return (DOSFALSE);
    }

    FillInfoBlock(fib, fattr, dent);

    if (lock == NULL) {
        /* Root scan for a NULL lock needs rewind for EXAMINE_NEXT */
        gvol->vl_root_rewind = 1;
    } else if (hm_type_is_dir(type)) {
        /* Directory pointer needs rewind for EXAMINE_NEXT */
        lock->fl_Flags |= FL_FLAG_NEEDS_REWIND;
    }

    sm_fclose(handle);
    return (DOSTRUE);
}

static ULONG
action_disk_info(void)
{
    struct InfoData *infodata;
    handle_t handle = gvol->vl_handle;
    hm_fdirent_t *dent;
    uint rc;
    uint rlen;
    uint numblks = 1 << 20;
    uint numused = 1 << 19;
    uint blksize = 1024;
    uint len = DIRENT_READ_LEN;

    printf("DISK_INFO %x\n", handle);
    rc = sm_fread(handle, len, (void **) &dent, &rlen, HM_FLAG_SEEK0);
    if ((rc == 0) && (rlen >= sizeof (*dent))) {
        uint entlen = dent->hmd_elen;

        if (entlen > 1024) {
            printf("Corrupt entlen=%x\n", entlen);
        } else if ((dent->hmd_blksize == 0) || (dent->hmd_size_lo == 0)) {
            /* Callers divide by these; keep the defaults */
            printf("Bad volume size %u blocks of %u bytes\n",
                   dent->hmd_size_lo, dent->hmd_blksize);
        } else {
            numblks = dent->hmd_size_lo;
            numused = dent->hmd_blks;
            blksize = dent->hmd_blksize;
            if (numused > numblks)
                numused = numblks;
        }
    } else if (rc == KM_STATUS_STALE) {
        volume_refresh_soon();  // Host restarted; reopen volume handle
    }

    if (gpack->dp_Type == ACTION_INFO)
        infodata = (struct InfoData *) BTOC(GARG2);
    else
        infodata = (struct InfoData *) BTOC(GARG1);

    infodata->id_NumSoftErrors = 0;
    infodata->id_UnitNumber    = gvol->vl_handle;
    infodata->id_DiskState     = ID_VALIDATED;  // ID_WRITE_PROTECTED
    infodata->id_NumBlocks     = numblks;
    infodata->id_NumBlocksUsed = numused;
    infodata->id_BytesPerBlock = blksize;
    infodata->id_DiskType      = ID_FFS_DISK;
    infodata->id_VolumeNode    = CTOB(gvol->vl_volnode);
    infodata->id_InUse         = gvol->vl_use_count;

    return (DOSTRUE);
}

static ULONG
action_examine_next(void)
{
    fs_lock_t       *lock  = BTOC(GARG1);
    FileInfoBlock_t *fib   = (FileInfoBlock_t *) BTOC(GARG2);
    fileattr_t      *fattr = NULL;
    hm_fdirent_t    *dent;
    handle_t         handle;
    uint             rc;
    uint             rlen;
    uint             read_flag = 0;
    uint             len = DIRENT_READ_LEN;

    printf("EXAMINE_NEXT %p %x\n", lock, (lock == NULL) ? 0 : lock->fl_Key);
    if ((gpack->dp_Type == ACTION_EX_NEXT) && (GARG3 != 0))
        fattr = (fileattr_t *) GARG3;

    if (lock == NULL) {
        /*
         * A NULL lock is the volume root. The volume's own handle is
         * opened for stat only, so the root is scanned through a
         * separate handle which the volume keeps (one scan at a time,
         * as there is no lock to hold the position).
         */
        if (gvol->vl_root_scan == 0) {
            uint type;
            rc = sm_fopen(gvol->vl_handle, "", HM_MODE_READ, &type, 0,
                          &gvol->vl_root_scan);
            if (rc != 0) {
                gvol->vl_root_scan = 0;
                gpack->dp_Res2 = km_status_to_amiga_error(rc);
                return (DOSFALSE);
            }
            gvol->vl_root_rewind = 1;
        }
        handle = gvol->vl_root_scan;
        if (gvol->vl_root_rewind) {
            gvol->vl_root_rewind = 0;
            read_flag |= HM_FLAG_SEEK0;
        }
    } else {
        handle = lock->fl_Key;
        if ((lock->fl_Flags & FL_FLAG_IS_DIR) == 0) {
            /* Not a directory: don't read file content as dir entries */
            gpack->dp_Res2 = ERROR_NO_MORE_ENTRIES;
            return (DOSFALSE);
        }
        if (lock->fl_Flags & FL_FLAG_NEEDS_REWIND) {
            lock->fl_Flags &= ~FL_FLAG_NEEDS_REWIND;
            read_flag |= HM_FLAG_SEEK0;
        }
    }

    rc = sm_fread(handle, len, (void **) &dent, &rlen, read_flag);
    if (rc != 0) {
        printf("dir read err %x\n", rc);
        if ((lock == NULL) && (rc != KM_STATUS_EOF)) {
            /* Possibly stale (host restart): reopen on the next scan */
            sm_fclose(gvol->vl_root_scan);
            gvol->vl_root_scan = 0;
        }
        gpack->dp_Res2 = km_status_to_amiga_error(rc);
        return (DOSFALSE);
    }
    if (!dirent_valid(dent, rlen)) {
        printf("Corrupt dirent rlen=%x for %x\n", rlen, handle);
        gpack->dp_Res2 = ERROR_BAD_TEMPLATE;
        /* Handle belongs to the lock; it is closed by FREE_LOCK */
        return (DOSFALSE);
    }

    FillInfoBlock(fib, fattr, dent);
    return (DOSTRUE);
}

static ULONG
action_examine_object(void)
{
    fs_lock_t       *lock  = BTOC(GARG1);
    FileInfoBlock_t *fib   = (FileInfoBlock_t *) BTOC(GARG2);
    fileattr_t      *fattr = NULL;

    printf("EXAMINE_OBJECT %p %x\n", lock,
           (lock == NULL) ? gvol->vl_handle : lock->fl_Key);
    if ((gpack->dp_Type == ACTION_EX_OBJECT) && (GARG3 != 0))
        fattr = (fileattr_t *) GARG3;

    return (examine_common(lock, fib, fattr));
}

static ULONG
action_end(void)
{
    fh_private_t *fp  = (fh_private_t *) GARG1;  // Comes from fh_Arg1

    printf("END %p\n", fp);
    if (fp != NULL) {
        fs_lock_t *lock   = (fs_lock_t *) fp->fp_lock;
        handle_t   handle = fp->fp_handle;
        sm_fclose(handle);
        if (lock != NULL)
            FreeLock(lock);
        FreeMem(fp, sizeof (*fp));
    }
    return (DOSTRUE);
}

static ULONG
action_findinput(void)
{
    FileHandle_t *fh     = (FileHandle_t *) BTOC(GARG1);
    fs_lock_t    *lock   = (fs_lock_t *) BTOC(GARG2);
    fs_lock_t    *newlock;
    fh_private_t *fp;
    char         *bname  = (char *) BTOC(GARG3);
    char         *name   = bname + 1;
    handle_t      phandle = (lock == NULL) ? gvol->vl_handle : lock->fl_Key;
    handle_t      handle;
    char          cho;
    uint          rc;
    uint          type;
    uint          hm_mode = HM_MODE_READ | HM_MODE_WRITE;
    uint          create_perms = 0;

    /* Temporarily NIL-terminate name */
    bname = name + (uint8_t) *bname;
    cho = *bname;
    *bname = '\0';
    name = strip_volume(name);

    if (gpack->dp_Type == ACTION_FINDUPDATE) {
        hm_mode = HM_MODE_READ | HM_MODE_WRITE | HM_MODE_CREATE;
    }

    printf("FIND%s p=%x %p '%s'\n",
           (hm_mode & HM_MODE_WRITE) ? "UPDATE" : "INPUT", phandle, lock, name);

    rc = fs_open(phandle, name, hm_mode, SHARED_LOCK, &type, create_perms,
                 &handle);

    if ((rc != 0) && (rc != KM_STATUS_INUSE)) {
        /*
         * Try again without write access. As with FFS, a write-protected
         * file may still be opened; writes to it will then fail.
         */
        uint rc2 = fs_open(phandle, name, HM_MODE_READ, SHARED_LOCK, &type,
                           create_perms, &handle);
        /*
         * If that also fails, report the first error: for FINDUPDATE it
         * explains why the file could not be created (for example, a
         * write-protected directory) rather than "object not found".
         */
        if ((rc2 == 0) || (gpack->dp_Type != ACTION_FINDUPDATE))
            rc = rc2;
    }
    *bname = cho;

    if (rc != 0) {
        gpack->dp_Res2 = km_status_to_amiga_error(rc);
        return (DOSFALSE);
    }

    if (hm_type_is_dir(type)) {
        /* As with FFS, a directory can't be opened as a file */
        printf("  is a directory\n");
        sm_fclose(handle);
        gpack->dp_Res2 = ERROR_OBJECT_WRONG_TYPE;
        return (DOSFALSE);
    }

    newlock = CreateLock(handle, phandle, SHARED_LOCK, type);
    if (newlock == NULL) {
        sm_fclose(handle);
        return (DOSFALSE);
    }

    fp = AllocMem(sizeof (*fp), MEMF_PUBLIC);
    if (fp == NULL) {
        sm_fclose(handle);
        FreeLock(newlock);
        gpack->dp_Res2 = ERROR_NO_FREE_STORE;
        return (DOSFALSE);
    }
    fp->fp_lock    = newlock;
    fp->fp_fh      = fh;
    fp->fp_handle  = handle;
    fp->fp_pos_cur = 0;
    fp->fp_pos_max = 0;

    fh->fh_Port = NULL;            // Non-zero only if interactive
    fh->fh_Type = gvol->vl_msgport;   // Handler message port
    fh->fh_Arg1 = (uintptr_t) fp;  // Filesystem-internal file identifier
    printf("  fp=%p fh=%p handle=%x\n", fp, fh, handle);

    return (DOSTRUE);
}

static ULONG
action_flush(void)
{
    return (DOSTRUE);
}

static ULONG
action_findoutput(void)
{
    FileHandle_t *fh     = (FileHandle_t *) BTOC(GARG1);
    fs_lock_t    *lock   = (fs_lock_t *) BTOC(GARG2);
    fs_lock_t    *newlock;
    fh_private_t *fp;
    char         *bname  = (char *) BTOC(GARG3);
    char         *name   = bname + 1;
    handle_t      phandle = (lock == NULL) ? gvol->vl_handle : lock->fl_Key;
    handle_t      handle;
    char          cho;
    uint          rc;
    uint          type;
    uint          hm_mode = HM_MODE_WRITE | HM_MODE_CREATE | HM_MODE_TRUNC;
    uint          create_perms = 0;

    /* Temporarily NIL-terminate name */
    bname = name + (uint8_t) *bname;
    cho = *bname;
    *bname = '\0';
    name = strip_volume(name);

    printf("FINDOUTPUT p=%x %p '%s'\n", phandle, lock, name);

    rc = fs_open(phandle, name, hm_mode, EXCLUSIVE_LOCK, &type, create_perms,
                 &handle);
    *bname = cho;

    if (rc != 0) {
        printf("fopen failed with %d\n", rc);
        gpack->dp_Res2 = km_status_to_amiga_error(rc);
        return (DOSFALSE);
    }

    newlock = CreateLock(handle, phandle, EXCLUSIVE_LOCK, HM_TYPE_FILE);
    if (newlock == NULL) {
        sm_fclose(handle);
        return (DOSFALSE);
    }

    fp = AllocMem(sizeof (*fp), MEMF_PUBLIC);
    if (fp == NULL) {
        sm_fclose(handle);
        FreeLock(newlock);
        gpack->dp_Res2 = ERROR_NO_FREE_STORE;
        return (DOSFALSE);
    }
    fp->fp_lock    = newlock;
    fp->fp_fh      = fh;
    fp->fp_handle  = handle;
    fp->fp_pos_cur = 0;
    fp->fp_pos_max = 0;

    fh->fh_Port = NULL;            // Non-zero only if interactive
    fh->fh_Type = gvol->vl_msgport;   // Handler message port
    fh->fh_Arg1 = (uintptr_t) fp;  // Filesystem-internal file identifier
    printf("  fp=%p fh=%p handle=%x\n", fp, fh, handle);

    return (DOSTRUE);
}

static ULONG
action_free_disk_fssm(void)
{
    /* Nothing to do since the FSSM is not supported */
    return (DOSTRUE);
}

static ULONG
action_free_lock(void)
{
    fs_lock_t *lock = (fs_lock_t *) BTOC(GARG1);

    handle_t   handle;

    printf("FREE_LOCK %p\n", lock);
    if (lock == NULL)
        return (DOSTRUE);  /* Freeing a NULL lock is a no-op */

    /*
     * Only close the handle of a lock known to this volume: an invalid
     * lock's key could be the handle of some other open lock or file.
     */
    handle = lock->fl_Key;
    if (FreeLock(lock) != 0) {
        gpack->dp_Res2 = ERROR_INVALID_LOCK;
        return (DOSFALSE);
    }
    sm_fclose(handle);
    return (DOSTRUE);
}

/*
 * action_get_disk_fssm
 * --------------------
 * Used to retrieve the filesystem startup message, which is used by
 * programs attempting to access the underlying trackdisk.device style
 * block driver. Since smashfs does not sit on top of this type of
 * device, the spec says that dp.Res1 should be 0 and dp.Res2 should
 * be ERROR_OBJECT_WRONG_TYPE.
 */
static ULONG
action_get_disk_fssm(void)
{
    gpack->dp_Res2 = ERROR_OBJECT_WRONG_TYPE;
    return (DOSFALSE);
}


static ULONG
action_is_filesystem(void)
{
    printf("IS_FILESYSTEM\n");
    return (DOSTRUE);
}

static ULONG
action_locate_object(void)
{
    fs_lock_t *newlock;
    fs_lock_t *lock   = (fs_lock_t *) BTOC(GARG1);
    char      *bname  = (char *) BTOC(GARG2);
    LONG       access = GARG3;
    char      *name   = bname + 1;
    char       cho;
    uint       rc;
    uint       type;
    handle_t   phandle = (lock == NULL) ? gvol->vl_handle : lock->fl_Key;
    handle_t   handle;

    /* Temporarily NIL-terminate name */
    bname = name + (uint8_t) *bname;
    cho = *bname;
    *bname = '\0';
    name = strip_volume(name);

    printf("LOCATE_OBJECT lock=%p phandle=%x name='%s' for %s\n",
           lock, phandle, name,
           (access == ACCESS_READ) ? "read" :
           (access == ACCESS_WRITE) ? "write" : "unknown");

    if (*name == '\0')
        name = ".";

    /*
     * The lock's handle is always opened for read, even for an exclusive
     * lock: no I/O is done through it, and exclusivity is enforced by the
     * host lock bits (see fs_open()). Opening for write would fail for
     * directories and write-protected files, which FFS can lock
     * exclusively.
     */
    rc = fs_open(phandle, name, HM_MODE_READ, access, &type, 0, &handle);
    if ((rc != 0) && (rc != KM_STATUS_INUSE)) {
        /* Attempt to open for stat (for example, a file without read perms) */
        rc = fs_open(phandle, name, HM_MODE_READDIR, access, &type, 0,
                     &handle);
    }
    *bname = cho;
    if (rc != 0) {
        printf("failed open with %x\n", rc);
        gpack->dp_Res2 = km_status_to_amiga_error(rc);
        return (DOSFALSE);
    }
    newlock = CreateLock(handle, phandle, access, type);
    if (newlock == NULL)
        sm_fclose(handle);
    return (CTOB(newlock));
}

static ULONG
action_make_link(void)
{
    fs_lock_t *lock     = (fs_lock_t *) BTOC(GARG1);
    char      *bname    = (char *) BTOC(GARG2);
    char      *name     = bname + 1;
    ULONG      linktype = GARG4;
    handle_t   phandle  = (lock == NULL) ? gvol->vl_handle : lock->fl_Key;
    char      *target = "";
    char       cho;
    uint       rc;
    uint       type;

    printf("MAKE_LINK p=%x %p '%.*s' %s\n", phandle, lock,
           (int) (uint8_t) *bname, name,
           (linktype == LINK_HARD) ? "hard" : "soft");

    if (linktype == LINK_SOFT) {
        target = (char *) GARG3;
        printf("  target=%s\n", target);
    } else {
        /* LINK_HARD */
        fs_lock_t *tlock   = (fs_lock_t *) BTOC(GARG3);
        handle_t   thandle = (tlock == NULL) ? gvol->vl_handle : tlock->fl_Key;

        /* Get target path */
        rc = sm_fpath(thandle, &target);
        if (rc != 0) {
            printf("sm_fpath failed with %d\n", rc);
            gpack->dp_Res2 = km_status_to_amiga_error(rc);
            return (DOSFALSE);
        }
        printf("  target=%s\n", target);
    }

    /* Temporarily NIL-terminate name */
    bname = name + (uint8_t) *bname;
    cho = *bname;
    *bname = '\0';
    name = strip_volume(name);

    type = (linktype == LINK_SOFT) ? HM_TYPE_LINK : HM_TYPE_HLINK;
    rc = sm_fcreate(phandle, name, target, type, 0);
    *bname = cho;

    if (rc != 0) {
        printf("  failed with %d\n", rc);
        gpack->dp_Res2 = km_status_to_amiga_error(rc);
        return (DOSFALSE);
    }
    return (DOSTRUE);
}


static ULONG
action_parent(void)
{
    fs_lock_t *lock = (fs_lock_t *) BTOC(GARG1);
    fs_lock_t *newlock;
    handle_t handle;
    handle_t phandle = (lock == NULL) ? gvol->vl_handle : lock->fl_Key;
    char    *name;
    char    *colon;
    uint     type;
    uint     rc;

    printf("PARENT\n");
    if (lock == NULL) {
        /* NULL lock is the volume root, which has no parent */
        return (0);
    }

    rc = sm_fpath(phandle, &name);
    if (rc != 0) {
        gpack->dp_Res2 = km_status_to_amiga_error(rc);
        return (DOSFALSE);
    }

    /*
     * The path is "Volume:" at the root, or "Volume:dir/name" otherwise.
     * A host file name may itself contain ':' or end with '/', so only
     * the first ':' separates the volume, and it must end the path (with
     * an optional trailing '/') for the lock to be at the root.
     */
    colon = strchr(name, ':');
    if (colon == NULL) {
        printf("  unexpected path '%s'\n", name);
        gpack->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        return (DOSFALSE);
    }
    if ((colon[1] == '\0') || ((colon[1] == '/') && (colon[2] == '\0'))) {
        /* At root of volume */
        gpack->dp_Res2 = 0;
        return (0);
    }

    /* The host resolves "/" relative to a handle as its parent directory */
    printf("  parent of %s\n", name);
    rc = fs_open(phandle, "/", HM_MODE_READ, SHARED_LOCK, &type, 0, &handle);
    if (rc != 0) {
        printf("failed parent open with %x\n", rc);
        gpack->dp_Res2 = ERROR_DIR_NOT_FOUND;
        return (DOSFALSE);
    }
    newlock = CreateLock(handle, 0, SHARED_LOCK, type);
    if (newlock == NULL)
        sm_fclose(handle);
    return (CTOB(newlock));
}

static ULONG
action_read(void)
{
    fh_private_t *fp  = (fh_private_t *) GARG1;  // Comes from fh_Arg1
    char         *buf = (char *) GARG2;
    LONG          len = (LONG) GARG3;
    handle_t      handle;
    uint          rc = 0;
    void         *data;
    uint          rlen;
    uint          count = 0;

    if (fp == NULL) {
        gpack->dp_Res2 = ERROR_REQUIRED_ARG_MISSING;
        return ((ULONG) -1);
    }
    if (len <= 0)
        return (0);
    handle = fp->fp_handle;
    printf("READ %x at pos=%llx len=%x\n", handle, fp->fp_pos_cur, len);

    while (count < (uint) len) {
        uint remain = (uint) len - count;
        rc = sm_fread(handle, remain, &data, &rlen, 0);
        if ((rc != 0) && (rc != KM_STATUS_EOF))
            printf("sm_fread got %d\n", rc);
        if (rlen == 0) {
            if (rc != KM_STATUS_EOF)
                printf("Failed to read %x at pos=%llx, count=%x: %d\n",
                       handle, fp->fp_pos_cur, count, rc);
            break;
        }
        if (rlen > remain)
            rlen = remain;
        memcpy(buf, data, rlen);
        buf            += rlen;
        count          += rlen;
        fp->fp_pos_cur += rlen;
        if (fp->fp_pos_max < fp->fp_pos_cur)
            fp->fp_pos_max = fp->fp_pos_cur;
        if (rc == KM_STATUS_EOF)
            break;
    }
    if ((rc != 0) && (rc != KM_STATUS_EOF) && (count == 0)) {
        /* Error with nothing read; partial data is returned below */
        gpack->dp_Res2 = km_status_to_amiga_error(rc);
        return ((ULONG) -1);
    }
    return (count);  /* 0 at end of file */
}

static ULONG
action_read_link(void)
{
    fs_lock_t *lock   = (fs_lock_t *) BTOC(GARG1);
    char      *name   = (char *) GARG2;
    char      *buf    = (char *) GARG3;
    char      *linkpath;
    ULONG      buflen = GARG4;
    handle_t   handle;
    handle_t   phandle;
    uint type;
    uint rc;
    uint rlen;

    if ((lock == NULL) || (name == NULL) || (buf == NULL) || (buflen == 0)) {
        gpack->dp_Res2 = ERROR_REQUIRED_ARG_MISSING;
        return (DOSFALSE);
    }
    phandle = lock->fl_Key;
    name = strip_volume(name);

    printf("ACTION_READ_LINK %x '%s' %p %ul\n", phandle, name, buf, buflen);

    rc = sm_fopen(phandle, name, HM_MODE_READLINK, &type, 0, &handle);
    if (rc != 0) {
        gpack->dp_Res2 = km_status_to_amiga_error(rc);
        return (DOSFALSE);
    }

    rc = sm_fread(handle, 1024, (void **) &linkpath, &rlen, 0);
    if (rc != 0) {
        sm_fclose(handle);
        gpack->dp_Res2 = km_status_to_amiga_error(rc);
        return (DOSFALSE);
    }

    if (rlen > buflen - 1)
        rlen = buflen - 1;
    memcpy(buf, linkpath, rlen);
    buf[rlen] = '\0';

    sm_fclose(handle);
    return (rlen);
}

static ULONG
action_rename_object(void)
{
    fs_lock_t *slock  = (fs_lock_t *) BTOC(GARG1);
    char      *sbname = (char *) BTOC(GARG2);
    fs_lock_t *dlock  = (fs_lock_t *) BTOC(GARG3);
    char      *dbname = (char *) BTOC(GARG4);
    char      *sname;
    char      *dname;
    handle_t   shandle;
    handle_t   dhandle;
    char       scho;
    char       dcho;
    uint       rc;

    if ((sbname == NULL) || (dbname == NULL) ||
        (*sbname == '\0') || (*dbname == '\0')) {
        gpack->dp_Res2 = ERROR_REQUIRED_ARG_MISSING;
        return (DOSFALSE);
    }

    sname = sbname + 1;
    dname = dbname + 1;
    shandle = (slock == NULL) ? gvol->vl_handle : slock->fl_Key;
    dhandle = (dlock == NULL) ? gvol->vl_handle : dlock->fl_Key;

    /* Temporarily NIL-terminate names (careful: order matters here) */
    sbname = sname + (uint8_t) *sbname;
    dbname = dname + (uint8_t) *dbname;
    scho = *sbname;
    dcho = *dbname;
    *sbname = '\0';
    *dbname = '\0';
    sname = strip_volume(sname);
    dname = strip_volume(dname);

    printf("RENAMEOBJECT p=%x %p '%s' -> %p '%s'\n",
           shandle, slock, sname, dlock, dname);

    rc = sm_frename(shandle, sname, dhandle, dname);
    *sbname = scho;
    *dbname = dcho;

    if (rc != 0) {
        gpack->dp_Res2 = km_status_to_amiga_error(rc);
        return (DOSFALSE);
    }
    return (DOSTRUE);
}

static ULONG
action_seek(void)
{
    fh_private_t *fp = (fh_private_t *) GARG1;  // Comes from fh_Arg1
    LONG          offset = (LONG) GARG2;  /* Signed: may seek backward */
    LONG          seek_mode = GARG3;
    uint64_t      prev_pos;
    uint64_t      new_pos;
    handle_t      handle;
    uint          rc;

    if (fp == NULL) {
        gpack->dp_Res2 = ERROR_REQUIRED_ARG_MISSING;
        return ((ULONG) -1);
    }
    handle = fp->fp_handle;

    printf("SEEK %x to %x mode %d\n", handle, offset, seek_mode);

    /* Fix up bad apps (like 3.2.2 TextEdit) which supply other values */
    if (seek_mode < 0)
        seek_mode = OFFSET_BEGINNING;
    else if (seek_mode > 0)
        seek_mode = OFFSET_END;

    /* Sign-extend so the host sees negative offsets as negative */
    rc = sm_fseek(handle, seek_mode, (uint64_t) (int64_t) offset,
                  &new_pos, &prev_pos);
    if (rc != 0) {
        printf("fseek(%x) to %d failed: %d\n", handle, offset, rc);
        gpack->dp_Res2 = ERROR_SEEK_ERROR;
        return ((ULONG) -1);
    }
    printf("  new_pos=%llx prev_pos=%llx\n", new_pos, prev_pos);

    fp->fp_pos_cur = new_pos;
    if (fp->fp_pos_max < fp->fp_pos_cur)
        fp->fp_pos_max = fp->fp_pos_cur;

    if (prev_pos > 0xffffffff)
        prev_pos = 0xffffffff;
    return ((uint32_t) prev_pos);
}

static ULONG
action_set_protect(void)
{
    fs_lock_t *lock  = (fs_lock_t *) BTOC(GARG2);
    char      *bname = (char *) BTOC(GARG3);
    char      *name  = bname + 1;
    ULONG      prot  = GARG4;
    char       cho;
    uint       rc;
    handle_t   phandle = (lock == NULL) ? gvol->vl_handle : lock->fl_Key;

    /* Temporarily NIL-terminate name */
    bname = name + (uint8_t) *bname;
    cho = *bname;
    *bname = '\0';
    name = strip_volume(name);

    printf("SET_PROTECT lock=%p phandle=%x name='%s' prot=%x\n",
           lock, phandle, name, prot);

    rc = sm_fsetprotect(phandle, name, prot);
    *bname = cho;

    if (rc != 0) {
        printf("failed set_protect with %x\n", rc);
        gpack->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
        return (DOSFALSE);
    }
    return (DOSTRUE);
}

static ULONG
action_same_lock(void)
{
    fs_lock_t *lock1 = (fs_lock_t *) BTOC(GARG1);
    fs_lock_t *lock2 = (fs_lock_t *) BTOC(GARG2);
    handle_t handle1;
    handle_t handle2;
    char *name1;
    char *name2;
    uint  name1_len;
    uint  rc;

    handle1 = (lock1 == NULL) ? gvol->vl_handle : lock1->fl_Key;
    handle2 = (lock2 == NULL) ? gvol->vl_handle : lock2->fl_Key;

    printf("SAMELOCK %p %u %p %u\n", lock1, handle1, lock2, handle2);

    if (handle1 == handle2) {
        /* Same exact handle */
        goto lock_same;
    }

    /* Otherwise, we need to compare paths */
    rc = sm_fpath(handle1, &name2);
    if (rc != 0) {
        gpack->dp_Res2 = km_status_to_amiga_error(rc);
        return (DOSFALSE);
    }

    name1_len = strlen(name2) + 1;
    name1 = AllocMem(name1_len, MEMF_PUBLIC);
    if (name1 == NULL) {
        gpack->dp_Res2 = ERROR_NO_FREE_STORE;
        return (DOSFALSE);
    }
    strcpy(name1, name2);  /* name2 buffer is reused by next sm_fpath() */
    rc = sm_fpath(handle2, &name2);
    if (rc != 0) {
        gpack->dp_Res2 = km_status_to_amiga_error(rc);
        FreeMem(name1, name1_len);
        return (DOSFALSE);
    }

    printf("Compare %s with %s\n", name1, name2);
    rc = strcmp(name1, name2);
    FreeMem(name1, name1_len);

    if (rc == 0) {
lock_same:
        /* Same handle path */
        gpack->dp_Res2 = LOCK_SAME;
        return (DOSTRUE);
    } else if (((lock1 == NULL) ? CTOB(gvol->vl_volnode) : lock1->fl_Volume) ==
               ((lock2 == NULL) ? CTOB(gvol->vl_volnode) : lock2->fl_Volume)) {
        /* A NULL lock is the root of this handler's volume */
        /* Same volume, different handle */
        gpack->dp_Res2 = LOCK_SAME_VOLUME;
        return (DOSFALSE);
    } else {
        /* Different volume, different handle */
        gpack->dp_Res2 = LOCK_DIFFERENT;
        return (DOSFALSE);
    }
}

static ULONG
action_set_date(void)
{
    fs_lock_t        *lock  = (fs_lock_t *) BTOC(GARG2);
    char             *bname = (char *) BTOC(GARG3);
    char             *name  = bname + 1;
    struct DateStamp *ds    = (struct DateStamp *) GARG4;
    handle_t phandle = (lock == NULL) ? gvol->vl_handle : lock->fl_Key;
    char     cho;
    uint     rc;
    uint     sec;
    uint     nsec;

    sec = amiga_datestamp_to_unix_time(ds, &nsec);

    /*
     * Example
     * setdate tt:9 12-10-2024 10:30:02
     * Fri Oct 12 10:30:02 PDT 2024
     *     Days 17086
     *     Min 630
     *     Tick 100
     *
     * 17086 / 365.25 + 1978 = 2024.77891
     *         0.77891 * 365 = 284.3
     * This is October 10, 2024 according to date +%j
     */

    bname = name + (uint8_t) *bname;
    cho = *bname;
    *bname = '\0';
    name = strip_volume(name);
    printf("SET_DATE %s %p %u.%u\n", name, ds, sec, nsec);
    printf("  Days %u\n", ds->ds_Days);
    printf("  Min %u\n", ds->ds_Minute);
    printf("  Tick %u\n", ds->ds_Tick);

    rc = sm_fsetdate(phandle, name, 0, &sec, &nsec);
    *bname = cho;

    if (rc != 0) {
        printf("failed set_date with %x\n", rc);
        gpack->dp_Res2 = km_status_to_amiga_error(rc);
        return (DOSFALSE);
    }
    return (DOSTRUE);
}

static ULONG
action_set_dates(void)
{
    uint              which = GARG1;
    fs_lock_t        *lock  = (fs_lock_t *) BTOC(GARG2);
    char             *bname = (char *) BTOC(GARG3);
    char             *name  = bname + 1;
    struct DateStamp *ds    = (struct DateStamp *) GARG4;
    handle_t phandle = (lock == NULL) ? gvol->vl_handle : lock->fl_Key;
    char     cho;
    uint     rc;
    uint     sec;
    uint     nsec;

    sec = amiga_datestamp_to_unix_time(ds, &nsec);

    bname = name + (uint8_t) *bname;
    cho = *bname;
    *bname = '\0';
    name = strip_volume(name);
    printf("SET_DATES %s %p %u.%u\n", name, ds, sec, nsec);
    printf("  Days %u\n", ds->ds_Days);
    printf("  Min %u\n", ds->ds_Minute);
    printf("  Tick %u\n", ds->ds_Tick);

    rc = sm_fsetdate(phandle, name, which, &sec, &nsec);
    *bname = cho;

    if (rc != 0) {
        printf("failed set_dates with %x\n", rc);
        gpack->dp_Res2 = km_status_to_amiga_error(rc);
        return (DOSFALSE);
    }

    if ((which == 1) || (which == 3) || (which == 5)) {
        /* Get date (1 = modify, 3 = change, 5 = access) */
        unix_time_to_amiga_datestamp(sec, nsec, ds);
    }

    return (DOSTRUE);
}

static ULONG
action_set_owner(void)
{
    fs_lock_t *lock = (fs_lock_t *) BTOC(GARG2);
    char      *bname = (char *) BTOC(GARG3);
    char      *name  = bname + 1;
    ULONG      owner = GARG4;
    handle_t   phandle = (lock == NULL) ? gvol->vl_handle : lock->fl_Key;
    char       cho;
    uint       rc;
    uint       oid;
    uint       gid;

    bname = name + (uint8_t) *bname;
    cho = *bname;
    *bname = '\0';
    name = strip_volume(name);
    printf("SET_OWNER %s %x\n", name, owner);

    oid = owner >> 16;
    gid = owner & 0xffff;

    rc = sm_fsetown(phandle, name, oid, gid);
    *bname = cho;

    if (rc != 0) {
        printf("failed set_owner with %x\n", rc);
        gpack->dp_Res2 = km_status_to_amiga_error(rc);
        return (DOSFALSE);
    }
    return (DOSTRUE);
}

static ULONG
action_undisk_info(void)
{
    return (DOSTRUE);
}

/*
 * Private buffer for ACTION_WRITE data (see action_write()), with room for
 * the hm_freadwrite_t message header in front of the data. It is allocated
 * on first use and kept until the handler exits.
 */
#ifndef WRITE_BOUNCE_SIZE
#define WRITE_BOUNCE_SIZE (64 * 1024)
#endif
#define WRITE_BOUNCE_MIN  (4 * 1024)   // Fallback if memory is short
static uint8_t *write_bounce      = NULL;
static uint     write_bounce_size = 0;

static uint8_t *
write_bounce_get(void)
{
    uint size;

    if (write_bounce != NULL)
        return (write_bounce);
    for (size = WRITE_BOUNCE_SIZE; size >= WRITE_BOUNCE_MIN; size /= 4) {
        write_bounce = AllocMem(size + sizeof (hm_freadwrite_t), MEMF_PUBLIC);
        if (write_bounce != NULL) {
            write_bounce_size = size;
            break;
        }
    }
    return (write_bounce);
}

static ULONG
action_write(void)
{
    fh_private_t *fp  = (fh_private_t *) GARG1;  // Comes from fh_Arg1
    uint8_t      *buf = (uint8_t *) GARG2;
    LONG          len = (LONG) GARG3;
    handle_t      handle;
    uint          rc;
    uint          count = 0;

    if (fp == NULL) {
        gpack->dp_Res2 = ERROR_REQUIRED_ARG_MISSING;
        return ((ULONG) -1);
    }
    if (len <= 0)
        return (0);
    handle = fp->fp_handle;
    printf("WRITE %x buf=%p at pos=%llx len=%x\n",
           handle, buf, fp->fp_pos_cur, len);

    /*
     * The message layer temporarily writes message headers into the
     * buffer being sent (sm_fwrite() and host_send_msg()). The caller's
     * buffer must not be modified: it may be in ROM or write-protected
     * memory, or be read by another task at the same time. Data is
     * therefore copied into a private buffer which has room for the
     * message header, and sent in chunks of up to that buffer's size.
     */
    if (write_bounce_get() == NULL) {
        gpack->dp_Res2 = ERROR_NO_FREE_STORE;
        return ((ULONG) -1);
    }
    while (count < (uint) len) {
        uint chunk = (uint) len - count;
        if (chunk > write_bounce_size)
            chunk = write_bounce_size;
        memcpy(write_bounce + sizeof (hm_freadwrite_t), buf + count, chunk);
        rc = sm_fwrite(handle, write_bounce, chunk, 1, 0);
        if (rc != 0) {
            printf("sm_fwrite(%x) got %d at pos=%llx, count=%x\n",
                   handle, rc, fp->fp_pos_cur, count);
            gpack->dp_Res2 = km_status_to_amiga_error(rc);
            return ((ULONG) -1);
        }
        count          += chunk;
        fp->fp_pos_cur += chunk;
        if (fp->fp_pos_max < fp->fp_pos_cur)
            fp->fp_pos_max = fp->fp_pos_cur;
    }

    return (count);
}

/*
 * fs_packet_cleanup
 * -----------------
 * Releases resources held by packet handling, at handler exit.
 */
void
fs_packet_cleanup(void)
{
    if (write_bounce != NULL) {
        FreeMem(write_bounce, write_bounce_size + sizeof (hm_freadwrite_t));
        write_bounce = NULL;
    }
}

void
handle_packet(void)
{
    ULONG res1;
    printf("vol=%s CMD=%u %x %x %x %x\n",
           gvol->vl_name, gpack->dp_Type, GARG1, GARG2, GARG3, GARG4);

    if (!grunning) {
        switch (gpack->dp_Type) {
            case ACTION_FREE_LOCK:
            case ACTION_END:
                /* Allow these packets, as they release resources */
                printf("not running but allowed\n");
                break;
            default:
                printf("not running, rejected %d\n", gpack->dp_Type);
                gpack->dp_Res1 = DOSFALSE;
                gpack->dp_Res2 = ERROR_DEVICE_NOT_MOUNTED;
                return;
        }
    }

    gpack->dp_Res2 = 0;
    switch (gpack->dp_Type) {
        case ACTION_NIL:
            res1 = DOSTRUE;
            break;
        case ACTION_COPY_DIR:
            res1 = action_copy_dir();
            break;
        case ACTION_CREATE_DIR:
            res1 = action_create_dir();
            break;
        case ACTION_CURRENT_VOLUME:
            res1 = action_current_volume();
            break;
        case ACTION_DELETE_OBJECT:
            res1 = action_delete_object();
            break;
        case ACTION_DIE:
            res1 = action_die();
            break;
        case ACTION_DISK_INFO:
        case ACTION_INFO:
            res1 = action_disk_info();
            break;
        case ACTION_END:
            res1 = action_end();
            break;
        case ACTION_EXAMINE_OBJECT:
        case ACTION_EX_OBJECT:  // AS225
            res1 = action_examine_object();
            break;
        case ACTION_EXAMINE_NEXT:
        case ACTION_EX_NEXT:
            res1 = action_examine_next();
            break;
        case ACTION_FINDINPUT:
        case ACTION_FINDUPDATE:
            res1 = action_findinput();
            break;
        case ACTION_FINDOUTPUT:
            res1 = action_findoutput();
            break;
        case ACTION_FLUSH:
            res1 = action_flush();
            break;
        case ACTION_FREE_DISK_FSSM:
            res1 = action_free_disk_fssm();
            break;
        case ACTION_FREE_LOCK:
            res1 = action_free_lock();
            break;
        case ACTION_GET_DISK_FSSM:
            res1 = action_get_disk_fssm();
            break;
        case ACTION_IS_FILESYSTEM:
            res1 = action_is_filesystem();
            break;
        case ACTION_LOCATE_OBJECT:
            res1 = action_locate_object();
            break;
        case ACTION_MAKE_LINK:
            res1 = action_make_link();
            break;
        case ACTION_PARENT:
            res1 = action_parent();
            break;
        case ACTION_READ:
            res1 = action_read();
            break;
        case ACTION_READ_LINK:
            res1 = action_read_link();
            break;
        case ACTION_RENAME_OBJECT:
            res1 = action_rename_object();
            break;
        case ACTION_SAME_LOCK:
            res1 = action_same_lock();
            break;
        case ACTION_SEEK:
            res1 = action_seek();
            break;
/* special BFFS packets */
#define ACTION_CREATE_OBJECT    2346
#define ACTION_GET_PERMS        2995
#define ACTION_SET_PERMS        2996
#define ACTION_SET_TIMES        2997
#define ACTION_SET_DATES        2998
        case ACTION_SET_DATE:
            res1 = action_set_date();
            break;
        case ACTION_SET_DATES:
            res1 = action_set_dates();
            break;
        case ACTION_SET_PROTECT:
            res1 = action_set_protect();
            break;
        case ACTION_SET_OWNER:
            res1 = action_set_owner();
            break;
        case ACTION_UNDISK_INFO:
            res1 = action_undisk_info();
            break;
        case ACTION_WRITE:
            res1 = action_write();
            break;
// Implement next:
//      ACTION_SET_FILE_SIZE
        case ACTION_RENAME_DISK:  // probably not
        case ACTION_WAIT_CHAR:   // ?
        case ACTION_MORE_CACHE:  // not necessary
        case ACTION_SET_COMMENT: // not necessary
        case ACTION_TIMER:       // ?
        case ACTION_INHIBIT:
        case ACTION_DISK_TYPE:    // Not needed (obsolete)
        case ACTION_DISK_CHANGE:
        case ACTION_SCREEN_MODE:  // ?
        case ACTION_READ_RETURN:  // ?
        case ACTION_WRITE_RETURN:  // ?
        case ACTION_SET_FILE_SIZE:
        case ACTION_WRITE_PROTECT:
        case ACTION_CHANGE_SIGNAL:
        case ACTION_FORMAT:
        case ACTION_FH_FROM_LOCK:
        case ACTION_CHANGE_MODE:    // convert lock to exclusive or shared
        case ACTION_COPY_DIR_FH:
        case ACTION_PARENT_FH:
        case ACTION_EXAMINE_ALL:
        case ACTION_EXAMINE_FH:
        case ACTION_LOCK_RECORD:
        case ACTION_FREE_RECORD:
        case ACTION_ADD_NOTIFY:
        case ACTION_REMOVE_NOTIFY:
        case ACTION_EXAMINE_ALL_END:
        case ACTION_SERIALIZE_DISK:
        default:
            printf("UNKNOWN %ld\n", gpack->dp_Type);
            res1 = DOSFALSE;
            gpack->dp_Res2 = ERROR_ACTION_NOT_KNOWN;
            break;
    }
    gpack->dp_Res1 = res1;
}
