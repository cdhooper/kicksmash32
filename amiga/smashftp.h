/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in August 2024.
 *
 * ---------------------------------------------------------------------
 *
 * Smash FTP command.
 */

#ifndef _FTP_H
#define _FTP_H

rc_t cmd_cd(int argc, char * const *argv);
rc_t cmd_chmod(int argc, char * const *argv);
rc_t cmd_debug(int argc, char * const *argv);
rc_t cmd_delay(int argc, char * const *argv);
rc_t cmd_echo(int argc, char * const *argv);
rc_t cmd_get(int argc, char * const *argv);
rc_t cmd_history(int argc, char * const *argv);
rc_t cmd_ignore(int argc, char * const *argv);
rc_t cmd_lcd(int argc, char * const *argv);
rc_t cmd_lls(int argc, char * const *argv);
rc_t cmd_ln(int argc, char * const *argv);
rc_t cmd_loop(int argc, char * const *argv);
rc_t cmd_lpwd(int argc, char * const *argv);
rc_t cmd_lrmdir(int argc, char * const *argv);
rc_t cmd_lrm(int argc, char * const *argv);
rc_t cmd_ls(int argc, char * const *argv);
rc_t cmd_mkdir(int argc, char * const *argv);
rc_t cmd_mv(int argc, char * const *argv);
rc_t cmd_put(int argc, char * const *argv);
rc_t cmd_pwd(int argc, char * const *argv);
rc_t cmd_rm(int argc, char * const *argv);
rc_t cmd_time(int argc, char * const *argv);
rc_t cmd_version(int argc, char * const *argv);
rc_t parse_value(const char *arg, uint8_t *value, uint width);
rc_t parse_addr(char * const **arg, int *argc, uint64_t *space, uint64_t *addr);

extern const char cmd_time_help[];
extern const char cmd_version_help[];

#endif /* _FTP_H */

