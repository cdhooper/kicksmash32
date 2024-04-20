/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2024.
 *
 * ---------------------------------------------------------------------
 *
 * Amiga-to-Kicksmash message interface functions.
 */

#ifndef _MSG_H
#define _MSG_H

/* Status codes from local message handling */
#define MSG_STATUS_SUCCESS    0           // No error
#define MSG_STATUS_FAILURE    1           // Generic failure
#define MSG_STATUS_NO_REPLY   0xfffffff9  // Did not get reply from Kicksmash
#define MSG_STATUS_BAD_LENGTH 0xfffffff8  // Bad length detected
#define MSG_STATUS_BAD_CRC    0xfffffff7  // CRC failure detected
#define MSG_STATUS_BAD_DATA   0xfffffff6  // Invalid data
#define MSG_STATUS_PRG_TMOUT  0xfffffff5  // Programming timeout
#define MSG_STATUS_PRG_FAIL   0xfffffff4  // Programming failure

typedef unsigned int uint;

void msg_init(void);

uint send_cmd_core(uint16_t cmd, void *arg, uint16_t arglen,
                   void *reply, uint replymax, uint *replyalen);

uint send_cmd(uint16_t cmd, void *arg, uint16_t arglen,
              void *reply, uint replymax, uint *replyalen);

extern uint smash_cmd_shift;

#endif /* _MSG_H */
