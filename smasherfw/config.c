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
#include "config.h"
#include "crc32.h"
#include "stm32flash.h"
#include "utils.h"
#include "ee_kicksmash.h"

#define CONFIG_MAGIC     0x19460602
#define CONFIG_VERSION   0x02
#define CONFIG_AREA_BASE 0x3e000
#define CONFIG_AREA_SIZE 0x02000
#define CONFIG_AREA_END  (CONFIG_AREA_BASE + CONFIG_AREA_SIZE)

uint64_t config_timer = 0;
uint8_t  cold_poweron = 0;

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
            uint crclen = ptr->size - crcpos;
            uint32_t crc;
            if (crclen > 1024)  // sanity check
                crclen = 1024;
            crc = crc32(0, &ptr->crc + 1, crclen);
            if (crc == ptr->crc) {
                printf("    Valid config at %lx", addr);
                memcpy(&config, (void *) addr, sizeof (config));
                if (config.name[0] != '\0')
                    printf("  (%s)", config.name);
                printf("\n");
                if (config.board_rev == 0)
                    config.board_rev = 1;
                config.version = CONFIG_VERSION;
                return;
            }
        }
    }
    printf("    New config\n");
    memset(&config, 0, sizeof (config));
    config.magic   = CONFIG_MAGIC;
    config.size    = sizeof (config);
    config.valid   = 0x01;
    config.version = CONFIG_VERSION;
    config.ee_mode = 3;  // EE_MODE_AUTO
    config.led_level = 80;  // 80%
    config.board_rev = 1;

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

void
config_name(const char *name)
{
    if (name == NULL) {
        /* Show current name */
        if (config.name[0] == '\0') {
            printf("Board is unnamed\n");
        } else {
            printf("%s\n", config.name);
        }
    } else {
        const char *copyname = name;
        if ((name[0] == '-') && (name[1] == '\0'))
            copyname = "";  // name "-" removes the board name
        if (strncmp(config.name, copyname, sizeof (config.name) - 1) == 0)
            return;
        strncpy(config.name, copyname, sizeof (config.name) - 1);
        config.name[sizeof (config.name) - 1] = '\0';
        config_updated();
    }
}

void
config_set_led(uint value)
{
    config.led_level = value;
    config_updated();
}
