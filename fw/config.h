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

typedef struct {
    uint32_t    magic;
    uint32_t    crc;
    uint16_t    size;
    uint8_t     valid;
    uint8_t     version;
    bank_info_t bi;
    uint8_t     ee_mode;
    uint8_t     unused2[63];
} config_t;

extern config_t config;

void config_updated(void);
void config_poll(void);
void config_read(void);

#define STM32FLASH_FLAG_AUTOERASE 1

#endif /* _CONFIG_H */


