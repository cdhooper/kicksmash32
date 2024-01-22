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

extern uint8_t amiga_not_in_reset;

void kbrst_poll(void);
void kbrst_amiga(uint hold);
