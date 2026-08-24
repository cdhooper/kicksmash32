#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>

#ifndef __MINGW32__
#include <sys/wait.h>
#endif

#include "../fw/crc32.h"
#include "../fw/smash_cmd.h"
#include "../amiga/host_cmd.h"
#include "../fw/version.h"
#include "hostsmash.h"
#include "hostsmash_net.h"
#include "hostsmash_netif.h"

static volatile uint netif_getmac = 0; // GETMAC pending from device
static uint      netif_up = 0;         // Network interface is up
static int       netif_write_fd = -1;  // Pipe end to write TO the gateway
static int       netif_capture = 0;    // Capture packets from net
static uint8_t  netif_hw_mac[6];  // Cached MAC from lower level
#ifndef __MINGW32__
static int       netif_read_fd = -1;   // Pipe end to read FROM the gateway
static pid_t     gateway_pid = -1;     // PID of the gateway child process
static pthread_t read_thread;
#endif

/* ---- Queue node and state ------------------------------------------- */
typedef struct pkt_node {
    struct pkt_node *next;
    uint             pktlen;
    hm_nreadwrite_t  hdr;     // Kicksmash Message header
    uint8_t          pkt[];   /* payload in the same allocation */
} pkt_node_t;

typedef struct {
    pkt_node_t     *head;      /* oldest packet (next to dequeue) */
    pkt_node_t     *tail;      /* newest packet (last queued)     */
    uint             count;
    pthread_mutex_t  lock;     /* remove/no-op if single-threaded */
} pkt_queue_t;

static pkt_queue_t sm_pkt_q = {
    .head = NULL,
    .tail = NULL,
    .count = 0,
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

/* ---- Init / teardown ---------------------------------------------- */

static void
sm_queue_init(pkt_queue_t *q)
{
    q->head = NULL;
    q->tail = NULL;
    q->count = 0;
    pthread_mutex_init(&q->lock, NULL);
}

static void
sm_queue_destroy(pkt_queue_t *q)
{
    pkt_node_t *n;

    pthread_mutex_lock(&q->lock);
    while (q->head != NULL) {
        n = q->head;
        q->head = n->next;
        free(n);           /* single free: header + payload together */
    }
    q->tail = NULL;
    q->count = 0;
    pthread_mutex_unlock(&q->lock);
    pthread_mutex_destroy(&q->lock);
}

void
sm_init_queues(void)
{
    sm_queue_init(&sm_pkt_q);
}

void
sm_destroy_queues(void)
{
    sm_queue_destroy(&sm_pkt_q);
}

#ifndef __MINGW32__
/*
 * sm_queue_packet() enqueues a single network packet to be received by
 *                   a subsequent sm_nread().
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

    if (pkt == NULL || pktlen == 0)
        return (-1);

    node = malloc(sizeof (*node) + pktlen);
    if (node == NULL)
        return (-1);

    memcpy(node->pkt, pkt, pktlen);
    node->pktlen = pktlen;
    node->next = NULL;

    pthread_mutex_lock(&sm_pkt_q.lock);

    if (sm_pkt_q.tail == NULL) {
        sm_pkt_q.head = node;
        sm_pkt_q.tail = node;
    } else {
        sm_pkt_q.tail->next = node;
        sm_pkt_q.tail = node;
    }
    sm_pkt_q.count++;

    pthread_mutex_unlock(&sm_pkt_q.lock);

    return (0);
}
#endif

/*
 * sm_dequeue_packet() removes the oldest network packet and copies its
 *                     payload out to caller-supplied storage.
 *
 * ---- Dequeue ----------------------------------------------------------
 * `buf` must be at least *buflen bytes; on return *buflen is set to the
 * actual packet length copied. Returns 0 on success, -1 if the queue is
 * empty, -2 if buf is too small (in which case *buflen is set to the
 * required size and nothing is dequeued).
 */
static pkt_node_t *
sm_dequeue_packet(void)
{
    pkt_node_t *node;

    pthread_mutex_lock(&sm_pkt_q.lock);

    if (sm_pkt_q.head == NULL) {
        pthread_mutex_unlock(&sm_pkt_q.lock);
        return (NULL);
    }

    node = sm_pkt_q.head;

    sm_pkt_q.head = node->next;
    if (sm_pkt_q.head == NULL)
        sm_pkt_q.tail = NULL;
    sm_pkt_q.count--;

    pthread_mutex_unlock(&sm_pkt_q.lock);

    return (node);
}

/* ---- Helpers ------------------------------------------------------------ */

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
    for (pos = 0; pos < packet_len; pos++)
        printf(" %02x", data[pos]);
    printf("\n");
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
    printf("Send GETMAC\n");
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

#ifndef __MINGW32__
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
            fprintf(stderr, "GET MAC\n");
            break;
        }
        case HS_NETIF_CMD_SETMAC:   // Set MAC
            fprintf(stderr, "SET MAC\n");
            memcpy(netif_hw_mac, data + 1, 6);
            netif_getmac = 0;  // Mark as no longer waiting
            break;
    }
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
    return (NULL);
}
#endif

/*
 * netif_start() opens the pipe to the virtual network interface and
 *               starts the frame reader thread.
 */
uint
netif_start(void)
{
#ifdef __MINGW32__
    perror("No Windows support for networking");
    return (1);
#else
    int parent_to_child[2];
    int child_to_parent[2];

    if (netif_up)
        return (0);

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

        execlp("hostsmash_netif", "hostsmash_netif", (char *)NULL);

        perror("Failed launching hostsmash_netif");
        exit(1);
    }

    close(parent_to_child[0]);
    close(child_to_parent[1]);

    netif_write_fd = parent_to_child[1];
    netif_read_fd  = child_to_parent[0];
    netif_up = 1;

    /* Start the frame reader thread */
    if (pthread_create(&read_thread, NULL, netif_read_thread, NULL) != 0) {
        perror("Error creating netif reader thread");
        close(netif_write_fd);
        close(netif_read_fd);
        kill(gateway_pid, SIGTERM);
        waitpid(gateway_pid, NULL, 0);
        netif_write_fd = -1;
        netif_read_fd  = -1;
        gateway_pid    = -1;
        netif_up       = 0;
        return (1);
    }
    netif_request_mac();

    return (0);
#endif
}

/*
 * netif_stop() closes the pipe to the virtual network interface,
 *              stopping the reader thread.
 */
void
netif_stop(void)
{
    if (!netif_up)
        return;

#ifdef __MINGW32__
#else
    /* Tell the thread to shut down */
    netif_capture = 0;
    netif_up = 0;

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

    if (gateway_pid > 0) {
        kill(gateway_pid, SIGTERM);
        waitpid(gateway_pid, NULL, 0);
        gateway_pid = -1;
    }

    /* Wait for thread to terminate */
    pthread_join(read_thread, NULL);
#endif
}

uint
sm_nopen(hm_nopenhandle_t *hm, uint *status)
{
    printf("NOPEN\n");
    netif_request_mac();
    hm->hm_hdr.km_op |= KM_OP_REPLY;
    hm->hm_hdr.km_status = KM_STATUS_OK;
    return (send_msg(hm, sizeof (*hm), status));
}

uint
sm_nclose(hm_nopenhandle_t *hm, uint *status)
{
    printf("NCLOSE\n");
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
    uint     hm_length = SWAP32(hm->hm_length);
    uint8_t *ndata     = (uint8_t *)(hm + 1);

    printf("NWRITE(l=%u rl=%u)", hm_length, rxlen);
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
    printf("\n");
#endif

    netif_write(hm_length, hm_length, ndata);

    hm->hm_hdr.km_op |= KM_OP_REPLY;
    hm->hm_hdr.km_status = KM_STATUS_OK;
    return (send_msg(hm, sizeof (*hm), status));
}

uint
sm_nread(hm_nreadwrite_t *hm, uint *status)
{
    static pkt_node_t *node;

    if (netif_start()) {
        node->hdr.hm_hdr.km_op |= KM_OP_REPLY;
        hm->hm_hdr.km_status    = KM_STATUS_NOEXIST;
        return (send_msg(hm, sizeof (*hm), status));
    }
    netif_capture = 1;
    node = sm_dequeue_packet();
    if (node != NULL) {
        uint rc;
        printf("NREAD(l=%u)", node->pktlen);
        memcpy(&node->hdr, &hm->hm_hdr, sizeof (node->hdr));
        node->hdr.hm_hdr.km_op    |= KM_OP_REPLY;
        node->hdr.hm_hdr.km_status = KM_STATUS_OK;
        node->hdr.hm_length        = node->pktlen;
#ifdef DUMP_PACKET
        dump_packet(node->pkt, node->pktlen);
#else
        printf("\n");
#endif
        rc = send_msg(&node->hdr, sizeof (node->hdr) + node->pktlen, status);
        free(node);
        return (rc);
    } else {
        if (!netif_up && netif_start()) {
            hm->hm_hdr.km_status = KM_STATUS_NOEXIST;
            return (send_msg(hm, sizeof (*hm), status));
        }

        hm->hm_hdr.km_status = KM_STATUS_EOF;
        return (send_msg(hm, sizeof (*hm), status));
    }
}

uint
sm_ngetmac(hm_nmac_t *hm, uint *status)
{
    uint rc;
    uint8_t *mac;

printf("sm_ngetmac\n");
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

    printf("GETMAC %02x:%02x:%02x:%02x:%02x:%02x\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    hm->hm_hdr.km_status = KM_STATUS_OK;
    rc = send_msg(hm, sizeof (*hm), status);
    if (rc != KM_STATUS_OK) {
        printf("Failed to send reply: %d\n", rc);
        /* Try again */
        rc = send_msg(hm, sizeof (*hm), status);
        if (rc != KM_STATUS_OK)
            printf("Failed again to send reply: %d\n", rc);
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

    printf("SETMAC %02x:%02x:%02x:%02x:%02x:%02x\n",
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
