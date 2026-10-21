/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2026.
 *
 * ---------------------------------------------------------------------
 *
 * Hostsmash Network services for the Amiga.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <time.h>

#ifdef __MINGW32__
#include <windows.h>
#define handle_t winhandle_t
#include <shlwapi.h>
#include <io.h>         /* _open_osfhandle */
typedef unsigned int uint;
#else
#include <sys/wait.h>
#endif

#include "../fw/crc32.h"
#include "../fw/smash_cmd.h"
#include "../amiga/host_cmd.h"
#include "../fw/version.h"
#include "hostsmash.h"
#include "hostsmash_net.h"
#include "hostsmash_netif.h"

const char      *netif_arg;            // Arguments for the netif program
                                       //   (hostsmash -n), split at spaces;
                                       //   NULL = none

static volatile uint netif_getmac = 0; // GETMAC pending from device
static volatile uint netif_up = 0;     // Network interface is up
static int       netif_write_fd = -1;  // Pipe end to write TO the gateway
static int       netif_read_fd = -1;   // Pipe end to read FROM the gateway
static int       netif_capture = 0;    // Capture packets from net
static uint8_t   netif_hw_mac[6];      // Cached MAC from lower level
static pthread_t read_thread;
static pthread_t fwd_thread;
static uint      netif_threads_active = 0;  // Threads created, not joined
#ifdef __MINGW32__
static HANDLE    gateway_process = NULL;  // Handle of the gateway child process
#else
static pid_t     gateway_pid = -1;        // PID of the gateway child process
#endif

/* ---- Queue node and state ------------------------------------------- */
typedef struct pkt_node {
    struct pkt_node *next;
    uint             pktlen;
    hm_nreadwrite_t  hdr;     // Kicksmash Message header
    uint8_t          pkt[];   /* payload in the same allocation */
} pkt_node_t;

/*
 * Extra bytes allocated after each packet payload. send_ks_cmd_core()
 * sends the payload rounded up to a 4-byte multiple, and mem16_swap()
 * touches the byte following an odd length.
 */
#define PKT_NODE_PAD   4

/*
 * Maximum packets held for the Amiga. Packets keep arriving from the
 * network while forwarding is suspended (no recent KM_OP_NREAD), so
 * the oldest are discarded beyond this depth.
 */
#define SM_PKT_Q_MAX   32

/*
 * Forwarding is suspended if the most recent KM_OP_NREAD from the Amiga
 * is older than this.
 */
#define NREAD_TAG_EXPIRE_MSEC  20000

typedef struct {
    pkt_node_t     *head;      /* oldest packet (next to dequeue) */
    pkt_node_t     *tail;      /* newest packet (last queued)     */
    uint             count;
    pthread_mutex_t  lock;     /* also protects the fwd_* state below */
} pkt_queue_t;

static pkt_queue_t sm_pkt_q = {
    .head = NULL,
    .tail = NULL,
    .count = 0,
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

/*
 * Network forwarding state, protected by sm_pkt_q.lock.
 *
 * fwd_cv is signaled when net_forward_thread() might have something new
 * to do: a packet was queued, a new KM_OP_NREAD tag was recorded, or
 * the network interface is going down.
 */
static pthread_cond_t  fwd_cv = PTHREAD_COND_INITIALIZER;
static hm_nreadwrite_t fwd_req;            // Most recent KM_OP_NREAD request
static uint64_t        fwd_req_msec = 0;   // When fwd_req was received
static uint            fwd_req_valid = 0;  // fwd_req has been recorded

/* ---- Helpers ---------------------------------------------------------- */

/*
 * net_time_msec() returns a monotonic millisecond timestamp, unaffected
 *                 by changes to the wall clock.
 */
static uint64_t
net_time_msec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

#ifdef TIMESTAMP_DEBUG_OUTPUT
#ifdef __MINGW32__
static uint64_t
get_timestamp_ns(void)
{
    FILETIME ft;
    ULARGE_INTEGER uli;

    // 1. Get current system time (100-nanosecond intervals since Jan 1, 1601)
    GetSystemTimeAsFileTime(&ft);

    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;

    // 2. Convert Windows Epoch (1601) to Unix Epoch (1970)
    // There are 11,644,473,600 seconds between 1601 and 1970.
    // In 100-nanosecond intervals, that is 116,444,736,000,000,000.
    uint64_t windows_intervals = uli.QuadPart;
    uint64_t unix_intervals = windows_intervals - 116444736000000000ULL;

    // 3. Convert 100-nanosecond intervals to pure nanoseconds
    return unix_intervals * 100ULL;
}
#else  /* Linux and MacOS */
static uint64_t
get_timestamp_ns(void)
{
    struct timespec ts;
    // CLOCK_REALTIME gets the actual calendar time since the Unix Epoch
    clock_gettime(CLOCK_REALTIME, &ts);

    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}
#endif

static char *
get_timestamp_str(void)
{
    static char     buf[32];
    static uint64_t first = 0;
    uint64_t        ts = get_timestamp_ns();
    uint            us;
    if (first == 0)
        first = ts;
    ts -= first;
    us = ts / 1000ULL;
    sprintf(buf, "%4lu.%06lu", us / 1000000UL, us % 1000000UL);
    return (buf);
}
#endif

/*
 * fwd_tag_fresh_locked() returns non-zero if a KM_OP_NREAD tag has been
 *                        recorded and it is recent enough to be used.
 *                        Caller must hold sm_pkt_q.lock.
 */
static uint
fwd_tag_fresh_locked(void)
{
    if (!fwd_req_valid)
        return (0);
    return ((net_time_msec() - fwd_req_msec) <= NREAD_TAG_EXPIRE_MSEC);
}

/*
 * fwd_wake_all() wakes net_forward_thread() so that it re-evaluates
 *                its state (used on shutdown).
 */
static void
fwd_wake_all(void)
{
    pthread_mutex_lock(&sm_pkt_q.lock);
    pthread_cond_broadcast(&fwd_cv);
    pthread_mutex_unlock(&sm_pkt_q.lock);
}

/* ---- Init / teardown ---------------------------------------------- */

/*
 * The queue lock and condition variable are statically initialized,
 * and may already be in use by the netif threads, so there is nothing
 * to (re)initialize here.
 */
void
sm_init_queues(void)
{
}

void
sm_destroy_queues(void)
{
    pkt_node_t *n;

    netif_stop();  // Stop and join the threads which use the queue

    pthread_mutex_lock(&sm_pkt_q.lock);
    while (sm_pkt_q.head != NULL) {
        n = sm_pkt_q.head;
        sm_pkt_q.head = n->next;
        free(n);           /* single free: header + payload together */
    }
    sm_pkt_q.tail = NULL;
    sm_pkt_q.count = 0;
    fwd_req_valid = 0;
    pthread_mutex_unlock(&sm_pkt_q.lock);
}

/*
 * sm_queue_packet() enqueues a single network packet to be forwarded to
 *                   the Amiga by net_forward_thread(), and wakes that
 *                   thread.
 *
 * ---- Enqueue --------------------------------------------------------
 * Single allocation: sizeof(pkt_node_t) + pktlen, with the payload
 * copied directly into the flexible array member.
 * Returns 0 on success, -1 on allocation failure.
 */
static int
sm_queue_packet(uint8_t *pkt, uint pktlen)
{
    pkt_node_t *node;
    pkt_node_t *drop = NULL;

    if (pkt == NULL || pktlen == 0)
        return (-1);

    node = malloc(sizeof (*node) + pktlen + PKT_NODE_PAD);
    if (node == NULL)
        return (-1);

    memcpy(node->pkt, pkt, pktlen);
    memset(node->pkt + pktlen, 0, PKT_NODE_PAD);
    node->pktlen = pktlen;
    node->next = NULL;

    pthread_mutex_lock(&sm_pkt_q.lock);

    if (sm_pkt_q.count >= SM_PKT_Q_MAX) {
        /* Discard oldest packet */
        drop = sm_pkt_q.head;
        sm_pkt_q.head = drop->next;
        if (sm_pkt_q.head == NULL)
            sm_pkt_q.tail = NULL;
        sm_pkt_q.count--;
    }

    if (sm_pkt_q.tail == NULL) {
        sm_pkt_q.head = node;
        sm_pkt_q.tail = node;
    } else {
        sm_pkt_q.tail->next = node;
        sm_pkt_q.tail = node;
    }
    sm_pkt_q.count++;
    pthread_cond_signal(&fwd_cv);

    pthread_mutex_unlock(&sm_pkt_q.lock);

    if (drop != NULL)
        free(drop);

    return (0);
}

/*
 * sm_dequeue_packet_locked() removes and returns the oldest network
 *                            packet, or NULL if the queue is empty.
 *                            Caller must hold sm_pkt_q.lock.
 */
static pkt_node_t *
sm_dequeue_packet_locked(void)
{
    pkt_node_t *node = sm_pkt_q.head;

    if (node == NULL)
        return (NULL);

    sm_pkt_q.head = node->next;
    if (sm_pkt_q.head == NULL)
        sm_pkt_q.tail = NULL;
    sm_pkt_q.count--;
    node->next = NULL;

    return (node);
}

/*
 * sm_requeue_packet_locked() puts a packet back at the head of the queue
 *                            (it was dequeued but could not be sent).
 *                            Caller must hold sm_pkt_q.lock.
 */
static void
sm_requeue_packet_locked(pkt_node_t *node)
{
    node->next = sm_pkt_q.head;
    sm_pkt_q.head = node;
    if (sm_pkt_q.tail == NULL)
        sm_pkt_q.tail = node;
    sm_pkt_q.count++;
}

#if 0
static uint
sm_queue_depth(void)
{
    uint n;
    pthread_mutex_lock(&sm_pkt_q.lock);
    n = sm_pkt_q.count;
    pthread_mutex_unlock(&sm_pkt_q.lock);
    return (n);
}

static int
sm_queue_is_empty(void)
{
    return (sm_queue_depth() == 0);
}
#endif

#undef DUMP_PACKET
#ifdef DUMP_PACKET
static void
dump_packet(uint8_t *data, uint packet_len)
{
    uint pos;
    if (!debug_net)
        return;
    for (pos = 0; pos < packet_len; pos++)
        netprintf(" %02x", data[pos]);
    netprintf("\n");
}
#endif

#undef NETSEND_DEBUG
#ifdef NETSEND_DEBUG
/*
 * udp_csum() computes and places the UDP checksum of an
 *            Ethernet + IPv4 + UDP packet. The result is inserted
 *            into the packet.
 */
static void
udp_csum(uint8_t *packet, uint packet_len)
{
    /* Minimum: 14 (Eth) + 20 (IP) + 8 (UDP) */
    if (packet_len < 42)
        return;

    /* EtherType must be IPv4 */
    if (packet[12] != 0x08 || packet[13] != 0x00)
        return;

    uint8_t *ip = packet + 14;
    uint8_t  ihl = (ip[0] & 0x0F) * 4;          /* IP header length in bytes */
    if (ihl < 20 || packet_len < 14 + ihl + 8)
        return;

    /* Protocol must be UDP (17) */
    if (ip[9] != 17)
        return;

    uint8_t *udp = ip + ihl;

    /* UDP length field (header + data) */
    uint16_t udp_len = ((uint16_t)udp[4] << 8) | udp[5];
    if (udp_len < 8 || 14 + ihl + udp_len > packet_len)
        return;

    /* Zero the checksum field before calculation */
    udp[6] = 0;
    udp[7] = 0;

    /* ------------------------------------------------------------------ */
    /* One's-complement sum                                               */
    /* ------------------------------------------------------------------ */
    uint32_t sum = 0;

    /* Pseudo-header: src IP, dst IP, zero, protocol, UDP length */
    sum += ((uint16_t)ip[12] << 8) | ip[13];   /* src IP high */
    sum += ((uint16_t)ip[14] << 8) | ip[15];   /* src IP low  */
    sum += ((uint16_t)ip[16] << 8) | ip[17];   /* dst IP high */
    sum += ((uint16_t)ip[18] << 8) | ip[19];   /* dst IP low  */
    sum += 17;                                 /* protocol = UDP */
    sum += udp_len;                            /* UDP length    */

    /* UDP header + data */
    const uint8_t *p   = udp;
    size_t         len = udp_len;

    while (len > 1) {
        sum += ((uint16_t)p[0] << 8) | p[1];
        p   += 2;
        len -= 2;
    }
    if (len)                                   /* odd byte left */
        sum += (uint16_t)p[0] << 8;

    /* Fold carries */
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    uint16_t csum = (uint16_t)(~sum);

    /* RFC 768: if the computed checksum is 0, store 0xFFFF */
    if (csum == 0)
        csum = 0xFFFF;

    /* Write the result back (network byte order) */
    udp[6] = (uint8_t)(csum >> 8);
    udp[7] = (uint8_t)(csum & 0xFF);
}

/*
 * ip_csum() computes and places the IP checksum of an Ethernet + IPv4 packet.
 *           The result is inserted *            into the packet.
 */
static void
ip_csum(uint8_t *packet, uint packet_len)
{
    /* Minimum: 14 (Eth) + 20 (IP header) */
    if (packet_len < 34)
        return;

    /* EtherType must be IPv4 (0x0800) */
    if (packet[12] != 0x08 || packet[13] != 0x00)
        return;

    uint8_t *ip = packet + 14;

    /* Version must be 4, IHL at least 5 (20 bytes) */
    if ((ip[0] >> 4) != 4)
        return;

    uint8_t ihl = (ip[0] & 0x0F) * 4;   /* header length in bytes */
    if (ihl < 20 || packet_len < 14 + ihl)
        return;

    /* Zero the checksum field (bytes 10-11) before calculation */
    ip[10] = 0;
    ip[11] = 0;

    /* ------------------------------------------------------------------ */
    /* One's-complement sum over the IP header only                       */
    /* ------------------------------------------------------------------ */
    uint32_t sum = 0;
    const uint8_t *p = ip;
    size_t len = ihl;

    while (len > 1) {
        sum += ((uint16_t)p[0] << 8) | p[1];
        p   += 2;
        len -= 2;
    }
    if (len)                            /* should never happen (IHL is even) */
        sum += (uint16_t)p[0] << 8;

    /* Fold carries */
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    uint16_t csum = (uint16_t)(~sum);

    /* Write the result back (network byte order) */
    ip[10] = (uint8_t)(csum >> 8);
    ip[11] = (uint8_t)(csum & 0xFF);
}

static void
ip_set_len(uint8_t *packet, uint packet_len)
{
    /* Minimum viable frame: 14 (Eth) + 20 (IP) */
    if (packet_len < 34)
        return;

    /* EtherType must be IPv4 (0x0800) */
    if (packet[12] != 0x08 || packet[13] != 0x00)
        return;

    uint8_t *ip = packet + 14;

    /* Version must be 4 */
    if ((ip[0] >> 4) != 4)
        return;

    uint8_t ihl = (ip[0] & 0x0F) * 4;
    if (ihl < 20 || packet_len < 14 + ihl)
        return;

    /* IP Total Length = everything after the Ethernet header */
    uint16_t ip_len = (uint16_t)(packet_len - 14);

    /* Write Total Length in network byte order (bytes 2-3 of IP header) */
    ip[2] = (uint8_t)(ip_len >> 8);
    ip[3] = (uint8_t)(ip_len & 0xFF);
}

static void
udp_set_len(uint8_t *packet, uint packet_len)
{
    /* Minimum: 14 (Eth) + 20 (IP) + 8 (UDP) */
    if (packet_len < 42)
        return;

    /* EtherType must be IPv4 (0x0800) */
    if (packet[12] != 0x08 || packet[13] != 0x00)
        return;

    uint8_t *ip = packet + 14;

    /* Version must be 4 */
    if ((ip[0] >> 4) != 4)
        return;

    uint8_t ihl = (ip[0] & 0x0F) * 4;   /* IP header length in bytes */
    if (ihl < 20 || packet_len < 14 + ihl + 8)
        return;

    /* Protocol must be UDP (17) */
    if (ip[9] != 17)
        return;

    uint8_t *udp = ip + ihl;

    /*
     * UDP Length = everything from the start of the UDP header to
     * the end of the frame
     */
    uint16_t udp_len = (uint16_t)(packet_len - 14 - ihl);

    /* Write Length in network byte order (bytes 4-5 of UDP header) */
    udp[4] = (uint8_t)(udp_len >> 8);
    udp[5] = (uint8_t)(udp_len & 0xFF);
}
#endif /* NETSEND_DEBUG */

/*
 * netif_write() performs a packet write to the network pipe
 */
static void
netif_write(uint xmit_len, uint frame_len, uint8_t *frame)
{
    if (!netif_up || (netif_write_fd < 0))
        return;

    uint16_t len_prefix = (uint16_t) xmit_len;

    int written;
    written = write(netif_write_fd, &len_prefix, sizeof (len_prefix));
    if (written < 0)
        perror("write(tap_fd) len failed");
    write(netif_write_fd, frame, frame_len);
    if (written < 0)
        perror("write(tap_fd) frame failed");
}

static void
netif_request_mac(void)
{
    uint8_t cmd_mac = HS_NETIF_CMD_GETMAC;
    netprintf("Send GETMAC\n");
    netif_getmac = 1;
    netif_write(0xff01, sizeof (cmd_mac), &cmd_mac);
#if 0
    for (int i = 0; i < 200; i++) {
        time_delay_msec(10);
        if (netif_getmac == 0)
            break;
    }
    if (netif_getmac) {
        hm->hm_hdr.km_status = KM_STATUS_FAIL;
        return (send_msg(hm, sizeof (hm->hm_hdr), status));
    }
#endif
}

static void
handle_cmd(uint8_t *data, uint len)
{
#ifdef DUMP_PACKET
    dump_packet(data, len);
#endif
    switch (data[0]) {              // First byte of data is always command
        case HS_NETIF_CMD_NOP:      // NOP
            break;
        case HS_NETIF_CMD_GETMAC: {   // Get MAC
            uint8_t cmd_mac[8];
            cmd_mac[0] = HS_NETIF_CMD_SETMAC;
            memcpy(&cmd_mac[1], netif_hw_mac, 6);
            netif_write(0xff07, sizeof (cmd_mac), cmd_mac);
            netprintf("GET MAC\n");
            break;
        }
        case HS_NETIF_CMD_SETMAC:   // Set MAC
            memcpy(netif_hw_mac, data + 1, 6);
            netprintf("SET MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                      netif_hw_mac[0], netif_hw_mac[1], netif_hw_mac[2],
                      netif_hw_mac[3], netif_hw_mac[4], netif_hw_mac[5]);
            netif_getmac = 0;  // Mark as no longer waiting
            break;
    }
}

/*
 * netif_mark_down() tears down pipe/process state after the netif
 *                   helper has gone away (EOF or read error on the
 *                   pipe), so that a subsequent netif_start() call
 *                   will treat the interface as down and retry
 *                   spawning the helper, instead of silently writing
 *                   into a dead pipe forever.
 *
 * This is called from *inside* netif_read_thread() itself on its way
 * out, so unlike netif_stop() it must NOT pthread_join(read_thread) --
 * that would be a self-join deadlock. Joining both netif threads is
 * left to netif_threads_join(), called by the next netif_start() or by
 * netif_stop().
 */
static void
netif_mark_down(void)
{
    if (!netif_up)
        return;

    netif_capture = 0;
    netif_up = 0;
    fwd_wake_all();  // net_forward_thread() exits on seeing !netif_up

    if (netif_write_fd >= 0) {
        close(netif_write_fd);
        netif_write_fd = -1;
    }
    if (netif_read_fd >= 0) {
        close(netif_read_fd);
        netif_read_fd = -1;
    }

#ifdef __MINGW32__
    if (gateway_process != NULL) {
        CloseHandle(gateway_process);
        gateway_process = NULL;
    }
#else
    if (gateway_pid > 0) {
        /*
         * Reap opportunistically without blocking -- we're running
         * on the reader thread and the helper may already be gone
         * (that's how we got here) or may still be exiting.
         */
        waitpid(gateway_pid, NULL, WNOHANG);
        gateway_pid = -1;
    }
#endif
}

/*
 * netif_read_thread() is a thread function which performs packet reads from
 *                     the network pipe and queues those packets for
 *                     processing.
 */
static void *
netif_read_thread(void *arg)
{
    (void) arg;
    uint8_t buf[2048]; // Oversized to fit maximum MTU frame
    uint16_t frame_len;
    uint     read_len;

    /* Loop until the pipeline hits EOF or thread is terminated */
    while (netif_up) {
        /* Read the 2-byte frame length prefix */
        size_t bytes_read = 0;
        while (bytes_read < sizeof (frame_len)) {
            ssize_t r = read(netif_read_fd, ((char *)&frame_len) + bytes_read,
                             sizeof (frame_len) - bytes_read);
            if (r <= 0)
                goto read_thread_exit;
            bytes_read += r;
        }

        if (frame_len <= sizeof (buf)) {
            read_len = frame_len;

            /* Read the raw Ethernet frame payload */
            bytes_read = 0;
            while (bytes_read < read_len) {
                ssize_t r = read(netif_read_fd, buf + bytes_read,
                                 read_len - bytes_read);
                if (r <= 0)
                    goto read_thread_exit;
                bytes_read += r;
            }

            if (netif_capture) {
                /* Send received packet to Amiga */
                sm_queue_packet(buf, read_len);
            }
        } else if ((frame_len >> 8) == 0xff) {
            /* Special case: command */
            read_len = frame_len & 0xff;

            /* Read the command payload */
            bytes_read = 0;
            while (bytes_read < read_len) {
                ssize_t r = read(netif_read_fd, buf + bytes_read,
                                 read_len - bytes_read);
                if (r <= 0)
                    goto read_thread_exit;
                bytes_read += r;
            }
            handle_cmd(buf, read_len);
        }
    }
read_thread_exit:
    netif_mark_down();
    return (NULL);
}

/*
 * net_forward_thread() is a thread function which asynchronously sends
 *                      network packets queued by netif_read_thread() to
 *                      the Amiga. Each packet is sent as a reply to the
 *                      most recent KM_OP_NREAD request recorded by
 *                      sm_nread() (using that request's tag).
 *
 * The thread blocks while there are no packets to send, and suspends
 * forwarding while the most recent KM_OP_NREAD is older than
 * NREAD_TAG_EXPIRE_MSEC (or none has been received yet). It exits under
 * the same condition as netif_read_thread(): when netif_up goes to 0.
 *
 * Sends to Kicksmash are coordinated with the main thread by the
 * Kicksmash send lock, taken at KS_SEND_PRIO_NET. If Kicksmash has no
 * room for a packet, the lock is released before the next attempt, so
 * the main thread can take it (and hold it until its own message is
 * sent).
 */
static void *
net_forward_thread(void *arg)
{
    (void) arg;

    while (netif_up) {
        pkt_node_t *node;
        uint        sendlen;

        /* Wait for a packet to send and a usable NREAD tag */
        pthread_mutex_lock(&sm_pkt_q.lock);
        while (netif_up &&
               ((sm_pkt_q.head == NULL) || !fwd_tag_fresh_locked())) {
            pthread_cond_wait(&fwd_cv, &sm_pkt_q.lock);
        }
        if (!netif_up) {
            pthread_mutex_unlock(&sm_pkt_q.lock);
            break;
        }
        node = sm_dequeue_packet_locked();
        pthread_mutex_unlock(&sm_pkt_q.lock);

        sendlen = sizeof (node->hdr) + node->pktlen;
        if (sendlen > SEND_MSG_MAX) {
            printf("NREAD: dropping %u byte packet (too large)\n",
                   node->pktlen);
            free(node);
            continue;
        }

        while (node != NULL) {
            uint status = KS_STATUS_FAIL;
            uint rc;

            if (!netif_up) {
                free(node);
                node = NULL;
                break;
            }

            /* Pick up the most recent NREAD tag */
            pthread_mutex_lock(&sm_pkt_q.lock);
            if (!fwd_tag_fresh_locked()) {
                /* Suspend forwarding; keep the packet for later */
                netprintf("NREAD tag expired; forwarding suspended\n");
                sm_requeue_packet_locked(node);
                pthread_mutex_unlock(&sm_pkt_q.lock);
                node = NULL;
                break;
            }
            memcpy(&node->hdr, &fwd_req, sizeof (node->hdr));
            pthread_mutex_unlock(&sm_pkt_q.lock);

            node->hdr.hm_hdr.km_op    |= KM_OP_REPLY;
            node->hdr.hm_hdr.km_status = KM_STATUS_OK;
            node->hdr.hm_length        = node->pktlen;

            ks_send_lock(KS_SEND_PRIO_NET);
            rc = send_msg_nowait(&node->hdr, sendlen, &status);
            ks_send_unlock();

            if ((rc == 0) && (status == KS_STATUS_OK)) {
                netprintf("NREAD(l=%u tag=%x)", node->pktlen,
                          SWAP16(node->hdr.hm_hdr.km_tag));
#ifdef DUMP_PACKET
                dump_packet(node->pkt, node->pktlen);
#else
                netprintf("\n");
#endif
                free(node);
                node = NULL;
                break;
            }
            if ((rc == 0) && (status == KS_STATUS_BADLEN)) {
                /*
                 * No room in the Kicksmash buffer right now. The lock
                 * has been released, so the main thread may get in
                 * before the next attempt.
                 */
                time_delay_msec(1);
                continue;
            }
            printf("NREAD: forward failed rc=%x status=%x; dropping packet\n",
                   rc, status);
            free(node);
            node = NULL;
        }
    }
    return (NULL);
}

/*
 * netif_threads_join() waits for the netif reader and network forwarding
 *                      threads to exit, if they were started. Must only
 *                      be called once netif_up is 0, and never from
 *                      either of those threads.
 */
static void
netif_threads_join(void)
{
    if (!netif_threads_active)
        return;
    fwd_wake_all();
    pthread_join(read_thread, NULL);
    pthread_join(fwd_thread, NULL);
    netif_threads_active = 0;
}

#ifdef __MINGW32__
/*
 * win_append_arg() appends one argument to a CreateProcess() command
 *                  line, quoted so that the child's C runtime turns it
 *                  back into exactly that one argv[] string, whatever
 *                  it contains (spaces, quotes, trailing backslashes).
 *                  Returns 0 on success, 1 if it does not fit.
 */
static uint
win_append_arg(char *cmd, size_t cmdsize, const char *arg)
{
    size_t pos = strlen(cmd);
    size_t bs  = 0;             // Backslashes seen but not yet written

    /* Worst case: every character doubled, plus space, quotes, and NIL */
    if (pos + 2 * strlen(arg) + 4 > cmdsize)
        return (1);

    cmd[pos++] = ' ';
    cmd[pos++] = '"';
    for (; *arg != '\0'; arg++) {
        if (*arg == '\\') {
            bs++;
        } else if (*arg == '"') {
            /* Backslashes ahead of a quote are doubled; the quote is escaped */
            for (; bs > 0; bs--) {
                cmd[pos++] = '\\';
                cmd[pos++] = '\\';
            }
            cmd[pos++] = '\\';
            cmd[pos++] = '"';
        } else {
            for (; bs > 0; bs--)
                cmd[pos++] = '\\';
            cmd[pos++] = *arg;
        }
    }
    /* Trailing backslashes sit ahead of the closing quote, so double them */
    for (; bs > 0; bs--) {
        cmd[pos++] = '\\';
        cmd[pos++] = '\\';
    }
    cmd[pos++] = '"';
    cmd[pos]   = '\0';
    return (0);
}
#endif

#define NETIF_MAX_ARGS 32  // Maximum arguments passed to hostsmash_netif

/*
 * netif_split_args() splits the hostsmash -n string into separate
 *                    arguments at spaces and tabs. The string is copied
 *                    into argbuf, which the returned argv[] entries point
 *                    into. Returns the number of arguments, or -1 if the
 *                    string is too long or has too many arguments.
 */
static int
netif_split_args(char *argbuf, size_t argbufsize, char *av[], int avmax)
{
    int   ac = 0;
    char *p  = argbuf;

    argbuf[0] = '\0';
    if (netif_arg == NULL)
        return (0);
    if (strlen(netif_arg) >= argbufsize)
        return (-1);
    strcpy(argbuf, netif_arg);

    while (1) {
        while ((*p == ' ') || (*p == '\t'))
            p++;
        if (*p == '\0')
            break;
        if (ac >= avmax)
            return (-1);
        av[ac++] = p;
        while ((*p != '\0') && (*p != ' ') && (*p != '\t'))
            p++;
        if (*p != '\0')
            *(p++) = '\0';
    }
    return (ac);
}

/*
 * netif_start() opens the pipe to the virtual network interface and
 *               starts the frame reader thread.
 *
 * The string given to hostsmash -n is split at spaces, and each piece is
 * handed to hostsmash_netif as a separate argument. For example,
 * "hostsmash -n macvtap" runs "hostsmash_netif macvtap", and
 * hostsmash -n "-d macvtap eth0" runs "hostsmash_netif -d macvtap eth0".
 * The arguments are checked by hostsmash_netif, not here.
 */
uint
netif_start(void)
{
    char  argbuf[512];
    char *nargv[NETIF_MAX_ARGS + 2];
    int   nargc;

    if (netif_up)
        return (0);

    nargc = netif_split_args(argbuf, sizeof (argbuf), nargv + 1,
                             NETIF_MAX_ARGS);
    if (nargc < 0) {
        fprintf(stderr, "Network arguments \"%s\" are too long\n", netif_arg);
        return (1);
    }
    nargv[0] = "hostsmash_netif";
    nargv[nargc + 1] = NULL;

    /*
     * If a previous instance went down on its own (helper exited), its
     * threads are finished or finishing; reap them before starting new
     * ones, so an old net_forward_thread() can never run alongside a
     * new one.
     */
    netif_threads_join();

#ifdef __MINGW32__
    /*
     * Windows has no fork()/pipe()/execlp(). The equivalent is:
     * CreatePipe() twice (one direction each), CreateProcessW() with
     * STARTUPINFO's hStdInput/hStdOutput pointed at the ends the child
     * should inherit, and then _open_osfhandle() to wrap our own ends
     * of the pipes as ordinary CRT file descriptors -- which is what
     * lets netif_write() and netif_read_thread() below stay identical
     * on every platform, since they only ever see plain fds.
     */
    HANDLE p2c_read = NULL, p2c_write = NULL;
    HANDLE c2p_read = NULL, c2p_write = NULL;
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    char    cmd[600] = "hostsmash_netif.exe";
    wchar_t cmdline[600];

    /*
     * Build the helper's command line. main()'s argv (and so netif_arg)
     * is in the ANSI code page, which is also what the helper's C
     * runtime converts its command line back into for its own argv.
     */
    for (int i = 1; i <= nargc; i++) {
        if (win_append_arg(cmd, sizeof (cmd), nargv[i])) {
            fprintf(stderr, "Network arguments \"%s\" are too long\n",
                    netif_arg);
            return (1);
        }
    }
    if (MultiByteToWideChar(CP_ACP, 0, cmd, -1, cmdline,
                            sizeof (cmdline) / sizeof (cmdline[0])) == 0) {
        fprintf(stderr, "Error converting hostsmash_netif command line\n");
        return (1);
    }

    memset(&sa, 0, sizeof (sa));
    sa.nLength = sizeof (sa);
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&p2c_read, &p2c_write, &sa, 0)) {
        fprintf(stderr, "Error creating transmission pipeline\n");
        return (1);
    }
    if (!CreatePipe(&c2p_read, &c2p_write, &sa, 0)) {
        fprintf(stderr, "Error creating reception pipeline\n");
        CloseHandle(p2c_read);
        CloseHandle(p2c_write);
        return (1);
    }

    /* Only the ends the child inherits should be inheritable */
    SetHandleInformation(p2c_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(c2p_read, HANDLE_FLAG_INHERIT, 0);

    memset(&si, 0, sizeof (si));
    si.cb = sizeof (si);
    si.dwFlags   |= STARTF_USESTDHANDLES;
    si.hStdInput  = p2c_read;
    si.hStdOutput = c2p_write;
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    memset(&pi, 0, sizeof (pi));

    if (!CreateProcessW(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL,
                         &si, &pi)) {
        fprintf(stderr, "Failed launching hostsmash_netif.exe (%lu)\n",
                (unsigned long)GetLastError());
        CloseHandle(p2c_read);
        CloseHandle(p2c_write);
        CloseHandle(c2p_read);
        CloseHandle(c2p_write);
        return (1);
    }

    CloseHandle(pi.hThread);
    gateway_process = pi.hProcess;

    /* The child owns its copies now; drop ours. */
    CloseHandle(p2c_read);
    CloseHandle(c2p_write);

    /*
     * _O_BINARY matters here: this is a binary length-prefixed framing
     * protocol (arbitrary byte values, including 0x0A), and the CRT's
     * default text mode would mangle it with newline translation.
     */
    netif_write_fd = _open_osfhandle((intptr_t)p2c_write,
                                     _O_WRONLY | _O_BINARY);
    netif_read_fd  = _open_osfhandle((intptr_t)c2p_read, _O_RDONLY | _O_BINARY);
    if (netif_write_fd < 0 || netif_read_fd < 0) {
        fprintf(stderr, "Error wrapping pipe handles as file descriptors\n");
        TerminateProcess(gateway_process, 1);
        CloseHandle(gateway_process);
        gateway_process = NULL;
        if (netif_write_fd >= 0)
            close(netif_write_fd);
        else
            CloseHandle(p2c_write);
        if (netif_read_fd >= 0)
            close(netif_read_fd);
        else
            CloseHandle(c2p_read);
        netif_write_fd = -1;
        netif_read_fd  = -1;
        return (1);
    }
#else
    int parent_to_child[2];
    int child_to_parent[2];

    if (pipe(parent_to_child) < 0) {
        perror("Error creating transmission pipeline");
        return (1);
    }
    if (pipe(child_to_parent) < 0) {
        perror("Error creating reception pipeline");
        close(parent_to_child[0]);
        close(parent_to_child[1]);
        return (1);
    }

    gateway_pid = fork();
    if (gateway_pid < 0) {
        perror("Forking connection worker failed");
        close(parent_to_child[0]);
        close(parent_to_child[1]);
        close(child_to_parent[0]);
        close(child_to_parent[1]);
        return (1);
    }

    if (gateway_pid == 0) {
        /* Connect child stdin to read end of parent_to_child */
        dup2(parent_to_child[0], STDIN_FILENO);
        /* Connect child stdout to write end of child_to_parent */
        dup2(child_to_parent[1], STDOUT_FILENO);

        close(parent_to_child[0]);
        close(parent_to_child[1]);
        close(child_to_parent[0]);
        close(child_to_parent[1]);

        execvp("hostsmash_netif", nargv);

        perror("Failed launching hostsmash_netif");
        exit(1);
    }

    close(parent_to_child[0]);
    close(child_to_parent[1]);

    netif_write_fd = parent_to_child[1];
    netif_read_fd  = child_to_parent[0];
#endif

    netif_up = 1;

    /* Start the network forwarding thread, then the frame reader thread */
    if (pthread_create(&fwd_thread, NULL, net_forward_thread, NULL) != 0) {
        perror("Error creating network forwarding thread");
        goto netif_start_fail;
    }
    if (pthread_create(&read_thread, NULL, netif_read_thread, NULL) != 0) {
        perror("Error creating netif reader thread");
        netif_up = 0;
        fwd_wake_all();
        pthread_join(fwd_thread, NULL);
netif_start_fail:
        netif_up = 0;
        close(netif_write_fd);
        close(netif_read_fd);
        netif_write_fd = -1;
        netif_read_fd  = -1;
#ifdef __MINGW32__
        TerminateProcess(gateway_process, 1);
        CloseHandle(gateway_process);
        gateway_process = NULL;
#else
        kill(gateway_pid, SIGTERM);
        waitpid(gateway_pid, NULL, 0);
        gateway_pid = -1;
#endif
        return (1);
    }
    netif_threads_active = 1;
    netif_request_mac();

    return (0);
}

/*
 * netif_stop() closes the pipe to the virtual network interface,
 *              stopping the reader and network forwarding threads.
 */
void
netif_stop(void)
{
    if (!netif_up) {
        /* May have gone down on its own; threads still need joining */
        netif_threads_join();
        return;
    }

    /* Tell the threads to shut down */
    netif_capture = 0;
    netif_up = 0;
    fwd_wake_all();

    /*
     * Close the network pipe by dropping stdin/stdout, forcing the
     * thread out of its blocking read.
     */
    if (netif_write_fd >= 0) {
        close(netif_write_fd);
        netif_write_fd = -1;
    }
    if (netif_read_fd >= 0) {
        close(netif_read_fd);
        netif_read_fd = -1;
    }

#ifdef __MINGW32__
    if (gateway_process != NULL) {
        if (WaitForSingleObject(gateway_process, 3000) == WAIT_TIMEOUT)
            TerminateProcess(gateway_process, 0);
        CloseHandle(gateway_process);
        gateway_process = NULL;
    }
#else
    if (gateway_pid > 0) {
        kill(gateway_pid, SIGTERM);
        waitpid(gateway_pid, NULL, 0);
        gateway_pid = -1;
    }
#endif

    /* Wait for threads to terminate */
    netif_threads_join();
}

uint
sm_nopen(hm_nopenhandle_t *hm, uint *status)
{
    netprintf("NOPEN\n");
    netif_request_mac();
    hm->hm_hdr.km_op |= KM_OP_REPLY;
    hm->hm_hdr.km_status = KM_STATUS_OK;
    return (send_msg(hm, sizeof (*hm), status));
}

uint
sm_nclose(hm_nopenhandle_t *hm, uint *status)
{
    netprintf("NCLOSE\n");
    hm->hm_hdr.km_op |= KM_OP_REPLY;
    hm->hm_hdr.km_status = KM_STATUS_OK;
    return (send_msg(hm, sizeof (*hm), status));
}

#ifdef NETSEND_DEBUG
    {
        const uint8_t server_mac[6] = { 0x00, 0x01, 0x02, 0x99, 0x98, 0x97 };
        const uint8_t broadcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
        if (memcmp(ndata, broadcast, sizeof (broadcast)) == 0) {
            /*
             * For now, assume it's a DHCP request.
             *
             * Byte offsets below assume a standard, option-free layout:
             *   Ethernet(14) + IPv4 no-options(20) + UDP(8) = 42 bytes
             *   of headers before the BOOTP/DHCP payload begins.
             * Within the BOOTP payload:
             *   op/htype/hlen/hops(4) + xid(4) -> xid at payload offset 4
             *   secs/flags/ciaddr/yiaddr/siaddr/giaddr(16) -> chaddr at
             *   payload offset 28.
             */
#define REQ_ETH_SRCMAC_OFF    6   /* client MAC, in the Ethernet header  */
#define REQ_BOOTP_OFF         42  /* eth(14) + ip(20) + udp(8)           */
#define REQ_BOOTP_XID_OFF     (REQ_BOOTP_OFF + 4)
#define RESP_BOOTP_XID_OFF    46  /* same layout in response_proto       */
#define RESP_BOOTP_GIADDR_OFF 66  /* 4 bytes, must be all zero            */
#define RESP_BOOTP_CHADDR_OFF 70  /* 16 bytes, first 6 = client MAC      */
            const uint8_t *client_mac = ndata + REQ_ETH_SRCMAC_OFF;
            const uint8_t *client_xid = ndata + REQ_BOOTP_XID_OFF;

            const uint8_t response_proto[] = {
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x0C, 0x29, 0xAA,
                0xBB, 0xCC, 0x08, 0x00, 0x45, 0x00, 0x01, 0x48, 0xAB, 0xCD,
                0x00, 0x00, 0x80, 0x11, 0xCC, 0x2E, 0xC0, 0xA8, 0x01, 0x01,
                0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x43, 0x00, 0x44, 0x01, 0x34,
                0x00, 0x00, 0x02, 0x01, 0x06, 0x00, 0x70, 0xB1, 0xC8, 0xED,
                0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0xA8,
                0x01, 0x7F, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, /* giaddr (4 bytes, offset 66-69) */
                0x00, 0x80, 0x10, 0x41, 0x53, 0x48, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                /*
                 * sname/file padding was 16 bytes short of the fixed
                 * 64+128-byte BOOTP layout, which put the magic cookie
                 * 16 bytes earlier than IP Total Length (328) and UDP
                 * Length (308) above call for. Pad it out so the header
                 * fields match the actual array length.
                 */
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00,
                0x63, 0x82, 0x53, 0x63,             // Magic cookie
                0x35, 0x01, 0x05,                   // Message Type = ACK
                0x01, 0x04, 0xFF, 0xFF, 0xFF, 0x00, // Subnet Mask 255.255.255.0
                0x03, 0x04, 0xC0, 0xA8, 0x01, 0x01, // Router 192.168.1.1
                0x06, 0x04, 0xC0, 0xA8, 0x01, 0x01, // DNS 192.168.1.1
                0x33, 0x04, 0x00, 0x01, 0x51, 0x80, // Lease Time 86400 sec
                0x36, 0x04, 0xC0, 0xA8, 0x01, 0x01, // Server ID: 192.168.1.1
                0xFF,                               // Option 255 (end of list)
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            };
            uint8_t response[sizeof (response_proto)];
            uint rlen = sizeof (response);
            memcpy(response, response_proto, sizeof (response));
            // memcpy(response, client_mac, 6);  // DST MAC = original SRC MAC
            memcpy(response + 6, server_mac, 6);

            /*
             * Echo the requester's xid so the DHCP client accepts this
             * reply as belonging to its current transaction.
             */
            memcpy(response + RESP_BOOTP_XID_OFF, client_xid, 4);

            /*
             * Echo the requester's own MAC into chaddr, and make sure
             * giaddr stays all-zero (this is not a relayed request).
             */
            memset(response + RESP_BOOTP_GIADDR_OFF, 0, 4);
            memset(response + RESP_BOOTP_CHADDR_OFF, 0, 16);
            memcpy(response + RESP_BOOTP_CHADDR_OFF, client_mac, 6);

            ip_set_len(response, rlen);
            udp_set_len(response, rlen);
            udp_csum(response, rlen);
            ip_csum(response, rlen);
            sm_queue_packet(response, rlen);
#undef REQ_ETH_SRCMAC_OFF
#undef REQ_BOOTP_OFF
#undef REQ_BOOTP_XID_OFF
#undef RESP_BOOTP_XID_OFF
#undef RESP_BOOTP_GIADDR_OFF
#undef RESP_BOOTP_CHADDR_OFF
        }
    }
#endif

uint
sm_nwrite(hm_nreadwrite_t *hm, uint *status, uint rxlen)
{
    uint     hm_length = hm->hm_length;
    uint8_t *ndata     = (uint8_t *)(hm + 1);

    netprintf("NWRITE(l=%u rl=%u)", hm_length, rxlen);
    if (netif_start()) {
        hm->hm_hdr.km_op |= KM_OP_REPLY;
        hm->hm_hdr.km_status = KM_STATUS_NOEXIST;
        return (send_msg(hm, sizeof (*hm), status));
    }
    netif_capture = 1;
    if (hm_length > rxlen)
        hm_length = rxlen;
    if (hm_length > 1522) // DEBUG (max ethernet non-Jumbo frame)
        hm_length = 1522;
#ifdef DUMP_PACKET
    dump_packet(ndata, hm_length);
#else
    netprintf("\n");
#endif

    netif_write(hm_length, hm_length, ndata);

    *status = KS_STATUS_OK;  // No reply sent; not an error
    return (0);
}

/*
 * sm_nread() handles KM_OP_NREAD from the Amiga.
 *
 * The request's header (including its tag) and the time it arrived are
 * recorded for net_forward_thread(), which asynchronously sends queued
 * network packets to the Amiga as replies carrying the most recent
 * recorded tag. Each KM_OP_NREAD also refreshes the expiration timer
 * which keeps forwarding active.
 *
 * If packets are waiting to be sent, no reply is sent here; the
 * forwarding thread is woken instead and its packets are the replies.
 * If nothing is waiting, a KM_STATUS_EOF reply is sent as before.
 */
uint
sm_nread(hm_nreadwrite_t *hm, uint *status)
{
    uint pending;

netprintf("sm_nread\n");
    *status = KS_STATUS_OK;  // No reply sent; not an error

    hm->hm_hdr.km_op |= KM_OP_REPLY;
    if (netif_start())
        return (0);

    netif_capture = 1;

    pthread_mutex_lock(&sm_pkt_q.lock);
    memcpy(&fwd_req, hm, sizeof (fwd_req));
    fwd_req_msec  = net_time_msec();
    fwd_req_valid = 1;
    pending = (sm_pkt_q.head != NULL);
    if (pending)
        pthread_cond_signal(&fwd_cv);
    pthread_mutex_unlock(&sm_pkt_q.lock);

    if (pending) {
        netprintf("NREAD tag=%x: %u pending, forwarding\n",
                  SWAP16(hm->hm_hdr.km_tag), sm_pkt_q.count);
    }
    return (0);
}

uint
sm_ngetmac(hm_nmac_t *hm, uint *status)
{
    uint rc;
    uint8_t *mac;

netprintf("sm_ngetmac\n");
    if (netif_start()) {
        hm->hm_hdr.km_status = KM_STATUS_NOEXIST;
        return (send_msg(hm, sizeof (*hm), status));
    }

    hm->hm_hdr.km_op |= KM_OP_REPLY;
    /*
     * Send this request to network interface and wait for reply.
     */
#if 0
    netif_request_mac();
#endif
    mac = hm->hm_mac;
    memcpy(mac, netif_hw_mac, 6);

    netprintf("GETMAC %02x:%02x:%02x:%02x:%02x:%02x\n",
              mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    hm->hm_hdr.km_status = KM_STATUS_OK;
    rc = send_msg(hm, sizeof (*hm), status);
    if (rc != KM_STATUS_OK) {
        netprintf("Failed to send reply: %d\n", rc);
        /* Try again */
        rc = send_msg(hm, sizeof (*hm), status);
        if (rc != KM_STATUS_OK)
            netprintf("Failed again to send reply: %d\n", rc);
    }
    return (rc);
}

uint
sm_nsetmac(hm_nmac_t *hm, uint *status)
{
    uint8_t *mac;
    uint8_t cmd_mac[7];
    uint    rc;
    hm->hm_hdr.km_op |= KM_OP_REPLY;
    hm->hm_hdr.km_status = KM_STATUS_OK;
    mac = hm->hm_mac;

    if (netif_start()) {
        hm->hm_hdr.km_status = KM_STATUS_NOEXIST;
        return (send_msg(hm, sizeof (*hm), status));
    }

    netprintf("SETMAC %02x:%02x:%02x:%02x:%02x:%02x\n",
              mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    cmd_mac[0] = HS_NETIF_CMD_SETMAC;
    memcpy(&cmd_mac[1], mac, 6);
    memcpy(netif_hw_mac, mac, 6);
    netif_write(0xff07, sizeof (cmd_mac), cmd_mac);

    rc = send_msg(hm, sizeof (hm->hm_hdr), status);
    if (rc != KM_STATUS_OK) {
        printf("Failed to send reply: %d\n", rc);
        /* Try again */
        rc = send_msg(hm, sizeof (hm->hm_hdr), status);
        if (rc != KM_STATUS_OK)
            printf("Failed again to send reply: %d\n", rc);
    }
    return (rc);
}
