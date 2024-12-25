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
#include "utils.h"
#include "m29f160xt.h"

#define CONFIG_MAGIC     0x19460602
#define CONFIG_VERSION   0x01
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
            uint crclen = sizeof (config_t) - crcpos;
            uint32_t crc = crc32(0, &ptr->crc + 1, crclen);
            if (crc == ptr->crc) {
                printf("Valid config at %lx", addr);
                memcpy(&config, (void *) addr, sizeof (config));
                if (config.name[0] != '\0')
                    printf("  (%s)", config.name);
                printf("\n");
                if (cold_poweron) {
                    config.bi.bi_bank_current = config.bi.bi_bank_poweron;
                    config.bi.bi_bank_nextreset = 0xff;
                }
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
    config.bi.bi_bank_nextreset = 0xff;
    config.bi.bi_bank_poweron = 0;
    memset(&config.bi.bi_longreset_seq, 0xff,
           sizeof (config.bi.bi_longreset_seq));

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

int
config_set_bank_name(uint bank, const char *name)
{
    uint len = strlen(name);
    if (len >= ARRAY_SIZE(config.bi.bi_name[bank])) {
        printf("Bank name \"%s\" is too long.\n", name);
        return (1);
    }
    strcpy(config.bi.bi_name[bank], name);
    config_updated();
    return (0);
}

int
config_set_bank_longreset(uint8_t *banks)
{
    uint bank;
    uint sub;
    for (bank = 0; bank < ROM_BANKS; bank++) {
        sub = config.bi.bi_merge[bank] & 0x0f;
        if (sub != 0) {
            printf("Bank %u is part of a merged block, but is not the "
                   "first (use %u)\n", bank, bank - sub);
            return (1);
        }
    }
    memcpy(config.bi.bi_longreset_seq, banks, ROM_BANKS);
    config_updated();
    return (0);
}

int
config_set_bank_merge(uint bank_start, uint bank_end, uint flag_unmerge)
{
    uint bank;
    uint banks_add = bank_end - bank_start;

    for (bank = bank_start; bank <= bank_end; bank++) {
        if (!flag_unmerge && (config.bi.bi_merge[bank] != 0)) {
            uint banks = (config.bi.bi_merge[bank] >> 4) + 1;
            printf("Bank %u is already part of a%s %u bank range\n",
                   bank, (banks == 8) ? "n" : "", banks);
            return (1);
        }
        if (flag_unmerge && (config.bi.bi_merge[bank] == 0)) {
            printf("Bank %u is not part of a bank range\n", bank);
            return (1);
        }
    }

    for (bank = bank_start; bank <= bank_end; bank++) {
        if (flag_unmerge) {
            config.bi.bi_merge[bank] = 0;
        } else {
            config.bi.bi_merge[bank] = (banks_add << 4) |
                                       (bank - bank_start);
        }
    }
    config_updated();
    return (0);
}

int
config_set_bank(uint bank, uint set_cur, uint set_poweron, uint set_reset)
{
    uint sub = config.bi.bi_merge[bank] & 0x0f;
    if (sub != 0) {
        printf("Bank %u is part of a merged block, but is not the "
               "first (use %u)\n", bank, bank - sub);
        return (1);
    }

    if (set_cur) {
        ee_set_bank(bank);
    }
    if (set_poweron) {
        config.bi.bi_bank_poweron = bank;
        config_updated();
    }
    if (set_reset) {
        config.bi.bi_bank_nextreset = bank;
    }
    return (0);
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
        if (strncmp(config.name, name, sizeof (config.name) - 1) == 0)
            return;
        strncpy(config.name, name, sizeof (config.name) - 1);
        config.name[sizeof (config.name) - 1] = '\0';
        config_updated();
    }
}

void
config_bank_show(void)
{
    uint bank;

    printf("Bank  Name            Merge LongReset  PowerOn  Current  "
           "NextReset\n");

    for (bank = 0; bank < ROM_BANKS; bank++) {
        uint aspaces = 2;
        uint pos;
        uint banks_add = config.bi.bi_merge[bank] >> 4;
        uint bank_sub  = config.bi.bi_merge[bank] & 0xf;
        printf("%-5u %-15s ", bank, config.bi.bi_name[bank]);

        if (banks_add < 1)
            aspaces += 4;
        else if (bank_sub == 0)
            printf("-\\  ");
        else if (bank_sub == banks_add)
            printf("-/  ");
        else
            printf("  | ");

        for (pos = 0; pos < ARRAY_SIZE(config.bi.bi_longreset_seq); pos++)
            if (config.bi.bi_longreset_seq[pos] == bank)
                break;

        if (pos < ARRAY_SIZE(config.bi.bi_longreset_seq)) {
            printf("%*s%u", aspaces, "", pos);
            aspaces = 0;
        } else {
            aspaces++;
        }
        aspaces += 10;

        if (bank == config.bi.bi_bank_poweron) {
            printf("%*s*", aspaces, "");
            aspaces = 0;
        } else {
            aspaces++;
        }
        aspaces += 8;

        if (bank == config.bi.bi_bank_current) {
            printf("%*s*", aspaces, "");
            aspaces = 0;
        } else {
            aspaces++;
        }
        aspaces += 8;

        if (bank == config.bi.bi_bank_nextreset) {
            printf("%*s*", aspaces, "");
            aspaces = 0;
        } else {
            aspaces++;
        }
        aspaces += 8;
        printf("\n");
    }
}
