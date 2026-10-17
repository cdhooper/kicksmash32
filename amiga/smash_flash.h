/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2026.
 *
 * ---------------------------------------------------------------------
 *
 * Smash utility file and flash operations.
 */

#ifndef _SMASH_FLASH_H
#define _SMASH_FLASH_H

/* smash_flash.c */
int cmd_readwrite(int argc, char *argv[]);
int cmd_erase(int argc, char *argv[]);

#endif /* _SMASH_FLASH_H */
