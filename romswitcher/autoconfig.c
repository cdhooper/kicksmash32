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
#include "printf.h"
#include "util.h"
#include "med_cmdline.h"

#define Z2_CFG_BASE 0x00e80000
#define Z3_CFG_BASE 0xff000000

#define Z2_SHUTUP       VADDR8(Z2_CFG_BASE + 0x4c)  // Go to next autoconfig
#define Z3_SHUTUP       VADDR8(Z3_CFG_BASE + 0x4c)  // Go to next autoconfig
#define Z2_BASE_A27_A24 VADDR8(Z2_CFG_BASE + 0x46)  // Z3 in Z2 space (1)
#define Z2_BASE_A31_A24 VADDR8(Z2_CFG_BASE + 0x44)  // Z3 in Z2 space (2)
#define Z2_BASE_A19_A16 VADDR8(Z2_CFG_BASE + 0x4a)  // Z2 / Z3 in Z2 space (3)
#define Z2_BASE_A23_A16 VADDR8(Z2_CFG_BASE + 0x48)  // Z2 / Z3 in Z2 space (4)

#define Z3_BASE_A23_A16 VADDR8(Z3_CFG_BASE + 0x48)  // Z3 in Z3 space (0)
#define Z3_BASE_A31_A24 VADDR8(Z3_CFG_BASE + 0x44)  // Z3 in Z3 space (1a)
#define Z3_BASE_A31_A16 VADDR16(Z3_CFG_BASE + 0x44)  // Z3 in Z3 space (1b)

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

#define AC_TYPE_INVALID  0
#define AC_TYPE_ALLOC_Z2 1  // Allocated to Zorro II device
#define AC_TYPE_ALLOC_Z3 2  // Allocated to Zorro III device
#define AC_TYPE_FREE_Z2  3  // Free in Zorro II address range
#define AC_TYPE_FREE_Z3  4  // Free in Zorro III address range

typedef struct ac_t ac_t;
struct ac_t {
    ac_t    *ac_next;
    uint8_t  ac_type;
    uint8_t  ac_product;
    uint16_t ac_mfg;
    uint32_t ac_addr;
    uint32_t ac_size;
};

static ac_t *ac_list = NULL;

/*
 * autoconfig_alloc
 * ----------------
 * Allocate a Zorro address in the specified address space
 */
static ac_t *
autoconfig_alloc(uint addr, uint size, uint zorro_type)
{
    ac_t *cur;
    for (cur = ac_list; cur != NULL; cur = cur->ac_next) {
        if (cur->ac_type != zorro_type)
            continue;  // Not free

        if (cur->ac_size < size)
            continue;

        if ((addr != 0) &&
            ((cur->ac_addr > addr) ||
             (cur->ac_addr + cur->ac_size < addr + size)))
            continue;  // Not within this range

        if ((addr != 0) && (addr > cur->ac_addr)) {
            /* Fragment this entry (request is inside entry) */
            uint frag_size = addr - cur->ac_addr;
            ac_t *node = malloc(sizeof (ac_t));
            node->ac_type = cur->ac_type;
            node->ac_size = frag_size;
            node->ac_addr = addr;
            node->ac_next = cur->ac_next;

            cur->ac_size = cur->ac_size - frag_size;
            cur->ac_next = node;
            cur = node;
        }
        if (cur->ac_size >= size + 0x10000) {
            /* Fragment this entry (request is at start of entry) */
            ac_t *node = malloc(sizeof (ac_t));
            node->ac_type = cur->ac_type;
            node->ac_size = cur->ac_size - size;
            node->ac_addr = cur->ac_addr + size;
            node->ac_next = cur->ac_next;
            cur->ac_next = node;
        }
        cur->ac_size = size;
        if (zorro_type == AC_TYPE_FREE_Z2)
            cur->ac_type = AC_TYPE_ALLOC_Z2;
        else
            cur->ac_type = AC_TYPE_ALLOC_Z3;
        return (cur);
    }
    printf("Could not allocate");
    if (addr != 0)
        printf(" %08x", addr);
    printf(" in %s space\n", (zorro_type == AC_TYPE_FREE_Z2) ? "Z2" : "Z3");
    return (NULL);
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
    uint8_t unibble = (*ADDR16(Z2_CFG_BASE + offset * 4 + 0) >> 8) & 0xf0;
    uint8_t lnibble = (*ADDR16(Z2_CFG_BASE + offset * 4 + 2) >> 12) & 0x0f;
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
    uint8_t unibble = (*ADDR16(Z3_CFG_BASE + offset * 4 + 0x000) >> 8) & 0xf0;
    uint8_t lnibble = (*ADDR16(Z3_CFG_BASE + offset * 4 + 0x100) >> 12) & 0x0f;
    return (unibble | lnibble);
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
    if (is_z3 && ((~cfgdata[0x00]) & BIT(5)))
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

#define PRESENT_CHECK 16
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
    /* Try Zorro II first */
    if (z2_is_present()) {
        printf("Telling ZII to shut up\n");
        *Z2_SHUTUP = 0;
        return (RC_SUCCESS);
    }

    /* Try Zorro III */
    if (z3_is_present()) {
        printf("Telling ZIII to shut up\n");
        *Z3_SHUTUP = 0;
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
    uint     is_z3_size = cfg0 & BIT(5);
    uint     is_z3 = 0;
    uint     addr_z3 = 0;
    uint32_t devsize;

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
    if (is_z3 && is_z3_size)
        devsize = z3_config_sizenums[cfg0 & 0x7];
    else
        devsize = z2_config_sizenums[cfg0 & 0x7];

    if (addr == 0)
        addr_z3 = is_z3;
    else if (addr >= 0x10000000)
        addr_z3 = 1;
    else
        addr_z3 = 0;

    ac_t *node = autoconfig_alloc(addr, devsize,
                                  addr_z3 ? AC_TYPE_FREE_Z3 : AC_TYPE_FREE_Z2);
    if (node == NULL)
        return (RC_BAD_PARAM);
    addr = node->ac_addr;

    if (addr & (devsize - 1)) {
        printf("Address %08x not aligned to device size %08x; try %08x\n",
               addr, devsize, (addr + devsize - 1) & ~(devsize - 1));
        return (RC_BAD_PARAM);
    }

    autoconfig_assign(node, 0);
    if (is_z3) {
        *Z2_BASE_A27_A24 = (addr >> 20);  // Nibble
        *Z2_BASE_A31_A24 = (addr >> 24);  // Byte
        *Z2_BASE_A19_A16 = (addr >> 12);  // Nibble
        *Z2_BASE_A23_A16 = (addr >> 16);  // Byte
    } else {
        *Z2_BASE_A19_A16 = (addr >> 12);  // Nibble
        *Z2_BASE_A23_A16 = (addr >> 16);  // Byte
    }
    show_autoconfig(node);
    return (RC_SUCCESS);
}

static rc_t
autoconfig_z3_address(uint32_t addr)
{
    uint8_t  cfg0       = get_z3_byte(0);
    uint     is_z3_size = cfg0 & BIT(5);
    uint     addr_z3 = 0;
    uint32_t devsize;

    if ((cfg0 >> 6) != 2) {
        printf("Invalid board (%x) detected for Zorro III\n", cfg0);
        return (RC_FAILURE);
    }

    /* Confirm that the address is allowed based on the board config */
    if (is_z3_size)
        devsize = z3_config_sizenums[cfg0 & 0x7];
    else
        devsize = z2_config_sizenums[cfg0 & 0x7];

    if (addr == 0)
        addr_z3 = 1;  // Assume Zorro III
    else if (addr >= 0x10000000)
        addr_z3 = 1;
    else
        addr_z3 = 0;

    ac_t *node = autoconfig_alloc(addr, devsize,
                                  addr_z3 ? AC_TYPE_FREE_Z3 : AC_TYPE_FREE_Z2);

    if (node == NULL)
        return (RC_BAD_PARAM);
    addr = node->ac_addr;

    if (addr & (devsize - 1)) {
        printf("Address %08x not aligned to device size %08x; try %08x\n",
               addr, devsize, (addr + devsize - 1) & ~(devsize - 1));
        return (RC_BAD_PARAM);
    }

    autoconfig_assign(node, 1);

#undef CONFIG_Z3_FROM_Z2
#ifdef CONFIG_Z3_FROM_Z2
    /* Config in Z2 space */
    *Z2_BASE_A27_A24 = (addr >> 20);  // Nibble
    *Z2_BASE_A31_A24 = (addr >> 24);  // Byte
    *Z2_BASE_A23_A16 = (addr >> 16);  // Nibble
    *Z2_BASE_A23_A16 = (addr >> 16);  // Byte
#else
    /* Config in Z3 space, as specified in the hardware reference manual */
    *Z3_BASE_A23_A16 = (addr >> 16);  // Byte
    *Z3_BASE_A31_A16 = (addr >> 16);  // Word
#endif
    show_autoconfig(node);
    return (RC_SUCCESS);
}

rc_t
autoconfig_address(uint32_t addr)
{
    /* Dynamically allocate an address based on the board type */
    if (z3_is_present())
        return (autoconfig_z3_address(addr));
    if (z2_is_present())
        return (autoconfig_z2_address(addr));
    return (RC_NO_DATA);
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
