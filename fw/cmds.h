/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2020.
 *
 * ---------------------------------------------------------------------
 *
 * Command implementations.
 */

#ifndef _CMDS_H
#define _CMDS_H

rc_t cmd_c(int argc, char * const *argv);
rc_t cmd_comp(int argc, char * const *argv);
rc_t cmd_copy(int argc, char * const *argv);
rc_t cmd_d(int argc, char * const *argv);
rc_t cmd_delay(int argc, char * const *argv);
rc_t cmd_echo(int argc, char * const *argv);
rc_t cmd_history(int argc, char * const *argv);
rc_t cmd_ignore(int argc, char * const *argv);
rc_t cmd_loop(int argc, char * const *argv);
rc_t cmd_patt(int argc, char * const *argv);
rc_t cmd_test(int argc, char * const *argv);
rc_t cmd_time(int argc, char * const *argv);
rc_t cmd_version(int argc, char * const *argv);
rc_t parse_value(const char *arg, uint8_t *value, uint width);
rc_t parse_addr(char * const **arg, int *argc, uint64_t *space, uint64_t *addr);

void print_addr(uint64_t space, uint64_t addr);

extern const char cmd_c_help[];
extern const char cmd_comp_help[];
extern const char cmd_copy_help[];
extern const char cmd_d_help[];
extern const char cmd_patt_help[];
extern const char cmd_test_help[];
extern const char cmd_time_help[];
extern const char cmd_version_help[];

#endif  /* _CMDS_H */
