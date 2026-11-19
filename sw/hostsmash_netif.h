#ifndef _HOSTSMASH_NETIF_H
#define _HOSTSMASH_NETIF_H

#define HS_NETIF_CMD_NOP    0x00
#define HS_NETIF_CMD_GETMAC 0x01
#define HS_NETIF_CMD_SETMAC 0x02

/*
 * Largest frame carried on the hostsmash <-> hostsmash_netif pipe.
 * Each message is a 16-bit length prefix followed by that many bytes;
 * prefixes 0xff00-0xffff are commands with (prefix & 0xff) payload bytes.
 */
#define NETIF_MAX_FRAME     2000

#endif /* _HOSTSMASH_NETIF_H */
