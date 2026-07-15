/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2020.
 *
 * ---------------------------------------------------------------------
 *
 * Input command line handling.
 */

#ifndef _READLINE_H
#define _READLINE_H

#define HISTORY_MAX_CHARS 2048
typedef struct _hist_entry {
    char *line;
} HIST_ENTRY;

typedef struct _hist_state {
    uint8_t history_cur_line;
    char   *history_cur;
    char    history_buf[HISTORY_MAX_CHARS];
} HISTORY_STATE;

void           add_history(const char *line);
int            history_expand(const char *line, char **expansion);
HIST_ENTRY    *history_get(int line_num);
HISTORY_STATE *history_get_history_state(void);
void           history_set_history_state(const HISTORY_STATE *state);
void           history_show(void);
char          *readline(const char *prompt);
int            rl_bind_key(int key, const void *func);
int            rl_initialize(void);
void           using_history(void);
int            get_new_input_line(const char *prompt, char **line);

extern int     history_base;
extern char    history_expansion_char;
extern char    history_subst_char;

static const int history_length = 512;
#define history_length 512
#define rl_insert      NULL

#endif
