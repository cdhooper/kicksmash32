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
#include <devices/input.h>
#include <devices/inputevent.h>
#include <libraries/dos.h>
#include <libraries/dosextens.h>
#include <libraries/filehandler.h>
#ifdef ROMFS
#include <libraries/expansionbase.h>
#endif
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
 * name_tolower()
 *      Lower-case a character as AmigaDOS compares names: ASCII letters and
 *      ISO-8859-1 accented letters (except the multiplication and division
 *      signs, which share the letters' bit pattern).
 */
static uint8_t
name_tolower(uint8_t ch)
{
    if (((ch >= 'A') && (ch <= 'Z')) ||
        ((ch >= 0xc0) && (ch <= 0xde) && (ch != 0xd7)))
        return (ch + 0x20);
    return (ch);
}

/*
 * bstreqv()
 *      Case-insensitive compare of a DOS node's BSTR name with a C string.
 *      The BSTR length is used, as a BSTR need not be NIL-terminated.
 */
static int
bstreqv(const char *bstr, const char *str)
{
    uint len = (uint8_t) bstr[0];
    uint pos;

    for (pos = 0; pos < len; pos++) {
        if ((str[pos] == '\0') ||
            (name_tolower(bstr[pos + 1]) != name_tolower(str[pos])))
            return (0);
    }
    return (str[len] == '\0');
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
            (tmp->dl_Name != 0) &&
            bstreqv((char *) BTOC(tmp->dl_Name), name)) {
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

/*
 * dosnode_link
 * ------------
 * Adds a node to the DOS list. The caller must hold the DosList lock.
 */
static void
dosnode_link(DosList_t *node)
{
    struct DosInfo *info = (struct DosInfo *)
                   BTOC(((struct RootNode *) DOSBase->dl_Root)->rn_Info);

    node->dol_Next = info->di_DevInfo;
    info->di_DevInfo = CTOB(node);
}

/*
 * dosnode_present
 * ---------------
 * Reports whether a node is in the DOS list. The caller must hold the
 * DosList lock.
 */
static int
dosnode_present(struct DosInfo *info, DosList_t *node)
{
    DosList_t *dl;

    for (dl = (DosList_t *) BTOC(info->di_DevInfo); dl != NULL;
         dl = (DosList_t *) BTOC(dl->dol_Next))
        if (dl == node)
            return (1);
    return (0);
}

/*
 * volnode_init
 * ------------
 * Creates a DOS device or volume node. Unless nolink is set, the node is
 * added to the DOS list, and the caller must hold the DosList lock.
 */
static struct DosList *
volnode_init(char *name, uint32_t access_time, LONG dl_type,
             struct MsgPort *msgport, void *ignore, uint nolink)
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
    if (nolink == 0)
        dosnode_link(dosnode);
    return (dosnode);
}

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

/*
 * volnode_readd
 * -------------
 * Adds the DOS nodes of a volume to the DOS list. For Workbench to see
 * the volume, it needs a DLT_VOLUME node; for Info to show it, it needs
 * a DLT_DEVICE node. So both are created, if the volume does not yet
 * have them. A volume which had previously dropped out of the DOS list
 * keeps its nodes when locks or files still reference them. Reusing the
 * nodes (rather than allocating new ones) keeps existing locks valid.
 * A node which is already in the DOS list (such as a boot node which DOS
 * added) is not added again. The caller must hold the DosList lock.
 */
static void
volnode_readd(char *name, uint32_t access_time, vollist_t *vol)
{
    struct DosInfo *info;
    DosList_t      *node;

    info = (struct DosInfo *)
           BTOC(((struct RootNode *) DOSBase->dl_Root)->rn_Info);

    if ((node = vol->vl_devnode) == NULL) {
        vol->vl_devnode = volnode_init(name, access_time, DLT_DEVICE,
                                       vol->vl_msgport, NULL, 0);
    } else {
        if (vol->vl_bootdev == 0)  // Boot node name is not from AllocVec()
            volnode_rename(info, node, name, NULL);
        node->dol_Task = vol->vl_msgport;
        if (!dosnode_present(info, node))
            dosnode_link(node);
    }

    if ((node = (DosList_t *) vol->vl_volnode) == NULL) {
        vol->vl_volnode = (DeviceList_t *)
                          volnode_init(name, access_time, DLT_VOLUME,
                                       vol->vl_msgport, vol->vl_devnode, 0);
    } else {
        volnode_rename(info, node, name, vol->vl_devnode);
        node->dol_Task = vol->vl_msgport;
        if (!dosnode_present(info, node))
            dosnode_link(node);
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

#ifdef ROMFS
/*
 * smashboot_find()
 *     Returns the boot device node which the smashboot ROM module created
 *     for the specified host volume, or NULL if there is none.
 */
static smashboot_dev_t *
smashboot_find(const char *name)
{
    struct ExpansionBase *ExpansionBase;
    struct BootNode      *bn;
    smashboot_dev_t      *sbd;
    smashboot_dev_t      *found = NULL;

    ExpansionBase = (struct ExpansionBase *)
                    OpenLibrary("expansion.library", 37);
    if (ExpansionBase == NULL)
        return (NULL);
    Forbid();
    for (bn = (struct BootNode *) ExpansionBase->MountList.lh_Head;
         bn->bn_Node.ln_Succ != NULL;
         bn = (struct BootNode *) bn->bn_Node.ln_Succ) {
        sbd = smashboot_dev(bn->bn_DeviceNode);
        if ((sbd != NULL) && (strcmp(sbd->sbd_hostname, name) == 0)) {
            found = sbd;
            break;
        }
    }
    Permit();
    CloseLibrary((struct Library *) ExpansionBase);
    return (found);
}

/*
 * volume_adopt_bootdev()
 *     Makes a boot device node the volume's device node, so that DOS
 *     packets sent to it arrive at the volume's message port.
 */
static void
volume_adopt_bootdev(vollist_t *vol, struct DeviceNode *dn)
{
    vol->vl_bootdev = 1;
    vol->vl_devnode = (DosList_t *) dn;
    dn->dn_Task     = vol->vl_msgport;
}
#endif

/*
 * volume_create()
 *     Opens a host volume and adds it to the volume list, without any
 *     DOS nodes. This does not need the DosList lock.
 */
static vollist_t *
volume_create(char *name)
{
    vollist_t *cur;
    MsgPort_t *msgport;
    handle_t   handle;
    uint       type;
    uint       rc;

    rc = sm_fopen(0, name, HM_MODE_READDIR, &type, 0, &handle);
    if (rc != 0) {
        printf("failed open of volume %s\n", name);
        return (NULL);
    }

    msgport = CreatePort(NULL, 0);
    if (msgport == NULL) {
        printf("failed to create port for volume %s\n", name);
        sm_fclose(handle);
        return (NULL);
    }

    cur = AllocMem(sizeof (vollist_t), MEMF_PUBLIC | MEMF_CLEAR);
    if (cur == NULL) {
        printf("failed to allocate volume %s\n", name);
        DeletePort(msgport);
        sm_fclose(handle);
        return (NULL);
    }
    strcpy(cur->vl_name, name);
    cur->vl_next = vollist;
    cur->vl_seen = 1;
    cur->vl_in_dos_list = 0;  // Added by volume_update()
    cur->vl_msgport = msgport;
    cur->vl_msg_mask = BIT(msgport->mp_SigBit);
    cur->vl_handle = handle;
    cur->vl_root_rewind = 1;
    vollist = cur;
    volume_msg_masks |= cur->vl_msg_mask;
    return (cur);
}

#ifdef ROMFS
/*
 * volume_boot_attach()
 *     Called when DOS has started smashfs as the handler of a boot device
 *     node. The volume is opened and the node pointed at its message port,
 *     so that the handler startup packet may be replied right away. The
 *     DOS volume node is added by the next volume_update(), which holds
 *     the DosList lock (DOS may hold it until the reply).
 */
void
volume_boot_attach(struct DeviceNode *dn)
{
    vollist_t *cur = volume_create(smashboot_dev(dn)->sbd_hostname);

    if (cur != NULL) {
        volume_adopt_bootdev(cur, dn);
        /* Locks need the volume node, so create it (unlisted) now */
        cur->vl_in_dos_list = 2;
        cur->vl_volnode = (DeviceList_t *)
                          volnode_init(cur->vl_name, 0, DLT_VOLUME,
                                       cur->vl_msgport, dn, 1);
    }
}
#endif

/*
 * volume_seen()
 *     Marks a volume as listed by the host. An unknown volume is opened
 *     and added to the volume list; its DOS nodes are added by the next
 *     volume_update(). This does not need the DosList lock.
 */
void
volume_seen(char *name, uint32_t access_time)
{
    vollist_t *cur;

    for (cur = vollist; cur != NULL; cur = cur->vl_next) {
        if (strcmp(name, cur->vl_name) == 0) {
            cur->vl_seen = 1;
            cur->vl_atime = access_time;
            return;
        }
    }

    cur = volume_create(name);
    if (cur != NULL)
        cur->vl_atime = access_time;
}

/*
 * volume_link()
 *     Adds the DOS nodes of a volume which is not in the DOS list.
 *     The caller must hold the DosList lock.
 */
static void
volume_link(vollist_t *cur)
{
    if (cur->vl_in_dos_list == 2) {
        /* Boot volume from volume_boot_attach(): DOS added the devnode */
        if (cur->vl_volnode != NULL)
            dosnode_link((DosList_t *) cur->vl_volnode);
    } else if (cur->vl_in_dos_list == 0) {
#ifdef ROMFS
        if ((cur->vl_bootdev == 0) && (cur->vl_devnode == NULL)) {
            /*
             * The volume has a boot node, which DOS normally added to the
             * DOS list at boot (but which was taken out if the volume was
             * flushed since). volnode_readd() adds it if it's missing.
             */
            smashboot_dev_t *sbd = smashboot_find(cur->vl_name);
            if (sbd != NULL)
                volume_adopt_bootdev(cur, &sbd->sbd_dn);
        }
#endif
        volnode_readd(cur->vl_name, cur->vl_atime, cur);
    }
    cur->vl_in_dos_list = 1;
}

static struct MsgPort  *event_port;  // input.device, for disk change events
static struct IOStdReq *event_req;

/*
 * volume_events_open()
 *     Opens input.device, so that volume_update() can report volumes
 *     added to and removed from the DOS list as disk change events.
 *     Without it, smashfs works as before, just without those events.
 */
void
volume_events_open(void)
{
    event_port = CreatePort(NULL, 0);
    if (event_port != NULL) {
        event_req = (struct IOStdReq *)
                    CreateExtIO(event_port, sizeof (*event_req));
        if ((event_req != NULL) &&
            (OpenDevice("input.device", 0,
                        (struct IORequest *) event_req, 0) == 0))
            return;
    }
    printf("smashfs: unable to open input.device\n");
    volume_events_close();
}

/*
 * volume_events_close()
 *     Closes what volume_events_open() opened.
 */
void
volume_events_close(void)
{
    if (event_req != NULL) {
        if (event_req->io_Device != NULL)
            CloseDevice((struct IORequest *) event_req);
        DeleteExtIO((struct IORequest *) event_req);
        event_req = NULL;
    }
    if (event_port != NULL) {
        DeletePort(event_port);
        event_port = NULL;
    }
}

/*
 * volume_disk_event()
 *     Sends a disk inserted or removed event (IECLASS_DISKINSERTED or
 *     IECLASS_DISKREMOVED) to the input stream, as a filesystem does
 *     when its media changes. A "Please insert volume" requester retries
 *     when it gets one, and Workbench and file requesters rescan their
 *     volumes. Must not be called while holding the DosList lock, as
 *     those may then read the DOS list.
 */
static void
volume_disk_event(UBYTE class)
{
    struct InputEvent ie;

    if (event_req == NULL)
        return;
    memset(&ie, 0, sizeof (ie));
    ie.ie_Class = class;
    event_req->io_Command = IND_WRITEEVENT;
    event_req->io_Data    = &ie;
    event_req->io_Length  = sizeof (ie);
    DoIO((struct IORequest *) event_req);
}

/*
 * volume_free()
 *     Closes the host handles of volumes which were removed from the
 *     volume list, and frees them. This does not need the DosList lock.
 */
static void
volume_free(vollist_t *cur)
{
    vollist_t    *next;
    DeviceList_t *dlnode;

    for (; cur != NULL; cur = next) {
        next = cur->vl_next;
        if (sm_fservice()) {
            sm_fclose(cur->vl_handle);
            if (cur->vl_root_scan != 0)
                sm_fclose(cur->vl_root_scan);
        }
        volume_msg_masks &= ~cur->vl_msg_mask;
        Forbid();
            /* No new packet can arrive between these two steps */
            volume_port_reject(cur->vl_msgport);
            DeletePort(cur->vl_msgport);
        Permit();

        if ((dlnode = cur->vl_volnode) != NULL) {
            FreeVec(BTOC(dlnode->dl_Name));
            FreeMem(dlnode, sizeof (*dlnode));
        }
        if (((dlnode = (DeviceList_t *) cur->vl_devnode) != NULL) &&
            (cur->vl_bootdev == 0)) {
            FreeVec(BTOC(dlnode->dl_Name));
            FreeMem(dlnode, sizeof (*dlnode));
        }
        FreeMem(cur, sizeof (vollist_t));
    }
}

/*
 * volume_update()
 *     Brings the DOS list up to date with the volumes the host listed
 *     (see volume_seen()) since the last update: their DOS nodes are
 *     added, and the DOS nodes of the other volumes are removed. Volumes
 *     which are no longer listed and not in use are freed.
 *
 *     The DosList lock is held only while the DOS list is changed, and
 *     not across any host message. On Kickstart 3.2, holding that lock is
 *     a Forbid(), so no other task could run while waiting for the host.
 *
 *     Returns -1 without making changes if the DosList lock is not
 *     available (handlers must not wait for it).
 */
int
volume_update(void)
{
    vollist_t *cur;
    vollist_t *next;
    vollist_t *prev = NULL;
    vollist_t *gone = NULL;
    uint8_t    added = 0;
    uint8_t    removed = 0;

    if (AttemptLockDosList(LDF_DEVICES | LDF_VOLUMES | LDF_WRITE) == NULL)
        return (-1);

    gvolumes_inuse = 0;
    for (cur = vollist; cur != NULL; cur = next) {
        next = cur->vl_next;
        if (cur->vl_seen) {
            if (cur->vl_in_dos_list != 1) {
                volume_link(cur);
                added = 1;
            }
            cur->vl_seen = 0;
        } else {
            printf("Flushing %s\n", cur->vl_name);
            if (cur->vl_in_dos_list) {
                cur->vl_in_dos_list = 0;
                volnode_remove(cur);
                removed = 1;
            }
            if (cur->vl_use_count == 0) {
                /* Okay to finish volume remove, after the unlock */
                if (prev == NULL)
                    vollist = next;
                else
                    prev->vl_next = next;
                cur->vl_next = gone;
                gone = cur;
                continue;
            }
            printf("%s use count still %u\n",
                   cur->vl_name, cur->vl_use_count);
        }
        gvolumes_inuse++;
        prev = cur;
    }
    UnLockDosList(LDF_DEVICES | LDF_VOLUMES | LDF_WRITE);

    volume_free(gone);
    if (removed)
        volume_disk_event(IECLASS_DISKREMOVED);
    if (added)
        volume_disk_event(IECLASS_DISKINSERTED);
    return (0);
}

/*
 * volume_port_reject()
 *     Replies to every packet still queued on a volume's message port with
 *     ERROR_DEVICE_NOT_MOUNTED. A process which found the volume's DOS
 *     node just before it was removed may have queued a packet; without
 *     a reply, that process would wait forever.
 */
void
volume_port_reject(struct MsgPort *mp)
{
    struct StandardPacket *sp;
    struct DosPacket      *pkt;

    while ((sp = (struct StandardPacket *) GetMsg(mp)) != NULL) {
        pkt = (struct DosPacket *) (sp->sp_Msg.mn_Node.ln_Name);
        ReplyPkt(pkt, packet_fail_res1(pkt->dp_Type),
                 ERROR_DEVICE_NOT_MOUNTED);
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
        /* The root scan handle is also stale; reopen it when next used */
        cur->vl_root_scan = 0;
        cur->vl_root_rewind = 1;
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
        if (sm_fservice()) {
            sm_fclose(cur->vl_handle);
            if (cur->vl_root_scan != 0)
                sm_fclose(cur->vl_root_scan);
        }

        mp = cur->vl_msgport;
        Forbid();
            /* No new message can arrive between these two steps */
            mp->mp_Flags   = PA_IGNORE;
            mp->mp_SigTask = NULL;
            while ((sp = (struct StandardPacket *) GetMsg(mp)) != NULL) {
                pkt = (struct DosPacket *) (sp->sp_Msg.mn_Node.ln_Name);
                ReplyPkt(pkt, packet_fail_res1(pkt->dp_Type),
                         ERROR_DEVICE_NOT_MOUNTED);
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
