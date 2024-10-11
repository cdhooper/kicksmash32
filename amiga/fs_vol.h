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
typedef struct MsgPort MsgPort_t;

typedef struct vollist vollist_t;
typedef struct vollist {
    char          vl_name[VOLNAME_MAXLEN + 1];
    vollist_t    *vl_next;
    uint8_t       vl_seen;
    uint8_t       vl_in_dos_list;
    uint          vl_use_count;
    uint          vl_flags;
    int8_t        vl_bootpri;
    ULONG         vl_msg_mask;
    handle_t      vl_handle;
    DeviceList_t *vl_volnode;
    DeviceList_t *vl_devnode;
    MsgPort_t    *vl_msgport;
} vollist_t;

extern vollist_t *gvol;  // current volume being handled

void volume_seen(char *name, uint32_t access_time, uint flags, int bootpri);
void volume_flush(void);
void volume_close(void);
void volume_message(uint mask);

void unix_time_to_amiga_datestamp(uint sec, struct DateStamp *ds);

extern ULONG volume_msg_masks;

#endif /* _FS_VOL_H */
