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

#ifndef _SMASH_FLASH_H
#define _SMASH_FLASH_H

extern int (*eflash_id_data)(uint32_t *data, uint data_count);

#endif /* _SMASH_FLASH_H */
