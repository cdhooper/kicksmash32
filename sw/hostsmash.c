/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2024.
 *
 * ---------------------------------------------------------------------
 *
 * UNIX side to interact with Amiga Kicksmash
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <sys/time.h>
#ifdef __MINGW32__
#include <time.h>
#include <sys/utime.h>
#define handle_t winhandle_t
#include <shlwapi.h>
#define strcasestr StrStrI
#undef handle_t
typedef unsigned int uint;
#else
#include <termios.h>
#include <sys/ioctl.h>
#include <err.h>
#include <poll.h>
#include <sys/statvfs.h>
#endif
#include <sys/file.h>
#include <signal.h>
#include <string.h>
#include <getopt.h>
#include <limits.h>
#define _GNU_SOURCE
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <ctype.h>
#include <inttypes.h>
#ifdef LINUX
#include <usb.h>
#endif
#include <dirent.h>
#include "../fw/crc32.h"
#include "../fw/smash_cmd.h"
#include "../amiga/host_cmd.h"
#include "../fw/version.h"
#include "hostsmash.h"
#include "hostsmash_fs.h"
#include "hostsmash_net.h"
#include "hostsmash_dfu.h"
#include "hostsmash_windrv.h"

#define MSG_DEBUG
#ifndef MSG_DEBUG
#define msgprintf(args...) do { } while (0)
#endif

/* Program long format options */
static const struct option long_opts[] = {
    { "all",      no_argument,       NULL, 'A' },
    { "addr",     required_argument, NULL, 'a' },
    { "bank",     required_argument, NULL, 'b' },
    { "clock",    required_argument, NULL, 'c' },
    { "delay",    required_argument, NULL, 'D' },
    { "device",   required_argument, NULL, 'd' },
    { "debugfs",  no_argument,       NULL, 0x80 + 'f' },
    { "debugmsg", no_argument,       NULL, 0x80 + 'm' },
    { "debugnet", no_argument,       NULL, 0x80 + 'n' },
    { "erase",    no_argument,       NULL, 'e' },
    { "fill",     no_argument,       NULL, 'f' },
    { "fwread",   required_argument, NULL, 'R' },
    { "fwupdate", required_argument, NULL, 'W' },
    { "fwverify", required_argument, NULL, 'F' },
    { "fwwrite",  required_argument, NULL, 'W' },
    { "identify", no_argument,       NULL, 'i' },
    { "help",     no_argument,       NULL, 'h' },
    { "len",      required_argument, NULL, 'l' },
    { "mount",    required_argument, NULL, 'm' },
    { "Mount",    required_argument, NULL, 'M' },
    { "net",      required_argument, NULL, 'n' },
    { "read",     no_argument,       NULL, 'r' },
    { "serial",   required_argument, NULL, 0x80 + 'S' },
    { "swap",     required_argument, NULL, 's' },
    { "term",     no_argument,       NULL, 't' },
    { "verify",   no_argument,       NULL, 'v' },
    { "version",  no_argument,       NULL, 'V' },
    { "write",    no_argument,       NULL, 'w' },
    { "yes",      no_argument,       NULL, 'y' },
    { NULL,       no_argument,       NULL,  0  }
};

static char short_opts[] = {
    ':',         // Missing argument
    'A',         // --all
    'V',         // --version
    'a', ':',    // --addr <addr>
    'b', ':',    // --bank <num>
    'c', ':',    // --clock [show|set]
    'D', ':',    // --delay <num>
    'F', ':',    // --fwverify <filename>
    'd', ':',    // --device <filename>
    'e',         // --erase
    'f',         // --fill
    'h',         // --help
    'i',         // --identify
    'l', ':',    // --len <num>
    'm', ':',    // --mount <vol> <dir>
    'M', ':',    // --Mount <vol> <dir>
    'n', ':',    // --net <args>
    'r',         // --read <filename>
    'R', ':',    // --fwread <filename>
    's', ':',    // --swap <mode>
    't',         // --term
    'v',         // --verify <filename>
    'W', ':',    // --fwwrite <filename>
    'w',         // --write <filename>
    'y',         // --yes
    '\0'
};

/* Program help text */
static const char usage_text[] =
"hostsmash <opts> <dev>\n"
"    -A --all                show all verify miscompares\n"
"    -V --version            display version\n"
"    -a --addr <addr>        starting EEPROM address\n"
"    -b --bank <num>         starting EEPROM address as multiple of file size\n"
"    -c --clock [show|set]   show or set Kicksmash time of day clock\n"
"    -D --delay <msec>       pacing delay between sent characters (ms)\n"
"    -d --device <filename>  serial device to use (e.g. /dev/ttyACM0)\n"
#ifdef FILE_DEBUG
"       --debugfs            debug filesystem operations\n"
#endif
"       --debugmsg           debug Amiga messages\n"
"       --debugnet           debug network operations\n"
"    -e --erase              erase EEPROM (use -a <addr> for sector erase)\n"
"    -F --fwverify <filename> verify Kicksmash firmware matches file\n"
"    -f --fill               fill EEPROM with duplicates of the same image\n"
"    -h --help               display usage\n"
"    -i --identify           identify installed EEPROM\n"
"    -l --len <num>          length in bytes\n"
"    -m --mount <vol:> <dir> file serve directory path to Amiga volume\n"
"    -n --net <args>         network service for Amiga; <args> are passed\n"
"                            to hostsmash_netif, split at spaces, such as\n"
"                            -n macvtap or -n \"-d macvtap eth0\"\n"
"                            (see hostsmash_netif -h)\n"
"    -r --read <filename>    read EEPROM and write to file\n"
"    -R --fwread <filename>  read Kicksmash firmware and write to file\n"
"    -s --swap <mode>        byte swap mode (2301, 3210, 1032, noswap=0123)\n"
"       --serial <serial>    select Kicksmash by USB serial number (or, for\n"
"                            --fwwrite to a DFU device, by USB port path)\n"
"    -v --verify <filename>  verify file matches EEPROM contents\n"
"    -W --fwwrite <filename> write Kicksmash firmware from file (DFU)\n"
"    -w --write <filename>   read file and write to EEPROM\n"
"    -t --term [<command>]   operate in terminal mode (CLI) to KickSmash\n"
"    -y --yes                answer all prompts with 'yes'\n"
"    TERM_DEBUG=`tty`        env variable for communication debug output\n"
"    TERM_DEBUG_HEX=1        show debug output in hex instead of ASCII\n"
"\n"
"Example (including specific TTY to open):\n"
#if defined(__MINGW32__)
"    hostsmash -d com5 -t\n"
#elif defined(OSX)
"    hostsmash -d /dev/cu.usbmodem* -t\n"
#else
"    hostsmash -d /dev/ttyACM0 -t\n"
#endif
"";

#define HOST_INTERFACE_VERSION 1
uint8_t amiga_interface_version = 0;

uint        debug_fs = 0;
static uint debug_msg = 0;
uint        debug_net = 0;
static uint ks_message_terminated = 0;  // Stop execution when non-zero

#ifdef MSG_DEBUG
ATTRIBUTE_PRINTF
int
msgprintf(const char *fmt, ...)
{
    int rc = 0;
    va_list args;

    if (debug_msg) {
        va_start(args, fmt);
        rc = vprintf(fmt, args);
        va_end(args);
    }

    return (rc);
}
#endif

ATTRIBUTE_PRINTF
int
netprintf(const char *fmt, ...)
{
    int rc = 0;
    va_list args;

    if (debug_net) {
        va_start(args, fmt);
        rc = vprintf(fmt, args);
        va_end(args);
    }

    return (rc);
}

/* Amiga time is in seconds since 1978 */
#define AMIGA_SEC_TO_UNIX_SEC (2922 * 24 * 60 * 60)  // 1978 - 1970 = 2922 days

/* Command line modes which may be specified by the user */
#define MODE_UNKNOWN   0x0000
#define MODE_ERASE     0x0001
#define MODE_ID        0x0002
#define MODE_READ      0x0004
#define MODE_TERM      0x0008
#define MODE_VERIFY    0x0010
#define MODE_WRITE     0x0020
#define MODE_MSG       0x0040
#define MODE_CLOCK_GET 0x0100
#define MODE_CLOCK_SET 0x0200
#define MODE_ID_KS     0x0400
#define MODE_VERSION   0x0800
#define MODE_FWREAD    0x1000
#define MODE_FWVERIFY  0x2000
#define MODE_FWWRITE   0x4000

/* XXX: Need to register USB device ID at http://pid.codes */
#define MX_VENDOR 0x1209
#define MX_DEVICE 0x1610

#define EEPROM_SIZE_DEFAULT       0x400000    // 4 MB
#define EEPROM_BANK_SIZE_DEFAULT  0x080000    // 512 KB
#define EEPROM_SIZE_NOT_SPECIFIED 0xffffffff
#define BANK_NOT_SPECIFIED        0xffffffff
#define ADDR_NOT_SPECIFIED        0xffffffff

#define DATA_CRC_INTERVAL         256  // How often CRC is sent (bytes)

/* Enable for gdb debug */
#undef DEBUG_CTRL_C_KILL

/* Enable for non-blocking tty input */
#undef USE_NON_BLOCKING_TTY

#ifndef EXIT_USAGE
#define EXIT_USAGE 2
#endif

#define KICKSMASH_MODE_32     0  // Swap mode 3210 (Amiga 3000/4000)
#define KICKSMASH_MODE_16     1  // Swap mode 1032 (Amiga 500/2000)
#define KICKSMASH_MODE_16HI   2  // Swap mode 1032 (Amiga 500/2000)
#define KICKSMASH_MODE_AUTO   3  // Swap mode 3210 (Amiga 3000/4000)
#define KICKSMASH_MODE_32SWAP 4  // Swap mode 1032 (Amiga 3000T)

#define SWAPMODE_AUTO   0xa040   // Automatic mode
#define SWAPMODE_16     0x0010   // Amiga 16-bit ROM format
#define SWAPMODE_32SWAP 0x1032   // Amiga 32-bit ROM format hi-lo swapped
#define SWAPMODE_32     0x3210   // Amiga 32-bit ROM format

#define SWAP_TO_ROM    0  // Bytes originated in a file (to be written in ROM)
#define SWAP_FROM_ROM  1  // Bytes originated in ROM (to be written to a file)

typedef unsigned int uint;

static void discard_input(int timeout);

#undef TRUE
#undef FALSE
typedef enum {
    TRUE  = 1,
    FALSE = 0,
} bool_t;

/*
 * ARRAY_SIZE() provides a count of the number of elements in an array.
 *              This macro works the same as the Linux kernel header
 *              definition of the same name.
 */
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(array) ((size_t) (sizeof (array) / sizeof ((array)[0])))
#endif
#define RX_RING_SIZE 8192
#define TX_RING_SIZE 4096
static volatile uint8_t rx_rb[RX_RING_SIZE];
static volatile uint    rx_rb_producer    = 0;
static volatile uint    rx_rb_consumer    = 0;
static volatile uint8_t  tx_rb[TX_RING_SIZE];
static volatile uint    tx_rb_producer = 0;
static volatile uint    tx_rb_consumer = 0;

/*
 * Serial writer wakeup. th_serial_writer() sleeps on tx_rb_cv while the
 * Tx ring is empty, and is woken by tx_rb_kick() once a block of output
 * has been queued, instead of polling the ring every 10 ms. A timed wait
 * is still used so that any producer which does not kick is serviced
 * within 10 ms, as before.
 */
static pthread_mutex_t  tx_rb_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   tx_rb_cv  = PTHREAD_COND_INITIALIZER;

/*
 * Define KS_RX_WAKEUP to have recv_ks_reply_core() sleep on rx_rb_cv
 * (woken by th_serial_reader() as data arrives) instead of polling the
 * Rx ring with a 1 ms delay.
 */
#define KS_RX_WAKEUP
#ifdef KS_RX_WAKEUP
static pthread_mutex_t  rx_rb_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   rx_rb_cv  = PTHREAD_COND_INITIALIZER;
#endif
#ifdef __MINGW32__
static HANDLE           dev_handle        = INVALID_HANDLE_VALUE;
static volatile bool    dev_write_failed  = FALSE;  // Writer saw error
#else
static int              dev_fd            = -1;
static int              got_terminfo      = 0;
static struct termios   saved_term;  // good terminal settings
#endif
static int              running           = 1;
static uint             ic_delay          = 0;  // Pacing delay (ms)
static char             device_name[PATH_MAX];
static char            *host_device_name  = device_name;
static bool             terminal_mode     = FALSE;
static bool             force_yes         = FALSE;
static uint             swapmode          = SWAPMODE_AUTO;
static uint             kicksmash_mode    = KICKSMASH_MODE_AUTO;
static char            *terminal_cmd      = NULL;
static uint             amiga_net_service = 0;
static bool             reopen_quiet      = FALSE;  // Hide reopen messages
static bool             cmd_quiet         = FALSE;  // Hide CMD: timeout

#ifdef __MINGW32__
#define UNUSED(x) (void)(x)

#define LOCK_SH 1       /* Shared lock.  */
#define LOCK_EX 2       /* Exclusive lock.  */
#define LOCK_UN 8       /* Unlock.  */
#define LOCK_NB 4       /* Don't block when locking.  */

#undef stat
#define stat stat64

int flock(int fd, int operation);

void
err(int ec, const char *fmt, ...)
{
    UNUSED(ec);
    va_list args;

    printf("%d: ", errno);
    va_start(args, fmt);
    (void) vprintf(fmt, args);
    va_end(args);
    putchar('\n');
    exit(ec);
}

void
errx(int ec, const char *fmt, ...)
{
    UNUSED(ec);
    va_list args;

    va_start(args, fmt);
    (void) vprintf(fmt, args);
    va_end(args);
    putchar('\n');
    exit(ec);
}

void
warn(const char *fmt, ...)
{
    va_list args;

    printf("%d: ", errno);
    va_start(args, fmt);
    (void) vprintf(fmt, args);
    va_end(args);
    putchar('\n');
}

void
warnx(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    (void) vprintf(fmt, args);
    va_end(args);
    putchar('\n');
}

static void
system_error(char *name)
{
    char *ptr = NULL;

    FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                  FORMAT_MESSAGE_FROM_SYSTEM,
                  0,
                  GetLastError(),
                  0,
                  (char *) &ptr,
                  1024,
                  NULL);

    fprintf(stderr, "Error %s: %s\n", name, ptr);
    LocalFree(ptr);
}
#endif

/*
 * atou() converts a numeric string into an integer.
 */
static uint
atou(const char *str)
{
    uint value;
    if (sscanf(str, "%u", &value) != 1)
        errx(EXIT_FAILURE, "'%s' is not an integer value", str);
    return (value);
}

static void
print_version(FILE *fp)
{
    fprintf(fp, "hostsmash "VERSION" built "BUILD_DATE" "BUILD_TIME"\n");
}

/*
 * usage() displays command usage.
 *
 * @param  [in]  None.
 * @return       None.
 */
static void
usage(FILE *fp)
{
    fprintf(fp, "\n");
    print_version(fp);
    (void) fputs(usage_text, fp);
}

static bool
is_printable_ascii(uint8_t ch)
{
    if (ch >= ' ' && ch <= '~')
        return (true);
    if (ch == '\t' || ch == '\r' || ch == '\n')
        return (true);
    return (false);
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

#if defined(DEBUG_READ_DATA)
#define VALUE_UNASSIGNED 0xffffffff

static void
dump_memory(void *buf, uint len, uint dump_base)
{
    uint pos;
    uint strpos;
    char str[20];
    uint32_t *src = buf;

    len = (len + 3) / 4;
    if (dump_base != VALUE_UNASSIGNED)
        printf("%05x:", dump_base);
    for (strpos = 0, pos = 0; pos < len; pos++) {
        uint32_t val = src[pos];
        printf(" %08x", val);
        str[strpos++] = printable_ascii(val);
        str[strpos++] = printable_ascii(val >> 8);
        str[strpos++] = printable_ascii(val >> 16);
        str[strpos++] = printable_ascii(val >> 24);
        if ((pos & 3) == 3) {
            str[strpos] = '\0';
            strpos = 0;
            printf(" %s\n", str);
            if ((dump_base != VALUE_UNASSIGNED) && ((pos + 1) < len)) {
                dump_base += 16;
                printf("%05x:", dump_base);
            }
        }
    }
    if ((pos & 3) != 0) {
        str[strpos] = '\0';
        printf("%*s%s\n", (4 - (pos & 3)) * 5, "", str);
    }
}
#endif

#if 0
static const char *
trim_path(const char *name, size_t *len)
{
    const char *ename;

    /* Eliminate leading ./ */
    while ((name[0] == '.') && (name[0] == '/')) {
        name += 2;
    }

    /* Find end of name */
    for (ename = name; *ename != '\0'; ename++)
        ;

    /* Eliminate trailing / and /. */
    while (1) {
        if ((ename > name) && (ename[-1] == '/')) {
            /* Trailing / */
            ename--;
            continue;
        }
        if ((ename > name + 1) && (ename[-1] == '.') && ename[-2] == '/') {
            /* Trailing /. */
            ename -= 2;
            continue;
        }
        break;
    }

    *len = ename - name;
    return (name);
}
#endif

/*
 * rx_rb_put() stores a next character in the device receive ring buffer.
 *
 * @param [in]  ch - The character to store in the device receive ring buffer.
 *
 * @return      0 = Success.
 * @return      1 = Failure (ring buffer is full).
 */
static int
rx_rb_put(int ch)
{
    uint new_prod = (rx_rb_producer + 1) % sizeof (rx_rb);

    if (new_prod == rx_rb_consumer)
        return (1);  // Discard input because ring buffer is full

    rx_rb[rx_rb_producer] = (uint8_t) ch;
    __sync_synchronize();  // Memory barrier required here on ARM
    rx_rb_producer = new_prod;
    return (0);
}

/*
 * rx_rb_get() returns the next character in the device receive ring buffer.
 *             A value of -1 is returned if there are no characters waiting
 *             to be received in the device receive ring buffer.
 *
 * @param  [in]  None.
 * @return       The next input character.
 * @return       -1 = No characters are pending.
 */
static int
rx_rb_get(void)
{
    int ch;

    if (rx_rb_consumer == rx_rb_producer)
        return (-1);  // Ring buffer empty

    ch = rx_rb[rx_rb_consumer];
    __sync_synchronize();
    rx_rb_consumer = (rx_rb_consumer + 1) % sizeof (rx_rb);
    return (ch);
}

/*
 * rx_rb_peek() retrieves the most recent received text, without
 *              disturbing the read position.
 */
static int
rx_rb_peek(char *buf, uint bufsize)
{
    int rx_rb_pos;
    uint bufcur;
    if (bufsize > sizeof (rx_rb) - 1)
        bufsize = sizeof (rx_rb) - 1;
    rx_rb_pos = rx_rb_producer - bufsize;
    if (rx_rb_pos < 0)
        rx_rb_pos += sizeof (rx_rb);
    for (bufcur = 0; bufcur < bufsize - 1; bufcur++) {
        *(buf++) = rx_rb[rx_rb_pos];
        rx_rb_pos = (rx_rb_pos + 1) % sizeof (rx_rb);
    }
    *buf = '\0';
    return (bufsize);
}

/*
 * tx_rb_put() stores next character to be sent to the remote device.
 *
 * @param [in]  ch - The character to store in the tty input ring buffer.
 *
 * @return      0 = Success.
 * @return      1 = Failure (ring buffer is full).
 */
static int
tx_rb_put(int ch)
{
    uint new_prod = (tx_rb_producer + 1) % sizeof (tx_rb);

    if (new_prod == tx_rb_consumer)
        return (1);  // Discard input because ring buffer is full

    tx_rb[tx_rb_producer] = (uint8_t) ch;
    __sync_synchronize();
    tx_rb_producer = new_prod;
    return (0);
}

/*
 * tx_rb_get() returns the next character to be sent to the remote device.
 *             A value of -1 is returned if there are no characters waiting
 *             to be received in the tty input ring buffer.
 *
 * @param  [in]  None.
 * @return       The next input character.
 * @return       -1 = No input character is pending.
 */
static int
tx_rb_get(void)
{
    int ch;

    if (tx_rb_consumer == tx_rb_producer)
        return (-1);  // Ring buffer empty

    ch = tx_rb[tx_rb_consumer];
    __sync_synchronize();
    tx_rb_consumer = (tx_rb_consumer + 1) % sizeof (tx_rb);
    return (ch);
}

/*
 * tx_rb_space() returns a count of the number of characters remaining
 *               in the transmit ring buffer before the buffer is
 *               completely full. A value of 0 means the buffer is
 *               already full.
 *
 * @param  [in]  None.
 * @return       Count of space remaining in the ring buffer (9=Full).
 */
static uint
tx_rb_space(void)
{
    uint diff = tx_rb_consumer - tx_rb_producer;
    return (diff + sizeof (tx_rb) - 1) % sizeof (tx_rb);
}

/*
 * tx_rb_flushed() tells whether there are still pending characters to be
 *                 sent from the Tx ring buffer.
 *
 * @param  [in]  None.
 * @return       TRUE  - Ring buffer is empty.
 * @return       FALSE - Ring buffer has output pending.
 */
static bool_t
tx_rb_flushed(void)
{
    if (tx_rb_consumer == tx_rb_producer)
        return (TRUE);   // Ring buffer empty
    else
        return (FALSE);  // Ring buffer has output pending
}

/*
 * cond_wait_msec() waits on a condition variable for at most msec
 *                  milliseconds. The caller must hold mtx.
 */
static void
cond_wait_msec(pthread_cond_t *cv, pthread_mutex_t *mtx, int msec)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long) msec * 1000000L;
    ts.tv_sec  += ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    (void) pthread_cond_timedwait(cv, mtx, &ts);
}

/*
 * tx_rb_kick() wakes th_serial_writer() so that output just queued in the
 *              Tx ring buffer is sent immediately.
 */
static void
tx_rb_kick(void)
{
    pthread_mutex_lock(&tx_rb_mtx);
    pthread_cond_signal(&tx_rb_cv);
    pthread_mutex_unlock(&tx_rb_mtx);
}

/*
 * tx_rb_wait() blocks the caller (th_serial_writer()) until output is
 *              kicked or msec milliseconds have elapsed. It returns
 *              immediately if output is already pending.
 */
static void
tx_rb_wait(int msec)
{
    pthread_mutex_lock(&tx_rb_mtx);
    if (tx_rb_consumer == tx_rb_producer)
        cond_wait_msec(&tx_rb_cv, &tx_rb_mtx, msec);
    pthread_mutex_unlock(&tx_rb_mtx);
}

#ifdef KS_RX_WAKEUP
/*
 * rx_rb_kick() wakes a thread waiting in rx_rb_wait(). It is called by
 *              th_serial_reader() after received data has been stored.
 */
static void
rx_rb_kick(void)
{
    pthread_mutex_lock(&rx_rb_mtx);
    pthread_cond_broadcast(&rx_rb_cv);
    pthread_mutex_unlock(&rx_rb_mtx);
}

/*
 * rx_rb_wait() blocks until received data is available or msec
 *              milliseconds have elapsed. It returns immediately if data
 *              is already waiting in the Rx ring buffer.
 */
static void
rx_rb_wait(int msec)
{
    pthread_mutex_lock(&rx_rb_mtx);
    if (rx_rb_consumer == rx_rb_producer)
        cond_wait_msec(&rx_rb_cv, &rx_rb_mtx, msec);
    pthread_mutex_unlock(&rx_rb_mtx);
}
#endif

#ifdef __MINGW32__
#include <windows.h>
static void
microsleep(__int64 usec)
{
    HANDLE timer;
    LARGE_INTEGER ft;

    /*
     * Convert to 100 nanosecond interval; negative value indicates
     * relative time.
     */
    ft.QuadPart = -(10*usec);

    timer = CreateWaitableTimer(NULL, TRUE, NULL);
    SetWaitableTimer(timer, &ft, 0, NULL, NULL, 0);
    WaitForSingleObject(timer, INFINITE);
    CloseHandle(timer);
}

#elif _POSIX_C_SOURCE >= 199309L
#include <time.h>   // for nanosleep
#else
#include <unistd.h> // for usleep
#endif

/*
 * time_delay_msec() will delay for a specified number of milliseconds.
 *
 * @param [in]  msec - Milliseconds from now.
 *
 * @return      None.
 */
void
time_delay_msec(int msec)
{
#ifdef __MINGW32__
    microsleep(msec * 1000);
//  Sleep(msec);
#elif _POSIX_C_SOURCE >= 199309L
    struct timespec ts;
    ts.tv_sec = msec / 1000;
    ts.tv_nsec = (msec % 1000) * 1000000;
    nanosleep(&ts, NULL);
#elif 0
    if (poll(NULL, 0, msec) < 0)
        warn("poll() failed");
#else
    if (msec >= 1000)
        sleep(msec / 1000);
    usleep((msec % 1000) * 1000);
#endif
}

#ifdef __MINGW32__
#include <shellapi.h>

/*
 * hostsmash_is_elevated() / hostsmash_ensure_elevated_for_net() --
 *
 * See the large comment above windows_ensure_privilege() in
 * netif_windows.c for the full story; short version here.
 *
 * On Windows there is no execve()-style in-place re-exec: gaining
 * admin rights means launching an entirely new, unrelated process,
 * and that new process cannot inherit handles -- including the
 * stdin/stdout pipe ends netif_start() (hostsmash_net.c) hands to
 * hostsmash_netif.exe via STARTF_USESTDHANDLES -- across the UAC
 * integrity-level boundary.
 *
 * That means hostsmash_netif.exe can only safely self-elevate when
 * run standalone/interactively; it must not do so when hostsmash.exe
 * has spawned it with piped stdio, since the relaunch would silently
 * strand that pipe (a disconnected, UAC-elevated copy of
 * hostsmash_netif.exe would run in its own new window, while the
 * original piped copy hostsmash.exe is actually talking to exits
 * right out from under it). netif_windows.c now refuses to relaunch
 * in that situation instead of doing that.
 *
 * So the responsibility for obtaining an admin token is moved up to
 * here, one level higher in the process tree: if network service
 * (-n) was requested and we're not already elevated, hostsmash.exe
 * relaunches *itself* via UAC before it ever creates the pipes or
 * spawns the netif helper. By the time netif_start() runs,
 * hostsmash.exe -- and therefore its inherited-handle child,
 * hostsmash_netif.exe -- is already elevated, so the helper's own
 * elevation check succeeds immediately and it never needs to touch
 * ShellExecuteExW at all.
 */
static int
hostsmash_is_elevated(void)
{
    HANDLE          token = NULL;
    TOKEN_ELEVATION elevation;
    DWORD           sz = sizeof (elevation);
    int             elevated = 0;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return (0);
    if (GetTokenInformation(token, TokenElevation, &elevation,
                             sizeof (elevation), &sz)) {
        elevated = elevation.TokenIsElevated ? 1 : 0;
    }
    CloseHandle(token);
    return (elevated);
}

/*
 * Relaunches this program elevated if network service was requested
 * and we're not already elevated. Never returns in that case -- the
 * relaunch is a whole new process, so this must be called early in
 * main(), before serial_open()/create_threads() or any other state
 * is created; anything set up before this call is simply discarded
 * when this process exits.
 */
static void
hostsmash_ensure_elevated_for_net(int argc, char * const *argv)
{
    wchar_t            exe_path[MAX_PATH];
    char               args[4096];
    wchar_t            wargs[4096];
    SHELLEXECUTEINFOW  sei;
    int                off = 0;
    int                i;

    if (hostsmash_is_elevated())
        return;

    fprintf(stderr,
            "hostsmash: network service (-n) requires administrator "
            "privileges on Windows; elevating via UAC prompt...\n");

    if (GetModuleFileNameW(NULL, exe_path, MAX_PATH) == 0) {
        fprintf(stderr, "hostsmash: GetModuleFileNameW failed\n");
        exit(EXIT_FAILURE);
    }

    args[0] = '\0';
    for (i = 1; i < argc && off < (int)sizeof (args) - 4; i++)
        off += snprintf(args + off, sizeof (args) - off, "\"%s\" ", argv[i]);
    MultiByteToWideChar(CP_UTF8, 0, args, -1, wargs,
                         (int)(sizeof (wargs) / sizeof (wargs[0])));

    memset(&sei, 0, sizeof (sei));
    sei.cbSize       = sizeof (sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb       = L"runas";
    sei.lpFile       = exe_path;
    sei.lpParameters = wargs;
    sei.nShow        = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        DWORD err = GetLastError();
        if (err == ERROR_CANCELLED)
            fprintf(stderr, "hostsmash: elevation declined by user\n");
        else
            fprintf(stderr, "hostsmash: ShellExecuteExW failed (%lu)\n",
                    (unsigned long)err);
        exit(EXIT_FAILURE);
    }
    if (sei.hProcess != NULL)
        CloseHandle(sei.hProcess);

    /*
     * The elevated relaunch is a brand-new process (its own console,
     * its own session) -- this copy has no further useful role and
     * must not fall through into serial_open()/create_threads()
     * unelevated.
     */
    exit(EXIT_SUCCESS);
}
#endif /* __MINGW32__ */

static void
diff_timeval(struct timeval *start, struct timeval *end, struct timeval *diff)
{
    long seconds = end->tv_sec - start->tv_sec;
    long microseconds = end->tv_usec - start->tv_usec;

    // Adjust for negative microseconds
    if (microseconds < 0) {
        seconds--;
        microseconds += 1000000;
    } else if (microseconds > 1000000) {
        seconds++;
        microseconds -= 1000000;
    }
    diff->tv_sec = seconds;
    diff->tv_usec = microseconds;
}

static int
time_has_elapsed(struct timeval *tv_timeout)
{
    struct timeval tv_now;
    struct timeval tv_diff;
    struct timezone tz;
    gettimeofday(&tv_now, &tz);
    diff_timeval(tv_timeout, &tv_now, &tv_diff);
    if ((tv_diff.tv_sec >= 0) && (tv_diff.tv_usec > 0))
        return (1);
    return (0);
}

static void
calc_timeout_msec(struct timeval *tv_timeout, int msec)
{
    struct timezone tz;
    gettimeofday(tv_timeout, &tz);

    tv_timeout->tv_usec += msec * 1000;
    tv_timeout->tv_sec += tv_timeout->tv_usec / 1000000;
    tv_timeout->tv_usec %= 1000000;
}

/*
 * send_ll_bin_nokick() queues a binary block of data to be sent to the
 *                      remote programmer, without waking the serial writer
 *                      thread. Use this to build up a multi-part message
 *                      which should go out in a single write, then call
 *                      tx_rb_kick().
 *
 * @param  [in] data  - Data to send to the programmer.
 * @param  [in] len   - Number of bytes to send.
 */
static int
send_ll_bin_nokick(const void *buf, size_t len)
{
    int timeout_count = 0;
    const uint8_t *data = (const uint8_t *)buf;
    size_t pos = 0;
    struct timeval tv_timeout;
    calc_timeout_msec(&tv_timeout, 500);

    while (pos < len) {
        if (tx_rb_put(*data)) {
            if (time_has_elapsed(&tv_timeout)) {
                printf("Send timeout at 0x%zx\n", pos);
                return (1);  // Timeout
            }
            if (timeout_count == 0) {
                printf("-\n"); fflush(stdout);  // XXX: shouldn't happen
            }
            tx_rb_kick();    // Ring is full: make sure the writer drains it
            timeout_count++;
            continue;        // Try again
        }
        if (timeout_count) {
            calc_timeout_msec(&tv_timeout, 500);
            timeout_count = 0;
        }
        data++;
        pos++;
    }
    return (0);
}

/*
 * send_ll_bin() sends a binary block of data to the remote programmer,
 *               and wakes the serial writer thread to send it immediately.
 *
 * @param  [in] data  - Data to send to the programmer.
 * @param  [in] len   - Number of bytes to send.
 */
static int
send_ll_bin(const void *buf, size_t len)
{
    int rc = send_ll_bin_nokick(buf, len);
    tx_rb_kick();
    return (rc);
}

/*
 * config_dev() will configure the serial device used for communicating
 *              with the programmer.
 *
 * @param  [in]  fd - Opened file descriptor for serial device.
 * @return       RC_FAILURE - Failed to configure device.
 */
#ifdef __MINGW32__
static rc_t
config_dev(void)
{
    COMMTIMEOUTS timeouts;
    DCB port;
    TIMECAPS timecaps;

    if (timeGetDevCaps(&timecaps, sizeof (timecaps)) == MMSYSERR_NOERROR) {
        // Set improved timer-accuracy for Sleep/Wait functions
        if (timeBeginPeriod(timecaps.wPeriodMin) != TIMERR_NOERROR)
            printf("timeBeginPeriod() failed\n");
    }

    /* Get the current DCB, and adjust to our liking */
    memset(&port, 0, sizeof (port));
    port.DCBlength = sizeof (port);
    if (GetCommState(dev_handle, &port) == 0)
        system_error("getting comm state");
    if (BuildCommDCB("baud=115200 parity=n data=8 stop=1", &port) == 0)
        system_error("building comm DCB");
    if (!SetCommState(dev_handle, &port))
        system_error("adjusting port settings");

    /* Set short timeouts on the COM port */
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = terminal_mode ? 100 : 0;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 10;
    if (!SetCommTimeouts(dev_handle, &timeouts))
        system_error("setting port time-outs.");

    if (!EscapeCommFunction(dev_handle, CLRDTR))
        system_error("clearing DTR");
    Sleep(200);
    if (!EscapeCommFunction(dev_handle, SETDTR))
        system_error("setting DTR");

    return (RC_SUCCESS);
}
#else
static rc_t
config_dev(int fd)
{
    struct termios tty;

    if (flock(fd, LOCK_EX | LOCK_NB) < 0)
        warnx("Failed to get exclusive lock on %s", device_name);

#ifdef OSX
    /* Disable non-blocking */
    if (fcntl(fd, F_SETFL, 0) < 0)
        warnx("Failed to enable blocking on %s", device_name);
#endif

    (void) memset(&tty, 0, sizeof (tty));

    if (tcgetattr(fd, &tty) != 0) {
        /* Failed to get terminal information */
        warn("Failed to get tty info for %s", device_name);
        close(fd);
        return (RC_FAILURE);
    }

#undef DEBUG_TTY
#ifdef DEBUG_TTY
    printf("tty: pre  c=%x i=%x o=%x l=%x\n",
           tty.c_cflag, tty.c_iflag, tty.c_oflag, tty.c_lflag);
#endif

    if (cfsetispeed(&tty, B115200) ||
        cfsetospeed(&tty, B115200)) {
        warn("failed to set %s speed to 115200 BPS", device_name);
        close(fd);
        return (RC_FAILURE);
    }

    tty.c_iflag &= IXANY;
    tty.c_iflag &= (IXON | IXOFF);            // sw flow off

    tty.c_cflag &= ~CRTSCTS;                  // hw flow off
    tty.c_cflag &= (uint)~CSIZE;              // no bits
    tty.c_cflag |= CS8;                       // 8 bits

    tty.c_cflag &= (uint)~(PARENB | PARODD);  // no parity
    tty.c_cflag &= (uint)~CSTOPB;             // one stop bit

    tty.c_iflag  = IGNBRK;                    // raw, no echo
    tty.c_lflag  = 0;
    tty.c_oflag  = 0;
    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_cflag &= ~ECHOPRT;                  // CR is not newline

    tty.c_cc[VINTR]    = 0;  // Ctrl-C
    tty.c_cc[VQUIT]    = 0;  // Ctrl-Backslash
    tty.c_cc[VERASE]   = 0;  // Del
    tty.c_cc[VKILL]    = 0;  // @
    tty.c_cc[VEOF]     = 4;  // Ctrl-D
    tty.c_cc[VTIME]    = 0;  // Inter-character timer unused
    tty.c_cc[VMIN]     = 1;  // Blocking read until 1 character arrives
#ifdef VSWTC
    tty.c_cc[VSWTC]    = 0;  // '\0'
#endif
    tty.c_cc[VSTART]   = 0;  // Ctrl-Q
    tty.c_cc[VSTOP]    = 0;  // Ctrl-S
    tty.c_cc[VSUSP]    = 0;  // Ctrl-Z
    tty.c_cc[VEOL]     = 0;  // '\0'
    tty.c_cc[VREPRINT] = 0;  // Ctrl-R
    tty.c_cc[VDISCARD] = 0;  // Ctrl-u
    tty.c_cc[VWERASE]  = 0;  // Ctrl-W
    tty.c_cc[VLNEXT]   = 0;  // Ctrl-V
    tty.c_cc[VEOL2]    = 0;  // '\0'

#ifdef DEBUG_TTY
    printf("tty: post c=%x i=%x o=%x l=%x cc=%02x %02x %02x %02x\n",
           tty.c_cflag, tty.c_iflag, tty.c_oflag, tty.c_lflag,
           tty.c_cc[0], tty.c_cc[1], tty.c_cc[2], tty.c_cc[3]);
#endif
    if (tcsetattr(fd, TCSANOW, &tty)) {
        warn("failed to set %s attributes", device_name);
        close(fd);
        return (RC_FAILURE);
    }
    return (RC_SUCCESS);
}
#endif

/*
 * reopen_dev() will wait for the serial device to reappear after it has
 *              disappeared.
 *
 * @param  [in]  None.
 * @return       None.
 */
#ifdef __MINGW32__
static void
reopen_dev(void)
{
    static time_t last_time = 0;
    time_t        now       = time(NULL);
    bool_t        printed   = FALSE;

    if (dev_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(dev_handle);
        dev_handle = INVALID_HANDLE_VALUE;
    }
    if (!reopen_quiet && (now - last_time > 5)) {
        printed = TRUE;
        printf("\n<< Closed %s >>", device_name);
        fflush(stdout);
    }
top:
    do {
        if (running == 0)
            return;
        time_delay_msec(400);
        dev_handle = CreateFile(host_device_name,
                                GENERIC_READ | GENERIC_WRITE,
                                0,
                                NULL,
                                OPEN_EXISTING,
                                0,
                                NULL);
    } while (dev_handle == INVALID_HANDLE_VALUE);
    dev_write_failed = FALSE;

    if (running == 0)
        return;

    if (config_dev() != RC_SUCCESS) {
        CloseHandle(dev_handle);
        dev_handle = INVALID_HANDLE_VALUE;
        goto top;
    }

    now = time(NULL);
    if (!reopen_quiet && (now - last_time > 5)) {
        if (printed == FALSE)
            printf("\n");
        printf("\r<< Reopened %s >>\n", device_name);
    }
    last_time = now;
}
#else
static void
reopen_dev(void)
{
    int           temp      = dev_fd;
    static time_t last_time = 0;
    time_t        now       = time(NULL);
    bool_t        printed   = FALSE;
    int           oflags    = O_NOCTTY;

#ifdef OSX
    oflags |= O_NONBLOCK;
#endif

    dev_fd = -1;
    if (temp != -1) {
        if (flock(temp, LOCK_UN | LOCK_NB) < 0)
            warnx("Failed to release exclusive lock on %s", device_name);
        close(temp);
    }
    if (!reopen_quiet && (now - last_time > 5)) {
        printed = TRUE;
        printf("\n<< Closed %s >>", device_name);
        fflush(stdout);
    }
top:
    do {
        if (running == 0)
            return;
        time_delay_msec(400);
    } while ((temp = open(host_device_name, oflags | O_RDWR)) == -1);

    if (config_dev(temp) != RC_SUCCESS) {
        close(temp);
        goto top;
    }

    /* Hand off the new I/O fd */
    dev_fd = temp;

    now = time(NULL);
    if (!reopen_quiet && (now - last_time > 5)) {
        if (printed == FALSE)
            printf("\n");
        printf("\r<< Reopened %s >>\n", device_name);
    }
    last_time = now;
}
#endif

/*
 * th_serial_reader() is a thread to read from serial port and store it in
 *                    a circular buffer.  The buffer's contents are retrieved
 *                    asynchronously by another thread.
 *
 * @param [in]  arg - Unused argument.
 *
 * @return      NULL pointer (unused)
 *
 * @see         serial_in_snapshot(), serial_in_count(), serial_in_advance(),
 *              serial_in_flush()
 */
static void *
th_serial_reader(void *arg)
{
    const char *log_file;
    FILE       *log_fp = NULL;
    uint        log_hex = 0;
    uint8_t     buf[128];

    if ((log_file = getenv("TERM_DEBUG")) != NULL) {
        /*
         * Examples:
         *     export TERM_DEBUG
         *     TERM_DEBUG=/dev/pts/4 hostsmash -t
         *     TERM_DEBUG=/tmp/term_debug hostsmash -t -d /dev/ttyACM0
         */
        log_fp = fopen(log_file, "wb");
        if (log_fp == NULL)
            warn("Unable to open %s for log", log_file);
        log_hex = (getenv("TERM_DEBUG_HEX") != NULL);
    }
    while (running) {
#ifdef __MINGW32__
        DWORD len;
        uint  idle = 0;
        while (ReadFile(dev_handle, buf, sizeof (buf), &len, NULL))
#else
        ssize_t len;
        while ((len = read(dev_fd, buf, sizeof (buf))) >= 0)
#endif
        {
            if (len == 0) {
#ifdef __MINGW32__
                /*
                 * After its USB device is removed (for example, when
                 * Kicksmash reboots), Windows may keep completing reads
                 * on the stale handle with no data rather than failing
                 * them. Detect that the port is gone by a failed write
                 * or by a periodic port status check, then reopen it.
                 */
                if (dev_write_failed)
                    break;
                if (++idle >= 25) {
                    DWORD   errors;
                    COMSTAT stat;
                    idle = 0;
                    if (!ClearCommError(dev_handle, &errors, &stat))
                        break;
                }
#endif
#if defined(USE_NON_BLOCKING_TTY) || defined(__MINGW32__)
                /* No input available */
                time_delay_msec(10);
                continue;
#else
                /* Error reading */
                break;
#endif
            }
            if (running == 0)
                break;

            if (terminal_mode) {
                fwrite(buf, len, 1, stdout);
                fflush(stdout);
            } else {
                uint pos;
                for (pos = 0; pos < len; pos++) {
                    while (rx_rb_put(buf[pos]) == 1) {
                        time_delay_msec(1);
                        printf("RX ring buffer overflow\n");
                        if (running == 0)
                            break;
                    }
                    if (running == 0)
                        break;
                }
#ifdef KS_RX_WAKEUP
                rx_rb_kick();  // Wake recv_ks_reply_core()
#endif
            }
            if (log_fp != NULL) {
                if (log_hex) {
                    uint pos;
                    fprintf(log_fp, " ");
                    for (pos = 0; pos < len; pos++)
                        fprintf(log_fp, " %02x", buf[pos]);
                    fprintf(log_fp, "\"");
                    for (pos = 0; pos < len; pos++) {
                        char ch = buf[pos];
                        if ((ch <= ' ') || (ch > '~') || (ch == '"'))
                            ch = '_';
                        fprintf(log_fp, "%c", ch);
                    }
                    fprintf(log_fp, "\"");
                } else {
                    fwrite(buf, len, 1, log_fp);
                }
                fflush(log_fp);
            }
        }
        if (running == 0)
            break;
        reopen_dev();
    }
    printf("not running\n");

    if (log_fp != NULL)
        fclose(log_fp);
    return (NULL);
}

/*
 * th_serial_writer() is a thread to read from the tty input ring buffer and
 *                    write data to the serial port.  The separation of tty
 *                    input from serial writes allows the program to still be
 *                    responsive to user interaction even when blocked on
 *                    serial writes.
 *
 * @param [in]  arg - Unused argument.
 *
 * @return      NULL pointer (unused)
 *
 * @see         serial_in_snapshot(), serial_in_count(), serial_in_advance(),
 *              serial_in_flush()
 */
/*
 * Size of the writer thread's local buffer. This is large enough that a
 * full Kicksmash message (SEND_MSG_MAX plus header and CRC) goes to the
 * device in a single write() rather than as many 64-byte writes.
 */
#define TX_WRITE_CHUNK 4096

/*
 * dev_write_all() writes len bytes to the serial device, retrying after
 *                 a partial write. Returns the number of bytes written,
 *                 or -1 if the device is not open or nothing could be
 *                 written because of an error.
 */
static int
dev_write_all(const char *buf, uint len)
{
    uint done  = 0;
    uint tries = 0;

    while (done < len) {
#ifdef __MINGW32__
        DWORD count;
        if (dev_handle == INVALID_HANDLE_VALUE)
            return ((done > 0) ? (int) done : -1);
        if (WriteFile(dev_handle, buf + done, len - done, &count, NULL) == 0) {
            dev_write_failed = TRUE;  // Reader thread will reopen device
            return ((done > 0) ? (int) done : -1);
        }
#else
        ssize_t count;
        if (dev_fd == -1)
            return ((done > 0) ? (int) done : -1);
        count = write(dev_fd, buf + done, len - done);
        if (count < 0) {
            if ((errno != EAGAIN) && (errno != EINTR))
                return ((done > 0) ? (int) done : -1);
            count = 0;
        }
#endif
        if (count == 0) {
            /* Device is not accepting data right now */
            if (++tries > 500)
                break;
            time_delay_msec(1);
            continue;
        }
        tries = 0;
        done += count;
    }
    return ((int) done);
}

static void *
th_serial_writer(void *arg)
{
    int ch;
    uint pos = 0;
    char lbuf[TX_WRITE_CHUNK];

    while (1) {
        ch = tx_rb_get();
        if (ch >= 0)
            lbuf[pos++] = ch;
        if (((ch < 0) && (pos > 0)) ||
             (pos >= sizeof (lbuf)) || (ic_delay != 0)) {
            int count = dev_write_all(lbuf, pos);
            if (count < 0) {
                /* Device closed or failed; wait for reader to reopen it */
                time_delay_msec(500);
                if (pos >= sizeof (lbuf))
                    pos--;
                continue;
            }
            if (ic_delay) {
                /* Inter-character pacing delay was specified */
                time_delay_msec(ic_delay);
            }

#ifdef DEBUG_TRANSFER
            printf(">%02x\n", lbuf[0]);
#endif
            if ((uint) count < pos) {
                printf("sent only %d of %u\n", count, pos);
            }
            pos = 0;
        } else if (ch < 0) {
            /* Nothing to send: sleep until output is kicked */
            tx_rb_wait(10);
            if (!running)
                break;
        }
    }
    return (NULL);
}

/*
 * serial_open() initializes a serial port for communication with a device.
 *
 * @param  [in]  None.
 * @return       None.
 */
static rc_t
serial_open(bool_t verbose)
{
#ifdef __MINGW32__
    /* Open the COM port */
    dev_handle = CreateFile(host_device_name,
                            GENERIC_READ | GENERIC_WRITE,
                            0,
                            NULL,
                            OPEN_EXISTING,
                            0,
                            NULL);

    if (dev_handle == INVALID_HANDLE_VALUE) {
        warnx("Failed to open %s", device_name);
        system_error("");
        return (RC_FAILURE);
    }

    if (config_dev()) {
        CloseHandle(dev_handle);
        return (RC_FAILURE);
    }
    return (RC_SUCCESS);
#else
    int oflags = O_NOCTTY;

#ifdef OSX
    oflags |= O_NONBLOCK;
#endif

    /* First verify the file exists */
    dev_fd = open(host_device_name, oflags | O_RDONLY);
    if (dev_fd == -1) {
        warn("Failed to open %s for read", device_name);
        return (RC_FAILURE);
    }
    close(dev_fd);

    dev_fd = open(host_device_name, oflags | O_RDWR);
    if (dev_fd == -1) {
        warn("Failed to open %s for write", device_name);
        return (RC_FAILURE);
    }
    return (config_dev(dev_fd));
#endif
}

/*
 * at_exit_func() cleans up the terminal.  This function is necessary because
 *                the terminal is put in raw mode in order to receive
 *                non-blocking character input which is not echoed to the
 *                console.  It is necessary because some libdevaccess
 *                functions may exit on a fatal error.
 *
 * @param  [in]  None.
 * @return       None.
 */
static void
at_exit_func(void)
{
#ifndef __MINGW32__
    if (got_terminfo) {
        got_terminfo = 0;
        tcsetattr(0, TCSANOW, &saved_term);
    }
#endif
}

/*
 * do_exit() exits gracefully.
 *
 * @param [in]  rc - The exit code with which to terminate the program.
 *
 * @return      This function does not return.
 */
static void __attribute__((noreturn))
do_exit(int rc)
{
    putchar('\n');
    exit(rc);
}

#ifndef __MINGW32__
/*
 * sig_exit() will exit on a fatal signal (SIGTERM, etc).
 */
static void
sig_exit(int sig)
{
    do_exit(EXIT_FAILURE);
}
#endif

/*
 * create_threads() sets up the communication threads with the programmer.
 */
static void
create_threads(void)
{
    pthread_attr_t thread_attr;
    pthread_t      thread_id;

    /* Create thread */
    pthread_attr_init(&thread_attr);
    pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread_id, &thread_attr, th_serial_reader, NULL))
        err(EXIT_FAILURE, "failed to create %s reader thread", device_name);
    if (pthread_create(&thread_id, &thread_attr, th_serial_writer, NULL))
        err(EXIT_FAILURE, "failed to create %s writer thread", device_name);
}

/*
 * receive_ll() receives bytes from the remote side until a timeout occurs
 *              or the specified length has been reached. If exact_bytes is
 *              specified, then a timeout warning will be issued if less
 *              than the specified number of bytes is received.
 *
 * @param  [out] buf     - Buffer into which output from the programmer is
 *                         to be captured.
 * @param  [in]  buflen  - Maximum number of bytes to receive.
 * @param  [in]  timeout - Number of milliseconds since last character before
 *                         giving up.
 */
static int
receive_ll(void *buf, size_t buflen, int timeout, bool exact_bytes)
{
    int timeout_count = 0;
    int received = 0;
    uint8_t *data = (uint8_t *)buf;
    struct timeval tv_timeout;
    calc_timeout_msec(&tv_timeout, 500);

    while (received < buflen) {
        int ch = rx_rb_get();
        if (ch == -1) {
            if (time_has_elapsed(&tv_timeout)) {
                if (ks_message_terminated)
                    return (0);
                if (exact_bytes && ((timeout > 50) || (received == 0))) {
                    printf("Receive timeout (%d ms): got %d of %zu bytes\n",
                           timeout, received, buflen);
                }
                return (received);
            }
            timeout_count++;
            continue;
        }
        if (timeout_count) {
            calc_timeout_msec(&tv_timeout, 500);
            timeout_count = 0;
        }
        *(data++) = ch;
        received++;
    }
    return (received);
}

/*
 * report_remote_failure_message() will report status on the console which
 *                                 was provided by the programmer.
 */
static int
report_remote_failure_message(void)
{
    uint8_t buf[64];
    int     len = receive_ll(buf, sizeof (buf), 100, false);

    if ((len > 2) && (buf[0] == ' ') && (buf[1] == ' ')) {
        /* Report remote failure message */
        printf("Status from programmer: %.*s", len - 2, buf + 2);
        if (buf[len - 1] != '\n')
            printf("\n");
        return (1);
    }
    /* No remote failure message detected */
    return (0);
}

/*
 * check_crc() verifies the CRC data value received matches the previously
 *             received data.
 */
static int
check_crc(uint32_t crc, uint spos, uint epos, bool send_status)
{
    uint32_t compcrc;
    uint8_t  rc;

    if (receive_ll(&compcrc, 4, 2000, false) == 0) {
        printf("CRC receive timeout at 0x%x-0x%x\n", spos, epos);
        return (1);
    }

    if (compcrc != crc) {
        if ((compcrc == 0x20202020) && report_remote_failure_message())
            return (1);  // Failure message from programmer
        warnx("Bad CRC %08x received from Kicksmash (should be %08x) "
              "at 0x%x-0x%x",
              compcrc, crc, spos, epos);
        rc = 1;
    } else {
        rc = 0;
    }
    if (send_status) {
        if (send_ll_bin(&rc, sizeof (rc))) {
            printf("Status send timeout at 0x%x\n", epos);
            return (-1);  // Timeout
        }
    }
    return (rc);
}

static int
check_rc(uint pos)
{
    uint8_t rc;
    if (receive_ll(&rc, 1, 200, false) == 0) {
        printf("RC receive timeout at 0x%x\n", pos);
        return (1);
    }
    if (rc != 0) {
        printf("Remote sent error %d\n", rc);
//      discard_input(250);
        return (1);
    }
    return (0);
}

/*
 * receive_ll_crc() receives data from the remote side with status and
 *                  CRC data embedded. This function checks status and CRC
 *                  and sends status back to the remote side.
 *
 * Protocol:
 *     SENDER:   <status> <data> <CRC> [<Status> <data> <CRC>...]
 *     RECEIVER: <status> [<status>...]
 *
 * SENDER
 *     The <status> byte is whether a failure occurred reading the data.
 *     If the sender is hostsmash, then it could also be user abort.
 *     <data> is 256 bytes (or less if the remaining transfer length is
 *     less than that amount. <CRC> is a 32-bit CRC over the previous
 *     (up to) 256 bytes of data.
 * RECEIVER
 *     The <status> byte is whether the received data matched the CRC.
 *     If the receiver is the programmer, then the <status> byte also
 *     indicates whether the data write was successful.
 *
 * @param  [out] buf     - Data received from the programmer.
 * @param  [in]  buflen  - Number of bytes to receive from programmer.
 *
 * @return       -1 a send timeout occurred.
 * @return       The number of bytes received.
 */
static int
receive_ll_crc(void *buf, size_t buflen)
{
    int      timeout = 200; // 200 ms
    uint     pos = 0;
    uint     tlen = 0;
    uint     received = 0;
    size_t   lpercent = -1;
    size_t   percent;
    uint32_t crc = 0;
    uint8_t *data = (uint8_t *)buf;
    uint8_t  rc;

    while (pos < buflen) {
        tlen = buflen - pos;
        if (tlen > DATA_CRC_INTERVAL)
            tlen = DATA_CRC_INTERVAL;

        received = receive_ll(&rc, 1, timeout, true);
        if (received == 0) {
            printf("Status receive timeout at 0x%x\n", pos);
            return (-1);  // Timeout
        }
        if (rc != 0) {
            printf("Read error %d at 0x%x\n", rc, pos);
            return (-1);
        }

        received = receive_ll(data, tlen, timeout, true);
        crc = crc32(crc, data, received);
#ifdef DEBUG_TRANSFER
        printf("c:%02x\n", crc); fflush(stdout);
#endif
        if (check_crc(crc, pos, pos + received, true))
            return (pos + received);

        data   += received;
        pos    += received;

        percent = (pos * 100) / buflen;
        if (lpercent != percent) {
            lpercent = percent;
            printf("\r%zu%%", percent);
            fflush(stdout);
        }

        if (received < tlen)
            return (pos);  // Timeout
    }
    printf("\r100%%\n");
    time_delay_msec(20); // Allow remaining CRC bytes to be sent
    return (pos);
}

/*
 * send_ll_str() sends a string to the programmer, typically a command.
 *
 * @param  [in] cmd - Command string to send to the programmer.
 */
static int
send_ll_str(const char *cmd)
{
    int timeout_count = 0;
    while (*cmd != '\0') {
        if (tx_rb_put(*cmd)) {
            time_delay_msec(1);
            if (timeout_count++ >= 1000) {
                return (1);  // Timeout
            }
        } else {
            timeout_count = 0;
            cmd++;
        }
    }
    tx_rb_kick();
    return (0);
}

/*
 * discard_input() discards following output from the programmer.
 *
 * @param  [in] timeout - Number of milliseconds since last character before
 *                        stopping discard.
 * @return      None.
 */
static void
discard_input(int timeout)
{
    struct timeval tv_timeout;
    calc_timeout_msec(&tv_timeout, timeout);

    while (!time_has_elapsed(&tv_timeout)) {
        int ch = rx_rb_get();
        if (ch == -1) {
            time_delay_msec(1);
            continue;
        }
    }
}

/*
 * send_ll_crc() sends a CRC-protected binary image to the remote programmer.
 *
 * @param  [in] data  - Data to send to the programmer.
 * @param  [in] len   - Number of bytes to send.
 *
 * @return      0 - Data successfully sent.
 * @return      1 - A timeout waiting for programmer occurred.
 * @return      2 - A CRC error was detected.
 *
 * Protocol:
 *     SENDER:   <status> <data> <CRC> [<Status> <data> <CRC>...]
 *     RECEIVER: <status> [<status>...]
 *
 * SENDER
 *     The <status> byte is whether a failure occurred reading the data.
 *     If the sender is hostsmash, then it could also be user abort.
 *     <data> is 256 bytes (or less if the remaining transfer length is
 *     less than that amount. <CRC> is a 32-bit CRC over the previous
 *     (up to) 256 bytes of data.
 * RECEIVER
 *     The <status> byte is whether the received data matched the CRC.
 *     If the receiver is the programmer, then the <status> byte also
 *     indicates whether the data write was successful.
 *
 * As the remote side receives data bytes, it will send status for every
 * 256 bytes of data received. The sender will continue sending while
 * waiting for the status to arrive, another 128 bytes. In this way,
 * the data transport is not throttled by turn-around time, but is still
 * throttled by how fast the programmer can actually write to the EEPROM.
 */
static int
send_ll_crc(const uint8_t *data, size_t len)
{
    uint     pos = 0;
    uint32_t crc = 0;
    uint32_t cap_pos[2];
    uint     cap_count = 0;
    uint     cap_prod  = 0;
    uint     cap_cons  = 0;
    size_t   percent;
    uint     crc_cap_pos = 0;
    size_t   lpercent = -1;

    discard_input(250);

    while (pos < len) {
        uint tlen = DATA_CRC_INTERVAL;
        if (tlen > len - pos)
            tlen = len - pos;
        if (send_ll_bin(data, tlen))
            return (1);
        crc = crc32(crc, data, tlen);
        data += tlen;
        pos  += tlen;

        if (cap_count >= ARRAY_SIZE(cap_pos)) {
            cap_count--;
            if (check_rc(cap_pos[cap_cons])) {
                return (RC_FAILURE);
            }
            if (++cap_cons >= ARRAY_SIZE(cap_pos))
                cap_cons = 0;
        }

        /* Send and record the current CRC position */
        if (send_ll_bin((uint8_t *)&crc, sizeof (crc))) {
            printf("Data send CRC timeout at 0x%x\n", pos);
            return (RC_TIMEOUT);
        }
        crc_cap_pos = pos;
        cap_pos[cap_prod] = pos;
        if (++cap_prod >= ARRAY_SIZE(cap_pos))
            cap_prod = 0;
        cap_count++;

        percent = (crc_cap_pos * 100) / len;
        if (lpercent != percent) {
            lpercent = percent;
            printf("\r%zu%%", percent);
            fflush(stdout);
        }
    }

    while (cap_count-- > 0) {
        if (check_rc(cap_pos[cap_cons]))
            return (1);
        if (++cap_cons >= ARRAY_SIZE(cap_pos))
            cap_cons = 0;
    }

    printf("\r100%%\n");
    return (0);
}


/*
 * wait_for_text() waits for a specific sequence of characters (string) from
 *                 the programmer. This is typically a command prompt or
 *                 expected status message.
 *
 * @param  [in] str     - Specific text string expected from the programmer.
 * @param  [in] timeout - Number of milliseconds since last character before
 *                        giving up.
 *
 * @return      0 - The text was received from the programmer.
 * @return      1 - A timeout waiting for the text occurred.
 */
static int
wait_for_text(const char *str, int timeout)
{
    int         ch;
    int         timeout_count = 0;
    const char *ptr = str;

#ifdef DEBUG_WAITFOR
    printf("waitfor %02x %02x %02x %02x %s\n",
           str[0], str[1], str[2], str[3], str);
#endif
    while (*ptr != '\0') {
        ch = rx_rb_get();
        if (ch == -1) {
            time_delay_msec(1);
            if (++timeout_count >= timeout) {
                return (1);
            }
            continue;
        }
        timeout_count = 0;
        if (*ptr == ch) {
            ptr++;
        } else {
            ptr = str;
        }
    }
    return (0);
}

/*
 * send_cmd() sends a command string to the programmer, verifying that the
 *            command prompt is present before issuing the command.
 *
 * @param  [in] cmd - Command string to send to the programmer.
 *
 * @return      0 - Command was issued to the programmer.
 * @return      1 - A timeout waiting for the command prompt occurred.
 */
static int
send_cmd(const char *cmd)
{
    send_ll_str("\025");       // ^U  (delete any command text)
    discard_input(50);         // Wait for buffered output to arrive
    send_ll_str("\n");         // ^M  (request new command prompt)

    if (wait_for_text("CMD>", 500)) {
        if (!cmd_quiet)
            warnx("CMD: timeout");
        return (1);
    }

    send_ll_str(cmd);
    send_ll_str("\n");         // ^M (execute command)
    wait_for_text("\n", 200);  // Discard echo of command and newline

    return (0);
}

/*
 * recv_output() receives output from the programmer, stopping on timeout or
 *               buffer length exceeded.
 *
 * @param  [out] buf     - Buffer into which output from the programmer is
 *                         to be captured.
 * @param  [in]  buflen  - Maximum number of bytes to receive.
 * @param  [out] rxcount - Number of bytes actually received.
 * @param  [in]  timeout - Number of milliseconds since last character before
 *                         giving up.
 *
 * @return       This function always returns 0.
 */
static int
recv_output(char *buf, size_t buflen, int *rxcount, int timeout)
{
    *rxcount = receive_ll(buf, buflen, timeout, false);

    if (*rxcount < buflen)
        buf[*rxcount] = '\0';

    if ((*rxcount >= 5) && (strncmp(buf + *rxcount - 5, "CMD> ", 5) == 0))
        *rxcount -= 5;  // Discard trailing CMD prompt

    return (0);
}

/*
 * are_you_sure() prompts the user to confirm that an operation is intended.
 *
 * @param  [in]  None.
 *
 * @return       TRUE  - User has confirmed (Y).
 * @return       FALSE - User has denied (N).
 */
bool
are_you_sure(const char *prompt)
{
    int ch;
    if (force_yes) {
        printf("%s: yes\n", prompt);
        return (true);
    }
ask_again:
    printf("%s -- are you sure? (y/n) ", prompt);
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

/* Byte order swap functions */
#define SWAP_3210(x) (((x) << 24) | ((x) >> 24) | \
                      (((x) & 0xff00) << 8) | (((x) >> 8) & 0xff00))
#define SWAP_2301(x) (((x) << 16) | ((x) >> 16))
#define SWAP_1032(x) ((((x) & 0x00ff00ff) << 8) | (((x) >> 8) & 0x00ff00ff))

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define BESWAP(x) SWAP32(x)
#else
#define BESWAP(x) (x)
#endif

/*
 * detect_byte_order() reports the byte order of the specified buffer
 *                     relative to an unswapped Kickstart ROM image.
 *
 * @param  [in]  bufp  - The buffer to check
 *
 * @return       0123  - Normal unswapped order [0] [1] [2] [3]
 * @return       3210  - Swapped byte order     [3] [2] [1] [0]
 * @return       1032  - Swapped byte order     [1] [0] [3] [2]
 * @return       2301  - Swapped byte order     [2] [3] [0] [1]
 */
static uint
detect_byte_order(uint8_t *bufp)
{
    /* BESWAP() is used because numbers are specified in big endian format */
    static const uint32_t comp[][2] = {
        { BESWAP(0x11144ef9), BESWAP(0x00f800d2) },  // 2.04+
        { BESWAP(0x11114ef9), BESWAP(0x00fc00d2) },  // 1.3
        { BESWAP(0x612e4447), BESWAP(0x00f80190) },  // DiagROM 2.x Beta
        { BESWAP(0x11144447), BESWAP(0x00f800d6) },  // DiagROM 2.x
        { BESWAP(0x11144ef9), BESWAP(0x00f80010) },  // Old ROM Switcher
        { BESWAP(0x11114ef9), BESWAP(0x00f8048c) },  // Logica-Dialoga
        { BESWAP(0x11114ef9), BESWAP(0x00f800f8) },  // AROS
    };
    uint32_t *buf32 = (uint32_t *) bufp;
    uint cur;

    for (cur = 0; cur < ARRAY_SIZE(comp); cur++) {
        if (comp[cur][0] == buf32[0])
            return (0123);
        if (SWAP_1032(comp[cur][0]) == buf32[0])
            return (1032);
        if (SWAP_2301(comp[cur][0]) == buf32[0])
            return (2301);
        if (SWAP_3210(comp[cur][0]) == buf32[0])
            return (3210);
    }
    return (0xffff);
}


/*
 * execute_swapmode() swaps bytes in the specified buffer according to the
 *                    currently active swap mode.
 *
 * @param  [io]  buf     - Buffer to modify.
 * @param  [in]  len     - Length of data in the buffer.
 * @global [in]  dir     - Image swap direction (SWAP_TO_ROM or SWAP_FROM_ROM)
 * @return       None.
 */
static int
execute_swapmode(uint8_t *buf, uint len, uint dir)
{
    bool_t  printed    = FALSE;
    uint    pos;
    uint8_t temp;
    uint    byteorder = detect_byte_order(buf);

    if (swapmode == SWAPMODE_AUTO) {
        printf("Auto swap mode: ");
        switch (kicksmash_mode) {
            default:
            case KICKSMASH_MODE_AUTO:
            case KICKSMASH_MODE_32:
                swapmode = SWAPMODE_32;
                printf("32, ");
                break;
            case KICKSMASH_MODE_16:
                swapmode = SWAPMODE_16;
                printf("16, ");
                break;
            case KICKSMASH_MODE_16HI:
                swapmode = SWAPMODE_16;
                printf("16hi, ");
                break;
            case KICKSMASH_MODE_32SWAP:
                swapmode = SWAPMODE_32SWAP;
                printf("32hi, ");
                break;
        }
        printed = TRUE;
    }

    switch (swapmode) {
        case 0:
        case 0123:
            break;  // Normal (no swap)
        swap_1032:
        case 1032:
            /* Swap adjacent bytes in 16-bit words */
            for (pos = 0; pos < len - 1; pos += 2) {
                temp         = buf[pos + 0];
                buf[pos + 0] = buf[pos + 1];
                buf[pos + 1] = temp;
            }
            break;
        swap_2301:
        case 2301:
            /* Swap adjacent (16-bit) words */
            for (pos = 0; pos < len - 3; pos += 4) {
                temp         = buf[pos + 0];
                buf[pos + 0] = buf[pos + 2];
                buf[pos + 2] = temp;
                temp         = buf[pos + 1];
                buf[pos + 1] = buf[pos + 3];
                buf[pos + 3] = temp;
            }
            break;
        swap_3210:
        case 3210:
            /* Swap bytes in 32-bit longs */
            for (pos = 0; pos < len - 3; pos += 4) {
                temp         = buf[pos + 0];
                buf[pos + 0] = buf[pos + 3];
                buf[pos + 3] = temp;
                temp         = buf[pos + 1];
                buf[pos + 1] = buf[pos + 2];
                buf[pos + 2] = temp;
            }
            break;
        case SWAPMODE_16:
            if (dir == SWAP_TO_ROM) {
                /* Need bytes in order: 14 11 f9 4e == 1032 */
                if (byteorder == 1032)
                    break;  // Already in desired order
                if (byteorder == 0123) {
                    printf("Swapping 2301, ");
                    goto swap_2301;  // Swap adjacent 16-bit words
                }
            }
            if (dir == SWAP_FROM_ROM) {
                /* Need bytes in order: 11 14 4e f9 == 3210 */
                if (byteorder == 0123)
                    break;  // Already in desired order
                if (byteorder == 2301) {
                    printf("Swapping 1032, ");
                    goto swap_1032;  // Swap odd/even bytes
                }
            }
            goto unrecognized;
        case SWAPMODE_32SWAP:
            if (dir == SWAP_TO_ROM) {
                /* Need bytes in order: 14 11 f9 4e == 1032 */
                if (byteorder == 1032) {
                    if (printed)
                        printf("No swap, ");
                    break;  // Already in desired order
                }
                if (byteorder == 2301) {
                    printf("Swapping 3210, ");
                    goto swap_3210;  // Swap bytes in 32-bit longs
                }
                if (byteorder == 3210) {
                    printf("Swapping 2301, ");
                    goto swap_2301;  // Swap adjacent 16-bit words
                }
                if (byteorder == 0123) {
                    printf("Swapping 1032, ");
                    goto swap_1032;  // Swap odd/even bytes
                }
            }
            if (dir == SWAP_FROM_ROM) {
                /* Need bytes in order: 4e f9 11 14 == 2301 */
                if (byteorder == 2301) {
                    if (printed)
                        printf("No swap, ");
                    break;  // Already in desired order
                }
                if (byteorder == 1032) {
                    printf("Swapping 3210, ");
                    goto swap_3210;  // Swap bytes in 32-bit longs
                }
                if (byteorder == 0123) {
                    printf("Swapping 2301, ");
                    goto swap_2301;  // Swap adjacent 16-bit words
                }
                if (byteorder == 3210) {
                    printf("Swapping 1032, ");
                    goto swap_1032;  // Swap odd/even bytes
                }
            }
            goto unrecognized;
        case SWAPMODE_32:
            if (dir == SWAP_TO_ROM) {
                /* Need bytes in order: f9 4e 14 11 == 3210 */
                if (byteorder == 3210) {
                    if (printed)
                        printf("No swap, ");
                    break;  // Already in desired order
                }
                if (byteorder == 0123) {
                    printf("Swapping 3210, ");
                    goto swap_3210;  // Swap bytes in 32-bit longs
                }
                if (byteorder == 1032) {
                    printf("Swapping 2301, ");
                    goto swap_2301;  // Swap adjacent 16-bit words
                }
                if (byteorder == 2301) {
                    printf("Swapping 1032, ");
                    goto swap_1032;  // Swap odd/even bytes
                }
            }
            if (dir == SWAP_FROM_ROM) {
                /* Need bytes in order: 11 14 4e f9 == 0123 */
                if (byteorder == 0123) {
                    if (printed)
                        printf("No swap, ");
                    break;  // Already in desired order
                }
                if (byteorder == 3210) {
                    printf("Swapping 3210, ");
                    goto swap_3210;  // Swap bytes in 32-bit longs
                }
                if (byteorder == 1032) {
                    printf("Swapping 2301, ");
                    goto swap_2301;  // Swap adjacent 16-bit words
                }
                if (byteorder == 2301) {
                    printf("Swapping 1032, ");
                    goto swap_1032;  // Swap odd/even bytes
                }
            }
            goto unrecognized;
        default:
unrecognized:
            printf("\n");
            warnx("Unrecognized Amiga ROM format: %02x %02x %02x %02x\n"
                  "If the ROM is in byte-order format, the following option "
                  "may work: -s 3210\n",
                  buf[0], buf[1], buf[2], buf[3]);
            return (1);
    }
#undef DEBUG_SWAPMODE
#ifdef DEBUG_SWAPMODE
    printf("Swapped:");
    for (pos = 0; pos < 4; pos++)
        printf(" %02x", buf[pos]);
    printf(", ");
#endif
    printf("Length 0x%x\n", len);
    return (0);
}

/*
 * eeprom_erase() sends a command to the programmer to erase a sector,
 *                a range of sectors, or the entire EEPROM.
 *
 * @param  [in]  bank  - Starting address addition multiplier for erase
 *                       length or BANK_NOT_SPECIFIED.
 * @param  [in]  addr  - The EEPROM starting address to erase.
 *                       ADDR_NOT_SPECIFIED will cause the entire chip to
 *                       be erased.
 * @param  [in]  len   - The length (in bytes) to erase. A value of
 *                       EEPROM_SIZE_NOT_SPECIFIED will cause a single
 *                       sector to be erased.
 * @return       None.
 */
static int
eeprom_erase(uint bank, uint addr, uint len)
{
    int  rxcount;
    char cmd_output[1024];
    char cmd[64];
    int  count;
    int  no_data;
    char prompt[80];

    if (bank != BANK_NOT_SPECIFIED) {
        if (addr == ADDR_NOT_SPECIFIED)
            addr = 0;
        addr += bank * EEPROM_BANK_SIZE_DEFAULT;
    }

    snprintf(cmd, sizeof (cmd) - 1, "prom id");
    if (send_cmd(cmd))
        return (1);  // "timeout" was reported in this case
    if (recv_output(cmd_output, sizeof (cmd_output), &rxcount, 80))
        return (1); // "timeout" was reported in this case
    if (rxcount == 0) {
        printf("Device ID timeout\n");
        return (1);
    }
    if (strcasestr(cmd_output, "Unknown") != NULL) {
        if (rxcount < sizeof (cmd_output))
            cmd_output[rxcount] = '\0';  // Eliminate "CMD>" prompt at end
        printf("Device ID failed: %s\n", cmd_output);
        return (1);
    }

    if (addr == ADDR_NOT_SPECIFIED) {
        /* Chip erase */
        sprintf(prompt, "Erase entire EEPROM");
        snprintf(cmd, sizeof (cmd) - 1, "prom erase chip");
    } else if (len == EEPROM_SIZE_NOT_SPECIFIED) {
        /* Single sector erase */
        sprintf(prompt, "Erase sector at 0x%x", addr);
        snprintf(cmd, sizeof (cmd) - 1, "prom erase %x", addr);
    } else {
        /* Possible multi-sector erase */
        sprintf(prompt, "Erase sector(s) from 0x%x to 0x%x", addr, addr + len);
        snprintf(cmd, sizeof (cmd) - 1, "prom erase %x %x", addr, len);
    }
    if (are_you_sure(prompt) == false)
        return (1);
    cmd[sizeof (cmd) - 1] = '\0';

    if (send_cmd(cmd))
        return (1);  // send_cmd() reported "timeout" in this case

    no_data = 0;
    for (count = 0; count < 1000; count++) {  // 100 seconds max
        if (recv_output(cmd_output, sizeof (cmd_output), &rxcount, 100))
            return (1); // "timeout" was reported in this case
        if (rxcount == 0) {
            if (no_data++ == 40) {
                printf("Receive timeout\n");
                return (1);  // No output for 4 seconds
            }
        } else {
            no_data = 0;
            printf("%.*s", rxcount, cmd_output);
            fflush(stdout);
            if ((strstr(cmd_output, "FAIL") != NULL) ||
                (strstr(cmd_output, "Invalid>") != NULL)) {
                return (1);
            }
            if (strstr(cmd_output, "CMD>") != NULL) {
                /* Normal end */
                break;
            }
        }
    }
    return (0);
}

#if 0
static int
eeprom_not_erased(uint bank, uint addr, uint len)
{
    int  pos = 0;
    int  spos;
    int  rxcount;
    char cmd_output[1024];
    char cmd[64];
    uint complen = len;
    uint curpos;
    uint value;
    uint paddr;

    if (len > 0x8000)
        printf("Verifying EEPROM area has been erased\n");
    if (bank != BANK_NOT_SPECIFIED) {
        if (addr == ADDR_NOT_SPECIFIED)
            addr = 0;
        addr += bank * EEPROM_BANK_SIZE_DEFAULT;
    }

    /* Manually check the first 32 bytes */
    if (complen > 0x20)
        complen = 0x20;
    sprintf(cmd, "dr prom %x %x", addr, complen);
    if (send_cmd(cmd))
        return (-1);  // send_cmd() reported "timeout" in this case
    if (recv_output(cmd_output, sizeof (cmd_output), &rxcount, 80))
        return (-1); // "timeout" was reported in this case

    for (curpos = 0; curpos < (complen + 3) / 4; curpos++) {
        if ((sscanf(cmd_output + pos, "%x%n", &value, &spos) != 1) ||
            (spos == 0)) {
            printf("scan failed at %s\n", cmd_output + pos);
            return (-1);
        }
        if (spos == 0) {
            return (-1);
        }
        if (value != 0xffffffff)
            return (1);  // Value not erased
        pos += spos;
    }

    paddr = addr;
    addr += complen;
    len  -= complen;
    if (len == 0)
        return (0);  // All data checked

    /* First 32 bytes passed. Now scale up to compare remainder. */
    while (len > 0) {
        if (complen > len)
            complen = len;

        sprintf(cmd, "comph prom %x prom %x %x", paddr, addr, complen);
#ifdef ERASE_CHECK_DEBUG
        printf("-> %s\n", cmd);
#endif
        if (send_cmd(cmd))
            return (-1);  // send_cmd() reported "timeout" in this case
        if (recv_output(cmd_output, sizeof (cmd_output), &rxcount,
                        complen / 256)) {
            return (-1); // "timeout" was reported in this case
        }

        if (strstr(cmd_output, "mismatch") != NULL)
            return (1);  // found mismatch

        paddr = addr;
        addr += complen;
        len -= complen;
        complen *= 2;
    }
    /* Completely erased */
    return (0);
}
#endif


/*
 * eeprom_id() sends a command to the programmer to request the EEPROM id.
 *             Response output is displayed for the user.
 *
 * @param  [in]  None.
 * @return       None.
 */
static void
eeprom_id(void)
{
    char cmd_output[100];
    int  rxcount;
    if (send_cmd("prom id"))
        return; // "timeout" was reported in this case
    if (recv_output(cmd_output, sizeof (cmd_output), &rxcount, 80))
        return; // "timeout" was reported in this case
    if (rxcount == 0)
        printf("Receive timeout\n");
    else
        printf("%.*s", rxcount, cmd_output);
}

static void
get_kicksmash_mode(void)
{
    char cmd[64];
    char cmd_output[80];
    int  rxcount;

    strcpy(cmd, "prom mode");
    if (send_cmd(cmd))
        exit(1); // "timeout" was reported in this case
    if (recv_output(cmd_output, sizeof (cmd_output), &rxcount, 100))
        exit(1); // "timeout" was reported in this case
    if (rxcount == 0)
        errx(EXIT_FAILURE, "Kicksmash receive timeout");
    if ((sscanf(cmd_output, "%u%n", &kicksmash_mode, &rxcount) != 1) ||
        (rxcount == 0) ||
        ((cmd_output[rxcount] != ' ') && (cmd_output[rxcount] != '\0'))) {
        errx(EXIT_FAILURE, "Bad response from Kicksmash: \"%s\"", cmd_output);
    }
}

/*
 * eeprom_read() reads all or part of the EEPROM image from the programmer,
 *               writing output to a file.
 *
 * @param  [in]  filename        - The file to write using EEPROM contents.
 * @param  [in]  bank            - Starting address addition multiplier for
 *                                 read length or BANK_NOT_SPECIFIED.
 * @param  [in]  addr            - The EEPROM starting address.
 * @param  [in]  len             - The length to write. A value of
 *                                 EEPROM_SIZE_NOT_SPECIFIED will use the
 *                                 size of the EEPROM as the length to write.
 * @return       None.
 * @exit         EXIT_FAILURE - The program will terminate on file access error.
 */
static int
eeprom_read(const char *filename, uint bank, uint addr, uint len)
{
    char cmd[64];
    char *eebuf;
    int rxcount;
    int rc = 0;

    if (addr == ADDR_NOT_SPECIFIED)
        addr = 0x000000;  // Start of EEPROM

    if (len == EEPROM_SIZE_NOT_SPECIFIED) {
        if (bank != BANK_NOT_SPECIFIED)
            len = EEPROM_BANK_SIZE_DEFAULT;
        else
            len = EEPROM_SIZE_DEFAULT - addr;
    }

    if (bank != BANK_NOT_SPECIFIED)
        addr += bank * EEPROM_BANK_SIZE_DEFAULT;

    eebuf = malloc(len + 4);
    if (eebuf == NULL)
        errx(EXIT_FAILURE, "Could not allocate %u byte buffer", len);

    snprintf(cmd, sizeof (cmd) - 1, "prom read %x %x", addr, len);
    cmd[sizeof (cmd) - 1] = '\0';
    if (send_cmd(cmd)) {
        rc = 1;
        goto read_fail; // "timeout" was reported in this case
    }
    rxcount = receive_ll_crc(eebuf, len);
    if (rxcount == -1) {
        rc = 1;
        goto read_fail;  // Send error was reported
    }
    if (rxcount < len) {
        printf("Receive failed at byte 0x%x.\n", rxcount);
        if (strncmp(eebuf + rxcount - 11, "FAILURE", 8) == 0) {
            rxcount -= 11;
            printf("Read %.11s\n", eebuf + rxcount);
        }
    }
    if (rxcount > 0) {
        size_t written;
        FILE *fp = fopen(filename, "wb");
        if (fp == NULL)
            err(EXIT_FAILURE, "Failed to open %s", filename);
        if (execute_swapmode((uint8_t *)eebuf, rxcount, SWAP_FROM_ROM)) {
            rc = 1;
            /* Continue to write file which can't be swapped */
        }
        written = fwrite(eebuf, rxcount, 1, fp);
        if (written != 1)
            err(EXIT_FAILURE, "Failed to write %s", filename);
        fclose(fp);
        printf("Read 0x%x bytes from device and wrote to file %s\n",
               rxcount, filename);
    }
read_fail:
    free(eebuf);
    return (rc);
}

static uint8_t *
file_read(const char *filename, uint len)
{
    FILE    *fp;
    uint8_t *filebuf;

    filebuf = malloc(len);
    if (filebuf == NULL)
        errx(EXIT_FAILURE, "Could not allocate %u bytes", len);

    fp = fopen(filename, "rb");
    if (fp == NULL)
        errx(EXIT_FAILURE, "Failed to open %s", filename);
    if (fread(filebuf, len, 1, fp) != 1)
        errx(EXIT_FAILURE, "Failed to read %u bytes from %s", len, filename);
    fclose(fp);

    return (filebuf);
}

/*
 * show_rx_peek() displays as ASCII text the most recent output received
 *                from Kicksmash. This is done as an aid in debugging failures.
 */
static void
show_rx_peek(void)
{
    char buf[1024];
    uint len = rx_rb_peek(buf, sizeof (buf));
    uint pos;
    uint print_count = 0;
    uint last_was_space = 1;
    printf("KS FW: ");
    for (pos = 0; pos < len; pos++) {
        char ch = buf[pos];
        if (print_count == 80) {
            print_count = 0;
            printf("\n       ");
        }
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\0') {
            if (ch == '\r' || ch == '\n') {
                if (print_count != 0) {
                    last_was_space = 1;
                    print_count = 0;
                    printf("\n       ");
                }
            } else if (last_was_space == 0) {
                last_was_space = 1;
                putchar(' ');
                print_count++;
            }
        } else if (ch > ' ' && ch <= '~') {
            putchar(ch);
            print_count++;
            last_was_space = 0;
        }
    }
    if (print_count > 0)
        printf("\n");
}

/*
 * eeprom_write() uses the programmer to writes all or part of an EEPROM image.
 *                Content to write is sourced from a local file.
 *
 * @param  [in]  filebuf         - The file content to write.
 * @param  [in]  addr            - The EEPROM starting address.
 * @param  [io]  len             - The length to write.
 * @return       0 - Verify successful.
 * @return       1 - Verify failed.
 * @exit         EXIT_FAILURE - The program will terminate on file access error.
 */
static uint
eeprom_write(const uint8_t *filebuf, uint addr, uint len)
{
    char        cmd[64];
    int         tcount = 0;

    printf("Writing 0x%06x bytes to EEPROM starting at address 0x%x\n",
           len, addr);
#ifdef __MINGW32__
    DWORD dwTickStart = GetTickCount();
#endif
    snprintf(cmd, sizeof (cmd) - 1, "prom write %x %x", addr, len);
    if (send_cmd(cmd))
        return (-1); // "timeout" was reported in this case

    if (send_ll_crc(filebuf, len)) {
        show_rx_peek();
        errx(EXIT_FAILURE, "Send failure");
    }

    while (tx_rb_flushed() == FALSE) {
        if (tcount++ > 500)
            errx(EXIT_FAILURE, "Send timeout");

        time_delay_msec(1);
    }
    printf("Wrote 0x%x bytes to device\n", len);
#ifdef __MINGW32__
    DWORD dwElapsed = (GetTickCount() - dwTickStart);
    if (dwElapsed > 0) {
        printf("Elapsed time = %lums\n", dwElapsed);
        printf("bytes/sec = %lu\n", (DWORD)(len/((float)dwElapsed/1000.0)));
    }
#endif

    return (0);
}

/*
 * show_fail_range() displays the contents of the range over which a verify
 *                   error has occurred.
 *
 * @param  [in]  filebuf         - File data to compare.
 * @param  [in]  eebuf           - EEPROM data to compare.
 * @param  [in]  len             - Length of data to compare.
 * @param  [in]  addr            - Base address of EEPROM contents.
 * @param  [in]  filepos         - Base address of file contents.
 * @param  [in]  miscompares_max - Maximum number of miscompares to report.
 *
 * @return       None.
 */
static void
show_fail_range(const uint8_t *filebuf, const uint8_t *eebuf, uint len,
                uint addr, uint filepos, uint miscompares_max)
{
    uint pos;

    printf("file   0x%06x:", filepos);
    for (pos = 0; pos < len; pos++) {
        if ((pos >= 16) && (miscompares_max != 0xffffffff)) {
            printf("...");
            break;
        }
        printf(" %02x", (uint8_t) filebuf[filepos + pos]);
    }

    printf("\neeprom 0x%06x:", addr + filepos);
    for (pos = 0; pos < len; pos++) {
        if ((pos >= 16) && (miscompares_max != 0xffffffff)) {
            printf("...");
            break;
        }
        printf(" %02x", (uint8_t) eebuf[filepos + pos]);
    }
    printf("\n");
}


/*
 * eeprom_verify() reads an image from the eeprom and compares it against
 *                 a file on disk. Differences are reported for the user.
 *
 * @param  [in]  filename        - The file to compare EEPROM contents against.
 * @param  [in]  addr            - The EEPROM starting address.
 * @param  [in]  len             - The length to compare.
 * @param  [in]  miscompares_max - Specifies the maximum number of miscompares
 *                                 to verbosely report.
 * @return       0 - Verify successful.
 * @return       1 - Verify failed.
 * @exit         EXIT_FAILURE - The program will terminate on file access error.
 */
static int
eeprom_verify(const uint8_t *filebuf, uint addr, uint len, uint miscompares_max)
{
    uint8_t    *eebuf;
    char        cmd[64];
    int         rxcount;
    int         pos;
    int         first_fail_pos = -1;
    uint        miscompares = 0;

    eebuf = malloc(len + 4);

    snprintf(cmd, sizeof (cmd) - 1, "prom read %x %x", addr, len);
    cmd[sizeof (cmd) - 1] = '\0';
    if (send_cmd(cmd)) {
        /* "timeout" was reported in this case */
fail_verify_read:
        free(eebuf);
        return (1);
    }
    rxcount = receive_ll_crc(eebuf, len);
    if (rxcount <= 0)
        goto fail_verify_read; // "timeout" was reported in this case
    if (rxcount < len) {
        const char *str = (const char *) eebuf + rxcount - 11;
        if ((strncmp(str, "FAILURE", 8) == 0) ||
            (strcasestr(str, "FAILURE") != NULL)) {
            rxcount -= 11;
            printf("Read %.11s\n", eebuf + rxcount);
        }
        printf("Only read 0x%x bytes of expected 0x%x\n", rxcount, len);
        goto fail_verify_read;
    }

    /* Compare two buffers */
    for (pos = 0; pos < len; pos++) {
        if (eebuf[pos] != filebuf[pos]) {
            miscompares++;
            if (first_fail_pos == -1)
                first_fail_pos = pos;
            if (miscompares == miscompares_max) {
                /* Report now and only count further miscompares */
                show_fail_range(filebuf, eebuf, pos - first_fail_pos + 1,
                                addr, first_fail_pos, miscompares_max);
                first_fail_pos = -1;
            }
        } else {
            if ((pos < len - 1) &&
                (eebuf[pos + 1] != filebuf[pos + 1])) {
                /* Consider single byte matches part of failure range */
                continue;
            }
            if (first_fail_pos != -1) {
                if (miscompares < miscompares_max) {
                    /* Report previous range */
                    show_fail_range(filebuf, eebuf, pos - first_fail_pos,
                                    addr, first_fail_pos, miscompares_max);
                }
                first_fail_pos = -1;
            }
        }
    }
    if ((first_fail_pos != -1) && (miscompares < miscompares_max)) {
        /* Report final range not previously reported */
        show_fail_range(filebuf, eebuf, pos - first_fail_pos, addr,
                        first_fail_pos, miscompares_max);
    }
    free(eebuf);
    if (miscompares) {
        printf("%u miscompares\n", miscompares);
        return (1);
    } else {
        printf("Verify success\n");
        return (0);
    }
}

/*
 * amiga_is_in_reset
 * -----------------
 * Returns 1 if the Amiga is in reset.
 * Returns 0 if the Amiga is not in reset.
 * Returns -1 if there was an error communicating with Kicksmash.
 */
static int
amiga_is_in_reset(void)
{
    char cmd_output[100];
    int  rxcount;
    if (send_cmd("prom id"))
        return (-1); // "timeout" was reported in this case
    if (recv_output(cmd_output, sizeof (cmd_output), &rxcount, 80))
        return (-1); // "timeout" was reported in this case
    if (rxcount == 0) {
        printf("Receive timeout\n");
        return (-1);
    }
    if (strstr(cmd_output, "not in reset") != NULL)
        return (0);
    return (1);
}

static int
reset_amiga(int hold)
{
    const char *cmd = hold ? "reset amiga hold" : "reset prom";

    if (send_cmd(cmd))
        return (1);
    return (0);
}

#ifdef __MINGW32__
/*
 * winkey_special() handles special windows virtual keystrokes, turning
 *                  them into input keystrokes which are compatible with
 *                  Kicksmash firmware input handling.
 */
static void
winkey_special(int ch)
{
    switch (ch) {
        case VK_LEFT:
            tx_rb_put(0x1b);
            tx_rb_put('[');
            tx_rb_put('D');
            break;
        case VK_RIGHT:
            tx_rb_put(0x1b);
            tx_rb_put('[');
            tx_rb_put('C');
            break;
        case VK_UP:
            tx_rb_put(0x1b);
            tx_rb_put('[');
            tx_rb_put('A');
            break;
        case VK_DOWN:
            tx_rb_put(0x1b);
            tx_rb_put('[');
            tx_rb_put('B');
            break;
        case VK_INSERT:
            tx_rb_put(0x1b);
            tx_rb_put('[');
            tx_rb_put('2');
            tx_rb_put('~');
            break;
        case VK_HOME:
            tx_rb_put(0x1b);
            tx_rb_put('[');
            tx_rb_put('1');
            tx_rb_put('~');
            break;
        case VK_END:
            tx_rb_put(0x1b);
            tx_rb_put('[');
            tx_rb_put('F');
            break;
        case VK_DELETE:
            tx_rb_put(4);  // ^D
            break;
        case VK_ESCAPE:
            tx_rb_put(0x1b);
            break;
        default:
//          printf("[%x]", ch);
            break;
    }
}
#endif

/*
 * run_terminal_mode() implements a terminal interface with the KickSmash
 *                     command line.
 *
 * @param  [in]  None.
 * @global [in]  device_name[] is the path to the device which was opened.
 * @return       None.
 */
static void
run_terminal_mode(void)
{
    int              ch         = 0;
    bool_t           literal    = FALSE;
#ifdef USE_NON_BLOCKING_TTY
    int              enable     = 1;
#endif

    if (terminal_cmd != NULL) {
        /*
         * A previous hostsmash operation (such as --fwwrite) may have left
         * Kicksmash in "prom service" mode, where it discards text until
         * it receives CR, LF, or ^C. The command would then be lost. Send
         * CR first and wait for the command prompt, without displaying it.
         */
        terminal_mode = FALSE;
        send_ll_str("\r");
        (void) wait_for_text("CMD>", 500);
        discard_input(50);
        terminal_mode = TRUE;
    }

#ifdef __MINGW32__
    HANDLE ihandle = GetStdHandle(STD_INPUT_HANDLE);
    DWORD inputtype;

    if (ihandle == INVALID_HANDLE_VALUE)
        errx(EXIT_FAILURE, "Bad input handle");

    if (terminal_cmd != NULL) {
        inputtype = FILE_TYPE_UNKNOWN;
    } else {
        inputtype = GetFileType(ihandle);
    }
    if (inputtype == FILE_TYPE_CHAR) {
        /* Set ihandle to raw input */
        DWORD mode;
        if (!GetConsoleMode(ihandle, &mode))
            system_error("getting input mode");
        mode &= ~ENABLE_PROCESSED_INPUT;
        mode |= ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT;
        if (!SetConsoleMode(ihandle, mode))
            system_error("setting input mode");

        printf("<< Type ^X to exit.  Opened %s >>\n", device_name);
    }

    while (running) {
        INPUT_RECORD inbuffer[120];
        char buffer[256];
        DWORD cur;
        DWORD read_count;

        while (tx_rb_space() == 0)
            time_delay_msec(20);

        if (terminal_cmd != NULL) {
            ch = *(terminal_cmd++);
            if (ch == '\0') {
                /* End of command */
                ch = '\r';
                tx_rb_put(ch);
                time_delay_msec(400);
                do_exit(EXIT_SUCCESS);
            }
            tx_rb_put(ch);
        } else if (inputtype == FILE_TYPE_CHAR) {
            if (PeekConsoleInput(ihandle, inbuffer, 128, &read_count) == 0) {
                system_error("PeekConsoleInput");
                running = 0;
                break;
            }
            if (read_count == 0)
                continue;
            if (ReadConsoleInput(ihandle, inbuffer, 128, &read_count) == 0) {
                system_error("ReadConsoleInput");
                running = 0;
                break;
            }
            for (cur = 0; cur < read_count; cur++) {
                // KEY_EVENT MOUSE_EVENT
                if (inbuffer[cur].EventType == KEY_EVENT) {
                    if (inbuffer[cur].Event.KeyEvent.bKeyDown) {
                        ch = inbuffer[cur].Event.KeyEvent.uChar.AsciiChar;
                        if (ch == 0) {
                            ch = inbuffer[cur].Event.KeyEvent.wVirtualKeyCode;
                            winkey_special(ch);
                            continue;
                        }
                        if (literal == TRUE) {
                            literal = FALSE;
                            tx_rb_put(ch);
                            ch = 0;
                            continue;
                        }
                        if (ch == 0x16) {        // ^V
                            literal = TRUE;
                            ch = 0;
                            continue;
                        }
                        if (ch == 0x18)  // ^X
                            do_exit(EXIT_SUCCESS);
                        tx_rb_put(ch);
                    }
                }
                if (ch == 0x18)  // ^X
                    break;
            }
        } else {
            uint pos;
            if (ReadFile(ihandle, buffer, sizeof (buffer),
                         &read_count, NULL) == 0) {
                system_error("ReadFile");
            }
            if (read_count == 0)
                break;
            for (pos = 0; pos < read_count; pos++) {
                ch = buffer[pos];
                tx_rb_put(ch);
            }
        }
    }
    time_delay_msec(400);
#else
    /* Linux / MacOS */
    if ((terminal_cmd == NULL) && isatty(fileno(stdin))) {
        struct termios term;
        if (tcgetattr(0, &saved_term))
            errx(EXIT_FAILURE, "Could not get terminal information");

        got_terminfo = 1;

        term = saved_term;
        cfmakeraw(&term);
        term.c_oflag |= OPOST;
#ifdef DEBUG_CTRL_C_KILL
        term.c_lflag |= ISIG;   // Enable to not trap ^C
#endif
        tcsetattr(0, TCSANOW, &term);
#ifdef USE_NON_BLOCKING_TTY
        if (ioctl(fileno(stdin), FIONBIO, &enable))  // Set input non-blocking
            warn("FIONBIO failed for stdin");
#endif
        printf("<< Type ^X to exit.  Opened %s >>\n", device_name);
    }

    while (running) {
        ssize_t len;

        while (tx_rb_space() == 0)
            time_delay_msec(20);

        if (terminal_cmd != NULL) {
            ch = *(terminal_cmd++);
            if (ch == '\0') {
                /* End of command */
                ch = '\r';
                tx_rb_put(ch);
                time_delay_msec(400);
                do_exit(EXIT_SUCCESS);
            }
        } else if ((len = read(0, &ch, 1)) <= 0) {
            if (len == 0) {
                time_delay_msec(400);
                do_exit(EXIT_SUCCESS);
            }
            if (errno != EAGAIN) {
                warn("read failed");
                do_exit(EXIT_FAILURE);
            }
            ch = -1;
        }
#ifdef USE_NON_BLOCKING_TTY
        if (ch == 0) {                   // EOF
            time_delay_msec(400);
            do_exit(EXIT_SUCCESS);
        }
#endif
        if (literal == TRUE) {
            literal = FALSE;
            tx_rb_put(ch);
            continue;
        }
        if (ch == 0x16) {                  // ^V
            literal = TRUE;
            continue;
        }

        if (ch == 0x18)  // ^X
            do_exit(EXIT_SUCCESS);

        if (ch >= 0)
            tx_rb_put(ch);
    }
#endif
    printf("not running\n");
    running = 0;
}

#define LINUX_BY_ID_DIR "/dev/serial/by-id"

/*
 * Locating the tty device associated with the USB connection of Kicksmash
 * (ks_port_list() and find_mx_programmer()).
 *
 * OS-specific implementation notes are below
 * Linux
 *     /dev/serial-by-id contains a directory of currently attached USB
 *     serial adapters. It's unfortunately not inclusive of onboard serial
 *     ports (as far as I know), but then no modern computer has these
 *     as far as I know.
 *
 *     Another option available on Linux
 *         Linux Path to tty FTDI info:
 *         /sys/class/tty/ttyUSB0/../../../../serial
 *              example: AM01F9T1
 *         /sys/class/tty/ttyUSB0/../../../../idVendor
 *              example: 0403
 *         /sys/class/tty/ttyUSB0/../../../../idProduct
 *              example: 6001
 *         /sys/class/tty/ttyUSB0/../../../../uevent
 *              has DEVNAME (example: "bus/usb/001/031")
 *              has BUSNUM (example: "001")
 *              has DEVNUM (example: "031")
 *         From the above, one could use /dev/bus/usb/001/031 to open a
 *              serial device which corresponds to the installed USB host.
 *              Unfortunately, the "../../../../" is not consistent across
 *              USB devices.
 *      Additionally on Linux, one could
 *          Walk USB busses/devices and search for the programmer.
 *          Then use /dev/bus/usb/<dirname>/<filename> to find the unique
 *              major.minor
 *          And use /sys/dev/char/<major>:<minor> to find the sysfs entry
 *              for the top node. Then walk the subdirectories to find the
 *              tty. This is a pain.
 *
 * MacOS (OSX)
 *      The ioreg utility is used with the "-lrx -c IOUSBHostDevice" to provide
 *          currently attached USB device information, including the path
 *          to any instantiated serial devices. The output is processed by
 *          this code using a simple state machine which first searches for
 *          the "MX29F1615" string and then takes the next serial device
 *          path located on a line with the "IOCalloutDevice" string.
 *      Additionally on MacOS, one could use the ioreg utility to output in
 *          archive format (-a option) which is really XML. An XML library
 *          could be used to parse that output. I originally started down
 *          that path, but found that the function of parsing that XML just
 *          to find the serial path was way too cumbersome and code-intensive.
 *
 * Windows
 *      Walk HKLM\SYSTEM\CurrentControlSet\Enum\USB (and USBCCGP) looking
 *          for keys containing VID_1209&PID_1610.  For each instance that
 *          has a Device Parameters\PortName value, collect the COM port
 *          only if that port is currently present in
 *          HARDWARE\DEVICEMAP\SERIALCOMM (i.e. the device is plugged in
 *          now, not merely remembered from a prior connection).
 *          The instance ID of the USB device is its serial number; a
 *          composite device's COM port is under an interface instance
 *          which is named with the device's ParentIdPrefix value.
 *          If exactly one Kicksmash is present, auto-select it; if several
 *          are present, list the COM ports and require the user to pass -d.
 */
/* A Kicksmash serial port on the host */
typedef struct {
    char path[PATH_MAX];  // Device path to open
    char serial[64];      // USB serial number (empty if unknown)
    char usbpath[32];     // USB port path, Linux only (empty if unknown)
} ks_port_t;

#define KS_PORTS_MAX 16

static char select_serial[64];  // --serial: select device by serial number

#ifdef LINUX
/*
 * ks_sysfs_read() reads a one-line sysfs attribute into buf.
 */
static void
ks_sysfs_read(const char *dir, const char *attr, char *buf, size_t buflen)
{
    char  path[PATH_MAX + 32];
    FILE *fp;

    buf[0] = '\0';
    snprintf(path, sizeof (path), "%s/%s", dir, attr);
    fp = fopen(path, "r");
    if (fp == NULL)
        return;
    if (fgets(buf, buflen, fp) == NULL)
        buf[0] = '\0';
    fclose(fp);
    buf[strcspn(buf, "\r\n")] = '\0';
}
#endif

/*
 * ks_port_list() returns the Kicksmash serial ports currently attached.
 *
 * @param [out] ports - Array to receive port information.
 * @param [in]  max   - Maximum number of ports to return.
 *
 * @return      Number of ports found.
 */
static uint
ks_port_list(ks_port_t *ports, uint max)
{
    uint nports = 0;
#ifdef LINUX
    /*
     * For Linux, walk /dev/serial/by-id looking for a name which matches
     * the programmer. The same device may appear more than once (usb-
     * and pci- prefixes), so ports are de-duplicated by tty device. The
     * USB serial number and port path come from sysfs.
     */
    char           ttys[KS_PORTS_MAX][PATH_MAX];
    DIR           *dirp;
    struct dirent *dent;
    uint           pos;

    dirp = opendir(LINUX_BY_ID_DIR);
    if (dirp == NULL)
        return (0);  // Old version of Linux?
    while (((dent = readdir(dirp)) != NULL) && (nports < max) &&
           (nports < KS_PORTS_MAX)) {
        char        link[PATH_MAX];
        char        tty[PATH_MAX];
        char        sysdev[PATH_MAX];
        char        usbdev[PATH_MAX];
        const char *tname;
        char       *ptr;

        if ((strstr(dent->d_name, "MX29F1615") == NULL) &&
            (strstr(dent->d_name, "KickSmash") == NULL)) {
            continue;
        }
        if (((size_t) snprintf(link, sizeof (link), "%s/%s", LINUX_BY_ID_DIR,
                               dent->d_name) >= sizeof (link)) ||
            (realpath(link, tty) == NULL)) {
            continue;
        }
        for (pos = 0; pos < nports; pos++)
            if (strcmp(ttys[pos], tty) == 0)
                break;
        if (pos < nports) {
            /* Duplicate: prefer the usb- name */
            if (strncmp(dent->d_name, "usb-", 4) == 0)
                strcpy(ports[pos].path, link);
            continue;
        }
        strcpy(ttys[nports], tty);
        memset(&ports[nports], 0, sizeof (ports[nports]));
        strcpy(ports[nports].path, link);

        /* /sys/class/tty/ttyACM0/device -> .../1-6.1.6/1-6.1.6:1.0 */
        tname = strrchr(tty, '/');
        tname = (tname == NULL) ? tty : tname + 1;
        if (((size_t) snprintf(link, sizeof (link), "/sys/class/tty/%s/device",
                               tname) < sizeof (link)) &&
            (realpath(link, sysdev) != NULL)) {
            strcpy(usbdev, sysdev);
            ptr = strrchr(usbdev, '/');
            if (ptr != NULL) {
                *ptr = '\0';
                ks_sysfs_read(usbdev, "serial", ports[nports].serial,
                              sizeof (ports[nports].serial));
                ptr = strrchr(usbdev, '/');
                ptr = (ptr != NULL) ? ptr + 1 : usbdev;
                if (strlen(ptr) < sizeof (ports[nports].usbpath))
                    strcpy(ports[nports].usbpath, ptr);
            }
        }
        nports++;
    }
    closedir(dirp);
#endif
#ifdef OSX
    char buf[256];
    char serial[64] = "";
    bool_t saw_programmer = FALSE;
    FILE *fp = popen("ioreg -lrx -c IOUSBHostDevice", "r");
    if (fp == NULL)
        return (0);

    /*
     * First find "KickSmash" text, then capture the "USB Serial Number"
     * property, then find the line with "IOCalloutDevice" to locate the
     * path to the serial interface.
     */
    while ((fgets(buf, sizeof (buf), fp) != NULL) && (nports < max)) {
        if (saw_programmer) {
            char *ptr;
            char *eptr;
            if ((strstr(buf, "\"USB Serial Number\"") != NULL) &&
                ((ptr = strchr(buf, '=')) != NULL) &&
                ((ptr = strchr(ptr, '"')) != NULL) &&
                ((eptr = strchr(ptr + 1, '"')) != NULL)) {
                *eptr = '\0';
                snprintf(serial, sizeof (serial), "%s", ptr + 1);
            }
            if (strstr(buf, "IOCalloutDevice") != NULL) {
                ptr = strchr(buf, '=');
                if (ptr != NULL) {
                    ptr += 3;
                    eptr = strchr(ptr, '"');
                    if (eptr != NULL)
                        *eptr = '\0';
                    memset(&ports[nports], 0, sizeof (ports[nports]));
                    snprintf(ports[nports].path, sizeof (ports[nports].path),
                             "%s", ptr);
                    snprintf(ports[nports].serial,
                             sizeof (ports[nports].serial), "%s", serial);
                    nports++;
                    saw_programmer = FALSE;  // Look for another
                    serial[0] = '\0';
                    continue;
                }
                printf("%.80s\n", buf);
            }
        }
        if ((strstr(buf, "MX29F1615") != NULL) ||
            (strstr(buf, "KickSmash") != NULL)) {
            saw_programmer = TRUE;
            serial[0] = '\0';
        }
    }

    pclose(fp);
#endif
#ifdef __MINGW32__
    /*
     * Walk the registry under SYSTEM\CurrentControlSet\Enum looking for
     * USB (or USB composite) devices with VID 1209 / PID 1610.  For each
     * match that has a "Device Parameters\PortName" value, collect the
     * COM port name only if it is currently present (listed under
     * HARDWARE\DEVICEMAP\SERIALCOMM).  Stale entries for previously
     * attached boards are therefore ignored.
     *
     * The instance ID of the USB device (Enum\USB\VID_1209&PID_1610) is
     * its serial number. A composite device's interface instances are
     * instead named with the device's ParentIdPrefix value.
     */
    {
        static const char *enum_roots[] = {
            "SYSTEM\\CurrentControlSet\\Enum\\USB",
            "SYSTEM\\CurrentControlSet\\Enum\\USBCCGP",
            NULL
        };
        struct {
            char serial[64];
            char prefix[64];
        } parents[KS_PORTS_MAX];
        uint nparents = 0;
        char vidpid[32];
        char path[512];
        uint r;
        HKEY hSerialMap = NULL;
        HKEY hDev;

        snprintf(vidpid, sizeof (vidpid), "VID_%04X&PID_%04X",
                 MX_VENDOR, MX_DEVICE);

        /* Serial numbers and their ParentIdPrefix values */
        snprintf(path, sizeof (path), "%s\\%s", enum_roots[0], vidpid);
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_READ,
                          &hDev) == ERROR_SUCCESS) {
            DWORD index = 0;
            while (nparents < ARRAY_SIZE(parents)) {
                char  inst[64];
                DWORD inst_len = sizeof (inst);
                DWORD ptype = REG_SZ;
                DWORD plen = sizeof (parents[0].prefix);
                HKEY  hInst;

                if (RegEnumKeyExA(hDev, index++, inst, &inst_len,
                                  NULL, NULL, NULL, NULL) != ERROR_SUCCESS) {
                    break;
                }
                snprintf(parents[nparents].serial,
                         sizeof (parents[nparents].serial), "%s", inst);
                parents[nparents].prefix[0] = '\0';
                if (RegOpenKeyExA(hDev, inst, 0, KEY_READ,
                                  &hInst) == ERROR_SUCCESS) {
                    if ((RegQueryValueExA(hInst, "ParentIdPrefix", NULL,
                                          &ptype,
                                          (LPBYTE) parents[nparents].prefix,
                                          &plen) != ERROR_SUCCESS) ||
                        (ptype != REG_SZ)) {
                        parents[nparents].prefix[0] = '\0';
                    }
                    RegCloseKey(hInst);
                }
                nparents++;
            }
            RegCloseKey(hDev);
        }

        /* Active COM ports only (connected right now) */
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                          "HARDWARE\\DEVICEMAP\\SERIALCOMM",
                          0, KEY_READ, &hSerialMap) != ERROR_SUCCESS) {
            hSerialMap = NULL;
        }

        for (r = 0; enum_roots[r] != NULL && nports < max; r++) {
            HKEY hEnum;
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, enum_roots[r], 0,
                              KEY_READ, &hEnum) != ERROR_SUCCESS) {
                continue;
            }
            char subkey[256];
            DWORD subkey_len;
            DWORD index = 0;
            while (nports < max) {
                subkey_len = sizeof (subkey);
                if (RegEnumKeyExA(hEnum, index++, subkey, &subkey_len,
                                  NULL, NULL, NULL, NULL) != ERROR_SUCCESS) {
                    break;
                }
                /*
                 * Match VID_1209&PID_1610 (case-insensitive; may have
                 * trailing &MI_xx or similar for composite interfaces)
                 */
                if (strcasestr(subkey, vidpid) == NULL)
                    continue;

                HKEY hVidPid;
                if (RegOpenKeyExA(hEnum, subkey, 0,
                                  KEY_READ, &hVidPid) != ERROR_SUCCESS) {
                    continue;
                }

                /* Enumerate instance IDs under the VID/PID key */
                char inst[256];
                DWORD inst_len;
                DWORD iindex = 0;
                while (nports < max) {
                    inst_len = sizeof (inst);
                    if (RegEnumKeyExA(hVidPid, iindex++, inst, &inst_len,
                                      NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                        break;

                    HKEY hInst;
                    snprintf(path, sizeof (path), "%s\\Device Parameters",
                             inst);
                    if (RegOpenKeyExA(hVidPid, path, 0,
                                      KEY_READ, &hInst) != ERROR_SUCCESS) {
                        continue;
                    }

                    char portname[32];
                    DWORD ptype = REG_SZ;
                    DWORD plen = sizeof (portname);
                    if ((RegQueryValueExA(hInst, "PortName", NULL, &ptype,
                                         (LPBYTE)portname, &plen) ==
                         ERROR_SUCCESS) &&
                        (ptype == REG_SZ) &&
                        (portname[0] != '\0')) {
                        /*
                         * Only accept the port if it is currently mapped
                         * (device is plugged in).  SERIALCOMM values are
                         * the COM names; value names are the device paths.
                         */
                        int present = 0;
                        if (hSerialMap != NULL) {
                            char vname[256];
                            char vdata[64];
                            DWORD vname_len;
                            DWORD vdata_len;
                            DWORD vtype;
                            DWORD vindex = 0;
                            while (1) {
                                vname_len = sizeof (vname);
                                vdata_len = sizeof (vdata);
                                if (RegEnumValueA(hSerialMap, vindex++,
                                                  vname, &vname_len,
                                                  NULL, &vtype,
                                                  (LPBYTE)vdata, &vdata_len) !=
                                    ERROR_SUCCESS) {
                                    break;
                                }
                                if (vtype == REG_SZ &&
                                    strcasecmp(vdata, portname) == 0) {
                                    present = 1;
                                    break;
                                }
                            }
                        }
                        if (present) {
                            /*
                             * Deduplicate (same port can appear under USB
                             * and USBCCGP)
                             */
                            uint already = 0;
                            uint p;
                            for (p = 0; p < nports; p++) {
                                if (strcasecmp(ports[p].path, portname) == 0) {
                                    already = 1;
                                    break;
                                }
                            }
                            if (!already) {
                                memset(&ports[nports], 0,
                                       sizeof (ports[nports]));
                                snprintf(ports[nports].path,
                                         sizeof (ports[nports].path),
                                         "%s", portname);
                                for (p = 0; p < nparents; p++) {
                                    size_t plen2 = strlen(parents[p].prefix);
                                    if ((strcasecmp(inst,
                                                    parents[p].serial) == 0) ||
                                        ((plen2 != 0) &&
                                         (strncasecmp(inst, parents[p].prefix,
                                                      plen2) == 0))) {
                                        snprintf(ports[nports].serial,
                                                 sizeof (ports[nports].serial),
                                                 "%s", parents[p].serial);
                                        break;
                                    }
                                }
                                nports++;
                            }
                        }
                    }
                    RegCloseKey(hInst);
                }
                RegCloseKey(hVidPid);
            }
            RegCloseKey(hEnum);
        }
        if (hSerialMap != NULL)
            RegCloseKey(hSerialMap);
    }
#endif
    (void) max;
    return (nports);
}

/*
 * ks_port_find() locates an attached Kicksmash serial port by serial
 *                number or USB port path. If neither is specified, a
 *                port which is not in the exclude list is located.
 *
 * @param [in]  serial   - USB serial number, or NULL / empty.
 * @param [in]  usbpath  - USB port path, or NULL / empty.
 * @param [in]  exclude  - Ports to ignore (when serial and usbpath unknown).
 * @param [in]  nexclude - Number of ports in exclude list.
 * @param [out] port     - The located port.
 *
 * @return      TRUE  - A port was found.
 * @return      FALSE - No matching port was found.
 */
static bool
ks_port_find(const char *serial, const char *usbpath,
             const ks_port_t *exclude, uint nexclude, ks_port_t *port)
{
    ks_port_t ports[KS_PORTS_MAX];
    uint      nports = ks_port_list(ports, ARRAY_SIZE(ports));
    uint      cur;
    uint      ex;

    for (cur = 0; cur < nports; cur++) {
        if ((serial != NULL) && (serial[0] != '\0')) {
            if (strcmp(ports[cur].serial, serial) != 0)
                continue;
        } else if ((usbpath != NULL) && (usbpath[0] != '\0') &&
                   (ports[cur].usbpath[0] != '\0')) {
            if (strcmp(ports[cur].usbpath, usbpath) != 0)
                continue;
        } else {
            for (ex = 0; ex < nexclude; ex++)
                if (strcmp(ports[cur].path, exclude[ex].path) == 0)
                    break;
            if (ex < nexclude)
                continue;
        }
        *port = ports[cur];
        return (TRUE);
    }
    return (FALSE);
}

/*
 * find_mx_programmer() will attempt to locate the tty device associated
 *                      with the USB connection of Kicksmash. If found,
 *                      it will update the device_name[] global with the
 *                      file path to the serial interface. If --serial
 *                      was specified, only a device with that USB serial
 *                      number is selected.
 *
 * @param  [in]  None.
 * @global [out] device_name[] is the located path of the programmer (if found).
 * @return       None.
 */
static void
find_mx_programmer(void)
{
    ks_port_t ports[KS_PORTS_MAX];
    uint      nports = ks_port_list(ports, ARRAY_SIZE(ports));
    uint      cur;
    uint      matches = 0;
    uint      first = 0;

    for (cur = 0; cur < nports; cur++) {
        if ((select_serial[0] != '\0') &&
            (strcmp(ports[cur].serial, select_serial) != 0)) {
            continue;
        }
        if (matches++ == 0)
            first = cur;
    }
    if (matches == 0)
        return;  // main() will error
#ifdef __MINGW32__
    if (matches > 1) {
        printf("Multiple Kicksmash devices found:");
        for (cur = 0; cur < nports; cur++)
            if ((select_serial[0] == '\0') ||
                (strcmp(ports[cur].serial, select_serial) == 0))
                printf(" %s", ports[cur].path);
        printf("\nSpecify one with -d <port> (e.g. -d %s)\n",
               ports[first].path);
        return;  // Leave device_name empty so main() will require -d
    }
#endif
    snprintf(device_name, sizeof (device_name), "%s", ports[first].path);
    printf("Using %s\n", device_name);
}

/*
 * wait_for_tx_writer() waits for the TX Writer thread to flush its transmit
 *                      buffer.
 *
 * @param  [in]  None.
 * @return       None.
 */
static void
wait_for_tx_writer(void)
{
    int count = 0;

    while (tx_rb_consumer != tx_rb_producer)
        if (count++ > 100)
            break;
        else
            time_delay_msec(10);
}

/* Status codes from local message handling */
#define MSG_STATUS_SUCCESS    0           // No error
#define MSG_STATUS_FAILURE    1           // Generic failure
#define MSG_STATUS_NO_REPLY   0xfffffff9  // Did not get reply from Kicksmash
#define MSG_STATUS_BAD_LENGTH 0xfffffff8  // Bad length detected
#define MSG_STATUS_BAD_CRC    0xfffffff7  // CRC failure detected
#define MSG_STATUS_BAD_DATA   0xfffffff6  // Invalid data
#define MSG_STATUS_PRG_TMOUT  0xfffffff5  // Programming timeout
#define MSG_STATUS_PRG_FAIL   0xfffffff4  // Programming failure

static const char *
smash_err(uint code)
{
    switch (code) {
        case KS_STATUS_OK:
            return ("Success");
        case KS_STATUS_FAIL:
            return ("KS Failure");
        case KS_STATUS_CRC:
            return ("KS reports CRC bad");
        case KS_STATUS_UNKCMD:
            return ("KS detected unknown command");
        case KS_STATUS_BADARG:
            return ("KS reports bad command argument");
        case KS_STATUS_BADLEN:
            return ("KS reports bad length");
        case KS_STATUS_NODATA:
            return ("KS reports no data available");
        case KS_STATUS_LOCKED:
            return ("KS reports resource locked");
        case MSG_STATUS_FAILURE:
            return ("Failure");
        case MSG_STATUS_NO_REPLY:
            return ("No Reply");
        case MSG_STATUS_BAD_LENGTH:
            return ("Smash detected bad length");
        case MSG_STATUS_BAD_CRC:
            return ("Smash detected bad CRC");
        case MSG_STATUS_BAD_DATA:
            return ("Invalid data");
        case MSG_STATUS_PRG_TMOUT:
            return ("Program/erase timeout");
        case MSG_STATUS_PRG_FAIL:
            return ("Program/erase failure");
        default:
            return ("Unknown");
    }
}

/*
 * Kicksmash send lock
 * -------------------
 * Every Kicksmash command is a transaction: the command is written to
 * the serial port and the reply is then collected from the receive ring
 * buffer. Only one thread may be in a transaction at a time, otherwise
 * commands and replies from two threads get interleaved. A multi-part
 * send_msg() must also not be interleaved with another thread's message.
 *
 * The lock is recursive for its owner (send_msg() -> send_ks_cmd()).
 * There are three priorities:
 *   KS_SEND_PRIO_MAIN - Main thread replies and commands. While one is
 *                       waiting, no other caller is granted the lock,
 *                       even if it happens to be free at that moment.
 *   KS_SEND_PRIO_POLL - Main thread idle polling for Amiga messages.
 *                       Competes equally with network forwarding, except
 *                       that forwarding gets at most KS_SEND_NET_BURST_MAX
 *                       grants in a row while a poll is waiting.
 *   KS_SEND_PRIO_NET  - Network forwarding thread.
 */
static pthread_mutex_t ks_send_mtx          = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  ks_send_cv           = PTHREAD_COND_INITIALIZER;
static pthread_t       ks_send_owner;
static uint            ks_send_depth        = 0;  // 0 = lock is free
static uint            ks_send_main_waiting = 0;  // Main thread is waiting
static uint            ks_send_poll_waiting = 0;  // Main thread idle poll waiting
static uint            ks_send_net_burst    = 0;  // NET grants while poll waits

/*
 * While an idle poll (KS_SEND_PRIO_POLL) is waiting, the network
 * forwarding thread may take the lock at most this many times in a row
 * before the poll is let in. This keeps forwarding bursts efficient
 * without starving the main thread's checks for Amiga messages.
 */
#define KS_SEND_NET_BURST_MAX 4

void
ks_send_lock(uint prio)
{
    pthread_t self = pthread_self();

    pthread_mutex_lock(&ks_send_mtx);
    if ((ks_send_depth != 0) && pthread_equal(ks_send_owner, self)) {
        /* Already held by this thread */
        ks_send_depth++;
        pthread_mutex_unlock(&ks_send_mtx);
        return;
    }
    if (prio == KS_SEND_PRIO_MAIN) {
        /* Replies and other main thread work: always first */
        ks_send_main_waiting++;
        while (ks_send_depth != 0)
            pthread_cond_wait(&ks_send_cv, &ks_send_mtx);
        ks_send_main_waiting--;
    } else if (prio == KS_SEND_PRIO_POLL) {
        /* Main thread idle poll: no priority over network forwarding */
        ks_send_poll_waiting++;
        while ((ks_send_depth != 0) || (ks_send_main_waiting != 0))
            pthread_cond_wait(&ks_send_cv, &ks_send_mtx);
        ks_send_poll_waiting--;
        ks_send_net_burst = 0;
    } else {
        /* Network forwarding */
        while ((ks_send_depth != 0) || (ks_send_main_waiting != 0) ||
               ((ks_send_poll_waiting != 0) &&
                (ks_send_net_burst >= KS_SEND_NET_BURST_MAX))) {
            pthread_cond_wait(&ks_send_cv, &ks_send_mtx);
        }
        if (ks_send_poll_waiting != 0)
            ks_send_net_burst++;
        else
            ks_send_net_burst = 0;
    }
    ks_send_owner = self;
    ks_send_depth = 1;
    pthread_mutex_unlock(&ks_send_mtx);
}

void
ks_send_unlock(void)
{
    pthread_mutex_lock(&ks_send_mtx);
    if ((ks_send_depth != 0) && (--ks_send_depth == 0))
        pthread_cond_broadcast(&ks_send_cv);
    pthread_mutex_unlock(&ks_send_mtx);
}

static const uint16_t sm_magic[] = { 0x0204, 0x1017, 0x0119, 0x0117 };
static const uint8_t *sm_magic_b = (uint8_t *) sm_magic;

static uint
send_ks_cmd_core(uint cmd, uint len, void *buf)
{
    uint32_t crc;
    uint16_t txlen = len;
    uint16_t txcmd = cmd;
    uint     len_roundup = (len + 3) & ~3;

    crc = crc32r(0, &txlen, 2);
    crc = crc32r(crc, &txcmd, 2);
#ifdef DEBUG_CRC
    if (len > 0) {
        uint32_t crc1a = crc32s(crc, buf, len & ~1);
        uint32_t crc1b = crc32s(crc1a, (uint8_t *)buf + len, 1);
        uint32_t crc1c = crc32s(crc1a, (uint8_t *)buf + len - 1, 1);
        printf("crc1=%08x crc1a=%08x crc1b=%08x crc1c=%08x last=%02x %02x\n",
               crc, crc1a, crc1b, crc1c, ((uint8_t *)buf)[len - 1],
               ((uint8_t *)buf)[len]);
    }
#endif
    crc = crc32s(crc, buf, len);
#ifdef DEBUG_CRC
    if (len > 0) {
        printf("crc2=%08x len=%x buf=", crc, len);
        uint8_t *bufp = (uint8_t *)buf;
        if (buf != NULL) {
        uint cur;
        for (cur = 0; cur < 16; cur++)
            printf("%02x ", bufp[cur]);
        }
        printf("\n");
    }
#endif

    crc = (crc << 16) | (crc >> 16);  // Convert to match Amiga format

#undef DEBUG_SEND_MSG
#ifdef DEBUG_SEND_MSG
    printf("send l=%x cmd=%x crc=%08x\n", len, cmd, crc);
    printf(" %04x %04x %04x %04x ",
           sm_magic[0], sm_magic[1], sm_magic[2], sm_magic[3]);
    printf(" %04x %04x ", txlen, txcmd);
    uint pos;
    for (pos = 0; pos < (len + 1) / 2; pos++) {
        printf(" %04x", ((uint16_t *) buf)[pos]);
    }
    printf("  %04x %04x\n", crc >> 16, (uint16_t)crc);
#endif
    /*
     * Queue the whole message, then wake the writer once, so that the
     * message normally goes to the device in a single write.
     */
    if (send_ll_bin_nokick(&sm_magic, sizeof (sm_magic)) ||
        send_ll_bin_nokick(&txlen, sizeof (txlen)) ||
        send_ll_bin_nokick(&txcmd, sizeof (txcmd)) ||
        ((len > 0) && send_ll_bin_nokick(buf, len_roundup)) ||
        send_ll_bin_nokick(&crc, sizeof (crc))) {
        tx_rb_kick();
        return (MSG_STATUS_FAILURE);
    }
    tx_rb_kick();
    return (MSG_STATUS_SUCCESS);
}

/*
 * cmd=8 l=0000 CRC 2608edb8
 *     0204 1017 0119 0117 0000 0008 2608 edb8
 * cmd=6 l=0000 CRC 1a864db2
 *     0204 1017 0119 0117 0000 0006 1a86 4db2
 */

#define KS_MSG_HEADER_LEN (sizeof (sm_magic) + 2 + 2)

static uint
recv_ks_reply_core(void *buf, uint buflen, uint flags,
                   uint *rxstatus, uint *rxlen)
{
    uint16_t len = 0;
    uint16_t len_roundup = 0;
    uint16_t status = 0;
    uint     pos = 0;
    uint32_t crc = 0;
    uint8_t *bufp = (uint8_t *)buf;
    const uint timeout = 500;
    uint32_t crc_rx = 0;
    int timeout_count = 0;
    uint8_t  localbuf[4096];
    uint     was_kicksmash = 0;

    if (buf == NULL) {
        bufp = localbuf;
        buflen = sizeof (localbuf);
        memset(localbuf, 0, buflen);
    }

    while (1) {
        uint ch = rx_rb_get();
        if ((int)ch == -1) {
            if (ks_message_terminated)
                return (0);
            if (timeout_count++ >= timeout) {
                printf("Receive timeout (%d ms): discarded %u bytes\n",
                       timeout, pos);
#define KS_REPLY_DEBUG
#ifdef KS_REPLY_DEBUG
                uint cur;
                if (flags & BIT(0))
                    printf("raw ");
                if (pos > sizeof (sm_magic) + 2)
                    printf("len=%04x ", len);
                if (pos > sizeof (sm_magic) + 4)
                    printf("status=%04x ", status);
                for (cur = 0; cur < sizeof (sm_magic); cur++)
                    if (cur < pos)
                        printf("%02x ", sm_magic_b[cur]);
                    else
                        break;
                if (pos > sizeof (sm_magic) + 2)
                    printf("%04x ", len);
                if (pos > sizeof (sm_magic) + 4)
                    printf("%04x ", status);
                if (pos > KS_MSG_HEADER_LEN) {
                    for (cur = 0; cur < pos - KS_MSG_HEADER_LEN; cur++) {
                        if (cur >= (buflen - 1)) {
                            printf("...\n");
                            break;
                        }
                        printf(" %02x", bufp[cur ^ 1]);
                    }
                }
                if (pos - KS_MSG_HEADER_LEN < len) {
                    printf(" [data short by %ld bytes]\n",
                           (long) (len - (pos - KS_MSG_HEADER_LEN)));
                } else if (pos - KS_MSG_HEADER_LEN < len + 4) {
                    printf(" [CRC short by %ld bytes]\n",
                           (long) (len + 4 - (pos - KS_MSG_HEADER_LEN)));
                } else {
                    printf("%08x got CRC???", crc_rx);
                }
                printf("\n");
#endif
                return (MSG_STATUS_NO_REPLY);
            }
#ifdef KS_RX_WAKEUP
            rx_rb_wait(1);  // Woken as soon as the reader stores data
#else
            time_delay_msec(1);
            sched_yield();
#endif
            continue;
        }
        if (flags & BIT(0)) {
            /* Capture raw data */
            if (pos < ((buflen + 1) & ~1))
                bufp[pos] = ch;
        } else {
            /* Just packet payload */
            if (pos - KS_MSG_HEADER_LEN < ((buflen + 1) & ~1))
                bufp[pos - KS_MSG_HEADER_LEN] = ch;
        }
        switch (pos) {
            case 0:  // Magic start
#if 0
                if ((ch == 0x3) || (ch == '\n') || (ch == '\r')) {
                    printf("got abort\n");
                    return (MSG_STATUS_FAILURE); // Abort received ^C, LF, or CR
                }
                /* FALLTHROUGH */
#endif
            case 1:  // Magic
            case 2:  // Magic
            case 3:  // Magic
            case 4:  // Magic
            case 5:  // Magic
            case 6:  // Magic
            case 7:  // Magic
                if (ch != sm_magic_b[pos]) {
                    pos = 0;
                    if (was_kicksmash == 0) {
                        was_kicksmash = 1;
                        printf("KS: ");
                    }
                    if (!is_printable_ascii(ch))
                        printf("[%02x %c]", ch, printable_ascii(ch));
                    else
                        putchar(ch);
                    if (ch == " CMD>"[was_kicksmash]) {
                        was_kicksmash++;
                        if (was_kicksmash == 5) {
                            printf("Kicksmash message interface terminated\n");
                            ks_message_terminated = 1;  // Stop execution
                            running = 0;
                            break;
                        }
                    }
                } else {
                    was_kicksmash = 0;
                    pos++;
                }
                break;
            case 8:  // Length phase 1
                len = ch;
                pos++;
                break;
            case 9:  // Length phase 2
                len |= (ch << 8);
                len_roundup = (len + 3) & ~3; // Round up for odd length
                pos++;
                break;
            case 10:  // Command phase 1
                status = ch;
                pos++;
                break;
            case 11:  // Command phase 2
                status |= (ch << 8);
                pos++;
                break;
            default:  // Data and CRC phase
                if (pos >= len_roundup + KS_MSG_HEADER_LEN) {
                    /* CRC */
                    uint crcpos = (pos - len_roundup - KS_MSG_HEADER_LEN) ^ 2;
                    crc_rx |= (ch << (8 * crcpos));
                }
                if (pos == len_roundup + KS_MSG_HEADER_LEN + 3) {
                    /* Last byte of CRC */

                    if (flags & BIT(0)) {
                        /* Raw data receive */
                        if (pos >= buflen) {
                            printf("message len 0x%x > raw buflen 0x%x for "
                                   "cmd=%x\n",
                                   pos, buflen, status);
                            return (MSG_STATUS_BAD_LENGTH);  // too large
                        }
                        crc = crc32s(0, bufp + sizeof (sm_magic), len + 4);
                        status = 0;
                    } else {
                        /* Regular data receive */
                        if (len > buflen) {
                            printf("message len 0x%x > buflen 0x%x for "
                                   "cmd=%x\n",
                                   pos, buflen, status);
                            return (MSG_STATUS_BAD_LENGTH);  // too large
                        }
                        crc = crc32s(0, &len, 2);       // length
                        crc = crc32s(crc, &status, 2);  // command
                        crc = crc32s(crc, bufp, len);   // data
                    }
                    if (crc != crc_rx) {
                        printf("Rx CRC %08x != expected %08x\n", crc, crc_rx);

                        uint pos;
                        printf(" status=%04x len=%04x\n", status, len);
                        for (pos = 0; pos < ARRAY_SIZE(sm_magic); pos++)
                            printf(" %04x", sm_magic[pos]);
                        printf(" %04x %04x", len, status);
                        for (pos = 0; pos < len; pos += 2) {
                            printf(" %04x", SWAP16(*(uint16_t *)(bufp + pos)));
                        }
                        printf(" %04x %04x\n", crc_rx >> 16, crc_rx & 0xffff);

                        return (MSG_STATUS_BAD_CRC);
                    }
                    if (rxlen != NULL)
                        *rxlen = len;
                    if (rxstatus != NULL)
                        *rxstatus = status;
                    return (MSG_STATUS_SUCCESS);
                } else {
                    pos++;
                }
                break;
        }
    }
}

static uint
send_ks_cmd_prio(uint prio, uint cmd, void *txbuf, uint txlen, void *rxbuf,
                 uint rxmax, uint *rxstatus, uint *rxlen, uint flags)
{
    uint rc;

    /*
     * The command and its reply form one transaction on the serial link.
     * A thread which already holds the lock (send_msg() or the network
     * forwarding thread) just nests here.
     */
    ks_send_lock(prio);
    rc = send_ks_cmd_core(cmd, txlen, txbuf);
    if (rc == 0)
        rc = recv_ks_reply_core(rxbuf, rxmax, flags, rxstatus, rxlen);
    ks_send_unlock();
    return (rc);
}

__attribute__((noinline))
static uint
send_ks_cmd(uint cmd, void *txbuf, uint txlen, void *rxbuf, uint rxmax,
            uint *rxstatus, uint *rxlen, uint flags)
{
    return (send_ks_cmd_prio(KS_SEND_PRIO_MAIN, cmd, txbuf, txlen, rxbuf,
                             rxmax, rxstatus, rxlen, flags));
}

static const char * const board_states[] = {
    "Alert", "Standalone", "Off", "In_Reset",
    "No_KBRST", "No_A17", "No_A18", "No_A19",
    "No_Flash0", "No_Flash1", "Bad_Flash",
};

static void
show_ks_version(void)
{
    char cmd_output[100];
    char *ptr = cmd_output;
    int  rxcount;
    if (send_cmd("version"))
        return; // "timeout" was reported in this case
    if (recv_output(cmd_output, sizeof (cmd_output), &rxcount, 80))
        return; // "timeout" was reported in this case
    if (rxcount == 0) {
        printf("Receive timeout\n");
        return;
    }
    if (rxcount >= sizeof (cmd_output))
        rxcount = sizeof (cmd_output) - 1;
    cmd_output[rxcount] = '\0';
    if (strncmp(ptr, "Version", 7) == 0)
        ptr += 8;
    printf("KickSmash %s", ptr);
}

static void
show_ks_inquiry(void)
{
    smash_id_t id;
    uint status;
    uint rc;
    uint16_t rev;
    rc = send_ks_cmd(KS_CMD_ID, NULL, 0, &id, sizeof (id), &status, NULL, 0);
    if (rc != 0) {
        printf("HS send message failed: %d (%s)\n", rc, smash_err(rc));
        return;
    }
    if (status != 0) {
        printf("HS message failure: %d (%s)\n", status, smash_err(status));
        return;
    }

    printf("  Kicksmash %u.%u built %02u%02u-%02u-%02u %02u:%02u:%02u\n",
           SWAP16(id.si_ks_version[0]), SWAP16(id.si_ks_version[1]),
           id.si_ks_date[0], id.si_ks_date[1],
           id.si_ks_date[2], id.si_ks_date[3],
           id.si_ks_time[0], id.si_ks_time[1], id.si_ks_time[2]);
    printf("  USB %08x  Size %u  Serial \"%s\"  Name \"%s\"\n",
           SWAP32(id.si_usbid), SWAP32(id.si_size), id.si_serial, id.si_name);
    printf("  Mode: %s\n",
           (id.si_mode == 0) ? "32-bit" :
           (id.si_mode == 1) ? "16-bit" :
           (id.si_mode == 2) ? "16-bit high" : "unknown");

    rev = SWAP16(id.si_rev);
    if (rev > 1) {
        uint32_t state = SWAP32(id.si_state);
        uint bit;
        printf("  CPU %s %u MHz, periph %u MHz\n",
               (id.si_cpu == 0) ? "STM32F107" :
               (id.si_cpu == 1) ? "GD2F107" : "Unknown",
               SWAP16(id.si_cpufreq), SWAP16(id.si_busfreq));
        for (bit = 0; bit < 32; bit++) {
            if (state & BIT(bit)) {
                if (bit < ARRAY_SIZE(board_states))
                    printf("  %s", board_states[bit]);
                else
                    printf("  bit%u", bit);
            }
        }
        if (state != 0)
            printf("\n");
    }
}

#if 0
static uint64_t
smash_time(void)
{
    uint64_t usecs;
    if (send_cmd(KS_CMD_UPTIME, NULL, 0, &usecs, sizeof (usecs), NULL))
        return (0);
    return (usecs);
}
#endif

/*
 * mem16_swap
 * -------------
 * Swap odd and even bytes of a buffer.
 */
static void
mem16_swap(void *buf, uint len)
{
    uint16_t *bufp = (uint16_t *)buf;
    len = (len + 1) / 2;
    while (len-- != 0) {
        *bufp = (*bufp << 8) | (*bufp >> 8);
        bufp++;
    }
}

#if defined(__MINGW32__)
#include <windows.h>
typedef unsigned int uint;

#elif defined(OSX) || defined(LINUX)
#include <pthread.h>
#include <sys/types.h>       /* uint */

#else
#error "Define one of LINUX, OSX, or __MINGW32__ to select a lock backend"
#endif

/*
 * Platform lock backend
 */
#if defined(__MINGW32__)
/*
 * SRWLOCK has a static initializer, just like PTHREAD_MUTEX_INITIALIZER,
 * so no explicit init/teardown call is required.
 */
static SRWLOCK queue_lock = SRWLOCK_INIT;
#define QUEUE_LOCK()    AcquireSRWLockExclusive(&queue_lock)
#define QUEUE_UNLOCK()  ReleaseSRWLockExclusive(&queue_lock)
#else /* OSX || LINUX */
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
#define QUEUE_LOCK()    pthread_mutex_lock(&queue_lock)
#define QUEUE_UNLOCK()  pthread_mutex_unlock(&queue_lock)
#endif

/*
 * Queue node / FIFO
 */
#define TAG_QUEUE_ANY  ((uint)-1)  // Give any queued message
#define TAG_QUEUE_NONE ((uint)-2)  // Just queue a received message

typedef struct msg_tag_queue msg_tag_queue_t;
struct msg_tag_queue {
    msg_tag_queue_t *mrg_next;    // Next node in FIFO order
    void            *mrg_buf;     // Buffer (might be exchanged)
    uint16_t         mrg_len;     // Length of message (NOT buffer size)
    uint16_t         mrg_tag;     // Desired reply tag
    uint8_t          mrg_status;  // Message receive status
};

/* Head/tail of the FIFO - global, protected by queue_lock */
static msg_tag_queue_t *mtq_head = NULL;
static msg_tag_queue_t *mtq_tail = NULL;

/*
 * tag_queue_add() - Appends a message to the tail of the FIFO.
 *                   This function is thread-safe.
 */
static void
tag_queue_add(uint16_t tag, void *buf, uint len, uint rx_status)
{
    msg_tag_queue_t *node = malloc(sizeof (*node));
    if (node == NULL) {
        /* No memory: discard message to avoid leaking the buffer */
        fprintf(stderr, "tag_queue_add: out of memory\n");
        free(buf);
        return;
    }
    node->mrg_buf    = buf;
    node->mrg_len    = (uint16_t)len;
    node->mrg_tag    = tag;
    node->mrg_status = rx_status;
    node->mrg_next   = NULL;

    QUEUE_LOCK();
    if (mtq_tail == NULL) {
        mtq_head = node;
        mtq_tail = node;
    } else {
        mtq_tail->mrg_next = node;
        mtq_tail = node;
    }
    QUEUE_UNLOCK();
}

/*
 * tag_queue_remove() - Locates the first node in FIFO order whose
 *                      tag matches. TAG_QUEUE_ANY matches any tag.
 *                      A pointer to the found node is returned.
 *                      NULL indicates no node with the specified tag
 *                      was located. This function is thread-safe.
 */
static void *
tag_queue_remove(uint target_tag, uint *len, uint *rx_status)
{
    msg_tag_queue_t *node, *prev = NULL;
    void *found_buf = NULL;

    if (target_tag == TAG_QUEUE_NONE)
        return (NULL);

    QUEUE_LOCK();
    for (node = mtq_head; node != NULL; prev = node, node = node->mrg_next) {
        if ((target_tag == TAG_QUEUE_ANY) || (node->mrg_tag == target_tag)) {
            /* Unlink node from the FIFO */
            if (prev == NULL)
                mtq_head = node->mrg_next;
            else
                prev->mrg_next = node->mrg_next;
            if (node == mtq_tail)
                mtq_tail = prev;
            break;
        }
    }
    QUEUE_UNLOCK();

    if (node != NULL) {
        found_buf   = node->mrg_buf;
        *len        = node->mrg_len;
        *rx_status  = node->mrg_status;
        free(node);
    }
    return (found_buf);
}


static uint
wait_for_send_space(uint sendlen, uint pos, uint len)
{
    uint rc;
    uint status;
    uint16_t utoa_avail;
    uint16_t atou_inuse;
    smash_msg_info_t mi;

    /*
     * Poll for the required space and then try again.
     */
    uint timeout;
    for (timeout = 2000; timeout > 0; timeout--) {
        /* Wait for space */
        rc = send_ks_cmd(KS_CMD_MSG_INFO, NULL, 0, &mi, sizeof (mi),
                         &status, NULL, 0);
        utoa_avail = SWAP16(mi.smi_utoa_avail);
        if ((rc != 0) || (utoa_avail >= sendlen))
            return (rc);
        atou_inuse = SWAP16(mi.smi_atou_inuse);
        if (atou_inuse > 0) {
            /*
             * Message(s) pending from Amiga. Read and queue those to prevent
             * message deadlock where Amiga code is waiting to send more.
             */
            uint8_t rxdata[4096];
            uint rxlen;
            (void) recv_msg(TAG_QUEUE_NONE, rxdata, sizeof (rxdata), &status,
                            &rxlen);
            continue;
        }
        time_delay_msec(1);
    }

    if (timeout == 0) {
        printf("Send timeout waiting for len=%x buffer at "
               "%x of %x\n",
               sendlen, pos - SEND_MSG_MAX, len - SEND_MSG_MAX);
        return (RC_TIMEOUT);
    }
    return (KM_STATUS_OK);
}

/*
 * send_msg_wait
 * -------------
 * Sends a message to the remote Amiga, waiting for space if necessary.
 */
uint
send_msg_wait(void *buf, uint len, uint *status, uint pos)
{
    uint rc;
    rc = send_ks_cmd(KS_CMD_MSG_SEND, buf, len, NULL, 0, status, NULL, 0);
    if (*status == KS_STATUS_BADLEN) {
        /*
         * The check for space significantly impacts file send time,
         * reducing performance about 30%. It is not really needed
         * in the general case, so it's only done on KS_STATUS_BADLEN
         * like when two requests occur around the same time).
         */
        rc = wait_for_send_space(len, pos, len);
        if (rc == KM_STATUS_OK) {
            /* Try again now that there is enough space */
            rc = send_ks_cmd(KS_CMD_MSG_SEND, buf, len, NULL, 0,
                             status, NULL, 0);
            if ((rc != 0) || (*status != 0))  {
                printf("Got fail %x %x\n", rc, *status);
            }
        }
    }
    return (rc);
}

/*
 * send_msg
 * --------
 * Sends a message to the remote Amiga
 */
uint
send_msg(void *buf, uint len, uint *status)
{
    uint rc;
    uint8_t msgbuf[SEND_MSG_MAX];
    uint sendlen = len;
    uint bodylen;
    uint pos;
    uint bodylen_rounded;

#undef MSG_DEBUG
#ifdef MSG_DEBUG
    km_msg_hdr_t *km = (km_msg_hdr_t *) buf;
    msgprintf("  send msg len=%04x op=%02x mstatus=%02x tag=%02x\n",
              len, km->km_op, km->km_status, SWAP16(km->km_tag));
#endif
    /*
     * Hold the Kicksmash send lock for the whole message (including all
     * parts of a multi-part message and any wait for buffer space), so
     * that the network forwarding thread can't slip a packet in between.
     */
    ks_send_lock(KS_SEND_PRIO_MAIN);
    mem16_swap(buf, len);  // Raw messages passed 16 bits at a time to Amiga
    if (sendlen > SEND_MSG_MAX)
        sendlen = SEND_MSG_MAX;
    rc = send_msg_wait(buf, sendlen, status, 0);

    if ((sendlen == len) || (rc != 0) || (*status != 0))
        goto finish;  // Entire message has been sent or error

    /*
     * Multiple send mode required for the large message.
     */

    uint8_t curpkt = 0;
    uint8_t *op = &msgbuf[1];  // km_op
    pos = sendlen;
    if (pos < len) {
        /*
         * Remaining payload will be sent as additional messages,
         * waiting for sufficient space available before each one.
         * Need to repeat a minimal header for each additional packet.
         */
        memcpy(msgbuf, buf, sizeof (km_msg_hdr_t));
        bodylen = pos - sizeof (km_msg_hdr_t);
    }
    while (pos < len) {
        *op = ++curpkt;  // Sequence number, starting at 1
        if (bodylen > len - pos)
            bodylen = len - pos;
        bodylen_rounded = (bodylen + 1) & ~1;
        memcpy(msgbuf + sizeof (km_msg_hdr_t),
               (uint8_t *)buf + pos, bodylen_rounded);
        sendlen = bodylen + sizeof (km_msg_hdr_t);

#undef DEBUG_SEND_MSG
#ifdef DEBUG_SEND_MSG
        printf("  send %x seq=%x (body=%x) pos=%x of %x\n",
               sendlen, *op, bodylen, pos, len);
#endif
        rc = send_msg_wait(msgbuf, sendlen, status, pos);
        if ((rc != 0) || (*status != 0)) {
            printf("send msg failed at %x of %x\n", pos, len);
            break;
        }
        pos += bodylen;
    }

finish:
    mem16_swap(buf, len);
    ks_send_unlock();
    return (rc);
}

/*
 * send_msg_nowait
 * ---------------
 * Makes a single attempt to send a message to the remote Amiga. Unlike
 * send_msg(), this function will not wait for buffer space and does not
 * support multi-part messages (len must not exceed SEND_MSG_MAX).
 * If Kicksmash does not currently have room for the message, *status is
 * set to KS_STATUS_BADLEN, and the caller may retry later.
 *
 * The caller is expected to hold the Kicksmash send lock.
 */
uint
send_msg_nowait(void *buf, uint len, uint *status)
{
    uint rc;

    if (len > SEND_MSG_MAX)
        return (MSG_STATUS_BAD_LENGTH);

#ifdef MSG_DEBUG
    km_msg_hdr_t *km = (km_msg_hdr_t *) buf;
    msgprintf("  send msg len=%04x op=%02x mstatus=%02x tag=%02x (async)\n",
              len, km->km_op, km->km_status, SWAP16(km->km_tag));
#endif
    mem16_swap(buf, len);  // Raw messages passed 16 bits at a time to Amiga
    rc = send_ks_cmd(KS_CMD_MSG_SEND, buf, len, NULL, 0, status, NULL, 0);
    mem16_swap(buf, len);
    return (rc);
}

/*
 * recv_msg
 * --------
 * Receives a message from the remote Amiga
 */
uint
recv_msg(uint tag, void *buf, uint bufsize, uint *rx_status, uint *rxlen)
{
    uint     rc;
    uint16_t msg_tag;
    uint8_t *localbuf;
    uint     timeout;

retry:
    /* Check for buffered message first */
    localbuf = tag_queue_remove(tag, rxlen, rx_status);
    if (localbuf != NULL) {
#undef DEBUG_MSG_FIFO
#ifdef DEBUG_MSG_FIFO
        km_msg_hdr_t *hdr = (km_msg_hdr_t *) localbuf;
        printf("GOT queue %slen=%x tag=%x op=%x status=%x\n",
               (tag == TAG_QUEUE_ANY) ? "any " : "",
               *rxlen, SWAP16(hdr->km_tag), hdr->km_op, hdr->km_status);
#endif

#undef DEBUG_MSG_FIFO_DUMP
#ifdef DEBUG_MSG_FIFO_DUMP
        uint pos;
        printf("   data");
        for (pos = 0; pos < 16; pos++)
            printf(" %02x", localbuf[pos]);
        printf("\n");
#endif
        memcpy(buf, localbuf, *rxlen);
        free(localbuf);
        return (0);
    }
    for (timeout = 0; timeout < 50; timeout++) {
        /*
         * An idle check for any message (from the main loop) does not
         * get priority over network forwarding; waiting for a specific
         * reply does.
         */
        uint prio = (tag == TAG_QUEUE_ANY) ? KS_SEND_PRIO_POLL :
                                             KS_SEND_PRIO_MAIN;
        rc = send_ks_cmd_prio(prio, KS_CMD_MSG_RECEIVE, NULL, 0, buf, bufsize,
                              rx_status, rxlen, 0);
        if ((rc != 0) || (*rxlen == 0))
            return (rc);
        if (*rx_status == KS_STATUS_NODATA) {
            if ((tag == TAG_QUEUE_ANY) || (tag == TAG_QUEUE_NONE))
                return (rc);  // No message pending
            continue;  // Poll again
        }
        mem16_swap(buf, *rxlen);
        msg_tag = ((km_msg_hdr_t *)buf)->km_tag;

        if ((tag == TAG_QUEUE_ANY) || (tag == msg_tag)) {
            /* Got desired message */
#ifdef DEBUG_MSG_FIFO
            km_msg_hdr_t *hdr = buf;
            printf("Recv tag=%x len=%x op=%x status=%x\n",
                   SWAP16(msg_tag), *rxlen, hdr->km_op, hdr->km_status);
#endif
            return (0);
        }

        localbuf = malloc(*rxlen);
        if (localbuf == NULL) {
            perror("malloc");
            return (RC_FAILURE);
        }
        memcpy(localbuf, buf, *rxlen);
#ifdef DEBUG_MSG_FIFO
        printf("SAVE Recv tag %x len=%x stat=%x\n",
               SWAP16(msg_tag), *rxlen, *rx_status);
#endif

#ifdef DEBUG_MSG_FIFO_DUMP
        uint pos;
        uint plen = 16;
        if (plen > *rxlen)
            plen = *rxlen;
        printf("   data");
        for (pos = 0; pos < plen; pos++)
            printf(" %02x", localbuf[pos]);
        printf("\n");
#endif
        if (*rxlen < 0x2000) {
            tag_queue_add(msg_tag, localbuf, *rxlen, *rx_status);
        } else {
            return (RC_FAILURE);
        }
goto retry;
        if ((tag == TAG_QUEUE_ANY) || (tag == TAG_QUEUE_NONE))
            return (rc);
    }
    if (tag == TAG_QUEUE_ANY)
        printf("Timeout waiting for any tag\n");
    else
        printf("Timeout waiting for tag %x\n", SWAP16(tag));
    return (RC_TIMEOUT);
}

static uint16_t app_state_send[2];

static uint
keep_app_state(void)
{
    uint status;
    uint rc;
    rc = send_ks_cmd(KS_CMD_MSG_STATE | KS_MSG_STATE_SET, app_state_send,
                     sizeof (app_state_send), NULL, 0, &status, NULL, 0);
    if (rc != 0) {
        printf("HS send message failed: %d (%s)\n", rc, smash_err(rc));
        return (rc);
    }
    return (MSG_STATUS_SUCCESS);
}

/*
 * get_localtime
 * -------------
 * Applies local time offset to the specified raw time.
 */
time_t
get_localtime(time_t rawtime)
{
#ifdef __MINGW32__
    const int local_time = localtime(&rawtime)->tm_hour;
    const int gm_time    = gmtime(&rawtime)->tm_hour;
    const int gmtoff     = (local_time - gm_time) * 60 * 60;
    return (rawtime + gmtoff);
#else
    struct tm *ptm = localtime(&rawtime);
    return (rawtime + ptm->tm_gmtoff);
#endif
}

/*
 * get_utctime
 * -----------
 * Undoes local time offset to the specified raw time.
 */
time_t
get_utctime(time_t rawtime)
{
#ifdef __MINGW32__
    const int local_time = localtime(&rawtime)->tm_hour;
    const int gm_time    = gmtime(&rawtime)->tm_hour;
    const int gmtoff     = (local_time - gm_time) * 60 * 60;
    return (rawtime - gmtoff);
#else
    struct tm *ptm = localtime(&rawtime);
    return (rawtime - ptm->tm_gmtoff);
#endif
}

static uint
sm_unknown(km_msg_hdr_t *km, uint *status)
{
    printf("HS unexpected op %x\n", km->km_op);
    km->km_status = KM_STATUS_UNKCMD;
    km->km_op |= KM_OP_REPLY;
    return (send_msg(km, sizeof (*km), status));
}

static uint
sm_null(km_msg_hdr_t *km, uint *status)
{
    km->km_status = KM_STATUS_OK;
    km->km_op |= KM_OP_REPLY;
    return (send_msg(km, sizeof (*km), status));
}

static uint
sm_loopback(km_msg_hdr_t *km, uint8_t *rxdata, uint rxlen, uint *status)
{
    /* Need to send back reply */
    km->km_status = KM_STATUS_OK;
    km->km_op |= KM_OP_REPLY;
#ifdef DEBUG_CMD_LOOPBACK
    printf("lb l=%x s=%04x data=%02x %02x\n",
             rxlen, *status, rxdata[0], rxdata[1]);
    int rc = send_msg(rxdata, rxlen, status);
    if (rc != 0)
        printf("[%d]\n", rc);
    return (rc);
#else
    return (send_msg(rxdata, rxlen, status));
#endif
}

static uint
sm_id(km_msg_hdr_t *km, uint *status)
{
    smash_id_t *reply = (smash_id_t *) (km + 1);
    uint temp[3];
    int  pos = 0;

    km->km_status = KM_STATUS_OK;
    km->km_op |= KM_OP_REPLY;
    memset(reply, 0, sizeof (*reply));
    sscanf(version_str + 8, "%u.%u%n", &temp[0], &temp[1], &pos);
    reply->si_ks_version[0] = SWAP16(temp[0]);
    reply->si_ks_version[1] = SWAP16(temp[1]);
    if (pos == 0)
        pos = 18;
    else
        pos += 8 + 7;
    sscanf(version_str + pos, "%04u-%02u-%02u",
           &temp[0], &temp[1], &temp[2]);
    reply->si_ks_date[0] = temp[0] / 100;
    reply->si_ks_date[1] = temp[0] % 100;
    reply->si_ks_date[2] = temp[1];
    reply->si_ks_date[3] = temp[2];
    pos += 11;
    sscanf(version_str + pos, "%02u:%02u:%02u",
           &temp[0], &temp[1], &temp[2]);
    reply->si_ks_time[0] = temp[0];
    reply->si_ks_time[1] = temp[1];
    reply->si_ks_time[2] = temp[2];
    reply->si_ks_time[3] = 0;
    strcpy(reply->si_serial, "-");           // MAC address here?
    reply->si_rev      = SWAP16(0x0001);     // Protocol version 0.1
    reply->si_features = SWAP16(0x0001);     // Features
    reply->si_usbid    = SWAP32(0x12091610); // IP address here?
    reply->si_mode     = 0xff;
    gethostname(reply->si_name, sizeof (reply->si_name));
    reply->si_name[sizeof (reply->si_name) - 1] = '\0';
    memset(reply->si_unused, 0, sizeof (reply->si_unused));
    return (send_msg(km, sizeof (*km) + sizeof (*reply), status));
}

static uint
sm_version(hm_version_t *hm, uint *status)
{
    amiga_interface_version = hm->hm_version;
    printf("Amiga interface version %x\n", amiga_interface_version);

    hm->hm_hdr.km_op |= KM_OP_REPLY;
    hm->hm_hdr.km_status = KM_STATUS_OK;
    hm->hm_version       = HOST_INTERFACE_VERSION;
    hm->hm_unused[0]     = 0;
    hm->hm_unused[1]     = 0;
    hm->hm_unused[2]     = 0;
    return (send_msg(hm, sizeof (*hm), status));
}

static uint
sm_clock(hm_clock_t *hm, uint *status)
{
    hm->hm_hdr.km_op |= KM_OP_REPLY;
    if (hm->hm_op == 0) {
        struct timeval tv;
        struct timezone tz;
        if (gettimeofday(&tv, &tz)) {
            hm->hm_hdr.km_status = KM_STATUS_FAIL;
        } else {
            uint32_t sec;
            hm->hm_hdr.km_status = KM_STATUS_OK;
            hm->hm_unused[0]     = 0;
            hm->hm_unused[1]     = 0;
            hm->hm_unused[2]     = 0;
            sec = get_localtime(tv.tv_sec - AMIGA_SEC_TO_UNIX_SEC);
            hm->hm_sec = SWAP32(sec);
            hm->hm_usec = SWAP32(tv.tv_usec);
        }
    } else {
        hm->hm_hdr.km_status = KM_STATUS_INVALID;
    }
    return (send_msg(hm, sizeof (*hm), status));
}

static void
process_msg(uint status, uint8_t *rxdata, uint rxlen)
{
    km_msg_hdr_t *km = (km_msg_hdr_t *) rxdata;
    uint pos;
    uint rc;
    uint op;
    uint retry = 1;

#ifdef MSG_DEBUG
    uint rxmax = (rxlen > 32) ? 32 : rxlen;
    msgprintf("  got msg %04x len=%04x op=%02x mstatus=%02x tag=%02x data ",
              status, rxlen, km->km_op, km->km_status, SWAP16(km->km_tag));
    for (pos = sizeof (*km); pos < rxmax; pos++) {
        msgprintf(" %02x", rxdata[pos]);
    }
    msgprintf("\n");
#endif
    if (rxlen < sizeof (*km)) {
        printf("Got invalid message: len=%04x status=%04x", rxlen, status);
        for (pos = sizeof (*km); pos < ((rxlen + 1) & ~1); pos++) {
            printf(" %02x", rxdata[pos]);
        }
        printf("\n");
    }
    if (km->km_op & KM_OP_REPLY) {
        printf("Got op %02x reply, status=%02x\n",
               km->km_op, km->km_status);
        return;
    }

    op = km->km_op;
    do {
        switch (op) {
            case KM_OP_NULL:
                rc = sm_null(km, &status);
                break;
            case KM_OP_LOOPBACK:
                rc = sm_loopback(km, rxdata, rxlen, &status);
                break;
            case KM_OP_ID:
                rc = sm_id(km, &status);
                break;
            case KM_OP_VERSION:
                rc = sm_version((hm_version_t *)rxdata, &status);
                break;
            case KM_OP_CLOCK:
                rc = sm_clock((hm_clock_t *)rxdata, &status);
                break;
            case KM_OP_FOPEN:
                rc = sm_fopen((hm_fopenhandle_t *)rxdata, &status);
                break;
            case KM_OP_FCLOSE:
                rc = sm_fclose((hm_fopenhandle_t *)rxdata, &status);
                break;
            case KM_OP_FREAD:
                rc = sm_fread((hm_freadwrite_t *)rxdata, &status);
                break;
            case KM_OP_FWRITE:
                rc = sm_fwrite((hm_freadwrite_t *)rxdata, rxlen, &status);
                break;
            case KM_OP_FSEEK:
                rc = sm_fseek((hm_fseek_t *)rxdata, &status);
                break;
            case KM_OP_FCREATE:
                rc = sm_fcreate((hm_fopenhandle_t *)rxdata, &status);
                break;
            case KM_OP_FDELETE:
                rc = sm_fdelete((hm_fhandle_t *)rxdata, &status);
                break;
            case KM_OP_FRENAME:
                rc = sm_frename((hm_frename_t *)rxdata, &status);
                break;
            case KM_OP_FPATH:
                rc = sm_fpath((hm_fhandle_t *)rxdata, &status);
                break;
            case KM_OP_FSETDATE:
                rc = sm_fsetdate((hm_fsetdate_t *)rxdata, &status);
                break;
            case KM_OP_FSETOWN:
                rc = sm_fsetown((hm_fsetown_t *)rxdata, &status);
                break;
            case KM_OP_FSETPERMS:
                rc = sm_fsetprotect((hm_fopenhandle_t *)rxdata, &status);
                break;
            case KM_OP_NOPEN:
                rc = sm_nopen((hm_nopenhandle_t *)rxdata, &status);
                break;
            case KM_OP_NCLOSE:
                rc = sm_nclose((hm_nopenhandle_t *)rxdata, &status);
                break;
            case KM_OP_NWRITE:
                rc = sm_nwrite((hm_nreadwrite_t *)rxdata, &status, rxlen);
                break;
            case KM_OP_NREAD:
                rc = sm_nread((hm_nreadwrite_t *)rxdata, &status);
                break;
            case KM_OP_NGETMAC:
                rc = sm_ngetmac((hm_nmac_t *)rxdata, &status);
                break;
            case KM_OP_NSETMAC:
                rc = sm_nsetmac((hm_nmac_t *)rxdata, &status);
                break;
            default:
                rc = sm_unknown(km, &status);
                break;
        }
        if (rc == 0)
            rc = status;

        if (rc == 0)
            break;

        printf("process_msg failure op=%x,%x status=%02x: %x (%s)\n",
               op, km->km_op, status, rc, smash_err(rc));
        time_delay_msec(100);
    } while (retry-- > 0);
}

static uint
handle_atou_messages(void)
{
    uint8_t   rxdata[4096];
//  void     *buf;
    uint      status;
    uint      rxlen;
    uint      rc;
    uint      handled = 0;

    /*
     * Note that the length, command, and payload of messages received from
     * the Amiga are byte-swapped (B1 B0 B3 B2 B5 B4...). The send_msg()
     * and recv_msg() functions take care of this byte swapping.
     */
    while (1) {
        rc = recv_msg(TAG_QUEUE_ANY, rxdata, sizeof (rxdata), &status, &rxlen);
        if (rc != 0) {
            printf("HS recv_msg failure: %d (%s)\n", rc, smash_err(rc));
            return (rc);
        }
        if ((status & ~KS_MSG_OVERRIDE) == KS_CMD_MSG_SEND) {
            process_msg(status, rxdata, rxlen);
            handled++;
        } else if ((status == KS_STATUS_NODATA) ||
                   (status == KS_STATUS_LOCKED)) {
            break;
        } else if (status != 0) {
            printf("HS recv_msg rc=%x status=%04x len=%x", rc, status, rxlen);
            if (rxlen > 0) {
                uint pos;
                printf(" data=");
                if (rxlen > 16)
                    rxlen = 16;
                for (pos = 0; pos < rxlen; pos++) {
                    if (pos > 0)
                        printf(" ");
                    printf("%02x", rxdata[pos]);
                }
            }
            printf("\n");
            break;
        }
    }
    return (handled);
}

static void
run_message_mode(void)
{
    char buf[2048];
    uint rxlen;
    uint status;
    uint rc;
    uint curtick = 10;
    uint count;
    time_t   time_now  = time(NULL) + 1;
    time_t   time_next = time_now + 4;
    uint16_t app_state = MSG_STATE_SERVICE_UP | MSG_STATE_HAVE_LOOPBACK;
    smash_msg_info_t mi;

    if (amiga_vol_head != NULL)
        app_state |= MSG_STATE_HAVE_FILE;

    if (amiga_net_service != 0)
        app_state |= MSG_STATE_HAVE_NET;
    msgprintf("Message mode\n");
    app_state_send[0] = SWAP16(0xffff);     // Affect all bits
    app_state_send[1] = SWAP16(app_state);  // Message service up

    if (send_cmd("prom service"))
        return; // "timeout" was reported in this case

    show_ks_inquiry();

    rc = send_ks_cmd(KS_CMD_MSG_STATE | KS_MSG_STATE_SET, app_state_send,
                     sizeof (app_state_send), buf, sizeof (buf), &status,
                     &rxlen, 1);
    if (rc == 0)
        rc = status;
    if (rc != 0) {
        printf("HS Set App State failed: %d (%s)\n", rc, smash_err(rc));
        return;
    }

    rc = send_ks_cmd(KS_CMD_MSG_FLUSH, NULL, 0, NULL, 0, &status, NULL, 0);
    if (rc == 0)
        rc = status;
    if (rc != 0) {
        printf("HS Msg Flush failed: %d (%s)\n", rc, smash_err(rc));
        return;
    }

    sm_init_queues();
    if (amiga_net_service != 0)
        netif_start();  // Starts netif reader and network forwarding threads

    while (1) {
        if (curtick != 0) {
            if (curtick < 1024)
                usleep(curtick);
            else
                time_delay_msec(curtick / 1024);
        }
        time_now = time(NULL);
        if (time_next <= time_now) {
            time_next = time_now + 4;
            keep_app_state();  // do this once every 4 seconds
        }

        if (ks_message_terminated)
            break;

        /*
         * The message buffer status is only reported (msgprintf), so
         * only spend a Kicksmash transaction on it when debugging.
         */
        if ((curtick > 1000) && debug_msg) {
            rc = send_ks_cmd(KS_CMD_MSG_INFO, NULL, 0, &mi, sizeof (mi),
                             &status, NULL, 0);
            if (ks_message_terminated)
                break;
            if (rc != 0) {
                printf("HS message failure: %d (%s)\n", rc, smash_err(rc));
                continue;
            }
            if ((mi.smi_atou_inuse != 0) || (mi.smi_utoa_inuse != 0)) {
                msgprintf("  atou inuse=%u avail=%u  utoa inuse=%u avail=%u\n",
                          SWAP16(mi.smi_atou_inuse),
                          SWAP16(mi.smi_atou_avail),
                          SWAP16(mi.smi_utoa_inuse),
                          SWAP16(mi.smi_utoa_avail));
            }
        }

        count = handle_atou_messages();
        if (count != 0) {
            /* Handled message */
            curtick = 0;
        } else {
            /* No message */
            if (curtick == 0)
                curtick = 10;
            else if (curtick < 500000)
                curtick += curtick / 2;
        }
    }
    sm_destroy_queues();
}

static rc_t
clock_ks_set(int enter)
{
    rc_t rc = 0;
    uint32_t amtime[2];
    struct timeval tv;
    struct timezone tz;
    uint rxlen;
    uint status;

    if (gettimeofday(&tv, &tz)) {
        printf("gettimeofday failed: %d\n", errno);
        return (RC_FAILURE);
    }
    amtime[0] = get_localtime(tv.tv_sec - AMIGA_SEC_TO_UNIX_SEC);
    amtime[1] = tv.tv_usec;

    if (enter && send_cmd("prom service")) {
        printf("could not enter prom service\n");
        return (RC_TIMEOUT); // "timeout" was reported in this case
    }
    rc = send_ks_cmd(KS_CMD_CLOCK | KS_CLOCK_SET, amtime, sizeof (amtime),
                     NULL, 0, &status, &rxlen, 0);
    if (rc != 0) {
        printf("HS clock set failed: %d (%s)\n", rc, smash_err(rc));
        return (rc);
    }
    return (rc);
}

static rc_t
clock_ks_show(int enter)
{
    time_t timev;
    uint32_t amtime[2];
    struct tm *tm;
    uint rxlen;
    uint status;
    rc_t rc;

    if (enter && send_cmd("prom service")) {
        printf("could not enter prom service\n");
        return (RC_TIMEOUT); // "timeout" was reported in this case
    }
    rc = send_ks_cmd(KS_CMD_CLOCK, NULL, 0, amtime, sizeof (amtime), &status,
                     &rxlen, 0);
    if (rc != 0) {
        printf("HS clock request failed: %d (%s)\n", rc, smash_err(rc));
        return (rc);
    }

    if ((amtime[0] == 0) && (amtime[1] == 0)) {
        printf("Kicksmash time has not been set\n");
        return (RC_FAILURE);
    }

    timev = get_utctime(amtime[0] + AMIGA_SEC_TO_UNIX_SEC);
    tm = localtime(&timev);

    printf("%04u-%02u-%02u %02u:%02u:%02u.%06u\n",
            tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
            tm->tm_hour, tm->tm_min, tm->tm_sec, amtime[1]);
    return (0);
}


#define FW_READ_CHUNK   2048            // Matches firmware FW_READ_MAX
#define FW_READ_MAXSIZE (256 << 10)     // STM32 / GD32 flash size

/*
 * fw_read() reads the Kicksmash firmware image from the STM32 internal
 *           flash and writes it to the specified file.
 *
 * @param [in] filename - Destination filename.
 *
 * @return     RC_SUCCESS - Firmware was read and written to the file.
 * @return     other      - Failure.
 */
static rc_t
fw_read(const char *filename)
{
    uint8_t    rxbuf[sizeof (ks_fw_rw_t) + FW_READ_CHUNK];
    uint8_t   *fwbuf;
    uint32_t   offset = 0;
    uint       status;
    uint       rxlen;
    uint       rc;
    ks_fw_rw_t req;
    ks_fw_rw_t *reply = (ks_fw_rw_t *) rxbuf;
    FILE      *fp;

    fwbuf = malloc(FW_READ_MAXSIZE);
    if (fwbuf == NULL)
        errx(EXIT_FAILURE, "Could not allocate %u bytes", FW_READ_MAXSIZE);

    if (send_cmd("prom service")) {
        printf("could not enter prom service\n");
        rc = RC_TIMEOUT;  // "timeout" was reported in this case
        goto fw_read_fail;
    }

    while (offset < FW_READ_MAXSIZE) {
        uint32_t got_offset;
        uint     got_size;

        memset(&req, 0, sizeof (req));
        req.kfrw_offset = SWAP32(offset);
        req.kfrw_size   = SWAP16(FW_READ_CHUNK);
        rc = send_ks_cmd(KS_CMD_FW_READ, &req, sizeof (req),
                         rxbuf, sizeof (rxbuf), &status, &rxlen, 0);
        if (rc != 0) {
            printf("FW read at 0x%x failed: %d (%s)\n",
                   offset, rc, smash_err(rc));
            goto fw_read_fail;
        }
        if ((status == KS_STATUS_FAIL) && (offset != 0)) {
            /* Offset is past end of image (image is multiple of chunk) */
            break;
        }
        if (status != KS_STATUS_OK) {
            printf("FW read at 0x%x failed: %d (%s)\n",
                   offset, status, smash_err(status));
            rc = status;
            goto fw_read_fail;
        }
        if (rxlen < sizeof (*reply)) {
            printf("FW read at 0x%x: short reply (%u bytes)\n",
                   offset, rxlen);
            rc = MSG_STATUS_BAD_LENGTH;
            goto fw_read_fail;
        }
        got_offset = SWAP32(reply->kfrw_offset);
        got_size   = SWAP16(reply->kfrw_size);
        if ((got_offset != offset) || (got_size > FW_READ_CHUNK) ||
            (got_size != rxlen - sizeof (*reply)) ||
            (got_size > FW_READ_MAXSIZE - offset)) {
            printf("FW read at 0x%x: bad reply offset=0x%x size=0x%x "
                   "len=0x%x\n", offset, got_offset, got_size, rxlen);
            rc = MSG_STATUS_BAD_LENGTH;
            goto fw_read_fail;
        }
        memcpy(fwbuf + offset, rxbuf + sizeof (*reply), got_size);
        offset += got_size;
        if (got_size < FW_READ_CHUNK)
            break;  // Short read means end of image
    }

    fp = fopen(filename, "wb");
    if (fp == NULL)
        err(EXIT_FAILURE, "Failed to open %s", filename);
    if (fwrite(fwbuf, offset, 1, fp) != 1)
        err(EXIT_FAILURE, "Failed to write %s", filename);
    fclose(fp);
    printf("Read 0x%x bytes of firmware and wrote to file %s\n",
           offset, filename);
    rc = RC_SUCCESS;

fw_read_fail:
    free(fwbuf);
    return (rc);
}

#define FW_READ_DOTS      32          // Width of firmware progress bar
#define FW_READ_RETRIES   5           // Attempts per firmware read transfer
#define FW_INFO_MAGIC     0x4B534657  // "KSFW" firmware info (fw/main.h)
#define FW_INFO_MIN_SIZE  20          // Minimum firmware info struct size
#define STM32_FLASH_BASE  0x08000000  // Kicksmash flash address of firmware

/*
 * fw_time_usec() returns the current time in microseconds.
 */
static uint64_t
fw_time_usec(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return ((uint64_t) tv.tv_sec * 1000000 + tv.tv_usec);
}

/*
 * fw_print_us_diff() displays the elapsed time between two fw_time_usec()
 *                    values in milliseconds or seconds.
 */
static void
fw_print_us_diff(uint64_t start, uint64_t end)
{
    uint64_t diff = end - start;
    uint32_t diff2;
    const char *scale = "ms";

    if ((diff >> 32) != 0)  // Finished before started?
        diff = 0;
    if (diff >= 100000) {
        diff /= 1000;
        scale = "sec";
    }
    diff2 = diff / 10;
    printf("%u.%02u %s\n", diff2 / 100, diff2 % 100, scale);
}

/*
 * fw_calc_kb_sec() returns the transfer rate in KB/sec.
 */
static uint
fw_calc_kb_sec(uint usecs, uint bytes)
{
    if (usecs == 0)
        usecs = 1;
    while (bytes > 4000000) {
        bytes >>= 1;
        usecs >>= 1;
    }
    return (bytes * 1000 / usecs);
}

/*
 * fw_le32() returns the little-endian (STM32 order) 32-bit value at the
 *           specified buffer location.
 */
static uint32_t
fw_le32(const uint8_t *buf)
{
    return (buf[0] | (buf[1] << 8) | (buf[2] << 16) | ((uint32_t) buf[3] << 24));
}

/*
 * fw_file_identify() locates the firmware info structure (FW_INFO_MAGIC)
 *                    in a firmware image read from disk, then fills in
 *                    the version, build date/time, and size fields of
 *                    the supplied smash_id_t from it, as KS_CMD_ID would
 *                    report (big-endian) for that firmware.
 *
 * @return 0 - Success.
 * @return 1 - The image does not appear to be valid.
 */
static int
fw_file_identify(const uint8_t *buf, uint buf_size, smash_id_t *id)
{
    uint        off;
    uint        vptr_off;
    uint        str_off;
    uint        ver[2];
    uint        date[3];
    uint        time[3];
    uint32_t    fi_struct_size = 0;
    uint32_t    fi_fw_size = 0;
    const char *str;
    const char *built;

    for (off = 0; off + FW_INFO_MIN_SIZE <= buf_size; off += 4) {
        if (fw_le32(buf + off) != FW_INFO_MAGIC)
            continue;
        fi_struct_size = fw_le32(buf + off + 4);
        fi_fw_size     = fw_le32(buf + off + 16);
        if ((fw_le32(buf + off + 8) == STM32_FLASH_BASE + off) &&
            (fi_struct_size >= FW_INFO_MIN_SIZE) &&
            (fi_fw_size <= buf_size) &&
            (off + fi_struct_size <= fi_fw_size)) {
            break;
        }
    }
    if (off + FW_INFO_MIN_SIZE > buf_size) {
        printf("Firmware info not found in file\n");
        return (1);
    }

    /* fi_version_str is the flash address of a pointer to version_str */
    vptr_off = fw_le32(buf + off + 12) - STM32_FLASH_BASE;
    if ((vptr_off > buf_size - 4) ||
        ((str_off = fw_le32(buf + vptr_off) - STM32_FLASH_BASE) >=
         buf_size) ||
        (memchr(buf + str_off, '\0', buf_size - str_off) == NULL)) {
        printf("Firmware version string not found in file\n");
        return (1);
    }

    /* Format: "Version 2.1+ built YYYY-MM-DD HH:MM:SS" */
    str = (const char *) buf + str_off;
    built = strstr(str, " built ");
    if ((sscanf(str, "Version %u.%u", &ver[0], &ver[1]) != 2) ||
        (built == NULL) ||
        (sscanf(built, " built %u-%u-%u %u:%u:%u", &date[0], &date[1],
                &date[2], &time[0], &time[1], &time[2]) != 6)) {
        printf("Could not parse firmware version \"%s\"\n", str);
        return (1);
    }

    memset(id, 0, sizeof (*id));
    id->si_ks_version[0] = SWAP16(ver[0]);
    id->si_ks_version[1] = SWAP16(ver[1]);
    id->si_ks_date[0]    = date[0] / 100;
    id->si_ks_date[1]    = date[0] % 100;
    id->si_ks_date[2]    = date[1];
    id->si_ks_date[3]    = date[2];
    id->si_ks_time[0]    = time[0];
    id->si_ks_time[1]    = time[1];
    id->si_ks_time[2]    = time[2];
    id->si_size          = SWAP32(fi_fw_size);
    return (0);
}

/*
 * fw_id_show() displays firmware version, build date/time, and size
 *              from a smash_id_t.
 */
static void
fw_id_show(const char *what, const smash_id_t *id)
{
    printf("%-8s Kicksmash %u.%u built %02u%02u-%02u-%02u %02u:%02u:%02u  "
           "Size %u\n", what,
           SWAP16(id->si_ks_version[0]), SWAP16(id->si_ks_version[1]),
           id->si_ks_date[0], id->si_ks_date[1],
           id->si_ks_date[2], id->si_ks_date[3],
           id->si_ks_time[0], id->si_ks_time[1], id->si_ks_time[2],
           SWAP32(id->si_size));
}

/*
 * fw_id_match() returns TRUE if the firmware identity fields of two
 *               smash_id_t match.
 */
static int
fw_id_match(const smash_id_t *id1, const smash_id_t *id2)
{
    return ((memcmp(id1->si_ks_version, id2->si_ks_version,
                    sizeof (id1->si_ks_version)) == 0) &&
            (memcmp(id1->si_ks_date, id2->si_ks_date,
                    sizeof (id1->si_ks_date)) == 0) &&
            (memcmp(id1->si_ks_time, id2->si_ks_time, 3) == 0) &&
            (id1->si_size == id2->si_size));
}

/*
 * fw_file_load() reads a firmware image file into allocated memory and
 *                identifies it with fw_file_identify(). The caller must
 *                free() the buffer when done.
 *
 * @return 0 - Success.
 * @return 1 - Failure (no buffer is then allocated).
 */
static int
fw_file_load(const char *filename, uint8_t **fbufp, uint *fbuf_sizep,
             smash_id_t *file_id)
{
    uint8_t *fbuf;
    uint     fbuf_size;
    long     flen;
    FILE    *fp;

    fp = fopen(filename, "rb");
    if (fp == NULL) {
        printf("Failed to open %s\n", filename);
        return (1);
    }
    if ((fseek(fp, 0, SEEK_END) != 0) || ((flen = ftell(fp)) <= 0) ||
        (fseek(fp, 0, SEEK_SET) != 0)) {
        printf("Failed to get size of %s\n", filename);
        fclose(fp);
        return (1);
    }
    fbuf_size = flen;
    fbuf = malloc(fbuf_size);
    if (fbuf == NULL) {
        printf("Failed to allocate %u bytes\n", fbuf_size);
        fclose(fp);
        return (1);
    }
    if (fread(fbuf, 1, fbuf_size, fp) != fbuf_size) {
        printf("Failed to read %s\n", filename);
        fclose(fp);
        free(fbuf);
        return (1);
    }
    fclose(fp);

    if (fw_file_identify(fbuf, fbuf_size, file_id) != 0) {
        free(fbuf);
        return (1);
    }
    if (SWAP32(file_id->si_size) != fbuf_size) {
        printf("Note: %s is %u bytes; firmware image is %u bytes\n",
               filename, fbuf_size, SWAP32(file_id->si_size));
    }
    *fbufp = fbuf;
    *fbuf_sizep = fbuf_size;
    return (0);
}

/*
 * fw_read_chunk() reads up to FW_READ_CHUNK bytes of Kicksmash firmware
 *                 at the specified offset using KS_CMD_FW_READ. The reply
 *                 header is placed at the start of rbuf, which must be
 *                 sizeof (ks_fw_rw_t) + FW_READ_CHUNK bytes, followed by
 *                 the data. Kicksmash may return less data than requested.
 *
 * @return Number of bytes read, or 0 on failure.
 */
static uint
fw_read_chunk(uint offset, uint fw_size, uint8_t *rbuf, uint *retries)
{
    ks_fw_rw_t  req;
    ks_fw_rw_t *rhdr = (ks_fw_rw_t *) rbuf;
    uint        rbuf_size = sizeof (ks_fw_rw_t) + FW_READ_CHUNK;
    uint        rlen = 0;
    uint        status = 0;
    uint        len;
    uint        rc = 0;
    uint        tries;

    memset(&req, 0, sizeof (req));
    req.kfrw_offset = SWAP32(offset);
    len = fw_size - offset;
    if (len > FW_READ_CHUNK)
        len = FW_READ_CHUNK;
    req.kfrw_size = SWAP16(len);

    /* Reads have no side effects, so retry any failure */
    for (tries = 0; tries < FW_READ_RETRIES; tries++) {
        rc = send_ks_cmd(KS_CMD_FW_READ, &req, sizeof (req),
                         rbuf, rbuf_size, &status, &rlen, 0);
        if (rc == 0)
            rc = status;
        if (rc == 0)
            break;
        (*retries)++;
        if (ks_message_terminated)
            break;
    }
    if (rc != 0) {
        printf("\nFW read at offset 0x%x failed: (%s)\n",
               offset, smash_err(rc));
        return (0);
    }
    len = SWAP16(rhdr->kfrw_size);
    if ((rlen < sizeof (*rhdr)) || (len == 0) ||
        (len > rlen - sizeof (*rhdr)) || (len > fw_size - offset) ||
        (SWAP32(rhdr->kfrw_offset) != offset)) {
        printf("\nFW read at offset 0x%x bad reply: "
               "rlen=%u offset=0x%x size=%u\n",
               offset, rlen, SWAP32(rhdr->kfrw_offset), len);
        return (0);
    }
    return (len);
}

/*
 * fw_progress() advances a progress bar for a firmware operation.
 */
static void
fw_progress(uint done, uint total, uint *dots)
{
    uint dots_now = done * FW_READ_DOTS / total;

    if (dots_now > *dots) {
        for (; *dots < dots_now; (*dots)++)
            printf(".");
        fflush(stdout);
    }
}

/*
 * fw_verify_image() reads the Kicksmash firmware with KS_CMD_FW_READ and
 *                   compares it with the firmware image in memory.
 *
 * @param [in] fbuf     - Firmware image.
 * @param [in] fw_size  - Firmware image size.
 * @param [in] filename - Firmware image filename (for messages).
 *
 * @return     0 - Firmware matches.
 * @return     1 - Firmware does not match, or verify failed.
 */
static int
fw_verify_image(const uint8_t *fbuf, uint fw_size, const char *filename)
{
    uint8_t    *rbuf;
    uint        rbuf_size = sizeof (ks_fw_rw_t) + FW_READ_CHUNK;
    uint        offset;
    uint        pos;
    uint        len;
    uint        retries = 0;
    uint        miscompares = 0;
    uint        first_bad = 0;
    uint        dots = 0;
    uint        dots_now;
    uint64_t    time_start;
    uint64_t    time_end;
    int         errs = 0;

    rbuf = malloc(rbuf_size);
    if (rbuf == NULL) {
        printf("Failed to allocate %u bytes\n", rbuf_size);
        return (1);
    }

    printf("Verifying [%*s]\rVerifying [", FW_READ_DOTS, "");
    fflush(stdout);
    time_start = fw_time_usec();

    for (offset = 0; offset < fw_size; offset += len) {
        if (ks_message_terminated) {
            errs++;
            break;
        }
        len = fw_read_chunk(offset, fw_size, rbuf, &retries);
        if (len == 0) {
            errs++;
            break;
        }
        for (pos = 0; pos < len; pos++) {
            if (rbuf[sizeof (ks_fw_rw_t) + pos] != fbuf[offset + pos]) {
                if (miscompares++ == 0)
                    first_bad = offset + pos;
            }
        }
        dots_now = (offset + len) * FW_READ_DOTS / fw_size;
        if (dots_now > dots) {
            for (; dots < dots_now; dots++)
                printf(".");
            fflush(stdout);
        }
    }
    time_end = fw_time_usec();
    if (errs == 0)
        printf("]");
    printf("\n");
    free(rbuf);

    if (retries != 0)
        printf("%u read retries were required\n", retries);
    if (errs != 0) {
        printf("Firmware verify incomplete\n");
    } else if (miscompares != 0) {
        printf("Kicksmash firmware does not match %s: %u bytes differ, "
               "first at offset 0x%x\n", filename, miscompares, first_bad);
        errs++;
    } else {
        printf("Verified %u bytes of firmware with %s (%u KB/sec) in ",
               fw_size, filename,
               fw_calc_kb_sec((uint) (time_end - time_start), fw_size));
        fw_print_us_diff(time_start, time_end);
    }
    return (errs ? 1 : 0);
}

/*
 * fw_verify() verifies the Kicksmash firmware matches the specified
 *             firmware image file.
 *
 * @param [in] filename - Firmware image file to compare against.
 *
 * @return     0 - Firmware matches.
 * @return     1 - Firmware does not match, or verify failed.
 */
static int
fw_verify(const char *filename)
{
    smash_id_t  id;
    smash_id_t  file_id;
    uint8_t    *fbuf;
    uint        fbuf_size;
    uint        fw_size;
    uint        rlen;
    uint        status;
    uint        rc;
    int         errs = 0;
    int         in_updater;

    if (fw_file_load(filename, &fbuf, &fbuf_size, &file_id) != 0)
        return (1);
    fw_size = SWAP32(file_id.si_size);

    if (send_cmd("prom service")) {
        printf("could not enter prom service\n");
        errs++;
        goto fwverify_end;
    }

    memset(&id, 0, sizeof (id));
    rc = send_ks_cmd(KS_CMD_ID, NULL, 0, &id, sizeof (id), &status, &rlen, 0);
    if (rc == 0)
        rc = status;
    if (rc != 0) {
        printf("Kicksmash ID failure: (%s)\n", smash_err(rc));
        errs++;
        goto fwverify_end;
    }

    /*
     * The updater reports its own version and build date/time, but the
     * size of the firmware in flash, so only the size can be checked.
     */
    in_updater = ((SWAP16(id.si_features) & SID_FEATURE_FW_UPDATE) != 0);
    fw_id_show(in_updater ? "Updater" : "Running", &id);
    fw_id_show("File", &file_id);
    if (in_updater ? (id.si_size != file_id.si_size) :
                     !fw_id_match(&id, &file_id)) {
        printf("Kicksmash firmware does not match %s\n", filename);
        errs++;
        goto fwverify_end;
    }
    errs += fw_verify_image(fbuf, fw_size, filename);

fwverify_end:
    free(fbuf);
    return (errs ? 1 : 0);
}

#define FW_WRITE_CHUNK    2048        // Maximum bytes per DFU download
#define FW_WRITE_RETRIES  5           // Attempts per firmware write transfer
#define FW_USB_SETTLE_MS  500         // USB re-enumeration delay
#define FW_DFU_WAIT_MS    10000       // Time to wait for DFU device to appear
#define FW_REAPPEAR_TRIES 20          // Serial port reappear polls (500 ms)

static bool fw_serial_started = TRUE;  // Serial device open, threads running
static char fw_dfu_id[64];             // DFU device to write (already in DFU)

/*
 * fw_serial_is_open() returns TRUE if the serial device is currently open.
 */
static bool
fw_serial_is_open(void)
{
#ifdef __MINGW32__
    return (dev_handle != INVALID_HANDLE_VALUE);
#else
    return (dev_fd != -1);
#endif
}

/*
 * fw_serial_use() switches communication to the specified serial port.
 *                 If the serial device has not been opened, it is opened
 *                 and the serial I/O threads are started. Otherwise, the
 *                 reader thread will reopen the device at the new path.
 *
 * @return TRUE  - The serial device is open.
 * @return FALSE - The serial device is not (yet) available.
 */
static bool
fw_serial_use(const ks_port_t *port)
{
    if (!fw_serial_started) {
        snprintf(device_name, sizeof (device_name), "%s", port->path);
#ifdef __MINGW32__
        host_device_name = malloc(strlen(device_name) + 16);
        if (host_device_name == NULL)
            err(EXIT_FAILURE, "malloc failed");
        sprintf(host_device_name, "\\\\.\\%s", device_name);
#endif
        if (serial_open(FALSE) != RC_SUCCESS)
            return (FALSE);
        create_threads();
        fw_serial_started = TRUE;
        return (TRUE);
    }
#ifndef __MINGW32__
    if (strcmp(device_name, port->path) != 0) {
        /* Board came back at a different path */
        snprintf(device_name, sizeof (device_name), "%s", port->path);
    }
#endif
    return (fw_serial_is_open());
}

/*
 * fw_ks_identify() enters prom service mode and issues KS_CMD_ID.
 *
 * @return 0 on success, otherwise a KS_STATUS or MSG_STATUS error code.
 */
static uint
fw_ks_identify(smash_id_t *id)
{
    uint status = 0;
    uint rlen;
    uint rc;

    if (send_cmd("prom service"))
        return (MSG_STATUS_NO_REPLY);
    memset(id, 0, sizeof (*id));
    rc = send_ks_cmd(KS_CMD_ID, NULL, 0, id, sizeof (*id), &status, &rlen, 0);
    if (rc == 0)
        rc = status;
    return (rc);
}

/*
 * fw_select_dfu() selects a DFU device to write when the --fwwrite
 *                 Kicksmash serial device was not specified (or is not
 *                 present). A DFU device is preferred over a Kicksmash
 *                 serial device. If --serial was specified, only a
 *                 matching DFU device (by serial number or USB port
 *                 path) is selected.
 *
 * @global [out] fw_dfu_id - The USB port path of the selected device.
 *
 * @return  1 - A single DFU device was selected.
 * @return  0 - No DFU device is present.
 * @return -1 - More than one DFU device is present (they are listed).
 */
static int
fw_select_dfu(void)
{
    dfu_info_t list[DFU_LIST_MAX];
    uint       count = dfu_list(list, ARRAY_SIZE(list));
    uint       matches = 0;
    uint       first = 0;
    uint       cur;

    /*
     * On Windows, a DFU device which has no driver is invisible to libusb.
     * Install the WinUSB driver for any such device, then look again.
     */
    if (dfu_install_drivers()) {
        time_delay_msec(2000);
        count = dfu_list(list, ARRAY_SIZE(list));
    }

    for (cur = 0; cur < count; cur++) {
        if (!dfu_id_match(&list[cur], select_serial))
            continue;
        if (matches++ == 0)
            first = cur;
    }
    if (matches == 0)
        return (0);
    if (matches > 1) {
        printf("Multiple DFU devices found:\n");
        for (cur = 0; cur < count; cur++) {
            if (!dfu_id_match(&list[cur], select_serial))
                continue;
            printf("    USB %-12s Serial %-16s %s\n", list[cur].usbpath,
                   list[cur].serial, list[cur].kind);
        }
        printf("Specify one with --serial <serial or USB path>\n");
        return (-1);
    }
    snprintf(fw_dfu_id, sizeof (fw_dfu_id), "%s",
             (list[first].usbpath[0] != '\0') ? list[first].usbpath :
             list[first].serial);
    return (1);
}

/*
 * fw_page_start() returns the address of the flash page which contains
 *                 the specified address.
 */
static uint32_t
fw_page_start(dfu_dev_t *dev, uint32_t addr)
{
    uint32_t page = dfu_flash_base(dev);
    uint     size;

    while (((size = dfu_page_size(dev, page)) != 0) && (page + size <= addr))
        page += size;
    return (page);
}

/*
 * fw_dfu_erase() erases each flash page which overlaps the specified
 *                range of the firmware image. Pages beyond the image
 *                (including the Kicksmash config area at the end of
 *                flash) are not touched.
 *
 * @param [in]  dev      - DFU device.
 * @param [in]  start    - Image start offset of range.
 * @param [in]  end      - Image end offset of range (exclusive).
 * @param [io]  retries  - Count of retries required.
 * @param [in]  progress - Display a progress bar.
 *
 * @return 0 - Success.
 * @return 1 - Failure.
 */
static int
fw_dfu_erase(dfu_dev_t *dev, uint start, uint end, uint *retries,
             bool progress)
{
    uint32_t base = dfu_flash_base(dev);
    uint32_t addr = fw_page_start(dev, base + start);
    char     errbuf[80];
    uint     size;
    uint     tries;
    uint     dots = 0;
    int      errs = 0;

    if (progress) {
        printf("Erasing [%*s]\rErasing [", FW_READ_DOTS, "");
        fflush(stdout);
    }
    while (addr < base + end) {
        if (ks_message_terminated) {
            errs++;
            break;
        }
        size = dfu_page_size(dev, addr);
        if (size == 0) {
            printf("\nFW erase at offset 0x%x is outside of flash\n",
                   addr - base);
            errs++;
            break;
        }
        for (tries = 0; tries < FW_WRITE_RETRIES; tries++) {
            if (dfu_erase_page(dev, addr, errbuf, sizeof (errbuf)) == 0)
                break;
            (*retries)++;
            dfu_recover(dev);
            if (ks_message_terminated)
                break;
        }
        if (tries >= FW_WRITE_RETRIES) {
            printf("\nFW erase at offset 0x%x failed: (%s)\n",
                   addr - base, errbuf);
            errs++;
            break;
        }
        addr += size;
        if (progress) {
            fw_progress(((addr - base < end) ? addr - base : end) - start,
                        end - start, &dots);
        }
    }
    if (progress) {
        if (errs == 0)
            printf("]");
        printf("\n");
    }
    return (errs);
}

/*
 * fw_dfu_program() writes the firmware image to the (erased) flash.
 *                  When the write chunk size matches the device transfer
 *                  size, the address is set once, and then sequential
 *                  DfuSe blocks are written. If a block must be retried,
 *                  the flash page holding it is erased again, and writing
 *                  restarts at the start of that page.
 *
 * @return 0 - Success.
 * @return 1 - Failure.
 */
static int
fw_dfu_program(dfu_dev_t *dev, const uint8_t *fbuf, uint fw_size,
               uint *retries)
{
    uint8_t  wbuf[FW_WRITE_CHUNK];
    char     errbuf[80];
    uint32_t base = dfu_flash_base(dev);
    uint     chunk = dfu_xfer_size(dev);
    bool     sequential;
    bool     need_setaddr = TRUE;
    uint     base_offset = 0;  // Image offset of DFU_FIRST_BLOCK
    uint     offset = 0;
    uint     fail_end = 0;     // End of the chunk which last failed
    uint     fails = 0;
    uint     len = 0;
    uint     dots = 0;
    int      errs = 0;

    if ((chunk == 0) || (chunk > FW_WRITE_CHUNK))
        chunk = FW_WRITE_CHUNK;
    sequential = (chunk == dfu_xfer_size(dev));

    printf("Writing [%*s]\rWriting [", FW_READ_DOTS, "");
    fflush(stdout);
    while (offset < fw_size) {
        uint block;
        int  rc = 0;

        if (ks_message_terminated) {
            errs++;
            break;
        }
        len = fw_size - offset;
        if (len > chunk)
            len = chunk;
        memcpy(wbuf, fbuf + offset, len);
        block = len;
        if (block & 1)
            wbuf[block++] = 0xff;  // Flash is programmed 16 bits at a time

        if (need_setaddr || !sequential) {
            rc = dfu_set_address(dev, base + offset, errbuf, sizeof (errbuf));
            base_offset = offset;
            need_setaddr = FALSE;
        }
        if (rc == 0) {
            rc = dfu_download(dev, DFU_FIRST_BLOCK +
                              (offset - base_offset) / chunk,
                              wbuf, block, errbuf, sizeof (errbuf));
        }
        if (rc == 0) {
            offset += len;
            if (offset >= fail_end)
                fails = 0;
            fw_progress(offset, fw_size, &dots);
            continue;
        }

        /* Erase the page holding the failed chunk and restart there */
        (*retries)++;
        if (++fails >= FW_WRITE_RETRIES) {
            printf("\nFW write at offset 0x%x failed: (%s)\n",
                   offset, errbuf);
            errs++;
            break;
        }
        fail_end = offset + len;
        dfu_recover(dev);
        offset = fw_page_start(dev, base + offset) - base;
        offset -= offset % chunk;
        (void) fw_dfu_erase(dev, offset, fail_end, retries, FALSE);
        need_setaddr = TRUE;
    }
    if (errs == 0)
        printf("]");
    printf("\n");
    return (errs);
}

/*
 * fw_reconnect() waits for Kicksmash to reappear on a serial port after
 *                a firmware update, then identifies it with KS_CMD_ID.
 *                The port is located by USB serial number if known,
 *                otherwise by USB port path (Linux) or as a port which
 *                was not present before the update.
 *
 * @param [in]  serial  - Expected USB serial number, or empty.
 * @param [in]  usbpath - USB port path of the DFU device, or empty.
 * @param [in]  before  - Kicksmash serial ports present before the update.
 * @param [in]  nbefore - Number of ports in before list.
 * @param [out] id      - Kicksmash identification.
 *
 * @return 0 on success, otherwise a KS_STATUS or MSG_STATUS error code.
 */
static uint
fw_reconnect(const char *serial, const char *usbpath,
             const ks_port_t *before, uint nbefore, smash_id_t *id)
{
    ks_port_t port;
    uint      tries;
    uint      rc = MSG_STATUS_NO_REPLY;

    cmd_quiet = TRUE;
    for (tries = 0; tries < FW_REAPPEAR_TRIES; tries++) {
        if (tries > 0)
            time_delay_msec(500);
        if (!ks_port_find(serial, usbpath, before, nbefore, &port) ||
            !fw_serial_use(&port)) {
            continue;
        }
        /*
         * Kicksmash enables USB before its command line is running, so
         * input sent right after it boots is queued. Its command line
         * then prints a prompt for each queued newline. Let it settle,
         * so that a late prompt isn't taken as the KS_CMD_ID reply.
         */
        send_ll_str("\n");
        (void) wait_for_text("CMD>", 1000);
        discard_input(200);
        rc = fw_ks_identify(id);
        if (rc == 0)
            break;
    }
    cmd_quiet = FALSE;
    return (rc);
}

/*
 * fw_write() writes a firmware image file to Kicksmash using USB DFU.
 *
 *            If Kicksmash is running its firmware, the Kicksmash firmware
 *            updater is started with KS_CMD_FW_UNLOCK, which causes
 *            Kicksmash to re-enumerate on USB as a DFU device. A device
 *            which is already in DFU mode (the Kicksmash updater or the
 *            STM32 ROM DFU bootloader) is written directly.
 *
 *            The flash pages of the image are erased, the image is
 *            written, and the DFU leave request (the equivalent of
 *            KS_CMD_FW_LOCK in the updater) starts the new firmware.
 *            Kicksmash is then located on its serial port, identified
 *            with KS_CMD_ID, and its flash is read back and compared.
 *
 * @param [in] filename - Firmware image file to write.
 *
 * @return     0 - Firmware was written and verified.
 * @return     1 - Failure.
 */
static int
fw_write(const char *filename)
{
    dfu_dev_t        *dev = NULL;
    const dfu_info_t *info;
    smash_id_t        id;
    smash_id_t        file_id;
    ks_port_t         before[KS_PORTS_MAX];
    uint8_t          *fbuf = NULL;
    char              serial[sizeof (info->serial)];
    char              usbpath[sizeof (info->usbpath)];
    uint              nbefore;
    uint              fbuf_size = 0;
    uint              fw_size;
    uint              rc;
    uint              retries = 0;
    uint64_t          time_start;
    uint64_t          time_end;
    int               errs = 0;
    bool              in_dfu = (fw_dfu_id[0] != '\0');

    if (!dfu_supported()) {
        printf("hostsmash was built without libusb; firmware write (DFU) "
               "is not supported\n");
        return (1);
    }
    if (fw_file_load(filename, &fbuf, &fbuf_size, &file_id) != 0)
        return (1);
    fw_size = SWAP32(file_id.si_size);
    if (fw_size == 0) {
        printf("Firmware image size %u is invalid\n", fw_size);
        errs++;
        goto fwwrite_end;
    }

    /* Identify the running firmware or the DFU device */
    serial[0] = '\0';
    usbpath[0] = '\0';
    if (in_dfu) {
        dev = dfu_wait_open(fw_dfu_id, FW_DFU_WAIT_MS);
        if (dev == NULL) {
            errs++;
            goto fwwrite_end;
        }
        info = dfu_dev_info(dev);
        printf("%-8s %s at USB %s  Serial %s\n", "DFU",
               info->kind, info->usbpath, info->serial);
        if (info->is_updater)
            snprintf(serial, sizeof (serial), "%s", info->serial);
        snprintf(usbpath, sizeof (usbpath), "%s", info->usbpath);
    } else {
        rc = fw_ks_identify(&id);
        if (rc != 0) {
            printf("Kicksmash ID failure: (%s)\n", smash_err(rc));
            errs++;
            goto fwwrite_end;
        }
        memcpy(serial, id.si_serial, sizeof (id.si_serial));
        serial[sizeof (id.si_serial)] = '\0';
        fw_id_show("Running", &id);
    }
    fw_id_show("File", &file_id);

    if (!are_you_sure("Write Kicksmash firmware")) {
        errs++;
        goto fwwrite_end;
    }

    /* Start the firmware updater, unless already in DFU mode */
    if (!in_dfu) {
        uint status = 0;

        reopen_quiet = TRUE;  // Serial device will disappear
        rc = send_ks_cmd(KS_CMD_FW_UNLOCK, NULL, 0, NULL, 0, &status, NULL, 0);
        if (rc == 0)
            rc = status;
        if (rc != 0) {
            printf("Kicksmash FW unlock failure: (%s)\n", smash_err(rc));
            if (rc == KS_STATUS_UNKCMD)
                printf("Use \"reset dfu\" at the Kicksmash CMD> prompt "
                       "to start the updater\n");
            errs++;
            goto fwwrite_end;
        }
        /* USB must re-enumerate */
        time_delay_msec(FW_USB_SETTLE_MS);
        dev = dfu_wait_open(serial, FW_DFU_WAIT_MS);
        if (dev == NULL) {
            errs++;
            goto fwwrite_end;
        }
        snprintf(usbpath, sizeof (usbpath), "%s", dfu_dev_info(dev)->usbpath);
    }
    if (dfu_flash_base(dev) != STM32_FLASH_BASE) {
        printf("DFU flash is at 0x%08x, not 0x%08x\n",
               dfu_flash_base(dev), STM32_FLASH_BASE);
        errs++;
        goto fwwrite_end;
    }
    if (fw_size > dfu_flash_size(dev)) {
        printf("Firmware image size %u exceeds %u bytes of flash\n",
               fw_size, dfu_flash_size(dev));
        errs++;
        goto fwwrite_end;
    }
    dfu_recover(dev);

    time_start = fw_time_usec();
    if ((fw_dfu_erase(dev, 0, fw_size, &retries, TRUE) != 0) ||
        (fw_dfu_program(dev, fbuf, fw_size, &retries) != 0)) {
        errs++;
    }
    time_end = fw_time_usec();
    if (retries != 0)
        printf("%u erase/write retries were required\n", retries);
    if (errs != 0) {
        printf("Firmware write incomplete; Kicksmash is still in DFU mode\n");
        goto fwwrite_end;
    }
    printf("Wrote %u bytes of firmware from %s (%u KB/sec) in ",
           fw_size, filename,
           fw_calc_kb_sec((uint) (time_end - time_start), fw_size));
    fw_print_us_diff(time_start, time_end);

    /* Note serial ports present now, to find a new one if serial unknown */
    reopen_quiet = TRUE;
    nbefore = ks_port_list(before, ARRAY_SIZE(before));

    /* Start the new firmware */
    dfu_leave(dev);
    dfu_close(dev);
    dev = NULL;

    /* USB must re-enumerate */
    time_delay_msec(FW_USB_SETTLE_MS);

    /* Wait for the new firmware to appear on its serial port */
    rc = fw_reconnect(serial, usbpath, before, nbefore, &id);
    reopen_quiet = FALSE;
    if (rc != 0) {
        printf("Kicksmash did not respond on a serial port after update: "
               "(%s)\n", smash_err(rc));
        printf("Firmware was written. To verify, use:\n"
               "    hostsmash --fwverify %s%s%s\n", filename,
               (serial[0] != '\0') ? " --serial " : "", serial);
        errs++;
        goto fwwrite_end;
    }
    if ((serial[0] != '\0') &&
        (strncmp(id.si_serial, serial, sizeof (id.si_serial)) != 0)) {
        printf("Kicksmash at %s has serial %.*s, expected %s\n",
               device_name, (int) sizeof (id.si_serial), id.si_serial,
               serial);
        errs++;
        goto fwwrite_end;
    }
    if (SWAP16(id.si_features) & SID_FEATURE_FW_UPDATE) {
        printf("Kicksmash is still running the firmware updater\n");
        errs++;
        goto fwwrite_end;
    }
    fw_id_show("Running", &id);
    if (!fw_id_match(&id, &file_id)) {
        printf("Kicksmash firmware does not match %s\n", filename);
        errs++;
        goto fwwrite_end;
    }
    if (fw_verify_image(fbuf, fw_size, filename) != 0) {
        errs++;
        goto fwwrite_end;
    }
    printf("Kicksmash firmware update verified at %s (serial %.*s)\n",
           device_name, (int) sizeof (id.si_serial), id.si_serial);

fwwrite_end:
    cmd_quiet = FALSE;
    reopen_quiet = FALSE;
    dfu_close(dev);
    free(fbuf);
    return (errs ? 1 : 0);
}

/*
 * run_mode() handles command line options provided by the user.
 *
 * @param [in] mode       - Bitmask of specified modes (some may be combined).
 * @param [in] bank       - Base address as multiple of length to read/write.
 * @param [in] baseaddr   - Base address, if specified.
 * @param [in] len        - Length, if specified.
 * @param [in] report_max - Maximum miscompares to show in verbose manner.
 * @param [in] fill       - Fill the remaining EEPROM with duplicate images.
 * @param [in] filename   - Source or destination filename.
 *
 * @return       0 - Success.
 * @return       1 - Failure.
 */
int
run_mode(uint mode, uint bank, uint baseaddr, uint len_specified,
         uint report_max, bool fill, const char *file1, const char *file2)
{
    int amiga_was_put_in_reset = 0;
    int rc;
    uint len = len_specified;
    uint8_t *filebuf = NULL;

    if (mode == MODE_UNKNOWN) {
        warnx("You must specify one of: -e -i -r -t or -w");
        usage(stderr);
        return (1);
    }
    if (mode & MODE_TERM) {
        run_terminal_mode();
        return (0);
    }
    if (mode & (MODE_ID | MODE_ID_KS)) {
        if (mode & MODE_ID)
            eeprom_id();
        if (mode & MODE_ID_KS) {
            if (send_cmd("prom service") == 0)
                show_ks_inquiry();
        }
        return (0);
    }
    if (mode & (MODE_VERSION)) {
        print_version(stdout);
        if (device_name[0] != '\0')
            show_ks_version();
        return (0);
    }
    if (mode & MODE_MSG) {
        run_message_mode();
        return (0);
    }
    if (mode & (MODE_CLOCK_GET | MODE_CLOCK_SET)) {
        int enter = 1;
        if (mode & MODE_CLOCK_SET)
            clock_ks_set(enter--);

        // XXX: Also add regular clock set service to message mode
        return (clock_ks_show(enter));
    }
    if (mode & MODE_FWREAD)
        return (fw_read(file1) != RC_SUCCESS);
    if (mode & MODE_FWVERIFY)
        return (fw_verify(file1));
    if (mode & MODE_FWWRITE)
        return (fw_write(file1));
    if (((file1 == NULL) || (file1[0] == '\0')) &&
        (mode & (MODE_READ | MODE_VERIFY | MODE_WRITE))) {
        warnx("You must specify a filename with -r or -v or -w option\n");
        usage(stderr);
        return (1);
    }

    if (mode & (MODE_WRITE | MODE_VERIFY)) {
        struct stat statbuf;
        if (stat(file1, &statbuf))
            errx(EXIT_FAILURE, "Failed to stat %s", file1);

        if (len == EEPROM_SIZE_NOT_SPECIFIED) {
            len = EEPROM_SIZE_DEFAULT;
            if (len > statbuf.st_size)
                len = statbuf.st_size;
        } else {
            if (file2 != NULL) {
                /* Length split among two files (value restored below) */
                len /= 2;
            }
            if (len > statbuf.st_size) {
                errx(EXIT_FAILURE, "Length 0x%x is greater than %s size %jx",
                     len, file1, (intmax_t)statbuf.st_size);
            }
        }
    }
#define KICKSMASH_ROM_BANK_SIZE

    if (bank != BANK_NOT_SPECIFIED) {
        if ((mode & (MODE_READ | MODE_ERASE)) &&
            (len == EEPROM_SIZE_NOT_SPECIFIED)) {
            len = EEPROM_BANK_SIZE_DEFAULT;  // Kicksmash unmerged bank size
        }
    }

    switch (amiga_is_in_reset()) {
        case -1:
            errx(EXIT_FAILURE, "Reset check failed");
        case 0:
            if (!are_you_sure("Put Amiga in reset"))
                exit(0);
            amiga_was_put_in_reset = 1;
            if (reset_amiga(1))
                errx(EXIT_FAILURE, "Failed to put Amiga in reset");
            break;
        default:
            break;
    }

    get_kicksmash_mode();
    if (mode & MODE_READ) {
        rc = eeprom_read(file1, bank, baseaddr, len);
        goto run_finish;
    }

    rc = 0;
    if (mode & (MODE_WRITE | MODE_VERIFY)) {
        filebuf = file_read(file1, len);
        if (file2 != NULL) {
            uint8_t *filebuf2 = file_read(file2, len);
            uint8_t *newbuf = malloc(len * 2);
            uint16_t *sptr1 = (uint16_t *) filebuf;
            uint16_t *sptr2 = (uint16_t *) filebuf2;
            uint16_t *dptr  = (uint16_t *) newbuf;
            uint      cur;
            if (newbuf == NULL)
                errx(EXIT_FAILURE, "Could not allocate %u bytes", len);

            /* Merge files */
            if (len <= EEPROM_BANK_SIZE_DEFAULT / 2)
                len *= 2;
            if (len_specified != EEPROM_SIZE_NOT_SPECIFIED)
                len = len_specified;
            for (cur = 0; cur < len; cur += 4) {
                *(dptr++) = *(sptr1++);
                *(dptr++) = *(sptr2++);
            }

            free(filebuf);
            free(filebuf2);
            filebuf = newbuf;
        }
        rc = execute_swapmode(filebuf, len, SWAP_TO_ROM);
        if (rc != 0)
            goto run_finish;
    }

    /* Length might have changed due to merged ROMs -- do erase now */
    if (mode & MODE_ERASE) {
        if (eeprom_erase(bank, baseaddr, len))
            return (1);
    } else if (mode & MODE_WRITE) {
        if (are_you_sure("Erase area before write?")) {
            uint temp = force_yes;
            force_yes = 1;
            if (eeprom_erase(bank, baseaddr, len))
                return (1);
            force_yes = temp;
        }
    }

    if (mode & (MODE_WRITE | MODE_VERIFY)) {
        if (baseaddr == ADDR_NOT_SPECIFIED)
            baseaddr = 0x000000;  // Start of EEPROM

        if (bank != BANK_NOT_SPECIFIED)
            baseaddr += bank * EEPROM_BANK_SIZE_DEFAULT;

        do {
            if ((mode & MODE_WRITE) &&
                (eeprom_write(filebuf, baseaddr, len) != 0)) {
                rc = 1;
                break;
            }

            if ((mode & MODE_VERIFY) &&
                (eeprom_verify(filebuf, baseaddr, len, report_max) != 0)) {
                rc = 1;
                break;
            }

            baseaddr += len;
            if (baseaddr >= EEPROM_SIZE_DEFAULT)
                break;
        } while (fill);

        free(filebuf);
    }
run_finish:
    if (amiga_was_put_in_reset) {
        reset_amiga(0);
        time_delay_msec(100);
        if (reset_amiga(0))
            warnx("Failed to take Amiga out of reset");
    }
    return (rc);
}

/*
 * construct_terminal_cmd() converts the remainder of the command line into
 *                          a string which may be passed to Kicksmash as a
 *                          terminal command.
 */
static void
construct_terminal_cmd(int argc, char * const argv[])
{
    int arg;
    size_t len = 1;

    if (argc == 0)
        return;

    /* Get length to allocate */
    for (arg = 0; arg < argc; arg++) {
        len += strlen(argv[arg]) + 1;
    }
    terminal_cmd = malloc(len);
    terminal_cmd[0] = '\0';
    for (arg = 0; arg < argc; arg++) {
        strcat(terminal_cmd, argv[arg]);
        strcat(terminal_cmd, " ");
    }
}

/*
 * main() is the entry point of the hostsmash utility.
 *
 * @param [in] argc     - Count of user arguments.
 * @param [in] argv     - Array of user arguments.
 *
 * @exit EXIT_USAGE   - Command argument invalid.
 * @exit EXIT_FAILURE - Command failed.
 * @exit EXIT_SUCCESS - Command completed.
 */
int
main(int argc, char * const *argv)
{
    int              pos;
    int              rc;
    int              ch;
    int              long_index = 0;
    bool             fill       = FALSE;
    uint             bank       = BANK_NOT_SPECIFIED;
    uint             baseaddr   = ADDR_NOT_SPECIFIED;
    uint             len        = EEPROM_SIZE_NOT_SPECIFIED;
    uint             report_max = 64;
    char            *file1      = NULL;
    char            *file2      = NULL;
    uint             mode       = MODE_UNKNOWN;
#ifndef __MINGW32__
    struct sigaction sa;

    memset(&sa, 0, sizeof (sa));
    sa.sa_handler = sig_exit;
    (void) sigaction(SIGTERM, &sa, NULL);
    (void) sigaction(SIGINT,  &sa, NULL);
    (void) sigaction(SIGQUIT, &sa, NULL);
    (void) sigaction(SIGPIPE, &sa, NULL);
#endif

#ifdef __MINGW32__
    /* Elevated copy of hostsmash run to install a Windows driver */
    rc = windrv_internal_main(argc, argv);
    if (rc >= 0)
        exit(rc);
#endif
    device_name[0] = '\0';

    while ((ch = getopt_long(argc, argv, short_opts, long_opts,
                             &long_index)) != EOF) {
reswitch:
        switch (ch) {
            case ':':
                if ((optopt == 'v') && file1 != NULL) {
errx(EXIT_FAILURE, "how did we get here?");
                    /* Allow -v to be specified at end to override write */
                    ch = optopt;
                    optarg = file1;
                    mode = MODE_UNKNOWN;
                    goto reswitch;
                }
                warnx("The -%c flag requires an argument", optopt);
                if (optopt == 's') {
                    errx(EXIT_FAILURE,
                         "Valid options are 1032, 2301, or 3210\n");
                }
                if (optopt == 'n') {
                    errx(EXIT_FAILURE,
                         "Valid options:"
#if defined(LINUX)
                         " macvtap"
#elif defined(OSX)  /* MacOS */
                         " bpf"
#elif defined(__MINGW32__)
                         " pcap"
                         " tap"
#endif
                         "\n");
                }
                usage(stderr);
                exit(EXIT_FAILURE);
                break;
            case 'A':
                report_max = 0xffffffff;
                break;
            case 'a':  // address
                if ((sscanf(optarg, "%i%n", (int *)&baseaddr, &pos) != 1) ||
                    (optarg[pos] != '\0') || (pos == 0)) {
                    errx(EXIT_FAILURE, "Invalid address \"%s\"", optarg);
                }
                break;
            case 'b':  // bank
                if ((sscanf(optarg, "%i%n", (int *)&bank, &pos) != 1) ||
                    (optarg[pos] != '\0') || (pos == 0)) {
                    errx(EXIT_FAILURE, "Invalid bank \"%s\"", optarg);
                }
                break;
            case 'c':
                if (strcmp(optarg, "set") == 0) {
                    mode |= MODE_CLOCK_SET;
                } else if ((strcmp(optarg, "show") == 0) ||
                           (strcmp(optarg, "get") == 0)) {
                    mode |= MODE_CLOCK_GET;
                } else {
                    errx(EXIT_FAILURE, "Invalid clock '%s': use set or show\n",
                         optarg);
                }
                break;
            case 'D':
                ic_delay = atou(optarg);
                break;
            case 'd':
                strcpy(device_name, optarg);
                break;
            case 'e':
                if (mode & (MODE_ID | MODE_READ | MODE_TERM))
                    errx(EXIT_FAILURE, "Only one of -iert may be specified");
                mode |= MODE_ERASE;
                break;
            case 'f':
                fill = TRUE;
                break;
            case 'i':
                if ((mode != MODE_UNKNOWN) &&
                    ((mode & (MODE_ID | MODE_ID_KS)) == 0)) {
                    errx(EXIT_FAILURE,
                         "-%c may not be specified with any other mode", ch);
                }
                if (mode & MODE_ID)
                    mode = MODE_ID_KS;
                else if (mode & MODE_ID_KS)
                    mode |= MODE_ID;
                else
                    mode = MODE_ID;
                break;
            case 'l':
                if ((sscanf(optarg, "%i%n", (int *)&len, &pos) != 1) ||
                    (optarg[pos] != '\0') || (pos == 0)) {
                    errx(EXIT_FAILURE, "Invalid length \"%s\"", optarg);
                }
                break;
            case 'M':
            case 'm':
                mode = MODE_MSG;
                if ((optind >= argc) || (argv[optind] == NULL)) {
                    errx(EXIT_FAILURE, "-m requires both am Amiga volume name "
                         "and local path to mount.\nExample: -m ks: .");
                }
                volume_add(optarg, argv[optind], (ch == 'M'));
                optind++;
                break;
            case 'n':
                mode = MODE_MSG;
                amiga_net_service = 1;
                netif_arg = optarg;
                break;
            case 'r':
                if (mode != MODE_UNKNOWN)
                    errx(EXIT_FAILURE,
                         "-%c may not be specified with any other mode", ch);
                mode = MODE_READ;
                break;
            case 'R':
                if (mode != MODE_UNKNOWN)
                    errx(EXIT_FAILURE,
                         "-%c may not be specified with any other mode", ch);
                mode = MODE_FWREAD;
                file1 = optarg;
                break;
            case 'F':
                if (mode != MODE_UNKNOWN)
                    errx(EXIT_FAILURE,
                         "-%c may not be specified with any other mode", ch);
                mode = MODE_FWVERIFY;
                file1 = optarg;
                break;
            case 'W':
                if (mode != MODE_UNKNOWN)
                    errx(EXIT_FAILURE,
                         "-%c may not be specified with any other mode", ch);
                mode = MODE_FWWRITE;
                file1 = optarg;
                break;
            case 's':
                if ((strcasecmp(optarg, "a3000") == 0) ||
                    (strcasecmp(optarg, "a4000") == 0) ||
                    (strcasecmp(optarg, "a4000t") == 0) ||
                    (strcasecmp(optarg, "a1200") == 0)) {
                    swapmode = SWAPMODE_32;
                    break;
                }
                if (strcasecmp(optarg, "a3000t") == 0) {
                    swapmode = SWAPMODE_32SWAP;
                    break;
                }
                if ((strcasecmp(optarg, "a500") == 0) ||
                    (strcasecmp(optarg, "a600") == 0) ||
                    (strcasecmp(optarg, "a1000") == 0) ||
                    (strcasecmp(optarg, "a2000") == 0) ||
                    (strcasecmp(optarg, "cdtv") == 0)) {
                    swapmode = SWAPMODE_16;
                    break;
                }
                if ((sscanf(optarg, "%i%n", (int *)&swapmode, &pos) != 1) ||
                    (optarg[pos] != '\0') || (pos == 0) ||
                    ((swapmode != 0123) && (swapmode != 1032) &&
                     (swapmode != 2301) && (swapmode != 3210))) {
                    errx(EXIT_FAILURE, "Invalid swap mode \"%s\", use "
                                       "1032, 2301, or 3210", optarg);
                }
                break;
            case 't':
                if (mode != MODE_UNKNOWN)
                    errx(EXIT_FAILURE,
                         "-%c may not be specified with any other mode", ch);
                mode = MODE_TERM;
                terminal_mode = TRUE;
                break;
            case 'w':
                if (mode & (MODE_ID | MODE_READ | MODE_TERM))
                    errx(EXIT_FAILURE, "Only one of -irtw may be specified");
                mode |= MODE_WRITE;
                break;
            case 'v':
                if (mode & (MODE_ID | MODE_READ | MODE_TERM))
                    errx(EXIT_FAILURE, "Only one of -irtv may be specified");
                mode |= MODE_VERIFY;
                break;
            case 'V':
                mode |= MODE_VERSION;
                break;
            case 'y':
                force_yes = TRUE;
                break;
            case 'h':
            case '?':
                usage(stdout);
                exit(EXIT_SUCCESS);
                break;
            case 0x80 + 'f':
                debug_fs++;
                break;
            case 0x80 + 'm':
                debug_msg++;
                break;
            case 0x80 + 'n':
                debug_net++;
                break;
            case 0x80 + 'S':
                snprintf(select_serial, sizeof (select_serial), "%s", optarg);
                break;
            default:
                warnx("Unknown option -%c 0x%x", ch, ch);
                usage(stderr);
                exit(EXIT_USAGE);
        }
    }

#ifdef __MINGW32__
    /*
     * Must happen before argc/argv are adjusted below (we want to
     * hand the *original* command line to the elevated relaunch) and
     * before anything else in main() opens a device or spawns
     * hostsmash_netif.exe.
     */
    if (amiga_net_service)
        hostsmash_ensure_elevated_for_net(argc, argv);
#endif

    argc -= optind;
    argv += optind;

    if (mode & (MODE_READ | MODE_WRITE | MODE_VERIFY)) {
        /* First two arguments are filenames */
        if (argc > 0) {
            file1 = argv[0];
            argv++;
            argc--;
        }
        if (argc > 0) {
            file2 = argv[0];
            argv++;
            argc--;
        }
    }
    if (mode & MODE_TERM) {
        construct_terminal_cmd(argc, argv);
        argc = 0;
    }

    if ((mode & (MODE_READ | MODE_WRITE | MODE_VERIFY | MODE_ERASE)) &&
        ((bank == BANK_NOT_SPECIFIED) && (baseaddr == ADDR_NOT_SPECIFIED))) {
        errx(EXIT_USAGE, "You must specify either a bank or an address");
    }

    if (argc > 0)
        errx(EXIT_USAGE, "Too many arguments: %s", argv[0]);

    if ((mode == MODE_FWWRITE) &&
        ((device_name[0] == '\0') || (access(device_name, F_OK) != 0))) {
        /*
         * Prefer a device which is already in DFU mode (the Kicksmash
         * updater or the STM32 ROM DFU bootloader) over one which has
         * a serial port.
         */
        int sel = fw_select_dfu();
        if (sel < 0)
            exit(EXIT_USAGE);
        if (sel > 0) {
            atexit(at_exit_func);
            fw_serial_started = FALSE;
            rc = run_mode(mode, bank, baseaddr, len, report_max, fill,
                          file1, file2);
            if (fw_serial_started)
                wait_for_tx_writer();
            exit(rc);
        }
    }

#ifdef __MINGW32__
    /*
     * A Kicksmash running firmware without an Interface Association
     * Descriptor has no working COM port unless a driver was installed
     * for it. Install Windows' built-in USB serial driver if needed.
     */
    if (windrv_fix_kicksmash()) {
        uint wait;
        for (wait = 0; wait < 50; wait++) {
            ks_port_t ports[KS_PORTS_MAX];
            if (ks_port_list(ports, ARRAY_SIZE(ports)) > 0)
                break;
            time_delay_msec(100);  // Allow time for the COM port to appear
        }
        if (device_name[0] != '\0')
            printf("Note: the Kicksmash COM port may have changed\n");
    }
#endif
    if (device_name[0] == '\0')
        find_mx_programmer();

    if (device_name[0] == '\0') {
        warnx("You must specify a device to open (-d <dev>)");
        if (mode == MODE_UNKNOWN)
            usage(stderr);
        exit(EXIT_USAGE);
    }
    if (len == 0)
        errx(EXIT_USAGE, "Invalid length 0x%x", len);

#ifdef __MINGW32__
    host_device_name = malloc(strlen(device_name) + 16);
    if (host_device_name == NULL)
        err(EXIT_FAILURE, "malloc failed");
    sprintf(host_device_name, "\\\\.\\%s", device_name);
#endif

    atexit(at_exit_func);

    if (serial_open(TRUE) != RC_SUCCESS)
        do_exit(EXIT_FAILURE);

    create_threads();
    rc = run_mode(mode, bank, baseaddr, len, report_max, fill, file1, file2);
    wait_for_tx_writer();

    exit(rc);
}
