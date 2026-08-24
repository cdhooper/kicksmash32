/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in August 2026.
 *
 * ---------------------------------------------------------------------
 *
 * Smash remote host network transfer and management functions.
 */

#ifndef _SM_NET_H
#define _SM_NET_H

typedef struct {
    uint8_t  dstmac[6];
    uint8_t  srcmac[6];
    uint16_t type;
} ethhdr_t;

uint sm_nservice(void);
uint sm_nopen(void);
uint sm_nclose(void);
uint sm_nwrite(ethhdr_t *hdr, void *buf, uint writelen, uint padded_header);
uint sm_nread(void **data, uint *readlen);
uint sm_ngetmac(uint8_t *mac);
uint sm_nsetmac(uint8_t *mac);

extern uint8_t sm_net_active;

#endif /* _SM_NET_H */
