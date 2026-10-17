/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2024.
 *
 * ---------------------------------------------------------------------
 *
 * Declarations shared between smash.c and smash_flash.c
 */

#ifndef _SMASH_H
#define _SMASH_H

/*
 * gcc clib2 headers are bad (for example, no stdint definitions) and are
 * not being included by our build.  Because of that, we need to fix up
 * some stdio definitions.
 */
extern struct iob ** __iob;
#undef stdout
#define stdout ((FILE *)__iob[1])

#define ROM_BASE         0x00f80000  /* Base address of Kickstart ROM */
#define CIAA_PRA         VADDR8(0x00bfe001)
#define CIAA_PRA_OVERLAY BIT(0)
#define CIAA_PRA_LED     BIT(1)

#define VALUE_UNASSIGNED 0xffffffff

#define ROM_WINDOW_SIZE   (512 << 10)  // 512 KB
#define MAX_CHUNK         (16 << 10)   // 16 KB

typedef struct {
    const char *const short_name;
    const char *const long_name;
} long_to_short_t;

typedef struct {
    uint16_t cb_chipid;   // Chip id code
    uint8_t  cb_bbnum;    // Boot block number (0=Bottom boot)
    uint8_t  cb_bsize;    // Common block size in Kwords (typical 32K)
    uint8_t  cb_ssize;    // Boot block sector size in Kwords (typical 4K)
    uint8_t  cb_map;      // Boot block sector erase map
} chip_blocks_t;

/* smash.c */
extern struct DosLibrary *DOSBase;
extern uint    flag_debug;
extern uint8_t flag_conservative_mode;
extern uint8_t flag_yes;

BOOL        is_user_abort(void);
const char *long_to_short(const char *ptr, long_to_short_t *ltos,
                          uint ltos_count);
void        print_us_diff(uint64_t start, uint64_t end);
uint64_t    smash_time(void);
const char *ee_id_string(uint32_t id);
const chip_blocks_t *get_chip_block_info(uint32_t chipid);
int         flash_cmd_core(uint32_t cmd, void *arg, uint argsize);
int         flash_read_mode(uint expect_rom_signature);
int         flash_id(uint32_t *dev1, uint32_t *dev2, uint *mode);

#endif /* _SMASH_H */
