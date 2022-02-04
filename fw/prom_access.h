/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2020.
 *
 * ---------------------------------------------------------------------
 *
 * EEPROM high level access code for MX29F1615 programmer.
 */

#ifndef _PROM_ACCESS_H
#define _PROM_ACCESS_H

rc_t prom_read(uint32_t addr, uint width, void *bufp);
rc_t prom_write(uint32_t addr, uint width, void *bufp);
rc_t prom_erase(uint mode, uint32_t addr, uint32_t len);
rc_t prom_read_binary(uint32_t addr, uint32_t len);
rc_t prom_write_binary(uint32_t addr, uint32_t len);
void prom_cmd(uint32_t addr, uint16_t cmd);
void prom_id(void);
void prom_disable(void);
void prom_status(void);
void prom_status_clear(void);
void prom_show_mode(void);
void prom_mode(uint mode);
int  prom_verify(int verbose);
void prom_snoop(void);

#define ERASE_MODE_CHIP   0
#define ERASE_MODE_SECTOR 1
#define ERASE_MODE_BLOCK  2

#endif /* _PROM_ACCESS_H */
