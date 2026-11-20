/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in August 2020.
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

/* A command line completion match (see rl_set_completer()) */
typedef struct rl_match {
    char *rm_name;   /* Replacement for the word being completed (malloc'd) */
    uint  rm_isdir;  /* Name is a directory */
} rl_match_t;

/*
 * Completion function: line is the input text before the word being
 * completed, and word is that word (with quoting removed). Returns the
 * number of matches, and a malloc'd array of matches in *matches.
 * *nocase is set non-zero if the names were matched without regard to case.
 */
typedef uint (*rl_completer_t)(const char *line, const char *word,
                               rl_match_t **matches, uint *nocase);

void           add_history(const char *line);
int            history_expand(const char *line, char **expansion);
HIST_ENTRY    *history_get(int line_num);
HISTORY_STATE *history_get_history_state(void);
void           history_set_history_state(const HISTORY_STATE *state);
void           history_show(void);
char          *readline(const char *prompt);
int            rl_bind_key(int key, const void *func);
int            rl_initialize(void);
void           rl_set_completer(rl_completer_t func);
void           using_history(void);
int            get_new_input_line(const char *prompt, char **line);

extern int     history_base;
extern char    history_expansion_char;
extern char    history_subst_char;

static const int history_length = 512;
#define history_length 512
#define rl_insert      NULL

#endif
