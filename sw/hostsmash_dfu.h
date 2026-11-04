/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2026.
 *
 * ---------------------------------------------------------------------
 *
 * Hostsmash USB DFU (DfuSe) interface to the Kicksmash firmware updater
 * and the STM32 and GD32 ROM DFU bootloaders.
 */

#ifndef _HOSTSMASH_DFU_H
#define _HOSTSMASH_DFU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DFU_FIRST_BLOCK   2     // First DfuSe data block number
#define DFU_LIST_MAX      16    // Maximum DFU devices reported by dfu_list()

/* Information about an attached DFU device */
typedef struct {
    char serial[64];   // USB serial number ("STM32" for the ROM bootloader)
    char product[64];  // USB product string
    char usbpath[32];  // USB port path (Linux sysfs format, e.g. "1-6.1.6")
    char kind[48];     // Description, e.g. "STM32 ROM DFU bootloader"
    bool is_updater;   // true = Kicksmash updater, false = ROM DFU
    bool is_gd32;      // true = GigaDevice GD32 ROM DFU bootloader
} dfu_info_t;

typedef struct dfu_dev dfu_dev_t;

bool              dfu_supported(void);
unsigned int      dfu_list(dfu_info_t *list, unsigned int max);
bool              dfu_id_match(const dfu_info_t *info, const char *id);
dfu_dev_t        *dfu_wait_open(const char *id, unsigned int timeout_ms);
bool              dfu_install_drivers(void);
void              dfu_close(dfu_dev_t *dev);
const dfu_info_t *dfu_dev_info(dfu_dev_t *dev);
unsigned int      dfu_xfer_size(dfu_dev_t *dev);
uint32_t          dfu_flash_base(dfu_dev_t *dev);
uint32_t          dfu_flash_size(dfu_dev_t *dev);
unsigned int      dfu_page_size(dfu_dev_t *dev, uint32_t addr);
void              dfu_recover(dfu_dev_t *dev);
int               dfu_erase_page(dfu_dev_t *dev, uint32_t addr,
                                 char *errbuf, size_t errlen);
int               dfu_set_address(dfu_dev_t *dev, uint32_t addr,
                                  char *errbuf, size_t errlen);
int               dfu_download(dfu_dev_t *dev, unsigned int blocknum,
                               const void *data, unsigned int len,
                               char *errbuf, size_t errlen);
void              dfu_leave(dfu_dev_t *dev);

#endif /* _HOSTSMASH_DFU_H */
