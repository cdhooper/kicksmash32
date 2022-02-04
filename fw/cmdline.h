/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2020.
 *
 * ---------------------------------------------------------------------
 *
 * Command line interpreter.
 */

#ifndef _CMDLINE_H
#define _CMDLINE_H

#ifdef AMIGA
#include "amiga_stdint.h"
#else
#include <stdint.h>
#endif

#define ARRAY_SIZE(x) (int)((sizeof (x) / sizeof ((x)[0])))

#ifdef AMIGA
typedef int _Bool;
#endif
typedef _Bool bool_t;
#define FALSE 0
#define TRUE !FALSE

typedef enum {
    RC_SUCCESS   = 0,  /* Successful completion */
    RC_FAILURE   = 1,  /* Call failed */
    RC_USER_HELP = 2,  /* User needs help */
    RC_USR_ABORT = 3,  /* User pressed ^C */
    RC_BUSY      = 4,  /* Call function again */
    RC_NO_DATA   = 5,  /* No data pending */
    RC_BAD_PARAM = 6,  /* Invalid parameter */
    RC_TIMEOUT   = 7,  /* Operation timed out */
} rc_t;

int cmdline(void);
int cmd_exec_string(const char *cmd);
char *cmd_string_from_argv(int argc, char * const *argv);
rc_t scan_int(const char *str, int *intval);
rc_t cmd_exec_argv(int argc, char * const *argv);
int make_arglist(const char *cmd, char *argv[]);
void free_arglist(int argc, char *argv[]);
char *eval_cmdline_expr(const char *str);

#define MAX_ARGS 64

#endif

