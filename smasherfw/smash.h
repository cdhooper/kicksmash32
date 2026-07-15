/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 20246
 *
 * ---------------------------------------------------------------------
 *
 * Kicksmash access utility
 */

#ifndef _SMASH_H
#define _SMASH_H

rc_t cmd_smash(int argc, char * const *argv);
int erase_flash(uint bank, uint addr, uint len, uint l_flag_yes);
uint read_from_flash(uint bank, uint addr, void *buf, uint len);
uint write_to_flash(uint bank, uint addr, void *buf, uint len);
void smash_restore_bank(void);
int flash_show_id(uint quiet);
int flash_generic_cmd(uint32_t addr, uint cmd);
uint smash_test_only(void);

extern const char cmd_smash_help[];

#endif /* _SMASH_H */
