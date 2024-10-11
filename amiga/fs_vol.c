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
#include "fs_hand.h"
#include "fs_vol.h"
#include "fs_packet.h"
#include "sm_file.h"

#define ID_SMASHFS_DISK (0x536d4653L)  /* 'SmFS' filesystem */

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
unix_time_to_amiga_datestamp(uint sec, struct DateStamp *ds)
{
#define UNIX_SEC_TO_AMIGA_SEC (2922 * 24 * 60 * 60)  // 1978 - 1970 = 2922 days
    if (sec >= UNIX_SEC_TO_AMIGA_SEC)
        sec -= UNIX_SEC_TO_AMIGA_SEC;

    ds->ds_Days   = sec / 86400;
    ds->ds_Minute = (sec % 86400) / 60;
    ds->ds_Tick   = (sec % 60) * TICKS_PER_SECOND;
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
                            struct DeviceList *ignore)
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

static void
fsname(struct DosInfo *info, char *name, char *volumename, DeviceList_t *ignore)
{
    int len;
    char *ptr;

    while ((*name == '/') || (*name == '/'))
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
    len += strlen(name + len);

    strcpy(volumename + 1, name);
    volumename[0] = len;
}

static struct DeviceList *
volnode_init(char *name, uint32_t access_time, LONG dl_type,
             struct MsgPort *msgport, struct DeviceList *ignore)
{
    uint namelen = strlen(name) + 5;
    struct DeviceList *volnode;
    char *volumename;
    struct DosInfo *info;

    info = (struct DosInfo *)
           BTOC(((struct RootNode *) DOSBase->dl_Root)->rn_Info);

    volnode = (struct DeviceList *)
              AllocMem(sizeof (struct DeviceList), MEMF_PUBLIC);

    if (volnode == NULL) {
        printf("volnode_init: unable to allocate %d bytes\n",
               sizeof (struct DeviceList));
        return (NULL);
    }

    volumename = (char *) AllocVec(namelen, MEMF_PUBLIC);
    if (volumename == NULL) {
        printf("volnode_init: unable to allocate %u bytes\n", namelen);
        FreeMem(volnode, sizeof (struct DeviceList));
        return (NULL);
    }

    /* Probably only useful if booting from this volume and RTC unavailable */
    unix_time_to_amiga_datestamp(access_time, &volnode->dl_VolumeDate);

    volnode->dl_Type     = dl_type;
    volnode->dl_Task     = msgport;
    volnode->dl_Lock     = 0;
    volnode->dl_LockList = 0;

    /*
     * Randell Jesup said (22-Dec-1991) that if we want Workbench to recognize
     * BFFS, it might need to fake ID_DOS_DISK in response to ACTION_INFO.  It
     * needs to be faked for ACTION_DISK_INFO, not here.
     */
    volnode->dl_DiskType = ID_SMASHFS_DISK;
    volnode->dl_unused   = 0;
    volnode->dl_Name     = CTOB(volumename);

    /*
     * Lock of volume list was done by the caller.
     *
     * According to the AmigaDOS RKM, handlers should AttemptLockDosList()
     * and if that fails, should continue to handle packets until the list
     * can be locked. Since that would unnecessarily complicate this code,
     * it's up to the caller to do the AttemptLockDosList().
     */
    // LockDosList(LDF_DEVICES | LDF_VOLUMES | LDF_WRITE);
        fsname(info, name, volumename, ignore);
        volnode->dl_Next = info->di_DevInfo;
        info->di_DevInfo = (BPTR) CTOB(volnode);
    // UnLockDosList(LDF_DEVICES | LDF_VOLUMES | LDF_WRITE);
    return (volnode);
}

static void
volnode_new(char *name, uint32_t access_time, vollist_t *vol)
{
// XXX: For WB to see it, we need DLT_VOLUME
//      For info to show it, we need DLT_DEVICE

    vol->vl_devnode = volnode_init(name, access_time, DLT_DEVICE,
                                   vol->vl_msgport, NULL);
    vol->vl_volnode = volnode_init(name, access_time, DLT_VOLUME,
                                   vol->vl_msgport, vol->vl_devnode);
}

static void
volnode_remove(vollist_t *vol)
{
    int             removed = 0;
    DeviceList_t   *volnode = vol->vl_volnode;
    DeviceList_t   *devnode = vol->vl_devnode;
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
            if ((current == volnode) || (current == devnode)) {
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
                volnode_new(name, access_time, cur);
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

    cur = AllocMem(sizeof (vollist_t), MEMF_PUBLIC);
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
                sm_fclose(cur->vl_handle);
                volume_msg_masks &= ~BIT(cur->vl_msgport->mp_SigBit);
                DeletePort(cur->vl_msgport);

                if ((dlnode = cur->vl_volnode) != NULL) {
                    FreeVec(BTOC(dlnode->dl_Name));
                    FreeMem(dlnode, sizeof (*dlnode));
                }
                if ((dlnode = cur->vl_devnode) != NULL) {
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
 * volume_close()
 *     Closes unclosed handles on all volumes
 */
void
volume_close(void)
{
    vollist_t *cur  = vollist;
    for (cur = vollist; cur != NULL; cur = cur->vl_next) {
        if (cur->vl_use_count != 0) {
            // XXX: Need to close handles for all system locks
            //      This information is not currently tracked
            //
            // Maybe walk the system lock list and compare lock->fl_Volume
            // against cur->vl_volnode.
            // Need to get file handle from locks
            // lock->fl_Handle gives KS handle to close, but there
            // might also be AmigaOS File Handles out there. I don't
            // know how those work yet because they don't seem to be
            // closed as expected.
            cur->vl_use_count = 0;
        }
    }
}

static void
reply_packet(struct MsgPort *mp)
{
#undef PACKET_MSG_DEBUG
#ifdef PACKET_MSG_DEBUG
    struct MsgPort *reply_port = gpack->dp_Port;
#else
    (void) mp;
#endif

#if 0
    // XXX: Does not seem to always wake caller
    gpack->dp_Port = mp;
    PutMsg(reply_port, gpack->dp_Link);
#else
    ReplyPkt(gpack, gpack->dp_Res1, gpack->dp_Res2);
#endif

#ifdef PACKET_MSG_DEBUG
    printf("<- Put msg (");
    if ((gpack->dp_Res1 > 0xff000000) || (gpack->dp_Res1 < 1000)) {
        printf("%d", gpack->dp_Res1);
    } else {
        printf("0x%x", gpack->dp_Res1);
    }
    printf(") to port %p from my port %p\n", reply_port, mp);
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
                reply_packet(mp);
#ifdef PACKET_MSG_DEBUG
                printf("-> Get msg (%u) for %s: on port %p\n",
                       gpack->dp_Type, cur->vl_volname, mp);
#else
                printf("\n");
#endif
            }
        }
    }
}
