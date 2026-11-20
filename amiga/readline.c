/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in August 2020.
 *
 * ---------------------------------------------------------------------
 *
 * Input command line handling.
 *
 * The GNU readline library is great, but unnecessary for the full
 * function of this utility.  This module implements a barebones
 * version to the same API.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exec/types.h>
#include <inline/exec.h>
#include <inline/expansion.h>
#include <proto/dos.h>
#include <exec/memory.h>
#include <exec/interrupts.h>
#include <exec/execbase.h>
#include <inline/alib.h>
#include <sys/time.h>
#include "smashftp_cli.h"
#include "readline.h"
#include "smashftp.h"

#define INPUT_BUF_MAX 512  /* Max input line length */

/* ASCII input and output keystrokes */
#define KEY_CTRL_A           0x01  /* ^A Line begin */
#define KEY_CTRL_B           0x02  /* ^B Cursor left */
#define KEY_CTRL_C           0x03  /* ^C Abort */
#define KEY_CTRL_D           0x04  /* ^D Delete char to the right */
#define KEY_CTRL_E           0x05  /* ^E Line end */
#define KEY_CTRL_F           0x06  /* ^F Cursor right */
#define KEY_CTRL_G           0x07  /* ^G Keyboard beep / bell */
#define KEY_CTRL_H           0x08  /* ^H Terminal backspace character */
#define KEY_CTRL_I           0x09  /* ^I Tab */
#define KEY_CTRL_J           0x0a  /* ^J Newline */
#define KEY_CTRL_K           0x0b  /* ^K Erase to end of line */
#define KEY_CTRL_L           0x0c  /* ^L Redraw line */
#define KEY_CTRL_M           0x0d  /* ^M Carriage Return */
#define KEY_CTRL_N           0x0e  /* ^N - Cursor down */
#define KEY_CTRL_P           0x10  /* ^P Cursor up */
#define KEY_CTRL_R           0x12  /* ^R Redraw line */
#define KEY_CTRL_U           0x15  /* ^U Erase to start of line */
#define KEY_CTRL_V           0x16  /* ^V Take next input as literal */
#define KEY_CTRL_W           0x17  /* ^W Erase word */
#define KEY_CTRL_X           0x18  /* ^X Erase entire line */
#define KEY_CTRL_Y           0x19  /* ^Y Show history */
#define KEY_ESC              0x1b  /* Escape key */
#define KEY_SPACE            0x20  /* Space key */
#define KEY_BACKSPACE2       0x7f  /* ^? Backspace on some keyboards */
#define KEY_AMIGA_ESC        0x9b  /* Amiga key sequence */

#define KEY_LINE_BEGIN       KEY_CTRL_A
#define KEY_LEFT             KEY_CTRL_B
#define KEY_DEL_CHAR         KEY_CTRL_D
#define KEY_LINE_END         KEY_CTRL_E
#define KEY_RIGHT            KEY_CTRL_F
#define KEY_BACKSPACE        KEY_CTRL_H
#define KEY_TAB              KEY_CTRL_I
#define KEY_NL               KEY_CTRL_J
#define KEY_CLEAR_TO_END     KEY_CTRL_K
#define KEY_REDRAW1          KEY_CTRL_L
#define KEY_CR               KEY_CTRL_M
#define KEY_DOWN             KEY_CTRL_N
#define KEY_CURSOR_UP        KEY_CTRL_P
#define KEY_REDRAW2          KEY_CTRL_R
#define KEY_CLEAR_TO_START   KEY_CTRL_U
#define KEY_DEL_WORD         KEY_CTRL_W
#define KEY_CLEAR            KEY_CTRL_X
#define KEY_HISTORY          KEY_CTRL_Y

/* Input ESC key modes */
typedef enum {
    INPUT_MODE_NORMAL,  /* Normal user input */
    INPUT_MODE_ESC,     /* ESC key pressed */
    INPUT_MODE_BRACKET, /* ESC [ pressed */
    INPUT_MODE_1,       /* ESC [ 1 pressed (HOME key sequence) */
    INPUT_MODE_2,       /* ESC [ 2 pressed (INSERT key sequence) */
    INPUT_MODE_3,       /* ESC [ 3 pressed (DEL key sequence) */
    INPUT_MODE_1SEMI,   /* ESC [ 1 ; pressed (ctrl-cursor key) */
    INPUT_MODE_1SEMI2,  /* ESC [ 1 ; 2 pressed (shift-cursor key) */
    INPUT_MODE_1SEMI3,  /* ESC [ 1 ; 3 pressed (alt-cursor key) */
    INPUT_MODE_1SEMI5,  /* ESC [ 1 ; 5 pressed (ctrl-cursor key) */
    INPUT_MODE_LITERAL, /* Control-V pressed (next input is literal) */
} input_mode_t;

#ifndef EMBEDDED_CMD
#ifndef AMIGA
static struct termios saved_term;  /* Original terminal settings */
#endif
static int            did_tcsetattr = 0;
#endif /* !EMBEDDED_CMD */
int                   history_base;
static char          *history_cur;
static char           history_buf[HISTORY_MAX_CHARS];
static uint8_t        history_cur_line;
static const char    *input_line_prompt;
static input_mode_t   input_mode = INPUT_MODE_NORMAL;
static uint           input_pos;
static uint8_t        input_need_prompt;
static char           input_buf[INPUT_BUF_MAX];
static uint           input_tab_count;   /* Consecutive Tab key presses */
static rl_completer_t rl_completer;      /* Command line completion func */


/* ------------------------ Internal functions ------------------------ */
#ifndef EMBEDDED_CMD
static void
delay_msec(int msec)
{
#ifdef AMIGA
#define TICKS_PER_SEC 50
    Delay(msec * TICKS_PER_SEC / 1000);
#else
    if (poll(NULL, 0, msec) < 0)
        warn("poll() failed");
#endif
}
#endif

static void
putchars(int ch, uint count)
{
    while (count-- > 0)
        putchar(ch);
}

static void
putstr(char *str)
{
    while (*str != '\0')
        putchar(*(str++));
}

#ifndef EMBEDDED_CMD
static void
readline_cleanup_at_exit(void)
{
    if (did_tcsetattr) {
        did_tcsetattr = 0;
#ifdef AMIGA
        setvbuf(stdin, NULL, _IOLBF, 0);   /* Line input mode */
        setvbuf(stdout, NULL, _IOLBF, 0);  /* Line output mode */
        SetMode(Output(), FALSE);          /* Disable raw output */
#else
        (void) tcsetattr(0, TCSANOW, &saved_term);
#endif
    }
}
#endif

static char *
history_char_next(char *ptr)
{
    if (++ptr >= history_buf + sizeof (history_buf))
        ptr = history_buf;
    return (ptr);
}

static char *
history_char_prev(char *ptr)
{
    if (ptr <= history_buf)
        ptr = history_buf + sizeof (history_buf);
    return (--ptr);
}

static bool_t
history_fetch(char *cmd, int line_num)
{
    char *ptr = history_char_prev(history_cur);

    if (line_num == 0) {
        /* Blank input line */
        *cmd = '\0';
        return (TRUE);
    }

    while (ptr != history_cur) {
        ptr = history_char_prev(ptr);

        if (*ptr != '\0') {
            /* Not yet at the start of the previous line */
            continue;
        }

        if (--line_num > 0) {
            /* Not yet at the desired history line */
            ptr = history_char_prev(ptr);  /* Skip '\0' */
            continue;
        }

        ptr = history_char_next(ptr);
        if (*ptr == '\0')
            return (FALSE);  /* No previous history */

        do {
            *(cmd++) = *ptr;
            ptr = history_char_next(ptr);
        } while (*ptr != '\0');
        *cmd = '\0';

        return (TRUE);
    }
    /* Specified position was not located in history */
    return (FALSE);
}

static bool_t
history_add(const char *cmd, int prev_hist_line)
{
    char *ptr;
    char  prev[sizeof (input_buf)];

    if (prev_hist_line == 0)
        prev_hist_line = 1;

    if (history_cur == NULL)
        history_cur = history_buf;

    /* Strip initial whitespace */
    while ((*cmd == KEY_SPACE || *cmd == KEY_TAB))
        cmd++;

    /* Don't add command line if it's blank */
    if (cmd[0] == '\0')
        return (FALSE);

    /* Don't add if it matches most recent history line */
    if ((history_fetch(prev, prev_hist_line) == TRUE) &&
        (strcmp(prev, cmd) == 0))
        return (FALSE);

    /* Copy new string to history */
    while (*cmd != '\0') {
        *history_cur = *(cmd++);
        history_cur  = history_char_next(history_cur);
    }

    /* Terminate string and clear remaining part of old string */
    for (ptr = history_cur; *ptr != '\0'; ptr = history_char_next(ptr))
        *ptr = '\0';

    history_cur = history_char_next(history_cur);
    return (TRUE);
}

static void
input_clear(void)
{
    input_pos = 0;
    input_buf[0] = '\0';
}

static void
input_show_prompt(const char *prompt)
{
    printf("%s%s", prompt, input_buf);
    putchars(KEY_BACKSPACE, strlen(input_buf) - input_pos);
    fflush(stdout);
}

/*
 * input_is_special
 * ----------------
 * Returns non-zero if the character must be escaped with a backslash
 * to be taken literally as part of a command line argument.
 */
static uint
input_is_special(char ch)
{
    switch (ch) {
        case ' ':
        case '\t':
        case '"':
        case '\'':
        case '\\':
        case ';':
        case '&':
        case '|':
            return (1);
    }
    return (0);
}

/*
 * input_lower
 * -----------
 * Converts an ASCII upper case letter to lower case. This is used instead
 * of tolower(), which does not work as expected in clib2.
 */
static char
input_lower(char ch)
{
    if ((ch >= 'A') && (ch <= 'Z'))
        return (ch + ('a' - 'A'));
    return (ch);
}

/*
 * input_name_cmp
 * --------------
 * Compares two names, optionally without regard to case.
 */
static int
input_name_cmp(const char *name1, const char *name2, uint nocase)
{
    while (*name1 != '\0') {
        char ch1 = *name1;
        char ch2 = *name2;
        if (nocase) {
            ch1 = input_lower(ch1);
            ch2 = input_lower(ch2);
        }
        if (ch1 != ch2)
            return ((uint8_t) ch1 - (uint8_t) ch2);
        name1++;
        name2++;
    }
    return (-(uint8_t) *name2);
}

/*
 * input_show_matches
 * ------------------
 * Displays the final name component of each completion match, sorted,
 * in columns. Directories are shown with a trailing '/'.
 */
static void
input_show_matches(rl_match_t *matches, uint count, uint nocase)
{
    uint cur;
    uint pos;
    uint maxlen = 0;
    uint cols;
    uint rows;
    uint row;
    uint col;

    /* Sort by name (insertion sort) */
    for (cur = 1; cur < count; cur++) {
        rl_match_t tmp = matches[cur];
        for (pos = cur; pos > 0; pos--) {
            if (input_name_cmp(matches[pos - 1].rm_name, tmp.rm_name,
                               nocase) <= 0)
                break;
            matches[pos] = matches[pos - 1];
        }
        matches[pos] = tmp;
    }

    /* Trim the directory prefix from each name */
    for (cur = 0; cur < count; cur++) {
        char *name = matches[cur].rm_name;
        char *ptr;
        uint  len;
        for (ptr = name; *ptr != '\0'; ptr++)
            if ((*ptr == '/') || (*ptr == ':'))
                name = ptr + 1;
        if (name != matches[cur].rm_name)
            memmove(matches[cur].rm_name, name, strlen(name) + 1);
        len = strlen(matches[cur].rm_name) + matches[cur].rm_isdir;
        if (maxlen < len)
            maxlen = len;
    }

    cols = 79 / (maxlen + 2);
    if (cols == 0)
        cols = 1;
    rows = (count + cols - 1) / cols;

    putchar(KEY_NL);
    for (row = 0; row < rows; row++) {
        for (col = 0; col < cols; col++) {
            uint len;
            cur = col * rows + row;
            if (cur >= count)
                break;
            printf("%s%s", matches[cur].rm_name,
                   matches[cur].rm_isdir ? "/" : "");
            len = strlen(matches[cur].rm_name) + matches[cur].rm_isdir;
            if ((col + 1 < cols) && (cur + rows < count))
                putchars(KEY_SPACE, maxlen + 2 - len);
        }
        putchar(KEY_NL);
    }
}

/*
 * input_complete
 * --------------
 * Handles the Tab key by completing the word before the cursor, using
 * the registered completion function. If the matches have more in common
 * than what has been typed, the common part is filled in. A unique match
 * is followed by '/' for a directory or a space otherwise. If nothing
 * can be filled in, a bell is sounded, and on the second consecutive Tab,
 * all matches are displayed.
 */
static void
input_complete(void)
{
    static char line[INPUT_BUF_MAX];  /* Static to reduce stack use */
    static char word[INPUT_BUF_MAX];
    static char repl[INPUT_BUF_MAX];
    rl_match_t *matches;
    uint        in_squotes = 0;
    uint        in_dquotes = 0;
    uint        wstart = 0;
    uint        wlen = 0;
    uint        rlen = 0;
    uint        pos;
    uint        cur;
    uint        count;
    uint        common;
    uint        oldlen;
    uint        taillen;
    uint        nocase = 0;

    if (rl_completer == NULL)
        return;

    /*
     * Find the start of the word at the cursor, removing quoting.
     * This follows the argument splitting rules of make_arglist().
     */
    for (pos = 0; pos < input_pos; pos++) {
        char ch = input_buf[pos];
        if (ch == '\\') {
            if (pos + 1 < input_pos)
                word[wlen++] = input_buf[++pos];
            continue;
        }
        if ((ch == '\'') && !in_dquotes) {
            in_squotes = !in_squotes;
            continue;
        }
        if ((ch == '"') && !in_squotes) {
            in_dquotes = !in_dquotes;
            continue;
        }
        if (!in_squotes && !in_dquotes &&
            ((ch == ' ') || (ch == '\t') || (ch == ';') ||
             (ch == '&') || (ch == '|'))) {
            wstart = pos + 1;
            wlen = 0;
            continue;
        }
        word[wlen++] = ch;
    }
    word[wlen] = '\0';
    memcpy(line, input_buf, wstart);
    line[wstart] = '\0';

    count = rl_completer(line, word, &matches, &nocase);
    if (count == 0) {
        putchar(KEY_CTRL_G);
        return;
    }

    /* Find the length the matches have in common */
    common = strlen(matches[0].rm_name);
    for (cur = 1; cur < count; cur++) {
        const char *name = matches[cur].rm_name;
        for (pos = 0; pos < common; pos++) {
            char ch1 = name[pos];
            char ch2 = matches[0].rm_name[pos];
            if (nocase) {
                ch1 = input_lower(ch1);
                ch2 = input_lower(ch2);
            }
            if (ch1 != ch2)
                break;
        }
        common = pos;
    }

    if ((count > 1) && (common <= wlen)) {
        /* Nothing more can be filled in */
        if (input_tab_count < 2) {
            putchar(KEY_CTRL_G);
        } else {
            input_show_matches(matches, count, nocase);
            input_need_prompt = 1;
        }
        goto complete_free;
    }

    /* Build the replacement word, escaping special characters */
    for (pos = 0; pos < common; pos++) {
        char ch = matches[0].rm_name[pos];
        if (rlen + 3 >= sizeof (repl))
            break;
        if (input_is_special(ch))
            repl[rlen++] = '\\';
        repl[rlen++] = ch;
    }
    if (count == 1)
        repl[rlen++] = matches[0].rm_isdir ? '/' : ' ';
    repl[rlen] = '\0';

    oldlen  = strlen(input_buf + wstart);
    taillen = strlen(input_buf + input_pos);
    if (wstart + rlen + taillen + 1 > sizeof (input_buf)) {
        putchar(KEY_CTRL_G);  /* Does not fit in input buffer */
        goto complete_free;
    }

    /* Replace the word and redisplay the line from the start of the word */
    memmove(input_buf + wstart + rlen, input_buf + input_pos, taillen + 1);
    memcpy(input_buf + wstart, repl, rlen);
    putchars(KEY_BACKSPACE, input_pos - wstart);
    putstr(input_buf + wstart);
    if (oldlen > rlen + taillen) {
        putchars(KEY_SPACE, oldlen - (rlen + taillen));
        taillen = oldlen - rlen;
    }
    putchars(KEY_BACKSPACE, taillen);
    input_pos = wstart + rlen;

complete_free:
    for (cur = 0; cur < count; cur++)
        free(matches[cur].rm_name);
    free(matches);
}

int
get_new_input_line(const char *prompt, char **line)
{
    uint len;
    uint tmp;
    int  ch;

    *line = NULL;

    input_line_prompt = prompt;
    if (input_need_prompt) {
        input_need_prompt = 0;
        input_show_prompt(prompt);
        if (is_user_abort())
            ch = getchar();  // Discard ^C
    }
    ch = getchar();

    if (ch <= 0)
        return (RC_NO_DATA);

    if (input_pos >= sizeof (input_buf))
        input_clear();

    switch (input_mode) {
        default:
        case INPUT_MODE_NORMAL:
            break;

        case INPUT_MODE_ESC:
            if ((ch == '[') || (ch == 'O')) {
                input_mode = INPUT_MODE_BRACKET;
            } else {
                /* Unrecognized ESC sequence -- swallow both */
                input_mode = INPUT_MODE_NORMAL;
            }
            return (RC_SUCCESS);

        case INPUT_MODE_BRACKET:
            input_mode = INPUT_MODE_NORMAL;
            switch (ch) {
                case 'A':
                    ch = KEY_CURSOR_UP;
                    break;
                case 'B':
                    ch = KEY_DOWN;
                    break;
                case 'C':
                    ch = KEY_RIGHT;
                    break;
                case 'D':
                    ch = KEY_LEFT;
                    break;
                case 'F':
                    ch = KEY_LINE_END;
                    break;
                case 'H':
                    ch = KEY_LINE_BEGIN;
                    break;
                case 'M':
                    ch = KEY_CR;  /* Enter on numeric keypad */
                    break;
                case '1':
                    input_mode = INPUT_MODE_1;
                    return (RC_SUCCESS);
                case '2':
                    input_mode = INPUT_MODE_2;
                    return (RC_SUCCESS);
                case '3':
                    input_mode = INPUT_MODE_3;
                    return (RC_SUCCESS);
                default:
                    printf("\nUnknown 'ESC [ %c'\n", ch);
                    input_mode = INPUT_MODE_NORMAL;
                    goto redraw_prompt;
            }
            break;

        case INPUT_MODE_1:
            input_mode = INPUT_MODE_NORMAL;
            switch (ch) {
                case ';':
                    input_mode = INPUT_MODE_1SEMI;
                    return (RC_SUCCESS);
                case '~':
                    ch = KEY_LINE_BEGIN;
                    break;
                default:
                    printf("\nUnknown 'ESC [ 1 %c'\n", ch);
                    goto redraw_prompt;
            }
            break;

        case INPUT_MODE_1SEMI:
            switch (ch) {
                case '2':
                    input_mode = INPUT_MODE_1SEMI2;
                    break;
                case '3':
                    input_mode = INPUT_MODE_1SEMI3;
                    break;
                case '5':
                    input_mode = INPUT_MODE_1SEMI5;
                    break;
                default:
                    input_mode = INPUT_MODE_NORMAL;
                    printf("\nUnknown 'ESC [ 1 ; %c'\n", ch);
                    goto redraw_prompt;
            }
            return (RC_SUCCESS);

        case INPUT_MODE_1SEMI2:
        case INPUT_MODE_1SEMI3:
        case INPUT_MODE_1SEMI5:
            input_mode = INPUT_MODE_NORMAL;
            switch (ch) {
                case 'C':
                    ch = KEY_LINE_END;
                    break;
                case 'D':
                    ch = KEY_LINE_BEGIN;
                    break;
                default:
                    printf("\nUnknown 'ESC [ 1 ; %c %c'\n",
                           (input_mode == INPUT_MODE_1SEMI2) ? '2' :
                           (input_mode == INPUT_MODE_1SEMI3) ? '3' : '5',
                           ch);
                    goto redraw_prompt;
            }
            break;

        case INPUT_MODE_2:
            if (ch != '~') {
                printf("\nUnknown 'ESC [ 1 %c'\n", ch);
                goto redraw_prompt;
            }
            /* Insert key */
            input_mode = INPUT_MODE_NORMAL;
            return (RC_SUCCESS);

        case INPUT_MODE_3:
            input_mode = INPUT_MODE_NORMAL;
            if (ch != '~') {
                printf("\nUnknown 'ESC [ 3 %c'\n", ch);
                goto redraw_prompt;
            }
            ch = KEY_DEL_CHAR;
            break;
        case INPUT_MODE_LITERAL:
            input_mode = INPUT_MODE_NORMAL;
            goto literal_input;
    }

    if (ch == KEY_TAB)
        input_tab_count++;
    else
        input_tab_count = 0;

    switch (ch) {
        case KEY_TAB:
            /* Tab completes the word before the cursor */
            input_complete();
            break;
        case KEY_REDRAW1:
        case KEY_REDRAW2:
            /* ^L or ^R redraws line */
redraw_prompt:
            putchar(KEY_NL);
            input_need_prompt = 1;
            break;
        case KEY_CR:
        case KEY_NL:
            /* CR executes command */
            putchar(KEY_NL);
            (void) history_add(input_buf, 0);
            history_cur_line = 0;
            input_need_prompt = 1;
            *line = input_buf;
            input_pos = 0;
            break;
        case KEY_CTRL_C:
            /* ^C Aborts current input */
            puts("^C");
            input_clear();
            input_need_prompt = 1;
            history_cur_line = 0;
            return (RC_USR_ABORT);
        case KEY_BACKSPACE:
        case KEY_BACKSPACE2:
            /* ^H deletes one character to the left */
            if (input_pos == 0)
                break;
            memmove(input_buf + input_pos - 1, input_buf + input_pos,
                    sizeof (input_buf) - input_pos);
            input_pos--;
            putchar(KEY_BACKSPACE);
            putstr(input_buf + input_pos);
            putchar(KEY_SPACE);
            putchars(KEY_BACKSPACE, strlen(input_buf + input_pos) + 1);
            break;
        case KEY_DEL_CHAR:
            if (input_buf[input_pos] == '\0')
                break;  /* Nothing more to delete at end of line */
            memmove(input_buf + input_pos, input_buf + input_pos + 1,
                    sizeof (input_buf) - input_pos);
            putstr(input_buf + input_pos);
            putchar(KEY_SPACE);
            putchars(KEY_BACKSPACE, strlen(input_buf + input_pos) + 1);
            break;
        case KEY_LINE_BEGIN:
            /* Go to the beginning of the input line (^A) */
            putchars(KEY_BACKSPACE, input_pos);
            input_pos = 0;
            break;
        case KEY_LINE_END:
            /* Go to the end of the input line (^E) */
            putstr(input_buf + input_pos);
            input_pos += strlen(input_buf + input_pos);
            break;
        case KEY_LEFT:
            /* Move the cursor one position to the left (^B) */
            if (input_pos == 0)
                break;
            input_pos--;
            putchar(KEY_BACKSPACE);
            break;
        case KEY_RIGHT:
            /* Move the cursor one position to the right (^F) */
            if (input_pos >= sizeof (input_buf) - 1)
                break;
            if (input_buf[input_pos] == '\0')
                break;
            putchar((uint8_t) input_buf[input_pos]);
            input_pos++;
            break;
        case KEY_CTRL_V:
            input_mode = INPUT_MODE_LITERAL;
            break;
        case KEY_ESC:
            /* ESC initiates an Escape sequence */
            input_mode = INPUT_MODE_ESC;
            break;
        case KEY_AMIGA_ESC:
            /* Amiga ESC initiates an Escape [ sequence */
            input_mode = INPUT_MODE_BRACKET;
            break;
        case KEY_CLEAR_TO_START:
            /* Delete all text to the left of the cursor */
            putchars(KEY_BACKSPACE, input_pos);
            putstr(input_buf + input_pos);
            len = strlen(input_buf + input_pos);
            putchars(KEY_SPACE, input_pos);
            putchars(KEY_BACKSPACE, input_pos + len);
            memmove(input_buf, input_buf + input_pos, len + 1);
            input_pos = 0;
            break;
        case KEY_CLEAR_TO_END:
            /* Delete all text from the cursor to end of line */
            len = strlen(input_buf + input_pos);
            putchars(KEY_SPACE, len);
            putchars(KEY_BACKSPACE, len);
            input_buf[input_pos] = 0;
            break;
        case KEY_CLEAR:
            /* Delete all text */
            len = strlen(input_buf + input_pos);
            putchars(KEY_BACKSPACE, input_pos);
            putchars(KEY_SPACE, input_pos + len);
            putchars(KEY_BACKSPACE, input_pos + len);
            input_clear();
            break;
        case KEY_DEL_WORD:
            /* Delete word */
            if (input_pos == 0)
                break;
            /* Skip whitespace */
            for (tmp = input_pos; tmp > 0; tmp--)
                if ((input_buf[tmp - 1] != KEY_SPACE) &&
                    (input_buf[tmp - 1] != KEY_TAB))
                    break;
            /* Find the start of the word */
            for (; tmp > 0; tmp--)
                if ((input_buf[tmp - 1] == KEY_SPACE) ||
                    (input_buf[tmp - 1] == KEY_TAB))
                    break;
            len = strlen(input_buf + input_pos);
            putchars(KEY_BACKSPACE, input_pos - tmp);
            putstr(input_buf + input_pos);
            putchars(KEY_SPACE, input_pos - tmp);
            putchars(KEY_BACKSPACE, len + input_pos - tmp);
            memmove(input_buf + tmp, input_buf + input_pos, len + 1);
            input_pos = tmp;
            break;
        case KEY_CURSOR_UP:
            /* History previous */
            len = strlen(input_buf);
            if (history_add(input_buf, history_cur_line) == TRUE)
                history_cur_line++;  /* Line was modified and is now saved */

            if (history_fetch(input_buf, ++history_cur_line) == FALSE) {
                history_cur_line--;
                break;
            }
            goto update_input_line;
        case KEY_DOWN:
            /* History next */
            len = strlen(input_buf);
            if (history_add(input_buf, history_cur_line) == TRUE)
                if (history_cur_line != 0)
                    history_cur_line++;  /* Modified line is now saved */

            if (history_cur_line == 0) {
                input_buf[0] = '\0';
                goto update_input_line;
            }
            if (history_fetch(input_buf, --history_cur_line) == FALSE) {
                history_cur_line++;
                break;
            }
update_input_line:
        {
            uint new_len = strlen(input_buf);

            if (len + strlen(input_line_prompt) > 80) {
                putchar(KEY_CR);
                input_pos = new_len;
                input_need_prompt = 1;
                break;
            }
            putchars(KEY_BACKSPACE, input_pos);
            putstr(input_buf);
            if (len > new_len) {
                /* Old command length was longer -- overwrite that portion */
                putchars(KEY_SPACE, len - new_len);
                putchars(KEY_BACKSPACE, len - new_len);
            }
            input_pos = new_len;
            break;
        }
        case KEY_HISTORY:
            putchar(KEY_NL);
            history_show();
            input_need_prompt = 1;
            break;
        default:
            /* Regular input is inserted at current cursor position */
            if ((ch < 0x20) || (ch >= 0x80))
                break;
literal_input:
            len = strlen(input_buf + input_pos) + 1;
            if (len + 1 + input_pos >= sizeof (input_buf))
                break;  /* End of input buffer */

            /* Push input following the cursor to the right */
            memmove(input_buf + input_pos + 1, input_buf + input_pos, len);
            input_buf[input_pos] = (uint8_t) ch;

            putstr(input_buf + input_pos);
            putchars(KEY_BACKSPACE, len - 1);
            input_pos++;
    }
    return (RC_SUCCESS);
}

/* --------------------------- Readline API --------------------------- */
void
add_history(const char *line)
{
    UNUSED(line);
}

HIST_ENTRY *
history_get(int line_num)
{
    static HIST_ENTRY cur;
    static char histbuf[2048];
    if (history_fetch(histbuf, line_num) == FALSE)
        return (NULL);
    cur.line = histbuf;
    return (&cur);
}

HISTORY_STATE *
history_get_history_state(void)
{
    HISTORY_STATE *state = malloc(sizeof (*state));
    if (state == NULL)
        errx(EXIT_FAILURE, "no memory");

    state->history_cur       = history_cur;
    state->history_cur_line  = history_cur_line;
    memcpy(state->history_buf, history_buf, sizeof (history_buf));
    return (state);
}

void
history_set_history_state(const HISTORY_STATE *state)
{
    history_cur      = state->history_cur;
    history_cur_line = state->history_cur_line;
    memcpy(history_buf, state->history_buf, sizeof (history_buf));
}

int
history_expand(const char *line, char **expansion)
{
    UNUSED(line);
    UNUSED(expansion);
    return (0);
}

void
using_history(void)
{
    history_base = 1;
    history_cur_line = 0;
    memset(history_buf, 0, sizeof (history_buf));
}

int
rl_bind_key(int key, const void *func)
{
    UNUSED(key);
    UNUSED(func);
    return (0);
}

void
rl_set_completer(rl_completer_t func)
{
    rl_completer = func;
}

int
rl_initialize(void)
{
    static bool_t did_rl_init = FALSE;

    if (did_rl_init == TRUE)
        return (0);

    did_rl_init       = TRUE;
    input_need_prompt = 1;
    history_cur       = history_buf;
    input_clear();

#ifndef EMBEDDED_CMD
#ifdef AMIGA
    did_tcsetattr = 1;
    setvbuf(stdin, NULL, _IONBF, 0);   /* Raw input mode */
    setvbuf(stdout, NULL, _IONBF, 0);  /* Raw output mode */
    setbuf(stdout, NULL);              /* Unbuffered output mode */
    SetMode(Output(), TRUE);           /* Raw output */
#else
    if (tcgetattr(0, &saved_term)) {
        warn("Could not get terminal information");
        did_tcsetattr = 0;
    } else {
        struct termios term   = saved_term;
        int            enable = 1;

        cfmakeraw(&term);
        term.c_oflag |= OPOST;
#ifdef LET_CTRL_C_KILL_PROGRAM
        term.c_lflag |= ISIG;
#endif

        did_tcsetattr = 1;
        if (tcsetattr(0, TCSANOW, &term) != 0)
            warn("tcsetattr failed");
        if (ioctl(fileno(stdin), FIONBIO, &enable)) /* Non-blocking input */
            warn("FIONBIO failed");
    }
#endif

    (void) atexit(readline_cleanup_at_exit);
#endif  /* !EMBEDDED_CMD */

    return (0);
}

#ifndef EMBEDDED_CMD
char *
readline(const char *prompt)
{
    char *cmd;

    (void) rl_initialize();

    input_clear();

    /* Acquire input command by processing keystrokes */
    do {
        rc_t rc = get_new_input_line(prompt, &cmd);
        if (rc == RC_USR_ABORT)
            return (NULL);
        if (rc == RC_NO_DATA)
            delay_msec(50);
    } while (cmd == NULL);

    return (strdup(cmd));
}
#endif

/* --------------------------- Extra functions --------------------------- */

void
history_show(void)
{
    int   cur_line = 0;
    char *ptr      = history_cur;

    if (ptr == NULL)
        return;

    /* Display each line from history */
    do {
        if (*ptr != '\0') {
            printf("%4d: ", cur_line++);
            while (*ptr != '\0') {
                putchar(*ptr);
                ptr = history_char_next(ptr);
                if (ptr == history_cur) {
                    putchar(KEY_NL);
                    return;
                }
            }
            putchar(KEY_NL);
        }
        ptr = history_char_next(ptr);
    } while (ptr != history_cur);
}
