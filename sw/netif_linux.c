/*
 * netif_linux.c -- Linux backend: bridge-mode macvtap over netlink/ioctl.
 *
 * Extracted from the original single-file hostsmash_netif.c so the
 * platform-agnostic poll loop, framing protocol, and command handling
 * in hostsmash_netif.c can be shared across Linux/macOS/Windows via
 * the netif_backend interface (see netif_backend.h).
 *
 * Everything in this file is exactly the Linux macvtap logic as it
 * existed before -- the netlink message building, the ioctl-based
 * flag/MAC manipulation, and the pkexec/capabilities elevation path
 * -- just reshaped behind the five backend calls plus ensure_privilege.
 * One behavioral addition: read_frame()/write_frame() now handle the
 * virtio_net_hdr strip/prepend internally, so hostsmash_netif.c no
 * longer needs to know macvtap has a header at all -- that knowledge
 * is fully contained here, matching how the other backends (which
 * have no such header) look from the caller's side.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <sys/uio.h>
#include <sys/capability.h>
#include <linux/virtio_net.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <net/if_arp.h>

#include "netif_backend.h"

static unsigned  cfg_by_ext_prog = 0; /* 0 = netlink (default), 1 = external "ip" */
static char      g_tap_name[IFNAMSIZ];
static int       g_tap_fd = -1;

/* Called from hostsmash_netif.c if the user passes -e/--external. */
void
netif_linux_set_use_external_ip(int use_external)
{
    cfg_by_ext_prog = use_external ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Netlink helpers                                                     */
/* ------------------------------------------------------------------ */

static int
nl_open(void)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) {
        perror("Error: netlink socket");
        return (-1);
    }
    return (fd);
}

/*
 * rta_append() will append a raw attribute; returning a pointer to the
 *              attribute or NULL on overflow
 */
static struct rtattr *
rta_append(struct nlmsghdr *nh, size_t maxlen, unsigned short type,
           const void *data, size_t datalen)
{
    struct rtattr *rta;
    size_t len = RTA_LENGTH(datalen);
    if (NLMSG_ALIGN(nh->nlmsg_len) + RTA_ALIGN(len) > maxlen)
        return (NULL);
    rta = (struct rtattr *) ((char *)nh + NLMSG_ALIGN(nh->nlmsg_len));
    rta->rta_type = type;
    rta->rta_len  = len;
    if (datalen && data)
        memcpy(RTA_DATA(rta), data, datalen);
    nh->nlmsg_len = NLMSG_ALIGN(nh->nlmsg_len) + RTA_ALIGN(len);
    return (rta);
}

static int
nl_talk(int fd, struct nlmsghdr *nh, size_t maxlen)
{
    struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
    struct iovec iov = { .iov_base = nh, .iov_len = nh->nlmsg_len };
    struct msghdr msg = {
        .msg_name    = &sa,
        .msg_namelen = sizeof (sa),
        .msg_iov     = &iov,
        .msg_iovlen  = 1,
    };

    nh->nlmsg_flags |= NLM_F_ACK;
    nh->nlmsg_seq    = 1;
    nh->nlmsg_pid    = 0;

    if (sendmsg(fd, &msg, 0) < 0) {
        perror("Error: netlink sendmsg");
        return (-1);
    }

    char buf[4096];
    iov.iov_base = buf;
    iov.iov_len  = sizeof (buf);
    ssize_t n = recvmsg(fd, &msg, 0);
    if (n < 0) {
        perror("Error: netlink recvmsg");
        return (-1);
    }

    for (struct nlmsghdr *r = (struct nlmsghdr *)buf;
         NLMSG_OK(r, (unsigned)n); r = NLMSG_NEXT(r, n)) {
        if (r->nlmsg_type == NLMSG_ERROR) {
            struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(r);
            if (err->error) {
                errno = -err->error;
                fprintf(stderr, "Error: netlink: %s\n", strerror(errno));
                return (-1);
            }
            return (0);
        }
    }
    fprintf(stderr, "Error: netlink: no ACK received\n");
    return (-1);
}

/*
 * nl_create_macvtap() creates a bridge-mode macvtap named @name on top
 *                     of @lower_ifindex. Returns 0 on success.
 */
static int
nl_create_macvtap(const char *name, int lower_ifindex)
{
    int fd = nl_open();
    if (fd < 0)
        return (-1);

    char buf[1024];
    memset(buf, 0, sizeof (buf));
    struct nlmsghdr *nh = (struct nlmsghdr *)buf;
    struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(nh);

    nh->nlmsg_len   = NLMSG_LENGTH(sizeof (*ifi));
    nh->nlmsg_type  = RTM_NEWLINK;
    nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL;

    ifi->ifi_family = AF_UNSPEC;

    if (!rta_append(nh, sizeof (buf), IFLA_IFNAME, name, strlen(name) + 1))
        goto overflow;

    if (!rta_append(nh, sizeof (buf), IFLA_LINK, &lower_ifindex,
                    sizeof (lower_ifindex)))
        goto overflow;

    struct rtattr *linkinfo;
    linkinfo = rta_append(nh, sizeof (buf), IFLA_LINKINFO, NULL, 0);
    if (!linkinfo)
        goto overflow;

    if (!rta_append(nh, sizeof (buf), IFLA_INFO_KIND, "macvtap",
                    strlen("macvtap") + 1))
        goto overflow;

    struct rtattr *infodata;
    infodata = rta_append(nh, sizeof (buf), IFLA_INFO_DATA, NULL, 0);
    if (!infodata)
        goto overflow;

    uint32_t mode = MACVLAN_MODE_BRIDGE;
    if (!rta_append(nh, sizeof (buf), IFLA_MACVLAN_MODE, &mode, sizeof (mode)))
        goto overflow;

    infodata->rta_len = (char *)nh + nh->nlmsg_len - (char *)infodata;
    linkinfo->rta_len = (char *)nh + nh->nlmsg_len - (char *)linkinfo;

    int rc = nl_talk(fd, nh, sizeof (buf));
    close(fd);
    return (rc);

overflow:
    fprintf(stderr, "Error: netlink message buffer overflow\n");
    close(fd);
    return (-1);
}

static int
if_get_name(struct ifreq *ifr, const char *name)
{
    if (strlen(name) > sizeof (ifr->ifr_name)) {
        perror("Error: interface name too long");
        return (1);
    }
    strcpy(ifr->ifr_name, name);
    return (0);
}

/*
 * if_set_flags() brings the named interface up (or down) via ioctl.
 */
static int
if_set_flags(const char *name, int up)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof (ifr));
    if (if_get_name(&ifr, name))
        return (1);

    int len = strlen(name);
    if (len >= IFNAMSIZ - 1) {
        perror("Error: interface name too long");
        return (1);
    }
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("Error: socket for SIOCSIFFLAGS");
        return (-1);
    }

    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) {
        perror("Error: SIOCGIFFLAGS");
        close(fd);
        return (-1);
    }

    if (up)
        ifr.ifr_flags |= IFF_UP;
    else
        ifr.ifr_flags &= ~IFF_UP;

    if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0) {
        perror("Error: SIOCSIFFLAGS");
        close(fd);
        return (-1);
    }

    close(fd);
    return (0);
}

/*
 * nl_delete_link() deletes a link by name via RTM_DELLINK.
 */
static int
nl_delete_link(const char *name)
{
    int ifindex = (int)if_nametoindex(name);
    if (ifindex == 0) {
        return (0);  // Already gone
    }

    int fd = nl_open();
    if (fd < 0)
        return (-1);

    char buf[256];
    memset(buf, 0, sizeof (buf));
    struct nlmsghdr *nh = (struct nlmsghdr *)buf;
    struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(nh);

    nh->nlmsg_len   = NLMSG_LENGTH(sizeof (*ifi));
    nh->nlmsg_type  = RTM_DELLINK;
    nh->nlmsg_flags = NLM_F_REQUEST;

    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_index  = ifindex;

    rta_append(nh, sizeof (buf), IFLA_IFNAME, name, strlen(name) + 1);

    int rc = nl_talk(fd, nh, sizeof (buf));
    close(fd);
    return (rc);
}

/* ------------------------------------------------------------------ */
/* Macvtap allocation / teardown                                       */
/* ------------------------------------------------------------------ */

static int
allocate_macvtap(const char *lower_dev, char *dev_name_buffer, size_t dev_name_buffer_sz)
{
    static int counter = 0;
    char name[IFNAMSIZ];
    snprintf(name, sizeof (name), "amiga-vtap%d", counter++);

    if (cfg_by_ext_prog) {
        char cmd[256];
        snprintf(cmd, sizeof (cmd),
                 "ip link add link %s name %s type macvtap mode bridge",
                 lower_dev, name);
        if (system(cmd) != 0) {
            fprintf(stderr, "Error: failed to create macvtap link on %s\n",
                    lower_dev);
            return (-1);
        }

        snprintf(cmd, sizeof (cmd), "ip link set %s up", name);
        if (system(cmd) != 0) {
            fprintf(stderr, "Error: failed to bring up %s\n", name);
            snprintf(cmd, sizeof (cmd), "ip link delete %s", name);
            system(cmd);
            return (-1);
        }
    } else {
        int lower_ifindex = (int)if_nametoindex(lower_dev);
        if (lower_ifindex == 0) {
            fprintf(stderr,
                    "Error: lower device %s does not exist\n", lower_dev);
            return (-1);
        }

        if (nl_create_macvtap(name, lower_ifindex) < 0) {
            fprintf(stderr, "Error: failed to create macvtap %s on %s\n",
                    name, lower_dev);
            return (-1);
        }

        if (if_set_flags(name, 1) < 0) {
            fprintf(stderr, "Error: failed to bring up %s\n", name);
            nl_delete_link(name);
            return (-1);
        }
    }

    char path[256], line[32];
    snprintf(path, sizeof (path), "/sys/class/net/%s/ifindex", name);
    FILE *f = fopen(path, "r");
    if (!f) {
        perror("Error: could not read ifindex for macvtap device");
        goto teardown_fail;
    }
    if (fgets(line, sizeof (line), f) == NULL) {
        fclose(f);
        goto teardown_fail;
    }
    fclose(f);
    int ifindex = atoi(line);

    char devpath[64];
    snprintf(devpath, sizeof (devpath), "/dev/tap%d", ifindex);
    int fd = open(devpath, O_RDWR);
    if (fd < 0) {
        perror("Error: open macvtap character device failed");
        goto teardown_fail;
    }

    strncpy(dev_name_buffer, name, dev_name_buffer_sz - 1);
    dev_name_buffer[dev_name_buffer_sz - 1] = '\0';
    strncpy(g_tap_name, name, IFNAMSIZ - 1);
    g_tap_name[IFNAMSIZ - 1] = '\0';
    return (fd);

teardown_fail:
    if (cfg_by_ext_prog) {
        char cmd[256];
        snprintf(cmd, sizeof (cmd), "ip link delete %s", name);
        system(cmd);
    } else {
        nl_delete_link(name);
    }
    return (-1);
}

static void
delete_macvtap(const char *dev_name, unsigned by_ext)
{
    if (by_ext) {
        char cmd[256];
        snprintf(cmd, sizeof (cmd), "ip link delete %s", dev_name);
        system(cmd);
    } else {
        nl_delete_link(dev_name);
    }
    g_tap_name[0] = '\0';
}

/* ------------------------------------------------------------------ */
/* Privilege elevation                                                 */
/* ------------------------------------------------------------------ */

static int
get_self_path(char *path, unsigned pathmax)
{
    ssize_t len = readlink("/proc/self/exe", path, pathmax - 1);
    if (len != -1) {
        path[len] = '\0';
    } else {
        return (1);
    }
    return (0);
}

static int
exec_again_with_privilege(int argc, char *argv[])
{
    char prog_path[PATH_MAX];
    printf("Elevating privileges via pkexec...\n");

    if (get_self_path(prog_path, sizeof (prog_path))) {
        strcpy(prog_path, argv[0]);
    }

    char **new_argv = malloc((argc + 2) * sizeof (char *));

    new_argv[0] = "pkexec";
    new_argv[1] = prog_path;

    for (int i = 1; i < argc; i++) {
        new_argv[i + 1] = argv[i];
    }
    new_argv[argc + 1] = NULL;

    execvp("pkexec", new_argv);

    perror("execvp failed");
    free(new_argv);
    return (1);
}

static int
has_net_admin_permission(void)
{
    cap_t            caps;
    cap_flag_value_t eff_val;
    cap_flag_value_t perm_val;
    int              has_permission = 0;

    caps = cap_get_proc();
    if (caps == NULL) {
        perror("cap_get_proc");
        return (0);
    }

    if (cap_get_flag(caps, CAP_NET_ADMIN, CAP_EFFECTIVE, &eff_val) == -1) {
        perror("cap_get_flag effective");
        cap_free(caps);
        return (0);
    }

    if (cap_get_flag(caps, CAP_NET_ADMIN, CAP_PERMITTED, &perm_val) == -1) {
        perror("cap_get_flag permitted");
        cap_free(caps);
        return (0);
    }

    if (eff_val == CAP_SET && perm_val == CAP_SET) {
        has_permission = 1;
    }

    cap_free(caps);
    return (has_permission);
}

static void
make_cap_inheritable(void)
{
    cap_t caps = cap_get_proc();
    cap_value_t cap_list[1] = { CAP_NET_ADMIN };

    cap_set_flag(caps, CAP_INHERITABLE, 1, cap_list, CAP_SET);
    cap_set_proc(caps);
    cap_free(caps);

    prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_NET_ADMIN, 0, 0);
}

/*
 * Per the ensure_privilege contract in netif_backend.h: returns 0 if
 * already privileged (or upgraded in-place via setuid(0)), otherwise
 * re-execs through pkexec (replacing the process on success) or
 * exits on failure.
 */
static int
linux_ensure_privilege(int argc, char *argv[])
{
    if (!has_net_admin_permission()) {
        if (geteuid() != 0)
            exit(exec_again_with_privilege(argc, argv));

        /*
         * Set real id to 0 to control network. This does not work in
         * modern Linux because the program must be given the
         * capability to change the real UID.
         */
        if (getuid() != 0)
            setuid(0);
    }
    make_cap_inheritable();
    return (0);
}

/* ------------------------------------------------------------------ */
/* MAC get/set                                                         */
/* ------------------------------------------------------------------ */

static void
set_mac_macvtap(const uint8_t *mac)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof (ifr));
    if (if_get_name(&ifr, g_tap_name))
        return;

    if (g_tap_name[0] == '\0') {
        fprintf(stderr, "Error: set_mac: no macvtap allocated\n");
        return;
    }

    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("Error: set_mac socket");
        return;
    }

    ifr.ifr_hwaddr.sa_family = ARPHRD_ETHER;
    memcpy(ifr.ifr_hwaddr.sa_data, mac, 6);

    if (ioctl(fd, SIOCSIFHWADDR, &ifr) == 0) {
        fprintf(stderr, "MAC set to %02x:%02x:%02x:%02x:%02x:%02x\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        close(fd);
        return;
    }

    /* Some drivers require the interface to be down */
    if (if_set_flags(g_tap_name, 0) < 0) {
        perror("Error: set_mac: could not bring interface down");
        close(fd);
        return;
    }

    if (ioctl(fd, SIOCSIFHWADDR, &ifr) < 0) {
        perror("Error: SIOCSIFHWADDR");
        if_set_flags(g_tap_name, 1);
        close(fd);
        return;
    }

    if (if_set_flags(g_tap_name, 1) < 0)
        perror("Error: set_mac: could not bring interface back up");

    fprintf(stderr, "MAC set to %02x:%02x:%02x:%02x:%02x:%02x\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    close(fd);
}

static int
get_mac_macvtap(uint8_t *mac)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof (ifr));
    if (if_get_name(&ifr, g_tap_name))
        return (1);

    if (g_tap_name[0] == '\0') {
        fprintf(stderr, "Error: get_mac: no macvtap allocated\n");
        memset(mac, 0, 6);
        return (1);
    }

    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("Error: get_mac socket");
        memset(mac, 0, 6);
        return (1);
    }

    if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
        perror("Error: SIOCGIFHWADDR");
        memset(mac, 0, 6);
        close(fd);
        return (1);
    }

    memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    close(fd);

    fprintf(stderr, "HW MAC is %02x:%02x:%02x:%02x:%02x:%02x\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return (0);
}

/* ------------------------------------------------------------------ */
/* netif_backend implementation                                        */
/* ------------------------------------------------------------------ */

static int
linux_open(const char *lower_dev, char *name_out, size_t name_out_sz)
{
    char tap_name[IFNAMSIZ];
    int fd = allocate_macvtap(lower_dev, tap_name, sizeof(tap_name));
    if (fd < 0)
        return (-1);

    g_tap_fd = fd;
    strncpy(name_out, tap_name, name_out_sz - 1);
    name_out[name_out_sz - 1] = '\0';

    fprintf(stderr, "Created macvtap interface %s on %s (%s)\n",
            tap_name, lower_dev, cfg_by_ext_prog ? "external ip" : "netlink");

    /* Wait for network MAC to settle, matching original behavior. */
    sleep(1);
    return (0);
}

static void
linux_close(void)
{
    if (g_tap_fd >= 0) {
        close(g_tap_fd);
        g_tap_fd = -1;
    }
    if (g_tap_name[0] != '\0')
        delete_macvtap(g_tap_name, cfg_by_ext_prog);
}

static int
linux_pollable_fd(void)
{
    return g_tap_fd;
}

/*
 * macvtap always prepends a struct virtio_net_hdr to every frame read
 * from the character device -- strip it here so the caller only ever
 * sees plain Ethernet, same as every other backend.
 */
static int
linux_read_frame(uint8_t *buf, size_t buflen)
{
    unsigned char raw[2000];
    ssize_t nread = read(g_tap_fd, raw, sizeof(raw));
    if (nread < 0) {
        if (errno == EAGAIN || errno == EINTR)
            return (0);
        perror("Error: netif read failed");
        return (-1);
    }
    if (nread <= (ssize_t)sizeof(struct virtio_net_hdr))
        return (0);

    size_t frame_len = nread - sizeof(struct virtio_net_hdr);
    if (frame_len > buflen)
        return (-1);

    memcpy(buf, raw + sizeof(struct virtio_net_hdr), frame_len);
    return ((int)frame_len);
}

/*
 * macvtap requires a virtio_net_hdr before every frame written into
 * the character device. Zeroed-out is correct for plain Ethernet
 * frames with no offload hints needed.
 */
static int
linux_write_frame(const uint8_t *buf, size_t len)
{
    struct virtio_net_hdr vnet_hdr;
    memset(&vnet_hdr, 0, sizeof(vnet_hdr));
    struct iovec iov[2];
    iov[0].iov_base = &vnet_hdr;
    iov[0].iov_len  = sizeof(vnet_hdr);
    iov[1].iov_base = (void *)buf;
    iov[1].iov_len  = len;

    ssize_t written = writev(g_tap_fd, iov, 2);
    if (written < 0) {
        perror("netif write failed");
        return (-1);
    }
    return (0);
}

static int
linux_set_mac(const uint8_t mac[6])
{
    set_mac_macvtap(mac);
    return (0);
}

static int
linux_get_mac(uint8_t mac[6])
{
    return get_mac_macvtap(mac) == 0 ? 0 : -1;
}

static const struct netif_backend linux_backend = {
    .open             = linux_open,
    .close            = linux_close,
    .pollable_fd      = linux_pollable_fd,
    .read_frame       = linux_read_frame,
    .write_frame      = linux_write_frame,
    .set_mac          = linux_set_mac,
    .get_mac          = linux_get_mac,
    .ensure_privilege = linux_ensure_privilege,
};

const struct netif_backend *
netif_backend_get(void)
{
    return &linux_backend;
}
