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

#include <inline/dos.h>
#include <inline/exec.h>

#include <devices/timer.h>
#include <libraries/dos.h>
#include <libraries/dosextens.h>
#include <libraries/filehandler.h>
#include "printf.h"
#include "fs_hand.h"
#include "fs_vol.h"
#include "fs_packet.h"
#include "sm_file.h"

#define ID_SMASHFS_DISK (0x536d4653L)  /* 'SmFS' filesystem */

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;

static vollist_t *vollist = NULL;

vollist_t *gvol;  // current volume being handled
ULONG      volume_msg_masks = 0;

/*
 * streqv()
 *      Perform a case-insensitive compare of filenames.
 */
static int
streqv(const char *str1, const char *str2)
{
#define NO_STRNICMP
#ifdef NO_STRNICMP
    while (*str1 != '\0') {
        if ((*str1 != *str2) && ((*str1 | 32) != (*str2 | 32)))
            return (0);
        str1++;
        str2++;
    }

    if (*str2 == '\0')
        return (1);
    else
        return (0);
#else
    return (strnicmp(str1, str2, DIRNAMELEN) == 0);
#endif
}

/*
 * unix_time_to_amiga_datestamp
 * ----------------------------
 * Convert UNIX seconds since 1970 to Amiga DateStamp format
 */
void
unix_time_to_amiga_datestamp(uint sec, uint nsec, struct DateStamp *ds)
{
#define AMIGA_SEC_TO_UNIX_SEC (2922 * 24 * 60 * 60)  // 1978 - 1970 = 2922 days
    if (sec >= AMIGA_SEC_TO_UNIX_SEC)
        sec -= AMIGA_SEC_TO_UNIX_SEC;

    ds->ds_Days   = sec / 86400;
    ds->ds_Minute = (sec % 86400) / 60;
    ds->ds_Tick   = (sec % 60) * TICKS_PER_SECOND;
    ds->ds_Tick  += nsec / (1000000000 / TICKS_PER_SECOND);
}

/*
 * amiga_datestamp_to_unix_time
 * ----------------------------
 * Convert Amiga DateStamp to seconds since 1978
 */
uint
amiga_datestamp_to_unix_time(struct DateStamp *ds, uint *nsec)
{
    uint secs = ds->ds_Days * 24 * 3600 +       // Days
                ds->ds_Minute * 60 +            // Minutes
                ds->ds_Tick / TICKS_PER_SECOND; // Seconds
    secs += AMIGA_SEC_TO_UNIX_SEC;
    *nsec = (ds->ds_Tick % TICKS_PER_SECOND) * 1000000000 / TICKS_PER_SECOND;
    return (secs);
}

/*
 * name_present_in_dos_devinfo
 * ---------------------------
 * Returns non-zero if the specified device is present in the
 * DosInfo structure. Note that a lock must be held on the device
 * info before calling this function.
 */
static int
name_present_in_dos_devinfo(struct DosInfo *info, const char *name,
                            void *ignore)
{
    struct DeviceList *tmp;
    uint present = 0;

    // LockDosList(LDF_DEVICES | LDF_VOLUMES | LDF_READ);
    for (tmp = (struct DeviceList *) BTOC(info->di_DevInfo);
         tmp != NULL;
         tmp = (struct DeviceList *) BTOC(tmp->dl_Next)) {
        if ((tmp != ignore) &&
            (streqv(((char *) BTOC(tmp->dl_Name)) + 1, name))) {
            present = 1;
            break;
        }
    }
    // UnLockDosList(LDF_DEVICES | LDF_VOLUMES | LDF_READ);
    return (present);
}

/*
 * fsname
 * ------
 * Converts name (modified in place) into a valid DOS name which is not
 * already present in the DOS list. The name buffer must have room for
 * VOLNAME_MAXLEN + 12 bytes (a ".<count>" suffix may be appended).
 */
static void
fsname(struct DosInfo *info, char *name, void *ignore)
{
    int len;
    char *ptr;

    while (*name == '/')
        name++;
    for (ptr = name; *ptr != '\0'; ptr++) {
        switch (*ptr) {
            case ':':
                if (ptr[1] == '\0') {
                    *ptr = '\0';
                    break;
                } else {
                    *ptr = '_';
                }
                break;
            case ' ':
                *ptr = '_';
                break;
        }
    }
//  printf("VOLNAME=%s\n", name);

    len = strlen(name);
    if (name_present_in_dos_devinfo(info, name, ignore)) {
        uint count = 0;
        name[len++] = '.';
        do {
            sprintf(name + len, "%u", count++);
        } while (name_present_in_dos_devinfo(info, name, ignore));
    }
}

/* BSTR handler name; BPTRs require longword alignment */
static const char smashfs_handler_bstr[] __attribute__((aligned(4))) =
    "\7smashfs";

static struct DosList *
volnode_init(char *name, uint32_t access_time, LONG dl_type,
             struct MsgPort *msgport, void *ignore)
{
    uint namelen;
    char *volumename;
    char  dosname[VOLNAME_MAXLEN + 12];
    struct DosInfo *info;
    struct DosList *dosnode;

    info = (struct DosInfo *)
           BTOC(((struct RootNode *) DOSBase->dl_Root)->rn_Info);

    /*
     * Work on a private copy: the caller's name may point into a
     * received message buffer, which must not be modified.
     *
     * Lock of volume list was done by the caller (see below).
     */
    strncpy(dosname, name, VOLNAME_MAXLEN);
    dosname[VOLNAME_MAXLEN] = '\0';
    fsname(info, dosname, ignore);
    namelen = strlen(dosname);

    dosnode = (struct DosList *) AllocMem(sizeof (*dosnode),
                                          MEMF_PUBLIC | MEMF_CLEAR);

    if (dosnode == NULL) {
        printf("volnode_init: unable to allocate %d bytes\n",
               sizeof (*dosnode));
        return (NULL);
    }

    volumename = (char *) AllocVec(namelen + 2, MEMF_PUBLIC);
    if (volumename == NULL) {
        printf("volnode_init: unable to allocate %u bytes\n", namelen);
        FreeMem(dosnode, sizeof (*dosnode));
        return (NULL);
    }

    if (dl_type == DLT_DEVICE) {
        dosnode->dol_misc.dol_handler.dol_Handler =
            CTOB(smashfs_handler_bstr);
        dosnode->dol_misc.dol_handler.dol_StackSize = 4096;
        dosnode->dol_misc.dol_handler.dol_Priority = 0;
        dosnode->dol_misc.dol_handler.dol_Startup = 0;  // FileSysStartupMsg
        dosnode->dol_misc.dol_handler.dol_SegList = 0;
        dosnode->dol_misc.dol_handler.dol_GlobVec = -1;
    } else if (dl_type == DLT_VOLUME) {
        /* Probably only useful if booting from this volume and no RTC */
        unix_time_to_amiga_datestamp(access_time, 0,
            &dosnode->dol_misc.dol_volume.dol_VolumeDate);
        dosnode->dol_misc.dol_volume.dol_LockList = 0;
        dosnode->dol_misc.dol_volume.dol_DiskType = ID_SMASHFS_DISK;
        /*
         * Randell Jesup said (22-Dec-1991) that if we want Workbench to
         * recognize BFFS, it might need to fake ID_DOS_DISK in response
         * to ACTION_INFO. It needs to be faked for ACTION_DISK_INFO,
         * but not here.
         */
    }

    dosnode->dol_Type     = dl_type;
    dosnode->dol_Task     = msgport;
    dosnode->dol_Lock     = 0;

    volumename[0] = namelen;
    strcpy(volumename + 1, dosname);
    dosnode->dol_Name     = CTOB(volumename);

    /*
     * Lock of volume list was done by the caller.
     *
     * According to the AmigaDOS RKM, handlers should AttemptLockDosList()
     * and if that fails, should continue to handle packets until the list
     * can be locked. Since that would unnecessarily complicate this code,
     * it's up to the caller to do the AttemptLockDosList().
     */
    // LockDosList(LDF_DEVICES | LDF_VOLUMES | LDF_WRITE);
        dosnode->dol_Next = info->di_DevInfo;
        info->di_DevInfo = (BPTR) CTOB(dosnode);
    // UnLockDosList(LDF_DEVICES | LDF_VOLUMES | LDF_WRITE);
    return (dosnode);
}

static void
volnode_new(char *name, uint32_t access_time, vollist_t *vol)
{
    /*
     * For Workbench to see it, we need DLT_VOLUME
     * For Info to show it, we need DLT_DEVICE
     *
     * So create both a device and a volume.
     */
    vol->vl_devnode = volnode_init(name, access_time, DLT_DEVICE,
                                   vol->vl_msgport, NULL);
    vol->vl_volnode = (DeviceList_t *)
                      volnode_init(name, access_time, DLT_VOLUME,
                                   vol->vl_msgport, vol->vl_devnode);
}

/*
 * volnode_readd
 * -------------
 * Puts back the DOS nodes of a volume which had previously dropped out of
 * the DOS list, but whose nodes were kept because locks or files still
 * reference them. Reusing the nodes (rather than allocating new ones)
 * keeps existing locks valid. The caller must hold the DosList lock.
 */
/*
 * volnode_rename
 * --------------
 * Gives a node which is not currently in the DOS list a name which does
 * not collide with any node in the list (another volume may have taken
 * its name while it was out of the list). The caller must hold the
 * DosList lock.
 */
static void
volnode_rename(struct DosInfo *info, DosList_t *node, const char *name,
               void *ignore)
{
    char  dosname[VOLNAME_MAXLEN + 12];
    char *oldname = (char *) BTOC(node->dol_Name);
    char *newname;
    uint  namelen;

    strncpy(dosname, name, VOLNAME_MAXLEN);
    dosname[VOLNAME_MAXLEN] = '\0';
    fsname(info, dosname, ignore);
    namelen = strlen(dosname);

    if ((oldname != NULL) && ((uint8_t) oldname[0] == namelen) &&
        (strncmp(oldname + 1, dosname, namelen) == 0))
        return;  // Unchanged

    newname = (char *) AllocVec(namelen + 2, MEMF_PUBLIC);
    if (newname == NULL)
        return;  // Keep the old name
    newname[0] = namelen;
    strcpy(newname + 1, dosname);
    node->dol_Name = CTOB(newname);
    if (oldname != NULL)
        FreeVec(oldname);
    printf("Volume renamed %s\n", dosname);
}

static void
volnode_readd(char *name, uint32_t access_time, vollist_t *vol)
{
    struct DosInfo *info;
    DosList_t      *node;

    info = (struct DosInfo *)
           BTOC(((struct RootNode *) DOSBase->dl_Root)->rn_Info);

    if ((node = vol->vl_devnode) == NULL) {
        vol->vl_devnode = volnode_init(name, access_time, DLT_DEVICE,
                                       vol->vl_msgport, NULL);
    } else {
        volnode_rename(info, node, name, NULL);
        node->dol_Task = vol->vl_msgport;
        node->dol_Next = info->di_DevInfo;
        info->di_DevInfo = CTOB(node);
    }

    if ((node = (DosList_t *) vol->vl_volnode) == NULL) {
        vol->vl_volnode = (DeviceList_t *)
                          volnode_init(name, access_time, DLT_VOLUME,
                                       vol->vl_msgport, vol->vl_devnode);
    } else {
        volnode_rename(info, node, name, vol->vl_devnode);
        node->dol_Task = vol->vl_msgport;
        node->dol_Next = info->di_DevInfo;
        info->di_DevInfo = CTOB(node);
    }
}

static void
volnode_remove(vollist_t *vol)
{
    int             removed = 0;
    DeviceList_t   *volnode = vol->vl_volnode;
    DosList_t      *devnode = vol->vl_devnode;
    DeviceList_t   *current;
    DeviceList_t   *parent;
    struct DosInfo *info;

    if ((volnode == NULL) && (devnode == NULL)) {
        printf("volnode already removed\n");
        return;
    }

    info   = (struct DosInfo *)
             BTOC(((struct RootNode *) DOSBase->dl_Root)->rn_Info);
    parent = NULL;

    /*
     * Lock of volume list was done by the caller.
     *
     * According to the AmigaDOS RKM, handlers should AttemptLockDosList()
     * and if that fails, should continue to handle packets until the list
     * can be locked. Since that would unnecessarily complicate this code,
     * it's up to the caller to do the AttemptLockDosList().
     */
    // LockDosList(LDF_DEVICES | LDF_VOLUMES | LDF_WRITE);
        current = (struct DeviceList *) BTOC(info->di_DevInfo);
        while (current != NULL) {
            if ((current == volnode) || (current == (void *) devnode)) {
                removed++;
                current->dl_Task = NULL;
                if (parent == NULL)
                    info->di_DevInfo = current->dl_Next;
                else
                    parent->dl_Next  = current->dl_Next;
                current = (struct DeviceList *) BTOC(current->dl_Next);
            } else {
                parent = current;
                current = (struct DeviceList *) BTOC(current->dl_Next);
            }
        }
    // UnLockDosList(LDF_DEVICES | LDF_VOLUMES | LDF_WRITE);

    if (removed == 0)
        printf("Unable to find volnode to remove\n");
}

/*
 * volume_seen()
 *     Mark volume as seen. Unknown volumes are immediately added to the
 *     DOS volume list.
 */
void
volume_seen(char *name, uint32_t access_time, uint flags, int bootpri)
{
    vollist_t *cur;
    MsgPort_t *msgport;
    handle_t phandle = 0;
    handle_t handle;
    uint     type;
    uint     rc;

    for (cur = vollist; cur != NULL; cur = cur->vl_next) {
        if (strcmp(name, cur->vl_name) == 0) {
            cur->vl_seen++;
            if (cur->vl_in_dos_list == 0) {
                /* Previously dropped out of volume list */
                cur->vl_in_dos_list = 1;
                volnode_readd(name, access_time, cur);
            }
            return;
        }
    }

    rc = sm_fopen(phandle, name, HM_MODE_READDIR, &type, 0, &handle);
    if (rc != 0) {
        printf("failed open of volume %s\n", name);
        return;
    }

    msgport = CreatePort(NULL, 0);
    if (msgport == NULL) {
        printf("failed to create port for volume %s\n", name);
        sm_fclose(handle);
        return;
    }

    cur = AllocMem(sizeof (vollist_t), MEMF_PUBLIC);
    if (cur == NULL) {
        printf("failed to allocate volume %s\n", name);
        DeletePort(msgport);
        sm_fclose(handle);
        return;
    }
    strcpy(cur->vl_name, name);
    cur->vl_next = vollist;
    cur->vl_seen = 1;
    cur->vl_in_dos_list = 1;
    cur->vl_use_count = 0;
    cur->vl_msgport = msgport;
    cur->vl_msg_mask = BIT(msgport->mp_SigBit);
    cur->vl_handle = handle;
    cur->vl_flags = flags;
    cur->vl_bootpri = bootpri;
    volnode_new(name, access_time, cur);
    vollist = cur;

#if 0
    if (flags & AV_FLAG_BOOTABLE) {
        /* Create boot node */
        AddBootNode(bootpri, ADNF_STARTPROC, dn, md->configDev);
    }
#endif

    volume_msg_masks |= cur->vl_msg_mask;
}
#define AV_FLAG_BOOTABLE 0x01

/*
 * volume_port_reject()
 *     Replies to every packet still queued on a volume's message port with
 *     ERROR_DEVICE_NOT_MOUNTED. A process which found the volume's DOS
 *     node just before it was removed may have queued a packet; without
 *     a reply, that process would wait forever.
 */
static void
volume_port_reject(struct MsgPort *mp)
{
    struct StandardPacket *sp;
    struct DosPacket      *pkt;

    while ((sp = (struct StandardPacket *) GetMsg(mp)) != NULL) {
        pkt = (struct DosPacket *) (sp->sp_Msg.mn_Node.ln_Name);
        ReplyPkt(pkt, DOSFALSE, ERROR_DEVICE_NOT_MOUNTED);
    }
}

/*
 * volume_flush()
 *     Removes DOS nodes for any volumes which are no longer present.
 */
void
volume_flush(void)
{
    vollist_t *cur  = vollist;
    vollist_t *prev = NULL;
    gvolumes_inuse = 0;
    while (cur != NULL) {
        if (cur->vl_seen == 0) {
            printf("Flushing %s\n", cur->vl_name);
            if (cur->vl_in_dos_list) {
                cur->vl_in_dos_list = 0;
                volnode_remove(cur);
            }
            if (cur->vl_use_count == 0) {
                /* Okay to finish volume remove */
                DeviceList_t *dlnode;
                vollist_t    *next = cur->vl_next;
                if (prev == NULL)
                    vollist = next;
                else
                    prev->vl_next = next;
                if (sm_fservice())
                    sm_fclose(cur->vl_handle);
                volume_msg_masks &= ~BIT(cur->vl_msgport->mp_SigBit);
                Forbid();
                    /* No new packet can arrive between these two steps */
                    volume_port_reject(cur->vl_msgport);
                    DeletePort(cur->vl_msgport);
                Permit();

                if ((dlnode = cur->vl_volnode) != NULL) {
                    FreeVec(BTOC(dlnode->dl_Name));
                    FreeMem(dlnode, sizeof (*dlnode));
                }
                if ((dlnode = (DeviceList_t *) cur->vl_devnode) != NULL) {
                    FreeVec(BTOC(dlnode->dl_Name));
                    FreeMem(dlnode, sizeof (*dlnode));
                }
                FreeMem(cur, sizeof (vollist_t));
                cur = next;
                continue;
            } else {
                printf("%s use count still %u\n",
                       cur->vl_name, cur->vl_use_count);
                gvolumes_inuse++;
            }
        } else {
            gvolumes_inuse++;
        }
        cur->vl_seen = 0;
        prev = cur;
        cur = cur->vl_next;
    }
}

/*
 * volume_reopen_handles()
 *     Called when hostsmash has restarted: every volume's host handle is
 *     from the previous host and is no longer valid, so open it again.
 *     A volume which can't be opened keeps its stale handle; operations
 *     using it fail, and the volume is flushed if the host no longer
 *     lists it.
 */
void
volume_reopen_handles(void)
{
    vollist_t *cur;
    handle_t   handle;
    uint       type;
    uint       rc;

    for (cur = vollist; cur != NULL; cur = cur->vl_next) {
        rc = sm_fopen(0, cur->vl_name, HM_MODE_READDIR, &type, 0, &handle);
        if (rc == 0) {
            printf("Reopened %s: handle %x -> %x\n",
                   cur->vl_name, cur->vl_handle, handle);
            cur->vl_handle = handle;
        } else {
            printf("Reopen of %s failed: %d\n", cur->vl_name, rc);
        }
    }
}

/*
 * volume_abandon()
 *     Called at exit, with the DosList locked, for volumes which still have
 *     outstanding locks or open files after the shutdown grace period.
 *     Those locks and files point at the volume's DOS nodes and message
 *     port, so those must never be freed. Each remaining volume is taken
 *     out of the DOS list, its queued packets are rejected, and its port
 *     is set to ignore further messages: a later packet (for example the
 *     UnLock() of a stale lock) is then queued and never answered, instead
 *     of signalling a task which no longer exists. The sending process
 *     will wait forever, but the system will not crash.
 *
 *     The port, DOS nodes, and locks are deliberately leaked.
 */
void
volume_abandon(void)
{
    vollist_t             *cur;
    vollist_t             *next;
    struct MsgPort        *mp;
    struct StandardPacket *sp;
    struct DosPacket      *pkt;

    for (cur = vollist; cur != NULL; cur = next) {
        next = cur->vl_next;
        printf("Abandoning %s with use count %u\n",
               cur->vl_name, cur->vl_use_count);
        if (cur->vl_in_dos_list) {
            cur->vl_in_dos_list = 0;
            volnode_remove(cur);
        }
        if (sm_fservice())
            sm_fclose(cur->vl_handle);

        mp = cur->vl_msgport;
        Forbid();
            /* No new message can arrive between these two steps */
            mp->mp_Flags   = PA_IGNORE;
            mp->mp_SigTask = NULL;
            while ((sp = (struct StandardPacket *) GetMsg(mp)) != NULL) {
                pkt = (struct DosPacket *) (sp->sp_Msg.mn_Node.ln_Name);
                ReplyPkt(pkt, DOSFALSE, ERROR_DEVICE_NOT_MOUNTED);
            }
        Permit();
        /* The port's signal bit is released when this task exits */
        volume_msg_masks &= ~cur->vl_msg_mask;
        FreeMem(cur, sizeof (vollist_t));
    }
    vollist = NULL;
    gvolumes_inuse = 0;
}

static void
reply_packet(void)
{
#undef PACKET_MSG_DEBUG
#ifdef PACKET_MSG_DEBUG
    struct MsgPort *reply_port = gpack->dp_Port;
#endif

    ReplyPkt(gpack, gpack->dp_Res1, gpack->dp_Res2);

#ifdef PACKET_MSG_DEBUG
    printf("<- Put msg (");
    if ((gpack->dp_Res1 > 0xff000000) || (gpack->dp_Res1 < 1000)) {
        printf("%d", gpack->dp_Res1);
    } else {
        printf("0x%x", gpack->dp_Res1);
    }
    printf(") to port %p\n", reply_port);
    (void) reply_port;
#endif
}

void
volume_message(uint mask)
{
    vollist_t             *cur;
    struct StandardPacket *sp;

    for (cur = vollist; cur != NULL; cur = cur->vl_next) {
        if (mask & cur->vl_msg_mask) {
            struct MsgPort *mp = cur->vl_msgport;
            gvol = cur;
            while ((sp = (struct StandardPacket *) GetMsg(mp)) != NULL) {
                gpack = (struct DosPacket *) (sp->sp_Msg.mn_Node.ln_Name);
                handle_packet();
                reply_packet();
#ifdef PACKET_MSG_DEBUG
                printf("-> Get msg (%u) for %s: on port %p\n",
                       gpack->dp_Type, cur->vl_name, mp);
#else
                printf("\n");
#endif
            }
        }
    }
}
