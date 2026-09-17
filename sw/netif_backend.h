/*
 * netif_backend.h
 *
 * Abstraction over the platform-specific mechanism used to get raw,
 * bridged Ethernet frames on/off the host's physical NIC. Each platform
 * implements this differently because there is no cross-platform
 * equivalent of Linux's macvtap:
 *
 *   Linux   - bridge-mode macvtap character device (existing code)
 *   macOS   - BPF (/dev/bpfN) promiscuous capture on the physical NIC
 *             (vmnet.framework's bridged mode would be the "native"
 *             choice, but needs Apple's com.apple.vm.networking
 *             entitlement, which isn't obtainable here)
 *   Windows - Npcap (WinPcap-API-compatible) promiscuous capture on
 *             the physical NIC -- see netif_windows.c. This is the
 *             same shape as the macOS BPF backend: there is no
 *             Windows equivalent of macvtap either, and a TAP-Windows6
 *             virtual adapter would require the user to install and
 *             bind a separate virtual NIC, so raw capture/injection on
 *             the real adapter (already the mechanism libpcap/Npcap
 *             and Wireshark use) is used instead.
 *
 * main.c's poll() loop only needs a pollable fd plus read/write calls,
 * so every backend -- however it actually receives frames internally --
 * must expose a real file descriptor that becomes readable when a frame
 * is available. For callback/dispatch-queue-driven APIs (vmnet is one),
 * the backend fakes this with an internal pipe: the delivery callback
 * writes queued frames into the pipe's write end, and backend_fd()
 * returns the read end for poll().
 *
 * Windows is the one platform where this "real pollable fd" contract
 * can't be honored cleanly: an Npcap capture handle surfaces as a
 * Windows HANDLE (via pcap_getevent()), not a fd poll()/select()
 * understands, and there is no poll()/WSAPoll() that spans both a
 * pcap HANDLE and a CRT stdio pipe at once. So on Windows,
 * pollable_fd() is a stub (returns -1, see netif_windows.c) and
 * hostsmash_netif.c's main() uses a Windows-specific thread-based loop
 * instead of poll() for that platform only; every other backend call
 * (open/close/read_frame/write_frame/get_mac/set_mac/ensure_privilege)
 * is used identically across all three platforms.
 */

#ifndef NETIF_BACKEND_H
#define NETIF_BACKEND_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#ifndef __MINGW32__
#include <net/if.h>
#endif

struct netif_backend {
    /*
     * Bring up a bridged L2 presence on `lower_dev` (e.g. "en0").
     * On success, returns 0 and fills `name_out` (backend-defined
     * label, may just echo lower_dev on backends with no virtual
     * interface name of their own).
     */
    int (*open)(const char *lower_dev, char *name_out, size_t name_out_sz);

    /* Tear down whatever `open` created. Idempotent. */
    void (*close)(void);

    /*
     * Return a file descriptor suitable for poll(POLLIN). Becomes
     * readable when at least one frame is queued for read_frame().
     */
    int (*pollable_fd)(void);

    /*
     * Read one frame (no virtio_net_hdr or other framing -- plain
     * Ethernet, dest MAC first). Returns frame length, 0 if nothing
     * available, -1 on error.
     */
    int (*read_frame)(uint8_t *buf, size_t buflen);

    /*
     * Write one plain Ethernet frame out to the physical segment.
     * Returns 0 on success, -1 on error.
     */
    int (*write_frame)(const uint8_t *buf, size_t len);

    /*
     * Set/get the MAC address frames should appear to originate from
     * or be delivered to. On backends with no separate virtual
     * adapter (BPF, Npcap raw) this is typically a no-op filter
     * update rather than a real adapter reconfiguration -- see each
     * backend's comments.
     */
    int (*set_mac)(const uint8_t mac[6]);
    int (*get_mac)(uint8_t mac[6]);

    /*
     * Ensure the process has whatever privilege this backend needs
     * (CAP_NET_ADMIN, root, Administrator, ...) before open() is
     * called. Contract:
     *
     *   - Returns 0 if already sufficiently privileged; the caller
     *     continues running in the current process.
     *   - Otherwise this function does NOT return control normally.
     *     It either exec()s a re-invocation of the program under an
     *     OS-native elevation prompt (pkexec, osascript
     *     "administrator privileges", UAC "runas", ...), which
     *     replaces the current process image entirely, or it prints
     *     an error and exit()s the process directly if elevation
     *     itself fails or is declined.
     *
     * Optional: NULL if a backend needs no elevation step of its own.
     */
    int (*ensure_privilege)(int argc, char *argv[]);

    int (*parse_args)(int argc, char *argv[]);
};

/* Returns the backend for the current platform. */
const struct netif_backend *netif_backend_get(void);

typedef enum {
    NETIF_MODE_TAP = 0, /* Default: Windows TAP Adapter */
    NETIF_MODE_PCAP = 1 /* Original Npcap path */
} netif_mode_t;

typedef struct {
    netif_mode_t mode;
    uint8_t virtual_mac[6];
    uint8_t host_phys_mac[6];
    /* Existing netif fields... */
} netif_config_t;

extern netif_config_t g_netif_cfg;

#endif /* NETIF_BACKEND_H */
