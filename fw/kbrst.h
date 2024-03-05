/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2023.
 *
 * ---------------------------------------------------------------------
 *
 * Amiga kbrst prototypes.
 */
#ifndef _KBRST_H
#define _KBRST_H

void kbrst_poll(void);
void kbrst_amiga(uint hold, uint longreset);

extern uint8_t amiga_not_in_reset;
extern uint    amiga_reboot_detect;

#endif /* _KBRST_H */

