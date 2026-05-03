/*
 * Zorro AutoConfig functions.
 *
 * This header file is part of the code base for a simple Amiga ROM
 * replacement sufficient to allow programs using some parts of GadTools
 * to function.
 *
 * Copyright 2025 Chris Hooper. This program and source may be used
 * and distributed freely, for any purpose which benefits the Amiga
 * community. All redistributions must retain this Copyright notice.
 *
 * DISCLAIMER: THE SOFTWARE IS PROVIDED "AS-IS", WITHOUT ANY WARRANTY.
 * THE AUTHOR ASSUMES NO LIABILITY FOR ANY DAMAGE ARISING OUT OF THE USE
 * OR MISUSE OF THIS UTILITY OR INFORMATION REPORTED BY THIS UTILITY.
 */
#ifndef _AUTOCONFIG_H
#define _AUTOCONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include "med_cmdline.h"

typedef struct {
    uint8_t  ac_type;
    uint8_t  ac_product;
    uint16_t ac_mfg;
    uint32_t ac_addr;
    uint32_t ac_size;
} autoconfig_dev_t;

void autoconfig_init(void);
void autoconfig_list(void);
rc_t autoconfig_address(uint32_t addr);
uint autoconfig_configure_all(void);
bool autoconfig_find(uint16_t mfg, uint8_t product, autoconfig_dev_t *dev);
rc_t autoconfig_shutup(void);
rc_t autoconfig_show(void);

#endif /* _AUTOCONFIG_H */
