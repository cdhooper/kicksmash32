#ifndef _HOSTSMASH_H
#define _HOSTSMASH_H

#define SWAP16(x) __builtin_bswap16(x)
#define SWAP32(x) __builtin_bswap32(x)
#define SWAP64(x) __builtin_bswap64(x)

typedef unsigned int uint;

uint send_msg(void *buf, uint len, uint *status);
void time_delay_msec(int msec);

#endif /* _HOSTSMASH_H */
