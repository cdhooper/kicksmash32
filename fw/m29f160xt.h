/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2020.
 *
 * ---------------------------------------------------------------------
 *
 * MX29F1615-specific code (read, write, erase, status, etc).
 */

#ifndef __MX29F1615_H
#define __MX29F1615_H

void     ee_enable(void);
void     ee_disable(void);
int      ee_read(uint32_t addr, void *data, uint count);
int      ee_write(uint32_t addr, void *data, uint count);
void     ee_id(uint32_t *part1, uint32_t *part2);
void     ee_init(void);
void     ee_read_mode(void);
int      ee_erase(uint mode, uint32_t addr, uint32_t len, int verbose);
void     ee_status_clear(void);
void     ee_cmd(uint32_t addr, uint32_t cmd);
void     ee_poll(void);
int      ee_verify(int verbose);
int      ee_test(void);
void     ee_address_override(uint8_t bits, uint override);
void     ee_set_mode(uint new_mode);
void     ee_set_bank(uint8_t bank);

const char *ee_id_string(uint32_t id);
const char *ee_vendor_string(uint32_t id);
void     ee_update_bank_at_poweron(void);
void     ee_update_bank_at_reset(void);
void     ee_update_bank_at_longreset(void);


/* Bus functions for manipulating GPIOs */
void     address_output_disable(void);
void     data_output_disable(void);
void     data_output(uint32_t data);
void     data_output_enable(void);
void     oe_output(uint value);
void     oe_output_enable(void);
void     oe_output_disable(void);

#define MX_ERASE_MODE_CHIP   0
#define MX_ERASE_MODE_SECTOR 1

#define EE_MODE_32      0  // 32-bit flash
#define EE_MODE_16_LOW  1  // 16-bit flash low device (bits 0-15)
#define EE_MODE_16_HIGH 2  // 16-bit flash high device (bits 16-31)
#define EE_MODE_AUTO    3  // Automatically select mode at boot
#define EE_MODE_32_SWAP 4  // 32-bit flash high / low swapped

extern uint ee_mode;
extern uint ee_default_mode;

#endif /* __MX29F1615_H */
