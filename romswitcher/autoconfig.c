/*
 * Zorro AutoConfig functions.
 *
 * This source file is part of the code base for a simple Amiga ROM
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
#include <stdint.h>
#include <stdbool.h>
#include "amiga_chipset.h"
#include "autoconfig.h"
#include "printf.h"
#include "util.h"
#include "med_cmdline.h"
#include "timer.h"

#define Z2_CFG_BASE 0x00e80000
#define Z3_CFG_BASE 0xff000000

#define Z2_SHUTUP       VADDR8(Z2_CFG_BASE + 0x4c)  // Go to next autoconfig
#define Z3_SHUTUP       VADDR8(Z3_CFG_BASE + 0x4c)  // Go to next autoconfig
#define Z2_BASE_A27_A24 VADDR8(Z2_CFG_BASE + 0x46)  // Z3 in Z2 space (1)
#define Z2_BASE_A31_A24 VADDR8(Z2_CFG_BASE + 0x44)  // Z3 in Z2 space (2)
#define Z2_BASE_A19_A16 VADDR8(Z2_CFG_BASE + 0x4a)  // Z2 / Z3 in Z2 space (3)
#define Z2_BASE_A23_A16 VADDR8(Z2_CFG_BASE + 0x48)  // Z2 / Z3 in Z2 space (4)

#define Z3_BASE_A23_A16 VADDR8(Z3_CFG_BASE + 0x48)   // Z3 in Z3 space (0)
#define Z3_BASE_A19_A16 VADDR8(Z3_CFG_BASE + 0x148)  // Z2 in Z3 space
#define Z3_BASE_A31_A24 VADDR8(Z3_CFG_BASE + 0x44)  // Z3 in Z3 space (1a)
#define Z3_BASE_A31_A16 VADDR16(Z3_CFG_BASE + 0x44)  // Z3 in Z3 space (1b)

#define Z2_SLOT_SIZE       (64 << 10)
#define Z2_MEMORY_START    0x00200000
#define Z2_MEMORY_END      0x00a00000
#define Z2_EXPANSION_START 0x00e90000
#define Z2_EXPANSION_END   0x00f00000

static const char z2_config_sizes[][8] =
{
    "8 MB", "64 KB", "128 KB", "256 KB", "512 KB", "1 MB", "2 MB", "4 MB"
};
static const uint32_t z2_config_sizenums[] =
{
    8 << 20, 64 << 10, 128 << 10, 256 << 10,
    512 << 10, 1 << 20, 2 << 20, 4 << 20
};

static const char z3_config_sizes[][8] =
{
    "16 MB", "32 MB", "64 MB", "128 MB", "256 MB", "512 MB", "1 GB", "RSVD"
};
static const uint32_t z3_config_sizenums[] =
{
    16 << 20, 32 << 20, 64 << 20, 128 << 20, 256 << 20, 512 << 20, 1 << 30, 0
};

static const char * const config_subsizes[] =
{
    "Same-as-Physical", "Automatically-sized", "64 KB", "128 KB",
    "256 KB", "512 KB", "1MB", "2MB",
    "4MB", "6MB", "8MB", "10MB", "12MB", "14MB", "Rsvd1", "Rsvd2"
};

#define AC_ROM_FLAGS     2
#define AC_FLAG_SIZE_EXT BIT(5)

#define AC_TYPE_INVALID  0
#define AC_TYPE_ALLOC_Z2 1  // Allocated to Zorro II device
#define AC_TYPE_ALLOC_Z3 2  // Allocated to Zorro III device
#define AC_TYPE_FREE_Z2  3  // Free in Zorro II address range
#define AC_TYPE_FREE_Z3  4  // Free in Zorro III address range

#define PRESENT_CHECK 16
#define AUTOCONFIG_MAX_BOARDS 32
#define AUTOCONFIG_SETTLE_TRIES 5
#define AUTOCONFIG_SETTLE_MSEC 2

typedef struct ac_t ac_t;
typedef struct z2_slot_rule_t z2_slot_rule_t;

struct ac_t {
    ac_t    *ac_next;
    uint8_t  ac_type;
    uint8_t  ac_product;
    uint16_t ac_mfg;
    uint32_t ac_addr;
    uint32_t ac_size;
};

typedef enum {
    AC_CFG_WINDOW_Z2 = 0,
    AC_CFG_WINDOW_Z3 = 1,
} ac_cfg_window_t;

struct z2_slot_rule_t {
    uint8_t slots;
    uint8_t offset_slots;
};

static const z2_slot_rule_t z2_slot_rules[] =
{
    { 128, 32 },  // 8 MB
    {   1,  0 },  // 64 KB
    {   2,  0 },  // 128 KB
    {   4,  0 },  // 256 KB
    {   8,  0 },  // 512 KB
    {  16,  0 },  // 1 MB
    {  32,  0 },  // 2 MB
    {  64, 32 },  // 4 MB
};

static ac_t *ac_list = NULL;

static void
autoconfig_dev_export(const ac_t *node, autoconfig_dev_t *dev)
{
    dev->ac_type = node->ac_type;
    dev->ac_product = node->ac_product;
    dev->ac_mfg = node->ac_mfg;
    dev->ac_addr = node->ac_addr;
    dev->ac_size = node->ac_size;
}

static uint32_t
autoconfig_align_up(uint32_t addr, uint32_t align, uint32_t offset)
{
    if (addr <= offset)
        return (offset);
    return (((addr - offset + align - 1) & ~(align - 1)) + offset);
}

static bool
autoconfig_addr_aligned(uint32_t addr, uint32_t align, uint32_t offset)
{
    return ((addr >= offset) && (((addr - offset) & (align - 1)) == 0));
}

/*
 * autoconfig_alloc_aligned_range
 * ------------------------------
 * Allocate a Zorro address in the specified address space with an alignment
 * rule matching AmigaOS expansion.library slot allocation.
 */
static ac_t *
autoconfig_alloc_aligned_range(uint32_t addr, uint32_t size, uint zorro_type,
                               uint32_t align, uint32_t offset,
                               uint32_t range_start, uint32_t range_end,
                               bool report_failure)
{
    ac_t *cur;
    uint alloc_type = (zorro_type == AC_TYPE_FREE_Z2) ?
                      AC_TYPE_ALLOC_Z2 : AC_TYPE_ALLOC_Z3;

    if ((addr != 0) && !autoconfig_addr_aligned(addr, align, offset)) {
        if (report_failure) {
            printf("Address %08x not aligned to device size %08x; try %08x\n",
                   addr, align, autoconfig_align_up(addr, align, offset));
        }
        return (NULL);
    }

    for (cur = ac_list; cur != NULL; cur = cur->ac_next) {
        uint32_t alloc_addr;
        uint32_t before_size;
        uint32_t after_size;
        uint32_t cur_end;
        uint32_t usable_start;
        uint32_t usable_end;
        ac_t *alloc_node = NULL;
        ac_t *after_node = NULL;

        if (cur->ac_type != zorro_type)
            continue;  // Not free

        cur_end = cur->ac_addr + cur->ac_size;
        usable_start = (cur->ac_addr > range_start) ? cur->ac_addr : range_start;
        usable_end = (cur_end < range_end) ? cur_end : range_end;

        if ((usable_end <= usable_start) || (size > usable_end - usable_start))
            continue;

        if (addr == 0)
            alloc_addr = autoconfig_align_up(usable_start, align, offset);
        else
            alloc_addr = addr;

        if ((alloc_addr < usable_start) || (size > usable_end - alloc_addr))
            continue;  // Not within this range

        before_size = alloc_addr - cur->ac_addr;
        after_size  = cur_end - (alloc_addr + size);

        if (before_size != 0) {
            alloc_node = malloc(sizeof (ac_t));
            if (alloc_node == NULL)
                return (NULL);
        }
        if (after_size != 0) {
            after_node = malloc(sizeof (ac_t));
            if (after_node == NULL)
                return (NULL);
        }

        if (before_size != 0) {
            alloc_node->ac_type = alloc_type;
            alloc_node->ac_size = size;
            alloc_node->ac_addr = alloc_addr;
            alloc_node->ac_next = cur->ac_next;

            cur->ac_size = before_size;
            cur->ac_next = alloc_node;
            cur = alloc_node;
        } else {
            cur->ac_type = alloc_type;
            cur->ac_size = size;
            cur->ac_addr = alloc_addr;
        }

        if (after_size != 0) {
            after_node->ac_type = zorro_type;
            after_node->ac_size = after_size;
            after_node->ac_addr = alloc_addr + size;
            after_node->ac_next = cur->ac_next;
            cur->ac_next = after_node;
        }
        return (cur);
    }
    if (report_failure) {
        printf("Could not allocate");
        if (addr != 0)
            printf(" %08x", addr);
        printf(" in %s space\n", (zorro_type == AC_TYPE_FREE_Z2) ? "Z2" : "Z3");
    }
    return (NULL);
}

/*
 * autoconfig_alloc
 * ----------------
 * Allocate a Zorro address in the specified address space
 */
static ac_t *
autoconfig_alloc(uint32_t addr, uint32_t size, uint zorro_type)
{
    return (autoconfig_alloc_aligned_range(addr, size, zorro_type, size, 0,
                                           0, UINT32_MAX, true));
}

static ac_t *
autoconfig_alloc_z2(uint32_t addr, uint size_code)
{
    const z2_slot_rule_t *rule;
    uint32_t size;
    uint32_t offset;
    ac_t *node;

    if (size_code >= (sizeof (z2_slot_rules) / sizeof (z2_slot_rules[0])))
        return (NULL);

    rule = &z2_slot_rules[size_code];
    size = rule->slots * Z2_SLOT_SIZE;
    offset = rule->offset_slots * Z2_SLOT_SIZE;

    if (addr != 0) {
        return (autoconfig_alloc_aligned_range(addr, size, AC_TYPE_FREE_Z2,
                                               size, offset, 0, UINT32_MAX,
                                               true));
    }

    if (rule->slots <= 4) {
        node = autoconfig_alloc_aligned_range(0, size, AC_TYPE_FREE_Z2,
                                              size, offset,
                                              Z2_EXPANSION_START,
                                              Z2_EXPANSION_END, false);
        if (node != NULL)
            return (node);
    }

    return (autoconfig_alloc_aligned_range(0, size, AC_TYPE_FREE_Z2,
                                           size, offset,
                                           Z2_MEMORY_START, Z2_MEMORY_END,
                                           true));
}

static ac_t *
autoconfig_alloc_board(uint32_t addr, uint32_t devsize, uint size_code,
                       uint is_z3, uint is_z3_size, uint addr_z3)
{
    if (!addr_z3 && !is_z3 && !is_z3_size)
        return (autoconfig_alloc_z2(addr, size_code));

    return (autoconfig_alloc(addr, devsize,
                             addr_z3 ? AC_TYPE_FREE_Z3 : AC_TYPE_FREE_Z2));
}

/*
 * autoconfig_list
 * ---------------
 * List the autoconfig address range blocks, including configured devices
 * and free space.
 */
void
autoconfig_list(void)
{
    ac_t *cur;
    for (cur = ac_list; cur != NULL; cur = cur->ac_next) {
        if ((cur->ac_type == AC_TYPE_ALLOC_Z2) ||
            (cur->ac_type == AC_TYPE_FREE_Z2))
            printf("Z2");
        else
            printf("Z3");
        printf(" %08x [%08x]", cur->ac_addr, cur->ac_size);
        if ((cur->ac_type == AC_TYPE_FREE_Z2) ||
            (cur->ac_type == AC_TYPE_FREE_Z3)) {
            printf(" FREE\n");
        } else {
            printf(" Board 0x%04x.0x%02x  %u / %u\n",
                   cur->ac_mfg, cur->ac_product,
                   cur->ac_mfg, cur->ac_product);
        }
    }
}

/*
 * get_z2_byte
 * -----------
 * Get a single byte from the Zorro II configuration address range
 */
static uint8_t
get_z2_byte(uint offset)
{
    uint8_t unibble = (*VADDR16(Z2_CFG_BASE + offset * 4 + 0) >> 8) & 0xf0;
    uint8_t lnibble = (*VADDR16(Z2_CFG_BASE + offset * 4 + 2) >> 12) & 0x0f;
    return (unibble | lnibble);
}

/*
 * get_z3_byte
 * -----------
 * Get a single byte from the Zorro III configuration address range
 */
static uint8_t
get_z3_byte(uint offset)
{
    uint8_t unibble = (*VADDR16(Z3_CFG_BASE + offset * 4 + 0x000) >> 8) & 0xf0;
    uint8_t lnibble = (*VADDR16(Z3_CFG_BASE + offset * 4 + 0x100) >> 12) & 0x0f;
    return (unibble | lnibble);
}

static uint8_t
autoconfig_window_byte(ac_cfg_window_t cfg_window, uint offset)
{
    return ((cfg_window == AC_CFG_WINDOW_Z3) ?
            get_z3_byte(offset) : get_z2_byte(offset));
}

static void
autoconfig_signature_read(ac_cfg_window_t cfg_window,
                          uint8_t sig[PRESENT_CHECK])
{
    uint cur;

    for (cur = 0; cur < PRESENT_CHECK; cur++)
        sig[cur] = autoconfig_window_byte(cfg_window, cur);
}

static bool
autoconfig_signature_same(ac_cfg_window_t cfg_window,
                          const uint8_t before[PRESENT_CHECK])
{
    uint cur;
    uint all_zero = 1;
    uint all_ff = 1;

    for (cur = 0; cur < PRESENT_CHECK; cur++) {
        uint8_t value = autoconfig_window_byte(cfg_window, cur);

        if (value != 0x00)
            all_zero = 0;
        if (value != 0xff)
            all_ff = 0;
        if (value != before[cur])
            return (false);
    }

    return (!all_zero && !all_ff);
}

static void
autoconfig_shutup_window(ac_cfg_window_t cfg_window)
{
    if (cfg_window == AC_CFG_WINDOW_Z3)
        *Z3_SHUTUP = 0;
    else
        *Z2_SHUTUP = 0;
}

static bool
autoconfig_advance_if_still_visible(ac_cfg_window_t cfg_window,
                                    const uint8_t before[PRESENT_CHECK])
{
    uint tries;

    for (tries = 0; tries < AUTOCONFIG_SETTLE_TRIES; tries++) {
        if (!autoconfig_signature_same(cfg_window, before))
            return (true);
        timer_delay_msec(AUTOCONFIG_SETTLE_MSEC);
    }

    if (!autoconfig_signature_same(cfg_window, before))
        return (true);

    printf("Z%c config window still has same card after assignment; "
           "sending shutup\n",
           (cfg_window == AC_CFG_WINDOW_Z3) ? '3' : '2');
    autoconfig_shutup_window(cfg_window);

    for (tries = 0; tries < AUTOCONFIG_SETTLE_TRIES; tries++) {
        timer_delay_msec(AUTOCONFIG_SETTLE_MSEC);
        if (!autoconfig_signature_same(cfg_window, before))
            return (true);
    }

    printf("Z%c config window still has same card after shutup\n",
           (cfg_window == AC_CFG_WINDOW_Z3) ? '3' : '2');
    return (false);
}

static void
autoconfig_write_base(ac_cfg_window_t cfg_window, uint32_t addr,
                      uint use_z3_assignment)
{
    if (cfg_window == AC_CFG_WINDOW_Z3) {
        (void) use_z3_assignment;

        /*
         * Zorro III config-space assignment is committed by writing register
         * 0x44.  This applies even when the decoded board will live in Zorro
         * II address space, as on systems that expose Z2 cards through the
         * Z3 config aperture.
         */
        *Z3_BASE_A19_A16 = (addr >> 12);  // Nibble
        *Z3_BASE_A23_A16 = (addr >> 16);  // Byte
        *Z3_BASE_A31_A16 = (addr >> 16);  // Word
    } else {
        if (use_z3_assignment) {
            /* Zorro III assignment in the Zorro II config aperture. */
            *Z2_BASE_A27_A24 = (addr >> 20);  // Nibble
            *Z2_BASE_A31_A24 = (addr >> 24);  // Byte
            *Z2_BASE_A19_A16 = (addr >> 12);  // Nibble
            *Z2_BASE_A23_A16 = (addr >> 16);  // Byte
        } else {
            /* Zorro II assignment in the Zorro II config aperture. */
            *Z2_BASE_A19_A16 = (addr >> 12);  // Nibble
            *Z2_BASE_A23_A16 = (addr >> 16);  // Byte
        }
    }
}

static void
show_creg_value(uint reg, uint8_t value)
{
    printf("   %02x   %02x", reg, value);
}

static uint8_t
show_creg(uint8_t *cfgdata, uint reg)
{
    uint8_t value = cfgdata[reg / 4];
    show_creg_value(reg, value);
    return (value);
}

static int
autoconfig_reserved(uint8_t *cfgdata, uint reg)
{
    uint8_t value = cfgdata[reg / 4];
    if (value != 0x00) {
        show_creg_value(reg, value);
        printf(" Reserved: should be 0x00\n");
        return (1);
    }
    return (0);
}

static void
autoconfig_assign(ac_t *node, uint is_z3)
{
    /* Fill in ac_mfg and ac_product */
    if (!is_z3) {
        node->ac_mfg = ~((get_z2_byte(0x10 / 4) << 8) | get_z2_byte(0x14 / 4));
        node->ac_product = ~get_z2_byte(0x04 / 4);
    } else {
        node->ac_mfg = ~((get_z3_byte(0x10 / 4) << 8) | get_z3_byte(0x14 / 4));
        node->ac_product = ~get_z3_byte(0x04 / 4);
    }
}

static rc_t
autoconfig_decode(uint8_t *cfgdata, uint cfgsize)
{
    uint8_t  value;
    uint32_t value32;
    int      errs = 0;
    int      is_z3 = 0;
    int      is_autoboot = 0;
    uint     byte;
    uint     product;
    const char *winsize;

    /* 0x00 check */
    for (byte = 0; byte < cfgsize; byte++)
        if (cfgdata[byte] != 0x00)
            break;
    if (byte == cfgsize)
        return (RC_NO_DATA);  // All 0x00

    /* 0xff check */
    for (byte = 0; byte < cfgsize; byte++)
        if (cfgdata[byte] != 0xff)
            break;
    if (byte == cfgsize)
        return (RC_NO_DATA);  // All 0xff

    printf("  Reg Data Decode\n");
    value = ~show_creg(cfgdata, 0x00);
    switch (value >> 6) {
        case 0:
        case 1:
            printf(" Zorro_Reserved");
            break;
        case 2:
            printf(" ZorroIII");
            is_z3 = 1;
            break;
        case 3:
            printf(" ZorroII");
            break;
    }
    if (value & BIT(5))
        printf(" Memory");
    if (cfgdata[AC_ROM_FLAGS] & AC_FLAG_SIZE_EXT)
        winsize = z3_config_sizes[value & 0x7];
    else
        winsize = z2_config_sizes[value & 0x7];
    printf(" Size=%s", winsize);
    if (value & BIT(4)) {
        printf(" Autoboot");
        is_autoboot = 1;
    }
    if (value & BIT(3))
        printf(" Link-to-next");
    printf("\n");

    product = show_creg(cfgdata, 0x04) & 0xff;
    printf(" Product=0x%02x\n", product);

    value = show_creg(cfgdata, 0x08);
    if (is_z3) {
        if (value & BIT(7)) {
            printf(" Device-Memory");
        } else {
            printf(" Device-IO");
        }
    } else {
        if (value & BIT(7))
            printf(" Fit-ZorroII");
        else
            printf(" Fit-anywhere");
    }
    if (value & BIT(6))
        printf(" NoShutup");
    else
        printf(" CanShutup");
    if (is_z3 && ((value & BIT(4)) == 0))
        printf(" Invalid_RSVD");

    if (value & BIT(5))
        printf(" SizeExt");
    printf(" %s\n", config_subsizes[value & 0x0f]);

    if (autoconfig_reserved(cfgdata, 0x0c))
        errs = 1;

    value32 = show_creg(cfgdata, 0x10) << 8;
    printf(" Mfg Number high byte\n");
    value32 |= show_creg(cfgdata, 0x14);
    printf(" Mfg Number low byte    ID 0x%04x.0x%02x  %u / %u\n",
           value32, product, value32, product);

    value32 = 0;
    for (byte = 0; byte < 4; byte++) {
        value32 <<= 8;
        value32 |= show_creg(cfgdata, 0x18 + byte * 4);
        printf(" Serial number byte %d", byte);
        if (byte == 3)
            printf("   Serial=0x%08x", value32);
        printf("\n");
    }

    if (is_autoboot) {
        value32 = show_creg(cfgdata, 0x28) << 8;
        printf(" Option ROM vector high\n");
        value32 |= show_creg(cfgdata, 0x2c);
        printf(" Option ROM vector low  Offset=0x%04x\n", value32);
    }
    for (byte = 0x30; byte <= 0x40; byte += 4)
        errs += autoconfig_reserved(cfgdata, byte);
#if 0
    /* NOTE: We don't load these bytes, so don't bother checking them */
    for (byte = 0x4c; byte <= 0x7c; byte += 4)
        errs += autoconfig_reserved(cfgdata, byte);
#endif

    if (errs != 0)
        return (RC_FAILURE);
    return (RC_SUCCESS);
}

static uint
z2_is_present(void)
{
    uint cur;
    for (cur = 0; cur < PRESENT_CHECK; cur++)
        if (get_z2_byte(cur) != 0x00)
            break;
    if (cur != PRESENT_CHECK) {
        for (cur = 0; cur < PRESENT_CHECK; cur++)
            if (get_z2_byte(cur) != 0xff)
                break;
    }
    if (cur != PRESENT_CHECK)
        return (1);  // Board does not have all 0x00 or 0xff values
    return (0);
}

static uint
z3_is_present(void)
{
    uint cur;
    for (cur = 0; cur < PRESENT_CHECK; cur++)
        if (get_z3_byte(cur) != 0x00)
            break;
    if (cur != PRESENT_CHECK) {
        for (cur = 0; cur < PRESENT_CHECK; cur++)
            if (get_z3_byte(cur) != 0xff)
                break;
    }
    if (cur != PRESENT_CHECK)
        return (1);  // Board does not have all 0x00 or 0xff values
    return (0);
}

rc_t
autoconfig_show(void)
{
    uint cur;
    uint8_t buf[17];
    rc_t rc1;
    rc_t rc2;

    /* Check Zorro II base */
    printf("ZII  00e80000:");
    for (cur = 0; cur < sizeof (buf); cur++) {
        uint8_t value = get_z2_byte(cur);
        buf[cur] = ~value;
        printf(" %02x", value);
    }
    printf("\n");
    rc1 = autoconfig_decode(buf, sizeof (buf));
    printf("ZIII ff000000:");
    for (cur = 0; cur < sizeof (buf); cur++) {
        uint8_t value = get_z3_byte(cur);
        buf[cur] = ~value;
        printf(" %02x", value);
    }
    printf("\n");
    rc2 = autoconfig_decode(buf, sizeof (buf));
    if (rc2 == RC_SUCCESS)
        rc1 = rc2;  // Success with either is success.
    return (rc1);
}

rc_t
autoconfig_shutup(void)
{
    if (z3_is_present()) {
        printf("Telling ZIII to shut up\n");
        *Z3_SHUTUP = 0;
        return (RC_SUCCESS);
    }

    if (z2_is_present()) {
        printf("Telling ZII to shut up\n");
        *Z2_SHUTUP = 0;
        return (RC_SUCCESS);
    }
    return (RC_NO_DATA);
}

static void
show_autoconfig(ac_t *node)
{
    char type;
    switch (node->ac_type) {
        case AC_TYPE_ALLOC_Z2:
            type = '2';
            break;
        case AC_TYPE_ALLOC_Z3:
            type = '3';
            break;
        default:
            type = '?';
            break;
    }
    printf("Z%c autoconfig at %08x  size %08x  0x%04x.0x%02x %u / %u\n", type,
           node->ac_addr, node->ac_size,
           node->ac_mfg, node->ac_product,
           node->ac_mfg, node->ac_product);
}

static rc_t
autoconfig_z2_address(uint32_t addr)
{
    uint8_t  cfg0       = get_z2_byte(0);
    uint8_t  cfgflags   = ~get_z2_byte(AC_ROM_FLAGS);
    uint     is_z3_size = cfgflags & AC_FLAG_SIZE_EXT;
    uint     is_z3 = 0;
    uint     addr_z3 = 0;
    uint32_t devsize;
    uint8_t  signature[PRESENT_CHECK];

    autoconfig_signature_read(AC_CFG_WINDOW_Z2, signature);

    switch (cfg0 >> 6) {
        case 2:  // Zorro III
            is_z3 = 1;
            break;
        case 3:  // Zorro II
            is_z3 = 0;
            break;
        default:
            printf("Invalid board (%x) detected for Zorro II\n", cfg0);
            return (RC_FAILURE);
    }

    /* Confirm that the address is allowed based on the board config */
    if (is_z3_size)
        devsize = z3_config_sizenums[cfg0 & 0x7];
    else
        devsize = z2_config_sizenums[cfg0 & 0x7];
    if (devsize == 0) {
        printf("Invalid board size (%x) detected for Zorro II\n", cfg0);
        return (RC_FAILURE);
    }

    if (addr == 0)
        addr_z3 = is_z3 || is_z3_size;
    else if (addr >= 0x10000000)
        addr_z3 = 1;
    else
        addr_z3 = 0;

    ac_t *node = autoconfig_alloc_board(addr, devsize, cfg0 & 0x7,
                                        is_z3, is_z3_size, addr_z3);
    if (node == NULL)
        return (RC_BAD_PARAM);
    addr = node->ac_addr;

    autoconfig_assign(node, 0);
    autoconfig_write_base(AC_CFG_WINDOW_Z2, addr, is_z3 || addr_z3);
    show_autoconfig(node);
    return (autoconfig_advance_if_still_visible(AC_CFG_WINDOW_Z2, signature) ?
            RC_SUCCESS : RC_FAILURE);
}

static rc_t
autoconfig_z3_address(uint32_t addr)
{
    uint8_t  cfg0       = get_z3_byte(0);
    uint8_t  cfgflags   = ~get_z3_byte(AC_ROM_FLAGS);
    uint     is_z3_size = cfgflags & AC_FLAG_SIZE_EXT;
    uint     is_z3 = 0;
    uint     addr_z3 = 0;
    uint32_t devsize;
    uint8_t  signature[PRESENT_CHECK];

    autoconfig_signature_read(AC_CFG_WINDOW_Z3, signature);

    switch (cfg0 >> 6) {
        case 2:  // Zorro III
            is_z3 = 1;
            break;
        case 3:  // Zorro II, visible in Zorro III config space
            is_z3 = 0;
            break;
        default:
            printf("Invalid board (%x) detected for Zorro III\n", cfg0);
            return (RC_FAILURE);
    }

    /* Confirm that the address is allowed based on the board config */
    if (is_z3_size)
        devsize = z3_config_sizenums[cfg0 & 0x7];
    else
        devsize = z2_config_sizenums[cfg0 & 0x7];
    if (devsize == 0) {
        printf("Invalid board size (%x) detected for Zorro III\n", cfg0);
        return (RC_FAILURE);
    }

    if (addr == 0)
        addr_z3 = is_z3 || is_z3_size;
    else if (addr >= 0x10000000)
        addr_z3 = 1;
    else
        addr_z3 = 0;

    ac_t *node = autoconfig_alloc_board(addr, devsize, cfg0 & 0x7,
                                        is_z3, is_z3_size, addr_z3);

    if (node == NULL)
        return (RC_BAD_PARAM);
    addr = node->ac_addr;

    autoconfig_assign(node, 1);

    autoconfig_write_base(AC_CFG_WINDOW_Z3, addr, is_z3 || addr_z3);
    show_autoconfig(node);
    return (autoconfig_advance_if_still_visible(AC_CFG_WINDOW_Z3, signature) ?
            RC_SUCCESS : RC_FAILURE);
}

rc_t
autoconfig_address(uint32_t addr)
{
    if ((addr >= 0x10000000) && (addr < 0x80000000))
        return (autoconfig_z3_address(addr));
    if (((addr >= 0x00200000) && (addr < 0x00a00000)) ||
        ((addr >= 0x00e90000) && (addr < 0x00f00000)))
        return (autoconfig_z2_address(addr));
    if (addr != 0) {
        printf("0x%08x is not a valid Zorro address\n", addr);
        return (RC_BAD_PARAM);
    }

    /*
     * Match Kickstart's dynamic probe order: the Zorro III config aperture
     * is checked first. Zorro II cards that legally appear there are still
     * assigned to Zorro II address space based on their decoded type/size.
     */
    if (z3_is_present())
        return (autoconfig_z3_address(0));
    if (z2_is_present())
        return (autoconfig_z2_address(0));

    return (RC_NO_DATA);
}

/*
 * autoconfig_configure_all
 * ------------------------
 * Assign addresses to all devices currently visible through the autoconfig
 * chain. Devices must be configured to make their normal register windows
 * accessible.
 */
uint
autoconfig_configure_all(void)
{
    uint count = 0;
    rc_t rc;

    while ((rc = autoconfig_address(0)) == RC_SUCCESS) {
        count++;
        if (count >= AUTOCONFIG_MAX_BOARDS) {
            printf("Autoconfig stopped after %u boards; "
                   "current card may not be accepting assignment\n", count);
            break;
        }
    }
    if ((rc != RC_NO_DATA) && (count == 0))
        printf("Autoconfig stopped with rc=%d\n", rc);
    return (count);
}

/*
 * autoconfig_find
 * ---------------
 * Find a configured board by manufacturer and product id.
 */
bool
autoconfig_find(uint16_t mfg, uint8_t product, autoconfig_dev_t *dev)
{
    ac_t *cur;

    for (cur = ac_list; cur != NULL; cur = cur->ac_next) {
        if ((cur->ac_type != AC_TYPE_ALLOC_Z2) &&
            (cur->ac_type != AC_TYPE_ALLOC_Z3))
            continue;
        if ((cur->ac_mfg != mfg) || (cur->ac_product != product))
            continue;
        if (dev != NULL)
            autoconfig_dev_export(cur, dev);
        return (true);
    }
    return (false);
}

static void
autoconfig_insert(uint8_t type, uint32_t addr, uint32_t size)
{
    ac_t *node = malloc(sizeof (ac_t));
    if (node == NULL)
        return;
    node->ac_type = type;
    node->ac_addr = addr;
    node->ac_size = size;
    node->ac_next = ac_list;
    ac_list = node;
}

void
autoconfig_init(void)
{
    /* Create address range lists      Base        Size             Top */
    autoconfig_insert(AC_TYPE_FREE_Z3, 0x10000000, 0x30000000);  // 0x40000000
    autoconfig_insert(AC_TYPE_FREE_Z3, 0x40000000, 0x40000000);  // 0x80000000
    autoconfig_insert(AC_TYPE_FREE_Z2, 0x00200000, 0x00800000);  // 0x00a00000
    autoconfig_insert(AC_TYPE_FREE_Z2, 0x00e90000, 0x00070000);  // 0x0f000000

    *ADDR8(GARY_BTIMEOUT) &= ~BIT(7);  // Disable BERR
}
