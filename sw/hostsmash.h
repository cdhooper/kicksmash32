#ifndef _HOSTSMASH_H
#define _HOSTSMASH_H

#define SWAP16(x) __builtin_bswap16(x)
#define SWAP32(x) __builtin_bswap32(x)
#define SWAP64(x) __builtin_bswap64(x)

#define BIT(x) (1U << (x))

#ifdef __clang__
#define ATTRIBUTE_PRINTF __attribute__((format(printf, 1, 2)))
#else
#define ATTRIBUTE_PRINTF __attribute__((format(__gnu_printf__, 1, 2)))
#endif

typedef enum {
    RC_SUCCESS = 0,
    RC_FAILURE = 1,
    RC_TIMEOUT = 2,
} rc_t;

typedef unsigned int uint;

#define SEND_MSG_MAX 2000  // Largest single message sent to Kicksmash

/*
 * Kicksmash send lock priorities. Main thread replies and commands always
 * win over the network forwarding thread. The main thread's idle polling
 * for Amiga messages does not, but is let in after a short burst of
 * forwarded packets.
 */
#define KS_SEND_PRIO_MAIN 0  // Main thread replies and commands (highest)
#define KS_SEND_PRIO_NET  1  // Network forwarding thread
#define KS_SEND_PRIO_POLL 2  // Main thread idle poll (shares with NET)

void ks_send_lock(uint prio);
void ks_send_unlock(void);

uint recv_msg(uint tag, void *buf, uint bufsize, uint *rx_status, uint *rxlen);
uint send_msg(void *buf, uint len, uint *status);
uint send_msg_nowait(void *buf, uint len, uint *status);
void time_delay_msec(int msec);
int netprintf(const char *fmt, ...);
extern uint debug_net;
time_t get_utctime(time_t rawtime);
time_t get_localtime(time_t rawtime);
void err(int ec, const char *fmt, ...);
void errx(int ec, const char *fmt, ...);
void warn(const char *fmt, ...);
void warnx(const char *fmt, ...);
#endif /* _HOSTSMASH_H */
