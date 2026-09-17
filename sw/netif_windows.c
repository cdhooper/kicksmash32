/*
 * netif_windows.c -- Npcap-based backend for Windows.
 *
 * Windows has no macvtap equivalent, and getting a bridged virtual
 * adapter (TAP-Windows6, vmnet-style) onto the host would require the
 * user to install and bind a whole separate virtual NIC driver. So,
 * like the macOS BPF backend, this talks to the physical NIC directly
 * via promiscuous capture/injection -- the same mechanism libpcap,
 * Wireshark, and tcpdump-on-Windows use. Npcap is the current
 * (WinPcap-API-compatible) driver/library that provides this; see
 * https://npcap.com/. It must be installed on the target machine, and
 * this file is built against the separate "Npcap SDK" download (see
 * the Makefile's NPCAP_SDK_DIR variable) which provides pcap.h plus
 * import libraries for wpcap.dll/Packet.dll -- Npcap does not ship a
 * dev SDK itself.
 *
 * Interface identification: unlike "eth0"/"en0", Npcap enumerates
 * adapters under opaque names like "\Device\NPF_{<GUID>}". Since the
 * caller (hostsmash_netif.c) hands this backend a human-typed or
 * auto-detected interface identifier the same way it does on
 * Linux/macOS, find_matching_device() below accepts any of: a raw
 * Npcap device name, an adapter description substring, or (most
 * commonly, since that's what auto-detection produces) a Windows
 * "friendly name" such as "Ethernet" or "Wi-Fi", resolved to its
 * adapter GUID via GetAdaptersAddresses() and then matched against
 * the Npcap device list.
 *
 * No pollable fd: see the long comment in netif_backend.h. Instead,
 * pcap_open_live() below is given a short read timeout, and
 * read_frame() surfaces "no packet yet" (0) rather than blocking
 * forever, so the dedicated capture thread in hostsmash_netif.c's
 * Windows main loop can periodically notice a shutdown request.
 *
 * Elevation: raw capture requires the Npcap driver, which by default
 * only admits Administrator-owned handles (there is no per-adapter
 * ACL group on Windows the way access_bpf works on macOS). There is
 * no Windows analog of pkexec/osascript as a simple re-exec-with-
 * privilege call; the practical option is relaunching this program via
 * ShellExecuteEx's "runas" verb, which pops the standard UAC consent
 * prompt.
 *
 * wpcap.dll is loaded at runtime via LoadLibrary()/GetProcAddress(),
 * rather than linked normally against the Npcap SDK's import library.
 * If we linked it normally and Npcap weren't installed, the Windows
 * loader would refuse to even start this process, popping its own
 * generic "The program can't start because wpcap.dll is missing from
 * your computer" dialog before any of our code ran. Loading it
 * ourselves lets ensure_wpcap_loaded() below detect that case and show
 * a dialog that actually explains what Npcap is and where to get it.
 * The Npcap SDK (see the Makefile's NPCAP_SDK_DIR) is still needed at
 * build time, but now only for pcap.h's type/constant declarations --
 * not for its import libraries.
 */

/*
 * Needed for CONDITION_VARIABLE / InitializeConditionVariable() /
 * SleepConditionVariableCS() / WakeConditionVariable(), used by the
 * TAP-mode RX queue below -- those are only declared by windows.h
 * when targeting Vista or later, which isn't guaranteed to be
 * MinGW-w64's default. Guarded so an explicit -D_WIN32_WINNT from the
 * Makefile still takes precedence.
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <pcap.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "netif_backend.h"

static pcap_t *g_pcap = NULL;
static char    g_lower_dev[256];        // human-readable name, for logging
static char    g_pcap_dev_name[256];    // raw pcap device name, e.g.
                                        // "\Device\NPF_{GUID}" -- this is
                                        // what get_physical_mac() matches
                                        // against, since it's the only
                                        // thing that reliably identifies
                                        // *this* adapter (see below)
static uint8_t g_virtual_mac[6];        // Amiga's MAC filtering identity
static int     g_have_virtual_mac = 0;

/*
 * Read timeout (milliseconds) passed to pcap_open_live(). This is the
 * only mechanism read_frame() has to periodically return control to
 * its calling thread so it can notice a shutdown request -- see the
 * file header comment and hostsmash_netif.c's win_capture_thread().
 */
#define NPCAP_READ_TIMEOUT_MS 200

/* ------------------------------------------------------------------ */
/* Dynamic loading of wpcap.dll                                        */
/* ------------------------------------------------------------------ */

typedef int      (*pcap_findalldevs_fn)(pcap_if_t **, char *);
typedef void     (*pcap_freealldevs_fn)(pcap_if_t *);
typedef pcap_t  *(*pcap_open_live_fn)(const char *, int, int, int, char *);
typedef void     (*pcap_close_fn)(pcap_t *);
typedef int      (*pcap_next_ex_fn)(pcap_t *, struct pcap_pkthdr **,
                                    const uint8_t **);
typedef int      (*pcap_sendpacket_fn)(pcap_t *, const uint8_t *, int);
typedef char    *(*pcap_geterr_fn)(pcap_t *);
typedef int      (*pcap_datalink_fn)(pcap_t *);

static HMODULE              g_wpcap_dll = NULL;
static pcap_findalldevs_fn  p_pcap_findalldevs;
static pcap_freealldevs_fn  p_pcap_freealldevs;
static pcap_open_live_fn    p_pcap_open_live;
static pcap_close_fn        p_pcap_close;
static pcap_next_ex_fn      p_pcap_next_ex;
static pcap_sendpacket_fn   p_pcap_sendpacket;
static pcap_geterr_fn       p_pcap_geterr;
static pcap_datalink_fn     p_pcap_datalink;

static void
show_npcap_missing_dialog(void)
{
    MessageBoxW(NULL,
        L"hostsmash needs Npcap to bridge Ethernet frames on Windows, "
        L"but wpcap.dll could not be loaded.\n"
        L"\n"
        L"Install Npcap (free) from:\n"
        L"    https://npcap.com/#download\n"
        L"\n"
        L"During setup, make sure \"Install Npcap in WinPcap API-"
        L"compatible Mode\" is CHECKED -- that is what installs "
        L"wpcap.dll.\n"
        L"\n"
        L"After installing, run this program again.",
        L"Npcap not found",
        MB_OK | MB_ICONERROR);
}

/*
 * Loads wpcap.dll and resolves the handful of pcap_* entry points this
 * backend uses. Safe to call more than once (a no-op once loaded).
 * Returns 0 on success, -1 (after printing an error and showing
 * show_npcap_missing_dialog()) if Npcap isn't installed or is somehow
 * missing/incompatible.
 */
static int
ensure_wpcap_loaded(void)
{
    if (g_wpcap_dll != NULL)
        return (0);

    g_wpcap_dll = LoadLibraryW(L"wpcap.dll");
    if (g_wpcap_dll == NULL) {
        fprintf(stderr,
                "[NETIF] wpcap.dll could not be loaded -- is Npcap "
                "installed? (https://npcap.com/)\n");
        show_npcap_missing_dialog();
        return (-1);
    }

#define LOAD_SYM(var, type, name)                                          \
    do {                                                                   \
        (var) = (type) GetProcAddress(g_wpcap_dll, (name));                \
        if ((var) == NULL) {                                               \
            fprintf(stderr,                                                \
                    "[NETIF] wpcap.dll is missing %s -- is it an "          \
                    "old or corrupt Npcap install?\n", (name));            \
            FreeLibrary(g_wpcap_dll);                                      \
            g_wpcap_dll = NULL;                                            \
            show_npcap_missing_dialog();                                   \
            return (-1);                                                   \
        }                                                                  \
    } while (0)

    LOAD_SYM(p_pcap_findalldevs, pcap_findalldevs_fn, "pcap_findalldevs");
    LOAD_SYM(p_pcap_freealldevs, pcap_freealldevs_fn, "pcap_freealldevs");
    LOAD_SYM(p_pcap_open_live,   pcap_open_live_fn,   "pcap_open_live");
    LOAD_SYM(p_pcap_close,       pcap_close_fn,       "pcap_close");
    LOAD_SYM(p_pcap_next_ex,     pcap_next_ex_fn,     "pcap_next_ex");
    LOAD_SYM(p_pcap_sendpacket,  pcap_sendpacket_fn,  "pcap_sendpacket");
    LOAD_SYM(p_pcap_geterr,      pcap_geterr_fn,      "pcap_geterr");
    LOAD_SYM(p_pcap_datalink,    pcap_datalink_fn,    "pcap_datalink");

#undef LOAD_SYM

    return (0);
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

/*
 * Case-insensitive substring search. MinGW doesn't reliably provide
 * strcasestr() (it's a GNU/BSD extension), but does provide the
 * Windows CRT's _strnicmp().
 */
static const char *
my_strcasestr(const char *haystack, const char *needle)
{
    size_t needle_len;
    const char *p;

    if (haystack == NULL || needle == NULL)
        return (NULL);
    if (*needle == '\0')
        return (haystack);

    needle_len = strlen(needle);
    for (p = haystack; *p != '\0'; p++) {
        if (_strnicmp(p, needle, needle_len) == 0)
            return (p);
    }
    return (NULL);
}

/*
 * Resolve a Windows adapter "friendly name" (e.g. "Ethernet") to its
 * adapter GUID string (e.g. "{4D36E972-...}"), by substring match
 * against GetAdaptersAddresses()' FriendlyName field. Returns 0 and
 * fills guid_out on success, -1 if nothing matched.
 */
static int
resolve_friendly_to_guid(const char *want, char *guid_out, size_t guid_out_sz)
{
    ULONG bufsz = 15000;
    IP_ADAPTER_ADDRESSES *addrs, *a;
    ULONG ret;
    int found = -1;

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
        free(addrs);
        return (-1);
    }

    for (a = addrs; a != NULL; a = a->Next) {
        char friendly[256];
        int n = WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, friendly,
                                    (int)sizeof (friendly), NULL, NULL);
        if (n <= 0)
            continue;
        if (my_strcasestr(friendly, want) != NULL) {
            strncpy(guid_out, a->AdapterName, guid_out_sz - 1);
            guid_out[guid_out_sz - 1] = '\0';
            found = 0;
            break;
        }
    }

    free(addrs);
    return (found);
}

/*
 * find_matching_device() resolves `want` (whatever was passed as the
 * interface argument, or auto-detected -- see get_default_iface()'s
 * Windows branch in hostsmash_netif.c) against Npcap's device list.
 * Tried in order: exact/substring match on the raw pcap device name;
 * substring match on Npcap's adapter description; and finally
 * treating `want` as a Windows friendly name and matching by GUID.
 * A NULL/empty `want` just takes the first available device.
 */
static pcap_if_t *
find_matching_device(pcap_if_t *alldevs, const char *want)
{
    pcap_if_t *d;
    char guid[256];

    if (want == NULL || want[0] == '\0')
        return (alldevs);

    for (d = alldevs; d != NULL; d = d->next) {
        if (d->name != NULL && my_strcasestr(d->name, want) != NULL)
            return (d);
    }

    for (d = alldevs; d != NULL; d = d->next) {
        if ((d->description != NULL) &&
            (my_strcasestr(d->description, want)) != NULL)
            return (d);
    }

    if (resolve_friendly_to_guid(want, guid, sizeof (guid)) == 0) {
        for (d = alldevs; d != NULL; d = d->next) {
            if (d->name != NULL && my_strcasestr(d->name, guid) != NULL)
                return (d);
        }
    }

    return (NULL);
}

#if 0
/*
 * Physical MAC of the adapter identified by `pcap_dev_name` (the raw
 * pcap device name captured in windows_open(), e.g.
 * "\Device\NPF_{4D36E972-E325-11CE-BFC1-08002BE10318}"), via
 * GetAdaptersAddresses().
 *
 * This intentionally does NOT compare human-readable names (Npcap's
 * adapter "description" vs. Windows' connection "FriendlyName" are
 * two independent, differently-sourced strings for the same adapter
 * -- e.g. "Linksys USB3GIGV1" vs. "Ethernet" -- and substring-matching
 * one against the other is unreliable at best, and reliably wrong
 * whenever they simply don't share text, which is the common case).
 * Instead it matches by GUID: every adapter's GUID is embedded in its
 * pcap device name (the "\Device\NPF_{GUID}" convention) and is
 * exactly what GetAdaptersAddresses() reports back as AdapterName for
 * that same adapter, so checking whether AdapterName is a substring
 * of pcap_dev_name unambiguously identifies the one true adapter --
 * same technique resolve_friendly_to_guid() above already relies on.
 */
static int
get_physical_mac(const char *pcap_dev_name, uint8_t mac[6])
{
    ULONG bufsz = 15000;
    IP_ADAPTER_ADDRESSES *addrs, *a;
    ULONG ret;
    int found = -1;

    if (pcap_dev_name == NULL || pcap_dev_name[0] == '\0')
        return (-1);

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
        free(addrs);
        return (-1);
    }

    for (a = addrs; a != NULL; a = a->Next) {
        if (a->AdapterName == NULL)
            continue;
        if (my_strcasestr(pcap_dev_name, a->AdapterName) == NULL)
            continue;
        if (a->PhysicalAddressLength == 6) {
            memcpy(mac, a->PhysicalAddress, 6);
            found = 0;
            break;
        }
    }

    free(addrs);
    return (found);
}
#endif

/* ------------------------------------------------------------------ */
/* netif_backend implementation (TAP)                                 */
/* ------------------------------------------------------------------ */

#include <winioctl.h>

/* TAP Driver Control Codes */
#define TAP_CONTROL_CODE(request, method) CTL_CODE(FILE_DEVICE_UNKNOWN, \
                                                   request, method, \
                                                   FILE_ANY_ACCESS)
#define TAP_IOCTL_SET_MEDIA_STATUS        TAP_CONTROL_CODE(6, METHOD_BUFFERED)

static HANDLE h_tap = INVALID_HANDLE_VALUE;
static HANDLE h_rx_thread = NULL;
static volatile int g_rx_running = 0;

/*
 * Queue between windows_rx_thread_tap() (producer -- one dedicated
 * background thread doing overlapped ReadFile() on the TAP handle)
 * and windows_read_frame_tap() (consumer -- called from
 * hostsmash_netif.c's win_capture_thread, same as
 * windows_read_frame_pcap() is in PCAP mode).
 *
 * A queue rather than a single "one pending frame, block the RX
 * thread until it's consumed" slot: the RX thread's ReadFile() loop
 * needs to keep pulling frames off the TAP handle continuously,
 * independent of how promptly the consumer happens to call
 * windows_read_frame_tap(). If the RX thread instead blocked after
 * each frame until the consumer drained it, any burst of back-to-back
 * frames (Amiga-side traffic doesn't arrive on a schedule that
 * matches our poll cadence) would stall capture entirely rather than
 * just queuing up -- effectively adding TAP-mode-only packet loss
 * that PCAP mode, where Npcap does its own kernel-side buffering,
 * doesn't have.
 *
 * The queue is small and bounded (TAP_RXQ_CAPACITY) rather than
 * unbounded: this is a live interactive link to the Amiga, not a
 * capture-to-disk tool, so if the consumer ever falls far enough
 * behind to fill it, queuing further would just mean replaying a
 * stale backlog -- better to drop the oldest frame and keep the
 * queue representing "recent" traffic. In normal operation the
 * consumer drains this promptly and it never gets close to full.
 */
#define TAP_MAX_FRAME       1522   // matches the RX read buffer below --
                                   // standard Ethernet MTU + headers, no
                                   // jumbo frame support in TAP mode
#define TAP_RXQ_CAPACITY    64
#define TAP_READ_TIMEOUT_MS 200    // bounds how long windows_read_frame_tap()
                                   // can block, same role NPCAP_READ_TIMEOUT_MS
                                   // plays for windows_read_frame_pcap(): lets
                                   // the calling thread periodically recheck
                                   // its shutdown flag instead of blocking
                                   // forever.

typedef struct {
    uint8_t  data[TAP_MAX_FRAME];
    uint16_t len;
} tap_frame_t;

static tap_frame_t        g_rxq[TAP_RXQ_CAPACITY];
static int                g_rxq_head  = 0;   // next slot the producer fills
static int                g_rxq_tail  = 0;   // next slot the consumer drains
static int                g_rxq_count = 0;
static unsigned long      g_rxq_dropped = 0; // frames dropped, queue-full
static CRITICAL_SECTION   g_rxq_lock;
static CONDITION_VARIABLE g_rxq_cv;
static int                g_rxq_inited = 0;
static volatile int       g_rx_thread_error = 0; // set once, on a fatal
                                                 // RX-thread failure

static void
tap_rxq_init(void)
{
    if (g_rxq_inited)
        return;
    InitializeCriticalSection(&g_rxq_lock);
    InitializeConditionVariable(&g_rxq_cv);
    g_rxq_head = g_rxq_tail = g_rxq_count = 0;
    g_rxq_dropped = 0;
    g_rx_thread_error = 0;
    g_rxq_inited = 1;
}

static void
tap_rxq_destroy(void)
{
    if (!g_rxq_inited)
        return;
    DeleteCriticalSection(&g_rxq_lock);
    /* CONDITION_VARIABLEs need no explicit cleanup. */
    g_rxq_inited = 0;
}

/*
 * Called from windows_rx_thread_tap() with a just-read frame. Copies
 * it into the queue (dropping the oldest queued frame first if full)
 * and wakes windows_read_frame_tap() if it's waiting.
 */
static void
tap_rxq_push(const uint8_t *buf, uint16_t len)
{
    EnterCriticalSection(&g_rxq_lock);
    if (g_rxq_count == TAP_RXQ_CAPACITY) {
        g_rxq_tail = (g_rxq_tail + 1) % TAP_RXQ_CAPACITY;
        g_rxq_count--;
        g_rxq_dropped++;
    }
    if (len > TAP_MAX_FRAME)
        len = TAP_MAX_FRAME;    // shouldn't happen -- ReadFile() was
                                // given a same-sized buffer -- but
                                // don't overrun g_rxq[].data if it ever does.
    memcpy(g_rxq[g_rxq_head].data, buf, len);
    g_rxq[g_rxq_head].len = len;
    g_rxq_head = (g_rxq_head + 1) % TAP_RXQ_CAPACITY;
    g_rxq_count++;
    LeaveCriticalSection(&g_rxq_lock);

    WakeConditionVariable(&g_rxq_cv);
}

static DWORD WINAPI windows_rx_thread_tap(LPVOID lpParam);

static int
windows_start_thread(void)
{
    if (h_tap == INVALID_HANDLE_VALUE) {
        fprintf(stderr,
                "[NETIF] Can't start RX thread: TAP interface not open.\n");
        return (-1);
    }

    if (h_rx_thread != NULL) {
        fprintf(stderr, "[NETIF] RX thread already running.\n");
        return (-1);
    }

    g_rx_running = 1;
    h_rx_thread = CreateThread(
        NULL,                   // Default security attributes
        0,                      // Default stack size
        windows_rx_thread_tap,  // Thread function entry point
        NULL,                   // Parameter passed to thread
        0,                      // Default creation flags (start immediately)
        NULL);                  // Thread identifier (not needed)

    if (h_rx_thread == NULL) {
        fprintf(stderr, "[NETIF] Failed to create RX thread. Error: %lu\n",
                GetLastError());
        g_rx_running = 0;
        return (-1);
    }

    return (0);
}

#include <winreg.h>

static int
get_tap_guid(char *guid_out, size_t guid_sz)
{
    HKEY adapters_key;
    const char *adapters_path = "SYSTEM\\CurrentControlSet\\Control\\Class\\"
                                "{4D36E972-E325-11CE-BFC1-08002BE10318}";

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, adapters_path, 0,
                      KEY_READ, &adapters_key) != ERROR_SUCCESS) {
        return (-1);
    }

    DWORD index = 0;
    char subkey_name[256];
    DWORD subkey_len = sizeof (subkey_name);

    while (RegEnumKeyExA(adapters_key, index++, subkey_name, &subkey_len,
                         NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
        HKEY subkey;
#undef DEBUG_GET_TAP_GUID
#ifdef DEBUG_GET_TAP_GUID
        fprintf(stderr, "sk=%s  ", subkey_name);
#endif
        if (RegOpenKeyExA(adapters_key, subkey_name, 0,
                          KEY_READ, &subkey) == ERROR_SUCCESS) {
            char component_id[256] = {0};
            DWORD data_len = sizeof (component_id);

            if (RegQueryValueExA(subkey, "ComponentId", NULL, NULL,
                                (LPBYTE)component_id,
                                &data_len) == ERROR_SUCCESS) {
#ifdef DEBUG_GET_TAP_GUID
                fprintf(stderr, "cid=%s\n", component_id);
#endif
                if ((my_strcasestr(component_id, "tap0901") != NULL) ||
                    (my_strcasestr(component_id, "wintun") != NULL) ||
                    (my_strcasestr(component_id, "ovpn-dco") != NULL)) {
                    data_len = guid_sz;
                    if (RegQueryValueExA(subkey, "NetCfgInstanceId", NULL,
                                         NULL, (LPBYTE)guid_out,
                                         (DWORD*)&data_len) == ERROR_SUCCESS) {
                        RegCloseKey(subkey);
                        RegCloseKey(adapters_key);
                        return (0); // Success
                    }
                }
            }
            RegCloseKey(subkey);
        }
        subkey_len = sizeof (subkey_name);
    }
    RegCloseKey(adapters_key);
    return (-1); // Not found
}

/* Open OpenVPN TAP-Windows6 Adapter */
static int
windows_open_tap(const char *lower_dev, char *name_out, size_t name_out_sz)
{
    (void) lower_dev;
    (void) name_out;
    (void) name_out_sz;
    strncpy(name_out, "name_out-unknown", name_out_sz);
    fprintf(stderr, "lower_dev=%s\n", lower_dev);
    char tap_path[256];
    char tap_guid[128];

    if (get_tap_guid(tap_guid, sizeof (tap_guid))) {
        fprintf(stderr, "Failed to acquire tap path\n");
        goto tap_invalid_handle;
    }
    snprintf(tap_path, sizeof (tap_path), "\\\\.\\Global\\%s.tap", tap_guid);

    fprintf(stderr, "tap guid=%s\n", tap_guid);
    fprintf(stderr, "tap path=%s\n", tap_path);

    tap_rxq_init();

    h_tap = CreateFileA(tap_path, GENERIC_READ | GENERIC_WRITE,
                        0, NULL, OPEN_EXISTING,
                        FILE_ATTRIBUTE_SYSTEM | FILE_FLAG_OVERLAPPED, NULL);

    if (h_tap == INVALID_HANDLE_VALUE) {
tap_invalid_handle:
        fprintf(stderr,
                "[NETIF] Failed to open TAP adapter device.\n"
                "        Did you install OpenTAP? "
                         " https://openvpn.net/community/\n"
//              "        https://build.openvpn.net/downloads/releases\n"
                "        1. On the install screen, ensure that TAP-Windows6\n"
                "           driver is selected.\n"
                "        2. Open Windows Network Connections\n"
                "        3. Right click active Wi-Fi or Ethernet adapter ->\n"
                "           Properties -> Sharing tab\n"
                "        4. Enable \"Allow other network users to connect"
                         " through\n"
                "           this computer's internet connection\".\n"
                "        5. For \"Networking connection\" select"
                         " \"OpenVPN TAP Windows6\".\n");
        return (-1);
    }

    /* Set TAP media status to Connected */
    ULONG status = 1;
    DWORD bytes_returned;
    DeviceIoControl(h_tap, TAP_IOCTL_SET_MEDIA_STATUS, &status, sizeof (status),
                   &status, sizeof (status), &bytes_returned, NULL);

    return (windows_start_thread());
}

/* Disconnects and shuts down the TAP adapter handle */
static void
windows_close_tap(void)
{
    /* Signal the thread to terminate */
    if (h_rx_thread != NULL) {
        g_rx_running = 0;

        /* Wait up to 1 second for the thread to exit cleanly */
        DWORD wait_result = WaitForSingleObject(h_rx_thread, 1000);
        if (wait_result == WAIT_TIMEOUT) {
            fprintf(stderr, "[NETIF] Warning: RX thread exit timeout.\n");
            TerminateThread(h_rx_thread, 0);
        }
        CloseHandle(h_rx_thread);
        h_rx_thread = NULL;
    }

    tap_rxq_destroy();

    if (h_tap != INVALID_HANDLE_VALUE) {
        /*
         * Cancel pending I/O requests (e.g. active ReadFile/WriteFile calls)
         */
        CancelIo(h_tap);

        /* Set TAP media status to Disconnected (0) */
        ULONG status = 0;
        DWORD bytes_returned;
        DeviceIoControl(h_tap, TAP_IOCTL_SET_MEDIA_STATUS, &status,
                        sizeof (status), &status, sizeof (status),
                        &bytes_returned, NULL);

        /* Close the Win32 handle */
        CloseHandle(h_tap);
        h_tap = INVALID_HANDLE_VALUE;

        fprintf(stderr, "[NETIF] TAP interface closed successfully.\n");
    }
}

/* Send Ethernet Frame */
static int
windows_write_frame_tap(const uint8_t *buf, size_t len)
{
    DWORD bytes_written;
    OVERLAPPED ol = {0};
    if (!WriteFile(h_tap, buf, len, &bytes_written, &ol)) {
        if (GetLastError() == ERROR_IO_PENDING) {
            GetOverlappedResult(h_tap, &ol, &bytes_written, TRUE);
        } else {
            return (-1);
        }
    }
    return ((int)bytes_written);
}

/*
 * Pull the next frame off the RX queue that windows_rx_thread_tap()
 * fills, or return 0 if TAP_READ_TIMEOUT_MS elapses with nothing
 * queued -- that's the calling thread's (hostsmash_netif.c's
 * win_capture_thread) cue to recheck its shutdown flag, not an error.
 * Mirrors windows_read_frame_pcap()'s contract exactly.
 */
static int
windows_read_frame_tap(uint8_t *buf, size_t buflen)
{
    int frame_len;

    if (h_tap == INVALID_HANDLE_VALUE || !g_rxq_inited)
        return (-1);

    EnterCriticalSection(&g_rxq_lock);
    while (g_rxq_count == 0 && !g_rx_thread_error) {
        if (!SleepConditionVariableCS(&g_rxq_cv, &g_rxq_lock,
                                       TAP_READ_TIMEOUT_MS)) {
            /*
             * Timed out (GetLastError() == ERROR_TIMEOUT) with nothing
             * queued -- not an error, same as pcap_next_ex()'s rc==0.
             */
            LeaveCriticalSection(&g_rxq_lock);
            return (0);
        }
    }

    if (g_rxq_count == 0) {
        /*
         * Woke up because windows_rx_thread_tap() hit a fatal error,
         * not because a frame arrived.
         */
        LeaveCriticalSection(&g_rxq_lock);
        return (-1);
    }

    if (g_rxq[g_rxq_tail].len > buflen) {
        frame_len = -1;   /* caller's buffer too small */
    } else {
        memcpy(buf, g_rxq[g_rxq_tail].data, g_rxq[g_rxq_tail].len);
        frame_len = (int)g_rxq[g_rxq_tail].len;
    }
    g_rxq_tail = (g_rxq_tail + 1) % TAP_RXQ_CAPACITY;
    g_rxq_count--;
    LeaveCriticalSection(&g_rxq_lock);

    return (frame_len);
}

/*
 * Asynchronously reads a single frame from the TAP interface.
 * Returns the number of bytes read, 0 if no packet was available
 * (timeout), or -1 on error.
 */
static int
windows_receive_packet_tap(uint8_t *buf, uint32_t buf_len, DWORD timeout_ms)
{
    if (h_tap == INVALID_HANDLE_VALUE || buf == NULL) {
        return (-1);
    }

    OVERLAPPED ol = {0};
    ol.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (ol.hEvent == NULL) {
        return (-1);
    }

    DWORD bytes_read = 0;
    BOOL success = ReadFile(h_tap, buf, buf_len, &bytes_read, &ol);

    if (!success) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            /* Wait for the packet read or timeout */
            DWORD wait_result = WaitForSingleObject(ol.hEvent, timeout_ms);

            if (wait_result == WAIT_OBJECT_0) {
                if (!GetOverlappedResult(h_tap, &ol, &bytes_read, FALSE)) {
                    bytes_read = 0;
                }
            } else if (wait_result == WAIT_TIMEOUT) {
                /* No frame received within the timeout window */
                CancelIo(h_tap);
                bytes_read = 0;
            } else {
                CancelIo(h_tap);
                bytes_read = -1;
            }
        } else {
            bytes_read = -1;
        }
    }

    CloseHandle(ol.hEvent);
    return ((int) bytes_read);
}

/*
 * Background RX loop: keeps a ReadFile() outstanding on the TAP handle
 * and queues each frame it captures for windows_read_frame_tap() to
 * hand to the caller -- see the big comment above tap_rxq_push().
 */
static DWORD WINAPI
windows_rx_thread_tap(LPVOID lpParam)
{
    (void) lpParam;
    uint8_t buf[TAP_MAX_FRAME];

    while (g_rx_running && (h_tap != INVALID_HANDLE_VALUE)) {
        int len = windows_receive_packet_tap(buf, sizeof (buf), 100);
        if (len > 0) {
            tap_rxq_push(buf, (uint16_t)len);
        } else if (len < 0) {
            fprintf(stderr, "[NETIF] Error reading from TAP interface.\n");
            g_rx_thread_error = 1;
            WakeConditionVariable(&g_rxq_cv);
            break;
        }
        /*
         * len == 0: windows_receive_packet_tap()'s own 100ms wait
         * elapsed with nothing captured -- loop back and recheck
         * g_rx_running, same as the pcap capture thread does.
         */
    }
    return (0);
}

/* ------------------------------------------------------------------ */
/* netif_backend implementation (PCAP)                                */
/* ------------------------------------------------------------------ */

static int
windows_open_pcap(const char *lower_dev, char *name_out, size_t name_out_sz)
{
    char errbuf[PCAP_ERRBUF_SIZE];
    pcap_if_t *alldevs, *dev, *d;

    if (ensure_wpcap_loaded() < 0)
        return (-1);

    if (p_pcap_findalldevs(&alldevs, errbuf) == -1) {
        fprintf(stderr, "[NETIF] pcap_findalldevs: %s\n", errbuf);
        fprintf(stderr,
                "[NETIF] is Npcap installed? (https://npcap.com/)\n");
        return (-1);
    }
    if (alldevs == NULL) {
        fprintf(stderr,
                "[NETIF] no capture devices found -- install Npcap "
                "from https://npcap.com/\n");
        return (-1);
    }

    dev = find_matching_device(alldevs, lower_dev);
    if (dev == NULL) {
        fprintf(stderr, "[NETIF] no adapter matching \"%s\" found\n",
                (lower_dev != NULL) ? lower_dev : "(default)");
        fprintf(stderr, "[NETIF] available adapters:\n");
        for (d = alldevs; d != NULL; d = d->next)
            fprintf(stderr, "  %s%s%s\n", d->name,
                    (d->description != NULL) ? " -- " : "",
                    (d->description != NULL) ? d->description : "");
        p_pcap_freealldevs(alldevs);
        return (-1);
    }

    /*
     * 65536 snaplen comfortably covers jumbo frames; promiscuous mode
     * so we see frames not addressed to the host itself (the Amiga's
     * MAC is not the host's real MAC); NPCAP_READ_TIMEOUT_MS so
     * read_frame() wakes up periodically -- see the file header.
     */
    g_pcap = p_pcap_open_live(dev->name, 65536, 1,
                              NPCAP_READ_TIMEOUT_MS, errbuf);
    if (g_pcap == NULL) {
        fprintf(stderr, "[NETIF] pcap_open_live(%s): %s\n",
                dev->name, errbuf);
        fprintf(stderr,
                "[NETIF] this usually means the process is not "
                "running elevated, or Npcap is not installed\n");
        p_pcap_freealldevs(alldevs);
        return (-1);
    }

    if (p_pcap_datalink(g_pcap) != DLT_EN10MB) {
        fprintf(stderr, "[NETIF] %s is not Ethernet, refusing\n",
                dev->name);
        p_pcap_close(g_pcap);
        g_pcap = NULL;
        p_pcap_freealldevs(alldevs);
        return (-1);
    }

    strncpy(g_lower_dev,
            (dev->description != NULL) ? dev->description : dev->name,
            sizeof (g_lower_dev) - 1);
    g_lower_dev[sizeof (g_lower_dev) - 1] = '\0';
    strncpy(g_pcap_dev_name, dev->name, sizeof (g_pcap_dev_name) - 1);
    g_pcap_dev_name[sizeof (g_pcap_dev_name) - 1] = '\0';
    snprintf(name_out, name_out_sz, "npcap(%s)", g_lower_dev);

    fprintf(stderr, "[NETIF] bound to %s via Npcap, promiscuous\n",
            g_lower_dev);

    p_pcap_freealldevs(alldevs);
    return (0);
}

static void
windows_close_pcap(void)
{
    if (g_pcap != NULL) {
        p_pcap_close(g_pcap);
        g_pcap = NULL;
    }
}

/*
 * Pull the next frame from Npcap, or return 0 if the read timeout
 * (NPCAP_READ_TIMEOUT_MS) elapsed with nothing captured -- that's the
 * calling thread's cue to recheck its shutdown flag, not an error.
 */
static int
windows_read_frame_pcap(uint8_t *buf, size_t buflen)
{
    struct pcap_pkthdr *hdr;
    const uint8_t *data;
    int rc;

    if (g_pcap == NULL)
        return (-1);

    rc = p_pcap_next_ex(g_pcap, &hdr, &data);
    if (rc == 1) {
        if (hdr->caplen > buflen)
            return (-1);
        memcpy(buf, data, hdr->caplen);
        return ((int)hdr->caplen);
    }
    if (rc == 0)
        return (0);   /* read timeout, nothing captured */

    fprintf(stderr, "[NETIF] pcap_next_ex: %s\n", p_pcap_geterr(g_pcap));
    return (-1);
}

static int
windows_write_frame_pcap(const uint8_t *buf, size_t len)
{
    if (g_pcap == NULL)
        return (-1);

    if (p_pcap_sendpacket(g_pcap, buf, (int)len) != 0) {
        fprintf(stderr, "[NETIF] pcap_sendpacket: %s\n",
                p_pcap_geterr(g_pcap));
        return (-1);
    }
    return (0);
}

/* ------------------------------------------------------------------ */
/* netif_backend implementation (common)                              */
/* ------------------------------------------------------------------ */

static int
windows_pollable_fd(void)
{
    /*
     * Not applicable on this backend -- see the long comment in
     * netif_backend.h. Never pass this to poll()/select()/WSAPoll();
     * the Windows main loop in hostsmash_netif.c doesn't use one.
     */
    return (-1);
}

/*
 * As on the macOS BPF backend: there is no real adapter reconfiguration
 * here (changing a physical Windows NIC's MAC is driver-dependent, at
 * best done through a "Network Address" registry override requiring an
 * adapter restart, and not reliable enough to depend on). This only
 * updates the in-memory filtering identity; accept/reject by
 * destination MAC still happens in hostsmash_netif.c's frame_filter().
 */
static int
windows_set_mac(const uint8_t mac[6])
{
    memcpy(g_virtual_mac, mac, 6);
    g_have_virtual_mac = 1;
    fprintf(stderr,
            "[NETIF] virtual MAC set to %02x:%02x:%02x:%02x:%02x:%02x "
            "(adapter's real MAC is unchanged)\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return (0);
}

static int
get_or_create_virtual_mac(uint8_t mac[6])
{
    const char *filename = "virtual_mac.txt";

    /* 1. Attempt to open in the current working directory */
    FILE *fp = fopen(filename, "r");

    /* 2. If not found, attempt to open in the executable's directory */
    if (fp == NULL) {
        char exe_path[MAX_PATH];
        DWORD length = GetModuleFileNameA(NULL, exe_path, MAX_PATH);
        if (length > 0 && length < MAX_PATH) {
            /* Strip the executable name to keep the directory path */
            char *last_slash = strrchr(exe_path, '\\');
            if (last_slash) {
                *(last_slash + 1) = '\0'; // Truncate after trailing backslash

                char full_path[MAX_PATH];
                snprintf(full_path, sizeof (full_path), "%s%s", exe_path,
                         filename);
                fp = fopen(full_path, "r");
            }
        }
    }

    /* Read MAC address if either open attempt succeeded */
    if (fp != NULL) {
        unsigned int imac[6];
        int count = fscanf(fp, "%x:%x:%x:%x:%x:%x",
                           &imac[0], &imac[1], &imac[2],
                           &imac[3], &imac[4], &imac[5]);
        fclose(fp);
        if (count != 6) {
            fprintf(stderr, "Failed to parse MAC in %s\n", filename);
            return (-1);
        }
        for (int i = 0; i < 6; i++)
            mac[i] = (uint8_t) imac[i];
        return (0);
    }

    /* Generate a random locally administered MAC address */
    srand((unsigned int)time(NULL));
    for (int i = 0; i < 6; i++) {
        mac[i] = rand() & 0xFF;
    }

    /* Set unicast (bit 0 = 0) and locally administered (bit 1 = 1) */
    mac[0] = (mac[0] & 0xFE) | 0x02;

    /* Write new MAC address to current working directory */
    fp = fopen(filename, "w");
    if (fp != NULL) {
        fprintf(fp, "%02x:%02x:%02x:%02x:%02x:%02x\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        fclose(fp);
    }
    return (0);
}

static int
windows_get_mac(uint8_t mac[6])
{
    if (g_have_virtual_mac) {
        memcpy(mac, g_virtual_mac, 6);
        return (0);
    }
    if (get_or_create_virtual_mac(mac)) {
        memset(mac, 0, 6);
        return (-1);
    }
    fprintf(stderr, "HW MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return (0);
}

/* ------------------------------------------------------------------ */
/* Elevation                                                            */
/* ------------------------------------------------------------------ */

static int
is_elevated(void)
{
    HANDLE token = NULL;
    TOKEN_ELEVATION elevation;
    DWORD sz = sizeof (elevation);
    int elevated = 0;

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
 * Returns 1 if our stdin/stdout look like the inherited pipe ends
 * hostsmash.exe's netif_start() wires up (see hostsmash_net.c), 0 if
 * they look like an interactive console -- i.e. someone ran this exe
 * by hand from a shell to test it.
 *
 * This distinction matters because of what happens next: a UAC
 * relaunch via ShellExecuteExW cannot carry inherited handles across
 * the elevation boundary (see the comment on windows_ensure_privilege
 * below), so taking that path when we're actually hostsmash.exe's
 * child would silently orphan the pipes hostsmash.exe is talking on,
 * leaving it waiting on a connection nothing will ever answer.
 */
static int
stdio_is_piped(void)
{
    HANDLE in  = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);

    return (in != NULL && out != NULL &&
            GetFileType(in)  == FILE_TYPE_PIPE &&
            GetFileType(out) == FILE_TYPE_PIPE);
}

/*
 * Per the ensure_privilege contract in netif_backend.h: returns 0 if
 * already elevated. Otherwise, since Windows has no execve()-style
 * in-place re-exec, this normally hands off to a UAC-elevated
 * relaunch of the same program with the same arguments via
 * ShellExecuteExW's "runas" verb, then exits this (unprivileged)
 * process -- functionally the same "never returns to the caller"
 * contract as pkexec/osascript.
 *
 * UNLIKE pkexec (execvp, in-place) or osascript's "do shell script"
 * (also runs in place and pipes output back through the caller's own
 * stdio), ShellExecuteExW's "runas" always creates a brand-new,
 * unrelated process with a fresh console and fresh standard handles:
 * Windows does not allow inheriting handles -- including the pipe
 * ends hostsmash.exe hands us via STARTF_USESTDHANDLES -- across a
 * UAC integrity-level boundary. If we relaunched unconditionally,
 * that would mean: when hostsmash.exe (unelevated) spawns us as its
 * netif helper, we'd pop a UAC prompt, launch a second, disconnected
 * copy of ourselves talking to nobody in a new window, and this
 * (piped) copy would exit(0) -- closing hostsmash.exe's pipe on it
 * immediately and silently.
 *
 * So: only take the relaunch path when stdio looks interactive (a
 * user manually testing this exe from a console). When stdio is
 * piped -- i.e. hostsmash.exe is our parent -- fail loudly instead of
 * pulling that rug out from under it. hostsmash.exe is expected to
 * have already elevated itself before spawning us in that case (see
 * hostsmash.c), so this path firing at all indicates hostsmash.exe's
 * own elevation didn't happen.
 */
static int
windows_ensure_privilege(int argc, char *argv[])
{
    wchar_t exe_path[MAX_PATH];
    char args[4096];
    wchar_t wargs[4096];
    SHELLEXECUTEINFOW sei;
    int off = 0;
    int i;

    /*
     * Check for Npcap before ever popping a UAC prompt -- there's no
     * point asking the user to elevate for a driver that isn't even
     * installed. ensure_wpcap_loaded() shows show_npcap_missing_dialog()
     * itself on failure.
     */
    if (ensure_wpcap_loaded() < 0)
        exit(1);

    if (is_elevated())
        return (0);

    if (stdio_is_piped()) {
        fprintf(stderr,
            "[NETIF] administrator privileges are required for packet capture,"
            " but this\n"
            "        process was launched with its stdin/stdout already"
            " connected to a pipe\n"
            "        (i.e. by hostsmash.exe), so it cannot UAC-elevate itself"
            " without\n"
            "        breaking that connection.\n"
            "[NETIF] run hostsmash.exe itself from an elevated command prompt"
            " instead\n"
            "        (Run as Administrator).\n");
        exit(1);
    }

    if (GetModuleFileNameW(NULL, exe_path, MAX_PATH) == 0) {
        fprintf(stderr, "[NETIF] GetModuleFileNameW failed\n");
        exit(1);
    }

    args[0] = '\0';
    for (i = 1; i < argc && off < (int)sizeof (args) - 4; i++)
        off += snprintf(args + off, sizeof (args) - off, "\"%s\" ", argv[i]);
    MultiByteToWideChar(CP_UTF8, 0, args, -1, wargs,
                        (int)(sizeof (wargs) / sizeof (wargs[0])));

    fprintf(stderr, "[NETIF] elevating via UAC prompt...\n");

    memset(&sei, 0, sizeof (sei));
    sei.cbSize = sizeof (sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe_path;
    sei.lpParameters = wargs;
    sei.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        DWORD err = GetLastError();
        if (err == ERROR_CANCELLED)
            fprintf(stderr, "[NETIF] elevation declined by user\n");
        else
            fprintf(stderr, "[NETIF] ShellExecuteExW failed (%lu)\n",
                    (unsigned long)err);
        exit(1);
    }

    if (sei.hProcess != NULL)
        CloseHandle(sei.hProcess);

    /* The elevated child carries on independently; we go away. */
    exit(0);
}

static int
windows_parse_args(int argc, char *argv[])
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--use-pcap") == 0) {
            g_netif_cfg.mode = NETIF_MODE_PCAP;
        }
        if (strcmp(argv[i], "--use-tap") == 0) {
            g_netif_cfg.mode = NETIF_MODE_TAP;
        }
    }
    if (g_netif_cfg.mode == NETIF_MODE_TAP) {
        fprintf(stderr, "[NETIF] Using Windows TAP Adapter path\n");
    } else {
        fprintf(stderr, "[NETIF] Using Npcap packet path\n");
    }
    return (0);
}

static const struct netif_backend windows_backend_tap = {
    .open             = windows_open_tap,
    .close            = windows_close_tap,
    .pollable_fd      = windows_pollable_fd,
    .read_frame       = windows_read_frame_tap,
    .write_frame      = windows_write_frame_tap,
    .set_mac          = windows_set_mac,
    .get_mac          = windows_get_mac,
    .ensure_privilege = windows_ensure_privilege,
    .parse_args       = windows_parse_args,
};

static const struct netif_backend windows_backend_pcap = {
    .open             = windows_open_pcap,
    .close            = windows_close_pcap,
    .pollable_fd      = windows_pollable_fd,
    .read_frame       = windows_read_frame_pcap,
    .write_frame      = windows_write_frame_pcap,
    .set_mac          = windows_set_mac,
    .get_mac          = windows_get_mac,
    .ensure_privilege = windows_ensure_privilege,
    .parse_args       = windows_parse_args,
};

const struct netif_backend *
netif_backend_get(void)
{
    g_netif_cfg.mode = NETIF_MODE_PCAP;

    if (g_netif_cfg.mode == NETIF_MODE_PCAP) {
        return (&windows_backend_pcap);
    } else {
        return (&windows_backend_tap);
    }
}
