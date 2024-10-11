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

#ifndef _FS_HAND_H
#define _FS_HAND_H

#ifndef BIT
#define BIT(x) (1U << (x))
#endif

#ifndef CTOB
#define CTOB(x) (((unsigned long) x)>>2)
#endif

#ifndef BTOC
#define BTOC(x) ((void *)(((unsigned long) x)<<2))
#endif

uint8_t grunning;        // 1=running, 0=stopping
uint8_t gvolumes_inuse;  // 0=no volumes

#endif /* _FS_HAND_H */
