/*
 * sm_rom_alias
 * ------------
 * Cache-inhibited MMU alias of the Kickstart ROM address range.
 *
 * Kicksmash is driven entirely by reads of the ROM address range: the
 * Amiga sends data as the addresses of the reads, and Kicksmash replies
 * with the data those reads return. Every read must therefore reach the
 * bus exactly once and in order, and it must reach the physical ROM even
 * when a ROM remapper such as MuFastRom has redirected the ROM addresses
 * to a copy in fast RAM. send_cmd() achieves this by turning off the
 * data cache and the MMU around every transfer.
 *
 * When mmu.library is present, this module instead maps an unused
 * logical address range onto the physical ROM, cache-inhibited and
 * serialized, and points sm_rom_base at it. send_cmd_core() then reads
 * through that range, and send_cmd() needs only to disable interrupts:
 * no CacheControl(), no cache push of the whole data cache, no MMU or
 * supervisor state switching on each transfer.
 *
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 */

#include <stdint.h>
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/lists.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <inline/macros.h>
#include "sm_msg.h"
#include "smash_cmd.h"
#include "cpu_control.h"

#define ROM_BASE        0x00f80000  /* Base address of Kickstart ROM */
#define ROM_ALIAS_SIZE  0x00080000  /* All of $F80000-$FFFFFF */

/*
 * The alias is placed in a hole of the 32-bit address space, below
 * ALIAS_LIMIT and at or above ALIAS_FLOOR. This keeps it out of the
 * 24-bit address space and out of the top 16 MB, which some boards
 * use for a mirror of the 24-bit space.
 */
#define ALIAS_FLOOR     0x01000000
#define ALIAS_LIMIT     0xff000000

/*
 * Definitions from mmu.library (MMULib by Thomas Richter): mmu/context.h,
 * mmu/mmutags.h and the library's function offsets. The toolchain does
 * not include the MMULib headers.
 */
struct MMUContext;

struct MappingNode {
    struct MappingNode *map_succ;
    struct MappingNode *map_pred;
    ULONG               map_Lower;       /* First address of this node */
    ULONG               map_Higher;      /* Last address, inclusive */
    ULONG               map_Flags;       /* Internal use only */
    ULONG               map_Properties;  /* MAPP_* */
    union {
        void           *map_UserData;    /* For MAPP_INVALID */
        LONG            map_Delta;       /* For MAPP_REMAPPED */
    } map_un;
};

#define MAPP_WRITEPROTECTED     (1UL << 2)
#define MAPP_USED               (1UL << 3)
#define MAPP_MODIFIED           (1UL << 4)
#define MAPP_CACHEINHIBIT       (1UL << 6)
#define MAPP_SUPERVISORONLY     (1UL << 7)
#define MAPP_BLANK              (1UL << 11)
#define MAPP_SINGLEPAGE         (1UL << 12)
#define MAPP_COPYBACK           (1UL << 13)
#define MAPP_INVALID            (1UL << 14)
#define MAPP_REMAPPED           (1UL << 15)
#define MAPP_SWAPPED            (1UL << 16)
#define MAPP_ROM                (1UL << 17)
#define MAPP_SHARED             (1UL << 18)
#define MAPP_TRANSLATED         (1UL << 19)
#define MAPP_REPAIRABLE         (1UL << 20)
#define MAPP_IMPRECISE          (1UL << 21)
#define MAPP_INDIRECT           (1UL << 22)
#define MAPP_BUNDLED            (1UL << 23)
#define MAPP_USER0              (1UL << 24)
#define MAPP_USER1              (1UL << 25)
#define MAPP_USER2              (1UL << 26)
#define MAPP_USER3              (1UL << 27)
#define MAPP_NONSERIALIZED      (1UL << 29)
#define MAPP_IO                 (1UL << 30)
#define MAPP_WINDOW             (1UL << 31)

/* GetMMUType() results, from mmu/mmubase.h */
#define MUTYPE_68030            '3'
#define MUTYPE_68040            '4'
#define MUTYPE_68060            '6'

#define MAPTAG_DUMMY            (TAG_USER + 0x03e00000)
#define MAPTAG_DESTINATION      (MAPTAG_DUMMY + 1)
#define MAPTAG_USERDATA         (MAPTAG_DUMMY + 3)

/* Properties which mark a hole that nothing else is using */
#define HOLE_ANY        (MAPP_INVALID | MAPP_BLANK)
#define HOLE_EXCLUDED   (MAPP_REMAPPED | MAPP_SWAPPED | MAPP_SHARED | \
                         MAPP_TRANSLATED | MAPP_REPAIRABLE | MAPP_INDIRECT | \
                         MAPP_BUNDLED | MAPP_WINDOW | MAPP_SINGLEPAGE | \
                         MAPP_USER0 | MAPP_USER1 | MAPP_USER2 | MAPP_USER3)

/* Properties this module sets or clears in the alias range */
#define ALIAS_PROPS     (MAPP_REMAPPED | MAPP_CACHEINHIBIT | MAPP_USED)
#define ALIAS_MASK      (MAPP_WRITEPROTECTED | MAPP_USED | \
                         MAPP_CACHEINHIBIT | MAPP_SUPERVISORONLY | \
                         MAPP_BLANK | MAPP_COPYBACK | MAPP_INVALID | \
                         MAPP_REMAPPED | MAPP_ROM | MAPP_IMPRECISE | \
                         MAPP_NONSERIALIZED | MAPP_IO)

#define MMU_BASE_NAME MMUBase

#define GetMapping(ctx) \
    LP1(0x24, struct MinList *, GetMapping, \
        struct MMUContext *, ctx, a0, , MMU_BASE_NAME)
#define ReleaseMapping(ctx, list) \
    LP2NR(0x2a, ReleaseMapping, struct MMUContext *, ctx, a0, \
          struct MinList *, list, a1, , MMU_BASE_NAME)
#define GetPageSize(ctx) \
    LP1(0x30, ULONG, GetPageSize, \
        struct MMUContext *, ctx, a0, , MMU_BASE_NAME)
#define GetMMUType() \
    LP0(0x36, char, GetMMUType, , MMU_BASE_NAME)
#define LockMMUContext(ctx) \
    LP1NR(0x48, LockMMUContext, \
          struct MMUContext *, ctx, a0, , MMU_BASE_NAME)
#define UnlockMMUContext(ctx) \
    LP1NR(0x4e, UnlockMMUContext, \
          struct MMUContext *, ctx, a0, , MMU_BASE_NAME)
#define SetPropertiesA(ctx, flags, mask, lower, size, tags) \
    LP6(0x54, BOOL, SetPropertiesA, struct MMUContext *, ctx, a0, \
        ULONG, flags, d1, ULONG, mask, d2, ULONG, lower, a1, \
        ULONG, size, d0, struct TagItem *, tags, a2, , MMU_BASE_NAME)
#define SuperContext(ctx) \
    LP1(0x90, struct MMUContext *, SuperContext, \
        struct MMUContext *, ctx, a0, , MMU_BASE_NAME)
#define DefaultContext() \
    LP0(0x96, struct MMUContext *, DefaultContext, , MMU_BASE_NAME)
#define PhysicalLocation(ctx, addrptr, lenptr) \
    LP3(0x102, ULONG, PhysicalLocation, struct MMUContext *, ctx, d1, \
        void **, addrptr, a0, ULONG *, lenptr, a1, , MMU_BASE_NAME)
#define RebuildTreesA(ctxlist) \
    LP1(0x168, BOOL, RebuildTreesA, \
        struct MMUContext **, ctxlist, a0, , MMU_BASE_NAME)

static struct Library     *MMUBase;
static struct MMUContext  *alias_ctx[3];       /* user, supervisor, NULL */
static ULONG               alias_lower;        /* 0 if no alias */
static ULONG               alias_old_props[2]; /* Restored on close */
static void               *alias_old_data[2];

/*
 * mmu_enabled
 * -----------
 * Return non-zero if the CPU has a working MMU and it is translating
 * addresses. mmu.library may be installed without having enabled the
 * MMU, and then a mapping would have no effect.
 *
 * TC is read only after mmu.library has confirmed an MMU of the type
 * that matches cpu_type: an EC CPU has no MMU, and reading TC with an
 * instruction for a different MMU type is not safe.
 */
static int
mmu_enabled(void)
{
    uint32_t tc = 0;
    char expected;
    APTR old_stack;

    switch (cpu_type) {
        case 68030:
            expected = MUTYPE_68030;
            break;
        case 68040:
            expected = MUTYPE_68040;
            break;
        case 68060:
            expected = MUTYPE_68060;
            break;
        default:
            return (0);
    }
    if (GetMMUType() != expected)
        return (0);

    old_stack = SuperState();

    if (cpu_type == 68030)
        tc = mmu_get_tc_030() & BIT(31);
    else if ((cpu_type == 68040) || (cpu_type == 68060))
        tc = mmu_get_tc_040() & BIT(15);
    UserState(old_stack);
    return (tc != 0);
}

/*
 * find_node
 * ---------
 * Return the node of a mapping which covers all of lower..higher.
 */
static struct MappingNode *
find_node(struct MinList *list, ULONG lower, ULONG higher)
{
    struct MappingNode *node;

    for (node = (struct MappingNode *) list->mlh_Head; node->map_succ != NULL;
         node = node->map_succ) {
        if ((node->map_Lower <= lower) && (node->map_Higher >= higher))
            return (node);
    }
    return (NULL);
}

static int
is_hole(struct MappingNode *node)
{
    return ((node != NULL) &&
            ((node->map_Properties & HOLE_ANY) != 0) &&
            ((node->map_Properties & HOLE_EXCLUDED) == 0));
}

/*
 * find_hole
 * ---------
 * Find the highest ROM_ALIAS_SIZE-aligned range which is an unused
 * hole in both the user and the supervisor mapping.
 */
static ULONG
find_hole(struct MinList *ulist, struct MinList *slist)
{
    struct MappingNode *node;

    for (node = (struct MappingNode *) ulist->mlh_TailPred;
         node->map_pred != NULL; node = node->map_pred) {
        ULONG higher = node->map_Higher;
        ULONG lower;

        if (!is_hole(node) || (node->map_Lower >= ALIAS_LIMIT))
            continue;
        if (higher > ALIAS_LIMIT - 1)
            higher = ALIAS_LIMIT - 1;
        if (higher < ALIAS_FLOOR + ROM_ALIAS_SIZE - 1)
            continue;
        lower = (higher + 1 - ROM_ALIAS_SIZE) & ~(ROM_ALIAS_SIZE - 1);
        if ((lower < node->map_Lower) || (lower < ALIAS_FLOOR))
            continue;
        if (is_hole(find_node(slist, lower, lower + ROM_ALIAS_SIZE - 1)))
            return (lower);
    }
    return (0);
}

/*
 * set_props
 * ---------
 * Set the alias range properties in one context. MAPP_REMAPPED and
 * MAPP_INVALID both use the single user data item of a range, so the
 * matching tag must always be passed.
 */
static BOOL
set_props(struct MMUContext *ctx, ULONG props, void *userdata)
{
    struct TagItem tags[2];

    if (props & MAPP_REMAPPED) {
        tags[0].ti_Tag = MAPTAG_DESTINATION;
        tags[0].ti_Data = ROM_BASE;
    } else {
        tags[0].ti_Tag = MAPTAG_USERDATA;
        tags[0].ti_Data = (ULONG) userdata;
    }
    tags[1].ti_Tag = TAG_DONE;
    tags[1].ti_Data = 0;
    return (SetPropertiesA(ctx, props, ALIAS_MASK, alias_lower,
                           ROM_ALIAS_SIZE, tags));
}

static void
restore_props(void)
{
    uint pos;

    for (pos = 0; alias_ctx[pos] != NULL; pos++)
        (void) set_props(alias_ctx[pos], alias_old_props[pos],
                         alias_old_data[pos]);
    (void) RebuildTreesA(alias_ctx);
}

static void
unlock_contexts(void)
{
    if (alias_ctx[1] != NULL)
        UnlockMMUContext(alias_ctx[1]);
    UnlockMMUContext(alias_ctx[0]);
}

/*
 * map_alias
 * ---------
 * Map an unused logical range onto the physical ROM in the default user
 * context and its supervisor context. Returns the logical address, or 0.
 */
static ULONG
map_alias(void)
{
    struct MinList *list[2] = { NULL, NULL };
    struct MappingNode *node;
    uint  pos;
    ULONG lower = 0;
    ULONG pagesize;

    alias_ctx[0] = DefaultContext();
    alias_ctx[1] = SuperContext(alias_ctx[0]);
    alias_ctx[2] = NULL;
    if (alias_ctx[0] == NULL)
        return (0);
    if (alias_ctx[1] == alias_ctx[0])
        alias_ctx[1] = NULL;

    pagesize = GetPageSize(alias_ctx[0]);
    if ((pagesize == 0) || ((ROM_ALIAS_SIZE % pagesize) != 0))
        return (0);

    LockMMUContext(alias_ctx[0]);
    if (alias_ctx[1] != NULL)
        LockMMUContext(alias_ctx[1]);

    for (pos = 0; alias_ctx[pos] != NULL; pos++) {
        list[pos] = GetMapping(alias_ctx[pos]);
        if (list[pos] == NULL)
            goto done;
    }

    alias_lower = find_hole(list[0], (list[1] != NULL) ? list[1] : list[0]);
    if (alias_lower == 0)
        goto done;

    for (pos = 0; alias_ctx[pos] != NULL; pos++) {
        node = find_node(list[pos], alias_lower,
                         alias_lower + ROM_ALIAS_SIZE - 1);
        alias_old_props[pos] = node->map_Properties;
        alias_old_data[pos] = node->map_un.map_UserData;
    }

    for (pos = 0; alias_ctx[pos] != NULL; pos++) {
        if (!set_props(alias_ctx[pos], ALIAS_PROPS, NULL)) {
            restore_props();
            goto done;
        }
    }
    if (!RebuildTreesA(alias_ctx)) {
        restore_props();
        goto done;
    }
    lower = alias_lower;

done:
    for (pos = 0; pos < 2; pos++)
        if (list[pos] != NULL)
            ReleaseMapping(alias_ctx[pos], list[pos]);
    unlock_contexts();
    if (lower == 0)
        alias_lower = 0;
    return (lower);
}

/*
 * sm_rom_alias_open
 * -----------------
 * Set up the cache-inhibited ROM alias and switch Kicksmash accesses to
 * it. If mmu.library is not loaded or finds no working MMU (such as on
 * an EC CPU), the MMU is not enabled, no unused
 * address range is found, or the alias does not reach Kicksmash, accesses stay at the ROM address and
 * send_cmd() keeps disabling the cache and MMU around each transfer.
 *
 * Must be called from a task (not an interrupt), with cpu_control_init()
 * already done. Returns the alias address, or 0 if none was set up.
 */
uint
sm_rom_alias_open(void)
{
    ULONG lower;
    void *phys;
    ULONG len = ROM_ALIAS_SIZE;
    uint  tries;
    uint  rc = MSG_STATUS_FAIL;

    if (alias_lower != 0)
        return (alias_lower);
    if ((cpu_type != 68030) && (cpu_type != 68040) && (cpu_type != 68060))
        return (0);

    /*
     * Only use mmu.library if something has already loaded it. Opening
     * it here for the first time would make it take over the MMU on a
     * system whose owner did not set that up.
     */
    Forbid();
    if (FindName(&SysBase->LibList, "mmu.library") != NULL)
        MMUBase = OpenLibrary("mmu.library", 43);
    Permit();
    if (MMUBase == NULL)
        return (0);
    if (!mmu_enabled())
        goto fail;

    lower = map_alias();
    if (lower == 0)
        goto fail;

    /* Check that the mmu.library agrees on where the alias leads */
    phys = (void *) lower;
    if ((PhysicalLocation(alias_ctx[0], &phys, &len) & MAPP_CACHEINHIBIT) ==
        0 || (phys != (void *) ROM_BASE) || (len < ROM_ALIAS_SIZE)) {
        goto fail_unmap;
    }

    /* Check that Kicksmash answers through the alias */
    Disable();
    sm_rom_base = lower;
    Enable();
    for (tries = 0; tries < 4; tries++) {
        rc = send_cmd(KS_CMD_NOP, NULL, 0, NULL, 0, NULL);
        if (rc == KS_STATUS_OK)
            return (lower);
    }
    Disable();
    sm_rom_base = ROM_BASE;
    Enable();

fail_unmap:
    LockMMUContext(alias_ctx[0]);
    if (alias_ctx[1] != NULL)
        LockMMUContext(alias_ctx[1]);
    restore_props();
    unlock_contexts();
    alias_lower = 0;
fail:
    CloseLibrary(MMUBase);
    MMUBase = NULL;
    return (0);
}

/*
 * sm_rom_alias_close
 * ------------------
 * Switch Kicksmash accesses back to the ROM address and remove the alias.
 */
void
sm_rom_alias_close(void)
{
    if (alias_lower == 0)
        return;

    /* send_cmd() checks sm_rom_base with interrupts disabled */
    Disable();
    sm_rom_base = ROM_BASE;
    Enable();

    LockMMUContext(alias_ctx[0]);
    if (alias_ctx[1] != NULL)
        LockMMUContext(alias_ctx[1]);
    restore_props();
    unlock_contexts();
    alias_lower = 0;

    CloseLibrary(MMUBase);
    MMUBase = NULL;
}
