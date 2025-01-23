/*
 * smash
 * ------
 * Utility to perform various operations with Kicksmash installed in
 * an Amiga.
 *
 * Copyright 2024 Chris Hooper. This program and source may be used
 * and distributed freely, for any purpose which benefits the Amiga
 * community. Commercial use of the binary, source, or algorithms requires
 * prior written approval from Chris Hooper <amiga@cdh.eebugs.com>.
 * All redistributions must retain this Copyright notice.
 *
 * DISCLAIMER: THE SOFTWARE IS PROVIDED "AS-IS", WITHOUT ANY WARRANTY.
 * THE AUTHOR ASSUMES NO LIABILITY FOR ANY DAMAGE ARISING OUT OF THE USE
 * OR MISUSE OF THIS UTILITY OR INFORMATION REPORTED BY THIS UTILITY.
 */
const char *version = "\0$VER: smashftp 1.1 ("__DATE__") © Chris Hooper";

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <clib/dos_protos.h>
#include <clib/timer_protos.h>
#include <time.h>
#include <fcntl.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include "smash_cmd.h"
#include "host_cmd.h"
#include "smashftp_cli.h"
#include "smashftp.h"
#include "crc32.h"
#include "readline.h"
#include "cpu_control.h"
#include "sm_msg.h"
#include "sm_file.h"

/*
 * gcc clib2 headers are bad (for example, no stdint definitions) and are
 * not being included by our build.  Because of that, we need to fix up
 * some stdio definitions.
 */
extern struct iob ** __iob;
#undef stdout
#define stdout ((FILE *)__iob[1])

#define VALUE_UNASSIGNED 0xffffffff

#define DIRBUF_SIZE 2000

#ifdef _DCC
typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned long  uint32_t;
typedef struct { unsigned long hi; unsigned long lo; } uint64_t;
#define __packed
#else
struct ExecBase *DOSBase;
struct Device   *TimerBase;
#endif

static BPTR save_currentdir = 0;
static char *cwd = NULL;
static handle_t cwd_handle = 0xffffffff;
BOOL __check_abort_enabled = 0;       // Disable gcc clib2 ^C break handling
uint flag_debug = 0;
uint8_t sm_file_active = 0;

static const char cmd_get_help[] =
"Usage:\n"
"    get [path/]<name>               - get file from remote and keep name\n"
"    get [path/]<name> <localname>   - get file from remote & rename locally\n"
"    get [path/]<name> <localdir>    - get file from remote to local dir\n"
"    get <name1> <name2> <name3...>  - get multiple files from remote\n"
;

static const char cmd_put_help[] =
"Usage:\n"
"    put [path/]<name>               - send file to remote and keep name\n"
"    put [path/]<name> <remotename>  - send file to remote & rename\n"
"    put [path/]<name> <remotedir>   - send file from local to remote dir\n"
"    put <name1> <name2> <name3...>  - send multiple files to remote dir\n"
;

const char cmd_time_help[] =
"time cmd <cmd> - measure command execution time\n"
;

static const char *const hmd_types[] = {
    "Unknown", "File", "Dir", "Link", "HLink", "BlockDev", "CharDev",
    "FIFO", "Socket", "Whiteout", "Volume", "VolumeDir"
};
STATIC_ASSERT(ARRAY_SIZE(hmd_types) == HM_TYPE_LAST_ENTRY);

/*
 * is_user_abort
 * -------------
 * Check for user break input (^C)
 */
static BOOL
is_user_abort(void)
{
    if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
        return (1);
    return (0);
}

/*
 * clear_user_abort
 * ----------------
 * Clear any pending user abort signal.
 */
void
clear_user_abort(void)
{
    SetSignal(0, SIGBREAKF_CTRL_C);
}

static uint64_t
smash_time(void)
{
    uint64_t usecs;
    if (send_cmd(KS_CMD_UPTIME, NULL, 0, &usecs, sizeof (usecs), NULL))
        return (0);
    return (usecs);
}

#if 0
static void
print_us_diff(uint64_t start, uint64_t end)
{
    uint64_t diff = end - start;
    uint32_t diff2;
    char *scale = "ms";

    if ((diff >> 32) != 0)  // Finished before started?
        diff = 0;
    if (diff >= 100000) {
        diff /= 1000;
        scale = "sec";
    }
    diff2 = diff / 10;
    printf("%u.%02u %s\n", diff2 / 100, diff2 % 100, scale);
}
#endif

static uint
calc_kb_sec(uint usecs, uint64_t bytes)
{
    if ((bytes >> 32) != 0) {
        bytes >>= 10;
        usecs >>= 20;
    } else if (bytes > 1000000000) {
        usecs >>= 10;
    } else if (bytes < 1000000) {
        bytes <<= 10;
    } else {
        usecs >>= 5;
        bytes <<= 5;
    }
    if (usecs == 0)
        usecs = 1;

    return (((uint32_t) bytes + usecs / 2) / usecs);
}

#if 0
/*
 * are_you_sure() prompts the user to confirm that an operation is intended.
 *
 * @param  [in]  None.
 *
 * @return       TRUE  - User has confirmed (Y).
 * @return       FALSE - User has denied (N).
 */
int
are_you_sure(const char *prompt)
{
    int ch;
ask_again:
    printf("%s - are you sure? (y/n) ", prompt);
    fflush(stdout);
    while ((ch = getchar()) != EOF) {
        if ((ch == 'y') || (ch == 'Y'))
            return (TRUE);
        if ((ch == 'n') || (ch == 'N'))
            return (FALSE);
        if (!isspace(ch))
            goto ask_again;
    }
    return (FALSE);
}

#endif

#define TICKS_PER_MINUTE (TICKS_PER_SECOND * 60)
#define MINUTES_PER_DAY  (24 * 60)
#define MS_PER_TICK (1000 / TICKS_PER_SECOND)

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

int
msleep(uint msec)
{
    while (msec > 1000) {
        Delay(TICKS_PER_SECOND);
        msec -= 1000;
        if (is_user_abort())
            return (1);
    }
    Delay(msec * TICKS_PER_SECOND / 1000);
    return (0);
}

__stdargs int
usleep(useconds_t usec)
{
    return (msleep(usec / 1000));
}

__stdargs unsigned
sleep(unsigned int sec)
{
    return (msleep(sec * 1000));
}

/*
 * unix_time_to_amiga_datestamp
 * -----------------------------
 * Convert UNIX seconds since 1970 to Amiga DateStamp format
 */
void
unix_time_to_amiga_datestamp(uint sec, struct DateStamp *ds)
{
#define UNIX_SEC_TO_AMIGA_SEC (2922 * 24 * 60 * 60)  // 1978 - 1970 = 2922 days
    if (sec >= UNIX_SEC_TO_AMIGA_SEC)
        sec -= UNIX_SEC_TO_AMIGA_SEC;

    ds->ds_Days   = sec / 86400;
    ds->ds_Minute = (sec % 86400) / 60;
    ds->ds_Tick   = (sec % 60) * TICKS_PER_SECOND;
}

static uint32_t
amiga_perms_from_host(uint host_perms)
{
    uint32_t perms;

    perms = ((host_perms & S_IRUSR) ? 0 : FIBF_READ) |
            ((host_perms & S_IWUSR) ? 0 : FIBF_WRITE |
                                          FIBF_DELETE) |
            ((host_perms & S_IXUSR) ? 0 : FIBF_EXECUTE) |

            ((host_perms & S_IRGRP) ? FIBF_GRP_READ    : 0) |
            ((host_perms & S_IWGRP) ? FIBF_GRP_WRITE |
                                      FIBF_GRP_DELETE  : 0) |
            ((host_perms & S_IXGRP) ? FIBF_GRP_EXECUTE : 0) |

            ((host_perms & S_IROTH) ? FIBF_OTR_READ    : 0) |
            ((host_perms & S_IWOTH) ? FIBF_OTR_WRITE |
                                      FIBF_OTR_DELETE  : 0) |
            ((host_perms & S_IXOTH) ? FIBF_OTR_EXECUTE : 0) |

            ((host_perms & S_ISUID) ? FIBF_HOLD   : 0) |  // SUID -> HOLD
            ((host_perms & S_ISGID) ? FIBF_PURE   : 0) |  // SGID -> PURE
            ((host_perms & S_ISVTX) ? FIBF_SCRIPT : 0);   // VTX  -> SCRIPT

    /*
     * Only the base R W E D bits are set = 1 to disable.
     * The rest of the Amiga bits are set = 1 to enable.
     *
     * There are not enough UNIX mode bits to support AMIGA_PERMS_ARCHIVE.
     *
     * Will Map:
     *     Set UID      -> HOLD (resident pure module stays in RAM)
     *     Set GID      -> PURE (re-entrant / re-executable program)
     *     VTX (sticky) -> SCRIPT
     *
     * chmod u+s - set uid (SUID) for HOLD (keep resident modules in memory)
     * chmod g+s - set group id (SGID) for PURE (re-entrant/re-executable)
     * chmod +t  - set sticky (VTX) for SCRIPT
     */

    return (perms);
}

static uint
host_perms_from_amiga(uint amiga_perms)
{
    uint32_t perms;

    perms = ((amiga_perms & FIBF_READ)        ? 0 : S_IRUSR) |
            ((amiga_perms & FIBF_WRITE)       ? 0 : S_IWUSR) |
            ((amiga_perms & FIBF_EXECUTE)     ? 0 : S_IXUSR) |

            ((amiga_perms & FIBF_GRP_READ)    ? S_IRGRP : 0) |
            ((amiga_perms & FIBF_GRP_WRITE)   ? S_IWGRP : 0) |
            ((amiga_perms & FIBF_GRP_EXECUTE) ? S_IXGRP : 0) |

            ((amiga_perms & FIBF_OTR_READ)    ? S_IROTH : 0) |
            ((amiga_perms & FIBF_OTR_WRITE)   ? S_IWOTH : 0) |
            ((amiga_perms & FIBF_OTR_EXECUTE) ? S_IXOTH : 0) |

            ((amiga_perms & FIBF_HOLD)        ? S_ISUID : 0) |
            ((amiga_perms & FIBF_PURE)        ? S_ISGID : 0) |
            ((amiga_perms & FIBF_SCRIPT)      ? S_ISVTX : 0) |
            ((amiga_perms & FIBF_ARCHIVE)     ? 0x10000 : 0);

    return (perms);
}

rc_t
cmd_cd(int argc, char * const *argv)
{
    handle_t      handle;
    uint          rc;
    uint          type;
    char          nbuf[256];
    const char   *nwd;
    char         *name;

    nwd = argv[1];

    if (argc == 1) {
        /* Return to top level (volume directory) */
        nwd = "::";
    }
    rc = sm_fopen(cwd_handle, nwd, HM_MODE_READDIR, &type, 0, &handle);
    if (rc != KM_STATUS_OK) {
        printf("Failed to open %s: %s\n", nwd, smash_err(rc));
        return (RC_FAILURE);
    }
    if (type == HM_TYPE_LINK) {
        /* Need to follow link to determine if it's a directory */
        sm_fclose(handle);
        strcpy(nbuf, nwd);
        strcat(nbuf, "/.");
        rc = sm_fopen(cwd_handle, nbuf, HM_MODE_READDIR, &type, 0, &handle);
        if (rc != KM_STATUS_OK)
            printf("Could not follow link %s: %s\n", nbuf, smash_err(rc));
        return (RC_FAILURE);
    }
    if ((type != HM_TYPE_DIR) && (type != HM_TYPE_VOLUME) &&
        (type != HM_TYPE_VOLDIR)) {
        printf("%s is not a directory (%x)\n", nwd, type);
        sm_fclose(handle);
        return (RC_FAILURE);
    }

    if ((rc = sm_fpath(handle, &name)) != 0) {
        printf("sm_fpath(%s) failed: %s\n", nwd, smash_err(rc));
        sm_fclose(handle);
        return (RC_FAILURE);
    }
    strcpy(nbuf, name);
    name = nbuf;

    if (cwd_handle != 0xffffffff)
        sm_fclose(cwd_handle);
    if (cwd != NULL)
        free(cwd);
    cwd = strdup(name);
    printf("cwd=%s\n", cwd);
    cwd_handle = handle;
    return (RC_SUCCESS);
}

/*
 * is_chmod_mode
 * -------------
 * Determines if the specified string is a chmod/SetProtect mode, and
 * parses the permissions to add and subtract as a result of that
 * specified string.
 *
 * Note that the definition of the owner 'RWED' bits must be inverted
 * when using this function. This was done to simplify the permission
 * setting logic.
 *
 * The function can handle various types of permission strings.
 * Examples:
 *     rewd     Set READ, EXECUTE, WRITE, DELETe and clear everything else.
 *              This is compatible with how the Protect command works.
 *     +r       Set READ permission for owner, group and other.
 *     g-w      Remove WRITE permission for group, leaving others intact.
 *     755      UNIX-style octal permissions (u=rwx g=rx, o=rx).
 *     o=x      Set execute in other group and leave other permissions intact.
 *     +s       Set file's SCRIPT flag (for shell scripts).
 */
static uint
is_chmod_mode(const char *str, uint *add, uint *subtract)
{
    uint mask = 0;
    uint ugomask = 0;
    const char *ptr;
    const char *sstr;
    uint equalsplusminus = 0;
#define SAW_PLUS   '+'
#define SAW_MINUS  '-'
#define SAW_EQUALS '='

    if (*str == '\0')
        return (RC_FAILURE);

    /* Check for [ugoa][=+-][rwexd] */
    for (ptr = str; *ptr != '\0'; ptr++) {
        if (*ptr == 'u') {
            ugomask |= FIBF_READ | FIBF_WRITE | FIBF_EXECUTE | FIBF_DELETE;
        } else if (*ptr == 'g') {
            ugomask |= FIBF_GRP_READ | FIBF_GRP_WRITE | FIBF_GRP_EXECUTE |
                       FIBF_GRP_DELETE;
        } else if (*ptr == 'o') {
            ugomask |= FIBF_OTR_READ | FIBF_OTR_WRITE | FIBF_OTR_EXECUTE |
                       FIBF_OTR_DELETE;
        } else if (*ptr == 'a') {
            ugomask |= FIBF_READ | FIBF_WRITE | FIBF_EXECUTE | FIBF_DELETE |
                       FIBF_GRP_READ | FIBF_GRP_WRITE | FIBF_GRP_EXECUTE |
                       FIBF_GRP_DELETE |
                       FIBF_OTR_READ | FIBF_OTR_WRITE | FIBF_OTR_EXECUTE |
                       FIBF_OTR_DELETE;
        } else {
            break;
        }
    }
    if (ugomask == 0) {
        ugomask = FIBF_READ | FIBF_WRITE | FIBF_EXECUTE | FIBF_DELETE |
                  FIBF_GRP_READ | FIBF_GRP_WRITE | FIBF_GRP_EXECUTE |
                  FIBF_GRP_DELETE |
                  FIBF_OTR_READ | FIBF_OTR_WRITE | FIBF_OTR_EXECUTE |
                  FIBF_OTR_DELETE;
    }
    if ((*ptr == SAW_PLUS) || (*ptr == SAW_MINUS) || (*ptr == SAW_EQUALS)) {
        /* Possibly [ugoa][=+-][rwexd] */
        if (ptr == str)
            str++;  // For the benefit of later "hsparwed" and 4755 code
        equalsplusminus = *ptr;
        for (ptr++; *ptr != '\0'; ptr++) {
            if (*ptr == 'r') {
                mask |= (ugomask & (FIBF_READ | FIBF_GRP_READ | FIBF_OTR_READ));
            } else if (*ptr == 'w') {
                mask |= (ugomask & (FIBF_WRITE | FIBF_GRP_WRITE |
                                    FIBF_OTR_WRITE));
            } else if ((*ptr == 'e') || (*ptr == 'x')) {
                mask |= (ugomask & (FIBF_EXECUTE | FIBF_GRP_EXECUTE |
                                    FIBF_OTR_EXECUTE));
            } else if (*ptr == 'd') {
                mask |= (ugomask & (FIBF_DELETE | FIBF_GRP_DELETE |
                                    FIBF_OTR_DELETE));
            } else {
                break;
            }
        }
        if ((*ptr == '\0') && (ptr != str)) {
            /* Got [ugoa][=+-][rwexd] */
mode_is_good:
            if (mask == 0) {
                return (0);
            }
            switch (equalsplusminus) {
                default:
                case SAW_EQUALS:
                    if (ugomask == 0) {
                        *add      = mask;
                        *subtract = ~mask;
                    } else {
                        *add      = mask & ugomask;
                        *subtract = ~mask & ugomask;
                    }
                    break;
                case SAW_PLUS:
                    *add      |= mask;
                    *subtract &= ~mask;
                    break;
                case SAW_MINUS:
                    *add      &= ~mask;
                    *subtract |= mask;
                    break;
            }
            return (1);
        }
    }

    mask = 0;
    ugomask = 0;

    /* Check for "hsparwed" | "x" SetProtect format */
    for (ptr = str; *ptr != '\0'; ptr++) {
        static const char permstr[] = "hsparwedx";
        char *pos = strchr(permstr, *ptr);
        uint  bit;
        if (pos == NULL)
            break;
        bit = pos - permstr;
        if (bit == 8)
            bit = 1;  // 'x' is the same as 'e'
        else
            bit = 7 - bit;
        mask |= BIT(bit);
    }
    if (*ptr == '\0') {
        /* Made it through entire string -- this is SetProtect format */
        goto mode_is_good;
    }

    mask = 0;

    /* Check for numeric (4755), which must be translated from UNIX format */
    for (sstr = str, ptr = str; *ptr != '\0'; ptr++) {
        if ((ptr - sstr) > 3)
            break;  // too long
        if ((*ptr < '0') || (*ptr > '7'))
            break;  // not octal
        mask <<= 3;
        mask |= (*ptr - '0');
    }
    if (*ptr == '\0') {
        /* Made it through entire string -- this is UNIX format */
        mask = amiga_perms_from_host(mask);  // e.g. 755 -> Amiga fmt
        mask ^= 0x000f;     // Invert bits which take away permission
        goto mode_is_good;
    }

    return (0);  // No match
}

rc_t
cmd_chmod(int argc, char * const *argv)
{
    /*
     * If there is a match of anything in "hsparwed" for the
     * first arg, then operate in SetProtect mode where this
     * sets all file permissions.
     *
     * If it instead has chmod syntax like 4755 or u+rw, then
     * default to chmod syntax  [ugoa][+-=][rexwd] [+-=][hsparewd x]
     */
    uint          do_remote = 0;
    uint          add      = 0;
    uint          subtract = 0;
    uint          perms;
    int           arg;
    rc_t          rc = RC_SUCCESS;
    rc_t          rc2;
    struct FileInfoBlock fib;

    if ((strcmp(argv[0], "chmod") == 0) ||
        (strncmp(argv[0], "protect", 4) == 0) ||
        (strncasecmp(argv[0], "setprotect", 7) == 0)) {
        do_remote = 1;
    }

    /* Handle parsing mode/permission settings */
    for (arg = 1; arg < argc; arg++)
        if (is_chmod_mode(argv[arg], &add, &subtract) == 0)
            break;

    if ((arg == argc) || ((add == 0) && (subtract == 0))) {
        printf("Need to supply at least one mask and filename\n");
        return (RC_USER_HELP);
    }
    // printf("add=%04x subtract=%04x\n", (uint16_t) add, (uint16_t) subtract);

    /* Remainder of args are filenames */
    for (; arg < argc; arg++) {
        char *name = argv[arg];

        if (do_remote) {
            handle_t      handle;
            uint          rlen;
            uint          type;
            hm_fdirent_t *dent;
            rc = sm_fopen(cwd_handle, name, HM_MODE_READDIR, &type, 0, &handle);
            if (rc != KM_STATUS_OK) {
                printf("Failed to open %s: %s\n", name, smash_err(rc));
                rc = RC_FAILURE;
                continue;
            }
            rc2 = sm_fread(handle, DIRBUF_SIZE, (void **) &dent, &rlen, 0);
            if (rlen == 0) {
                printf("Failed to stat remote file %s: %s\n",
                       name, smash_err(rc2));
                rc = RC_FAILURE;
                sm_fclose(handle);
                continue;
            }
            perms = dent->hmd_aperms;
            sm_fclose(handle);
        } else {
            BPTR lock = Lock(name, ACCESS_READ);
            if (lock == 0) {
                printf("Failed to lock %s\n", name);
                rc = RC_FAILURE;
                continue;
            }
            if (Examine(lock, &fib) == 0) {
                printf("%s can not be examined\n", name);
                UnLock(lock);
                rc = RC_FAILURE;
                continue;
            }
            UnLock(lock);
            perms = fib.fib_Protection;
        }

        perms ^= 0x0000000f;  // Amiga RWED are inverted for permission
        perms &= ~subtract;
        perms |= add;
        perms ^= 0x0000000f;

        if (do_remote) {
            rc2 = sm_fsetprotect(cwd_handle, name, perms);
            if (rc2 != KM_STATUS_OK) {
                printf("Failed to set protection on %s: %s\n",
                       name, smash_err(rc2));
                rc = RC_FAILURE;
            }
        } else {
            if (SetProtection(name, perms) == 0) {
                printf("Failed to set protection on %s\n", name);
                rc = RC_FAILURE;
            }
        }
    }
    return (rc);
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
cmd_debug(int argc, char * const *argv)
{
    UNUSED(argc);
    UNUSED(argv);
    flag_debug++;
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
    char restore = '\0';

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
            restore = *ptr;
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
                if (is_user_abort()) {
                    printf("^C\n");
                    return (RC_USR_ABORT);
                }
            }
            break;
        case 1:  /* minutes */
            for (count = 0; count < 60 * value; count++) {
                sleep(1);
                if (is_user_abort()) {
                    printf("^C\n");
                    return (RC_USR_ABORT);
                }
            }
            break;
        case 0:  /* seconds */
            for (count = 0; count < value; count++) {
                sleep(1);
                if (is_user_abort()) {
                    printf("^C\n");
                    return (RC_USR_ABORT);
                }
            }
            break;
        case -1:  /* milliseconds */
            while (value > 1000) {
                sleep(1);
                if (is_user_abort()) {
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
    if (ptr != NULL)
        *ptr = restore;
    return (RC_SUCCESS);
}

static uint
is_dir(const char *name)
{
    BPTR                 lock;
    struct FileInfoBlock fib;

    lock = Lock(name, ACCESS_READ);
    if (lock == 0)
        return (0);

    if (Examine(lock, &fib) == 0) {
        printf("%s can not be examined\n", name);
        UnLock(lock);
        return (0);
    }
    UnLock(lock);

    switch (fib.fib_DirEntryType) {
        case ST_ROOT:
        case ST_USERDIR:
        case ST_SOFTLINK:
        case ST_LINKDIR:
            return (1);
    }
    return (0);
}

static uint
is_remote_dir(const char *name)
{
    handle_t handle;
    uint     rc;
    uint     type;
    char     nbuf[256];

    rc = sm_fopen(cwd_handle, name, HM_MODE_READDIR, &type, 0, &handle);
    if (rc != KM_STATUS_OK)
        return (0);
    sm_fclose(handle);

    if (type == HM_TYPE_LINK) {
        /* Need to follow link to determine if it's a directory */
        strcpy(nbuf, name);
        strcat(nbuf, "/.");
        rc = sm_fopen(cwd_handle, nbuf, HM_MODE_READDIR, &type, 0, &handle);
        if (rc != KM_STATUS_OK)
            return (0);
        sm_fclose(handle);
        return (0);
    }
    if ((type == HM_TYPE_DIR) ||
        (type == HM_TYPE_VOLUME) ||
        (type == HM_TYPE_VOLDIR)) {
        return (1);
    }
    return (0);
}

#if 0
static uint
get_file_size(const char *filename)
{
    struct FileInfoBlock fib;
    BPTR lock;
    fib.fib_Size = VALUE_UNASSIGNED;
    lock = Lock(filename, ACCESS_READ);
    if (lock == 0L) {
        printf("Lock %s failed\n", filename);
        return (VALUE_UNASSIGNED);
    }
    if (Examine(lock, &fib) == 0) {
        printf("Examine %s failed\n", filename);
        UnLock(lock);
        return (VALUE_UNASSIGNED);
    }
    UnLock(lock);
    return (fib.fib_Size);
}
#endif

static uint
get_file_fib(const char *filename, struct FileInfoBlock *fib)
{
    BPTR lock;
    lock = Lock(filename, ACCESS_READ);
    if (lock == 0L) {
        printf("Lock %s failed\n", filename);
        return (1);
    }
    if (Examine(lock, fib) == 0) {
        printf("Examine %s failed\n", filename);
        UnLock(lock);
        return (1);
    }
    UnLock(lock);
    return (0);
}

static rc_t
get_file(const char *src, const char *dst)
{
    int      bytes;
    uint     diff;
    uint     type;
    uint     rc;
    uint     rlen;
    uint     fileperms;
    uint     filemtime;
    uint     buflen = 32768;
    handle_t handle;
    uint8_t *data;
    uint64_t pos = 0;
    uint64_t filesize = 0;
    uint64_t time_start;
    uint64_t time_end;
    FILE    *fp;
    hm_fdirent_t *dent;
    struct DateStamp datestamp;

    rc = sm_fopen(cwd_handle, src, HM_MODE_READDIR, &type, 0, &handle);
    if (rc != KM_STATUS_OK) {
        printf("Failed to open %s for stat: %s\n", src, smash_err(rc));
        return (RC_FAILURE);
    }

    rc = sm_fread(handle, DIRBUF_SIZE, (void **) &dent, &rlen, 0);
    if (rlen == 0) {
        printf("Failed to stat remote file %s: %s\n", src, smash_err(rc));
        return (RC_FAILURE);
    }

    filesize = ((uint64_t) dent->hmd_size_hi << 32) | dent->hmd_size_lo;
    fileperms = dent->hmd_aperms;
    filemtime = dent->hmd_mtime;
    sm_fclose(handle);

    if (is_user_abort()) {
        printf("^C\n");
        return (RC_USR_ABORT);
    }
    printf("Get %s as %s ", src, dst);
    if (filesize < 1000000)
        printf("(%u bytes) ", (uint) filesize);
    else
        printf("(%u KB) ", (uint) ((filesize + 512) >> 10));
    fflush(stdout);

    rc = sm_fopen(cwd_handle, src, HM_MODE_READ, &type, 0, &handle);
    if (rc != KM_STATUS_OK) {
        printf("Failed to open %s for read: %s\n", src, smash_err(rc));
        return (RC_FAILURE);
    }
    fp = fopen(dst, "w");
    if (fp == NULL) {
        printf("Failed to open %s for write\n", dst);
        sm_fclose(handle);
        return (RC_FAILURE);
    }

    rc = RC_SUCCESS;
    time_start = smash_time();
    while (pos < filesize) {
        if (is_user_abort()) {
            printf("^C\n");
            fclose(fp);
            sm_fclose(handle);
            return (RC_USR_ABORT);
        }
        rc = sm_fread(handle, buflen, (void **) &data, &rlen, 0);
        if (rlen == 0) {
failed_to_read:
            printf("Failed to read %s at pos %x: %s\n",
                   src, (uint) pos, smash_err(rc));
            break;
        }
        if (flag_debug)
            printf("got %u bytes\n", rlen);
        bytes = fwrite(data, 1, rlen, fp);
        if (bytes < rlen) {
            printf("Failed to write all bytes to %s at pos %x\n",
                   dst, (uint) pos);
            rc = RC_FAILURE;
            break;
        }
        pos += rlen;
        if (rc != RC_SUCCESS)
            goto failed_to_read;
    }
    time_end = smash_time();
    diff = (uint) (time_end - time_start);
    if (flag_debug)
        printf("%u usec  ", diff);
    printf(" %u KB/sec\n", calc_kb_sec(diff, filesize));
    fclose(fp);
    sm_fclose(handle);

    if (SetProtection(dst, fileperms) == 0)
        printf("Failed to set protection on %s\n", dst);
    unix_time_to_amiga_datestamp(filemtime, &datestamp);
    if (SetFileDate(dst, &datestamp) == 0)
        printf("Failed to set date on %s\n", dst);

    if (rc == KM_STATUS_EOF)
        rc = 0;
    return (rc);
}

static rc_t
get_files(const char *src, const char *dst)
{
    rc_t     rc;
    handle_t handle;
    uint     type;
    uint     recursive = 0;

    if ((dst == NULL) || (strcmp(dst, ".") == 0)) {
        /* Need to trim dst file name from src name */
        for (dst = src; *dst != '\0'; dst++)
            ;
        if (dst > src)
            dst--;
        if (*dst == '/') {
            printf("Can not yet get remote directory: %s\n", src);
            // XXX: This would be a nice feature to add.
            //      Recursive is too much work, however.
            //
            //      Would also be nice to be able to support wildcards
            //      that could be implemented on top of remote directory
            //      support.
            return (RC_FAILURE);
        }
        for (; dst > src; dst--)
            if ((dst[-1] == '/') || (dst[-1] == ':'))
                break;
    }

    rc = sm_fopen(cwd_handle, src, HM_MODE_READ, &type, 0, &handle);
    if (rc != KM_STATUS_OK) {
        printf("Failed to open %s: %s\n", src, smash_err(rc));
        return (RC_FAILURE);
    }
    if (type != HM_TYPE_FILE) {
        printf("Can not yet get non-file: %s (%x)\n", src, type);
        sm_fclose(handle);
        return (RC_FAILURE);
    }

    if (is_dir(dst) || (strcmp(dst, ".") == 0)) {
        /* Single or multiple file get */
        uint  dstlen  = strlen(dst);
        char *dstpath = malloc(dstlen + 256);
        uint  srclen  = strlen(src);
        char *srcpath = malloc(srclen + 256);
        char *srcname = (char *)src;
        if ((dstpath == NULL) || (srcpath == NULL)) {
            printf("malloc(%u) failure\n", dstlen + 256);
            if (dstpath != NULL)
                free(dstpath);
            if (srcpath != NULL)
                free(srcpath);
            return (RC_FAILURE);
        }
        memcpy(dstpath, dst, dstlen);
        if (dstlen > 0) {
            if ((dstpath[dstlen - 1] != ':') && (dstpath[dstlen - 1] != '/'))
                dstpath[dstlen++] = '/';
        }
        dstpath[dstlen] = '\0';

        memcpy(srcpath, src, srclen);
        srcpath[srclen] = '\0';
#if 0
        if (src_is_dir) {
            srcpath[srclen++] = '/';
            srcpath[srclen] = '\0';

            srcname = dir_next(handle, dirbuf);
            strcpy(srcpath + srclen, srcname);
        } else
#endif
        // Assume single file for now
        if (!recursive) {
            char *ptr;
            for (ptr = srcname; *ptr != '\0'; ptr++)
                ;
            for (--ptr; ptr > srcname; ptr--) {
                if ((ptr[-1] == '/') || (ptr[-1] == ':'))
                    break;
            }
            if (srcname < ptr)
                srcname = ptr;
            strcpy(dstpath + dstlen, srcname);
        }
        strcpy(dstpath + dstlen, srcname);

        rc = get_file(srcpath, dstpath);
        free(dstpath);
        free(srcpath);
        sm_fclose(handle);
    } else {
        /* Simple file get */
        sm_fclose(handle);
        if ((type == HM_TYPE_DIR) || (type == HM_TYPE_VOLDIR)) {
            printf("Source %s is a directory but destination %s "
                   "does not exist\n", src, dst);
            return (RC_FAILURE);
        }
        rc = get_file(src, dst);
    }
    return (rc);
}

rc_t
cmd_get(int argc, char * const *argv)
{
    int arg;
    rc_t rc = RC_SUCCESS;
    const char *getas = NULL;
    const char *saveas = NULL;

    for (arg = 1; arg < argc; arg++) {
        const char *ptr = argv[arg];
        if (*ptr == '-') {
            while (*(++ptr) != '\0') {
                switch (*ptr) {
                    default:
                        printf("Unknown argument -%s\n", ptr);
                        printf(cmd_get_help);
                        return (RC_BAD_PARAM);
                }
            }
        }
    }
    for (arg = 1; arg < argc; arg++) {
        const char *ptr = argv[arg];
        if (*ptr != '-') {
            if (getas == NULL) {
                getas = ptr;
                continue;
            } else if (saveas == NULL) {
                saveas = ptr;
                continue;
            } else {
                /* get multiple */
                char *dst = NULL;
                char *final = argv[argc - 1];
                if (is_dir(final)) {
                    dst = final;
                    argc--;
                }
                rc = get_files(getas, dst);
                if (rc != RC_SUCCESS)
                    return (rc);
                rc = get_files(saveas, dst);
                if (rc != RC_SUCCESS)
                    return (rc);
                for (; arg < argc; arg++) {
                    rc = get_files(argv[arg], dst);
                    if (rc != RC_SUCCESS)
                        return (rc);
                }
                return (rc);
            }
        }
    }
    if (getas != NULL) {
        rc = get_files(getas, saveas);
    } else {
        printf(cmd_get_help);
        return (RC_BAD_PARAM);
    }
    return (rc);
}

rc_t
cmd_history(int argc, char * const *argv)
{
    UNUSED(argc);
    UNUSED(argv);
    history_show();
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

rc_t
cmd_lcd(int argc, char * const *argv)
{
    BPTR old_lock;
    BPTR new_lock;

    if (argc != 2)
        return (RC_USER_HELP);

    new_lock = Lock(argv[1], SHARED_LOCK);
    if (new_lock == 0L) {
        printf("Failed to access %s\n", argv[1]);
        return (RC_FAILURE);
    }
    old_lock = CurrentDir(new_lock);
    if (save_currentdir == 0)
        save_currentdir = old_lock;
    else
        UnLock(old_lock);
    return (RC_SUCCESS);
}

/*
 * remote_quotes
 * -------------
 * Remove surrounding quotes, if present.
 */
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
loop_index_substitute(char *src, int value, int count, int loop_level)
{
    char   valbuf[10];
    int    vallen  = sprintf(valbuf, "%x", value);
    size_t newsize = strlen(src) + vallen * count + 1;
    char  *nstr    = malloc(newsize);
    char  *dptr    = nstr;
    char   varstr[4] = "$a";

    varstr[1] += loop_level;  // $a, $b, $c, etc.
    if (nstr == NULL)
        return (strdup(src));

    while (*src != '\0') {
        char *ptr = strstr(src, varstr);
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
loop_index_count(char *src, int loop_level)
{
    int   count = 0;
    char  varstr[4] = "$a";
    varstr[1] += loop_level;  // $a, $b, $c, etc.

    while (*src != '\0') {
        src = strstr(src, varstr);
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
    static uint loop_level = 0;  // for nested loops

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
    index_uses = loop_index_count(cmd, loop_level);
    if (index_uses == 0)
        nargc = make_arglist(cmd, nargv);

    for (cur = 0; cur < count; cur++) {
        if (index_uses > 0) {
            if (cur != 0)
                free_arglist(nargc, nargv);
            ptr = loop_index_substitute(cmd, cur, index_uses, loop_level);
            nargc = make_arglist(ptr, nargv);
            free(ptr);
        }
        loop_level++;
        rc = cmd_exec_argv(nargc, nargv);
        loop_level--;
        if (rc != RC_SUCCESS) {
            if (rc == RC_USER_HELP)
                rc = RC_FAILURE;
            goto finish;
        }
        if (is_user_abort()) {
            printf("^C\n");
            rc = RC_USR_ABORT;
            break;
        }
    }
finish:
    free(cmdline);
    free_arglist(nargc, nargv);
    return (rc);
}

rc_t
cmd_lpwd(int argc, char * const *argv)
{
    struct Process *this_proc = (struct Process *) FindTask(NULL);
    BPTR            lock;
    char            name[256];
    UNUSED(argc);
    UNUSED(argv);
    if ((this_proc == NULL) || ((lock = this_proc->pr_CurrentDir) == 0)) {
        printf("Unknown\n");
        return (RC_FAILURE);
    }

    if (NameFromLock(lock, name, sizeof (name)) == 0) {
        printf("NameFromLock failed\n");
        return (RC_FAILURE);
    }
    printf("%s\n", name);
    return (RC_SUCCESS);
}

static void
print_daytime(struct DateStamp *dstamp)
{
    char datebuf[32];
    char timebuf[32];
    uint   Tflag = 1;
    struct DateTime dtime;
    struct DateStamp todaystamp;

    dtime.dat_Stamp.ds_Days   = dstamp->ds_Days;
    dtime.dat_Stamp.ds_Minute = dstamp->ds_Minute;
    dtime.dat_Stamp.ds_Tick   = dstamp->ds_Tick;
    if (Tflag)
        dtime.dat_Format      = FORMAT_CDN;
    else
        dtime.dat_Format      = FORMAT_DOS;
    dtime.dat_Flags           = 0x0;
    dtime.dat_StrDay          = NULL;
    dtime.dat_StrDate         = datebuf;
    dtime.dat_StrTime         = timebuf;
    DateToStr(&dtime);
    DateStamp(&todaystamp);

    if (datebuf[0] == '0')              /* remove date leading zero */
        datebuf[0] = ' ';

    if (datebuf[0] == '-') {
show_blank:
        printf("            ");
    } else if (Tflag) {
        int day;
        int month;
        int year;
        if (sscanf(datebuf, "%d-%d-%d", &day, &month, &year) != 3)
            goto show_blank;
        if (year >= 70)
            year += 1900;
        else
            year += 2000;
        printf("%4d-%02d-%02d %s", year, month, day, timebuf);
    } else if ((dtime.dat_Stamp.ds_Days + 274 > todaystamp.ds_Days) &&
               (dtime.dat_Stamp.ds_Days < todaystamp.ds_Days + 91)) {
        /* It's over about nine months old; show year and no time */
        printf("%-3.3s %2.2s %5.5s", datebuf + 3,
                datebuf, timebuf);
    } else {
        printf("%-3.3s %2.2s  %d%.2s", datebuf + 3, datebuf,
                (datebuf[7] > '6') ? 19 : 20, datebuf + 7);
    }
}

static void
print_amiga_perms(uint perms, uint style, uint hmd_type)
{
    uint bit;
    if (style == 0) {
        /* Amiga style permissions */
        char permstr[] = "hsparwed ";
        perms ^= 0xf0;  // top bits are 'flag set' when 1
        for (bit = 0; bit <= 7; bit++)
            if (perms & BIT(bit))
                permstr[7 - bit] = '-';
        printf(permstr);
    } else {
        /* UNIX style */
        uint uperms = host_perms_from_amiga(perms);
        char utype[]   = "?-dlbcpswvD";
        char permstr[] = "-rwxrwxrwx ";
        if (hmd_type < sizeof (utype) - 1)
            permstr[0] = utype[hmd_type];  // 'd' for directory, etc
        for (bit = 0; bit < 9; bit++)
            if ((uperms & BIT(bit)) == 0)
                permstr[9 - bit] = '-';
        if (uperms & S_ISUID)
            permstr[3] = (permstr[3] == '-') ? 'S' : 's';
        if (uperms & S_ISGID)
            permstr[6] = (permstr[6] == '-') ? 'S' : 's';
        if (uperms & 0x10000)
            permstr[7] = (permstr[7] == '-') ? 'A' : 'a';  // Archived
        if (uperms & S_ISVTX)
            permstr[9] = (permstr[9] == '-') ? 'T' : 't';
        printf(permstr);
    }
}

#define LS_FLAG_LONG       0x0001  // long listing
#define LS_FLAG_ATIME      0x0002  // show last access time
#define LS_FLAG_CTIME      0x0004  // show creation time
#define LS_FLAG_ALL        0x0008  // show . and ..
#define LS_FLAG_DIR        0x0010  // show simple Amiga dir format
#define LS_FLAG_LIST       0x0020  // long Amiga list format
#define LS_FLAG_CLASSIFY   0x0040  // classify names (add / to dirs)
#define LS_FLAG_DIRENT     0x0080  // show directory entry


static uint
amiga_dir_type_to_hmd_type(LONG entry_type)
{
    switch (entry_type) {
        case ST_ROOT:
            return (HM_TYPE_VOLDIR);
        case ST_USERDIR:
        case ST_SOFTLINK:
            return (HM_TYPE_DIR);
        case ST_LINKDIR:
            return (HM_TYPE_LINK);
        case ST_FILE:
            return (HM_TYPE_FILE);
        case ST_LINKFILE:
            return (HM_TYPE_LINK);
        case ST_PIPEFILE:
            return (HM_TYPE_FIFO);
        default:
            return (HM_TYPE_UNKNOWN);
    }
}

static void
lls_show_fib(struct FileInfoBlock *fib, uint flags)
{
    uint noslash = 0;
    char filesize[32];
    const char *dname = fib->fib_FileName;
    uint hmd_type = amiga_dir_type_to_hmd_type(fib->fib_DirEntryType);
    char *cname;

    if ((dname[0] == '.') &&
        (dname[1] == '\0' || (dname[1] == '.' && dname[2] == '\0'))) {
        if ((flags & LS_FLAG_ALL) == 0)
            return;
        noslash = 1;
    }
    sprintf(filesize, "%8u", fib->fib_Size);
    if (flags & LS_FLAG_LIST) {
        /* Amiga "list" format */
        int namelen = strlen(dname);
        int filesizelen;
        int namemax;

        if ((hmd_type < ARRAY_SIZE(hmd_types)) && (hmd_type != HM_TYPE_FILE))
            strcpy(filesize, hmd_types[hmd_type]);
        filesizelen = strlen(filesize);
        namemax = 38 - filesizelen;
        if ((flags & LS_FLAG_LONG) && (namemax < namelen))
            namemax = namelen;
        printf("%-*.*s %s ", namemax, namemax, dname, filesize);
        print_amiga_perms(fib->fib_Protection, 0, hmd_type);
        print_daytime(&fib->fib_Date);
    } else if (flags & LS_FLAG_LONG) {
        /* Unix "ls -l" format */
        print_amiga_perms(fib->fib_Protection, 1, hmd_type);
        printf("%s ", filesize);
        print_daytime(&fib->fib_Date);
        printf(" %s", dname);
    } else {
        /* UNIX "ls" format */
        printf("%s", dname);
    }
    cname = fib->fib_Comment;
    if (flags & LS_FLAG_LONG) {
        if (hmd_type == HM_TYPE_VOLDIR) {
            printf(":");
        } else if (hmd_type == HM_TYPE_DIR) {
            if ((flags & LS_FLAG_CLASSIFY) && (noslash == 0))
                printf("/");
        } else if (hmd_type == HM_TYPE_LINK) {
            printf(" -> %s", cname);
        } else if (cname[0] != '\0') {
            printf("\n: %s", cname);
        }
    } else {
        if (hmd_type == HM_TYPE_DIR) {
            if ((flags & LS_FLAG_CLASSIFY) && (noslash == 0))
                printf("/");
        } else if (hmd_type == HM_TYPE_LINK) {
            if (flags & LS_FLAG_CLASSIFY)
                printf("@");
        }
    }
    printf("\n");
}

static rc_t
lls_show(const char *name, uint flags)
{
    BPTR lock;
    uint isdir = 0;
    struct FileInfoBlock fib;

    lock = Lock(name, ACCESS_READ);
    if (lock == 0) {
        printf("Failed to open %s\n", name);
        return (RC_FAILURE);
    }
    if (Examine(lock, &fib) == 0) {
        printf("%s can not be examined\n", name);
        UnLock(lock);
        return (RC_FAILURE);
    }
    switch (fib.fib_DirEntryType) {
        case ST_ROOT:
        case ST_USERDIR:
        case ST_SOFTLINK:
        case ST_LINKDIR:
            isdir = 1;
            break;
    }

    if (isdir && ((flags & LS_FLAG_DIRENT) == 0)) {
        while (ExNext(lock, &fib) != 0) {
            lls_show_fib(&fib, flags);
        }
    } else {
        lls_show_fib(&fib, flags);
    }
    UnLock(lock);
    return (RC_SUCCESS);
}

rc_t
cmd_lls(int argc, char * const *argv)
{
    int arg;
    rc_t rc = RC_SUCCESS;
    rc_t rc2;
    uint flags = 0;
    uint did_show = 0;

    if (strcmp(argv[0], "llist") == 0)
        flags |= LS_FLAG_LIST;
    else if (strcmp(argv[0], "ldir") == 0)
        flags |= LS_FLAG_DIR;

    for (arg = 1; arg < argc; arg++) {
        const char *ptr = argv[arg];
        if (*ptr == '-') {
            while (*(++ptr) != '\0') {
                switch (*ptr) {
                    case 'a':
                    case 'A':
                        flags |= LS_FLAG_ALL;  // all
                        break;
                    case 'd':
                        flags |= LS_FLAG_DIRENT;    // directory entry
                        break;
                    case 'F':
                        flags |= LS_FLAG_CLASSIFY;  // classify ( dir/ )
                        break;
                    case 'l':
                        flags |= LS_FLAG_LONG;   // long (Unix ls -l))
                        break;
                    case 'L':
                        flags |= LS_FLAG_LIST;   // list (Amiga list)
                        break;
                    default:
                        printf("Unknown argument -%s\n"
                               "Usage:\n", ptr);
                        printf("    %s -A  - show . and ..\n"
                               "    %s -d  - show directory itself instead "
                                            "of contents\n"
                               "    %s -l  - show long listing with file "
                                            "size and date\n",
                               argv[0], argv[0], argv[0]);
                        return (RC_BAD_PARAM);
                }
            }
        }
    }

    for (arg = 1; arg < argc; arg++) {
        const char *ptr = argv[arg];
        if (*ptr != '-') {
            did_show++;
            rc2 = lls_show(ptr, flags);
            if (rc == RC_SUCCESS)
                rc = rc2;
        }
    }
    if (did_show == 0) {
        rc2 = lls_show("", flags);
        if (rc == RC_SUCCESS)
            rc = rc2;
    }
    return (rc);
}

static uint
show_dirent(hm_fdirent_t *dent, uint flags)
{
    uint  noslash = 0;
    uint  entlen = dent->hmd_elen;
    char *dname  = (char *) (dent + 1);
    char *cname;
    struct DateStamp ds;
    uint sec;
    char filesize[32];

    if (entlen > 256) {
        printf("Corrupt entlen=%x for %.20s\n", entlen, dname);
        return (0);
    }
    if ((dname[0] == '.') &&
        (dname[1] == '\0' || (dname[1] == '.' && dname[2] == '\0'))) {
        if ((flags & LS_FLAG_ALL) == 0)
            return (entlen);
        noslash = 1;
    }
    if (flags & (LS_FLAG_LIST | LS_FLAG_LONG)) {
        if (dent->hmd_size_hi != 0) {
            /* Work around fact clib2 can't print 64-bit values */
            uint gb = (((uint64_t) dent->hmd_size_hi << 32) |
                       dent->hmd_size_lo) / 1000000000U;
            sprintf(filesize, "%u%09u", gb, dent->hmd_size_lo % 1000000000U);
        } else {
            sprintf(filesize, "%8u", dent->hmd_size_lo);
        }
        if (flags & LS_FLAG_CTIME)
            sec = dent->hmd_ctime;
        else if (flags & LS_FLAG_ATIME)
            sec = dent->hmd_atime;
        else
            sec = dent->hmd_mtime;
        unix_time_to_amiga_datestamp(sec, &ds);
    }

    if (flags & LS_FLAG_LIST) {
        /* Amiga "list" format */
        int namelen = strlen(dname);
        int filesizelen;
        int namemax;

        if ((dent->hmd_type < ARRAY_SIZE(hmd_types)) &&
            (dent->hmd_type != HM_TYPE_FILE))
            strcpy(filesize, hmd_types[dent->hmd_type]);
        filesizelen = strlen(filesize);

        namemax = 38 - filesizelen;
        if ((flags & LS_FLAG_LONG) && (namemax < namelen))
            namemax = namelen;
        printf("%-*.*s %s ", namemax, namemax, dname, filesize);
        print_amiga_perms(dent->hmd_aperms, 0, dent->hmd_type);
        print_daytime(&ds);
    } else if (flags & LS_FLAG_LONG) {
        /* Unix "ls -l" format */
        print_amiga_perms(dent->hmd_aperms, 1, dent->hmd_type);
        printf("%s ", filesize);
        print_daytime(&ds);
        printf(" %s", dname);
    } else {
        /* UNIX "ls" format */
        printf("%s", dname);
    }
    cname = dname + strlen(dname) + 1;
    if (flags & LS_FLAG_LONG) {
        if (dent->hmd_type == HM_TYPE_DIR) {
            if ((flags & LS_FLAG_CLASSIFY) && (noslash == 0))
                printf("/");
        } else if (dent->hmd_type == HM_TYPE_LINK) {
            printf(" -> %s", cname);
        } else if (cname[0] != '\0') {
            printf("\n: %s", cname);
        }
    } else {
        if (dent->hmd_type == HM_TYPE_DIR) {
            if ((flags & LS_FLAG_CLASSIFY) && (noslash == 0))
                printf("/");
        } else if (dent->hmd_type == HM_TYPE_LINK) {
            if (flags & LS_FLAG_CLASSIFY)
                printf("@");
        }
    }
    printf("\n");
    return (entlen);
}

static rc_t
ls_show(const char *name, uint flags)
{
    handle_t handle;
    uint     type;
    uint     rlen;
    uint     pos;
    uint     rc;
    uint     open_mode = HM_MODE_READ;
    uint     entlen;
    uint8_t *data;
    hm_fdirent_t *dent;

    if (flags & LS_FLAG_DIRENT) {
        /* Open file or dir as directory entry (like STAT) */
        open_mode = HM_MODE_READDIR;
        open_mode |= HM_MODE_NOFOLLOW;
    }

    /* Open directory */
try_open_again:
    rc = sm_fopen(cwd_handle, name, open_mode, &type, 0, &handle);

    if ((handle == 0) && ((open_mode & HM_MODE_DIR) == 0)) {
        // XXX: is this a wildcard? might need to open as dir so
        //      remote can do wildcard match or show files which can not
        //      be opened (FIFO, BDEV, file with no read permission, etc.)
        //
        // I think I'd like to push the wildcard processing to the remote
        /* Open as directory entries */
        open_mode = HM_MODE_READDIR;
        goto try_open_again;
    }
    if (rc != KM_STATUS_OK) {
        printf("Failed to open %s: %s\n", name, smash_err(rc));
        return (RC_FAILURE);
    }
    if (((type != HM_TYPE_DIR) && (type != HM_TYPE_VOLDIR)) &&
        ((open_mode & HM_MODE_DIR) == 0)) {
//      printf("not a dir (%u)\n", type);
        sm_fclose(handle);
        open_mode = HM_MODE_READDIR;
        goto try_open_again;
    }
    while (1) {
        rc = sm_fread(handle, DIRBUF_SIZE, (void **) &data, &rlen, 0);
        if ((rlen == 0) && (rc != KM_STATUS_EOF)) {
            printf("Dir read failed: %s\n", smash_err(rc));
            goto ls_fail;
        }
#ifdef LS_DUMP_READ
        dump_memory(data, rlen, VALUE_UNASSIGNED);
#endif
        for (pos = 0; pos < rlen; ) {
            dent = (hm_fdirent_t *)(((uintptr_t) data) + pos);
            entlen = show_dirent(dent, flags);
            if (entlen == 0)
                break;
            pos += sizeof (*dent) + entlen;

            if (is_user_abort()) {
                printf("^C\n");
                sm_fclose(handle);
                return (RC_USR_ABORT);
            }
        }
        if (rc == KM_STATUS_EOF) {
            rc = RC_SUCCESS;
            break;  // End of directory reached
        }
    }
ls_fail:
    sm_fclose(handle);
    if (rc == 0)
        return (RC_SUCCESS);
    else
        return (RC_FAILURE);
}

rc_t
cmd_ls(int argc, char * const *argv)
{
    int arg;
    rc_t rc = RC_SUCCESS;
    rc_t rc2;
    uint flags = 0;
    uint did_show = 0;

    if (strcmp(argv[0], "list") == 0)
        flags |= LS_FLAG_LIST;
    else if (strcmp(argv[0], "dir") == 0)
        flags |= LS_FLAG_DIR;

    for (arg = 1; arg < argc; arg++) {
        const char *ptr = argv[arg];
        if (*ptr == '-') {
            while (*(++ptr) != '\0') {
                switch (*ptr) {
                    case 'a':
                    case 'A':
                        flags |= LS_FLAG_ALL;  // all
                        break;
                    case 'c':
                        flags |= LS_FLAG_CTIME;  // creation time
                        flags |= LS_FLAG_LONG;   // long
                        break;
                    case 'd':
                        flags |= LS_FLAG_DIRENT;    // directory entry
                        break;
                    case 'F':
                        flags |= LS_FLAG_CLASSIFY;  // classify ( dir/ )
                        break;
                    case 'l':
                        flags |= LS_FLAG_LONG;   // long (Unix ls -l))
                        break;
                    case 'L':
                        flags |= LS_FLAG_LIST;   // list (Amiga list)
                        break;
                    case 'u':
                        flags |= LS_FLAG_ATIME;  // access time
                        flags |= LS_FLAG_LONG;   // long
                        break;
                    default:
                        printf("Unknown argument -%s\n"
                               "Usage:\n", ptr);
                        printf("    %s -A  - show . and ..\n"
                               "    %s -c  - show file creation time\n"
                               "    %s -d  - show directory itself instead "
                                            "of contents\n"
                               "    %s -l  - show long listing with file "
                                            "size and date\n"
                               "    %s -u  - show file last access time\n",
                               argv[0], argv[0], argv[0], argv[0], argv[0]);
                        return (RC_BAD_PARAM);
                }
            }
        }
    }
    for (arg = 1; arg < argc; arg++) {
        const char *ptr = argv[arg];
        if (*ptr != '-') {
            did_show++;
            rc2 = ls_show(ptr, flags);
            if (rc == RC_SUCCESS)
                rc = rc2;
        }
    }
    if (did_show == 0) {
        rc2 = ls_show(".", flags);
        if (rc == RC_SUCCESS)
            rc = rc2;
    }
    return (rc);
}

rc_t
lmkdir_work(const char *name, uint flag_path)
{
    BPTR lock;
    lock = CreateDir(name);
    if (lock == 0) {
        rc_t rc = RC_FAILURE;
        if (flag_path) {
            char *tname = strdup(name);
            char *ptr = tname;

            if (tname == NULL)
                return (RC_FAILURE);

            /* Go to end of name string */
            while (*ptr != '\0')
                ptr++;

            /* Remove final directory element and try again */
            while (--ptr > tname) {
                if (*ptr == '/') {
                    *ptr = '\0';
                    rc = lmkdir_work(tname, flag_path);
                    break;
                }
            }
            free(tname);
            if (rc == RC_SUCCESS) {
                lock = CreateDir(name);
                if (lock == 0)
                    return (RC_FAILURE);
                UnLock(lock);
                return (RC_SUCCESS);
            }
        }
        if (rc != RC_SUCCESS) {
            lock = Lock(name, ACCESS_READ);
            printf("Failed to create %s", name);
            if (lock != 0)
                printf(": object exists");
            printf("\n");
            if (lock != 0)
                UnLock(lock);
        }
        return (rc);
    }
    UnLock(lock);
    return (RC_SUCCESS);
}

#ifdef ALLOW_CREATE_LINK
rc_t
cmd_ln(int argc, char * const *argv)
{
    int arg;
    const char *name_tgt = NULL;
    const char *name     = NULL;
    uint do_remote = 0;
    uint flag_hard_link = 0;
    rc_t rc;

    if ((strcmp(argv[0], "ln") == 0) || (strcmp(argv[0], "makelink") == 0))
        do_remote = 1;

    for (arg = 1; arg < argc; arg++) {
        const char *ptr = argv[arg];
        if (*ptr == '-') {
            while (*(++ptr) != '\0') {
                switch (*ptr) {
                    case 'h':
                        flag_hard_link++;
                        break;
                    case 's':
                        /* soft link (symlink) is default */
                        break;
                    default:
                        printf("Unknown argument -%s\n"
                               "Usage:\n", ptr);
                        printf("    %s <tgt> <new>\n", argv[0]);
                        return (RC_BAD_PARAM);
                }
            }
        }
    }
    for (arg = 1; arg < argc; arg++) {
        const char *ptr = argv[arg];
        if (*ptr != '-') {
            if (name_tgt == NULL) {
                name_tgt = ptr;
            } else if (name == NULL) {
                name = ptr;
            } else {
                printf("Too many arguments to %s: '%s' '%s' and '%s'\n",
                       argv[0], name_tgt, name, ptr);
                return (RC_FAILURE);
            }
        }
    }
    if (name == NULL) {
        printf("Need to supply a target name and a new filename\n");
        return (RC_USER_HELP);
    }
    if (do_remote) {
        uint linktype = flag_hard_link ? HM_TYPE_HLINK : HM_TYPE_LINK;
        rc = sm_fcreate(cwd_handle, name, name_tgt, linktype, 0);
        if (rc != RC_SUCCESS) {
            printf("Failed to link %s to existing %s: %s\n",
                   name, name_tgt, smash_err(rc));
        }
    } else {
        rc = RC_SUCCESS;
        BPTR dest;
        if (flag_hard_link) {
            dest = Lock(name_tgt, SHARED_LOCK);
        } else {
            dest = (BPTR) name_tgt;
            if (dest == 0) {
                printf("Failed to open %s\n", name_tgt);
                return (RC_FAILURE);
            }
        }
        if (MakeLink(name, dest, !flag_hard_link) == 0) {
            printf("Failed to create link %s to %s\n", name, name_tgt);
            rc = RC_FAILURE;
        }
    }
    return (rc);
}
#endif /* ALLOW_CREATE_LINK */

rc_t
cmd_lrm(int argc, char * const *argv)
{
    int arg;
    uint did_rm = 0;
    rc_t rc = RC_SUCCESS;

    for (arg = 1; arg < argc; arg++) {
        const char *ptr = argv[arg];
        if (*ptr == '-') {
            while (*(++ptr) != '\0') {
                switch (*ptr) {
                    default:
                        printf("Unknown argument -%s\n"
                               "Usage:\n", ptr);
                        printf("    %s <path...>\n", argv[0]);
                        return (RC_BAD_PARAM);
                }
            }
        }
    }
    for (arg = 1; arg < argc; arg++) {
        const char *name = argv[arg];
        if (*name != '-') {
            BPTR lock;
            struct FileInfoBlock fib;

            did_rm++;
            lock = Lock(name, SHARED_LOCK);
            if (lock == 0) {
                printf("Failed to lock %s\n", name);
                rc = RC_FAILURE;
                continue;
            }
            if (Examine(lock, &fib) == 0) {
                printf("Failed to examine %s\n", name);
                UnLock(lock);
                rc = RC_FAILURE;
                continue;
            }
            UnLock(lock);
            if (fib.fib_DirEntryType >= 0) {
                printf("%s is not a file\n", name);
                rc = RC_FAILURE;
                continue;
            }

            if (DeleteFile(name) == 0) {
                printf("Failed to delete %s\n", name);
                return (RC_FAILURE);
            }
        }
    }
    if (did_rm == 0) {
        printf("Need to supply at least one filename to delete\n");
        return (RC_USER_HELP);
    }
    return (rc);
}

rc_t
lrmdir_work(const char *name, uint flag_path)
{
    BPTR lock;
    struct FileInfoBlock fib;

    lock = Lock(name, SHARED_LOCK);
    if (lock == 0) {
        printf("Failed to lock %s\n", name);
        return (RC_FAILURE);
    }
    if (Examine(lock, &fib) == 0) {
        printf("Failed to examine %s\n", name);
        UnLock(lock);
        return (RC_FAILURE);
    }
    UnLock(lock);

    if (fib.fib_DirEntryType < 0) {
        printf("%s is not a directory\n", name);
        return (RC_FAILURE);
    }

    if (DeleteFile(name) == 0) {
        printf("Failed to delete %s\n", name);
        return (RC_FAILURE);
    }

    if (flag_path) {
        rc_t rc = RC_SUCCESS;
        char *ptr;
        char *tname = strdup(name);

        if (tname == NULL)
            return (RC_FAILURE);
        for (ptr = tname; *ptr != '\0'; ptr++)
            ;
        for (--ptr; ptr > tname; ptr--) {
            if (*ptr == '/') {
                *ptr = '\0';
                rc = lrmdir_work(tname, 0);
                if (rc != RC_SUCCESS)
                    break;
            }
        }
        free(tname);
    }
    return (RC_SUCCESS);
}

rc_t
cmd_lrmdir(int argc, char * const *argv)
{
    int  arg;
    rc_t rc;
    uint flag_path = 0;
    uint did_rmdir = 0;

    for (arg = 1; arg < argc; arg++) {
        const char *ptr = argv[arg];
        if (*ptr == '-') {
            while (*(++ptr) != '\0') {
                switch (*ptr) {
                    case 'p':
                        flag_path++;  // delete full path
                        break;
                    default:
                        printf("Unknown argument -%s\n"
                               "Usage:\n", ptr);
                        printf("    %s [-p] <path...>\n", argv[0]);
                        return (RC_BAD_PARAM);
                }
            }
        }
    }
    for (arg = 1; arg < argc; arg++) {
        const char *ptr = argv[arg];
        if (*ptr != '-') {
            did_rmdir++;
            rc = lrmdir_work(ptr, flag_path);
            if (rc != RC_SUCCESS)
                return (rc);
        }
    }
    if (did_rmdir == 0) {
        printf("Need to supply at least one directory to delete\n");
        return (RC_USER_HELP);
    }
    return (RC_SUCCESS);
}

rc_t
mkdir_work(const char *name, uint flag_path)
{
    rc_t rc;

    rc = sm_fcreate(cwd_handle, name, "", HM_TYPE_DIR, 0);
    if (rc != RC_SUCCESS) {
        if (flag_path) {
            char *tname = strdup(name);
            char *ptr = tname;

            if (tname == NULL)
                return (RC_FAILURE);

            /* Go to end of name string */
            while (*ptr != '\0')
                ptr++;

            /* Remove final directory element and try again */
            while (--ptr > tname) {
                if (*ptr == '/') {
                    *ptr = '\0';
                    rc = mkdir_work(tname, flag_path);
                    break;
                }
            }
            free(tname);
            if (rc == RC_SUCCESS) {
                rc = sm_fcreate(cwd_handle, name, "", HM_TYPE_DIR, 0);
                if (rc != RC_SUCCESS)
                    return (rc);
                return (RC_SUCCESS);
            }
        }
        if (rc != RC_SUCCESS) {
            printf("Failed to create %s: %s\n", name, smash_err(rc));
        }
        return (rc);
    }
    return (RC_SUCCESS);
}

/*
 * cmd_mkdir
 * ---------
 * This function handles both local (lmkdir) and remote (mkdir) commands.
 */
rc_t
cmd_mkdir(int argc, char * const *argv)
{
    int  arg;
    rc_t rc;
    uint flag_path = 0;
    uint did_mkdir = 0;
    uint do_remote = 0;

    if ((strcmp(argv[0], "mkdir") == 0) || (strcmp(argv[0], "makedir") == 0))
        do_remote = 1;

    for (arg = 1; arg < argc; arg++) {
        const char *ptr = argv[arg];
        if (*ptr == '-') {
            while (*(++ptr) != '\0') {
                switch (*ptr) {
                    case 'p':
                        flag_path++;  // create full path
                        break;
                    default:
                        printf("Unknown argument -%s\n"
                               "Usage:\n", ptr);
                        printf("    %s [-p] <path...>\n", argv[0]);
                        return (RC_BAD_PARAM);
                }
            }
        }
    }
    for (arg = 1; arg < argc; arg++) {
        const char *ptr = argv[arg];
        if (*ptr != '-') {
            did_mkdir++;
            if (do_remote)
                rc = mkdir_work(ptr, flag_path);
            else
                rc = lmkdir_work(ptr, flag_path);
            if (rc != RC_SUCCESS)
                return (rc);
        }
    }
    if (did_mkdir == 0) {
        printf("Need to supply at least one directory to create\n");
        return (RC_USER_HELP);
    }
    return (RC_SUCCESS);
}

rc_t
cmd_mv(int argc, char * const *argv)
{
    int arg;
    const char *name_old = NULL;
    const char *name_new = NULL;
    uint do_remote = 0;

    if ((strcmp(argv[0], "mv") == 0) || (strncmp(argv[0], "rename", 3) == 0))
        do_remote = 1;

    for (arg = 1; arg < argc; arg++) {
        const char *ptr = argv[arg];
        if (*ptr == '-') {
            while (*(++ptr) != '\0') {
                switch (*ptr) {
                    default:
                        printf("Unknown argument -%s\n"
                               "Usage:\n", ptr);
                        printf("    %s <old> <new>\n", argv[0]);
                        return (RC_BAD_PARAM);
                }
            }
        }
    }
    for (arg = 1; arg < argc; arg++) {
        const char *ptr = argv[arg];
        if (*ptr != '-') {
            if (name_old == NULL) {
                name_old = ptr;
            } else if (name_new == NULL) {
                name_new = ptr;
            } else {
                printf("Too many arguments to %s: '%s' '%s' and '%s'\n",
                       argv[0], name_old, name_new, ptr);
                return (RC_FAILURE);
            }
        }
    }
    if (name_new == NULL) {
        printf("Need to supply a filename to rename and new name\n");
        return (RC_USER_HELP);
    }
    if (do_remote) {
        return (sm_frename(cwd_handle, name_old, cwd_handle, name_new));
    } else {
        if (Rename(name_old, name_new) == 0) {
            printf("Failed to rename %s to %s\n", name_old, name_new);
            return (RC_FAILURE);
        }
        return (RC_SUCCESS);
    }
}

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

static rc_t
put_file(const char *src, const char *dst)
{
    int      bytes;
    uint     diff;
    uint     filesize;
    uint     rc;
    uint     rlen;
    uint     type;
    uint     buflen = 32768;
    char    *bufptr;
    char    *bufdata;
    FILE    *fp;
    handle_t handle;
    uint64_t pos = 0;
    uint64_t time_start;
    uint64_t time_end;
    struct FileInfoBlock fib;

    if (get_file_fib(src, &fib)) {
        printf("Failed to open %s for STAT\n", src);
        return (RC_FAILURE);
    }

    filesize = fib.fib_Size;

    fp = fopen(src, "r");
    if (fp == NULL) {
        printf("Failed to open %s for read\n", dst);
        return (RC_FAILURE);
    }

    rc = sm_fopen(cwd_handle, dst, HM_MODE_WRITE | HM_MODE_CREATE,
                 &type, fib.fib_Protection, &handle);
    if (rc != KM_STATUS_OK) {
        printf("Failed to open %s for write: %s\n", src, smash_err(rc));
        fclose(fp);
        return (RC_FAILURE);
    }
    bufptr = malloc(buflen + sizeof (hm_freadwrite_t));
    if (bufptr == NULL) {
        printf("Failed to allocate %u bytes\n",
               buflen + sizeof (hm_freadwrite_t));
        fclose(fp);
        sm_fclose(handle);
        return (RC_FAILURE);
    }
    printf("Put %s as %s ", src, dst);
    bufdata = bufptr + sizeof (hm_freadwrite_t);
    if (filesize < 1000000)
        printf("(%u bytes) ", (uint) filesize);
    else
        printf("(%u KB) ", (uint) ((filesize + 512) >> 10));

    rc = RC_SUCCESS;
    time_start = smash_time();
    while (pos < filesize) {
        if (is_user_abort()) {
            printf("^C\n");
            fclose(fp);
            sm_fclose(handle);
            return (RC_USR_ABORT);
        }
        rlen = buflen;
        if (rlen > filesize - pos)
            rlen = filesize - pos;
        bytes = fread(bufdata, 1, rlen, fp);
        if (bytes < rlen) {
            printf("Failed to read all bytes from %s at pos %x\n",
                   src, (uint) pos);
            rc = RC_FAILURE;
            break;
        }
        rc = sm_fwrite(handle, bufptr, bytes, 1, 0);
        if (rc != KM_STATUS_OK) {
            printf("Remote write %s failed at pos %x: %s\n",
                   dst, (uint) pos, smash_err(rc));
            rc = RC_FAILURE;
            break;
        }

        pos += bytes;
    }

    time_end = smash_time();
    diff = (uint) (time_end - time_start);
    if (flag_debug)
        printf("%u usec  ", diff);
    printf(" %u KB/sec\n", calc_kb_sec(diff, filesize));
    fclose(fp);
    sm_fclose(handle);
    return (rc);
}

static rc_t
put_files(const char *src, const char *dst)
{
    rc_t     rc;
    char    *dstbuf = NULL;

    if (dst == NULL) {
        /* Need to trim dst file name from src name */
        for (dst = src; *dst != '\0'; dst++)
            ;
        if (dst > src)
            dst--;
        if (*dst == '/') {
            printf("Can not yet put directory: %s\n", src);
            // XXX: This would be a nice feature to add.
            //      Recursive is too much work, however.
            //
            //      Would also be nice to be able to support wildcards
            //      that could be implemented on top of remote directory
            //      support.
            return (RC_FAILURE);
        }
        for (; dst > src; dst--)
            if ((dst[-1] == '/') || (dst[-1] == ':'))
                break;
    }
//  printf("src='%s' dst='%s'\n", src, dst);
    if (is_dir(src)) {
        printf("Can not yet put directory\n");
        return (RC_FAILURE);
    }
    if (is_remote_dir(dst)) {
        uint dstlen = strlen(dst);
        uint alloclen = dstlen + strlen(src) + 2;
        dstbuf = malloc(alloclen);
        if (dstbuf == NULL) {
            printf("malloc(%u) failure\n", alloclen);
            return (RC_FAILURE);
        }
        strcpy(dstbuf, dst);
        if ((dstlen > 0) && (dstbuf[dstlen - 1] != '/') &&
                            (dstbuf[dstlen - 1] != ':')) {
            dstbuf[dstlen++] = '/';
        }
        strcpy(dstbuf + dstlen, src);
        dst = dstbuf;
    }

    /* Simple file put (for now) */
    rc = put_file(src, dst);

    if (dstbuf != NULL)
        free(dstbuf);

    return (rc);
}

rc_t
cmd_put(int argc, char * const *argv)
{
    int         arg;
    uint        rc;
    const char *readas = NULL;
    const char *putas = NULL;

    for (arg = 1; arg < argc; arg++) {
        const char *ptr = argv[arg];
        if (*ptr == '-') {
            while (*(++ptr) != '\0') {
                switch (*ptr) {
                    default:
                        printf("Unknown argument -%s\n", ptr);
                        printf(cmd_put_help);
                        return (RC_BAD_PARAM);
                }
            }
        }
    }
    for (arg = 1; arg < argc; arg++) {
        const char *ptr = argv[arg];
        if (*ptr != '-') {
            if (readas == NULL) {
                readas = ptr;
                continue;
            } else if (putas == NULL) {
                putas = ptr;
                continue;
            } else {
                /* put multiple */
                char *dst = NULL;
                char *final = argv[argc - 1];

                if (is_remote_dir(final)) {
                    dst = final;
                    argc--;
                }

                rc = put_files(readas, dst);
                if (rc != RC_SUCCESS)
                    return (rc);
                rc = put_files(putas, dst);
                if (rc != RC_SUCCESS)
                    return (rc);
                for (; arg < argc; arg++) {
                    rc = put_files(argv[arg], dst);
                    if (rc != RC_SUCCESS)
                        return (rc);
                }
                return (rc);
            }
        }
    }

    if (is_user_abort()) {
        printf("^C\n");
        return (RC_USR_ABORT);
    }

    if (readas != NULL) {
        rc = put_files(readas, putas);
    } else {
        printf(cmd_put_help);
        return (RC_BAD_PARAM);
    }
    return (rc);
}

rc_t
cmd_pwd(int argc, char * const *argv)
{
    UNUSED(argc);
    UNUSED(argv);
    printf("%s\n", cwd);
    return (RC_SUCCESS);
}

#define RM_TYPE_ANY  0
#define RM_TYPE_FILE 1
#define RM_TYPE_DIR  2

static rc_t
rm_object(const char *name, uint rm_type)
{
    uint rc;
    uint type;
    handle_t handle;

    if (rm_type != RM_TYPE_ANY) {
        rc = sm_fopen(cwd_handle, name, HM_MODE_READDIR, &type, 0, &handle);
        if (rc != KM_STATUS_OK) {
            printf("Failed to open %s: %s\n", name, smash_err(rc));
            return (RC_FAILURE);
        }

        if ((rm_type == RM_TYPE_FILE) &&
            ((type == HM_TYPE_DIR) || (type == HM_TYPE_VOLUME) ||
             (type == HM_TYPE_VOLDIR))) {
            printf("%s is not a file (%x)\n", name, type);
            sm_fclose(handle);
            return (RC_FAILURE);
        }
        if ((rm_type == RM_TYPE_DIR) &&
            (type != HM_TYPE_DIR) && (type != HM_TYPE_VOLUME) &&
            (type != HM_TYPE_VOLDIR)) {
            printf("%s is not a directory (%x)\n", name, type);
            sm_fclose(handle);
            return (RC_FAILURE);
        }
        sm_fclose(handle);
    }
    return (sm_fdelete(cwd_handle, name));
}

rc_t
cmd_rm(int argc, char * const *argv)
{
    int arg;
    uint rm_type;
    uint did_rm = 0;
    rc_t rc = RC_SUCCESS;
    rc_t rc2;

    if (strncmp(argv[0], "delete", 3) == 0)
        rm_type = RM_TYPE_ANY;
    else if (strcmp(argv[0], "rmdir") == 0)
        rm_type = RM_TYPE_DIR;
    else
        rm_type = RM_TYPE_FILE;  // "rm"

    for (arg = 1; arg < argc; arg++) {
        const char *ptr = argv[arg];
        if (*ptr == '-') {
            while (*(++ptr) != '\0') {
                switch (*ptr) {
                    default:
                        printf("Unknown argument -%s\n"
                               "Usage:\n", ptr);
                        printf("    %s <path...>\n", argv[0]);
                        return (RC_BAD_PARAM);
                }
            }
        }
    }
    for (arg = 1; arg < argc; arg++) {
        const char *name = argv[arg];
        if (*name != '-') {
            did_rm++;
            rc2 = rm_object(name, rm_type);
            if (rc == RC_SUCCESS)
                rc = rc2;
        }
    }
    if (did_rm == 0) {
        printf("Need to supply at least one filename to delete\n");
        return (RC_USER_HELP);
    }
    return (rc);
}

rc_t
cmd_time(int argc, char * const *argv)
{
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
    printf("%u ms\n", (uint)diff_dstamp(&etime, &stime));
    if (rc == RC_USER_HELP)
        rc = RC_FAILURE;

    return (rc);
}

rc_t
cmd_version(int argc, char * const *argv)
{
    UNUSED(argc);
    UNUSED(argv);
    printf("%s\n", version + 7);
    return (RC_SUCCESS);
}

int
main(int argc, char *argv[])
{
    int rc;
    char *cmdbuf;

    cwd = strdup("");

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds="
    SysBase = *(struct ExecBase **)4UL;
#pragma GCC diagnostic pop


    cpu_control_init();  // cpu_type, SysBase

    cmdbuf = cmd_string_from_argv(argc - 1, argv + 1);

    if (cmdbuf == NULL) {
        rc = cmdline();
    } else {
        rc = cmd_exec_string(cmdbuf);
        free(cmdbuf);
    }
    if (cwd != NULL)
        free(cwd);
    if (cwd_handle != 0xffffffff)
        sm_fclose(cwd_handle);
    if (save_currentdir != 0) {
        BPTR old_lock = CurrentDir(save_currentdir);
        UnLock(old_lock);
    }
    exit(rc);
}
