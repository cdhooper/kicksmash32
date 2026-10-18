/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2024.
 *
 * ---------------------------------------------------------------------
 *
 * Declarations shared between smash.c and smash_flash.c
 */

#ifndef _SMASH_H
#define _SMASH_H

#define ROM_BASE         0x00f80000  /* Base address of Kickstart ROM */

rc_t cmd_smash(int argc, char * const *argv);

extern const char cmd_smash_help[];
extern uint8_t    flag_conservative_mode;
extern uint32_t   rom_header_expected;

#endif /* _SMASH_H */
