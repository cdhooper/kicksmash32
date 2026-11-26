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
#define CONFIG_VERSION   0x02
#define CONFIG_AREA_BASE 0x3e000
#define CONFIG_AREA_SIZE 0x02000
#define CONFIG_AREA_END  (CONFIG_AREA_BASE + CONFIG_AREA_SIZE)
#define CONFIG_PAGE_SIZE 0x00800  // STM32F107 flash page size

/* seq took the last 4 bytes of unused[]; the record layout must not change */
extern char config_check_size[(sizeof (config_t) == 256) ? 1 : -1];
extern char config_check_seq[(offsetof(config_t, seq) == 216) ? 1 : -1];
extern char config_check_flags[(offsetof(config_t, flags) == 220) ? 1 : -1];

uint64_t config_timer = 0;
uint32_t board_state  = 0;
uint8_t  cold_poweron = 0;

config_t config;

void
config_updated(void)
{
    config_timer = timer_tick_plus_msec(1000);
}

/*
 * config_record_ok
 * ----------------
 * Returns true if the specified address holds a valid config record
 * with a correct CRC.
 */
static bool
config_record_ok(const config_t *ptr)
{
    uint crcpos = offsetof(config_t, crc) + sizeof (ptr->crc);
    uint crclen;

    if ((ptr->magic != CONFIG_MAGIC) || (ptr->valid == 0) ||
        (ptr->size <= crcpos) ||
        ((uint32_t) ptr + ptr->size > CONFIG_AREA_END))
        return (false);
    crclen = ptr->size - crcpos;
    if (crclen > 1024)  // sanity check
        crclen = 1024;
    return (crc32(0, &ptr->crc + 1, crclen) == ptr->crc);
}

/*
 * config_find_newest
 * ------------------
 * Returns the address of the newest valid config record, or 0 if there
 * is none. The newest record has the highest sequence number. Records
 * written by older firmware have sequence number 0, so among equal
 * sequence numbers, the last one in the area is the newest.
 *
 * More than one valid record exists only if power was lost while
 * config_write() was updating the area.
 */
static uint32_t
config_find_newest(void)
{
    uint32_t addr;
    uint32_t found = 0;
    uint32_t found_seq = 0;

    for (addr = CONFIG_AREA_BASE; addr < CONFIG_AREA_END; addr += 4) {
        const config_t *ptr = (const config_t *) addr;
        if (config_record_ok(ptr) &&
            ((found == 0) || ((int32_t) (ptr->seq - found_seq) >= 0))) {
            found = addr;
            found_seq = ptr->seq;
        }
    }
    return (found);
}

/*
 * config_area_blank
 * -----------------
 * Returns true if the specified range of the config area is erased.
 */
static bool
config_area_blank(uint32_t addr, uint len)
{
    const uint32_t *ptr = (const uint32_t *) addr;
    const uint32_t *end = (const uint32_t *) (addr + len);

    for (; ptr < end; ptr++)
        if (*ptr != 0xffffffff)
            return (false);
    return (true);
}

/*
 * config_write
 * ------------
 * Write new config record. The config area is used as a ring of flash
 * pages. A new record is written after the newest record. When that
 * space is not blank (the ring wrapped), only the page which will hold
 * the new record is erased; it never holds the newest record. The new
 * record is written and verified before previous records are invalidated,
 * so a power loss at any point leaves at least one valid record, and
 * config_read() picks the newest one by its sequence number.
 */
static void
config_write(void)
{
    config_t *ptr;
    uint32_t  addr;
    uint32_t  oaddr;
    uint32_t  newest;
    uint32_t  page;
    uint      crcpos;
    uint      crclen;

    config.magic = CONFIG_MAGIC;
    config.size  = sizeof (config);
    config.valid = 0x01;
    crcpos       = offsetof(config_t, crc) + sizeof (config.crc);
    crclen       = sizeof (config_t) - crcpos;
    config.crc   = crc32(0, &config.crc + 1, crclen);

    newest = config_find_newest();
    if ((newest != 0) &&
        (memcmp((void *) newest, &config, sizeof (config)) == 0)) {
        /*
         * Written record already matches the current config.
         * No need to write new record.
         */
        return;
    }

    /* New record goes after the newest, wrapping at the end of the area */
    if (newest == 0) {
        addr = CONFIG_AREA_BASE;
        config.seq = 1;
    } else {
        addr = (newest + ((config_t *) newest)->size + 3) & ~3;
        config.seq = ((config_t *) newest)->seq + 1;
    }
    config.crc = crc32(0, &config.crc + 1, crclen);

    if (addr + config.size > CONFIG_AREA_END)
        addr = CONFIG_AREA_BASE;
    if (!config_area_blank(addr, config.size)) {
        /*
         * Erase the page which will hold the new record. If the newest
         * record is in that page, use the start of the next page instead.
         */
        page = addr & ~(CONFIG_PAGE_SIZE - 1);
        if ((newest != 0) &&
            (newest + config.size > page) &&
            (newest < page + CONFIG_PAGE_SIZE)) {
            page += CONFIG_PAGE_SIZE;
            if (page >= CONFIG_AREA_END)
                page = CONFIG_AREA_BASE;
        }
        addr = page;
        printf("Config area erase %lx\n", addr);
        if (stm32flash_erase(addr, CONFIG_PAGE_SIZE) != 0) {
            printf("Failed to erase config area page %lx\n", addr);
            return;  // Keep previous records valid
        }
    }

    printf("config write at %lx\n", addr);
    if ((stm32flash_write(addr, config.size, &config, 0) != 0) ||
        (memcmp((void *) addr, &config, sizeof (config)) != 0)) {
        printf("Config area update failed at %lx\n", addr);
        return;  // Keep previous records valid
    }

    /* Invalidate previous config records */
    for (oaddr = CONFIG_AREA_BASE; oaddr < CONFIG_AREA_END; oaddr += 4) {
        if (oaddr == addr)
            continue;
        ptr = (config_t *) oaddr;
        if ((ptr->magic == CONFIG_MAGIC) && (ptr->valid)) {
            uint16_t buf = 0;
            stm32flash_write((uint32_t) &ptr->valid, sizeof (buf), &buf, 0);
        }
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
    uint32_t found;
    uint     bank;

    found = config_find_newest();
    if (found != 0) {
        addr = found;
        printf("    Valid config at %lx", addr);
        memcpy(&config, (void *) addr, sizeof (config));

        /* Older firmware could store names without a NIL */
        config.name[sizeof (config.name) - 1] = '\0';
        for (bank = 0; bank < ROM_BANKS; bank++)
            config.bi.bi_name[bank][sizeof (config.bi.bi_name[0]) - 1] =
                '\0';

        if (config.name[0] != '\0')
            printf("  (%s)", config.name);
        printf("\n");
        if (cold_poweron) {
            config.bi.bi_bank_current = config.bi.bi_bank_poweron;
            config.bi.bi_bank_nextreset = 0xff;
        }
        config.bi.bi_valid = 1;
        if (config.led_level == 0)
            config.led_level = 100;  // Old board
        if (config.board_rev == 0)
            config.board_rev = 7;
        if (config.version < 2) {
            /* Structure expanded for this new field */
            memset(config.nv_mem, 0, sizeof (config.nv_mem));
        }
        config.version = CONFIG_VERSION;
        return;
    }
    printf("    New config\n");
    memset(&config, 0, sizeof (config));
    config.magic   = CONFIG_MAGIC;
    config.size    = sizeof (config);
    config.valid   = 0x01;
    config.version = CONFIG_VERSION;
    config.ee_mode = 3;  // EE_MODE_AUTO
    config.led_level = 40;  // 40%
    config.board_rev = 7;

    config.bi.bi_valid = 1;
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
    uint pos;
    uint bank;
    uint sub;

    /* Each bank in the sequence must be the first bank of its merge range */
    for (pos = 0; pos < ROM_BANKS; pos++) {
        bank = banks[pos];
        if (bank == 0xff)
            continue;  // Unused sequence entry
        if (bank >= ROM_BANKS) {
            printf("Bank %u is invalid (maximum bank is %u)\n",
                   bank, ROM_BANKS - 1);
            return (1);
        }
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

/*
 * config_check_bank_merge
 * -----------------------
 * Verifies that a bank range may be merged or unmerged. A merge requires
 * that no bank in the range is already merged. An unmerge requires that
 * the range is exactly one whole merged group. The caller must already
 * have verified the range size and alignment. Returns 0 if the request
 * is valid, otherwise a BANK_MERGE_ERR_* code, with the offending bank
 * number in *badbank.
 */
uint
config_check_bank_merge(uint bank_start, uint bank_end, uint flag_unmerge,
                        uint *badbank)
{
    uint bank;
    uint banks_add = bank_end - bank_start;

    for (bank = bank_start; bank <= bank_end; bank++) {
        uint8_t merge = config.bi.bi_merge[bank];
        *badbank = bank;
        if (!flag_unmerge) {
            if (merge != 0)
                return (BANK_MERGE_ERR_MERGED);
        } else if (merge == 0) {
            return (BANK_MERGE_ERR_NOT_MERGED);
        } else if (merge != ((banks_add << 4) | (bank - bank_start))) {
            return (BANK_MERGE_ERR_PARTIAL);
        }
    }
    return (0);
}

int
config_set_bank_merge(uint bank_start, uint bank_end, uint flag_unmerge)
{
    uint bank;
    uint banks_add = bank_end - bank_start;
    uint merge;
    uint banks;

    switch (config_check_bank_merge(bank_start, bank_end, flag_unmerge,
                                    &bank)) {
        case 0:
            break;
        case BANK_MERGE_ERR_MERGED:
            banks = (config.bi.bi_merge[bank] >> 4) + 1;
            printf("Bank %u is already part of a%s %u bank range\n",
                   bank, (banks == 8) ? "n" : "", banks);
            return (1);
        case BANK_MERGE_ERR_NOT_MERGED:
            printf("Bank %u is not part of a bank range\n", bank);
            return (1);
        default:  // BANK_MERGE_ERR_PARTIAL
            merge = config.bi.bi_merge[bank];
            banks = (merge >> 4) + 1;
            printf("Bank %u is part of the %u bank range %u-%u (unmerge "
                   "the whole range)\n", bank, banks, bank - (merge & 0xf),
                   bank - (merge & 0xf) + banks - 1);
            return (1);
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

void
config_set_led(uint value)
{
    config.led_level = value;
    config_updated();
}
