/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2024.
 *
 * ---------------------------------------------------------------------
 *
 * Config area management (non-volatile storage)
 */

#ifndef _CONFIG_H
#define _CONFIG_H

#include "smash_cmd.h"

#define CF_OEWE_PIN_SHOW    0x00000001  // Show measured speed of OEWE pin test

typedef struct {
    uint32_t    magic;      // Structure magic
    uint32_t    crc;        // Structure CRC
    uint16_t    size;       // Structure size in bytes
    uint8_t     valid;      // Structure record is valid
    uint8_t     version;    // Structure version
    bank_info_t bi;         // Flash bank information
    uint8_t     ee_mode;    // Flash mode (0=32-bit, 1=16-bit, 2=16-bit hi)
    char        name[16];   // Unique name for this board
    uint8_t     led_level;  // Power LED brightness (0 to 100)
    uint8_t     board_rev;  // Board revision
    uint8_t     unused[29]; // Unused
    uint32_t    flags;      // Runtime flags
    uint8_t     nv_mem[32]; // Non-volatile storage for Amiga
} config_t;

extern config_t config;

void config_updated(void);
void config_poll(void);
void config_read(void);

int  config_set_bank_name(uint bank, const char *comment);
int  config_set_bank_longreset(uint8_t *banks);
int  config_set_bank_merge(uint bank_start, uint bank_end, uint flag_unmerge);
int  config_set_bank(uint bank, uint set_cur, uint set_poweron, uint set_reset);
void config_bank_show(void);
void config_name(const char *name);
void config_set_led(uint value);

#define STM32FLASH_FLAG_AUTOERASE 1

#endif /* _CONFIG_H */
