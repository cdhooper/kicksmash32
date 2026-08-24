/*
 * macos_bpf_netif.c -- BPF-based backend (replaces the vmnet.framework
 * attempt: vmnet's bridged mode needs Apple's com.apple.vm.networking
 * entitlement, which isn't obtainable here).
 *
 * This talks to the physical NIC directly via /dev/bpfN, the same
 * mechanism libpcap/tcpdump use on macOS:
 *
 *   - BIOCSETIF binds the bpf device to a specific interface (e.g. en0)
 *   - BIOCPROMISC puts it in promiscuous mode so we see frames not
 *     addressed to the host itself (needed since the Amiga's MAC is
 *     not the host's MAC)
 *   - BIOCSHDRCMPLT tells the kernel our writes already contain a
 *     complete link-layer header (source MAC included) -- otherwise
 *     BPF stamps the *host's* MAC as source on every outbound frame,
 *     which would defeat the whole point
 *   - BIOCSSEESENT off stops our own injected frames from being
 *     echoed back to us as if they were received
 *   - BIOCIMMEDIATE makes reads return as soon as a packet is
 *     available instead of waiting for the buffer to fill
 *
 * Unlike vmnet, a bpf fd is directly select()/poll()-able, so there's
 * no async-callback-to-pipe bridge needed here (contrast with the
 * discarded vmnet backend). One read() can return several packets
 * back-to-back in a bpf_hdr-framed buffer, so read_frame() keeps a
 * static cursor into the last-read buffer and drains it before
 * issuing another read().
 *
 * No special entitlement is required. Root (or membership in the
 * `access_bpf` group with the right ACL on /dev/bpf*, which is not
 * the default on a stock Mac) is required to open /dev/bpfN at all.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/bpf.h>
#include <net/if_dl.h>
#include <ifaddrs.h>
#include <mach-o/dyld.h>

#include "netif_backend.h"

#define MAX_FRAME   2048
#define BPF_BUFSIZE 131072   /* 128KB capture buffer */

static int      g_bpf_fd = -1;
static char     g_lower_dev[IFNAMSIZ];
static uint8_t  g_rdbuf[BPF_BUFSIZE];
static ssize_t  g_rdbuf_len = 0;   /* bytes currently valid in g_rdbuf */
static ssize_t  g_rdbuf_pos = 0;   /* cursor into g_rdbuf for the next frame */
static uint8_t  g_virtual_mac[6];  /* the "Amiga's" MAC for filtering purposes */
static int      g_have_virtual_mac = 0;

/*
 * Open the first free /dev/bpfN. macOS doesn't hand these out
 * cloned-on-open the way Linux gives you a fresh fd from a single
 * node -- you have to probe individual device nodes and take
 * whichever isn't already claimed by another process (tcpdump,
 * Wireshark, etc.).
 */
static int
open_bpf_device(void)
{
    char path[32];
    for (int i = 0; i < 256; i++) {
        snprintf(path, sizeof(path), "/dev/bpf%d", i);
        int fd = open(path, O_RDWR);
        if (fd >= 0)
            return fd;
        if (errno == ENOENT)
            break;      /* no more device nodes to try */
        /* EBUSY (already claimed) or EACCES -- try the next one */
    }
    return -1;
}

/* Read the physical interface's own MAC via getifaddrs/AF_LINK. */
static int
get_physical_mac(const char *ifname, uint8_t mac[6])
{
    struct ifaddrs *ifap, *ifa;
    int found = -1;

    if (getifaddrs(&ifap) != 0) {
        perror("macos_bpf_netif: getifaddrs");
        return -1;
    }

    for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_LINK)
            continue;
        if (strcmp(ifa->ifa_name, ifname) != 0)
            continue;

        struct sockaddr_dl *sdl = (struct sockaddr_dl *)(void *)ifa->ifa_addr;
        if (sdl->sdl_alen != 6)
            continue;

        memcpy(mac, LLADDR(sdl), 6);
        found = 0;
        break;
    }

    freeifaddrs(ifap);
    return found;
}

static int
bpf_open(const char *lower_dev, char *name_out, size_t name_out_sz)
{
    g_bpf_fd = open_bpf_device();
    if (g_bpf_fd < 0) {
        fprintf(stderr,
            "macos_bpf_netif: could not open any /dev/bpfN (all busy, or "
            "insufficient permission -- run as root)\n");
        return -1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, lower_dev, sizeof(ifr.ifr_name) - 1);

    if (ioctl(g_bpf_fd, BIOCSETIF, &ifr) < 0) {
        perror("macos_bpf_netif: BIOCSETIF");
        goto fail;
    }

    u_int dlt;
    if (ioctl(g_bpf_fd, BIOCGDLT, &dlt) < 0) {
        perror("macos_bpf_netif: BIOCGDLT");
        goto fail;
    }
    if (dlt != DLT_EN10MB) {
        fprintf(stderr,
            "macos_bpf_netif: %s is not Ethernet (DLT %u), refusing\n",
            lower_dev, dlt);
        goto fail;
    }

    u_int enable = 1;
    if (ioctl(g_bpf_fd, BIOCPROMISC, &enable) < 0) {
        perror("macos_bpf_netif: BIOCPROMISC");
        goto fail;
    }
    if (ioctl(g_bpf_fd, BIOCSHDRCMPLT, &enable) < 0) {
        perror("macos_bpf_netif: BIOCSHDRCMPLT");
        goto fail;
    }
    if (ioctl(g_bpf_fd, BIOCIMMEDIATE, &enable) < 0) {
        perror("macos_bpf_netif: BIOCIMMEDIATE");
        goto fail;
    }
    u_int disable = 0;
    if (ioctl(g_bpf_fd, BIOCSSEESENT, &disable) < 0) {
        perror("macos_bpf_netif: BIOCSSEESENT");
        /* non-fatal -- worst case we see our own injected frames
           echoed back and main.c's frame_filter()/dest-MAC check
           will typically discard them anyway */
    }

    u_int blen = BPF_BUFSIZE;
    if (ioctl(g_bpf_fd, BIOCSBLEN, &blen) < 0) {
        perror("macos_bpf_netif: BIOCSBLEN");
        /* non-fatal -- kernel default buffer size still works,
           just means more syscalls under load */
    }

    strncpy(g_lower_dev, lower_dev, sizeof(g_lower_dev) - 1);
    g_lower_dev[sizeof(g_lower_dev) - 1] = '\0';
    g_rdbuf_len = g_rdbuf_pos = 0;

    snprintf(name_out, name_out_sz, "bpf(%s)", lower_dev);
    fprintf(stderr, "macos_bpf_netif: bound to %s via /dev/bpfN, promiscuous\n",
            lower_dev);
    return 0;

fail:
    close(g_bpf_fd);
    g_bpf_fd = -1;
    return -1;
}

static void
bpf_close(void)
{
    if (g_bpf_fd >= 0) {
        close(g_bpf_fd);
        g_bpf_fd = -1;
    }
    g_rdbuf_len = g_rdbuf_pos = 0;
}

static int
bpf_pollable_fd(void)
{
    return g_bpf_fd;
}

/*
 * Pull the next frame out of the current bpf buffer, refilling with a
 * fresh read() when exhausted. Each packet in the buffer is preceded
 * by a struct bpf_hdr; packets are BPF_WORDALIGN-padded, matching how
 * libpcap walks the same buffer format.
 */
static int
bpf_read_frame(uint8_t *buf, size_t buflen)
{
    if (g_rdbuf_pos >= g_rdbuf_len) {
        ssize_t n = read(g_bpf_fd, g_rdbuf, sizeof(g_rdbuf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR)
                return 0;
            perror("macos_bpf_netif: read");
            return -1;
        }
        if (n == 0)
            return 0;
        g_rdbuf_len = n;
        g_rdbuf_pos = 0;
    }

    if (g_rdbuf_pos + (ssize_t)sizeof(struct bpf_hdr) > g_rdbuf_len) {
        /* malformed/truncated trailing header -- resync on next read */
        g_rdbuf_len = g_rdbuf_pos = 0;
        return 0;
    }

    struct bpf_hdr *bh = (struct bpf_hdr *)(void *)(g_rdbuf + g_rdbuf_pos);
    uint32_t caplen  = bh->bh_caplen;
    uint32_t hdrlen  = bh->bh_hdrlen;

    if (g_rdbuf_pos + (ssize_t)hdrlen + (ssize_t)caplen > g_rdbuf_len) {
        g_rdbuf_len = g_rdbuf_pos = 0;
        return 0;
    }

    int ret;
    if (caplen > buflen) {
        ret = -1;   /* caller's buffer too small; still advance past it */
    } else {
        memcpy(buf, g_rdbuf + g_rdbuf_pos + hdrlen, caplen);
        ret = (int)caplen;
    }

    ssize_t consumed = hdrlen + caplen;
    g_rdbuf_pos += BPF_WORDALIGN(consumed);
    return ret;
}

static int
bpf_write_frame(const uint8_t *buf, size_t len)
{
    if (g_bpf_fd < 0 || len == 0 || len > MAX_FRAME)
        return -1;

    ssize_t w = write(g_bpf_fd, buf, len);
    if (w < 0) {
        perror("macos_bpf_netif: write");
        return -1;
    }
    if ((size_t)w != len) {
        fprintf(stderr, "macos_bpf_netif: short write (%zd of %zu)\n", w, len);
        return -1;
    }
    return 0;
}

/*
 * See file header: there is no real adapter here to reconfigure, so
 * this only updates the in-memory filtering identity. Frame accept/
 * reject by destination MAC still happens where it already did in
 * the shared main.c (frame_filter()) -- this just gives that logic
 * something authoritative to compare against on this backend.
 */
static int
bpf_set_mac(const uint8_t mac[6])
{
    memcpy(g_virtual_mac, mac, 6);
    g_have_virtual_mac = 1;
    fprintf(stderr,
        "macos_bpf_netif: virtual MAC set to %02x:%02x:%02x:%02x:%02x:%02x "
        "(filtering identity only -- %s's real MAC is unchanged)\n",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], g_lower_dev);
    return 0;
}

static int
bpf_get_mac(uint8_t mac[6])
{
    if (g_have_virtual_mac) {
        memcpy(mac, g_virtual_mac, 6);
        return 0;
    }
    /* No SETMAC seen yet -- report the physical NIC's real MAC as a
       sane initial default, same as what a fresh macvtap would show
       before the Amiga sends its own. */
    if (get_physical_mac(g_lower_dev, mac) != 0) {
        memset(mac, 0, 6);
        return -1;
    }
    return 0;
}

static const struct netif_backend macos_bpf_backend = {
    .open         = bpf_open,
    .close        = bpf_close,
    .pollable_fd  = bpf_pollable_fd,
    .read_frame   = bpf_read_frame,
    .write_frame  = bpf_write_frame,
    .set_mac      = bpf_set_mac,
    .get_mac      = bpf_get_mac,
};

const struct netif_backend *
netif_backend_get(void)
{
    return &macos_bpf_backend;
}

/*
 * Elevation: unchanged from the vmnet attempt -- BPF device access
 * still requires root on a stock Mac (no access_bpf ACL by default),
 * so the same osascript "with administrator privileges" re-exec
 * applies. Kept here so this file is a complete drop-in replacement.
 */
int
macos_exec_again_with_privilege(int argc, char *argv[])
{
    char prog_path[1024];
    uint32_t sz = sizeof(prog_path);
    if (_NSGetExecutablePath(prog_path, &sz) != 0) {
        strncpy(prog_path, argv[0], sizeof(prog_path) - 1);
    }

    char cmd[4096];
    int off = snprintf(cmd, sizeof(cmd), "'%s'", prog_path);
    for (int i = 1; i < argc && off < (int)sizeof(cmd) - 4; i++) {
        off += snprintf(cmd + off, sizeof(cmd) - off, " '%s'", argv[i]);
    }

    char osa_arg[4300];
    snprintf(osa_arg, sizeof(osa_arg),
             "do shell script \"%s\" with administrator privileges", cmd);

    execlp("osascript", "osascript", "-e", osa_arg, (char *)NULL);
    perror("macos_bpf_netif: execlp osascript failed");
    return 1;
}
