/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2024.
 *
 * ---------------------------------------------------------------------
 *
 * UNIX side to interact with MX29F1615 programmer and Amiga Kicksmash
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <signal.h>
#include <string.h>
#include <getopt.h>
#include <limits.h>
#define _GNU_SOURCE
#include <pthread.h>
#include <errno.h>
#include <err.h>
#include <poll.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <ctype.h>
#ifdef LINUX
#include <usb.h>
#include <dirent.h>
#endif
#include "crc32.h"
#include "smash_cmd.h"
#include "host_cmd.h"
#include "version.h"

#define SWAP16(x) __builtin_bswap16(x)
#define SWAP32(x) __builtin_bswap32(x)
#define SWAP64(x) __builtin_bswap64(x)

#define BIT(x) (1U << (x))

/* Program long format options */
static const struct option long_opts[] = {
    { "all",      no_argument,       NULL, 'A' },
    { "addr",     required_argument, NULL, 'a' },
    { "bank",     required_argument, NULL, 'b' },
    { "delay",    required_argument, NULL, 'D' },
    { "device",   required_argument, NULL, 'd' },
    { "erase",    no_argument,       NULL, 'e' },
    { "fill",     no_argument,       NULL, 'f' },
    { "identify", no_argument,       NULL, 'i' },
    { "help",     no_argument,       NULL, 'h' },
    { "len",      required_argument, NULL, 'l' },
    { "mount",    required_argument, NULL, 'm' },
    { "Mount",    required_argument, NULL, 'M' },
    { "read",     no_argument,       NULL, 'r' },
    { "swap",     required_argument, NULL, 's' },
    { "term",     no_argument,       NULL, 't' },
    { "verify",   no_argument,       NULL, 'v' },
    { "write",    no_argument,       NULL, 'w' },
    { "yes",      no_argument,       NULL, 'y' },
    { NULL,       no_argument,       NULL,  0  }
};

static char short_opts[] = {
    ':',         // Missing argument
    'A',         // --all
    'a', ':',    // --addr <addr>
    'b', ':',    // --bank <num>
    'D', ':',    // --delay <num>
    'd', ':',    // --device <filename>
    'e',         // --erase
    'f',         // --fill
    'h',         // --help
    'i',         // --identify
    'l', ':',    // --len <num>
    'm', ':',    // --mount <vol> <dir>
    'M', ':',    // --Mount <vol> <dir>
    'r',         // --read <filename>
    's', ':',    // --swap <mode>
    't',         // --term
    'v',         // --verify <filename>
    'w',         // --write <filename>
    'y',         // --yes
    '\0'
};

/* Program help text */
static const char usage_text[] =
"hostsmash <opts> <dev>\n"
"    -A --all                show all verify miscompares\n"
"    -a --addr <addr>        starting EEPROM address\n"
"    -b --bank <num>         starting EEPROM address as multiple of file size\n"
"    -D --delay <msec>       pacing delay between sent characters (ms)\n"
"    -d --device <filename>  serial device to use (e.g. /dev/ttyACM0)\n"
"    -e --erase              erase EEPROM (use -a <addr> for sector erase)\n"
"    -f --fill               fill EEPROM with duplicates of the same image\n"
"    -h --help               display usage\n"
"    -i --identify           identify installed EEPROM\n"
"    -l --len <num>          length in bytes\n"
"    -m --mount <vol:> <dir> file serve directory path to Amiga volume\n"
"    -r --read <filename>    read EEPROM and write to file\n"
"    -s --swap <mode>        byte swap mode (2301, 3210, 1032, noswap=0123)\n"
"    -v --verify <filename>  verify file matches EEPROM contents\n"
"    -w --write <filename>   read file and write to EEPROM\n"
"    -t --term               just act in terminal mode (CLI)\n"
"    -y --yes                answer all prompts with 'yes'\n"
"    TERM_DEBUG=`tty`        env variable for communication debug output\n"
"    TERM_DEBUG_HEX=1        show debug output in hex instead of ASCII\n"
"\n"
"Example (including specific TTY to open):\n"
#ifdef OSX
"    hostsmash -d /dev/cu.usbmodem* -t\n"
#else
"    hostsmash -d /dev/ttyACM0 -t\n"
#endif
"";

/* Command line modes which may be specified by the user */
#define MODE_UNKNOWN 0x00
#define MODE_ERASE   0x01
#define MODE_ID      0x02
#define MODE_READ    0x04
#define MODE_TERM    0x08
#define MODE_VERIFY  0x10
#define MODE_WRITE   0x20
#define MODE_MSG     0x40

/* XXX: Need to register USB device ID at http://pid.codes */
#define MX_VENDOR 0x1209
#define MX_DEVICE 0x1610

#define EEPROM_SIZE_DEFAULT       0x400000    // 4 MB
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

#define SWAPMODE_A500  0xA500   // Amiga 16-bit ROM format
#define SWAPMODE_A3000 0xA3000  // Amiga 32-bit ROM format

#define SWAP_TO_ROM    0  // Bytes originated in a file (to be written in ROM)
#define SWAP_FROM_ROM  1  // Bytes originated in ROM (to be written to a file)

/* AmigaOS FileInfoBlock Permissions */
#define FIBF_OTR_READ      0x00008000  // Other: file is readable
#define FIBF_OTR_WRITE     0x00004000  // Other: file is writable
#define FIBF_OTR_EXECUTE   0x00002000  // Other: file is executable
#define FIBF_OTR_DELETE    0x00001000  // Other: file may not be deleted
#define FIBF_GRP_READ      0x00000800  // Group: file is readable
#define FIBF_GRP_WRITE     0x00000400  // Group: file is writable
#define FIBF_GRP_EXECUTE   0x00000200  // Group: file is executable
#define FIBF_GRP_DELETE    0x00000100  // Group: file may not be deleted
#define FIBF_HOLD          0x00000080  // Keep pure module resident
#define FIBF_SCRIPT        0x00000040  // Executable script
#define FIBF_PURE          0x00000020  // Reentrant and re-executable
#define FIBF_ARCHIVE       0x00000010  // File has been archived
#define FIBF_READ          0x00000008  // Owner: file is readable
#define FIBF_WRITE         0x00000004  // Owner: file is writable
#define FIBF_EXECUTE       0x00000002  // Owner: file is executable
#define FIBF_DELETE        0x00000001  // Owner: file may not be deleted

#define KS_PATH_MAX              4096  // Maximum lenght of file pathname

typedef unsigned int uint;

static void discard_input(int timeout);

typedef enum {
    RC_SUCCESS = 0,
    RC_FAILURE = 1,
    RC_TIMEOUT = 2,
} rc_t;

typedef enum {
    TRUE  = 1,
    FALSE = 0,
} bool_t;

typedef struct amiga_vol amiga_vol_t;

typedef struct handle_ent handle_ent_t;
typedef struct handle_ent {
    handle_t      he_handle;   // Reference handle for Amiga interface
    char         *he_name;     // Name of file's path relative to parent
    int           he_fd;       // Open file number
    uint          he_type;     // One of HM_TYPE_*
    uint          he_mode;     // Open mode
    uint          he_count;    // Open count
    uint          he_entnum;   // Volume directory entry number
    DIR          *he_dir;      // Open directory pointer
    amiga_vol_t  *he_avolume;  // Volume descriptor for this handle
    handle_ent_t *he_volume;   // Volume for this file
    handle_ent_t *he_next;     // Next in list of all handles
} handle_ent_t;

static handle_ent_t *handle_list_head = NULL;
static handle_t      handle_unique = 0;
static handle_t      handle_default = 0;  // Volume directory is default

typedef struct amiga_vol {
    const char   *av_volume;   // Amiga path
    const char   *av_path;     // Host path
    handle_ent_t *av_handle;
    amiga_vol_t  *av_next;
} amiga_vol_t;
amiga_vol_t *amiga_vol_head = NULL;

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
static int              dev_fd            = -1;
static int              got_terminfo      = 0;
static int              running           = 1;
static uint             ic_delay          = 0;  // Pacing delay (ms)
static char             device_name[PATH_MAX];
static struct termios   saved_term;  // good terminal settings
static bool             terminal_mode     = FALSE;
static bool             force_yes         = FALSE;
static uint             swapmode          = 0123;  // no swap

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

/*
 * usage() displays command usage.
 *
 * @param  [in]  None.
 * @return       None.
 */
static void
usage(FILE *fp)
{
    (void) fputs(usage_text, fp);
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

#if 0
static int
is_same_path(const char *path1, const char *path2)
{
    size_t len1;
    size_t len2;

    /* Trim paths */
    path1 = trim_path(path1, &len1);
    path2 = trim_path(path2, &len2);

    printf("compare '%.*s' %zu with '%.*s' %zu\n", (int)len1, path1, len1, (int)len2, path2, len2);
    if ((len1 == len2) && (strncmp(path1, path2, len1) == 0))
        return (1);
    return (0);
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
    rx_rb_consumer = (rx_rb_consumer + 1) % sizeof (rx_rb);
    return (ch);
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
 * time_delay_msec() will delay for a specified number of milliseconds.
 *
 * @param [in]  msec - Milliseconds from now.
 *
 * @return      None.
 */
static void
time_delay_msec(int msec)
{
    if (poll(NULL, 0, msec) < 0)
        warn("poll() failed");
}

/*
 * send_ll_bin() sends a binary block of data to the remote programmer.
 *
 * @param  [in] data  - Data to send to the programmer.
 * @param  [in] len   - Number of bytes to send.
 */
static int
send_ll_bin(const void *buf, size_t len)
{
    int timeout_count = 0;
    const uint8_t *data = (const uint8_t *)buf;
    size_t pos = 0;

    while (pos < len) {
        if (tx_rb_put(*data)) {
            time_delay_msec(1);
            if (timeout_count++ >= 500) {
                printf("Send timeout at 0x%zx\n", pos);
                return (1);  // Timeout
            }
            printf("-\n"); fflush(stdout);  // XXX: shouldn't happen
            continue;        // Try again
        }
        timeout_count = 0;
        data++;
        pos++;
    }
    return (0);
}

/*
 * config_dev() will configure the serial device used for communicating
 *              with the programmer.
 *
 * @param  [in]  fd - Opened file descriptor for serial device.
 * @return       RC_FAILURE - Failed to configure device.
 */
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
    tty.c_iflag &= (IXON | IXOFF);        // sw flow off

    tty.c_cflag &= ~CRTSCTS;              // hw flow off
    tty.c_cflag &= (uint)~CSIZE;              // no bits
    tty.c_cflag |= CS8;               // 8 bits

    tty.c_cflag &= (uint)~(PARENB | PARODD);  // no parity
    tty.c_cflag &= (uint)~CSTOPB;         // one stop bit

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

/*
 * reopen_dev() will wait for the serial device to reappear after it has
 *              disappeared.
 *
 * @param  [in]  None.
 * @return       None.
 */
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
    if (now - last_time > 5) {
        printed = TRUE;
        printf("\n<< Closed %s >>", device_name);
        fflush(stdout);
    }
top:
    do {
        if (running == 0)
            return;
        time_delay_msec(400);
    } while ((temp = open(device_name, oflags | O_RDWR)) == -1);

    if (config_dev(temp) != RC_SUCCESS)
        goto top;

    /* Hand off the new I/O fd */
    dev_fd = temp;

    now = time(NULL);
    if (now - last_time > 5) {
        if (printed == FALSE)
            printf("\n");
        printf("\r<< Reopened %s >>\n", device_name);
    }
    last_time = now;
}

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
        log_fp = fopen(log_file, "w");
        if (log_fp == NULL)
            warn("Unable to open %s for log", log_file);
        log_hex = (getenv("TERM_DEBUG_HEX") != NULL);
    }

    while (running) {
        ssize_t len;
        while ((len = read(dev_fd, buf, sizeof (buf))) >= 0) {
            if (len == 0) {
#ifdef USE_NON_BLOCKING_TTY
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
static void *
th_serial_writer(void *arg)
{
    int ch;
    uint pos = 0;
    char lbuf[64];

    while (1) {
        ch = tx_rb_get();
        if (ch >= 0)
            lbuf[pos++] = ch;
        if (((ch < 0) && (pos > 0)) ||
             (pos >= sizeof (lbuf)) || (ic_delay != 0)) {
            ssize_t count;
            if (dev_fd == -1) {
                time_delay_msec(500);
                if (pos >= sizeof (lbuf))
                    pos--;
                continue;
            } else if ((count = write(dev_fd, lbuf, pos)) < 0) {
                /* Wait for reader thread to close / reopen */
                time_delay_msec(500);
                if (pos >= sizeof (lbuf))
                    pos--;
                continue;
            } else if (ic_delay) {
                /* Inter-character pacing delay was specified */
                time_delay_msec(ic_delay);
            }
#ifdef DEBUG_TRANSFER
            printf(">%02x\n", lbuf[0]);
#endif
            if (count < pos) {
                printf("sent only %zd of %u\n", count, pos);
            }
            pos = 0;
        } else if (ch < 0) {
            time_delay_msec(10);
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
    int oflags = O_NOCTTY;

#ifdef OSX
    oflags |= O_NONBLOCK;
#endif

    /* First verify the file exists */
    dev_fd = open(device_name, oflags | O_RDONLY);
    if (dev_fd == -1) {
        warn("Failed to open %s for read", device_name);
        return (RC_FAILURE);
    }
    close(dev_fd);

    dev_fd = open(device_name, oflags | O_RDWR);
    if (dev_fd == -1) {
        warn("Failed to open %s for write", device_name);
        return (RC_FAILURE);
    }
    return (config_dev(dev_fd));
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
    if (got_terminfo) {
        got_terminfo = 0;
        tcsetattr(0, TCSANOW, &saved_term);
    }
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

/*
 * sig_exit() will exit on a fatal signal (SIGTERM, etc).
 */
static void
sig_exit(int sig)
{
    do_exit(EXIT_FAILURE);
}

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
    int received = 0;
    int timeout_count = 0;
    uint8_t *data = (uint8_t *)buf;

    while (received < buflen) {
        int ch = rx_rb_get();
        if (ch == -1) {
            if (timeout_count++ >= timeout) {
                if (exact_bytes && ((timeout > 50) || (received == 0))) {
                    printf("Receive timeout (%d ms): got %d of %zu bytes\n",
                           timeout, received, buflen);
                }
                return (received);
            }
            time_delay_msec(1);
            continue;
        }
        timeout_count = 0;
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
        warnx("Bad CRC %08x received from programmer (should be %08x) "
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
        discard_input(250);
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
    int timeout_count = 0;
    while (timeout_count <= timeout) {
        int ch = rx_rb_get();
        if (ch == -1) {
            timeout_count++;
            time_delay_msec(1);
            continue;
        }
        timeout_count = 0;
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
send_ll_crc(uint8_t *data, size_t len)
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

/*
 * execute_swapmode() swaps bytes in the specified buffer according to the
 *                    currently active swap mode.
 *
 * @param  [io]  buf     - Buffer to modify.
 * @param  [in]  len     - Length of data in the buffer.
 * @gloabl [in]  dir     - Image swap direction (SWAP_TO_ROM or SWAP_FROM_ROM)
 * @return       None.
 */
static void
execute_swapmode(uint8_t *buf, uint len, uint dir)
{
    uint    pos;
    uint8_t temp;
    static const uint8_t str_f94e1411[] = { 0xf9, 0x4e, 0x14, 0x11 };
    static const uint8_t str_11144ef9[] = { 0x11, 0x14, 0x4e, 0xf9 };
    static const uint8_t str_1411f94e[] = { 0x14, 0x11, 0xf9, 0x4e };
    static const uint8_t str_4ef91114[] = { 0x4e, 0xf9, 0x11, 0x14 };

    switch (swapmode) {
        case 0:
        case 0123:
            return;  // Normal (no swap)
        swap_1032:
        case 1032:
            /* Swap adjacent bytes in 16-bit words */
            for (pos = 0; pos < len - 1; pos += 2) {
                temp         = buf[pos + 0];
                buf[pos + 0] = buf[pos + 1];
                buf[pos + 1] = temp;
            }
            return;
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
            return;
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
            return;
        case SWAPMODE_A500:
            if (dir == SWAP_TO_ROM) {
                /* Need bytes in order: 14 11 f9 4e */
                if (memcmp(buf, str_1411f94e, 4) == 0)
                    return;  // Already in desired order
                if (memcmp(buf, str_11144ef9, 4) == 0) {
                    printf("Swap mode 2301\n");
                    goto swap_2301;  // Swap adjacent 16-bit words
                }
            }
            if (dir == SWAP_FROM_ROM) {
                /* Need bytes in order: 11 14 4e f9 */
                if (memcmp(buf, str_11144ef9, 4) == 0)
                    return;  // Already in desired order
                if (memcmp(buf, str_1411f94e, 4) == 0) {
                    printf("Swap mode 1032\n");
                    goto swap_1032;  // Swap odd/even bytes
                }
            }
            goto unrecognized;
        case SWAPMODE_A3000:
            if (dir == SWAP_TO_ROM) {
                /* Need bytes in order: f9 4e 14 11 */
                if (memcmp(buf, str_f94e1411, 4) == 0)
                    return;  // Already in desired order
                if (memcmp(buf, str_11144ef9, 4) == 0) {
                    printf("Swap mode 3210\n");
                    goto swap_3210;  // Swap bytes in 32-bit longs
                }
                if (memcmp(buf, str_1411f94e, 4) == 0) {
                    printf("Swap mode 2301\n");
                    goto swap_2301;  // Swap adjacent 16-bit words
                }
                if (memcmp(buf, str_4ef91114, 4) == 0) {
                    printf("Swap mode 1032\n");
                    goto swap_1032;  // Swap odd/even bytes
                }
            }
            if (dir == SWAP_FROM_ROM) {
                /* Need bytes in order: 11 14 4e f9 */
                if (memcmp(buf, str_11144ef9, 4) == 0)
                    return;  // Already in desired order
                if (memcmp(buf, str_f94e1411, 4) == 0) {
                    printf("Swap mode 3210\n");
                    goto swap_3210;  // Swap bytes in 32-bit longs
                }
                if (memcmp(buf, str_4ef91114, 4) == 0) {
                    printf("Swap mode 2301\n");
                    goto swap_2301;  // Swap adjacent 16-bit words
                }
                if (memcmp(buf, str_1411f94e, 4) == 0) {
                    printf("Swap mode 1032\n");
                    goto swap_1032;  // Swap odd/even bytes
                }
            }
unrecognized:
            errx(EXIT_FAILURE,
                 "Unrecognized Amiga ROM format: %02x %02x %02x %02x\n",
                 buf[0], buf[1], buf[2], buf[3]);
    }
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
        addr += bank * len;
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
            if (no_data++ == 20) {
                printf("Receive timeout\n");
                break;  // No output for 2 seconds
            }
        } else {
            no_data = 0;
            printf("%.*s", rxcount, cmd_output);
            fflush(stdout);
            if (strstr(cmd_output, "CMD>") != NULL) {
                /* Normal end */
                break;
            }
        }
    }
    return (0);
}

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
static void
eeprom_read(const char *filename, uint bank, uint addr, uint len)
{
    char cmd[64];
    char *eebuf;
    int rxcount;

    if (addr == ADDR_NOT_SPECIFIED)
        addr = 0x000000;  // Start of EEPROM

    if (len == EEPROM_SIZE_NOT_SPECIFIED)
        len = EEPROM_SIZE_DEFAULT - addr;

    if (bank != BANK_NOT_SPECIFIED)
        addr += bank * len;

    eebuf = malloc(len + 4);
    if (eebuf == NULL)
        errx(EXIT_FAILURE, "Could not allocate %u byte buffer", len);

    snprintf(cmd, sizeof (cmd) - 1, "prom read %x %x", addr, len);
    cmd[sizeof (cmd) - 1] = '\0';
    if (send_cmd(cmd))
        return; // "timeout" was reported in this case
    rxcount = receive_ll_crc(eebuf, len);
    if (rxcount == -1)
        return;  // Send error was reported
    if (rxcount < len) {
        printf("Receive failed at byte 0x%x.\n", rxcount);
        if (strncmp(eebuf + rxcount - 11, "FAILURE", 8) == 0) {
            rxcount -= 11;
            printf("Read %.11s\n", eebuf + rxcount);
        }
    }
    if (rxcount > 0) {
        size_t written;
        FILE *fp = fopen(filename, "w");
        if (fp == NULL)
            err(EXIT_FAILURE, "Failed to open %s", filename);
        execute_swapmode((uint8_t *)eebuf, rxcount, SWAP_FROM_ROM);
        written = fwrite(eebuf, rxcount, 1, fp);
        if (written != 1)
            err(EXIT_FAILURE, "Failed to write %s", filename);
        fclose(fp);
        printf("Read 0x%x bytes from device and wrote to file %s\n",
               rxcount, filename);
    }
    free(eebuf);
}

/*
 * eeprom_write() uses the programmer to writes all or part of an EEPROM image.
 *                Content to write is sourced from a local file.
 *
 * @param  [in]  filename        - The file to write to the EEPROM.
 * @param  [in]  addr            - The EEPROM starting address.
 * @param  [io]  len             - The length to write.
 * @return       0 - Verify successful.
 * @return       1 - Verify failed.
 * @exit         EXIT_FAILURE - The program will terminate on file access error.
 */
static uint
eeprom_write(const char *filename, uint addr, uint len)
{
    FILE       *fp;
    uint8_t    *filebuf;
    char        cmd[64];
#undef SUPPORTS_PROM_STATUS
#ifdef SUPPORTS_PROM_STATUS
    char        cmd_output[64];
    int         rxcount;
#endif
    int         tcount = 0;

    filebuf = malloc(len);
    if (filebuf == NULL)
        errx(EXIT_FAILURE, "Could not allocate %u byte buffer", len);

    fp = fopen(filename, "r");
    if (fp == NULL)
        errx(EXIT_FAILURE, "Failed to open %s", filename);
    if (fread(filebuf, len, 1, fp) != 1)
        errx(EXIT_FAILURE, "Failed to read %u bytes from %s", len, filename);
    fclose(fp);
    execute_swapmode(filebuf, len, SWAP_TO_ROM);

    printf("Writing 0x%06x bytes to EEPROM starting at address 0x%x\n",
           len, addr);

    snprintf(cmd, sizeof (cmd) - 1, "prom write %x %x", addr, len);
    if (send_cmd(cmd))
        return (-1); // "timeout" was reported in this case

    if (send_ll_crc(filebuf, len)) {
        errx(EXIT_FAILURE, "Send failure");
    }

    while (tx_rb_flushed() == FALSE) {
        if (tcount++ > 500)
            errx(EXIT_FAILURE, "Send timeout");

        time_delay_msec(1);
    }
    printf("Wrote 0x%x bytes to device from file %s\n", len, filename);

#ifdef SUPPORTS_PROM_STATUS
    snprintf(cmd, sizeof (cmd) - 1, "prom status");
    cmd[sizeof (cmd) - 1] = '\0';
    if (send_cmd(cmd))
        return (-1); // "timeout" was reported in this case
    if (recv_output(cmd_output, sizeof (cmd_output), &rxcount, 100))
        return (-1); // "timeout" was reported in this case
    if (rxcount == 0) {
        printf("Status receive timeout\n");
        exit(1);
    } else {
        printf("Status: %.*s", rxcount, cmd_output);
    }
#endif

    free(filebuf);
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
show_fail_range(char *filebuf, char *eebuf, uint len, uint addr, uint filepos,
                uint miscompares_max)
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
eeprom_verify(const char *filename, uint addr, uint len, uint miscompares_max)
{
    FILE       *fp;
    char       *filebuf;
    char       *eebuf;
    char        cmd[64];
    int         rxcount;
    int         pos;
    int         first_fail_pos = -1;
    uint        miscompares = 0;

    filebuf = malloc(len);
    eebuf   = malloc(len + 4);
    if ((eebuf == NULL) || (filebuf == NULL))
        errx(EXIT_FAILURE, "Could not allocate %u byte buffer", len);

    fp = fopen(filename, "r");
    if (fp == NULL)
        errx(EXIT_FAILURE, "Failed to open %s", filename);
    if (fread(filebuf, len, 1, fp) != 1)
        errx(EXIT_FAILURE, "Failed to read %u bytes from %s", len, filename);
    fclose(fp);
    execute_swapmode((uint8_t *)filebuf, len, SWAP_TO_ROM);

    snprintf(cmd, sizeof (cmd) - 1, "prom read %x %x", addr, len);
    cmd[sizeof (cmd) - 1] = '\0';
    if (send_cmd(cmd))
        return (1); // "timeout" was reported in this case
    rxcount = receive_ll_crc(eebuf, len);
    if (rxcount <= 0)
        return (1); // "timeout" was reported in this case
    if (rxcount < len) {
        if (strncmp(eebuf + rxcount - 11, "FAILURE", 8) == 0) {
            rxcount -= 11;
            printf("Read %.11s\n", eebuf + rxcount);
        }
        printf("Only read 0x%x bytes of expected 0x%x\n", rxcount, len);
        return (1);
    }

    /* Compare two buffers */
    for (pos = 0; pos < len; pos++) {
        if (eebuf[pos] != filebuf[pos]) {
            miscompares++;
            if (first_fail_pos == -1)
                first_fail_pos = pos;
            if (miscompares == miscompares_max) {
                /* Report now and only count futher miscompares */
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
    free(filebuf);
    if (miscompares) {
        printf("%u miscompares\n", miscompares);
        return (1);
    } else {
        printf("Verify success\n");
        return (0);
    }
}

/*
 * run_terminatl_mode() implements a terminal interface with the programmer's
 *                      command line.
 *
 * @param  [in]  None.
 * @global [in]  device_name[] is the path to the device which was opened.
 * @return       None.
 */
static void
run_terminal_mode(void)
{
    struct termios   term;
    bool_t           literal    = FALSE;
#ifdef USE_NON_BLOCKING_TTY
    int              enable     = 1;
#endif

    if (isatty(fileno(stdin))) {
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
    }

    if (isatty(fileno(stdin)))
        printf("<< Type ^X to exit.  Opened %s >>\n", device_name);

    while (running) {
        int ch = 0;
        ssize_t len;

        while (tx_rb_space() == 0)
            time_delay_msec(20);

        if ((len = read(0, &ch, 1)) <= 0) {
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
    printf("not running\n");
    running = 0;
}

#define LINUX_BY_ID_DIR "/dev/serial/by-id"

/*
 * find_mx_programmer() will attempt to locate tty device associated with USB
 *                      connection of the MX25F1615 programmer. If found, it
 *                      will update the device_name[] global with the file
 *                      path to the serial interface.
 *
 * @param  [in]  None.
 * @global [out] device_name[] is the located path of the programmer (if found).
 * @return       None.
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
 *              USB devices. The ACM device of the MX29F1615 programmer
 *              has a depth of "../../../" from the appropriate uevent file.
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
 */
static void
find_mx_programmer(void)
{
#ifdef LINUX
    /*
     * For Linux, walk /dev/serial/by-id looking for a name which matches
     * the programmer.
     */
    DIR *dirp;
    struct dirent *dent;
    dirp = opendir(LINUX_BY_ID_DIR);
    if (dirp == NULL)
        return;  // Old version of Linux?
    while ((dent = readdir(dirp)) != NULL) {
        if ((strstr(dent->d_name, "MX29F1615") != 0) ||
            (strstr(dent->d_name, "KickSmash") != 0)) {
            snprintf(device_name, sizeof (device_name), "%s/%s",
                     LINUX_BY_ID_DIR, dent->d_name);
            closedir(dirp);
            printf("Using %s\n", device_name);
            return;
        }
    }
    closedir(dirp);
#endif
#ifdef OSX
    char buf[128];
    bool_t saw_programmer = FALSE;
    FILE *fp = popen("ioreg -lrx -c IOUSBHostDevice", "r");
    if (fp == NULL)
        return;

    /*
     * First find "MX29F1615" text and then find line with "IOCalloutDevice"
     * to locate the path to the serial interface for the installed programmer.
     */
    while (fgets(buf, sizeof (buf), fp) != NULL) {
        if (saw_programmer) {
            if (strstr(buf, "IOCalloutDevice") != NULL) {
                char *ptr = strchr(buf, '=');
                if (ptr != NULL) {
                    char *eptr;
                    ptr += 3;
                    eptr = strchr(ptr, '"');
                    if (eptr != NULL)
                        *eptr = '\0';
                    strncpy(device_name, ptr, sizeof (device_name) - 1);
                    device_name[sizeof (device_name) - 1] = '\0';
                    printf("Using %s\n", device_name);
                    return;
                }
                printf("%.80s\n", buf);
            }
            continue;
        }
        if (strstr(buf, "MX29F1615") != NULL) {
            saw_programmer = TRUE;
        }
    }

    fclose(fp);
#endif
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

static handle_ent_t *
handle_new(const char *name, handle_ent_t *parent, uint type, uint mode)
{
    handle_t handle;
    handle_ent_t *node = malloc(sizeof (*node));
    if (node == NULL) {
        printf("alloc %ju bytes failed\n", sizeof (*node));
        return (0);
    }
    memset(node, 0, sizeof (*node));
    handle = ++handle_unique;

    node->he_handle  = handle;
    node->he_name    = strdup(name);
    node->he_type    = type;
    node->he_mode    = mode;
    node->he_count   = 1;
    node->he_entnum  = 0;
    node->he_next    = handle_list_head;
    handle_list_head = node;

    if (type == HM_TYPE_VOLUME) {
        node->he_volume  = node;  // This is the root of the volume
        node->he_avolume = NULL;  // Will be assigned later?
    } else if (parent != NULL) {
        node->he_volume  = parent->he_volume;
        node->he_avolume = parent->he_avolume;

        /* Parent can be NULL if referencing the Volume Directory */
    }

    return (node);
}

static void
handle_free(handle_t handle)
{
    handle_ent_t *parent = NULL;
    handle_ent_t *node;
    for (node = handle_list_head; node != NULL; node = node->he_next) {
        if (node->he_handle == handle) {
            node->he_count--;
            if (node->he_count != 0)
                return;
            if (parent == NULL)
                handle_list_head = node->he_next;
            else
                parent->he_next = node->he_next;
            free(node->he_name);
            free(node);
            return;
        }
        parent = node;
    }
    printf("Failed to find %x in handle list for free\n", handle);
}

static handle_ent_t *
handle_get(handle_t handle)
{
    handle_ent_t *node;
    if (handle == 0xffffffff)  // default can be specified with -M switch
        handle = handle_default;
    if (handle == 0)
        return (NULL);
    for (node = handle_list_head; node != NULL; node = node->he_next)
        if (node->he_handle == handle)
            return (node);
    printf("Failed to find %x in handle list\n", handle);
    return (NULL);
}

static handle_ent_t *
handle_get_name(const char *name)
{
    handle_ent_t *node;
    for (node = handle_list_head; node != NULL; node = node->he_next)
        if ((node->he_name != NULL) && (strcmp(node->he_name, name) == 0))
            return (node);
    printf("Failed to find \"%s\" in handle list\n", name);
    return (NULL);
}

#if 0
static void
handle_list_show(void)
{
    char *type;
    handle_ent_t *node;
    printf("    Type Handle FD D Path               APath              "
           "HPath\n");
    for (node = handle_list_head; node != NULL; node = node->he_next) {
        switch (node->he_type) {
            default:
            case HM_TYPE_UNKNOWN:
                type = "UNKNOWN";
                break;
            case HM_TYPE_FILE:
                type = "FILE";
                break;
            case HM_TYPE_DIR:
                type = "DIR";
                break;
            case HM_TYPE_LINK:
                type = "LINK";
                break;
            case HM_TYPE_BDEV:
                type = "BDEV";
                break;
            case HM_TYPE_CDEV:
                type = "CDEV";
                break;
            case HM_TYPE_FIFO:
                type = "FIFO";
                break;
            case HM_TYPE_SOCKET:
                type = "SOCKET";
                break;
            case HM_TYPE_WHTOUT:
                type = "WHTOUT";
                break;
            case HM_TYPE_VOLUME:
                type = "VOLUME";
                break;
            case HM_TYPE_VOLDIR:
                type = "VOLDIR";
                break;
        }
        printf(" %7s %6x %2d %s %-18s\n",
               type, node->he_handle, node->he_fd,
               node->he_dir ? "Y" : " ", node->he_name);
    }
}
#endif

static void
volume_add(const char *volume_name, const char *local_path, uint is_default)
{
    handle_ent_t *handle;
    amiga_vol_t  *node = malloc(sizeof (amiga_vol_t));
    char *vpos;
    char volnamebuf[128];
    uint volnamelen = strlen(volume_name);

    if (volnamelen > sizeof (volnamebuf) - 2)
        errx(EXIT_FAILURE, "Volume name '%s' too long\n", volume_name);
    if (strchr(volume_name, '/') != NULL)
        errx(EXIT_FAILURE, "Volume name '%s' may not contain '/'\n",
             volume_name);
    vpos = strchr(volume_name, ':');
    if ((vpos != NULL) && ((vpos - volume_name) < volnamelen - 1)) {
        errx(EXIT_FAILURE, "Volume name '%s' only have ':' at end\n",
             volume_name);
    }
    strcpy(volnamebuf, volume_name);
    if (vpos == NULL) {
        /* Append colon */
        volnamebuf[volnamelen++] = ':';
        volnamebuf[volnamelen] = '\0';
    }

    handle = handle_new(volnamebuf, NULL, HM_TYPE_VOLUME, HM_MODE_READ);

    node->av_volume    = strdup(volnamebuf);
    node->av_path      = local_path;
    node->av_handle    = handle;
    node->av_next      = amiga_vol_head;
    amiga_vol_head     = node;

    handle->he_avolume = node;
    handle->he_volume  = handle;  // This is the volume's root handle

    if (is_default) {
        /*
         * This is the optional default parent handle when the Amiga
         * specifies a handle of 0xffffffff.
         */
        handle_default = handle->he_handle;
    }

    printf("add volume %s = %s\n", volnamebuf, local_path);
}

static amiga_vol_t *
volume_get_by_handle(handle_ent_t *handle)
{
    amiga_vol_t *node;
    for (node = amiga_vol_head; node != NULL; node = node->av_next)
        if (node->av_handle == handle)
            return (node);
    printf("Could not locate handle %u in volume list\n", handle->he_handle);
    return (NULL);
}

static amiga_vol_t *
volume_get_by_index(uint index)
{
    amiga_vol_t *node;
    uint count = 0;
    for (node = amiga_vol_head; node != NULL; node = node->av_next)
        if (count++ == index)
            return (node);
    return (NULL);
}

static amiga_vol_t *
volume_get_by_path(const char *path)
{
    amiga_vol_t *node;
    for (node = amiga_vol_head; node != NULL; node = node->av_next)
        if (strcmp(node->av_path, path) == 0)
            return (node);
    return (NULL);
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

static const uint16_t sm_magic[] = { 0x0204, 0x1017, 0x0119, 0x0117 };
static const uint8_t *sm_magic_b = (uint8_t *) sm_magic;

static uint
send_ks_cmd_core(uint cmd, uint len, void *buf)
{
    uint32_t crc;
    uint16_t txlen = len;
    uint16_t txcmd = cmd;
    uint     len_roundup = (len + 1) & ~1;

    crc = crc32r(0, &txlen, 2);
    crc = crc32r(crc, &txcmd, 2);
#if 0
if (len > 0) {
    uint32_t crc1a = crc32s(crc, buf, len & ~1);
    uint32_t crc1b = crc32s(crc1a, (uint8_t *)buf + len, 1);
    uint32_t crc1c = crc32s(crc1a, (uint8_t *)buf + len - 1, 1);
    printf("crc1=%08x crc1a=%08x crc1b=%08x crc1c=%08x last=%02x %02x\n", crc, crc1a, crc1b, crc1c, ((uint8_t *)buf)[len - 1], ((uint8_t *)buf)[len]);
}
#endif
    crc = crc32s(crc, buf, len);
#if 0
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
    if (send_ll_bin(&sm_magic, sizeof (sm_magic)) ||
        send_ll_bin(&txlen, sizeof (txlen)) ||
        send_ll_bin(&txcmd, sizeof (txcmd))) {
        return (MSG_STATUS_FAILURE);
    }
    if ((len > 0) && send_ll_bin(buf, len_roundup))
        return (MSG_STATUS_FAILURE);
    if (send_ll_bin(&crc, sizeof (crc)))
        return (MSG_STATUS_FAILURE);
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
    const uint timeout = 1000;
    uint32_t crc_rx = 0;
    int timeout_count = 0;
    uint8_t  localbuf[4096];

    if (buf == NULL) {
        bufp = localbuf;
        buflen = sizeof (localbuf);
    }

    while (1) {
        uint ch = rx_rb_get();
        if ((int)ch == -1) {
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
                for (cur = 0; cur < pos - KS_MSG_HEADER_LEN; cur++) {
                    if (cur >= (buflen - 1)) {
                        printf("...\n");
                        break;
                    }
                    printf(" %02x", bufp[cur ^ 1]);
                }
                if (pos - KS_MSG_HEADER_LEN < len) {
                    printf(" [data short by %ld bytes]\n",
                           len - (pos - KS_MSG_HEADER_LEN));
                } else if (pos - KS_MSG_HEADER_LEN < len + 4) {
                    printf(" [CRC short by %ld bytes]\n",
                           len + 4 - (pos - KS_MSG_HEADER_LEN));
                } else {
                    printf("%08x got CRC???", crc_rx);
                }
                printf("\n");
#endif
                return (MSG_STATUS_NO_REPLY);
            }
            time_delay_msec(1);
            sched_yield();
            continue;
        }
// printf("[%x]", ch);
        if (flags & BIT(0)) {
            /* Capture raw data */
            if (pos < buflen)
                bufp[pos] = ch;
        } else {
            /* Just packet payload */
            if (pos - KS_MSG_HEADER_LEN < buflen)
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
                if (ch != sm_magic_b[pos])
                    pos = 0;
                else
                    pos++;
                break;
            case 8:  // Length phase 1
                len = ch;
                pos++;
                break;
            case 9:  // Length phase 2
                len |= (ch << 8);
                len_roundup = (len + 1) & ~1; // Round up for odd length
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
#if 0
#ifdef ENDIAN_IS_BIG
                    crc_rx = (crc_rx << 8) | ch;
#else
                    uint shift = 8 * (pos - len - KS_MSG_HEADER_LEN);
                    crc_rx |= (ch << shift);
#endif
#endif
                }
                if (pos == len_roundup + KS_MSG_HEADER_LEN + 3) {
                    /* Last byte of CRC */

                    if (flags & BIT(0)) {
                        /* Raw data receive */
                        if (pos >= buflen) {
                            printf("message len 0x%x > raw buflen 0x%x\n",
                                   pos, buflen);
                            return (MSG_STATUS_BAD_LENGTH);  // too large
                        }
                        crc = crc32s(0, bufp + sizeof (sm_magic), len + 4);
                        status = 0;
                    } else {
                        /* Regular data receive */
                        if ((pos - KS_MSG_HEADER_LEN - 3) > buflen) {
                            printf("message len 0x%x > buflen 0x%x\n",
                                   (uint)(pos - KS_MSG_HEADER_LEN - 3), buflen);
                            return (MSG_STATUS_BAD_LENGTH);  // too large
                        }
                        crc = crc32s(0, &len, 2);       // length

//
// DEBUG next steps for odd length (1 byte)
// Have STM32 print out CRC that it receives and compare against
// 9f202df9 and 39120626
//

                        crc = crc32s(crc, &status, 2);  // command
#if 0
                        if (len & 1) {
                            crc = crc32s(crc, bufp, len & ~1);   // data
printf("crc3=%08x\n", crc);
printf("final byte=%02x\n", bufp[len]);
                            crc = crc32(crc, bufp + len, 1);   // data
                        } else {
                            crc = crc32s(crc, bufp, len);   // data
                        }
#elif 1
                        crc = crc32s(crc, bufp, len);   // data
#endif
                    }
                    if (crc != crc_rx) {
                        printf("Rx CRC %08x != expected %08x\n", crc, crc_rx);
#if 1
                        uint pos;
                        printf(" status=%04x len=%04x\n", status, len);
                        for (pos = 0; pos < ARRAY_SIZE (sm_magic); pos++)
                            printf(" %04x", sm_magic[pos]);
                        printf(" %04x %04x", len, status);
                        for (pos = 0; pos < len; pos += 2) {
                            printf(" %04x", SWAP16(*(uint16_t *)(bufp + pos)));
                        }
                        printf(" %04x %04x\n", crc_rx >> 16, crc_rx & 0xffff);
#endif
                        return (MSG_STATUS_BAD_CRC);
                    }
                    if (rxlen != NULL)
                        *rxlen = len;
                    if (rxstatus != NULL)
                        *rxstatus = status;
                    return (MSG_STATUS_SUCCESS);
                    pos = 0;
                } else {
                    pos++;
                }
                break;
        }
    }
}

static uint
send_ks_cmd(uint cmd, void *txbuf, uint txlen, void *rxbuf, uint rxmax,
            uint *rxstatus, uint *rxlen, uint flags)
{
    uint rc;
    rc = send_ks_cmd_core(cmd, txlen, txbuf);
    if (rc != 0)
        return (rc);
    return (recv_ks_reply_core(rxbuf, rxmax, flags, rxstatus, rxlen));
}

static void
show_ks_inquiry(void)
{
    smash_id_t id;
    uint status;
    uint rc;
    rc = send_ks_cmd(KS_CMD_ID, NULL, 0, &id, sizeof (id), &status, NULL, 0);
    if (rc != 0) {
        printf("KS send message failed: %d (%s)\n", rc, smash_err(rc));
        return;
    }
    if (status != 0) {
        printf("KS message failure: %d (%s)\n", status, smash_err(status));
        return;
    }

    printf("ID\n");
    printf("  Kicksmash %u.%u built %02u%02u-%02u-%02u %02u:%02u:%02u\n",
           SWAP16(id.si_ks_version[0]), SWAP16(id.si_ks_version[1]),
           id.si_ks_date[0], id.si_ks_date[1],
           id.si_ks_date[2], id.si_ks_date[3],
           id.si_ks_time[0], id.si_ks_time[1], id.si_ks_time[2]);
    printf("  USB %08x  Serial \"%s\"  Name \"%s\"\n",
           SWAP32(id.si_usbid), id.si_serial, id.si_name);
    printf("  Mode: %s\n",
           (id.si_mode == 0) ? "32-bit" :
           (id.si_mode == 1) ? "16-bit" :
           (id.si_mode == 2) ? "16-bit high" : "unknown");
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

#define SEND_MSG_MAX 2000

/*
 * send_msg
 * --------
 * Sends a message to the remote Amiga
 */
static uint
send_msg(void *buf, uint len, uint *status)
{
    uint rc;
    uint8_t msgbuf[SEND_MSG_MAX];
    uint sendlen = len;
    uint bodylen;
    uint pos;

    mem16_swap(buf, len);
    if (sendlen > SEND_MSG_MAX)
        sendlen = SEND_MSG_MAX;
    rc = send_ks_cmd(KS_CMD_MSG_SEND, buf, sendlen, NULL, 0, status, NULL, 0);
    if (rc == 0) {
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
            if (bodylen > len - pos)
                bodylen = len - pos;
            memcpy(msgbuf + sizeof (km_msg_hdr_t),
                   (uint8_t *)buf + pos, bodylen);
            sendlen = bodylen + sizeof (km_msg_hdr_t);
            rc = send_ks_cmd(KS_CMD_MSG_SEND, msgbuf, sendlen, NULL, 0,
                             status, NULL, 0);
            if (rc != 0) {
                printf("send msg failed at %x of %x\n", pos, len);
                break;
            }
            pos += bodylen;
#undef DEBUG_SEND_MSG
#ifdef DEBUG_SEND_MSG
            printf("send %x (body=%x) pos=%x of %x\n",
                   sendlen, bodylen, pos, len);
#endif
        }
    }

    mem16_swap(buf, len);
    return (rc);
}

/*
 * recv_msg
 * --------
 * Receives a message from the remote Amiga
 */
static uint
recv_msg(void *buf, uint bufsize, uint *rx_status, uint *rx_len)
{
    uint rc;
    rc = send_ks_cmd(KS_CMD_MSG_RECEIVE, NULL, 0, buf, bufsize,
                     rx_status, rx_len, 0);
    if (rc == 0)
        mem16_swap(buf, *rx_len);
    return (rc);
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
        printf("KS send message failed: %d (%s)\n", rc, smash_err(rc));
        return (rc);
    }
    return (MSG_STATUS_SUCCESS);
}

/*
 * time_offset
 * -----------
 * Converts a UNIX-style seconds since 1970 (GMT) value to Amiga-style
 * seconds since 1970 (local time) value.
 */
time_t
time_offset(time_t rawtime)
{
#define UNIX_SEC_TO_AMIGA_SEC (2922 * 24 * 60 * 60)  // 1978 - 1970 = 2922 days
    struct tm *ptm;
    if (rawtime >= UNIX_SEC_TO_AMIGA_SEC)
        rawtime -= UNIX_SEC_TO_AMIGA_SEC;
    ptm = localtime(&rawtime);
    return (rawtime + ptm->tm_gmtoff);
}

static uint
get_relative_path(handle_ent_t **parent, const char *name, char *pathbuf)
{
    char pathname[4096];
    const char *nptr = name;
    const char *nstart;
    char *tptr;
    char *colon;
    char *slash;

    if ((*parent != NULL) && ((*parent)->he_type == HM_TYPE_VOLDIR)) {
        /* Root of all volumes (Volume Directory) */
        *parent = NULL;
    }
    if ((nptr[0] == ':') && (nptr[1] == ':')) {
        /* Root of all volumes (Volume Directory) */
        *parent = NULL;
        nptr += 2;
    }
    nstart = nptr;
    if (nptr[0] == ':') {
        /* Root of current volume */
        if (*parent != NULL)
            *parent = (*parent)->he_volume;
        nptr++;
        nstart++;
    } else {
        colon = strchr(nstart, ':');
        slash = strchr(nstart, '/');
        if ((colon != NULL) && ((slash == NULL) || (colon < slash))) {
            /* Got a volume name ending in colon */
            uint len = colon - nstart + 1;
            memcpy(pathname, nstart, colon - nstart + 1);
            pathname[len] = '\0';
#undef REL_PATH_DEBUG
#ifdef REL_PATH_DEBUG
            printf("find vol name %s\n", pathname);
#endif
            *parent = handle_get_name(pathname);
            if (*parent != NULL) {
                nptr += len;
            }
        }
    }

    /* Parent path */
    tptr = pathname;
    if ((*parent != NULL) && ((*parent)->he_type != HM_TYPE_VOLUME)) {
        strcpy(tptr, (*parent)->he_name);
        tptr += strlen(tptr);
        if ((tptr > pathname) && (tptr[-1] != '/'))
            *(tptr++) = '/';
    }

    /* Build the new path */
    while (1) {
        if ((*nptr == '/') || (*nptr == '\0')) {
            /* End of a path element */
            if (*parent == NULL) {
                /* Seek volume name */
                tptr[0] = ':';
                tptr[1] = '\0';
                if (((pathname[0] == ':') && (pathname[1] == '\0')) ||
                    ((pathname[0] == '.') && (pathname[1] == ':') &&
                     (pathname[2] == '\0')) ||
                    ((pathname[0] == '.') && (pathname[1] == '.') &&
                     (pathname[2] == ':') && (pathname[3] == '\0'))) {
                    /* Got "" or "." or ".." at volume directory */
                    tptr = pathname;
                    goto get_rel_restart;
                }
#ifdef REL_PATH_DEBUG
                printf("Seek vol name %s\n", pathname);
#endif
                *parent = handle_get_name(pathname);
                if (*parent == NULL)
                    return (1);
                /* Got volume; start again with path name */
get_rel_restart:
                tptr = pathname;
get_rel_next:
                if (*nptr == '\0')
                    break;
                nptr++;
                continue;
            } else if ((*nptr == '/') && (tptr == pathname)) {
                /* Consume leading / */
#ifdef REL_PATH_DEBUG
                printf("saw /\n");
#endif
                goto get_rel_next;
            } else if ((tptr > pathname) && (tptr[-1] == '.') &&
                       ((tptr == pathname + 1) || (tptr[-2] == '/'))) {
                /* Consume ./ meaning "same directory" */
#ifdef REL_PATH_DEBUG
                printf("saw ./\n");
#endif
                tptr--;
                goto get_rel_next;
            } else if ((tptr > pathname) && (tptr[-1] == '/') &&
                                               (*nptr == '/')) {
                /* Consume // meaning "up a directory" for Amiga */
#ifdef REL_PATH_DEBUG
                printf("saw //\n");
#endif
                goto get_rel_trim_dotdot;
            } else if ((tptr > pathname + 1) &&
                       (tptr[-1] == '.') && (tptr[-2] == '.') &&
                       ((tptr == pathname + 2) || (tptr[-3] == '/'))) {
                /* Consume .. and previous path element */
#ifdef REL_PATH_DEBUG
                printf("saw ..\n");
#endif
                tptr -= 2;
get_rel_trim_dotdot:
                if ((tptr > pathname) && (--tptr > pathname)) {
                    for (tptr--; tptr > pathname; tptr--) {
                        if (*tptr == '/')
                            break;
                    }
                    if ((*tptr == '/') && (tptr > pathname))
                        tptr++;  // went too far
                }
                goto get_rel_next;
            }
        }
        if (*nptr == '\0')
            break;
        *(tptr++) = *(nptr++);
    }
    *tptr = '\0';

    strcpy(pathbuf, pathname);
#ifdef REL_PATH_DEBUG
    printf("got relative path %s from '%s' and '%s'\n",
           pathbuf, name, *parent ? (*parent)->he_name : "NULL");
#endif
    return (0);
}

/*
 * merge_host_paths is used to build a final path for file open.
 * It takes two paths and simply merges them together, inserting
 * a slash in the middle as appropriate.
 */
char *
merge_host_paths(const char *base, const char *append)
{
    char pathbuf[KS_PATH_MAX];
    uint len = strlen(base);
// printf("base=%s len=%u append=%s\n", base, len, append);
    if (len == 0)
        return (strdup(append));
    if (strlen(append) == 0)
        return (strdup(base));
    strcpy(pathbuf, base);
    if (base[len - 1] != '/')
        pathbuf[len++] = '/';
    strcpy(pathbuf + len, append);
    return (strdup(pathbuf));
}

char *
merge_amiga_paths(const char *base, const char *append)
{
    char pathbuf[2048];
    uint len;

    if (base == NULL)
        return (strdup(append));

    len = strlen(base);
    if (len == 0)
        return (strdup(append));
    if (strlen(append) == 0)
        return (strdup(base));
    strcpy(pathbuf, base);
    if ((base[len - 1] != '/') && (base[len - 1] != ':'))
        pathbuf[len++] = '/';
    strcpy(pathbuf + len, append);
    return (strdup(pathbuf));
}

static uint
st_mode_to_hm_type(uint st_mode)
{
    uint hm_type;
    switch (st_mode & S_IFMT) {
        case S_IFBLK:
            hm_type = HM_TYPE_BDEV;
            break;
        case S_IFCHR:
            hm_type = HM_TYPE_CDEV;
            break;
        case S_IFDIR:
            hm_type = HM_TYPE_DIR;
            break;
        case S_IFIFO:
            hm_type = HM_TYPE_FIFO;
            break;
        case S_IFLNK:
            hm_type = HM_TYPE_LINK;
            break;
        case S_IFREG:
            hm_type = HM_TYPE_FILE;
            break;
        case S_IFSOCK:
            hm_type = HM_TYPE_SOCKET;
            break;
        default:
            printf("unknown dir type(%x)\n", st_mode & S_IFMT);
            hm_type = HM_TYPE_UNKNOWN;
            break;
    }
    return (hm_type);
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
    /* See comments in amiga_perms_from_host() for more information */

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
            ((amiga_perms & FIBF_SCRIPT)      ? S_ISVTX : 0);

    /* No place to capture AMIGA_PERMS_ARCHIVE */
    return (perms);
}

static uint
errno_to_km_status(void)
{
    switch (errno) {
        case EACCES:
        case EBUSY:
        case EFAULT:
        case EPERM:
        case EROFS:
            return (KM_STATUS_PERM);
        case EBADF:
        case EINVAL:
        case EISDIR:
            return (KM_STATUS_INVALID);
        case EEXIST:
            return (KM_STATUS_EXIST);
        case ENOENT:
            return (KM_STATUS_NOEXIST);
        case ENOTEMPTY:
            return (KM_STATUS_NOTEMPTY);
        default:
            printf("errno=%d\n", errno);
            return (KM_STATUS_FAIL);
    }
}

static uint
sm_unknown(km_msg_hdr_t *km, uint *status)
{
    printf("KS unexpected op %x\n", km->km_op);
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
#if 0
    printf("lb l=%x s=%04x data=%02x %02x\n",
           rxlen, status, rxdata[0], rxdata[1]);
#endif
    return (send_msg(rxdata, rxlen, status));
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
sm_fopen(hm_fopenhandle_t *hm, uint *status)
{
    char         *hm_name = (char *)(hm + 1);
    handle_ent_t *phandle = handle_get(hm->hm_handle);
    handle_ent_t *handle;
    char         *host_path = NULL;
    char          name[KS_PATH_MAX];
    uint16_t      hm_type;
    uint16_t      hm_mode = SWAP16(hm->hm_mode);
    uint          oflags;
    int           fd;
    struct stat   st;

    printf("fopen(%s) in %d\n", hm_name, hm->hm_handle);

    hm->hm_hdr.km_op |= KM_OP_REPLY;
    hm->hm_hdr.km_status = KM_STATUS_OK;

#ifdef FOPEN_DEBUG
    printf("parent handle=%d type=%x path=%s\n",
           hm->hm_handle, phandle ? phandle->he_type: 0,
           phandle ? phandle->he_name : NULL);
#endif
    if (get_relative_path(&phandle, hm_name, name)) {
        printf("fopen(%s) relative path failed\n", hm_name);
reply_open_fail:
        hm->hm_handle = 0;
        if (hm->hm_hdr.km_status == KM_STATUS_OK)
            hm->hm_hdr.km_status = KM_STATUS_FAIL;
        return (send_msg(hm, sizeof (*hm), status));
    }
    if (phandle == NULL) {
        /* Opening the volume directory */
        if ((hm_mode & ~HM_MODE_DIR) != HM_MODE_READ) {
            printf("Did not open volume directory for read (%x)\n",
                   hm_mode);
            goto reply_open_fail;
        }
        hm->hm_type = SWAP16(HM_TYPE_DIR);

        handle = handle_new(name, NULL, HM_TYPE_VOLDIR, hm_mode);
        /* Volume directory has no he_volume or he_avolume pointers */

        hm->hm_hdr.km_status = KM_STATUS_OK;
        hm->hm_handle = handle->he_handle;
        hm->hm_mode   = 0;
        return (send_msg(hm, sizeof (*hm), status));
    }
    if (phandle->he_avolume != NULL)  {
        host_path = merge_host_paths(phandle->he_avolume->av_path, name);
    } else {
        host_path = name;
    }
printf("host_path=%s\n", host_path);

    if (hm_mode & HM_MODE_WRITE) {
        hm_type = SWAP16(hm->hm_type);
    }
    if ((hm_mode & ~HM_MODE_DIR) & HM_MODE_READ) {
        if (lstat(host_path, &st) != 0) {
            printf("fopen(%s) stat failed errno=%d\n", host_path, errno);
            goto reply_open_fail;
        }
        hm_type = st_mode_to_hm_type(st.st_mode);
        hm->hm_type = SWAP16(hm_type);
    }

    if (hm_mode & HM_MODE_DIR) {
        if ((hm_mode & ~HM_MODE_DIR) != HM_MODE_READ) {
            printf("Did not open dirent %s for read (%x)\n",
                   host_path, hm_mode);
            goto reply_open_fail;
        }
        /*
         * Special mode which returns directory information for
         * a single file.
         */
        printf("dirmode phandle=%p %d  name=%s\n", (void *)phandle,
               (phandle != NULL) ? phandle->he_handle : -1, name);
        handle = handle_new(name, phandle, hm_type, hm_mode);
        printf("dirmode handle %u avolume=%s\n",
               handle->he_handle, handle->he_avolume->av_volume);

        hm->hm_hdr.km_status = KM_STATUS_OK;
        hm->hm_handle = handle->he_handle;
        hm->hm_mode   = 0;
        return (send_msg(hm, sizeof (*hm), status));
    } else if (hm_type == HM_TYPE_DIR) {
        DIR *dir;
        if ((hm_mode & ~HM_MODE_DIR) != HM_MODE_READ) {
            printf("Did not open dir %s for read (%x)\n",
                   host_path, hm_mode);
            goto reply_open_fail;
        }
        dir = opendir(host_path);
        if (dir == NULL) {
            printf("opendir(%s) failed\n", host_path);
            goto reply_open_fail;
        }

        handle = handle_new(name, phandle, hm_type, hm_mode);
        handle->he_dir = dir;

        hm->hm_hdr.km_status = KM_STATUS_OK;
        hm->hm_handle = handle->he_handle;
        hm->hm_mode   = 0;
        return (send_msg(hm, sizeof (*hm), status));
    }
    switch (hm_mode & HM_MODE_RDWR) {
        case HM_MODE_READ:
            oflags = O_RDONLY;
            break;
        case HM_MODE_WRITE:
            oflags = O_WRONLY;
            break;
        case HM_MODE_RDWR:
            oflags = O_RDWR;
            break;
        default:
            oflags = 0;
            break;
    }
    if (hm_mode & HM_MODE_APPEND)
        oflags |= O_APPEND;
    if (hm_mode & HM_MODE_CREATE)
        oflags |= O_CREAT;
    if (hm_mode & HM_MODE_TRUNC)
        oflags |= O_TRUNC;

    if (oflags & O_CREAT) {
        uint32_t aperms = SWAP32(hm->hm_aperms);
        uint mode = host_perms_from_amiga(aperms);
printf("O_CREAT %s oflags=%x mode=%x\n", host_path, oflags, mode);
        fd = open(host_path, oflags, mode);
    } else {
        fd = open(host_path, oflags);
        if ((fd == -1) && (oflags & HM_MODE_WRITE)) {
            fd = open(host_path, oflags | O_CREAT, 0777);
        }
    }
    if (fd == -1) {
        printf("File open %s fail: %d\n", host_path, errno);
        hm->hm_hdr.km_status = errno_to_km_status();
        goto reply_open_fail;
    }

    handle = handle_new(name, phandle, hm_type, hm_mode);
    handle->he_fd = fd;

    hm->hm_hdr.km_status = KM_STATUS_OK;
    hm->hm_handle = handle->he_handle;
    hm->hm_mode   = 0;
    return (send_msg(hm, sizeof (*hm), status));
}

static uint
sm_fclose(hm_fopenhandle_t *hm, uint *status)
{
    handle_ent_t *handle = handle_get(hm->hm_handle);

#define DEBUG_CLOSE
#ifdef DEBUG_CLOSE
    printf("fclose(%d)\n", hm->hm_handle);
#endif
    hm->hm_hdr.km_status = KM_STATUS_OK;
    hm->hm_hdr.km_op |= KM_OP_REPLY;

    if (handle == NULL) {
        printf("Handle %d not open for close\n", hm->hm_handle);
        hm->hm_hdr.km_status = KM_STATUS_FAIL;
        return (send_msg(hm, sizeof (*hm), status));
    }

    if (handle->he_mode & HM_MODE_DIR) {
#ifdef DEBUG_CLOSE
        printf("Close STAT\n");
#endif
        goto sm_fclose_end;
    }
    switch (handle->he_type) {
        case HM_TYPE_VOLDIR:
#ifdef DEBUG_CLOSE
            printf("close volume directory\n");
#endif
            break;
        case HM_TYPE_VOLUME:
#ifdef DEBUG_CLOSE
            printf("close volume\n");
#endif
            break;
        case HM_TYPE_DIR:
            if (handle->he_dir == NULL) {
                printf("BUG: attempt close of NULL dir: %s\n",
                       handle->he_name);
                break;
            }
#ifdef DEBUG_CLOSE
            printf("close dir\n");
#endif
            closedir(handle->he_dir);
            break;
        default:
            close(handle->he_fd);
            break;
    }
sm_fclose_end:
    handle_free(hm->hm_handle);
    return (send_msg(hm, sizeof (*hm), status));
}

static uint
sm_fread(hm_freadwrite_t *hm, uint *status)
{
    hm_freadwrite_t *hmr;
    uint             rc;
    uint             pos;
    uint             len;
    uint             hm_length = SWAP32(hm->hm_length);
    handle_ent_t    *handle = handle_get(hm->hm_handle);
    uint             pathlen = 0;
    char             pathbuf[2048];

    hm->hm_hdr.km_op |= KM_OP_REPLY;

    printf("fread(%d, l=%x)\n", hm->hm_handle, hm_length);
    if (handle == NULL) {
        printf("handle get %d failed\n", hm->hm_handle);
reply_read_fail:
        if (hm->hm_hdr.km_status == KM_STATUS_OK)
            hm->hm_hdr.km_status = KM_STATUS_FAIL;
        return (send_msg(hm, sizeof (*hm), status));
    }
    if ((handle->he_mode & HM_MODE_READ) == 0) {
        printf("%s not opened for read mode: %x\n",
               handle->he_name, handle->he_mode);
        hm->hm_hdr.km_status = KM_STATUS_INVALID;
        goto reply_read_fail;
    }

    if (handle->he_type == HM_TYPE_DIR) {
        printf("DIR %s\n", handle->he_name);
        goto dir_read_common;
    } else if (handle->he_type == HM_TYPE_VOLDIR) {
        printf("VOLDIR %s\n", handle->he_name);
        goto dir_read_common;
    } else if (handle->he_mode & HM_MODE_DIR) {
        printf("STAT %s\n", handle->he_name);
dir_read_common:
        pathlen = strlen(handle->he_name);
        if (pathlen > sizeof (pathbuf) - 257) {
            printf("Path too long: %u bytes\n", pathlen);
            goto reply_read_fail;
        }
        strcpy(pathbuf, handle->he_name);
        if ((handle->he_mode & HM_MODE_DIR) &&
            (handle->he_type != HM_TYPE_VOLDIR)) {
            char *sname = pathbuf;
            /* Trim file name from path */
            while (*sname != '\0')
                sname++;
            for (sname--; sname > pathbuf; sname--)
                if (*sname == '/')
                    break;
                else
                    *sname = '\0';
            pathlen = strlen(pathbuf);
        } else if ((pathlen > 1) && (pathbuf[pathlen - 1] != '/')) {
            pathbuf[pathlen++] = '/';  // Append trailing slash
            pathbuf[pathlen] = '\0';
        }
#ifdef DEBUG_READ
        printf("STAT pathbuf=%s\n", pathbuf);
#endif
    }
    hmr = malloc(sizeof(*hmr) + hm_length);
    pos = 0;
    rc = 0;
    while (pos < hm_length) {
        char *ndata = ((char *)(hmr + 1)) + pos;
        len = hm_length - pos;
        if ((handle->he_type == HM_TYPE_DIR) ||
            (handle->he_type == HM_TYPE_VOLDIR) ||
            (handle->he_mode & HM_MODE_DIR)) {
            struct dirent *dp;
            char *nptr;
            uint nlen;
            uint hmd_type;
            uint32_t size_hi = 0;
            uint32_t size_lo = 0;
            uint32_t amiga_perms;
            struct stat st;
            static struct dirent ldp;
            hm_fdirent_t *hm_dirent = (hm_fdirent_t *)ndata;
            uint maxlen = hm_length - pos;
            char *host_path = NULL;

            if ((sizeof (*hm_dirent) +
                 sizeof (dp->d_name) + 2 > maxlen) && (pos > 0)) {
                rc = 0;  // next entry might not fit
#ifdef DEBUG_READ
                printf("No space for next dirent %lu %u\n",
                       sizeof (*dp), maxlen);
#endif
                break;
            }

            if (handle->he_type == HM_TYPE_VOLDIR) {
                amiga_vol_t *vol;

                vol = volume_get_by_index(handle->he_entnum);
                if (vol == NULL) {
                    dp = NULL;
                } else {
                    handle->he_entnum++;
                    dp = &ldp;
                    dp->d_type = DT_DIR;
                    strcpy(dp->d_name, vol->av_volume);
                    if (handle->he_mode & HM_MODE_DIR) {
                        if (handle->he_entnum == 1) {
                            strcpy(dp->d_name, "Volume Directory");
                        } else {
                            dp = NULL;
                        }
                    }
                }
            } else if (handle->he_mode & HM_MODE_DIR) {
                /* This mode is to STAT a single file */
                if (handle->he_entnum != 0) {
                    dp = NULL;
                } else {
                    const char *sname;

                    handle->he_entnum++;
                    if ((handle->he_name[0] == '\0') ||
                        ((handle->he_name[0] == '.') &&
                         (handle->he_name[1] == '\0'))) {
                        /* Volume root */
                        sname = handle->he_avolume->av_volume;
                        handle->he_type = HM_TYPE_VOLDIR;
                        hmd_type = HM_TYPE_VOLDIR;
                    } else {
                        /* Find name following directory path */
                        sname = handle->he_name;

                        while (*sname != '\0')
                            sname++;
                        if ((sname > handle->he_name) &&
                            (sname[-1] == '/')) {
                            /* Name is before trailing slash */
                            sname--;
                        }
                        for (sname--; sname > handle->he_name; sname--)
                            if (sname[-1] == '/')
                                break;
                    }
                    dp = &ldp;
                    dp->d_type = DT_REG;
                    dp->d_ino = 0;
                    strcpy(dp->d_name, sname);
                }
            } else {
                dp = readdir(handle->he_dir);
            }
            if (dp == NULL) {
                rc = KM_STATUS_EOF;  // end of directory
                break;
            }
            switch (dp->d_type) {
                default:
                case DT_UNKNOWN:
                    hmd_type = HM_TYPE_UNKNOWN;
                    break;
                case DT_FIFO:
                    hmd_type = HM_TYPE_FIFO;
                    break;
                case DT_CHR:
                    hmd_type = HM_TYPE_CDEV;
                    break;
                case DT_DIR:
                    hmd_type = HM_TYPE_DIR;
                    break;
                case DT_BLK:
                    hmd_type = HM_TYPE_BDEV;
                    break;
                case DT_REG:
                    hmd_type = HM_TYPE_FILE;
                    break;
                case DT_LNK:
                    hmd_type = HM_TYPE_LINK;
                    break;
                case DT_SOCK:
                    hmd_type = HM_TYPE_SOCKET;
                    break;
                case DT_WHT:
                    hmd_type = HM_TYPE_WHTOUT;
                    break;
            }
            if (handle->he_type == HM_TYPE_VOLDIR)
                hmd_type = HM_TYPE_VOLDIR;
            strcpy(pathbuf + pathlen, dp->d_name);
            if (hmd_type != HM_TYPE_VOLDIR) {
                /* he_avolume is NULL for the Volume Directory */
                amiga_vol_t *avol = handle->he_avolume;
                if (avol == NULL) {
                    printf("BUG: handle=%u he_avolume is NULL\n",
                           handle->he_handle);
                    break;
                }
                host_path = merge_host_paths(avol->av_path, handle->he_name);
                if ((handle->he_mode & HM_MODE_DIR) == 0) {
                    char *temp_path = host_path;
                    host_path = merge_host_paths(temp_path, dp->d_name);
                    free(temp_path);
                }
            }
            if (hmd_type == HM_TYPE_VOLDIR) {
                time_t utctime = time(NULL);
                time_t time_a  = time_offset(utctime);
                hm_dirent->hmd_atime = SWAP32(time_a);
                hm_dirent->hmd_ctime = SWAP32(time_a);
                hm_dirent->hmd_mtime = SWAP32(time_a);
                hmd_type = HM_TYPE_VOLUME;
            } else if (lstat(host_path, &st) == 0) {
                uint32_t time_a = time_offset(st.st_atime);
                uint32_t time_c = time_offset(st.st_ctime);
                uint32_t time_m = time_offset(st.st_mtime);
                hm_dirent->hmd_atime = SWAP32(time_a);
                hm_dirent->hmd_ctime = SWAP32(time_c);
                hm_dirent->hmd_mtime = SWAP32(time_m);
                size_hi = st.st_size >> 32;
                size_lo = (uint32_t) st.st_size;
                hmd_type = st_mode_to_hm_type(st.st_mode);
            } else {
                printf("lstat %s failed\n", host_path);
                hm_dirent->hmd_atime = 0;
                hm_dirent->hmd_ctime = 0;
                hm_dirent->hmd_mtime = 0;
                size_hi = 0;
                size_lo = 0;
            }

            amiga_perms = amiga_perms_from_host(st.st_mode);
            hm_dirent->hmd_aperms  = SWAP32(amiga_perms);
            hm_dirent->hmd_type    = SWAP16(hmd_type);
            hm_dirent->hmd_ino     = SWAP32(dp->d_ino);
            hm_dirent->hmd_size_hi = SWAP32(size_hi);
            hm_dirent->hmd_size_lo = SWAP32(size_lo);
            hm_dirent->hmd_rsvd[0] = 0;
            hm_dirent->hmd_rsvd[1] = 0;

            nptr = (char *) (hm_dirent + 1);
            nlen = strlen(dp->d_name) + 1;  // Include NIL
            memcpy(nptr, dp->d_name, nlen);
            if (hmd_type == HM_TYPE_LINK) {
                char lbuf[1024];
                int llen;
                llen = readlink(host_path, lbuf, sizeof (lbuf) - 1);
                if (llen == -1) {
                    printf("readlink %s failed\n", host_path);
                    llen = 0;
                }
                lbuf[llen++] = '\0';
                memcpy(nptr + nlen, lbuf, llen);

                /* Fill comment with link information */
                nlen += llen;
            } else {
                nptr[nlen++] = '\0';            // Comment NIL
            }
            if (nlen & 1)                   // Round up
                nlen++;
#ifdef DEBUG_READ
            printf("dirent %u %s\n", nlen, nptr);
#endif
            hm_dirent->hmd_elen = SWAP16(nlen);
            pos += sizeof (*hm_dirent) + nlen;
            free(host_path);
        } else {
            /* Regular file */
            rc = read(handle->he_fd, ndata, len);
#ifdef DEBUG_READ
            printf("read %d bytes from fd=%d %s\n",
                   rc, handle->he_fd, handle->he_name);
#endif
            if (rc <= 0) {
                if (rc == 0) {
                    rc = KM_STATUS_EOF;
                } else {
                    rc = errno_to_km_status();
                }
                break;
            }
            pos += rc;
            rc = 0;
        }
    }
    if ((rc != KM_STATUS_OK) && (rc != KM_STATUS_EOF))
        printf("Returning odd rc=%d\n", rc);

    hmr->hm_hdr.km_op = hm->hm_hdr.km_op;
    hmr->hm_hdr.km_status = rc;
    hmr->hm_hdr.km_tag = hm->hm_hdr.km_tag;
    hmr->hm_handle = hm->hm_handle;
    hmr->hm_length = SWAP32(pos);
    if (0)
        dump_memory(hmr, sizeof (*hmr) + pos, VALUE_UNASSIGNED);
    rc = send_msg(hmr, sizeof (*hmr) + pos, status);
    free(hmr);
    return (rc);
}

static uint
sm_fwrite(hm_freadwrite_t *hm, uint rxlen, uint *status)
{
    uint             rc;
    uint             hm_length = SWAP32(hm->hm_length);
    handle_ent_t    *handle    = handle_get(hm->hm_handle);
    char            *ndata     = (char *)(hm + 1);

    printf("fwrite(%d, l=%x)\n", hm->hm_handle, hm_length);
    hm->hm_hdr.km_op |= KM_OP_REPLY;

    if (handle == NULL) {
        printf("handle get %d failed\n", hm->hm_handle);
reply_write_fail:
        if (hm->hm_hdr.km_status == KM_STATUS_OK)
            hm->hm_hdr.km_status = KM_STATUS_FAIL;
        return (send_msg(hm, sizeof (*hm), status));
    }
    if ((handle->he_mode & HM_MODE_WRITE) == 0) {
        printf("%s not opened for write mode: %x\n",
               handle->he_name, handle->he_mode);
        hm->hm_hdr.km_status = KM_STATUS_INVALID;
        goto reply_write_fail;
    }
    if ((handle->he_type == HM_TYPE_DIR) ||
        (handle->he_type == HM_TYPE_VOLDIR) ||
        (handle->he_mode & HM_MODE_DIR)) {
        printf("Can't write to directory\n");
        /*
         * XXX: Writing a file's dirent could be used as a method to
         *      change atime, ctime, mtime, aperms, ouid, ogid, and
         *      maybe even allow rename.
         *      Maybe require a field which specifies which other
         *      fields are to be updated.
         */
        hm->hm_hdr.km_status = KM_STATUS_INVALID;
        goto reply_write_fail;
    }

    if (rxlen >= sizeof (hm_freadwrite_t))
        rxlen -= sizeof (hm_freadwrite_t);
    else
        rxlen = 0;

    if (rxlen < hm_length) {
        /* More data pending */
        uint8_t *rdata = malloc(hm_length + sizeof (km_msg_hdr_t));
        uint8_t  rxdata[4096];
        uint     rdatapos = rxlen;
        uint     timeout = 0;

        if (rdata == NULL) {
            hm->hm_hdr.km_status = KM_STATUS_FAIL;
            return (send_msg(hm, sizeof (*hm), status));
        }
        memcpy(rdata, ndata, rxlen);
        ndata = (char *) ((km_msg_hdr_t *) rxdata + 1);
        while (rdatapos < hm_length) {
            uint rxmax = hm_length - rdatapos + sizeof (km_msg_hdr_t);
            rc = recv_msg(rxdata, rxmax, status, &rxlen);
            if (rc != RC_SUCCESS)
                break;
            if (rxlen == 0) {
                if (++timeout < 20)
                    continue;
                printf("fwrite(%d) data timeout at pos=%x\n",
                       hm->hm_handle, rdatapos);
                rc = RC_FAILURE;
                break;
            }
            timeout = 0;
            if (rxlen >= sizeof (km_msg_hdr_t))
                rxlen -= sizeof (km_msg_hdr_t);
            else
                rxlen = 0;
            if (((km_msg_hdr_t *)rxdata)->km_tag != hm->hm_hdr.km_tag) {
                printf("tag mismatch: %04x != expected %04x\n",
                       ((km_msg_hdr_t *)rxdata)->km_tag, hm->hm_hdr.km_tag);
                rc = RC_FAILURE;
                break;
            }
#if 0
printf("km_op=%x km_status=%x km_tag=%x rlen=%x ",
((km_msg_hdr_t *)rxdata)->km_op,
((km_msg_hdr_t *)rxdata)->km_status,
((km_msg_hdr_t *)rxdata)->km_tag, rxlen);
            printf("rdatapos=%x rxlen=%x\n", rdatapos, rxlen);
#endif
            memcpy(rdata + rdatapos, ndata, rxlen);
            rdatapos += rxlen;
        }
        if (rc == RC_SUCCESS) {
            rc = write(handle->he_fd, rdata, hm_length);
        }
        free(rdata);
    } else {
        rc = write(handle->he_fd, ndata, hm_length);
    }
    if (rc < 0) {
        printf("write rc=%d errno=%d\n", rc, errno);
        rc = errno_to_km_status();
    } else {
        rc = KM_STATUS_OK;
    }

    hm->hm_hdr.km_status = rc;
    return (send_msg(hm, sizeof (*hm), status));
}

static uint
sm_fseek(hm_fseek_t *hm, uint *status)
{
    handle_ent_t *handle = handle_get(hm->hm_handle);
    printf("fseek(%d, o=%lx)\n", hm->hm_handle,
           ((uint64_t) hm->hm_offset_hi << 32) | hm->hm_offset_lo);
    hm->hm_hdr.km_status = KM_STATUS_OK;
    if (handle == NULL) {
        printf("handle get %d failed\n", hm->hm_handle);
        if (hm->hm_hdr.km_status == KM_STATUS_OK)
            hm->hm_hdr.km_status = KM_STATUS_FAIL;
        return (send_msg(hm, sizeof (*hm), status));
    }
    if ((handle->he_type == HM_TYPE_VOLDIR) ||
        (handle->he_mode & HM_MODE_DIR)) {
        /* Can rewind volume dir or file stat */
        handle->he_entnum = 0;  // Can only rewind volume dir pointer
    } else if (handle->he_type == HM_TYPE_DIR) {
        /* Can rewind dir */
        rewinddir(handle->he_dir);
    } else if (handle->he_type == HM_TYPE_FILE) {
        uint32_t hi = SWAP32(hm->hm_offset_hi);
        uint32_t lo = SWAP32(hm->hm_offset_lo);
        off_t pos;
        off_t offset = ((uint64_t) hi << 32) | lo;
        pos = lseek(handle->he_fd, offset, SEEK_SET);
        if (pos < 0) {
            /* Seek failed */
            printf("Seek %u to %jd failed\n", hm->hm_handle, pos);
            hm->hm_hdr.km_status = KM_STATUS_FAIL;
        } else {
            hi = pos >> 32;
            lo = (uint32_t) pos;
            hm->hm_offset_hi = SWAP32(hi);
            hm->hm_offset_lo = SWAP32(lo);
        }
    } else {
        printf("Can't seek in file type %x\n", handle->he_type);
        hm->hm_hdr.km_status = KM_STATUS_INVALID;
    }
    hm->hm_hdr.km_op |= KM_OP_REPLY;
    return (send_msg(hm, sizeof (*hm), status));
}

static uint
sm_fcreate(hm_fopenhandle_t *hm, uint *status)
{
    handle_ent_t *phandle = handle_get(hm->hm_handle);
    char         *hm_name = (char *)(hm + 1);
    uint32_t      aperms  = SWAP32(hm->hm_aperms);
    uint32_t      umode   = host_perms_from_amiga(aperms);
    uint          hm_type = SWAP16(hm->hm_type);
    uint          dev     = SWAP16(hm->hm_mode);
    uint          ftype;
    char         *host_path = NULL;
    char          buf[KS_PATH_MAX];

    printf("fcreate(%s) type=%x perms=%x umode=%x in %x\n",
           hm_name, hm_type, hm->hm_mode, umode, hm->hm_handle);

    hm->hm_hdr.km_op |= KM_OP_REPLY;
    hm->hm_hdr.km_status = KM_STATUS_OK;

    if (get_relative_path(&phandle, hm_name, buf)) {
        printf("fcreate(%s) relative path failed\n", hm_name);
reply_create_fail:
        hm->hm_handle = 0;
        if (hm->hm_hdr.km_status == KM_STATUS_OK)
            hm->hm_hdr.km_status = KM_STATUS_FAIL;
        return (send_msg(hm, sizeof (*hm), status));
    }
    if (phandle == NULL) {
        /* Can't create the volume directory */
        printf("Can't create the volume directory\n");
        hm->hm_hdr.km_status = KM_STATUS_INVALID;
        goto reply_create_fail;
    }
    if (phandle->he_avolume != NULL)  {
        host_path = merge_host_paths(phandle->he_avolume->av_path, buf);
    } else {
        host_path = buf;
    }

    printf("host path=%s\n", host_path);
    switch (hm_type) {
        case HM_TYPE_FILE:   // Regular file
            ftype = S_IFREG;
create_node:
            if (mknod(host_path, ftype | umode, dev)) {
                hm->hm_hdr.km_status = errno_to_km_status();
                goto reply_create_fail;
            }
            break;
        case HM_TYPE_DIR:    // Directory
            if (mkdir(host_path, umode)) {
                hm->hm_hdr.km_status = errno_to_km_status();
                goto reply_create_fail;
            }
            break;
#ifdef ALLOW_CREATE_LINK
        case HM_TYPE_LINK: {   // Symbolic (soft) link
            uint  hm_name_len = strlen(hm_name) + 1;
            char *hm_lname = hm_name + hm_name_len;
            if (symlink(hm_lname, host_path)) {
                printf("symlink: %s -> %s failed: %d\n",
                       host_path, hm_lname, errno);
                hm->hm_hdr.km_status = errno_to_km_status();
                goto reply_create_fail;
            }
            break;
        }
        case HM_TYPE_HLINK: {  // Hard link
            uint  hm_name_len = strlen(hm_name) + 1;
            char *hm_lname = hm_name + hm_name_len;
            if (link(hm_lname, host_path)) {
                printf("hard link %s -> %s failed: %d\n",
                       host_path, hm_lname, errno);
                hm->hm_hdr.km_status = errno_to_km_status();
                goto reply_create_fail;
            }
            break;
        }
#endif /* ALLOW_CREATE_LINK */
        case HM_TYPE_BDEV:   // Block device
            ftype = S_IFBLK;
            goto create_node;
        case HM_TYPE_CDEV:   // Block device
            ftype = S_IFCHR;
            goto create_node;
        case HM_TYPE_FIFO:   // FIFO
            ftype = S_IFIFO;
            goto create_node;
        case HM_TYPE_SOCKET: // Socket
            ftype = S_IFSOCK;
            goto create_node;
        case HM_TYPE_WHTOUT: // Whiteout entry
        case HM_TYPE_VOLUME: // Disk volume
        case HM_TYPE_VOLDIR: // Volume directory
        default:
            hm->hm_hdr.km_status = KM_STATUS_INVALID;
            break;
    }

    return (send_msg(hm, sizeof (*hm), status));
}

static uint
sm_fdelete(hm_fhandle_t *hm, uint *status)
{
    handle_ent_t *phandle = handle_get(hm->hm_handle);
    char         *hm_name = (char *)(hm + 1);
    char         *host_path = NULL;
    char          buf[KS_PATH_MAX];
    struct stat   st;

    printf("fdelete(%s) in %d\n", hm_name, hm->hm_handle);

    hm->hm_hdr.km_op |= KM_OP_REPLY;
    hm->hm_hdr.km_status = KM_STATUS_OK;

    if (get_relative_path(&phandle, hm_name, buf)) {
        printf("fdelete(%s) relative path failed\n", hm_name);
reply_delete_fail:
        hm->hm_handle = 0;
        if (hm->hm_hdr.km_status == KM_STATUS_OK)
            hm->hm_hdr.km_status = KM_STATUS_FAIL;
        return (send_msg(hm, sizeof (*hm), status));
    }
    if (phandle == NULL) {
        /* Can't delete the volume directory */
        printf("Can't delete the volume directory\n");
        hm->hm_hdr.km_status = KM_STATUS_INVALID;
        goto reply_delete_fail;
    }
    if (phandle->he_avolume != NULL)  {
        host_path = merge_host_paths(phandle->he_avolume->av_path, buf);
    } else {
        host_path = buf;
    }

    if (lstat(host_path, &st) != 0) {
        printf("fdelete(%s) stat failed errno=%d\n", host_path, errno);
        goto reply_delete_fail;
    }
    switch (st.st_mode & S_IFMT) {
        case S_IFDIR:
            /* Use rmdir() */
            if (volume_get_by_path(host_path) != NULL) {
                printf("fdelete(%s) can't remove a volume\n", host_path);
                hm->hm_hdr.km_status = KM_STATUS_PERM;
                goto reply_delete_fail;
            }
            if (rmdir(host_path)) {
                /* Failed */
                printf("rmdir(%s) failed: %d\n", host_path, errno);
                hm->hm_hdr.km_status = errno_to_km_status();
                goto reply_delete_fail;
            }
            break;
        default:
        case S_IFBLK:   // Block device
        case S_IFCHR:   // Character device
        case S_IFIFO:   // FIFO (Pipe)
        case S_IFLNK:   // Symlink
        case S_IFREG:   // Regular file
        case S_IFSOCK:  // Socket
            /* Use unlink() */
            if (unlink(host_path)) {
                printf("unlink(%s) failed: %d\n", host_path, errno);
                hm->hm_hdr.km_status = errno_to_km_status();
                goto reply_delete_fail;
            }
            break;
    }
    return (send_msg(hm, sizeof (*hm), status));
}

static uint
sm_frename(hm_fhandle_t *hm, uint *status)
{
    handle_ent_t *phandle_old  = handle_get(hm->hm_handle);
    handle_ent_t *phandle_new  = phandle_old;
    char         *name_old     = (char *)(hm + 1);
    uint          len_old_name = strlen(name_old) + 1;
    char         *name_new     = name_old + len_old_name;
    char         *path_old;
    char         *path_new;
    char          buf_old[KS_PATH_MAX];
    char          buf_new[KS_PATH_MAX];

    printf("frename(%s to %s) in %x\n",
           name_old, name_new, hm->hm_handle);
    hm->hm_hdr.km_op |= KM_OP_REPLY;
    hm->hm_hdr.km_status = KM_STATUS_OK;

    if (get_relative_path(&phandle_old, name_old, buf_old)) {
        printf("frename(%s) relative path failed\n", name_old);
reply_rename_fail:
        hm->hm_handle = 0;
        if (hm->hm_hdr.km_status == KM_STATUS_OK)
            hm->hm_hdr.km_status = KM_STATUS_FAIL;
        return (send_msg(hm, sizeof (*hm), status));
    }
    if (phandle_old == NULL) {
        /* Can't rename the volume directory */
        printf("frename(%s) Can't rename the volume directory\n", name_old);
        hm->hm_hdr.km_status = KM_STATUS_INVALID;
        goto reply_rename_fail;
    }
    if (phandle_old->he_avolume != NULL)  {
        path_old = merge_host_paths(phandle_old->he_avolume->av_path, buf_old);
    } else {
        path_old = buf_old;
    }

    if (get_relative_path(&phandle_new, name_new, buf_new)) {
        printf("frename(%s) relative path failed\n", name_new);
        goto reply_rename_fail;
    }
    if (phandle_new == NULL) {
        /* Can't rename the volume directory */
        printf("frename(%s) Can't rename to the volume directory\n", name_new);
        hm->hm_hdr.km_status = KM_STATUS_INVALID;
        goto reply_rename_fail;
    }
    if (phandle_new->he_avolume != NULL)  {
        path_new = merge_host_paths(phandle_new->he_avolume->av_path, buf_new);
    } else {
        path_new = buf_new;
    }

    if (volume_get_by_path(path_old) != NULL) {
        printf("frename(%s) can't rename a volume\n", path_old);
        hm->hm_hdr.km_status = KM_STATUS_PERM;
        goto reply_rename_fail;
    }
    if (volume_get_by_path(path_new) != NULL) {
        printf("frename(%s) can't rename to a volume\n", path_new);
        hm->hm_hdr.km_status = KM_STATUS_PERM;
        goto reply_rename_fail;
    }
    if (rename(path_old, path_new)) {
        printf("rename %s to %s failed\n", path_old, path_new);
        hm->hm_hdr.km_status = errno_to_km_status();
        goto reply_rename_fail;
    }

    hm->hm_hdr.km_status = KM_STATUS_OK;
    return (send_msg(hm, sizeof (*hm), status));
}

static uint
sm_fpath(hm_fhandle_t *hm, uint *status)
{
    /* Resolve handle to Amiga-specific path */
    hm_fhandle_t *hmr;
    handle_ent_t *handle = handle_get(hm->hm_handle);
    char          pathbuf[2048];
    char          pathlen;
    uint          rc;

    printf("fpath(%d)\n", hm->hm_handle);
    hm->hm_hdr.km_status = KM_STATUS_OK;
    hm->hm_hdr.km_op |= KM_OP_REPLY;
    if (handle == NULL) {
        /* Volume directory */
        strcpy(pathbuf, "::");
    } else {
        char *host_path;
        handle_ent_t *volhandle = handle->he_volume;

        pathbuf[0] = '\0';
        if (volhandle != NULL) {
            amiga_vol_t *vol = volume_get_by_handle(volhandle);
            if (vol != NULL)
                strcpy(pathbuf, vol->av_volume);
        }
        host_path = merge_host_paths(pathbuf, handle->he_name);
        strcpy(pathbuf, host_path);
        free(host_path);
    }

    pathlen = strlen(pathbuf) + 2;
    hmr = malloc(sizeof(*hmr) + pathlen);
    memcpy(hmr, hm, sizeof (*hmr));
    strcpy((char *) (hmr + 1), pathbuf);

    rc = send_msg(hmr, sizeof (*hmr) + pathlen, status);
    free(hmr);
    return (rc);
}

static uint
sm_fset_perms(hm_fopenhandle_t *hm, uint *status)
{
    /* Resolve handle to Amiga-specific path */
    handle_ent_t *phandle = handle_get(hm->hm_handle);
    char         *name   = (char *)(hm + 1);
    char         *path;
    char          pathbuf[KS_PATH_MAX];
    uint32_t      aperms = SWAP32(hm->hm_aperms);
    uint          uperms;

    printf("fset_perms(%s %x)\n", name, aperms);
    hm->hm_hdr.km_op |= KM_OP_REPLY;

    if (get_relative_path(&phandle, name, pathbuf)) {
        printf("fset_perms(%s) relative path failed\n", name);
reply_set_perms_fail:
        hm->hm_handle = 0;
        if (hm->hm_hdr.km_status == KM_STATUS_OK)
            hm->hm_hdr.km_status = KM_STATUS_FAIL;
        return (send_msg(hm, sizeof (*hm), status));
    }
    if (phandle == NULL) {
        /* Can't set perms on the volume directory */
        printf("fset_perms(%s) can't set perms on the volume directory\n",
               name);
        hm->hm_hdr.km_status = KM_STATUS_INVALID;
        goto reply_set_perms_fail;
    }
    if (phandle->he_avolume != NULL)  {
        path = merge_host_paths(phandle->he_avolume->av_path, name);
    } else {
        path = name;
    }

    if (volume_get_by_path(path) != NULL) {
        printf("fset_perms(%s) can't set perms on a volume\n", path);
        hm->hm_hdr.km_status = KM_STATUS_PERM;
        goto reply_set_perms_fail;
    }
    uperms = host_perms_from_amiga(aperms);
    printf("uperms=%x %o\n", uperms, uperms);

    if (chmod(path, uperms)) {
        printf("chmod fail\n");
        hm->hm_hdr.km_status = errno_to_km_status();
        goto reply_set_perms_fail;
    }

    hm->hm_hdr.km_status = KM_STATUS_OK;
    return (send_msg(hm, sizeof (*hm), status));
}

static void
process_msg(uint status, uint8_t *rxdata, uint rxlen)
{
    km_msg_hdr_t *km = (km_msg_hdr_t *) rxdata;
    uint pos;
    uint rc;

#undef MSG_DEBUG
#ifdef MSG_DEBUG
    printf("  got msg %04x len=%04x op=%02x mstatus=%02x tag=%02x data ",
           status, rxlen, km->km_op, km->km_status, km->km_tag);
    for (pos = sizeof (*km); pos < 32; pos++) {
        printf(" %02x", rxdata[pos]);
    }
    printf("\n");
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

    switch (km->km_op) {
        case KM_OP_NULL:
            rc = sm_null(km, &status);
            break;
        case KM_OP_LOOPBACK:
            rc = sm_loopback(km, rxdata, rxlen, &status);
            break;
        case KM_OP_ID:
            rc = sm_id(km, &status);
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
            rc = sm_frename((hm_fhandle_t *)rxdata, &status);
            break;
        case KM_OP_FPATH:
            rc = sm_fpath((hm_fhandle_t *)rxdata, &status);
            break;
        case KM_OP_FSETPERMS:
            rc = sm_fset_perms((hm_fopenhandle_t *)rxdata, &status);
            break;
        default:
            rc = sm_unknown(km, &status);
            break;
    }
    if (rc == 0)
        rc = status;

    if (rc != 0)  {
        printf("KS send_msg failure op=%x status=%02x: %d (%s)\n",
               km->km_op, status, rc, smash_err(rc));
    }
}

static uint
handle_atou_messages(void)
{
    uint8_t   rxdata[4096];
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
        rc = recv_msg(rxdata, sizeof (rxdata), &status, &rxlen);

        if (rc != 0) {
            printf("KS recv_msg failure: %d (%s)\n", rc, smash_err(rc));
            return (rc);
        }
        if (status == KS_CMD_MSG_SEND) {
            process_msg(status, rxdata, rxlen);
            handled++;
        } else if ((status == KS_STATUS_NODATA) ||
                   (status == KS_STATUS_LOCKED)) {
            break;
        } else {
            printf("status=%04x\n", status);
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
    uint fstick = 0;
    uint count;
    uint16_t app_state = MSG_STATE_SERVICE_UP | MSG_STATE_HAVE_LOOPBACK;
    smash_msg_info_t mi;

    printf("Message mode\n");
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
        printf("KS Set App State failed: %d (%s)\n", rc, smash_err(rc));
        return;
    }

    rc = send_ks_cmd(KS_CMD_MSG_FLUSH, NULL, 0, NULL, 0, &status, NULL, 0);
    if (rc == 0)
        rc = status;
    if (rc != 0) {
        printf("KS Msg Flush failed: %d (%s)\n", rc, smash_err(rc));
        return;
    }

    while (1) {
        if (curtick != 0) {
            if (curtick < 1024)
                usleep(curtick);
            else
                time_delay_msec(curtick / 1024);
            fstick += (curtick / 1024) + 10;
        } else {
            fstick += 20;
        }
        if (fstick >= 5000) {
            fstick = 0;
            keep_app_state();  // do this once every 5 seconds
        }

        if (curtick > 1000) {
            rc = send_ks_cmd(KS_CMD_MSG_INFO, NULL, 0, &mi, sizeof (mi),
                             &status, NULL, 0);
            if (rc != 0) {
                printf("KS message failure: %d (%s)\n", rc, smash_err(rc));
                break;
            }
            if ((mi.smi_atou_inuse != 0) || (mi.smi_utoa_inuse != 0)) {
                printf("  atou inuse=%u avail=%u  utoa inuse=%u avail=%u\n",
                       SWAP16(mi.smi_atou_inuse), SWAP16(mi.smi_atou_avail),
                       SWAP16(mi.smi_utoa_inuse), SWAP16(mi.smi_utoa_avail));
            }
        }

        count = handle_atou_messages();
        if (count != 0) {
            /* Handled message */
            curtick = 0;
            fstick += count * 10;
        } else {
            /* No message */
            if (curtick == 0)
                curtick = 10;
            else if (curtick < 500000)
                curtick += curtick / 2;
        }
    }
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
run_mode(uint mode, uint bank, uint baseaddr, uint len, uint report_max,
         bool fill, const char *filename)
{
    if (mode == MODE_UNKNOWN) {
        warnx("You must specify one of: -e -i -r -t or -w");
        usage(stderr);
        return (1);
    }
    if (mode & MODE_TERM) {
        run_terminal_mode();
        return (0);
    }
    if (mode & MODE_ID) {
        eeprom_id();
        return (0);
    }
    if (mode & MODE_MSG) {
        run_message_mode();
        return (0);
    }
    if (((filename == NULL) || (filename[0] == '\0')) &&
        (mode & (MODE_READ | MODE_VERIFY | MODE_WRITE))) {
        warnx("You must specify a filename with -r or -v or -w option\n");
        usage(stderr);
        return (1);
    }

    if (bank != BANK_NOT_SPECIFIED) {
        if ((mode & MODE_READ) && (len == EEPROM_SIZE_NOT_SPECIFIED)) {
            warnx("You must specify a length with -r and -b together\n");
            usage(stderr);
            return (1);
        }
        if ((mode & MODE_ERASE) && (len == EEPROM_SIZE_NOT_SPECIFIED)) {
            warnx("You must specify a length with -e and -b together\n");
            usage(stderr);
            return (1);
        }
    }

    if (mode & MODE_READ) {
        eeprom_read(filename, bank, baseaddr, len);
        return (0);
    }
    if (mode & MODE_ERASE) {
        if (eeprom_erase(bank, baseaddr, len))
            return (1);
    }

    if (mode & (MODE_WRITE | MODE_VERIFY)) {
        struct stat statbuf;
        if (baseaddr == ADDR_NOT_SPECIFIED)
            baseaddr = 0x000000;  // Start of EEPROM

        if (stat(filename, &statbuf))
            errx(EXIT_FAILURE, "Failed to stat %s", filename);

        if (len == EEPROM_SIZE_NOT_SPECIFIED) {
            len = EEPROM_SIZE_DEFAULT;
            if (len > statbuf.st_size)
                len = statbuf.st_size;
        }
        if (len > statbuf.st_size) {
            errx(EXIT_FAILURE, "Length 0x%x is greater than %s size %jx",
                 len, filename, (intmax_t)statbuf.st_size);
        }
        if (bank != BANK_NOT_SPECIFIED)
            baseaddr += bank * len;

        do {
            if ((mode & MODE_WRITE) &&
                (eeprom_write(filename, baseaddr, len) != 0))
                return (1);

            if ((mode & MODE_VERIFY) &&
                (eeprom_verify(filename, baseaddr, len, report_max) != 0))
                return (1);

            baseaddr += len;
            if (baseaddr >= EEPROM_SIZE_DEFAULT)
                break;
        } while (fill);
    }
    return (0);
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
    char            *filename   = NULL;
    uint             mode       = MODE_UNKNOWN;
    struct sigaction sa;

    memset(&sa, 0, sizeof (sa));
    sa.sa_handler = sig_exit;
    (void) sigaction(SIGTERM, &sa, NULL);
    (void) sigaction(SIGINT,  &sa, NULL);
    (void) sigaction(SIGQUIT, &sa, NULL);
    (void) sigaction(SIGPIPE, &sa, NULL);

    device_name[0] = '\0';

    while ((ch = getopt_long(argc, argv, short_opts, long_opts,
                             &long_index)) != EOF) {
reswitch:
        switch (ch) {
            case ':':
                if ((optopt == 'v') && filename != NULL) {
errx(EXIT_FAILURE, "how did we get here?");
                    /* Allow -v to be specified at end to override write */
                    ch = optopt;
                    optarg = filename;
                    mode = MODE_UNKNOWN;
                    goto reswitch;
                }
                warnx("The -%c flag requires an argument", optopt);
                if (optopt == 's')
                    warnx("Valid options are 1032, 2301, or 3210\n");
                usage(stderr);
                exit(EXIT_FAILURE);
                break;
            case 'A':
                report_max = 0xffffffff;
                break;
            case 'a':
                if ((sscanf(optarg, "%i%n", (int *)&baseaddr, &pos) != 1) ||
                    (optarg[pos] != '\0') || (pos == 0)) {
                    errx(EXIT_FAILURE, "Invalid address \"%s\"", optarg);
                }
                break;
            case 'b':
                if ((sscanf(optarg, "%i%n", (int *)&bank, &pos) != 1) ||
                    (optarg[pos] != '\0') || (pos == 0)) {
                    errx(EXIT_FAILURE, "Invalid bank \"%s\"", optarg);
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
                if (mode != MODE_UNKNOWN)
                    errx(EXIT_FAILURE,
                         "-%c may not be specified with any other mode", ch);
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
            case 'r':
                if (mode != MODE_UNKNOWN)
                    errx(EXIT_FAILURE,
                         "-%c may not be specified with any other mode", ch);
                mode = MODE_READ;
//              filename = optarg;
                break;
            case 's':
                if ((strcasecmp(optarg, "a3000") == 0) ||
                    (strcasecmp(optarg, "a4000") == 0) ||
                    (strcasecmp(optarg, "a3000t") == 0) ||
                    (strcasecmp(optarg, "a4000t") == 0) ||
                    (strcasecmp(optarg, "a1200") == 0)) {
                    swapmode = SWAPMODE_A3000;
                    break;
                }
                if ((strcasecmp(optarg, "a500") == 0) ||
                    (strcasecmp(optarg, "a600") == 0) ||
                    (strcasecmp(optarg, "a1000") == 0) ||
                    (strcasecmp(optarg, "a2000") == 0) ||
                    (strcasecmp(optarg, "cdtv") == 0)) {
                    swapmode = SWAPMODE_A500;
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
//              filename = optarg;
                break;
            case 'v':
                if (mode & (MODE_ID | MODE_READ | MODE_TERM))
                    errx(EXIT_FAILURE, "Only one of -irtv may be specified");
                mode |= MODE_VERIFY;
//              filename = optarg;
                break;
            case 'y':
                force_yes = TRUE;
                break;
            case 'h':
            case '?':
                usage(stdout);
                exit(EXIT_SUCCESS);
                break;
            default:
                warnx("Unknown option -%c 0x%x", ch, ch);
                usage(stderr);
                exit(EXIT_USAGE);
        }
    }

    argc -= optind;
    argv += optind;

    if (argc > 0) {
        filename = argv[0];
        argv++;
        argc--;
    }

    if (argc > 0)
        errx(EXIT_USAGE, "Too many arguments: %s", argv[0]);

    if (device_name[0] == '\0')
        find_mx_programmer();

    if (device_name[0] == '\0') {
        warnx("You must specify a device to open (-d <dev>)");
        usage(stderr);
        exit(EXIT_USAGE);
    }
    if (len == 0)
        errx(EXIT_USAGE, "Invalid length 0x%x", len);

    atexit(at_exit_func);

    if (serial_open(TRUE) != RC_SUCCESS)
        do_exit(EXIT_FAILURE);

    create_threads();
    rc = run_mode(mode, bank, baseaddr, len, report_max, fill, filename);
    wait_for_tx_writer();

    exit(rc);
}
