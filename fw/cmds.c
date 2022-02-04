/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2020.
 *
 * ---------------------------------------------------------------------
 *
 * Command line handling.
 */

#include "cmdline.h"
#ifdef EMBEDDED_CMD
#include "main.h"
#include "pcmds.h"
#include "prom_access.h"
#include <stdbool.h>
#include "timer.h"
#include "uart.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <time.h>
#ifdef AMIGA
#include "amiga_stdint.h"
#else
#include <malloc.h>
#include <stdint.h>
#include <unistd.h>
#endif
#ifndef EMBEDDED_CMD
#include "sfile.h"
#endif
#include "cmds.h"
#include "readline.h"
#include "mem_access.h"
#include "version.h"

#ifdef AMIGA
#define IS_BIG_ENDIAN
#define BUILD_DATE __DATE__
#define BUILD_TIME __TIME__
#endif

#ifndef EMBEDDED_CMD
static int
input_break_pending(void)
{
#ifdef AMIGA
    chkabort();  /* Handle ^C */
#endif
    return (0);
}
#endif

#define MAX_TRANSFER 128

const char cmd_c_help[] =
"c[bwlqoh] <addr> <value...>\n"
"   b = 1 byte\n"
"   w = word (2 bytes)\n"
"   l = long (4 bytes)\n"
"   q = quad (8 bytes)\n"
"   o = oct (16 bytes)\n"
"   h = hex (32 bytes)\n"
"   S = swap bytes (endian)";

const char cmd_comp_help[] =
"comp[bwlqoh] <addr> <addr> <len>\n"
"   b = 1 byte\n"
"   w = word (2 bytes)\n"
"   l = long (4 bytes)\n"
"   q = quad (8 bytes)\n"
"   o = oct (16 bytes)\n"
"   h = hex (32 bytes)\n";

const char cmd_copy_help[] =
"copy[bwlqoh] <saddr> <daddr> <len>\n"
"   b = 1 byte\n"
"   w = word (2 bytes)\n"
"   l = long (4 bytes)\n"
"   q = quad (8 bytes)\n"
"   o = oct (16 bytes)\n"
"   h = hex (32 bytes)\n";

const char cmd_d_help[] =
"d[bwlqoh] <addr> [<len>]\n"
"   b = 1 byte\n"
"   w = word (2 bytes)\n"
"   l = long (4 bytes)\n"
"   q = quad (8 bytes)\n"
"   o = oct (16 bytes)\n"
"   h = hex (32 bytes)\n"
"   A = no ASCII\n"
"   N = no output (only perform read)\n"
"   R = raw output (no address or ASCII output)\n"
"   S = swap bytes (endian)\n"
"  SS = swap ASCII display (endian)";

const char cmd_patt_help[] =
"patt[bwlqoh] <addr> <len> <pattern>\n"
"   b = 1 byte\n"
"   w = word (2 bytes)\n"
"   l = long (4 bytes)\n"
"   q = quad (8 bytes)\n"
"   o = oct (16 bytes)\n"
"   h = hex (32 bytes)\n"
"   S = swap bytes (endian)\n"
"   <pattern> may be one, zero, blip, rand, strobe, walk0, walk1, or a "
"specific value\n";
const char cmd_patt_patterns[] =
    "<pattern> may be one, zero, blip, rand, strobe, walk0, walk1, or a "
"specific value\n";

const char cmd_test_help[] =
"test[bwlqoh] <addr> <len> <mode> [read|write]\n"
"   b = 1 byte\n"
"   w = word (2 bytes)\n"
"   l = long (4 bytes)\n"
"   q = quad (8 bytes)\n"
"   o = oct (16 bytes)\n"
"   h = hex (32 bytes)\n"
"   <mode> may be one, zero, rand, walk0, or walk1\n";
const char cmd_test_patterns[] =
    "<mode> may be one, zero, rand, walk0, or walk1\n";

const char cmd_time_help[] =
"time cmd <cmd> - measure command execution time\n"
#ifndef AMIGA
"time now       - display the current time\n"
#endif
#ifdef EMBEDDED_CMD
"time test      - test timers\n"
"time watch     - watch the timer to verify tick is working correctly\n"
#endif
;

#ifdef AMIGA
#include <dos/dos.h>
void
msleep(uint msec)
{
    Delay(msec * 1000 / TICKS_PER_SECOND);
}

void
usleep(uint usec)
{
    msleep(usec / 1000);
}
#endif

#ifdef EMBEDDED_CMD
unsigned
sleep(uint sec)
{
    timer_delay_msec(sec * 1000);
    return (0);
}

int
usleep(useconds_t us)
{
    timer_delay_usec(us);
    return (0);
}
#endif

#define SPACE_OFFSET 0
#define SPACE_MEMORY 1
#define SPACE_FILE   2
#define SPACE_PROM   3

static rc_t
data_read(uint64_t space, uint64_t addr, uint width, void *buf)
{
    switch ((uint8_t) space) {
        case SPACE_MEMORY:
            return (mem_read(addr, width, buf));
#ifdef HAVE_SPACE_PROM
        case SPACE_PROM:
            return (prom_read((uint32_t)addr, width, buf));
#endif
#ifdef HAVE_SPACE_FILE
        case SPACE_FILE:
            return (file_read(space, addr, width, buf));
#endif
        default:
            printf("Internal error: Unknown space %x\n", (uint8_t) space);
            return (RC_FAILURE);
    }
}

static rc_t
data_write(uint64_t space, uint64_t addr, uint width, void *buf)
{
    switch ((uint8_t) space) {
        case SPACE_MEMORY:
            return (mem_write(addr, width, buf));
#ifdef HAVE_SPACE_PROM
        case SPACE_PROM:
            return (prom_write((uint32_t)addr, width, buf));
#endif
#ifdef HAVE_SPACE_FILE
        case SPACE_FILE:
            return (file_write(space, addr, width, buf));
#endif
        default:
            printf("Internal error: Unknown space %x\n", (uint8_t) space);
            return (RC_FAILURE);
    }
}

void
print_addr(uint64_t space, uint64_t addr)
{
    switch ((uint8_t) space) {
        case SPACE_OFFSET:
            printf("%04llx", (long long)addr);
            break;
        case SPACE_MEMORY: {
            int awidth = 16;

            if ((addr >> 32) == 0)
                awidth = 8;
            printf("%0*llx", awidth, (long long)addr);
            break;
        }
#ifdef HAVE_SPACE_PROM
        case SPACE_PROM:
            printf("%06x", (int)addr);
            break;
#endif
#ifdef HAVE_SPACE_FILE
        case SPACE_FILE: {
            int awidth = 16;
            uint slot = (uint8_t) (space >> 8);

            if ((addr >> 32) == 0) {
                awidth = 8;
                if ((addr >> 16) == 0)
                    awidth = 4;
            }
            printf("%s:%0*llx",
                   file_track[slot].filename, awidth, (long long)addr);
        }
#endif
    }
}

#ifdef HAVE_SPACE_FILE
static int
space_add_filename(uint64_t *space, const char *name)
{
    const char *end = strchr(name, ':');
    uint        slot;
    size_t      len;

    if (end == NULL)
        end = strchr(name, '\0');
    len = end - name;

    slot = file_track_filename(name, len);
    if (slot == -1)
        len = 0;  /* No slots available */
    else
        *space |= (slot << 8);

    return (len);
}
#endif

rc_t
parse_addr(char * const **arg, int *argc, uint64_t *space, uint64_t *addr)
{
    int pos = 0;
    unsigned long long x;
    const char *argp = **arg;

    if (*argc < 1) {
        printf("<addr> argument required\n");
        return (RC_USER_HELP);
    }
    *space = SPACE_MEMORY;  /* Default */

#ifdef HAVE_SPACE_PROM
    if (strncmp(argp, "prom", 5) == 0) {
        *space = SPACE_PROM;
        if (strchr(argp, ':') != NULL) {
            argp += 6;
        } else {
            (*arg)++;
            (*argc)--;
            if (*argc < 1) {
                printf("<addr> argument required\n");
                return (RC_USER_HELP);
            }
            argp = **arg;
        }
    }
#endif
#ifdef HAVE_SPACE_FILE
    if (strncmp(argp, "file", 4) == 0) {
        int len;
        *space = SPACE_FILE;
        if (strchr(argp, ':') != NULL) {
            argp += 5;
            len = space_add_filename(space, argp);
            if (len == 0)
                return (RC_FAILURE);
            argp += len + 1;
            if (*argp == '\0') {
                /* Try next argument */
                (*arg)++;
                (*argc)--;
                if (*argc < 1) {
                    printf("<addr> argument required\n");
                    return (RC_USER_HELP);
                }
                argp = **arg;
            }
        } else {
            (*arg)++;
            (*argc)--;
            if (*argc < 1) {
                printf("<filename> argument required\n");
                return (RC_USER_HELP);
            }
            argp = **arg;
            len = space_add_filename(space, argp);
            if (len == 0)
                return (RC_FAILURE);

            (*arg)++;
            (*argc)--;
            if (*argc < 1) {
                printf("<addr> argument required\n");
                return (RC_USER_HELP);
            }
            argp = **arg;
        }
    }
#endif

    if ((sscanf(argp, "%llx%n", &x, &pos) != 1) ||
        ((argp[pos] != '\0') && (argp[pos] != ' '))) {
        printf("Invalid address \"%s\"\n", argp);
        return (RC_FAILURE);
    }
    *addr = x;
    (*arg)++;
    (*argc)--;
    return (RC_SUCCESS);
}

/*
 * rand32
 * ------
 * Very simple pseudo-random number generator
 */
static uint32_t rand_seed = 0;
static uint32_t
rand32(void)
{
    rand_seed = (rand_seed * 25173) + 13849;
    return (rand_seed);
}

/*
 * srand32
 * -------
 * Very simple random number seed
 */
static void
srand32(uint32_t seed)
{
    rand_seed = seed;
}

static uint
ascii_hex_to_digit(char ch)
{
    if (ch >= '0' && ch <= '9')
        return (ch - '0');
    else if (ch >= 'a' && ch <= 'f')
        return (ch - 'a' + 10);
    else if (ch >= 'A' && ch <= 'F')
        return (ch - 'A' + 10);
    else
        return (0xff);
}

rc_t
parse_value(const char *arg, uint8_t *value, uint width)
{
    size_t pos    = 0;
    size_t arglen = strlen(arg);

    for (pos = 0; pos < arglen; pos++) {
        char ch      = arg[arglen - pos - 1];
        uint digit   = ascii_hex_to_digit(ch);
#ifdef IS_BIG_ENDIAN
        uint bytepos = width - 1 - (pos >> 1);
#else
        uint bytepos = (pos >> 1);
#endif
        if (bytepos >= width) {
            printf("Invalid value \"%s\" for %d byte width\n", arg, width);
            return (RC_FAILURE);
        }

        if (digit > 0xf) {
            printf("Invalid digit '%c' in \"%s\"\n", ch, arg);
            return (RC_FAILURE);
        }

        if (pos & 1)
            value[bytepos] |= (digit << 4);
        else
            value[bytepos] = digit;
    }

    pos++;
    pos >>= 1;

#ifdef IS_BIG_ENDIAN
    while (pos < width)
        value[width - 1 - pos++] = 0x00;
#else
    while (pos < width)
        value[pos++] = 0x00;
#endif

    return (RC_SUCCESS);
}

static rc_t
parse_uint(const char *arg, uint *value)
{
    int pos = 0;
    unsigned int x;

    if ((sscanf(arg, "%x%n", &x, &pos) != 1) ||
        ((arg[pos] != '\0') && (arg[pos] != ' ')) ||
        (pos > (int)(sizeof (x) * 2))) {
        printf("Invalid value \"%s\"\n", arg);
        return (RC_FAILURE);
    }
    *value = x;
    return (RC_SUCCESS);
}

static rc_t
parse_width(const char *arg, uint *width, char *other, size_t other_size)
{
    size_t other_count = 0;
    const char *ptr;
    *width = 0;
    *other = '\0';

    for (ptr = arg; *ptr != '\0'; ptr++) {
        switch (*ptr) {
            case 'b':
                *width = 1;
                break;
            case 'w':
                *width = 2;
                break;
            case 'l':
                *width = 4;
                break;
            case 'q':
                *width = 8;
                break;
            case 'o':
                *width = 16;
                break;
            case 'h':
                *width = 32;
                break;
            default:
                if (isdigit((uint8_t)*ptr)) {
                    while (isdigit((uint8_t)*ptr)) {
                        *width *= 10;
                        *width += *ptr - '0';
                        ptr++;
                    }
                    if (*ptr == '\0')
                        ptr--;
                } else {
                    other[other_count++] = *ptr;
                    if (other_count >= other_size) {
                        printf("Argument \"%s\" too long\n", arg);
                        return (RC_FAILURE);
                    }
                    other[other_count] = '\0';
                }
        }
    }

    /* Default to 32 bits */
    if (*width == 0)
        *width = 4;

    if (*width > MAX_TRANSFER) {
        printf("Invalid width %u bytes (maximum %u)\n", *width, MAX_TRANSFER);
        return (RC_FAILURE);
    }

    return (RC_SUCCESS);
}

rc_t
cmd_c(int argc, char * const *argv)
{
    rc_t     rc;
    int      arg;
    char    *ptr;
    uint64_t addr;
    uint64_t space;
    uint     width;
    uint     offset;
    char     other[32];
    bool_t   flag_S = FALSE;
    uint8_t  buf[MAX_TRANSFER];

    if (argc <= 2) {
        printf("c requires arguments: <address> [<values...>]\n");
        return (RC_USER_HELP);
    }
    rc = parse_width(argv[0] + 1, &width, other, sizeof (other));
    if (rc != RC_SUCCESS)
        return (RC_USER_HELP);

    for (ptr = other; *ptr != '\0'; ptr++) {
        switch (*ptr) {
            case 's':
            case 'S':
                flag_S = TRUE;
                break;
            default:
                printf("Unknown flag \"%s\"\n", ptr);
                return (RC_USER_HELP);
        }
    }

    argc--;
    argv++;
    if ((rc = parse_addr(&argv, &argc, &space, &addr)) != RC_SUCCESS)
        return (RC_USER_HELP);
    if (argc < 1) {
        printf("c requires arguments: <address> [<values...>]\n");
        return (RC_USER_HELP);
    }

    offset = 0;
    for (arg = 0; arg < argc; arg++) {
        if ((rc = parse_value(argv[arg], buf, width)) != RC_SUCCESS)
            return (rc);
        if (flag_S) {
            uint pos;
            for (pos = 0; pos < width / 2; pos++) {
                uint8_t temp = buf[pos];
                buf[pos] = buf[width - 1 - pos];
                buf[width - 1 - pos] = temp;
            }
        }
        rc = data_write(space, addr + offset, width, buf);
        if (rc != RC_SUCCESS) {
            printf("Error writing %d bytes at ", width);
            print_addr(space, addr + offset);
            printf("\n");
            return (rc);
        }
        offset += width;
    }
    return (RC_SUCCESS);
}

static char const *
skip(const char *str, const char *skip)
{
    while ((*str == *skip) && (*skip != '\0')) {
        str++;
        skip++;
    }
    return (str);
}

rc_t
cmd_comp(int argc, char * const *argv)
{
    rc_t        rc;
    uint        width;
    uint64_t    addr1;
    uint64_t    space1;
    uint64_t    addr2;
    uint64_t    space2;
    uint        len;
    uint        offset;
    uint        temp;
    char        other[32];
    uint8_t     buf1[MAX_TRANSFER];
    uint8_t     buf2[MAX_TRANSFER];
    bool_t      printed = FALSE;
    bool_t      flag_A = FALSE;
    const char *cmd;
    const char *ptr;
    uint        mismatch_count = 0;

    if (argc < 4) {
        printf("compare requires three arguments: <addr1> <addr2> <len>\n");
        return (RC_USER_HELP);
    }
    cmd = skip(argv[0], "compare");
    rc = parse_width(cmd, &width, other, sizeof (other));
    if (rc != RC_SUCCESS)
        return (RC_USER_HELP);
    for (ptr = other; *ptr != '\0'; ptr++) {
        switch (*ptr) {
            case 'a':
            case 'A':
                flag_A = TRUE;
                break;
            default:
                printf("Unknown flag \"%s\"\n", ptr);
                return (RC_USER_HELP);
        }
    }
    argc--;
    argv++;
    if ((rc = parse_addr(&argv, &argc, &space1, &addr1)) != RC_SUCCESS)
        return (RC_USER_HELP);
    if (argc < 2) {
        printf("compare requires three arguments: <addr1> <addr2> <len>\n");
        return (RC_USER_HELP);
    }
    if ((rc = parse_addr(&argv, &argc, &space2, &addr2)) != RC_SUCCESS)
        return (RC_USER_HELP);
    if (argc != 1) {
        printf("compare requires three arguments: <addr1> <addr2> <len>\n");
        return (RC_USER_HELP);
    }
    if ((rc = parse_uint(argv[0], &len)) != RC_SUCCESS)
        return (RC_USER_HELP);

    for (offset = 0; offset < len; offset += width) {
        rc = data_read(space1, addr1 + offset, width, buf1);
        if (rc != RC_SUCCESS) {
            if (printed)
                printf("\n");
            printf("Error reading %d bytes at ", width);
            print_addr(space1, addr1 + offset);
            printf("\n");
            return (rc);
        }
        rc = data_read(space2, addr2 + offset, width, buf2);
        if (rc != RC_SUCCESS) {
            if (printed)
                printf("\n");
            printf("Error reading %d bytes at ", width);
            print_addr(space2, addr2 + offset);
            printf("\n");
            return (rc);
        }
        if (memcmp(buf1, buf2, width) != 0) {
            if ((mismatch_count++ < 8) || (flag_A)) {
                printf("mismatch ");
                print_addr(space1, addr1 + offset);
                printf(" ");
                for (temp = 0; temp < width; temp++)
                    printf("%02x", buf1[temp]);
                printf(" != ");
                print_addr(space2, addr2 + offset);
                printf(" ");
                for (temp = 0; temp < width; temp++)
                    printf("%02x", buf2[temp]);
                printf("\n");
            }
        }
        if (input_break_pending()) {
            printf("^C\n");
            return (RC_USR_ABORT);
        }
    }
    if (mismatch_count > 0) {
        printf("%d mismatches\n", mismatch_count);
        return (RC_FAILURE);
    }
    return (RC_SUCCESS);
}

rc_t
cmd_copy(int argc, char * const *argv)
{
    rc_t        rc;
    uint        width;
    uint64_t    saddr;
    uint64_t    sspace;
    uint64_t    daddr;
    uint64_t    dspace;
    uint        len;
    uint        offset;
    char        other[32];
    uint8_t     buf[MAX_TRANSFER];
    const char *cmd;

    if (argc < 4) {
        printf("copy requires three arguments: <saddr> <daddr> <len>\n");
        return (RC_USER_HELP);
    }
    cmd = skip(argv[0], "copy");
    rc = parse_width(cmd, &width, other, sizeof (other));
    if (rc != RC_SUCCESS)
        return (RC_USER_HELP);
    argc--;
    argv++;
    if ((rc = parse_addr(&argv, &argc, &sspace, &saddr)) != RC_SUCCESS)
        return (RC_USER_HELP);
    if (argc < 2) {
        printf("copy requires three arguments: <saddr> <daddr> <len>\n");
        return (RC_USER_HELP);
    }
    if ((rc = parse_addr(&argv, &argc, &dspace, &daddr)) != RC_SUCCESS)
        return (RC_USER_HELP);
    if (argc != 1) {
        printf("copy requires three arguments: <saddr> <daddr> <len>\n");
        return (RC_USER_HELP);
    }
    if ((rc = parse_uint(argv[0], &len)) != RC_SUCCESS)
        return (RC_USER_HELP);

    for (offset = 0; offset < len; offset += width) {
        rc = data_read(sspace, saddr + offset, width, buf);
        if (rc != RC_SUCCESS) {
            printf("Error reading %d bytes at ", width);
            print_addr(sspace, saddr + offset);
            printf("\n");
            return (rc);
        }
        rc = data_write(dspace, daddr + offset, width, buf);
        if (rc != RC_SUCCESS) {
            printf("Error writing %d bytes at ", width);
            print_addr(dspace, daddr + offset);
            printf("\n");
            return (rc);
        }
        if (input_break_pending()) {
            printf("^C\n");
            return (RC_USR_ABORT);
        }
    }
    return (RC_SUCCESS);
}

static char
printable_ascii(uint8_t ch)
{
    if (ch >= ' ' && ch <= '~')
        return (ch);
    if (ch == '\t' || ch == '\r' || ch == '\n' || ch == '\0')
        return (' ');
    return ('.');
}

static void
cmd_d_conv_printable(char *buf, const uint8_t *data, uint64_t len, bool_t swap)
{
    uint pos = 0;

    for (pos = 0; pos < len; pos++) {
        if (swap)
            buf[len - pos - 1] = printable_ascii(data[pos]);
        else
            buf[pos] = printable_ascii(data[pos]);
    }
}

rc_t
cmd_d(int argc, char * const *argv)
{
    rc_t     rc;
    char    *ptr;
    uint     width;
    uint     temp;
    uint     len = 64;
    char     other[32];
    char     charbuf[MAX_TRANSFER];
    uint     charpos = 0;
    uint     offset;
    uint     per_line;
    uint8_t  buf[MAX_TRANSFER];
    uint64_t addr;
    uint64_t space;
    bool_t   flag_A  = FALSE;  /* Don't show ASCII */
    bool_t   flag_N  = FALSE;  /* Don't print */
    bool_t   flag_R  = FALSE;  /* Raw (only data values) */
    bool_t   flag_S  = FALSE;  /* Swap endian */
    bool_t   flag_SS = FALSE;  /* Swap endian of ASCII */
    bool_t   printed = FALSE;

#ifndef IS_BIG_ENDIAN
    flag_S = TRUE;
#endif

    if (argc <= 1) {
        printf("This command requires an argument: <address>\n");
        return (RC_USER_HELP);
    }
    rc = parse_width(argv[0] + 1, &width, other, sizeof (other));
    if (rc != RC_SUCCESS)
        return (RC_USER_HELP);
    for (ptr = other; *ptr != '\0'; ptr++) {
        switch (*ptr) {
            case 'a':
            case 'A':
                flag_A = TRUE;
                break;
            case 'n':
            case 'N':
                flag_N = TRUE;
                break;
            case 'r':
            case 'R':
                flag_R = TRUE;
                break;
            case 's':
            case 'S':
                if (flag_S)
                    flag_SS = TRUE;
                flag_S = !flag_S;
                break;
            default:
                printf("Unknown flag \"%s\"\n", ptr);
                return (RC_USER_HELP);
        }
    }
    argv++;
    argc--;
    if ((rc = parse_addr(&argv, &argc, &space, &addr)) != RC_SUCCESS)
        return (RC_USER_HELP);

    if (argc > 1) {
        printf("This command supports two arguments: <address> [<length]>\n");
        return (RC_USER_HELP);
    }
    if (argc >= 1) {
        if ((rc = parse_uint(argv[0], &len)) != RC_SUCCESS)
            return (RC_USER_HELP);
    }

    if (width > sizeof (buf))
        width = sizeof (buf);
    switch (width) {
        case 1:
            per_line = 16;
            break;
        case 2:
            per_line = 16;
            break;
        case 4:
            per_line = 16;
            break;
        case 8:
            per_line = 16;
            break;
        default:
            per_line = 16;
            break;
    }
    if (flag_R)
        flag_A = TRUE;

    for (offset = 0; offset < len; offset += width) {
        if (!flag_N && !flag_R && ((offset % per_line) == 0)) {
            if (!flag_A && (offset != 0)) {
                /* Display ASCII */
                printf(" %.*s", charpos, charbuf);
                charpos = 0;
                printed = TRUE;
            }
            if (printed)
                printf("\n");
            print_addr(space, addr + offset);
            printf(":");
            printed = TRUE;
        }
        rc = data_read(space, addr + offset, width, buf);
        if (rc != RC_SUCCESS) {
            if (printed)
                printf("\n");
            printf("Error reading %d bytes at ", width);
            print_addr(space, addr + offset);
            printf("\n");
            return (rc);
        }
        if (flag_N) {
            if (input_break_pending()) {
                printf("^C\n");
                return (RC_USR_ABORT);
            }
            continue;  /* Don't print -- just fetch */
        }

        if (!flag_R || (offset > 0))
            putchar(' ');
        if (flag_S) {
            for (temp = 0; temp < width; temp++)
                printf("%02x", buf[width - temp - 1]);
        } else {
            for (temp = 0; temp < width; temp++)
                printf("%02x", buf[temp]);
        }
        printed = TRUE;

        if (!flag_A) {
            cmd_d_conv_printable(charbuf + charpos, buf, width, flag_SS);
            charpos += width;
        }
    }
    if (!flag_N && !flag_A && (offset != 0)) {
        /* Display ASCII */
        if (per_line > charpos) {
            uint chars_missing = per_line - charpos;
            uint spaces_missing = chars_missing * 2 + chars_missing / width;
            printf("%*s", spaces_missing, "");
        }
        printf(" %.*s", charpos, charbuf);
        charpos = 0;
        printed = TRUE;
    }
    if (!flag_N && printed)
        printf("\n");
    return (RC_SUCCESS);
}

rc_t
cmd_echo(int argc, char * const *argv)
{
    int arg;
    for (arg = 1; arg < argc; arg++) {
        if (arg > 1)
            printf(" ");
        printf("%s", argv[arg]);
    }
    printf("\n");
    return (RC_SUCCESS);
}

rc_t
cmd_ignore(int argc, char * const *argv)
{
    if (argc <= 1) {
        printf("error: ignore command requires command to execute\n");
        return (RC_USER_HELP);
    }
    (void) cmd_exec_argv(argc - 1, argv + 1);
    return (RC_SUCCESS);
}

/* Remove surrounding quotes, if present */
static char *
remove_quotes(char *line)
{
    char *ptr = line;
    if (*ptr == '"') {
        int len = strlen(line) - 1;
        if (len >= 0 && ptr[len] == '"') {
            ptr[len] = '\0';
            ptr++;
        }
    }
    return (ptr);
}

/*
 * XXX: Eventually make eval_cmdline_expr() able to evaluate variables
 *      directly (including index variables). That would eliminate the
 *      need for this function.
 */
static char *
loop_index_substitute(char *src, int value, int count)
{
    char   valbuf[10];
    int    vallen  = sprintf(valbuf, "%x", value);
    size_t newsize = strlen(src) + vallen * count + 1;
    char  *nstr    = malloc(newsize);
    char  *dptr    = nstr;

    if (nstr == NULL)
        return (strdup(src));

    while (*src != '\0') {
        char *ptr = strstr(src, "$a");
        int   len;
        if (ptr == NULL) {
            /* Nothing more */
            strcpy(dptr, src);
            break;
        }
        len = ptr - src;
        memcpy(dptr, src, len);
        src = ptr + 2;
        dptr += len;
        strcpy(dptr, valbuf);
        dptr += vallen;
    }
    dptr = eval_cmdline_expr(nstr);
    free(nstr);

    return (dptr);
}

static int
loop_index_count(char *src)
{
    int   count = 0;
    while (*src != '\0') {
        src = strstr(src, "$a");
        if (src == NULL)
            break;
        count++;
        src++;
    }
    return (count);
}

rc_t
cmd_loop(int argc, char * const *argv)
{
    int     count;
    int     cur;
    int     nargc = 0;
    int     index_uses;
    char   *nargv[128];
    char   *ptr;
    char   *cmd;
    char   *cmdline;
    rc_t    rc = RC_SUCCESS;

    if (argc <= 2) {
        printf("error: loop command requires count and command to execute\n");
        return (RC_USER_HELP);
    }
    if ((rc = scan_int(argv[1], &count)) != RC_SUCCESS)
        return (rc);
    cmdline = cmd_string_from_argv(argc - 2, argv + 2);
    if (cmdline == NULL)
        return (RC_FAILURE);
    cmd = remove_quotes(cmdline);
    index_uses = loop_index_count(cmd);
    if (index_uses == 0)
        nargc = make_arglist(cmd, nargv);

    for (cur = 0; cur < count; cur++) {
        if (index_uses > 0) {
            if (cur != 0)
                free_arglist(nargc, nargv);
            ptr = loop_index_substitute(cmd, cur, index_uses);
            nargc = make_arglist(ptr, nargv);
            free(ptr);
        }
        rc = cmd_exec_argv(nargc, nargv);
        if (rc != RC_SUCCESS) {
            if (rc == RC_USER_HELP)
                rc = RC_FAILURE;
            goto finish;
        }
        if (input_break_pending()) {
            printf("^C\n");
            return (RC_USR_ABORT);
        }
    }
finish:
    free(cmdline);
    free_arglist(nargc, nargv);
    return (rc);
}

rc_t
cmd_history(int argc, char * const *argv)
{
    history_show();
    return (RC_SUCCESS);
}

static rc_t
convert_name_to_time_units(const char *arg, int *units)
{
    int len = strlen(arg);
    if (len == 0)
        return (RC_FAILURE);
    if (strncmp(arg, "sec", len) == 0) {
        *units = 0;
    } else if (strncmp(arg, "minutes", len) == 0) {
        *units = 1;
    } else if (strncmp(arg, "hours", len) == 0) {
        *units = 2;
    } else if ((strncmp(arg, "ms", len) == 0) ||
               (strncmp(arg, "milliseconds", len) == 0)) {
        *units = -1;
    } else if ((strncmp(arg, "useconds", len) == 0) ||
               (strncmp(arg, "microseconds", len) == 0)) {
        *units = -2;
    } else if ((strncmp(arg, "nseconds", len) == 0) ||
               (strncmp(arg, "nanoseconds", len) == 0)) {
        *units = -3;
    } else {
        return (RC_FAILURE);
    }
    return (RC_SUCCESS);
}

rc_t
cmd_delay(int argc, char * const *argv)
{
    int  value = 0;
    int  units = 0; /* default: seconds */
    int  pos   = 0;
    int  count;
    char *ptr;

    if (argc <= 1) {
        printf("This command requires an argument: <time>\n");
        return (RC_USER_HELP);
    }
    if (argc > 3) {
        printf("This command requires at most: <time> <h|m|s|ms|us>\n");
        return (RC_USER_HELP);
    }
    for (ptr = argv[1]; *ptr != '\0'; ptr++) {
        if (convert_name_to_time_units(ptr, &units) == RC_SUCCESS) {
            *ptr = '\0';
            break;
        }
    }

    if ((sscanf(argv[1], "%i%n", &value, &pos) != 1) ||
        (argv[1][pos] != '\0')) {
        printf("Invalid value \"%s\"\n", argv[1]);
        return (RC_BAD_PARAM);
    }

    if (argc > 2) {
        if (convert_name_to_time_units(argv[2], &units) != RC_SUCCESS) {
            printf("Unknown units: %s\n", argv[2]);
            return (RC_USER_HELP);
        }
    }

    switch (units) {
        case 2:  /* hours */
            for (count = 0; count < 3600 * value; count++) {
                sleep(1);
                if (input_break_pending()) {
                    printf("^C\n");
                    return (RC_USR_ABORT);
                }
            }
            break;
        case 1:  /* minutes */
            for (count = 0; count < 60 * value; count++) {
                sleep(1);
                if (input_break_pending()) {
                    printf("^C\n");
                    return (RC_USR_ABORT);
                }
            }
            break;
        case 0:  /* seconds */
            for (count = 0; count < value; count++) {
                sleep(1);
                if (input_break_pending()) {
                    printf("^C\n");
                    return (RC_USR_ABORT);
                }
            }
            break;
        case -1:  /* milliseconds */
            while (value > 1000) {
                sleep(1);
                if (input_break_pending()) {
                    printf("^C\n");
                    return (RC_USR_ABORT);
                }
                value -= 1000;
            }
            usleep(value * 1000);
            break;
        case -2:  /* microseconds */
            usleep(value);
            break;
        case -3:  /* nanoseconds */
            usleep(value / 1000);
            break;
    }
    return (RC_SUCCESS);
}

rc_t
cmd_patt(int argc, char * const *argv)
{
    rc_t        rc;
    uint        step = 0;
    uint        alt = 0;
    uint        width;
    uint64_t    addr;
    uint64_t    space;
    uint        len;
    uint        offset;
    char        other[32];
    bool_t      flag_S = FALSE;
    uint8_t     buf[MAX_TRANSFER];
    const char *cmd;
    char       *ptr;
    static enum {
        PATT_ZERO,
        PATT_ONE,
        PATT_BLIP,
        PATT_RAND,
        PATT_STROBE,
        PATT_WALK0,
        PATT_WALK1,
        PATT_VALUE,
    } pattmode = PATT_ONE;

    if (argc < 4) {
        printf("Need address\n");
        printf("patt requires three arguments: <addr> <len> <pattern>\n");
        return (RC_USER_HELP);
    }
    cmd = skip(argv[0], "pattern");
    rc = parse_width(cmd, &width, other, sizeof (other));
    if (rc != RC_SUCCESS)
        return (RC_USER_HELP);

    for (ptr = other; *ptr != '\0'; ptr++) {
        switch (*ptr) {
            case 's':
            case 'S':
                flag_S = TRUE;
                break;
            default:
                printf("Unknown flag \"%s\"\n", ptr);
                return (RC_USER_HELP);
        }
    }

    argc--;
    argv++;
    if ((rc = parse_addr(&argv, &argc, &space, &addr)) != RC_SUCCESS)
        return (RC_USER_HELP);
    if (argc < 2) {
        printf("Need length\n");
        printf("patt requires three arguments: <addr> <len> <pattern>\n");
        return (RC_USER_HELP);
    }
/* XX: support swap */
    if ((rc = parse_uint(argv[0], &len)) != RC_SUCCESS)
        return (RC_USER_HELP);
    argc--;
    argv++;
    if (argc != 1) {
        printf("Need pattern\n");
        printf("patt requires three arguments: <addr> <len> <pattern>\n");
show_patterns:
        printf(cmd_patt_patterns);
        return (RC_USER_HELP);
    }
    if (strcmp(argv[0], "?") == 0) {
        printf(cmd_patt_patterns);
        return (RC_FAILURE);
    } else if (strcmp(argv[0], "one") == 0) {
        pattmode = PATT_ONE;
        memset(buf, 0xff, width);
    } else if (strcmp(argv[0], "zero") == 0) {
        pattmode = PATT_ZERO;
        memset(buf, 0x00, width);
    } else if (strcmp(argv[0], "blip") == 0) {
        pattmode = PATT_BLIP;
        memset(buf, 0x00, width);
    } else if (strcmp(argv[0], "rand") == 0) {
        pattmode = PATT_RAND;
        srand32(time(NULL));
    } else if (strcmp(argv[0], "strobe") == 0) {
        pattmode = PATT_STROBE;
        memset(buf, 0x00, width);
    } else if (strcmp(argv[0], "walk0") == 0) {
        pattmode = PATT_WALK0;
        memset(buf, 0xff, width);
    } else if (strcmp(argv[0], "walk1") == 0) {
        pattmode = PATT_WALK1;
        memset(buf, 0x00, width);
    } else {
        if ((rc = parse_value(argv[0], buf, width)) != RC_SUCCESS) {
            printf("Invalid pattern %s\n", argv[0]);
            goto show_patterns;
        }
        pattmode = PATT_VALUE;
    }

    for (offset = 0; offset < len; offset += width) {
        switch (pattmode) {
            case PATT_WALK0: {
                int pos  = (step >> 3) & (width - 1);
                int opos = ((step - 1) >> 3) & (width - 1);
                if (flag_S) {
                    pos  = width - 1 - pos;
                    opos = width - 1 - opos;
                }
                buf[pos] = ~(1 << (step & 7));
                if (pos != opos)
                    buf[opos] = 0xff;
                break;
            }
            case PATT_WALK1: {
                int pos  = (step >> 3) & (width - 1);
                int opos = ((step - 1) >> 3) & (width - 1);
                if (flag_S) {
                    pos  = width - 1 - pos;
                    opos = width - 1 - opos;
                }
                buf[pos] = (1 << (step & 7));
                if (pos != opos)
                    buf[opos] = 0x00;
                break;
            }
            case PATT_RAND: {
                uint swidth;
                for (swidth = 0; swidth < width; swidth += 4)
                    *(uint32_t *) (buf + swidth) = rand32();
                break;
            }
            case PATT_STROBE: {
                /* Just alternate between all on and all off */
                if (step & 1)
                    memset(buf, 0xff, width);
                else
                    memset(buf, 0x00, width);
                break;
            }
            case PATT_BLIP: {
                /*
                 * Cycle through several iterations of all bits one state,
                 * with a single iteration where they all flip.
                 */

                if ((step & 7) >= 5) {
                    int set_high = ((step & 8) == 0) ^ (step & 1);
                    if (set_high)
                        memset(buf, 0xff, width);
                    else
                        memset(buf, 0x00, width);
                }
                if (alt++ == 23) {
                    alt = 0;
                    step++; /* Skew the step, so it doesn't remain aligned */
                }
                break;
            }
            default:
                break;
        }
        rc = data_write(space, addr + offset, width, buf);
        if (rc != RC_SUCCESS) {
            printf("Error writing %d bytes at ", width);
            print_addr(space, addr + offset);
            printf("\n");
            return (rc);
        }
        step++;
        if (input_break_pending()) {
            printf("^C\n");
            return (RC_USR_ABORT);
        }
    }
    return (RC_SUCCESS);
}

rc_t
cmd_test(int argc, char * const *argv)
{
    rc_t        rc;
    uint        width;
    uint        count = 0;
    uint64_t    addr;
    uint64_t    space;
    uint        len;
    uint        offset;
    char        other[32];
    uint8_t     buf[MAX_TRANSFER];
    uint8_t     rbuf[MAX_TRANSFER];
    uint32_t    srand_seed;
    const char *cmd;
    static enum {
        TEST_ZERO,
        TEST_ONE,
        TEST_RAND,
        TEST_WALK0,
        TEST_WALK1,
        TEST_VALUE,
    } testmode = TEST_VALUE;
    static enum {
        RWMODE_READ,
        RWMODE_WRITE,
    } rwmode = RWMODE_READ;
    bool_t      flag_S = FALSE;

    if (argc < 4) {
        printf("%d: test requires three arguments: <addr> <len> <mode>\n",
               argc);
        return (RC_USER_HELP);
    }
    cmd = skip(argv[0], "test");
    rc = parse_width(cmd, &width, other, sizeof (other));
    if (rc != RC_SUCCESS)
        return (RC_USER_HELP);
    argc--;
    argv++;
    if ((rc = parse_addr(&argv, &argc, &space, &addr)) != RC_SUCCESS)
        return (RC_USER_HELP);
    if (argc < 2) {
        printf("%d: test requires three arguments: <addr> <len> <mode>\n",
               argc + 1);
        return (RC_USER_HELP);
    }
    if ((rc = parse_uint(argv[0], &len)) != RC_SUCCESS)
        return (RC_USER_HELP);
    argc--;
    argv++;
    if (argc > 1) {
        printf("%d: test requires three arguments: <addr> <len> <mode>\n",
               argc + 2);
show_patterns:
        printf(cmd_test_patterns);
        return (RC_USER_HELP);
    }
    if (argc == 1) {
        rwmode = RWMODE_WRITE;
        if (strcmp(argv[0], "?") == 0) {
            printf(cmd_test_patterns);
        } else if (strcmp(argv[0], "one") == 0) {
            testmode = TEST_ONE;
            memset(buf, 0xff, width);
        } else if (strcmp(argv[0], "read") == 0) {
            rwmode = RWMODE_READ;
        } else if (strcmp(argv[0], "rand") == 0) {
            testmode = TEST_RAND;
            srand_seed = time(NULL);
            srand32(srand_seed);
        } else if (strcmp(argv[0], "walk0") == 0) {
            testmode = TEST_WALK0;
            memset(buf, 0x00, width);
        } else if (strcmp(argv[0], "walk1") == 0) {
            testmode = TEST_WALK1;
            memset(buf, 0xff, width);
        } else if (strcmp(argv[0], "zero") == 0) {
            testmode = TEST_ZERO;
            memset(buf, 0x00, width);
        } else {
            testmode = TEST_VALUE;
            if ((rc = parse_value(argv[0], buf, width)) != RC_SUCCESS) {
                printf("Invalid mode %s\n", argv[0]);
                goto show_patterns;
            }
        }
    } else {
        rwmode = RWMODE_READ;
    }

    for (offset = 0; offset < len; offset += width) {
        count++;
        if (rwmode == RWMODE_WRITE) {
            uint step = 0;
            switch (testmode) {
                case TEST_RAND: {
                    uint swidth;
                    for (swidth = 0; swidth < width; swidth += 4)
                        *(uint32_t *) (buf + swidth) = rand32();
                    break;
                }
                case TEST_WALK0: {
                    int pos  = (step >> 3) & (width - 1);
                    int opos = ((step - 1) >> 3) & (width - 1);
                    if (flag_S) {
                        pos  = width - 1 - pos;
                        opos = width - 1 - opos;
                    }
                    buf[pos] = ~(1 << (step & 7));
                    if (pos != opos)
                        buf[opos] = 0xff;
                    break;
                }
                case TEST_WALK1: {
                    int pos  = (step >> 3) & (width - 1);
                    int opos = ((step - 1) >> 3) & (width - 1);
                    if (flag_S) {
                        pos  = width - 1 - pos;
                        opos = width - 1 - opos;
                    }
                    buf[pos] = (1 << (step & 7));
                    if (pos != opos)
                        buf[opos] = 0x00;
                    break;
                }
                default:
                    break;
            }
            count++;
            rc = data_write(space, addr + offset, width, buf);
            if (rc != RC_SUCCESS) {
                printf("Error writing %d bytes at ", width);
                print_addr(space, addr + offset);
                printf("\n");
                return (rc);
            }
        }
        rc = data_read(space, addr + offset, width, rbuf);
        if (rc != RC_SUCCESS) {
            printf("Error reading %d bytes at ", width);
            print_addr(space, addr + offset);
            printf("\n");
            return (rc);
        }
        (void) testmode;
        if (input_break_pending()) {
            printf("^C\n");
            return (RC_USR_ABORT);
        }
    }
    return (RC_SUCCESS);
}

rc_t
cmd_version(int argc, char * const *argv)
{
    printf("%s\n", version_str);
    return (RC_SUCCESS);
}

#ifdef AMIGA
/* XXX: this should go in cmds_amiga.c */
#include <time.h>
#include <clib/timer_protos.h>
#define TICKS_PER_MINUTE (TICKS_PER_SECOND * 60)
#define MINUTES_PER_DAY  (24 * 60)
#define MS_PER_TICK (1000 / TICKS_PER_SECOND)

static uint64_t
diff_dstamp(struct DateStamp *ds1, struct DateStamp *ds2)
{
    struct DateStamp ds_val;
    ds_val.ds_Days   = ds1->ds_Days   - ds2->ds_Days;
    ds_val.ds_Minute = ds1->ds_Minute - ds2->ds_Minute;
    ds_val.ds_Tick   = ds1->ds_Tick   - ds2->ds_Tick;
    if (ds_val.ds_Tick < 0) {
        ds_val.ds_Tick += TICKS_PER_MINUTE;
        ds_val.ds_Minute--;
    }
    if (ds_val.ds_Minute < 0) {
        ds_val.ds_Minute += MINUTES_PER_DAY;
        ds_val.ds_Days--;
    }
    return ((uint64_t) ds_val.ds_Tick * MS_PER_TICK +
            (uint64_t) ds_val.ds_Minute * 60 * 1000 +
            (uint64_t) ds_val.ds_Days * 24 * 60 * 60 * 1000);
}

rc_t
cmd_time(int argc, char * const *argv)
{
    uint64_t time_diff;
    struct DateStamp stime;
    struct DateStamp etime;
    rc_t     rc;

    if ((argc <= 2) || (strcmp(argv[1], "cmd") != 0)) {
        printf("error: time command requires cmd and command to execute\n");
        return (RC_USER_HELP);
    }
    argv += 2;
    argc -= 2;

    DateStamp(&stime);
    rc = cmd_exec_argv(argc, argv);
    DateStamp(&etime);
    time_diff = diff_dstamp(&etime, &stime);
    printf("%lld ms\n", diff_dstamp(&etime, &stime));
    if (rc == RC_USER_HELP)
        rc = RC_FAILURE;

    return (rc);
}
#elif !defined(EMBEDDED_CMD)  /* UNIX */
/* XXX: this should go in cmds_unix.c */
#include <sys/time.h>

static uint64_t
diff_timeofday(struct timeval *tv1, struct timeval *tv2)
{
    struct timeval tv;
    tv.tv_sec = tv1->tv_sec - tv2->tv_sec;
    if (tv1->tv_usec > tv2->tv_usec) {
        tv.tv_usec = tv1->tv_usec - tv2->tv_usec;
    } else {
        tv.tv_usec = tv1->tv_usec - tv2->tv_usec + 1000000;
        tv.tv_sec--;
    }
    return ((uint64_t) tv.tv_usec + (uint64_t) tv.tv_sec * 1000000);
}

rc_t
cmd_time(int argc, char * const *argv)
{
    struct timezone tz;
    rc_t            rc;

    if (argc <= 1)
        return (RC_USER_HELP);

    if (strncmp(argv[1], "cmd", 1) == 0) {
        struct timeval  stime;
        struct timeval  etime;
        if (argc <= 2) {
            printf("error: time cmd requires command to execute\n");
            return (RC_USER_HELP);
        }
        gettimeofday(&stime, &tz);
        rc = cmd_exec_argv(argc - 2, argv + 2);
        gettimeofday(&etime, &tz);
        printf("%lld us\n", (long long) diff_timeofday(&etime, &stime));
        if (rc == RC_USER_HELP)
            rc = RC_FAILURE;
    } else if (strncmp(argv[1], "now", 1) == 0) {
        struct timeval now;
        gettimeofday(&now, &tz);
        printf("now=%lu.%06lu sec.usec\n", now.tv_sec, now.tv_usec);
        rc = RC_SUCCESS;
    } else {
        printf("Unknown argument %s\n", argv[1]);
        return (RC_USER_HELP);
    }
    return (rc);
}
#endif
