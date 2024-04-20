/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in August 2020.
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
"mxprog <opts> <dev>\n"
"    -A --all                show all verify miscompares\n"
"    -a --addr <addr>        starting EEPROM address\n"
"    -b --bank <num>         starting EEPROM address as multiple of file size\n"
"    -D --delay              pacing delay between sent characters (ms)\n"
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
"    mxprog -d /dev/cu.usbmodem* -i\n"
#else
"    mxprog -d /dev/ttyACM0 -i\n"
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
#define MX_DEVICE 0x1615

#define EEPROM_SIZE_DEFAULT       0x200000    // 2MB
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
         *     TERM_DEBUG=/dev/pts/4 mxprog -t
         *     TERM_DEBUG=/tmp/term_debug mxprog -t -d /dev/ttyACM0
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
 *     If the sender is mxprog, then it could also be user abort.
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
 *     If the sender is mxprog, then it could also be user abort.
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
    char        cmd_output[64];
    int         rxcount;
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

typedef struct amiga_vol {
    const char *av_volume;
    const char *av_path;
    struct amiga_vol *next;
} amiga_vol_t;
amiga_vol_t *amiga_vol_head = NULL;

static void
add_volume(const char *volume_name, const char *local_path)
{
    amiga_vol_t *node = malloc(sizeof (amiga_vol_t));
    node->av_volume = volume_name;
    node->av_path   = local_path;
    node->next      = amiga_vol_head;
    amiga_vol_head  = node;
    printf("add volume %s = %s\n", volume_name, local_path);
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

/*
 * send_msg
 * --------
 * Sends a message to the remote Amiga
 */
static uint
send_msg(void *buf, uint len, uint *status)
{
    uint rc;
    mem16_swap(buf, len);
    rc = send_ks_cmd(KS_CMD_MSG_SEND, buf, len, NULL, 0, status, NULL, 0);
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
    rc = send_ks_cmd(KS_CMD_APP_STATE | KS_APP_STATE_SET, app_state_send,
                     sizeof (app_state_send), NULL, 0, &status, NULL, 0);
    if (rc != 0) {
        printf("KS send message failed: %d (%s)\n", rc, smash_err(rc));
        return (rc);
    }
    return (MSG_STATUS_SUCCESS);
}

static void
process_msg(uint status, uint8_t *rxdata, uint rxlen)
{
    km_msg_hdr_t *km = (km_msg_hdr_t *) rxdata;
    uint      pos;
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
            km->km_status = 0;
            km->km_op |= KM_OP_REPLY;
            rc = send_msg(km, sizeof (*km), &status);
            goto check_fail;
        case KM_OP_LOOPBACK:
            /* Need to send back reply */
            km->km_status = 0;
            km->km_op |= KM_OP_REPLY;
// printf("lb l=%x s=%04x data=%02x %02x\n", rxlen, status, rxdata[0], rxdata[1]);
            rc = send_msg(rxdata, rxlen, &status);
check_fail:
            if (rc == 0)
                rc = status;
            if (rc != 0)  {
                printf("KS send_msg failure op=%x status=%02x: %d (%s)\n",
                       km->km_op, status, rc, smash_err(rc));
            }
            break;
        case KM_OP_ID: {
            smash_id_t *reply = (smash_id_t *) (km + 1);
            uint temp[3];
            int  pos = 0;

            km->km_status = 0;
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
            rc = send_msg(km, sizeof (*km) + sizeof (*reply), &status);
            goto check_fail;
        }
        case KM_OP_FILE:
            break;
        default:
            printf("KS unexpected op %x\n", km->km_op);
            break;
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
    uint16_t *msg;
    uint16_t app_state = APP_STATE_SERVICE_UP | APP_STATE_HAVE_LOOPBACK;
    smash_msg_info_t mi;

    printf("message mode\n");
    app_state_send[0] = SWAP16(0xffff);     // Affect all bits
    app_state_send[1] = SWAP16(app_state);  // Message service up

    if (send_cmd("prom service"))
        return; // "timeout" was reported in this case

    show_ks_inquiry();

    rc = send_ks_cmd(KS_CMD_APP_STATE | KS_APP_STATE_SET, app_state_send,
                     sizeof (app_state_send), buf, sizeof (buf), &status,
                     &rxlen, 1);
    if (rc == 0)
        rc = status;
    if (rc != 0) {
        printf("KS Set App State failed: %d (%s)\n", rc, smash_err(rc));
        return;
    }

    msg = (uint16_t *)(buf + 8);
    printf("got %u bytes: len=%04x status=%04x %04x %04x\n",
           rxlen, msg[0], msg[1], msg[2], msg[3]);

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
//printf("%u\n", curtick);
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

        if (lstat(filename, &statbuf))
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
 * main() is the entry point of the mxprog utility.
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
            case 'm': {
                mode = MODE_MSG;
                if ((optind >= argc) || (argv[optind] == NULL)) {
                    errx(EXIT_FAILURE, "-m requires both am Amiga volume name "
                         "and local path to mount.\nExample: -m ks: .");
                }
                add_volume(optarg, argv[optind]);
                optind++;
                break;
            }
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
