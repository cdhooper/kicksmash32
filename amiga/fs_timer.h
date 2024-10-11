/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2024.
 *
 * ---------------------------------------------------------------------
 *
 * Filesystem timer handling code.
 */

#ifndef _FS_TIMER_H
#define _FS_TIMER_H

void timer_open(void);
void timer_close(void);
void timer_restart(uint msec);

extern ULONG timer_msg_mask;
extern struct timerequest *timerio;

#endif /* _FS_TIMER_H */
