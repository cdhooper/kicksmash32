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

uint recv_msg(uint tag, void *buf, uint bufsize, uint *rx_status, uint *rxlen);
uint send_msg(void *buf, uint len, uint *status);
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
