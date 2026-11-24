/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2024.
 *
 * ---------------------------------------------------------------------
 *
 * Filesystem Handler
 */

#ifndef _FS_PACKET_H
#define _FS_PACKET_H

#include "fs_vol.h"

void handle_packet(void);
void fs_packet_cleanup(void);
LONG packet_fail_res1(LONG type);

extern struct DosPacket *gpack;  // current packet being processed

#endif /* _FS_PACKET_H */
