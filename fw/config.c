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

#include "printf.h"
#include "main.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "timer.h"
#include "smash_cmd.h"
#include "config.h"
#include "crc32.h"
#include "stm32flash.h"

#define CONFIG_MAGIC     0x19460602
#define CONFIG_VERSION   0x01
#define CONFIG_AREA_BASE 0x3e000
#define CONFIG_AREA_SIZE 0x02000
#define CONFIG_AREA_END  (CONFIG_AREA_BASE + CONFIG_AREA_SIZE)

uint64_t config_timer = 0;

config_t config;

void
config_updated(void)
{
    config_timer = timer_tick_plus_msec(1000);
}

/*
 * config_write
 * ------------
 * Write new config area
 */
static void
config_write(void)
{
    /* Locate and invalidate previous config areas */
    config_t *ptr;
    uint32_t  addr;
    uint      crcpos;
    uint      crclen;

    config.magic = CONFIG_MAGIC;
    config.size  = sizeof (config);
    config.valid = 0x01;
    crcpos       = offsetof(config_t, crc) + sizeof (config.crc);
    crclen       = sizeof (config_t) - crcpos;
    config.crc   = crc32(0, &config.crc + 1, crclen);

    for (addr = CONFIG_AREA_BASE; addr < CONFIG_AREA_END; addr += 4) {
        ptr = (config_t *) addr;
        if ((ptr->magic == CONFIG_MAGIC) && (ptr->valid)) {
            uint16_t buf = 0;
            if (memcmp(ptr, &config, sizeof (config)) == 0) {
                /*
                 * Written record already matches the current config.
                 * No need to write new record.
                 */
                return;
            }
            stm32flash_write((uint32_t) &ptr->valid, sizeof (buf), &buf, 0);
        }
    }

    /* Locate space for new config area */
    for (addr = CONFIG_AREA_BASE; addr < CONFIG_AREA_END; addr += 4) {
        ptr = (config_t *) addr;
        if (ptr->magic == CONFIG_MAGIC)
            addr += ptr->size - 4;  // quickly skip to next area
        else if (ptr->magic == 0xffffffff)
            break;
    }
    if (addr + config.size > CONFIG_AREA_END) {
        addr = CONFIG_AREA_BASE;  // Need to erase config area
        printf("Config area erase %lx\n", addr);
        if (stm32flash_erase(CONFIG_AREA_BASE, CONFIG_AREA_SIZE) != 0) {
            printf("Failed to erase config area\n");
            stm32flash_erase(CONFIG_AREA_BASE, CONFIG_AREA_SIZE);  // try again
        }
    }
    printf("config write at %lx\n", addr);
    if (stm32flash_write(addr, config.size, &config, 0) != 0) {
        printf("Config area update failed at %lx\n", addr);
    }
}

/*
 * config_read
 * -----------
 * Locates and reads the valid config area in STM32 internal flash.
 * If none is found, a new config structure will be populated.
 */
void
config_read(void)
{
    uint32_t addr;
    config_t *ptr;
    for (addr = CONFIG_AREA_BASE; addr < CONFIG_AREA_END; addr += 4) {
        ptr = (config_t *) addr;
        if ((ptr->magic == CONFIG_MAGIC) && (ptr->valid)) {
            uint crcpos = offsetof(config_t, crc) + sizeof (ptr->crc);
            uint crclen = sizeof (config_t) - crcpos;
            uint32_t crc = crc32(0, &ptr->crc + 1, crclen);
            if (crc == ptr->crc) {
                printf("Valid config at %lx\n", addr);
                memcpy(&config, (void *) addr, sizeof (config));
                return;
            }
        }
    }
    printf("New config\n");
    memset(&config, 0, sizeof (config));
    config.magic   = CONFIG_MAGIC;
    config.size    = sizeof (config);
    config.valid   = 0x01;
    config.version = CONFIG_VERSION;
    config.ee_mode = 3;  // EE_MODE_AUTO

    config.bi.bi_bank_current = 0;
    config.bi.bi_bank_nextreset = 0;
    config.bi.bi_bank_poweron = 0;
    memset(&config.bi.bi_longreset_seq, 0xff,
           sizeof (config.bi.bi_longreset_seq));

#if 1
    // XXX: For debug
    config.bi.bi_longreset_seq[0] = 2;
    config.bi.bi_longreset_seq[1] = 0;
    config.bi.bi_longreset_seq[2] = 0xff;
    config.bi.bi_longreset_seq[3] = 0xff;
    config.bi.bi_longreset_seq[4] = 0xff;
    config.bi.bi_longreset_seq[5] = 0xff;
    config.bi.bi_longreset_seq[6] = 0xff;
    config.bi.bi_longreset_seq[7] = 0xff;
    config.bi.bi_longreset_seq[7] = 0xff;
    config.bi.bi_merge[0] = 0x00;  // 512KB bank
    config.bi.bi_merge[1] = 0x00;  // 512KB bank
    config.bi.bi_merge[2] = 0x00;  // 512KB bank
    config.bi.bi_merge[3] = 0x00;  // 512KB bank
    config.bi.bi_merge[4] = 0x30;  // 2MB bank
    config.bi.bi_merge[5] = 0x31;
    config.bi.bi_merge[6] = 0x32;
    config.bi.bi_merge[7] = 0x33;
    strcpy(config.bi.bi_desc[0], "3.2.2 A");
    strcpy(config.bi.bi_desc[1], "3.2.2 B");
    strcpy(config.bi.bi_desc[2], "DiagROM");
    strcpy(config.bi.bi_desc[4], "2MB ROM");
#endif

    config_updated();
}

/*
 * config_poll
 * -----------
 * Run background service on config area -- flush dirty config.
 */
void
config_poll(void)
{
    if ((config_timer != 0) && timer_tick_has_elapsed(config_timer)) {
        config_timer = 0;
        config_write();
    }
}
