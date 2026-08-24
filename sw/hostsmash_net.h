#ifndef _HOSTSMASH_NET_H
#define _HOSTSMASH_NET_H

uint sm_nopen(hm_nopenhandle_t *hm, uint *status);
uint sm_nclose(hm_nopenhandle_t *hm, uint *status);
uint sm_nwrite(hm_nreadwrite_t *hm, uint *status, uint rxlen);
uint sm_nread(hm_nreadwrite_t *hm, uint *status);
uint sm_ngetmac(hm_nmac_t *hm, uint *status);
uint sm_nsetmac(hm_nmac_t *hm, uint *status);

void sm_init_queues(void);
void sm_destroy_queues(void);
uint netif_start(void);
void netif_stop(void);

#endif /* _HOSTSMASH_NET_H */
