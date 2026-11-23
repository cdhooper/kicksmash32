/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2024.
 *
 * ---------------------------------------------------------------------
 *
 * Filesystem Amiga Volume handling code.
 */

#ifndef _FS_VOL_H
#define _FS_VOL_H

#include "smash_cmd.h"
#include "host_cmd.h"

#define VOLNAME_MAXLEN 32

typedef struct DeviceList DeviceList_t;
typedef struct DosList DosList_t;
typedef struct MsgPort MsgPort_t;

typedef struct vollist vollist_t;
typedef struct vollist {
    char          vl_name[VOLNAME_MAXLEN + 1];
    vollist_t    *vl_next;
    uint8_t       vl_seen;         // Listed by the host since last update
    uint8_t       vl_in_dos_list;  // 0=no, 1=yes, 2=boot devnode only
    uint32_t      vl_atime;        // Host access time (volume node date)
    uint8_t       vl_bootdev;      // vl_devnode is a smashboot_dev_t
    uint          vl_use_count;
    ULONG         vl_msg_mask;
    handle_t      vl_handle;
    handle_t      vl_root_scan;    // Root dir scan for NULL lock (0=none)
    uint8_t       vl_root_rewind;  // vl_root_scan must rewind before read
    DosList_t    *vl_devnode;
    DeviceList_t *vl_volnode;
    MsgPort_t    *vl_msgport;
} vollist_t;

extern vollist_t *gvol;  // current volume being handled

#ifdef ROMFS
/*
 * Device node of a bootable volume, created by the smashboot ROM module
 * before DOS starts, and placed in the expansion MountList with
 * AddBootNode(). Every one shares a handler stub which starts smashfs
 * (rom_handler_entry()). Boot device nodes are never freed, as the
 * BootNode in the MountList points at them.
 */
#define ID_SMASHBOOT_DOSTYPE 0x534d5348  // 'SMSH' (shown by the boot menu)

typedef struct {
    struct DeviceNode sbd_dn;                        // Must be first
    struct FileSysStartupMsg sbd_fssm;               // dn_Startup
    ULONG             sbd_env[DE_DOSTYPE + 1];       // fssm_Environ
    char              sbd_bname[VOLNAME_MAXLEN + 2]; // BSTR DOS name
    char              sbd_hostname[VOLNAME_MAXLEN + 1];
} smashboot_dev_t;

smashboot_dev_t *smashboot_dev(struct DeviceNode *dn);
void volume_boot_attach(struct DeviceNode *dn);
#endif

void volume_seen(char *name, uint32_t access_time);
int  volume_update(void);
void volume_events_open(void);
void volume_events_close(void);
void volume_port_reject(struct MsgPort *mp);
void volume_abandon(void);
void volume_reopen_handles(void);
void volume_message(uint mask);

void unix_time_to_amiga_datestamp(uint sec, uint nsec, struct DateStamp *ds);
uint amiga_datestamp_to_unix_time(struct DateStamp *ds, uint *nsec);

extern ULONG volume_msg_masks;

#endif /* _FS_VOL_H */
