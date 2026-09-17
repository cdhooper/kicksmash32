#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#ifdef __MINGW32__
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <io.h>         /* _setmode, _fileno */
#include <fcntl.h>      /* _O_BINARY */
typedef unsigned int uint;
#ifndef IFNAMSIZ
#define IFNAMSIZ 256    /* Npcap adapter identifiers can be long GUIDs */
#endif
#else
#include <sys/types.h>  /* uint */
#include <poll.h>
#include <net/if.h>     /* IFNAMSIZ */
#endif
#include "netif_backend.h"
#include "hostsmash_netif.h"

/*
 * After compiling, it's helpful to setuid root this program on Linux.
 * Otherwise, you will need to enter a password every time the network
 * stack starts up.
 *      sudo chown root objs.*\/hostsmash_netif
 *      sudo chmod 4755 objs.*\/hostsmash_netif
 *
 * An alternative might be to give it network admin permission:
 *      sudo setcap cap_dac_override,cap_net_admin+ep objs.*\/hostsmash_netif
 *
 * On Linux, default operation creates/destroys the macvtap via netlink
 * (no external "ip" binary required). Pass --external (or -e) to
 * instead use the system("ip ...") path. This option has no effect on
 * other platforms.
 *
 * On macOS, this program elevates via a native administrator-privilege
 * prompt (osascript) and talks to the physical NIC directly via BPF
 * (/dev/bpfN) in promiscuous mode -- there is no macvtap equivalent.
 *
 * On Windows, this program elevates via a UAC prompt (ShellExecuteEx
 * "runas") and talks to the physical NIC directly via Npcap in
 * promiscuous mode -- again, there is no macvtap equivalent, and no
 * poll()-able fd for the capture handle, so the Windows build of
 * main() below uses a small thread-based loop instead of poll().
 *
 * Build with -DOSX on macOS so the OSX-specific branches below (only
 * the default-route detection needs one -- everything else platform-
 * specific lives behind netif_backend.h in netif_linux.c /
 * netif_macos_bpf.c / netif_windows.c) compile in, and link against
 * netif_macos_bpf.c instead of netif_linux.c. On Windows (__MINGW32__
 * is predefined by the cross-compiler), link against netif_windows.c.
 */

/* Global volatile flag for clean termination */
static volatile sig_atomic_t keep_running = 1;

/* Currently active backend, set once in main() */
static const struct netif_backend *g_be;

/* Cached MAC the Amiga side has told us about / that we told it */
static uint8_t netif_hw_mac[6];

#ifdef __MINGW32__
netif_config_t g_netif_cfg;

/*
 * Handle of the thread blocked reading stdin, so the console control
 * handler below can unstick it (see win_stdin_thread()).
 */
static HANDLE g_stdin_thread_handle = NULL;

/*
 * Console control handler: Windows' analog of the SIGINT/SIGTERM
 * handler below. A blocking fread() on stdin can't be woken by a
 * flag alone, so CancelSynchronousIo() is used to force it to return.
 */
static BOOL WINAPI
console_ctrl_handler(DWORD ctrl_type)
{
    switch (ctrl_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            keep_running = 0;
            if (g_stdin_thread_handle != NULL)
                CancelSynchronousIo(g_stdin_thread_handle);
            return (TRUE);
        default:
            return (FALSE);
    }
}
#else
/* Signal handler to catch termination events */
static void
handle_signal(int sig)
{
    (void) sig;
    keep_running = 0;
}
#endif

/*
 * Auto-detect the host's current default route interface
 * (e.g. "eth0", "en0")
 */
static int
get_default_iface(char *iface_buffer, size_t bufsize)
{
#if defined(__MINGW32__)
    /*
     * Windows has no popen()-friendly "ip route"/"route -n get
     * default" equivalent worth scraping text from -- ask iphlpapi
     * for the default route's interface index instead, then resolve
     * that index to its friendly name (e.g. "Ethernet", "Wi-Fi") via
     * GetAdaptersAddresses(). That friendly name is what gets handed
     * to the backend's open(), which resolves it against Npcap's
     * device list (see find_matching_device() in netif_windows.c).
     */
    MIB_IPFORWARDROW row;
    IP_ADAPTER_ADDRESSES *addrs, *a;
    ULONG bufsz = 15000;
    ULONG ret;
    int found = -1;

    memset(&row, 0, sizeof (row));
    if (GetBestRoute(0, 0, &row) != NO_ERROR) {
        fprintf(stderr, "Error: GetBestRoute could not find a default route\n");
        return (-1);
    }

    addrs = malloc(bufsz);
    if (addrs == NULL)
        return (-1);
    ret = GetAdaptersAddresses(AF_UNSPEC,
                               GAA_FLAG_SKIP_ANYCAST |
                               GAA_FLAG_SKIP_MULTICAST |
                               GAA_FLAG_SKIP_DNS_SERVER,
                               NULL, addrs, &bufsz);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        free(addrs);
        addrs = malloc(bufsz);
        if (addrs == NULL)
            return (-1);
        ret = GetAdaptersAddresses(AF_UNSPEC,
                                   GAA_FLAG_SKIP_ANYCAST |
                                   GAA_FLAG_SKIP_MULTICAST |
                                   GAA_FLAG_SKIP_DNS_SERVER,
                                   NULL, addrs, &bufsz);
    }
    if (ret != NO_ERROR) {
        fprintf(stderr, "Error: GetAdaptersAddresses failed (%lu)\n",
                (unsigned long)ret);
        free(addrs);
        return (-1);
    }

    for (a = addrs; a != NULL; a = a->Next) {
        int n;
        if (a->IfIndex != row.dwForwardIfIndex)
            continue;
        n = WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1,
                                 iface_buffer, (int)bufsize, NULL, NULL);
        if (n > 0)
            found = 0;
        break;
    }

    free(addrs);
    if (found < 0)
        fprintf(stderr, "Error: could not determine default route interface\n");
    return (found);
#elif defined(OSX)
    /*
     * macOS has no "ip route" -- the BSD equivalent is:
     *     route -n get default
     * which prints a block of "key: value" lines including one like:
     *     interface: en0
     */
    FILE *fp = popen("route -n get default", "r");
    if (!fp) {
        perror("Error: popen(route -n get default) failed");
        return (-1);
    }

    char line[512];
    int found = -1;
    while (fgets(line, sizeof (line), fp) != NULL) {
        char *tok = strstr(line, "interface:");
        if (tok != NULL) {
            tok += strlen("interface:");
            while (*tok == ' ' || *tok == '\t')
                tok++;
            unsigned int i = 0;
            while (tok[i] != '\0' && tok[i] != ' ' &&
                   tok[i] != '\n' && i < bufsize - 1) {
                iface_buffer[i] = tok[i];
                i++;
            }
            iface_buffer[i] = '\0';
            if (i > 0)
                found = 0;
            break;
        }
    }

    pclose(fp);
    if (found < 0)
        fprintf(stderr, "Error: could not determine default route interface\n");
    return (found);
#else
    /* Linux: "ip route show default" -- "default via 1.2.3.4 dev eth0 ..." */
    FILE *fp = popen("ip route show default", "r");
    if (!fp) {
        perror("Error: popen(ip route show default) failed");
        return (-1);
    }

    char line[512];
    int found = -1;
    if (fgets(line, sizeof (line), fp) != NULL) {
        char *dev_tok = strstr(line, " dev ");
        if (dev_tok != NULL) {
            dev_tok += 5;
            unsigned int i = 0;
            while (dev_tok[i] != '\0' && dev_tok[i] != ' ' &&
                   dev_tok[i] != '\n' && i < bufsize - 1) {
                iface_buffer[i] = dev_tok[i];
                i++;
            }
            iface_buffer[i] = '\0';
            if (i > 0)
                found = 0;
        }
    }

    pclose(fp);
    if (found < 0)
        fprintf(stderr, "Error: could not determine default route interface\n");
    return (found);
#endif
}

static void
usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options] [interface]\n"
            "\n"
            "  Bridge the host's network onto the emulated Amiga and relay\n"
            "  raw Ethernet frames over stdin/stdout.\n"
            "\n"
            "Options:\n"
#ifdef LINUX
            "  -e, --external   (Linux only) Use external 'ip' commands for\n"
            "                   link setup, instead of netlink.\n"
#endif
            "  -h, --help       Show this help.\n"
            "\n"
            "If interface is omitted the host's current default-route\n"
            "interface is used.\n",
            prog);
}

static void
dump_packet(uint8_t *data, uint packet_len)
{
    uint pos;
#if 0
    if (packet_len > 30)
        packet_len = 30;
#endif
    for (pos = 0; pos < packet_len; pos++)
        fprintf(stderr, " %02x", data[pos]);
    fprintf(stderr, "\n");
}

static int
send_pkt(uint xmit_len, uint real_len, uint8_t *frame)
{
    uint16_t frame_len = xmit_len;
    if (fwrite(&frame_len, sizeof (frame_len), 1, stdout) != 1) {
        perror("write to upstream failed\n");
        if (errno != 0) {
            fprintf(stderr, "write to pipe failed: %d: %s",
                    errno, strerror(errno));
            return (1);
        }
    }
    if (fwrite(frame, real_len, 1, stdout) != 1) {
        perror("write to upstream failed\n");
        if (errno != 0) {
            fprintf(stderr, "write to pipe failed: %d: %s",
                    errno, strerror(errno));
            return (1);
        }
    }
    fflush(stdout); /* Crucial: Push out of C buffer immediately */
    return (0);
}

static void
handle_cmd(uint8_t *data, uint len)
{
    fprintf(stderr, "cmd ");
    dump_packet(data, len);
    switch (data[0]) {              // First byte of data is always command
        case HS_NETIF_CMD_NOP:      // NOP
            break;
        case HS_NETIF_CMD_GETMAC:   // Get MAC
            fprintf(stderr, "Get MAC\n");
            if (g_be->get_mac(&data[1]) == 0) {
                uint8_t *mac = &data[1];
                fprintf(stderr, "Sending MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                data[0] = HS_NETIF_CMD_SETMAC;
                send_pkt(0xff07, 7, data);  // CMD + MAC
                memcpy(netif_hw_mac, data + 1, sizeof (netif_hw_mac));
            } else {
                fprintf(stderr, "Get MAC failed\n");
            }
            break;
        case HS_NETIF_CMD_SETMAC:   // Set MAC
            fprintf(stderr, "Set MAC\n");
            g_be->set_mac(data + 1);
            memcpy(netif_hw_mac, data + 1, sizeof (netif_hw_mac));
            break;
    }
}

static int
is_multicast(uint8_t *dstmac)
{
    if (dstmac[0] & 1) {  // Also captures all broadcasts
        return (1);
    } else {
        return (0);
    }
}

static int
is_broadcast(uint8_t *dstmac)
{
    if ((dstmac[0] == 0xff) && (dstmac[1] == 0xff) &&
        (dstmac[2] == 0xff) && (dstmac[3] == 0xff) &&
        (dstmac[4] == 0xff) && (dstmac[5] == 0xff)) {
        return (1);
    } else {
        return (0);
    }
}

static int
packet_filter(uint8_t *frame, uint frame_len)
{
    (void) frame_len;

    /* Filter out IPv6 */
    if ((frame[12] == 0x86) && (frame[13] == 0xdd))
        return (0);

    /* Check for destination MAC address match */
    if (memcmp(frame, netif_hw_mac, 6) == 0)
        return (1);

    if (is_multicast(frame) || is_broadcast(frame))
        return (1);

fprintf(stderr, "[nm %x]", frame_len);
    return (0);
}

#ifdef __MINGW32__
/*
 * Windows has no poll() that can wait on both an Npcap capture handle
 * and a CRT stdio pipe at once (see netif_backend.h), so the two
 * directions each run on their own dedicated thread instead of a
 * shared poll loop. win_capture_thread() mirrors "Case A" and
 * win_stdin_thread() mirrors "Case B" of the POSIX poll loop below.
 */
/*
 * A single read_frame() error is treated as transient (a brief
 * Npcap/adapter hiccup -- Wi-Fi power-save, adapter reset, sleep/
 * wake, a VPN adapter coming up or down, etc.) rather than fatal:
 * previously ANY negative return here tore down the whole program on
 * the spot, which is a much bigger blast radius than the error
 * usually warrants. Only bail out once failures are persistent.
 */
#define CAPTURE_MAX_CONSECUTIVE_ERRORS 8
#define CAPTURE_ERROR_RETRY_DELAY_MS   250

static DWORD WINAPI
win_capture_thread(LPVOID arg)
{
    unsigned char buffer[2000];
    int consecutive_errors = 0;

    (void) arg;
    while (keep_running) {
        int frame_bytes = g_be->read_frame(buffer, sizeof (buffer));
        if (frame_bytes > 0) {
            uint16_t frame_len = (uint16_t) frame_bytes;
            consecutive_errors = 0;
            if (packet_filter(buffer, frame_len)) {
                fprintf(stderr, ">> recv %u\n", frame_len);
                dump_packet(buffer, frame_len);
                if (send_pkt(frame_len, frame_len, buffer))
                    break;
            }
        } else if (frame_bytes < 0) {
            if (++consecutive_errors >= CAPTURE_MAX_CONSECUTIVE_ERRORS) {
                fprintf(stderr,
                    "netif read error (%d in a row) -- giving up\n",
                    consecutive_errors);
                break;
            }
            fprintf(stderr,
                "netif read error (%d/%d) -- retrying\n",
                consecutive_errors, CAPTURE_MAX_CONSECUTIVE_ERRORS);
            Sleep(CAPTURE_ERROR_RETRY_DELAY_MS);
        } else {
            /*
             * frame_bytes == 0: the backend's read timeout elapsed
             * with nothing captured -- not an error.
             */
            consecutive_errors = 0;
        }
    }
    keep_running = 0;
    return (0);
}

static DWORD WINAPI
win_stdin_thread(LPVOID arg)
{
    unsigned char buffer[2000];
    uint16_t frame_len;

    (void) arg;
    while (keep_running) {
        /* Read the length prefix first */
        if (fread(&frame_len, sizeof (frame_len), 1, stdin) != 1)
            break;   /* EOF, broken pipe, or CancelSynchronousIo() */

        if (frame_len <= sizeof (buffer)) {
            fprintf(stderr, ">> send %u\n", frame_len);
            size_t nread = fread(buffer, 1, frame_len, stdin);
            dump_packet(buffer, frame_len);
            if (nread == frame_len)
                g_be->write_frame(buffer, nread);
        } else if ((frame_len >> 8) == 0xff) {
            /* Special case: command */
            uint   read_len = frame_len & 0xff;
            fprintf(stderr, ">> send CMD l=%u\n", read_len);
            size_t nread    = fread(buffer, 1, read_len, stdin);
            if (nread == read_len)
                handle_cmd(buffer, read_len);
        }
    }
    keep_running = 0;
    return (0);
}
#endif /* __MINGW32__ */

int
main(int argc, char *argv[])
{
#ifdef __MINGW32__
    char iface_name[IFNAMSIZ + 32];
    char lower_dev[IFNAMSIZ];
    int argi = 1;
    HANDLE cap_thread;
    HANDLE handles[2];
    WSADATA wsadata;

    /*
     * The framing protocol on stdin/stdout is binary (arbitrary byte
     * values, including 0x0A) -- make sure the CRT doesn't mangle it
     * with newline translation, which is the default text mode.
     */
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    WSAStartup(MAKEWORD(2, 2), &wsadata);

    while (argi < argc) {
        if (strcmp(argv[argi], "-h") == 0 ||
            strcmp(argv[argi], "--help") == 0) {
            usage(argv[0]);
            return (0);
        } else if (argv[argi][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[argi]);
            usage(argv[0]);
            return (1);
        } else {
            break; /* positional interface name */
        }
    }

    g_be = netif_backend_get();

    /*
     * See the ensure_privilege contract in netif_backend.h: this
     * either returns already-elevated, or hands off to a UAC-elevated
     * relaunch and exits this process -- it never returns "failed".
     */
    if (g_be->ensure_privilege)
        g_be->ensure_privilege(argc, argv);

    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    if (argi < argc) {
        strncpy(lower_dev, argv[argi], sizeof (lower_dev) - 1);
        lower_dev[sizeof (lower_dev) - 1] = '\0';
    } else if (get_default_iface(lower_dev, sizeof (lower_dev)) < 0) {
        return (1);
    }

    if (g_be->open(lower_dev, iface_name, sizeof (iface_name)) < 0)
        return (1);

    g_be->get_mac(netif_hw_mac);

    cap_thread = CreateThread(NULL, 0, win_capture_thread, NULL, 0, NULL);
    g_stdin_thread_handle = CreateThread(NULL, 0, win_stdin_thread,
                                         NULL, 0, NULL);
    if (cap_thread == NULL || g_stdin_thread_handle == NULL) {
        fprintf(stderr, "Error: could not start worker threads\n");
        g_be->close();
        return (1);
    }

    handles[0] = cap_thread;
    handles[1] = g_stdin_thread_handle;

    /* Block until either direction ends (peer exited/EOF, or Ctrl-C) */
    WaitForMultipleObjects(2, handles, FALSE, INFINITE);

    /*
     * Signal + unstick whichever thread is still running, then give
     * both a moment to notice before we tear down the backend under
     * them.
     */
    keep_running = 0;
    CancelSynchronousIo(g_stdin_thread_handle);
    WaitForMultipleObjects(2, handles, TRUE, 5000);

    CloseHandle(cap_thread);
    CloseHandle(g_stdin_thread_handle);

    fprintf(stderr, "\nClosing %s\n", iface_name);
    g_be->close();
    WSACleanup();
    return (0);
#else
    char iface_name[IFNAMSIZ + 32];
    char lower_dev[IFNAMSIZ];
    unsigned char buffer[2000]; // Fits standard 1500-byte MTU frames + headers
    int argi = 1;
    int use_external_ip = 0;
    (void) use_external_ip;

    /* Simple option parsing (keep it dependency-free) */
    while (argi < argc) {
        if (strcmp(argv[argi], "-e") == 0 ||
            strcmp(argv[argi], "--external") == 0) {
#ifdef LINUX
            extern void netif_linux_set_use_external_ip(int);
            netif_linux_set_use_external_ip(1);
#else
            fprintf(stderr,
                    "Warning: --external is a Linux-only option, ignoring\n");
#endif
            argi++;
        } else if (strcmp(argv[argi], "-h") == 0 ||
                   strcmp(argv[argi], "--help") == 0) {
            usage(argv[0]);
            return (0);
        } else if (argv[argi][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[argi]);
            usage(argv[0]);
            return (1);
        } else {
            break; /* positional interface name */
        }
    }

    g_be = netif_backend_get();

    /*
     * 1. Ensure we have whatever privilege this platform's backend
     *    needs (CAP_NET_ADMIN via pkexec on Linux, administrator via
     *    osascript on macOS). Per the ensure_privilege contract, this
     *    either returns having already got it, or replaces/exits the
     *    process itself -- it never returns having failed silently.
     */
    if (g_be->ensure_privilege)
        g_be->ensure_privilege(argc, argv);

    /*
     * 2. Register signal actions for clean termination
     */
    struct sigaction sa;
    memset(&sa, 0, sizeof (sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /*
     * 3. Determine which physical interface to bridge onto. Optional
     *    remaining argv lets you pin it explicitly (e.g. "en0" /
     *    "eth0"); otherwise it's auto-detected from the host's
     *    current default route.
     */
    if (argi < argc) {
        strncpy(lower_dev, argv[argi], IFNAMSIZ - 1);
        lower_dev[IFNAMSIZ - 1] = '\0';
    } else if (get_default_iface(lower_dev, sizeof (lower_dev)) < 0) {
        exit(1);
    }

    /*
     * 4. Bring up the platform-specific bridged L2 presence on top of
     *    it (macvtap on Linux, BPF promiscuous capture on macOS).
     */
    if (g_be->open(lower_dev, iface_name, sizeof (iface_name)) < 0)
        exit(1);

    /* 5. Configure the polling loop */
    struct pollfd fds[2];

    /* Watch the backend for incoming OS frames */
    fds[0].fd = g_be->pollable_fd();
    fds[0].events = POLLIN;

    /* Watch standard input for frames coming from your unprivileged program */
    fds[1].fd = STDIN_FILENO;
    fds[1].events = POLLIN;

    g_be->get_mac(netif_hw_mac);

    uint16_t frame_len;
    while (keep_running) {
        int ret = poll(fds, 2, -1); /* Block until data is available */
        if (ret < 0) {
            /*
             * If the poll was interrupted by the signal handler, the
             * loop will break naturally.
             */
            continue;
        }

        /* Check for errors */
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "poll netif failed: %x\n", fds[0].revents);
            break;
        }
        if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "poll upstream Tx failed: %x\n", fds[1].revents);
            break;
        }

        /*
         * Case A: The host network stack has a frame bound for the
         * Amiga. Backends already hand us plain Ethernet (any
         * platform-specific framing, e.g. Linux's virtio_net_hdr, is
         * stripped inside the backend itself).
         */
        if (fds[0].revents & POLLIN) {
            int frame_bytes = g_be->read_frame(buffer, sizeof (buffer));
            if (frame_bytes > 0) {
                frame_len = (uint16_t)frame_bytes;
                if (packet_filter(buffer, frame_len)) {
                    fprintf(stderr, ">> recv %u\n", frame_len);
                    dump_packet(buffer, frame_len);
                    if (send_pkt(frame_len, frame_len, buffer))
                        break;
                }
            } else if (frame_bytes < 0) {
                fprintf(stderr, "netif read error\n");
            }
        }

        /* Case B: Unprivileged program piped an Amiga frame via stdin */
        if (fds[1].revents & POLLIN) {
            /* Read the length prefix first */
            if (fread(&frame_len, sizeof (frame_len), 1, stdin) == 1) {
                if (frame_len <= sizeof (buffer)) {
                    fprintf(stderr, ">> send %u\n", frame_len);
                    size_t nread = fread(buffer, 1, frame_len, stdin);
                    dump_packet(buffer, frame_len);
                    if (nread == frame_len) {
                        if (g_be->write_frame(buffer, nread) < 0) {
                            /* error already logged by the backend */
                        }
                    }
                } else if ((frame_len >> 8) == 0xff) {
                    /* Special case: command */
                    uint   read_len = frame_len & 0xff;
                    fprintf(stderr, ">> send CMD l=%u\n", read_len);
                    size_t nread    = fread(buffer, 1, read_len, stdin);
                    if (nread == read_len) {
                        handle_cmd(buffer, read_len);
                    }
                }
            } else {
                /* If stdin hits EOF or breaks, drop out and clean up. */
                break;
            }
        }
    }

    /* 6. Safe Termination and Plumbing Removal */
    fprintf(stderr, "\nClosing %s\n", iface_name);
    g_be->close();

    return (0);
#endif /* ! __MINGW32__ */
}
