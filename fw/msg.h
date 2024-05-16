/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2024.
 *
 * ---------------------------------------------------------------------
 *
 * Amiga message interface.
 */

#ifndef __MSG_H
#define __MSG_H

int      address_log_replay(uint max);
void     bus_snoop(uint mode);
void     msg_poll(void);
void     msg_init(void);
void     msg_shutdown(void);
void     msg_mode(uint mode);
void     msg_usb_service(void);

#endif /* __MSG_H */
