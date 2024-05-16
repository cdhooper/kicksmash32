/*
 * smash
 * -----
 * Utility to perform various operations with Kicksmash installed in
 * an Amiga.
 *
 * Copyright 2024 Chris Hooper. This program and source may be used
 * and distributed freely, for any purpose which benefits the Amiga
 * community. Commercial use of the binary, source, or algorithms requires
 * prior written approval from Chris Hooper <amiga@cdh.eebugs.com>.
 * All redistributions must retain this Copyright notice.
 *
 * DISCLAIMER: THE SOFTWARE IS PROVIDED "AS-IS", WITHOUT ANY WARRANTY.
 * THE AUTHOR ASSUMES NO LIABILITY FOR ANY DAMAGE ARISING OUT OF THE USE
 * OR MISUSE OF THIS UTILITY OR INFORMATION REPORTED BY THIS UTILITY.
 */
const char *version = "\0$VER: smash 0.2 ("__DATE__") © Chris Hooper";

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <clib/dos_protos.h>
#include <fcntl.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include "smash_cmd.h"
#include "host_cmd.h"
#include "crc32.h"
#include "sm_msg.h"
#include "cpu_control.h"

/*
 * gcc clib2 headers are bad (for example, no stdint definitions) and are
 * not being included by our build.  Because of that, we need to fix up
 * some stdio definitions.
 */
extern struct iob ** __iob;
#undef stdout
#define stdout ((FILE *)__iob[1])

#ifdef _DCC
typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned long  uint32_t;
typedef struct { unsigned long hi; unsigned long lo; } uint64_t;
#define __packed
#else
struct ExecBase *DOSBase;
struct Device   *TimerBase;
static struct    timerequest TimeRequest;
#endif

/*
 * ULONG has changed from NDK 3.9 to NDK 3.2.
 * However, PRI*32 did not. What is the right way to implement this?
 */
#if INCLUDE_VERSION < 47
#undef PRIu32
#define PRIu32 "lu"
#undef PRId32
#define PRId32 "ld"
#undef PRIx32
#define PRIx32 "lx"
#endif

#define BIT(x) (1U << (x))
#define ARRAY_SIZE(x) ((sizeof (x) / sizeof ((x)[0])))

#define ROM_BASE         0x00f80000  /* Base address of Kickstart ROM */
#define AMIGA_PPORT_DIR  0x00bfe301  /* Amiga parallel port dir register */
#define AMIGA_PPORT_DATA 0x00bfe101  /* Amiga parallel port data reg. */

#define CIAA_PRA         ADDR8(0x00bfe001)
#define CIAA_PRA_OVERLAY BIT(0)
#define CIAA_PRA_LED     BIT(1)

#define VALUE_UNASSIGNED 0xffffffff

#define TEST_LOOPBACK_BUF 4096
#define TEST_LOOPBACK_MAX 64
#define MEM_LOOPS         1000000
#define ROM_WINDOW_SIZE   (512 << 10)  // 512 KB
#define MAX_CHUNK         (16 << 10)   // 16 KB

static const char cmd_options[] =
    "usage: smash <options>\n"
    "   bank <opt>   ROM bank operations (-b ?, show, ...)\n"
    "   clock <opt>  save / restore Amiga clock with KS (-c)\n"
    "   debug        show debug output (-d)\n"
    "   erase <opt>  erase flash (-e ?, bank, ...)\n"
    "   identify     identify Kicksmash and Flash parts (-i[ii])\n"
    "   read <opt>   read from flash (-r ?, bank, file, ...)\n"
    "   verify <opt> verify flash matches file (-v ?, bank, file, ...)\n"
    "   write <opt>  write to flash (-w ?, bank, file, ...)\n"
    "   loop <num>   repeat the command a specified number of times (-l)\n"
    "   quiet        minimize test output\n"
    "   sr <addr>    spin loop reading address (-x)\n"
    "   srr <addr>   spin loop reading address with ROM OVL set (-y)\n"
    "   test[0123]   do interface test (-t)\n";

static const char cmd_bank_options[] =
    "  show                       Display all ROM bank information (-s)\n"
    "  merge <start> <end>        Merge banks for larger ROMs (-m)\n"
    "  unmerge <start> <end>      Unmerge banks (-u)\n"
    "  name <bank> <text>         Set bank name / description (-n)\n"
    "  longreset <bank> [<bank>]  Banks to sequence at long reset (-l)\n"
    "  poweron <bank> [reboot]    Default bank at poweron (-p)\n"
    "  current <bank> [reboot]    Force new bank immediately (-c)\n"
    "  nextreset <bank> [reboot]  Force new bank at next reset (-N)\n";

static const char cmd_clock_options[] =
    "   load         load Amiga time from KS clock (-l)\n"
    "   loadifset    load Amiga time from KS clock if it is known (-k)\n"
    "   save         save Amiga time to KS clock (-s)\n"
    "   saveifnotset save Amiga time to KS clock if not already saved (-n)\n"
    "   show         show current KS clock (-S)\n";

static const char cmd_read_options[] =
    "smash -r options\n"
    "   addr <hex>   starting address (-a)\n"
    "   bank <num>   flash bank on which to operate (-b)\n"
    "   dump         save hex/ASCII instead of binary (-d)\n"
    "   file <name>  file where to save content (-f)\n"
    "   len <hex>    length to read in bytes (-l)\n"
    "   swap <mode>  byte swap mode (1032, 2301, 3210) (-s)\n"
    "   yes          skip prompt (-y)\n";

static const char cmd_write_options[] =
    "smash -w options\n"
    "   addr <hex>   starting address (-a)\n"
    "   bank <num>   flash bank on which to operate (-b)\n"
//  "   dump         save hex/ASCII instead of binary (-d)\n"
    "   file <name>  file from which to read (-f)\n"
    "   len <hex>    length to program in bytes (-l)\n"
    "   swap <mode>  byte swap mode (1032, 2301, 3210) (-s)\n"
    "   yes          skip prompt (-y)\n";

static const char cmd_verify_options[] =
    "smash -v options\n"
    "   addr <hex>   starting address (-a)\n"
    "   bank <num>   flash bank on which to operate (-b)\n"
//  "   dump         save hex/ASCII instead of binary (-d)\n"
    "   file <name>  file to verify against (-f)\n"
    "   len <hex>    length to read in bytes (-l)\n"
    "   swap <mode>  byte swap mode (1032, 2301, 3210) (-s)\n"
    "   yes          skip prompt (-y)\n";

static const char cmd_erase_options[] =
    "smash -e options\n"
    "   addr <hex>   starting address (-a)\n"
    "   bank <num>   flash bank on which to operate (-b)\n"
    "   len <hex>    length to erase in bytes (-l)\n"
    "   yes          skip prompt (-y)\n";

typedef struct {
    const char *const short_name;
    const char *const long_name;
} long_to_short_t;
long_to_short_t long_to_short_main[] = {
    { "-b", "bank" },
    { "-c", "clock" },
    { "-d", "debug" },
    { "-e", "erase" },
    { "-i", "inquiry" },
    { "-i", "identify" },
    { "-i", "id" },
    { "-l", "loop" },
    { "-q", "quiet" },
    { "-r", "read" },
    { "-s", "spin" },
    { "-t", "test" },
    { "-v", "verify" },
    { "-w", "write" },
    { "-x", "sr" },   // spinread
    { "-y", "srr" },  // spinreadrom
};

long_to_short_t long_to_short_bank[] = {
    { "-c", "current" },
    { "-h", "?" },
    { "-h", "help" },
    { "-l", "longreset" },
    { "-m", "merge" },
    { "-n", "name" },
    { "-N", "nextreset" },
    { "-p", "poweron" },
    { "-s", "show" },
    { "-u", "unmerge" },
};

long_to_short_t long_to_short_clock[] = {
    { "-h", "?" },
    { "-h", "help" },
    { "-k", "loadifset" },
    { "-l", "load" },
    { "-s", "save" },
    { "-n", "saveifnotset" },
    { "-S", "show" },
};

long_to_short_t long_to_short_erase[] = {
    { "-a", "addr" },
    { "-b", "bank" },
    { "-d", "debug" },
    { "-h", "?" },
    { "-h", "help" },
    { "-l", "len" },
    { "-l", "length" },
    { "-y", "yes" },
};

long_to_short_t long_to_short_readwrite[] = {
    { "-a", "addr" },
    { "-b", "bank" },
    { "-D", "debug" },
    { "-d", "dump" },
    { "-f", "file" },
    { "-h", "?" },
    { "-h", "help" },
    { "-l", "len" },
    { "-l", "length" },
    { "-s", "swap" },
    { "-v", "verify" },
    { "-y", "yes" },
};

BOOL __check_abort_enabled = 0;       // Disable gcc clib2 ^C break handling
uint flag_debug = 0;
uint flag_quiet = 0;
uint8_t *test_loopback_buf = NULL;

/*
 * is_user_abort
 * -------------
 * Check for user break input (^C)
 */
static BOOL
is_user_abort(void)
{
    if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
        return (1);
    return (0);
}

static void
usage(void)
{
    printf("%s\n\n%s", version + 7, cmd_options);
}

const char *
long_to_short(const char *ptr, long_to_short_t *ltos, uint ltos_count)
{
    uint cur;

    for (cur = 0; cur < ltos_count; cur++)
        if (strcmp(ptr, ltos[cur].long_name) == 0)
            return (ltos[cur].short_name);
    return (ptr);
}

/*
 * rand32
 * ------
 * Very simple pseudo-random number generator
 */
static uint32_t rand_seed = 0;
static uint32_t
rand32(void)
{
    rand_seed = (rand_seed * 25173) + 13849;
    return (rand_seed);
}

/*
 * srand32
 * -------
 * Very simple random number seed
 */
static void
srand32(uint32_t seed)
{
    rand_seed = seed;
}

void
local_memcpy(void *dst, void *src, size_t len)
{
    uint32_t *dst32 = dst;
    uint32_t *src32 = src;

    if ((uintptr_t) dst | (uintptr_t) src | len) {
        /* fast mode */
        uint xlen = len >> 2;
        len -= (xlen << 2);
        while (xlen > 0) {
            *(dst32++) = *(src32++);
            xlen--;
        }
    }
    if (len > 0) {
        uint8_t *dst8 = (uint8_t *) dst32;
        uint8_t *src8 = (uint8_t *) src32;
        while (len > 0) {
            *(dst8++) = *(src8++);
            len--;
        }
    }
}

static void
print_us_diff(uint64_t start, uint64_t end)
{
    uint64_t diff = end - start;
    uint32_t diff2;
    char *scale = "ms";

    if ((diff >> 32) != 0)  // Finished before started?
        diff = 0;
    if (diff >= 100000) {
        diff /= 1000;
        scale = "sec";
    }
    diff2 = diff / 10;
    printf("%u.%02u %s\n", diff2 / 100, diff2 % 100, scale);
}

static void
show_test_state(const char * const name, int state)
{
    if (state == 0) {
        if (!flag_quiet)
            printf("PASS\n");
        return;
    }

    if (!flag_quiet || (state != -1))
        printf("  %-15s ", name);

    if (state == -1) {
        fflush(stdout);
        return;
    }
    printf("FAIL\n");
}

typedef struct {
    uint16_t cv_id;       // Vendor code
    char     cv_vend[12]; // Vendor string
} chip_vendors_t;

static const chip_vendors_t chip_vendors[] = {
    { 0x0001, "AMD" },      // AMD, Alliance, ST, Micron, others
    { 0x0004, "Fujitsu" },
    { 0x0020, "ST" },
    { 0x00c2, "Macronix" }, // MXIC
    { 0x0000, "Unknown" },  // Must remain last
};

typedef struct {
    uint32_t ci_id;       // Vendor code
    char     ci_dev[16];  // ID string for display
} chip_ids_t;
static const chip_ids_t chip_ids[] = {
    { 0x000122D2, "M29F160FT" },   // AMD+others 2MB top boot
    { 0x000122D8, "M29F160FB" },   // AMD+others 2MB bottom boot
    { 0x000122D6, "M29F800FT" },   // AMD+others 1MB top boot
    { 0x00012258, "M29F800FB" },   // AMD+others 1MB bottom boot
    { 0x00012223, "M29F400FT" },   // AMD+others 512K top boot
    { 0x000122ab, "M29F400FB" },   // AMD+others 512K bottom boot
    { 0x000422d2, "M29F160TE" },   // Fujitsu 2MB top boot
//  { 0x002022cc, "M29F160BT" },   // ST-Micro 2MB top boot
//  { 0x0020224b, "M29F160BB" },   // ST-Micro 2MB bottom boot
    { 0x00c222D6, "MX29F800CT" },  // Macronix 2MB top boot
    { 0x00c22258, "MX29F800CB" },  // Macronix 2MB bottom boot

    { 0x00000000, "Unknown" },     // Must remain last
};

typedef struct {
    uint16_t cb_chipid;   // Chip id code
    uint8_t  cb_bbnum;    // Boot block number (0=Bottom boot)
    uint8_t  cb_bsize;    // Common block size in Kwords (typical 32K)
    uint8_t  cb_ssize;    // Boot block sector size in Kwords (typical 4K)
    uint8_t  cb_map;      // Boot block sector erase map
} chip_blocks_t;

static const chip_blocks_t chip_blocks[] = {
    { 0x22D2, 31, 32, 4, 0x71 },  // 01110001 8K 4K 4K 16K (top)
    { 0x22D8,  0, 32, 4, 0x1d },  // 00011101 16K 4K 4K 8K (bottom)
    { 0x22D6, 15, 32, 4, 0x71 },  // 01110001 8K 4K 4K 16K (top)
    { 0x2258,  0, 32, 4, 0x1d },  // 00011101 16K 4K 4K 8K (bottom)
//  { 0x22CC, 31, 32, 4, 0x71 },  // 01110001 8K 4K 4K 16K (top)
//  { 0x224B,  0, 32, 4, 0x1d },  // 00011101 16K 4K 4K 8K (bottom)
    { 0x0000,  0, 32, 4, 0x1d },  // Default to bottom boot
};

static const chip_blocks_t *
get_chip_block_info(uint32_t chipid)
{
    uint16_t cid = (uint16_t) chipid;
    uint pos;

    /* Search for exact match */
    for (pos = 0; pos < ARRAY_SIZE(chip_blocks) - 1; pos++)
        if (chip_blocks[pos].cb_chipid == cid)
            break;

    return (&chip_blocks[pos]);
}

const char *
ee_vendor_string(uint32_t id)
{
    uint16_t vid = id >> 16;
    uint     pos;

    for (pos = 0; pos < ARRAY_SIZE(chip_vendors) - 1; pos++)
        if (chip_vendors[pos].cv_id == vid)
            break;
    return (chip_vendors[pos].cv_vend);
}

const char *
ee_id_string(uint32_t id)
{
    uint pos;

    for (pos = 0; pos < ARRAY_SIZE(chip_ids) - 1; pos++)
        if (chip_ids[pos].ci_id == id)
            break;

    if (pos == ARRAY_SIZE(chip_ids)) {
        uint16_t cid = id & 0xffff;
        for (pos = 0; pos < ARRAY_SIZE(chip_ids) - 1; pos++)
            if ((chip_ids[pos].ci_id & 0xffff) == cid)
                break;
    }
    return (chip_ids[pos].ci_dev);
}

static void __attribute__ ((noinline))
spin(uint loops)
{
    uint count;
    for (count = 0; count < loops; count++) {
        __asm__ __volatile__("nop");
    }
}

static void
spin_memory(uint32_t addr)
{
    uint     count;

    SUPERVISOR_STATE_ENTER();
    INTERRUPTS_DISABLE();
    CACHE_DISABLE_DATA();
    MMU_DISABLE();

#if 0
    CacheClearE((void *)addr, MEM_LOOPS * 4, CACRF_ClearD);
#endif

    for (count = 0; count < MEM_LOOPS; count += 4) {
        (void) *ADDR32(addr);
    }

    spin(MEM_LOOPS);

    MMU_RESTORE();
    CACHE_RESTORE_STATE();
    INTERRUPTS_ENABLE();
    SUPERVISOR_STATE_EXIT();
    printf("done\n");
}

static void
spin_memory_ovl(uint32_t addr)
{
    uint     count;

    SUPERVISOR_STATE_ENTER();
    INTERRUPTS_DISABLE();
    CACHE_DISABLE_DATA();
    MMU_DISABLE();

    *CIAA_PRA |= CIAA_PRA_OVERLAY | CIAA_PRA_LED;
    for (count = 0; count < MEM_LOOPS; count++) {
        (void) *ADDR32(addr);
    }
    *CIAA_PRA &= ~(CIAA_PRA_OVERLAY | CIAA_PRA_LED);

    spin(MEM_LOOPS);

    MMU_RESTORE();
    CACHE_RESTORE_STATE();
    INTERRUPTS_ENABLE();
    SUPERVISOR_STATE_EXIT();
    printf("done\n");
}


#if 0
static uint
smash_nop(void)
{
    return (send_cmd(KS_CMD_NOP, NULL, 0, NULL, 0, NULL));
}
#endif

static uint64_t
smash_time(void)
{
    uint64_t usecs;
    if (send_cmd(KS_CMD_UPTIME, NULL, 0, &usecs, sizeof (usecs), NULL))
        return (0);
    return (usecs);
}

static uint
smash_identify(void)
{
    uint64_t   usecs;
    uint       sec;
    uint       usec;
    smash_id_t id;
    uint       rlen;
    uint       rc;

    memset(&id, 0, sizeof (id));
    rc = send_cmd(KS_CMD_ID, NULL, 0, &id, sizeof (id), &rlen);

    if (rc != 0) {
        printf("Reply message failure: %d (%s)\n", (int) rc, smash_err(rc));
        if (flag_debug)
            dump_memory(&id, rlen, DUMP_VALUE_UNASSIGNED);
        return (rc);
    }
    if (flag_quiet == 0) {
        printf("ID\n");
        printf("  Kicksmash %u.%u built %02u%02u-%02u-%02u %02u:%02u:%02u\n",
               id.si_ks_version[0], id.si_ks_version[1],
               id.si_ks_date[0], id.si_ks_date[1],
               id.si_ks_date[2], id.si_ks_date[3],
               id.si_ks_time[0], id.si_ks_time[1], id.si_ks_time[2]);
        printf("  USB %08x  Serial \"%s\"  Name \"%s\"\n",
               id.si_usbid, id.si_serial, id.si_name);
        printf("  Mode: %s\n",
               (id.si_mode == 0) ? "32-bit" :
               (id.si_mode == 1) ? "16-bit" :
               (id.si_mode == 2) ? "16-bit high" : "unknown");
    }

    usecs = smash_time();
    if (flag_quiet == 0) {
        if (usecs != 0) {
            sec  = usecs / 1000000;
            usec = usecs % 1000000;
            printf("  Uptime: %u.%06u sec\n", sec, usec);
        }
    }

    return (0);
}

static const uint32_t test_pattern[] = {
    0x54455354, 0x50415454, 0x202d2053, 0x54415254,
    0x5555aaaa, 0x3333cccc, 0x1111eeee, 0x99996666,
    0x01000200, 0x04000800, 0x10002000, 0x40008000,
    0x00010002, 0x00040008, 0x00100020, 0x00400080,
    0xfefffdff, 0xfbfff7ff, 0xefffdfff, 0xbfff7fff,
    0xfffefffd, 0xfffbfff7, 0xffefffdf, 0xffbfff7f,
    0x54455354, 0x50415454, 0x20454e44, 0x20636468,
};

static int
smash_test_pattern(void)
{
    uint32_t reply_buf[64];
    uint start;
    uint pos;
    uint err_count = 0;
    uint rlen;
    uint rc;

    show_test_state("Test pattern", -1);

    memset(reply_buf, 0, sizeof (reply_buf));
#ifdef SEND_TESTPATT_ALT
    reply_buf[0] = 0xa5a5a5a5;
    rc = send_cmd(KS_CMD_TESTPATT, reply_buf, 2, reply_buf,
                  sizeof (reply_buf), &rlen);
#else
    rc = send_cmd(KS_CMD_TESTPATT, NULL, 0, reply_buf,
                  sizeof (reply_buf), &rlen);
#endif
    if (rc != 0) {
        printf("Reply message failure: %d (%s)\n", (int) rc, smash_err(rc));
        if (flag_debug)
            dump_memory(reply_buf, rlen, DUMP_VALUE_UNASSIGNED);
        show_test_state("Test pattern", rc);
        return (rc);
    }

    for (start = 0; start < ARRAY_SIZE(reply_buf); start++)
        if (reply_buf[start] == test_pattern[0])  // Found first data
            break;

    if (start == ARRAY_SIZE(reply_buf)) {
        printf("No test pattern marker [%08x] in reply\n", test_pattern[0]);
fail:
        dump_memory(reply_buf, sizeof (reply_buf), DUMP_VALUE_UNASSIGNED);
        show_test_state("Test pattern", 1);
        return (1);
    }

    if (start != 0) {
        printf("Pattern start 0x%x is not at beginning of buffer\n",
               start);
        err_count++;
    }
    for (pos = 0; pos < ARRAY_SIZE(test_pattern); pos++) {
        if (reply_buf[start + pos] != test_pattern[pos]) {
            printf("At pos=%x reply %08x != expected %08x (diff %08x)\n", pos,
                   reply_buf[start + pos], test_pattern[pos],
                   reply_buf[start + pos] ^ test_pattern[pos]);
            if (err_count > 6)
                goto fail;
        }
    }
    if (err_count > 6)
        goto fail;
    if (flag_debug > 1)
        dump_memory(reply_buf, sizeof (reply_buf), DUMP_VALUE_UNASSIGNED);
    show_test_state("Test pattern", 0);
    return (0);
}

static int
smash_test_loopback(void)
{
    uint8_t   *tx_buf;
    uint8_t   *rx_buf;
    uint       cur;
    uint       rc;
    uint       count;
    uint       rlen = 0;
    uint       nums = (rand32() % (TEST_LOOPBACK_MAX - 1)) + 1;
    uint64_t   time_start;
    uint64_t   time_end;

    show_test_state("Test loopback", -1);

    tx_buf = &test_loopback_buf[0];
    rx_buf = &test_loopback_buf[TEST_LOOPBACK_BUF];
    memset(rx_buf, 0, TEST_LOOPBACK_MAX * 4);
    for (cur = 0; cur < nums; cur++)
        tx_buf[cur] = (uint8_t) (rand32() >> 8);

    /* Measure IOPS */
    INTERRUPTS_DISABLE();
    time_start = smash_time();
    for (count = 0; count < 10; count++) {
        rc = send_cmd(KS_CMD_LOOPBACK, tx_buf, 4,
                      rx_buf, TEST_LOOPBACK_MAX, &rlen);
        if ((rc != KS_CMD_LOOPBACK))
            break;
    }
    time_end = smash_time();
    INTERRUPTS_ENABLE();

    if (rc == KS_CMD_LOOPBACK) {
        /* Test loopback accuracy */
        rc = send_cmd(KS_CMD_LOOPBACK, tx_buf, nums,
                      rx_buf, TEST_LOOPBACK_MAX, &rlen);
    }
    if (rc == KS_CMD_LOOPBACK) {
        rc = 0;
    } else {
        printf("FAIL: %d (%s)\n", rc, smash_err(rc));
fail_handle_debug:
        if (flag_debug) {
            uint pos;
            uint8_t *txp = (uint8_t *)tx_buf;
            uint8_t *rxp = (uint8_t *)rx_buf;
            for (pos = 0; pos < nums; pos++)
                if (rxp[pos] != txp[pos])
                    break;
            dump_memory(tx_buf, nums, DUMP_VALUE_UNASSIGNED);
            if (pos < nums) {
                printf("--- Tx above  Rx below --- ");
                printf("First diff at 0x%x of 0x%x\n", pos, nums);
                dump_memory(rx_buf, rlen, DUMP_VALUE_UNASSIGNED);
            } else {
                printf("Tx and Rx buffers (len=0x%x) match\n", nums);
            }
        }
        goto fail_loopback;
    }
    if (rlen != nums) {
        printf("FAIL: rlen=%u != sent %u\n", rlen, nums);
        rc = MSG_STATUS_BAD_LENGTH;
        goto fail_handle_debug;
    }
    for (cur = 0; cur < nums; cur++) {
        if (rx_buf[cur] != tx_buf[cur]) {
            if (rc++ == 0)
                printf("\nLoopback data miscompare\n");
            if (rc < 5) {
                printf("    [%02x] %02x != expected %02x\n",
                       cur, rx_buf[cur], tx_buf[cur]);
            }
        }
    }
    if (rc >= 4)
        printf("%u miscompares\n", rc);
    if ((rc == 0) && (flag_quiet == 0)) {
        uint diff = (uint) (time_end - time_start);
        if (diff == 0)
            diff = 1;
        printf("PASS  %u IOPS\n", 1000000 * count / diff);
        return (0);
    }
fail_loopback:
    show_test_state("Test loopback", rc);
    return (rc);
}

static int
smash_test_loopback_perf(void)
{
    const uint lb_size  = 1000;
    const uint xfers    = 100;
    const uint lb_alloc = lb_size + KS_HDR_AND_CRC_LEN;  // Magic+len+cmd+CRC
    uint32_t  *buf;
    uint       cur;
    uint       rc;
    uint       diff;
    uint       total;
    uint       perf;
    uint64_t   time_start;
    uint64_t   time_end;

    show_test_state("Loopback perf", -1);

    buf = AllocMem(lb_alloc, MEMF_PUBLIC);
    if (buf == NULL) {
        printf("Memory allocation failure\n");
        return (1);
    }
    memset(buf, 0xa5, lb_size);

    time_start = smash_time();

    for (cur = 0; cur < xfers; cur++) {
        rc = send_cmd(KS_CMD_LOOPBACK, buf, lb_size, buf, lb_size, NULL);
        if (rc == KS_CMD_LOOPBACK) {
            rc = 0;
        } else {
            printf("FAIL: %d (%s)\n", rc, smash_err(rc));
            if (flag_debug) {
                dump_memory(buf, lb_size, DUMP_VALUE_UNASSIGNED);
            }
            goto cleanup;
        }
    }

    if (flag_quiet == 0) {
        time_end = smash_time();
        diff = (uint) (time_end - time_start);
        if (diff == 0)
            diff = 1;
        total = xfers * (lb_size + KS_HDR_AND_CRC_LEN);
        perf = total * 1000 / diff;
        perf *= 2;  // Write data + Read (reply) data
        printf("PASS  %u KB/sec\n", perf);
    }

cleanup:
    if (buf != NULL)
        FreeMem(buf, lb_alloc);
    return (rc);
}

#define DELAY_MS_PER_TICK (1000 / 50)  // 50 ticks per second = 20 ms/tick

static uint
recv_msg(void *buf, uint len, uint *rlen, uint timeout_ms)
{
    uint rc;
    rc = send_cmd(KS_CMD_MSG_RECEIVE, NULL, 0, buf, len, rlen);
    while (rc == KS_STATUS_NODATA) {
        Delay(1);
        rc = send_cmd(KS_CMD_MSG_RECEIVE, NULL, 0, buf, len, rlen);
        if (timeout_ms > DELAY_MS_PER_TICK)
            timeout_ms -= DELAY_MS_PER_TICK;
        else
            break;
    }
    if (rc == KS_CMD_MSG_SEND)
        rc = 0;
    if (rc != 0) {
        printf("Get message failed: %d (%s)\n", rc, smash_err(rc));
        if (flag_debug)
            dump_memory(buf, 0x40, DUMP_VALUE_UNASSIGNED);
    }
    return (rc);
}

static uint
recv_msg_loopback(void *buf, uint len, uint *rlen, uint which_buf)
{
    uint rc;
    uint cmd = KS_CMD_MSG_RECEIVE;

    if (which_buf == 0)
        cmd |= KS_MSG_ALTBUF;

    rc = send_cmd(cmd, NULL, 0, buf, len, rlen);
    if ((rc == KS_CMD_MSG_SEND) || (rc == (KS_CMD_MSG_SEND | KS_MSG_ALTBUF)))
        rc = 0;
    if (rc != 0) {
        printf("Get message failed: %d (%s)\n", rc, smash_err(rc));
        if (flag_debug)
            dump_memory(buf, 0x40, DUMP_VALUE_UNASSIGNED);
    }
    return (rc);
}

static uint
send_msg_loopback(void *buf, uint len, uint which_buf)
{
    uint rc;
    uint cmd = KS_CMD_MSG_SEND;
    uint32_t rbuf[16];

    if (which_buf != 0)
        cmd |= KS_MSG_ALTBUF;

    rc = send_cmd(cmd, buf, len, rbuf, sizeof (rbuf), NULL);
    if (rc != 0) {
        printf("Send message buf%s l=%u failed: %d (%s)\n",
               which_buf ? " alt" : "", len, rc, smash_err(rc));
        if (flag_debug)
            dump_memory(rbuf, sizeof (rbuf), DUMP_VALUE_UNASSIGNED);
    }
    return (rc);
}

static uint
get_msg_info(smash_msg_info_t *msginfo)
{
    uint rc = send_cmd(KS_CMD_MSG_INFO, NULL, 0,
                       msginfo, sizeof (*msginfo), NULL);
    if (rc != 0)
        printf("Get message info failed: %d (%s)\n", rc, smash_err(rc));
    return (rc);
}

static uint
calc_kb_sec(uint usecs, uint bytes)
{
    if (usecs == 0)
        usecs = 1;
    while (bytes > 4000000) {
        bytes >>= 1;
        usecs >>= 1;
    }
    return (bytes * 1000 / usecs);
}

static int
smash_test_msg_loopback(void)
{
    smash_msg_info_t omsginfo;
    smash_msg_info_t msginfo;
    uint8_t  *buf;
    uint      rc;
    uint      rc2;
    uint      rlen;
    uint      sent;
    uint      pos;
    uint      count = 0;
    uint      pass;
    uint16_t  lockbits;
    uint32_t  rseed[2];
    uint      scount[2];
    uint64_t  time_start;
    uint64_t  time_end;
    uint      count_w1 = 0;
    uint      count_w2 = 0;
    uint      count_r  = 0;
    uint      time_w[2];
    uint      time_r[2];

    show_test_state("Message buffer", -1);

    buf = AllocMem(MAX_CHUNK, MEMF_PUBLIC);
    if (buf == NULL) {
        printf("Memory allocation failure\n");
        return (1);
    }

    /* Lock message buffers */
    lockbits = BIT(0) | BIT(1);
    rc = send_cmd(KS_CMD_MSG_LOCK, &lockbits, sizeof (lockbits), NULL, 0, NULL);
    if (rc != 0) {
        printf("Message lock failed: %d (%s)\n", rc, smash_err(rc));
        goto fail;
    }

#define MAX_MESSAGES 150
    if ((rc = get_msg_info(&omsginfo)) != 0)
        goto fail;
    if ((omsginfo.smi_atou_inuse != 0) || (omsginfo.smi_utoa_inuse != 0)) {
        printf("Clearing atou=%u and utoa=%u bytes\n",
                omsginfo.smi_atou_inuse, omsginfo.smi_utoa_inuse);

        /* Discard old messages */
        if ((rc = send_cmd(KS_CMD_MSG_FLUSH, NULL, 0, NULL, 0, NULL)) != 0) {
            printf("Msg flush failed: %d (%s)\n", rc, smash_err(rc));
            goto fail;
        }
        rc = send_cmd(KS_CMD_MSG_FLUSH | KS_MSG_ALTBUF, NULL, 0, NULL, 0, NULL);
        if (rc != 0) {
            printf("Msg flush failed: %d (%s)\n", rc, smash_err(rc));
            goto fail;
        }
    }

    for (count = 0; count < MAX_CHUNK; count++)
        buf[count] = count;

    /* Buffers are empty at this point; fill both */
    for (pass = 0; pass < 2; pass++) {
        uint avail;
        uint inuse;

        if ((rc = get_msg_info(&msginfo)) != 0)
            goto fail;
        if (pass == 0)
            avail = msginfo.smi_atou_avail;
        else
            avail = msginfo.smi_utoa_avail;
        rseed[pass] = rand_seed;
        sent = 0;

        for (count = 0; count < MAX_MESSAGES; count++) {
            uint len = (rand32() & 0x1f) + 0x20;
            if (avail < 2)
                break;
            if (len > avail)
                break;  // This will later force a wrap

            if ((rc = send_msg_loopback(buf, len, pass)) != 0)
                goto fail;
            count_w1 += len + KS_HDR_AND_CRC_LEN;
            len = (len + 1) & ~1;   // round up for buffer use
            sent += len + KS_HDR_AND_CRC_LEN;
            if ((rc = get_msg_info(&msginfo)) != 0)
                goto fail;
            if (pass == 0) {
                avail = msginfo.smi_atou_avail;
                inuse = msginfo.smi_atou_inuse;
            } else {
                avail = msginfo.smi_utoa_avail;
                inuse = msginfo.smi_utoa_inuse;
            }

            if (inuse != sent) {
                printf("FAIL: Sent %u to buf%u, but atou=%u utoa=%u\n", sent,
                       pass, msginfo.smi_atou_inuse, msginfo.smi_utoa_inuse);
                rc = MSG_STATUS_BAD_LENGTH;
                goto fail;
            }
        }
        scount[pass] = count;
    }

    if ((rc = get_msg_info(&msginfo)) != 0)
        goto fail;
    if ((msginfo.smi_atou_inuse < msginfo.smi_atou_avail) ||
        (msginfo.smi_utoa_inuse < msginfo.smi_utoa_avail)) {
        printf("Fail: message buffers should be almost full at this point\n"
               "  atou_inuse=%u atou_avail=%u utoa_inuse=%u utoa_avail=%u\n",
               msginfo.smi_atou_inuse, msginfo.smi_atou_avail,
               msginfo.smi_utoa_inuse, msginfo.smi_utoa_avail);
        rc = 1;
        goto fail;
    }

    /* Extract messages from buffers and verify contents */
    for (pass = 0; pass < 2; pass++) {
        uint rlen;
        srand32(rseed[pass]);
        for (count = 0; count < MAX_MESSAGES * 2; count++) {
            uint inuse;
            uint len;

            if ((rc = recv_msg_loopback(buf, MAX_CHUNK, &rlen, pass)) != 0)
                goto fail;
            count_r += rlen + KS_HDR_AND_CRC_LEN;
            for (pos = 0; pos < rlen; pos++) {
                if (buf[pos] != (uint8_t)pos) {
                    printf("Data corrupt at %x of %x: %02x != expected %02x\n",
                           pos, rlen, buf[pos], (uint8_t)pos);
                    buf[pos] = pos;
                    break;
                }
            }
            if (pos < rlen) {
                rc = MSG_STATUS_FAIL;
                goto fail;
            }
            if ((rc = get_msg_info(&msginfo)) != 0)
                goto fail;
            if (pass == 0)
                inuse = msginfo.smi_atou_inuse;
            else
                inuse = msginfo.smi_utoa_inuse;
            if (inuse == 0)
                break;
#define BIG_WRITE_LEN 0x108
            if (count < scount[pass])
                len = (rand32() & 0x1f) + 0x20;
            else
                len = BIG_WRITE_LEN;
            if ((rlen != len) && (count != scount[pass])) {
                printf("Receive length %u != expected %u at %u of %s %u\n",
                       rlen, len, count, pass ? "utoa" : "atou", scount[pass]);
                rc = MSG_STATUS_BAD_LENGTH;
                goto fail;
            }
            if ((count > scount[pass] - 4) && (count < MAX_MESSAGES)) {
                if ((rc = send_msg_loopback(buf, BIG_WRITE_LEN, pass)) != 0) {
                    printf("fail at %u\n", count);
                    goto fail;
                }
                count_w2 += BIG_WRITE_LEN + KS_HDR_AND_CRC_LEN;
            }
        }
    }
    if ((rc = get_msg_info(&msginfo)) != 0)
        goto fail;

    if ((msginfo.smi_atou_inuse != 0) || (msginfo.smi_utoa_inuse != 0)) {
        printf("Fail: message buffers should be empty at this point\n"
               "atou_inuse=%u atou_avail=%u utoa_inuse=%u utoa_avail=%u\n",
               msginfo.smi_atou_inuse, msginfo.smi_atou_avail,
               msginfo.smi_utoa_inuse, msginfo.smi_utoa_avail);
        rc = 1;
        goto fail;
    }

    if (count_r != count_w1 + count_w2) {
        printf("Count of read bytes %u != write bytes %u\n",
               count_r, count_w1 + count_w2);
        rc = 1;
        goto fail;
    }

    /* Measure write performance */
    time_start = smash_time();
    for (pos = 0; pos < 2; pos++) {
        for (pass = 0; pass < 2; pass++) {
            srand(rseed[pass]);
            for (count = 0; count < 10; count++) {
                uint len;
                if (pos == 0)
                    len = (rand32() & 0x1f) + 0x20;
                else
                    len = 0x100;
                if ((rc = send_msg_loopback(buf, len, pass)) != 0)
                    goto fail;
            }
        }
        time_end    = smash_time();
        time_w[pos] = time_end - time_start;
        time_start  = time_end;
    }

    /* Measure read performance */
    for (pos = 0; pos < 2; pos++) {
        for (pass = 0; pass < 2; pass++) {
            for (count = 0; count < 10; count++) {
                if ((rc = recv_msg_loopback(buf, MAX_CHUNK, &rlen, pass)) != 0)
                    goto fail;
            }
        }
        time_end    = smash_time();
        time_r[pos] = time_end - time_start;
        time_start  = time_end;
    }

fail:
    /* Unlock message buffers */
    rc2 = send_cmd(KS_CMD_MSG_LOCK | KS_MSG_UNLOCK, &lockbits,
                   sizeof (lockbits), NULL, 0, NULL);
    if (rc2 != 0) {
        printf("Message unlock failed: %d (%s)\n", rc2, smash_err(rc2));
        if (rc == 0)
            rc = rc2;
    }

    FreeMem(buf, MAX_CHUNK);

    if (rc == 0) {
        if (flag_quiet == 0) {
            printf("PASS  %u-%u KB/sec (W)  %u-%u KB/sec (R)\n",
                   calc_kb_sec(time_w[0], 2 * 10 * 0x30),
                   calc_kb_sec(time_w[1], 2 * 10 * 0x100),
                   calc_kb_sec(time_r[0], 2 * 10 * 0x30),
                   calc_kb_sec(time_r[1], 2 * 10 * 0x100));
            return (0);
        }
    } else {
        show_test_state("Message buffer", rc);
    }
    return (rc);
}

static int
smash_test_usb_msg_loopback(void)
{
    km_msg_hdr_t *skm;
    km_msg_hdr_t *rkm;
    smash_msg_info_t msginfo;
    uint8_t  *buf[2];
    uint      pos;
    uint      rc;
    uint      maxlen = 2000;
    uint      len = 32;
    uint      rlen;
    uint      count;
    uint      icount;
    uint      total;
    uint      perf;
    uint      diff;
    uint      errs = 0;
    uint64_t  time_start;
    uint64_t  time_end;
    static uint8_t worked_before = 0;

    show_test_state("Remote message", -1);

    if ((rc = get_msg_info(&msginfo)) != 0) {
        printf("Get msg info failed: %d (%s)\n", rc, smash_err(rc));
        return (rc);
    }

    if ((msginfo.smi_state_usb &
         (MSG_STATE_SERVICE_UP | MSG_STATE_HAVE_LOOPBACK)) !=
         (MSG_STATE_SERVICE_UP | MSG_STATE_HAVE_LOOPBACK)) {
        printf("Unavailable\n");
        if (worked_before)
            return (1);  // Then this is a failure
        else
            return (0);  // Was probably never available
    }

    buf[0] = NULL;
    buf[1] = NULL;
    for (pos = 0; pos < ARRAY_SIZE(buf); pos++) {
        buf[pos] = AllocMem(MAX_CHUNK, MEMF_PUBLIC);
        if (buf[pos] == NULL) {
            printf("Memory allocation failure\n");
            goto fail;
        }
    }

    for (pos = 0; pos < maxlen; pos++) {
        buf[0][pos] = pos;
        buf[1][pos] = ~pos;
    }

    rkm = (km_msg_hdr_t *)buf[1];
    skm = (km_msg_hdr_t *)buf[0];
    skm->km_op = KM_OP_LOOPBACK;
    skm->km_status = 0xff;
    skm->km_tag = 0;

    /* Discard old messages */
    if ((rc = send_cmd(KS_CMD_MSG_FLUSH, NULL, 0, NULL, 0, NULL)) != 0) {
        printf("Msg flush failed: %d (%s)\n", rc, smash_err(rc));
        goto fail;
    }

    for (count = 0; count < 10; count++) {
        len = (rand() & (maxlen / 2 - 1)) + sizeof (*skm);
        skm->km_tag = count;
        if ((rc = send_msg_loopback(buf[0], len, 0)) != 0)
            goto fail;
        if ((rc = recv_msg(buf[1], MAX_CHUNK, &rlen, 1000)) != 0) {
            printf("  Expected message len=%u\n", len);
            if (flag_debug) {
                for (pos = 0; pos < len; pos++)
                    printf(" %02x", buf[0][pos]);
                printf("\n");
            }
            goto fail;
        }
        if (rlen != len) {
            printf("Receive length %u != expected %u\n", rlen, len);
            rc = MSG_STATUS_BAD_DATA;
            break;
        }
        if (rkm->km_op != (KM_OP_LOOPBACK | KM_OP_REPLY)) {
            printf("Receive message op %02x != expected %02x\n",
                   rkm->km_op, KM_OP_LOOPBACK | KM_OP_REPLY);
            rc = MSG_STATUS_BAD_DATA;
            break;
        }
        if (rkm->km_tag != count) {
            printf("Receive message tag %02x != expected %02x\n",
                   rkm->km_tag, count);
            rc = MSG_STATUS_BAD_DATA;
            break;
        }
#if 0
        printf("rlen=%u op=%x status=%x seq=%x\n",
               rlen, rkm->km_op, rkm->km_status, rkm->km_tag);
#endif
        for (pos = sizeof (*rkm); pos < len; pos++) {
            if (buf[1][pos] != buf[0][pos]) {
                if (errs++ < 5) {
                    if (errs == 1)
                        printf("Miscompare\n");
                    printf("  Pass %u pos=0x%x %02x != expected %02x\n",
                           count, pos, buf[1][pos], buf[0][pos]);
                }
            }
        }
        if (errs != 0) {
            rc = MSG_STATUS_BAD_DATA;
            break;
        }
    }
    if (rc != 0)
        goto fail;

    len = maxlen;
    time_start = smash_time();
    for (count = 0; count < 10; count++) {
        skm->km_tag = count;
        for (icount = 0; icount < 1; icount++) {
            if ((rc = send_msg_loopback(buf[0], len, 0)) != 0)
                goto fail;
        }
        for (icount = 0; icount < 1; icount++) {
            if ((rc = recv_msg(buf[1], MAX_CHUNK, &rlen, 1000)) != 0) {
                if (flag_debug) {
                    printf("Expected message len=%u\n", len);
                    for (pos = 0; pos < len; pos++)
                        printf(" %02x", buf[0][pos]);
                    printf("\n");
                }
                goto fail;
            }
            if (rlen != len) {
                printf("Receive length %u != expected %u\n", rlen, len);
                rc = MSG_STATUS_BAD_DATA;
                break;
            }
            if (rkm->km_op != (KM_OP_LOOPBACK | KM_OP_REPLY)) {
                printf("Receive message op %02x != expected %02x\n",
                       rkm->km_op, KM_OP_LOOPBACK | KM_OP_REPLY);
                rc = MSG_STATUS_BAD_DATA;
                break;
            }
            if (rkm->km_tag != count) {
                printf("Receive message tag %02x != expected %02x\n",
                       rkm->km_tag, count);
                rc = MSG_STATUS_BAD_DATA;
                break;
            }
        }
    }
    if (flag_quiet == 0) {
        time_end = smash_time();
        diff = (uint) (time_end - time_start);
        if (diff == 0)
            diff = 1;
        total = count * (maxlen + KS_HDR_AND_CRC_LEN);
        perf = total * 1000 / diff;
        perf *= 2 * icount;  // Write data + Read (reply) data
        printf("PASS  %u KB/sec\n", perf);
    }

fail:
    for (pos = 0; pos < ARRAY_SIZE(buf); pos++) {
        if (buf[pos] != NULL)
            FreeMem(buf[pos], MAX_CHUNK);
    }
    worked_before = 1;
    return (rc);
}


static int
smash_test(uint mask)
{
    int rc = 0;
    if (mask & BIT(0)) {
        rc = smash_test_pattern();
        if (rc != 0)
            return (rc);
    }

    if (is_user_abort())
        return (1);

    if (mask & BIT(1)) {
        rc = smash_test_loopback();
        if (rc != 0)
            return (rc);
    }

    if (is_user_abort())
        return (1);

    if (mask & BIT(2)) {
        rc = smash_test_loopback_perf();
        if (rc != 0)
            return (rc);
    }

    if (is_user_abort())
        return (1);

    if (mask & BIT(3)) {
        rc = smash_test_msg_loopback();
        if (rc != 0)
            return (rc);
    }

    if (is_user_abort())
        return (1);

    if (mask & BIT(4)) {
        rc = smash_test_usb_msg_loopback();
        if (rc != 0)
            return (rc);
    }
    return (0);
}

/*
 * flash_cmd_core
 * --------------
 * Must be called with interrupts and cache disabled
 *
 * This function will send a command to the flash after setting the
 * transaction up with the Kicksmash CPU.
 */
static int
flash_cmd_core(uint32_t cmd, void *arg, uint argsize)
{
    uint32_t addr;
    uint32_t addrs[64];
//  uint32_t data[64];
//  uint32_t val;
//  uint     retry = 0;
    uint     num_addr;
    uint     pos;
    int      rc;

    rc = send_cmd_core(cmd, arg, argsize, addrs, sizeof (addrs), &num_addr);
    if (rc != 0)
        goto flash_cmd_cleanup;

    /*
     * All Kicksmash flash commands have a similar sequence, where
     * the command is issued to Kicksmash, then Kicksmash will reply
     * with the sequence of addresses to blind read.
     * There is additional work for write and erase, where status must be
     * repeatedly read to poll flash state. It is left to the caller
     * to complete whatever additional polling is necessary and issue
     * the next command to Kicksmash.
     */
    num_addr /= 4;

    cia_spin(CIA_USEC(5));

    addr = ROM_BASE + ((addrs[0] << smash_cmd_shift) & 0x7ffff);
    (void) *ADDR32(addr);  // Generate OE strobe to kick off DMA
    cia_spin(1);

    for (pos = 0; pos < num_addr; pos++) {
        uint32_t addr = ROM_BASE + ((addrs[pos] << smash_cmd_shift) & 0x7ffff);
#if 1
        (void) *ADDR32(addr);  // Generate address on the bus
#else
        val = *ADDR32(addr);  // Generate address on the bus
        /* Hopefully never needed */
        if ((pos == 0) && (val == 0xffffffff) && (retry++ < 5)) {
            pos--;      /* Try again */
            continue;
        }
#endif
#if 0
        /* Debug this particular command sequence */
        if (cmd == KS_CMD_FLASH_ID) {
            if (pos < ARRAY_SIZE(data)) {
                *ADDR32(0x7780000 + pos * 4) = addr;
                *ADDR32(0x7780010 + pos * 4) = val;
                data[pos] = val;
            }
        }
#endif
    }

#if 0
    // below is for debug, to ensure all Kicksmash DMA is drained.
    for (pos = 0; pos < 10000; pos++) {
        (void) *ADDR32(ROM_BASE);
    }
#endif

flash_cmd_cleanup:
    if (rc == 0) {
#if 0
        if (flag_debug && !irq_disabled) {
            printf("Flash sequence\n");
            for (pos = 0; pos < num_addr; pos++)
                printf("    %08x = %08x\n", addrs[pos], data[pos]);
        }
#endif
    } else {
        /* Attempt to drain data and wait for Kicksmash to enable flash */
        for (pos = 0; pos < 1000; pos++) {
            (void) *ADDR32(ROM_BASE);
        }
        cia_spin(CIA_USEC_LONG(25000));
    }
    return (rc);
}

#if 0
static int
flash_cmd(uint32_t cmd, void *arg, uint argsize)
{
    int rc;
    SUPERVISOR_STATE_ENTER();
    INTERRUPTS_DISABLE();
    CACHE_DISABLE_DATA();
    MMU_DISABLE();

    rc = flash_cmd_core(cmd, arg, argsize);

    MMU_RESTORE();
    CACHE_RESTORE_STATE();
    INTERRUPTS_ENABLE();
    SUPERVISOR_STATE_EXIT();
    return (rc);
}
#endif

#if 0
static int
flash_read(void)
{
    return (flash_cmd(KS_CMD_FLASH_READ, NULL, 0));
}
#endif

static int
flash_id(uint32_t *dev1, uint32_t *dev2, uint *mode)
{
    uint32_t data[32];
    uint     pos;
    int      rc1;
    int      rc2;

    *dev1 = 0;
    *dev2 = 0;

    /*
     * XXX: This code needs to be revised for 16-bit machines so that
     *      it probes with both smash_cmd_shift = 2 and also = 1.
     *      An alternative is to just ask KS what mode it's running in
     *      with the KS_CMD_ID call.
     */
    SUPERVISOR_STATE_ENTER();
    INTERRUPTS_DISABLE();
    CACHE_DISABLE_DATA();
    MMU_DISABLE();

#ifdef TEST_CMD_FLASH_CMD
    uint32_t values[] = { 0x00555, 0x002aa, 0x00555,
                          0x00aa00aa, 0x00550055, 0x00900090 };
    rc1 = flash_cmd_core(KS_CMD_FLASH_CMD, values, sizeof (values));
#else
    rc1 = flash_cmd_core(KS_CMD_FLASH_ID, NULL, 0);
#endif
    if (rc1 == 0) {
        for (pos = 0; pos < ARRAY_SIZE(data); pos++) {
            data[pos] = *ADDR32(ROM_BASE + pos * 4);
        }
    }
    rc2 = flash_cmd_core(KS_CMD_FLASH_READ, NULL, 0);

    MMU_RESTORE();
    CACHE_RESTORE_STATE();
    INTERRUPTS_ENABLE();
    SUPERVISOR_STATE_EXIT();

    if (flag_debug || rc1 || rc2) {
        printf("rc1=%d (%s)  rc2=%d (%s)\n",
               rc1, smash_err(rc1), rc2, smash_err(rc2));
    }

    if (rc1 == 0) {
        /*
         * Validation for a recognized flash device is done by
         * get_chip_block_info(). The code just needs to determine
         * whether it's a 16-bit or 32-bit device.
         */
        uint flash_mode;
        uint16_t device;
        const chip_blocks_t *cb;

        if ((data[0x2] == 0x00000000) && (data[0x3] == 0x00000000)) {
            /* flash is 32-bit */
            flash_mode = 32;
            device = data[1] & 0xffff;
            cb = get_chip_block_info(device);
            if ((cb->cb_chipid == 0) ||  // Unknown chip
                (data[0x4] != data[0x0]) || (data[0x5] != data[0x1]) ||
                (data[0x8] != data[0x0]) || (data[0x9] != data[0x1]) ||
                (data[0xc] != data[0x0]) || (data[0xd] != data[0x1])) {
                rc1 = 1;
            }
        } else {
            /* flash is 16-bit */
            flash_mode = 16;
            cb = get_chip_block_info(device);
            device = data[0] & 0xffff;
            cb = get_chip_block_info(device);
            if ((cb->cb_chipid == 0) ||  // Unknown chip
                (data[0x2] != data[0x0]) || (data[0x3] != data[0x1]) ||
                (data[0x4] != data[0x0]) || (data[0x5] != data[0x1]) ||
                (data[0x6] != data[0x0]) || (data[0x7] != data[0x1])) {
                rc1 = 1;
            }
        }
        if (mode != NULL)
            *mode = flash_mode;

        if (flash_mode == 16)
            smash_cmd_shift = 1;
        else
            smash_cmd_shift = 2;

        if (flag_debug) {
            printf("Flash ID: %svalid\n", rc1 ? "in" : "");
            if (flag_debug > 1)
                dump_memory(data, sizeof (data) / 2, DUMP_VALUE_UNASSIGNED);
        }
    }
    if (rc1 == 0) {
        *dev1 = (data[0] << 16) | ((uint16_t) data[1]);
        *dev2 = (data[0] & 0xffff0000) | (data[1] >> 16);
    }

    return (rc1 ? rc1 : rc2);
}

static int
flash_show_id(void)
{
    int      rc;
    uint     mode;
    uint32_t flash_dev1;
    uint32_t flash_dev2;
    const char *id1;
    const char *id2;

    rc = flash_id(&flash_dev1, &flash_dev2, &mode);
    if (rc != 0) {
        printf("Flash id failure %d (%s)\n", rc, smash_err(rc));
        return (rc);
    }

    id1 = ee_id_string(flash_dev1);
    id2 = ee_id_string(flash_dev2);

    if (strcmp(id1, "Unknown") == 0) {
        printf("Failed to identify device 1 (%08x)\n", flash_dev1);
        rc = MSG_STATUS_BAD_DATA;
    }
    if ((mode == 32) && (strcmp(id2, "Unknown") == 0)) {
        printf("Failed to identify device 2 (%08x)\n", flash_dev2);
        rc = MSG_STATUS_BAD_DATA;
    }

    if (flag_quiet)
        return (rc);

    printf("Flash ID\n");
    if (mode == 16) {
        printf("  %08x %s", flash_dev1, id1);
    } else {
        printf("  %08x %08x %s %s", flash_dev1, flash_dev2, id1, id2);
    }
    printf(" (%u-bit mode)\n", mode);
    if ((mode == 32) && (flash_dev1 != flash_dev2)) {
        printf("  Warning: flash device ids differ\n");
        rc = MSG_STATUS_NO_REPLY;
    }

    return (rc);
}

static void
rom_bank_show(void)
{
    bank_info_t info;
    uint        rlen;
    uint        rc;
    uint        bank;

    rc = send_cmd(KS_CMD_BANK_INFO, NULL, 0, &info, sizeof (info), &rlen);
    if (rc != 0) {
        printf("Failed to get bank information: %d %s\n", rc, smash_err(rc));
        return;
    }
    printf("Bank  Name            Merge LongReset  PowerOn  Current  "
           "NextReset\n");
    for (bank = 0; bank < ROM_BANKS; bank++) {
        uint aspaces = 2;
        uint pos;
        uint banks_add = info.bi_merge[bank] >> 4;
        uint bank_sub  = info.bi_merge[bank] & 0xf;
        printf("%-5u %-15s ", bank, info.bi_name[bank]);

        if (banks_add < 1)
            aspaces += 4;
        else if (bank_sub == 0)
            printf("-\\  ");
        else if (bank_sub == banks_add)
            printf("-/  ");
        else
            printf("  | ");

        for (pos = 0; pos < ARRAY_SIZE(info.bi_longreset_seq); pos++)
            if (info.bi_longreset_seq[pos] == bank)
                break;

        if (pos < ARRAY_SIZE(info.bi_longreset_seq)) {
            printf("%*s%u", aspaces, "", pos);
            aspaces = 0;
        } else {
            aspaces++;
        }
        aspaces += 10;

        if (bank == info.bi_bank_poweron) {
            printf("%*s*", aspaces, "");
            aspaces = 0;
        } else {
            aspaces++;
        }
        aspaces += 8;

        if (bank == info.bi_bank_current) {
            printf("%*s*", aspaces, "");
            aspaces = 0;
        } else {
            aspaces++;
        }
        aspaces += 8;

        if (bank == info.bi_bank_nextreset) {
            printf("%*s*", aspaces, "");
            aspaces = 0;
        } else {
            aspaces++;
        }
        aspaces += 8;
        printf("\n");
    }
}

static int
cmd_bank(int argc, char *argv[])
{
    const char *ptr;
    uint8_t  bank_start = 0;
    uint8_t  bank_end   = 0;
    uint     flag_bank_merge       = 0;
    uint     flag_bank_unmerge     = 0;
    uint     bank;
    uint     rc;
    uint     opt = 0;
    int      arg;
    uint16_t argval;

    if (argc < 2) {
        printf("-b requires an argument\n");
        printf("One of: ?, show, name, longreset, nextreset, "
               "poweron, merge, unmerge\n");
        return (1);
    }
    for (arg = 1; arg < argc; arg++) {
        ptr = long_to_short(argv[arg], long_to_short_bank,
                            ARRAY_SIZE(long_to_short_bank));
        if (*ptr == '-') {
            for (++ptr; *ptr != '\0'; ptr++) {
                switch (*ptr) {
                    case 'c':  // current
                        opt = KS_BANK_SETCURRENT;
set_current_poweron_reset_bank:
                        if (++arg >= argc) {
                            printf("-b -%s requires a <bank> number to set\n",
                                    ptr);
                            return (1);
                        }
                        bank = atoi(argv[arg]);
                        if (bank >= ROM_BANKS) {
                            printf("Bank %u is invalid (maximum bank is %u)\n",
                                   bank, ROM_BANKS - 1);
                            return (1);
                        }

                        if (++arg < argc) {
                            if (strcmp(argv[arg], "reboot") == 0) {
                                opt |= KS_BANK_REBOOT;
                            } else {
                                printf("-b -%s only accepts \"reboot\" as an "
                                       "option after bank number\n", ptr);
                                return (1);
                            }
                        }

                        argval = bank;
                        rc = send_cmd(KS_CMD_BANK_SET | opt, &argval,
                                      sizeof (argval), NULL, 0, NULL);
                        if (rc != 0) {
                            printf("Bank set failed: %d %s\n",
                                   rc, smash_err(rc));
                        }
                        return (rc);
                    case 'h':  // help
                        rc = 0;
usage:
                        printf("%s", cmd_bank_options);
                        return (rc);
                    case 'l': {  // longreset
                        bank_info_t info;
                        uint8_t     banks[ROM_BANKS];
                        uint        cur;
                        uint        rlen;

                        rc = send_cmd(KS_CMD_BANK_INFO, NULL, 0, &info,
                                      sizeof (info), &rlen);
                        if (rc != 0) {
                            printf("Failed to get bank information: %d %s\n",
                                   rc, smash_err(rc));
                            return (rc);
                        }

                        rc = 0;
                        for (cur = 0; cur < ROM_BANKS; cur++) {
                            if (++arg < argc) {
                                uint sub;
                                bank = atoi(argv[arg]);
                                if (bank >= ROM_BANKS) {
                                    printf("Bank %u is invalid (maximum bank "
                                           "is %u)\n", bank, ROM_BANKS - 1);
                                    rc++;
                                    continue;
                                }
                                sub = info.bi_merge[bank] & 0x0f;
                                if (sub != 0) {
                                    printf("Bank %u is part of a merged bank, "
                                           "but is not the first (use %u)\n",
                                           bank, bank - sub);
                                    rc++;
                                }
                                banks[cur] = bank;
                            } else {
                                banks[cur] = 0xff;
                            }
                        }
                        if (rc != 0)
                            return (rc);

                        rc = send_cmd(KS_CMD_BANK_LRESET, &banks,
                                      sizeof (banks), NULL, 0, NULL);
                        if (rc != 0) {
                            printf("Bank longreset failed: %d %s\n",
                                   rc, smash_err(rc));
                        }
                        return (rc);
                    }
                    case 'm': {  // merge
                        bank_info_t info;
                        uint        count;
                        uint        rlen;
                        flag_bank_merge++;
bank_merge_unmerge:
                        if (++arg != argc - 2) {
                            printf("-b -%s requires <start> and <end> bank "
                                   "numbers (range)\n", ptr);
                            return (1);
                        }
                        bank_start = atoi(argv[arg++]);
                        bank_end   = atoi(argv[arg]);
                        count = bank_end - bank_start + 1;
                        if (bank_start > bank_end) {
                            printf("bank %u is not less than end %u\n",
                                   bank_start, bank_end);
                            return (1);
                        }
                        if (bank_end >= ROM_BANKS) {
                            printf("Bank %u is invalid (maximum bank is %u)\n",
                                   bank_end, ROM_BANKS - 1);
                            return (1);
                        }
                        if ((count != 1) && (count != 2) && (count != 4) &&
                            (count != 8)) {
                            printf("Bank sizes must be a power of 2 "
                                   "(1, 2, 4, or 8 banks)\n");
                            return (1);
                        }
                        if ((count == 2) && (bank_start & 1)) {
                            printf("Two-bank ranges must start with an "
                                   "even bank number (0, 2, 4, or 6)\n");
                            return (1);
                        }
                        if ((count == 4) &&
                            (bank_start != 0) && (bank_start != 4)) {
                            printf("Four-bank ranges must start with either "
                                   "bank 0 or bank 4\n");
                            return (1);
                        }
                        if ((count == 8) && (bank_start != 0)) {
                            printf("Eight-bank ranges must start with "
                                   "bank 0\n");
                            return (1);
                        }

                        rc = send_cmd(KS_CMD_BANK_INFO, NULL, 0,
                                      &info, sizeof (info), &rlen);
                        if (rc != 0) {
                            printf("Failed to get bank information: %d %s\n",
                                   rc, smash_err(rc));
                            return (rc);
                        }
                        for (bank = bank_start; bank <= bank_end; bank++) {
                            if (flag_bank_merge && (info.bi_merge[bank] != 0)) {
                                uint banks = (info.bi_merge[bank] >> 4) + 1;
                                printf("Bank %u is already part of a%s %u "
                                       "bank range\n",
                                       bank, (banks == 8) ? "n" : "", banks);
                                return (1);
                            }
                            if (flag_bank_unmerge &&
                                (info.bi_merge[bank] == 0)) {
                                printf("Bank %u is not part of a bank range\n",
                                       bank);
                                return (1);
                            }
                        }

                        argval = bank_start | (bank_end << 8);
                        rc = send_cmd(KS_CMD_BANK_MERGE | opt, &argval,
                                      sizeof (argval), NULL, 0, NULL);
                        if (rc != 0) {
                            printf("Bank %smerge failed: %d %s\n",
                                   (opt != 0) ? "un" : "", rc, smash_err(rc));
                            return (rc);
                        }
                        break;
                    }
                    case 'n': {  // name
                        char argbuf[64];
                        if (++arg != argc - 2) {
                            printf("-b %s requires a <bank> number and "
                                   "\"name text\"\n", argv[1]);
                            return (1);
                        }
                        bank = atoi(argv[arg]);
                        if (bank >= ROM_BANKS) {
                            printf("Bank %u is invalid (maximum bank is %u)\n",
                                   bank, ROM_BANKS - 1);
                            return (1);
                        }
                        argval = bank;
                        memcpy(argbuf, &argval, 2);
                        strncpy(argbuf + 2, argv[++arg], sizeof (argbuf) - 3);
                        argbuf[sizeof (argbuf) - 1] = '\0';
                        rc = send_cmd(KS_CMD_BANK_NAME, argbuf,
                                      strlen(argbuf + 2) + 3, NULL, 0, NULL);
                        if (rc != 0) {
                            printf("Bank name set failed: %d %s\n",
                                   rc, smash_err(rc));
                        }
                        return (rc);
                    }
                    case 'N':  // nextreset
                        opt = KS_BANK_SETRESET;
                        goto set_current_poweron_reset_bank;
                    case 'p':  // poweron
                        opt = KS_BANK_SETPOWERON;
                        goto set_current_poweron_reset_bank;
                    case 's':  // show
                        rom_bank_show();
                        return (0);
                    case 'u':  // unmerge
                        flag_bank_unmerge++;
                        opt = KS_BANK_UNMERGE;
                        goto bank_merge_unmerge;
                    default:
                        printf("Unknown argument %s \"-%s\"\n",
                               argv[0], ptr);
                        goto usage;
                }
            }
        } else {
            printf("Unknown argument %s \"%s\"\n",
                   argv[0], ptr);
            goto usage;
        }
    }
    return (0);
}

/*
 * are_you_sure() prompts the user to confirm that an operation is intended.
 *
 * @param  [in]  None.
 *
 * @return       TRUE  - User has confirmed (Y).
 * @return       FALSE - User has denied (N).
 */
int
are_you_sure(const char *prompt)
{
    int ch;
ask_again:
    printf("%s - are you sure? (y/n) ", prompt);
    fflush(stdout);
    while ((ch = getchar()) != EOF) {
        if (is_user_abort()) {
            printf("^C\n");
            return (FALSE);
        }
        if ((ch == 'y') || (ch == 'Y'))
            return (TRUE);
        if ((ch == 'n') || (ch == 'N'))
            return (FALSE);
        if (!isspace(ch))
            goto ask_again;
    }
    return (FALSE);
}

static uint
get_file_size(const char *filename)
{
    struct FileInfoBlock fb;
    BPTR lock;
    fb.fib_Size = VALUE_UNASSIGNED;
    lock = Lock(filename, ACCESS_READ);
    if (lock == 0L) {
        printf("Lock %s failed\n", filename);
        return (VALUE_UNASSIGNED);
    }
    if (Examine(lock, &fb) == 0) {
        printf("Examine %s failed\n", filename);
        UnLock(lock);
        return (VALUE_UNASSIGNED);
    }
    UnLock(lock);
    return (fb.fib_Size);
}

#define SWAPMODE_A500  0xA500   // Amiga 16-bit ROM format
#define SWAPMODE_A3000 0xA3000  // Amiga 32-bit ROM format

#define SWAP_TO_ROM    0  // Bytes originated in a file (to be written in ROM)
#define SWAP_FROM_ROM  1  // Bytes originated in ROM (to be written to a file)

/*
 * execute_swapmode() swaps bytes in the specified buffer according to the
 *                    currently active swap mode.
 *
 * @param  [io]  buf      - Buffer to modify.
 * @param  [in]  len      - Length of data in the buffer.
 * @gloabl [in]  dir      - Image swap direction (SWAP_TO_ROM or SWAP_FROM_ROM)
 * @gloabl [in]  swapmode - Swap operation to perform (0123, 3210, etc)
 * @return       None.
 */
static void
execute_swapmode(uint8_t *buf, uint len, uint dir, uint swapmode)
{
    uint    pos;
    uint8_t temp;
    static const uint8_t str_f94e1411[] = { 0xf9, 0x4e, 0x14, 0x11 };
    static const uint8_t str_11144ef9[] = { 0x11, 0x14, 0x4e, 0xf9 };
    static const uint8_t str_1411f94e[] = { 0x14, 0x11, 0xf9, 0x4e };
    static const uint8_t str_4ef91114[] = { 0x4e, 0xf9, 0x11, 0x14 };

    switch (swapmode) {
        case 0:
        case 0123:
            return;  // Normal (no swap)
        swap_1032:
        case 1032:
            /* Swap adjacent bytes in 16-bit words */
            for (pos = 0; pos < len - 1; pos += 2) {
                temp         = buf[pos + 0];
                buf[pos + 0] = buf[pos + 1];
                buf[pos + 1] = temp;
            }
            return;
        swap_2301:
        case 2301:
            /* Swap adjacent (16-bit) words */
            for (pos = 0; pos < len - 3; pos += 4) {
                temp         = buf[pos + 0];
                buf[pos + 0] = buf[pos + 2];
                buf[pos + 2] = temp;
                temp         = buf[pos + 1];
                buf[pos + 1] = buf[pos + 3];
                buf[pos + 3] = temp;
            }
            return;
        swap_3210:
        case 3210:
            /* Swap bytes in 32-bit longs */
            for (pos = 0; pos < len - 3; pos += 4) {
                temp         = buf[pos + 0];
                buf[pos + 0] = buf[pos + 3];
                buf[pos + 3] = temp;
                temp         = buf[pos + 1];
                buf[pos + 1] = buf[pos + 2];
                buf[pos + 2] = temp;
            }
            return;
        case SWAPMODE_A500:
            if (dir == SWAP_TO_ROM) {
                /* Need bytes in order: 14 11 f9 4e */
                if (memcmp(buf, str_1411f94e, 4) == 0)
                    return;  // Already in desired order
                if (memcmp(buf, str_11144ef9, 4) == 0) {
                    printf("Swap mode 2301\n");
                    goto swap_2301;  // Swap adjacent 16-bit words
                }
            }
            if (dir == SWAP_FROM_ROM) {
                /* Need bytes in order: 11 14 4e f9 */
                if (memcmp(buf, str_11144ef9, 4) == 0)
                    return;  // Already in desired order
                if (memcmp(buf, str_1411f94e, 4) == 0) {
                    printf("Swap mode 1032\n");
                    goto swap_1032;  // Swap odd/even bytes
                }
            }
            goto unrecognized;
        case SWAPMODE_A3000:
            if (dir == SWAP_TO_ROM) {
                /* Need bytes in order: f9 4e 14 11 */
                if (memcmp(buf, str_f94e1411, 4) == 0)
                    return;  // Already in desired order
                if (memcmp(buf, str_11144ef9, 4) == 0) {
                    printf("Swap mode 3210\n");
                    goto swap_3210;  // Swap bytes in 32-bit longs
                }
                if (memcmp(buf, str_1411f94e, 4) == 0) {
                    printf("Swap mode 2301\n");
                    goto swap_2301;  // Swap adjacent 16-bit words
                }
                if (memcmp(buf, str_4ef91114, 4) == 0) {
                    printf("Swap mode 1032\n");
                    goto swap_1032;  // Swap odd/even bytes
                }
            }
            if (dir == SWAP_FROM_ROM) {
                /* Need bytes in order: 11 14 4e f9 */
                if (memcmp(buf, str_11144ef9, 4) == 0)
                    return;  // Already in desired order
                if (memcmp(buf, str_f94e1411, 4) == 0) {
                    printf("Swap mode 3210\n");
                    goto swap_3210;  // Swap bytes in 32-bit longs
                }
                if (memcmp(buf, str_4ef91114, 4) == 0) {
                    printf("Swap mode 2301\n");
                    goto swap_2301;  // Swap adjacent 16-bit words
                }
                if (memcmp(buf, str_1411f94e, 4) == 0) {
                    printf("Swap mode 1032\n");
                    goto swap_1032;  // Swap odd/even bytes
                }
            }
unrecognized:
            printf("Unrecognized Amiga ROM format: %02x %02x %02x %02x\n",
                   buf[0], buf[1], buf[2], buf[3]);
            exit(EXIT_FAILURE);
    }
}

static uint
read_from_flash(uint bank, uint addr, void *buf, uint len)
{
    uint rc;
    uint16_t bankarg = bank;
    SUPERVISOR_STATE_ENTER();
    INTERRUPTS_DISABLE();
    CACHE_DISABLE_DATA();
    MMU_DISABLE();

#undef USE_OVERLAY
#ifdef USE_OVERLAY
    *CIAA_PRA |= CIAA_PRA_OVERLAY | CIAA_PRA_LED;
#endif
    rc = send_cmd_core(KS_CMD_BANK_SET | KS_BANK_SETTEMP,
                       &bankarg, sizeof (bankarg), NULL, 0, NULL);
    cia_spin(CIA_USEC(1000));
#ifdef USE_OVERLAY
    local_memcpy(buf, (void *) (addr), len);
    *CIAA_PRA &= ~(CIAA_PRA_OVERLAY | CIAA_PRA_LED);
#else
    local_memcpy(buf, (void *) (ROM_BASE + addr), len);
#endif
    rc |= send_cmd_core(KS_CMD_BANK_SET | KS_BANK_UNSETTEMP,
                        &bankarg, sizeof (bankarg), NULL, 0, NULL);
    cia_spin(CIA_USEC(1000));

    MMU_RESTORE();
    CACHE_RESTORE_STATE();
    INTERRUPTS_ENABLE();
    SUPERVISOR_STATE_EXIT();
    return (rc);
}

static int
wait_for_flash_done(uint addr, uint erase_mode)
{
    uint32_t status;
    uint32_t cstatus = 0;
    uint32_t lstatus;
    uint     spins = erase_mode ? 1000000 : 50000; // 1 sec or 50ms
    uint     spin_count = 0;
    int      same_count = 0;
    int      see_fail_count = 0;
    lstatus = *ADDR32(addr);
    while (spin_count < spins) {
        status = *ADDR32(addr);

        cstatus = status;
        /* Filter out checking of status which is already done */
        if (((cstatus ^ lstatus) & 0x0000ffff) == 0)
            cstatus &= ~0x0000ffff;
        if (((cstatus ^ lstatus) & 0xffff0000) == 0)
            cstatus &= ~0xffff0000;

        if (status == lstatus) {
            if (same_count++ >= 1) {
                /* Same for 2 tries */
                if (erase_mode && (status != 0xffffffff)) {
                    /* Something went wrong -- block protected? */
                    return (MSG_STATUS_PRG_FAIL);
                }
                return (0);
            }
        } else {
            same_count = 0;
            lstatus = status;
        }

        if (cstatus & (BIT(5) | BIT(5 + 16)))  // Program / erase failure
            if (see_fail_count++ > 5)
                break;
        __asm__ __volatile__("nop");
        cia_spin(1);
    }

    if (cstatus & (BIT(5) | BIT(5 + 16))) {
        /* Program / erase failure */
        return (MSG_STATUS_PRG_FAIL);
    }

    /* Program / erase timeout */
    return (MSG_STATUS_PRG_TMOUT);
}

static uint
write_to_flash(uint bank, uint addr, void *buf, uint len)
{
    uint rc;
    uint xlen;
    uint8_t *xbuf = buf;
    uint16_t bankarg = bank;

    SUPERVISOR_STATE_ENTER();
    INTERRUPTS_DISABLE();
    CACHE_DISABLE_DATA();
    MMU_DISABLE();

    /* Switch to flash bank to be programmed */
    rc = send_cmd_core(KS_CMD_BANK_SET | KS_BANK_SETTEMP,
                       &bankarg, sizeof (bankarg), NULL, 0, NULL);
    cia_spin(CIA_USEC(1000));

    if (rc == 0) {
        /* Write flash data */
        while (len > 0) {
            xlen = len;
            if (xlen > 4)
                xlen = 4;

            rc = flash_cmd_core(KS_CMD_FLASH_WRITE, xbuf, xlen);
            if (rc != 0)
                break;

            *ADDR32(ROM_BASE + addr);  // Generate address for write
            rc = wait_for_flash_done(ROM_BASE, 0);
            if (rc != 0)
                break;

            len  -= xlen;
            xbuf += xlen;
            addr += xlen;
        }
    }
    cia_spin(CIA_USEC(1000));

    /* Restore flash to read mode */
    rc |= flash_cmd_core(KS_CMD_FLASH_READ, NULL, 0);
    cia_spin(CIA_USEC(1000));

    /* Return to "current" flash bank */
    rc |= send_cmd_core(KS_CMD_BANK_SET | KS_BANK_UNSETTEMP,
                        &bankarg, sizeof (bankarg), NULL, 0, NULL);
    cia_spin(CIA_USEC(1000));

    MMU_RESTORE();
    CACHE_RESTORE_STATE();
    INTERRUPTS_ENABLE();
    SUPERVISOR_STATE_EXIT();
    return (rc);
}

static uint
erase_flash_block(uint bank, uint addr)
{
    uint rc;
    uint rc1;
    uint16_t bankarg = bank;

    SUPERVISOR_STATE_ENTER();
    INTERRUPTS_DISABLE();
    CACHE_DISABLE_DATA();
    MMU_DISABLE();

    /* Switch to flash bank to be programmed */
    rc = send_cmd_core(KS_CMD_BANK_SET | KS_BANK_SETTEMP,
                       &bankarg, sizeof (bankarg), NULL, 0, NULL);
    cia_spin(CIA_USEC(1000));

    /* Send erase command */
    rc = flash_cmd_core(KS_CMD_FLASH_ERASE, NULL, 0);
    if (rc == 0) {
        *ADDR32(ROM_BASE + addr);  // Generate address for erase
        rc = wait_for_flash_done(ROM_BASE + addr, 1);
    }
    cia_spin(CIA_USEC(1000));

    /* Restore flash to read mode */
    rc1 = flash_cmd_core(KS_CMD_FLASH_READ, NULL, 0);
    if (rc == 0)
        rc = rc1;
    cia_spin(CIA_USEC(1000));

    /* Return to "current" flash bank */
    rc1 = send_cmd_core(KS_CMD_BANK_SET | KS_BANK_UNSETTEMP,
                        &bankarg, sizeof (bankarg), NULL, 0, NULL);
    if (rc == 0)
        rc = rc1;
    cia_spin(CIA_USEC(1000));

    MMU_RESTORE();
    CACHE_RESTORE_STATE();
    INTERRUPTS_ENABLE();
    SUPERVISOR_STATE_EXIT();
    return (rc);
}

/*
 * cmd_readwrite
 * -------------
 * Read from flash and write to file or read from file and write to flash.
 */
int
cmd_readwrite(int argc, char *argv[])
{
    bank_info_t info;
    const char *ptr;
    const char *filename = NULL;
    uint64_t    time_start;
    uint64_t    time_rw_end;
    uint64_t    time_end;
    int         arg;
    int         pos;
    int         bytes;
    uint        flag_dump = 0;
    uint        flag_yes = 0;
    uint        addr = VALUE_UNASSIGNED;
    uint        bank = VALUE_UNASSIGNED;
    uint        len  = VALUE_UNASSIGNED;
    uint        start_addr;
    uint        start_bank;
    uint        start_len;
    uint        file_is_stdio = 0;
    uint        rlen;
    uint        rc = 1;
    uint        bank_sub;
    uint        bank_size;
    FILE       *file;
    uint        swapmode = 0123;  // no swap
    uint        writemode = 0;
    uint        verifymode = 0;
    uint        readmode = 0;
    uint        dot_count = 1;
    uint        dot_iters = 1;
    uint        dot_max;
    uint8_t    *buf;
    uint8_t    *vbuf = NULL;

    if ((strcmp(argv[0], "-w") == 0) || (strcmp(argv[0], "write") == 0))
        writemode = 1;
    else if ((strcmp(argv[0], "-v") == 0) || (strcmp(argv[0], "verify") == 0))
        verifymode = 1;
    else
        readmode = 1;

    for (arg = 1; arg < argc; arg++) {
        ptr = long_to_short(argv[arg], long_to_short_readwrite,
                            ARRAY_SIZE(long_to_short_readwrite));
        if (*ptr == '-') {
            for (++ptr; *ptr != '\0'; ptr++) {
                switch (*ptr) {
                    case 'a':  // addr
                        if (++arg > argc) {
                            printf("smash %s %s requires an option\n",
                                   argv[0], ptr);
                            goto usage;
                        }
                        pos = 0;
                        if ((sscanf(argv[arg], "%x%n", &addr, &pos) != 1) ||
                            (pos == 0) || (argv[arg][pos] != '\0')) {
                            printf("Invalid argument \"%s\" for %s %s\n",
                                   argv[arg], argv[0], ptr);
                            goto usage;
                        }
                        break;
                    case 'b':  // bank
                        if (++arg > argc) {
                            printf("smash %s %s requires an option\n",
                                   argv[0], ptr);
                            goto usage;
                        }
                        pos = 0;
                        if ((sscanf(argv[arg], "%x%n", &bank, &pos) != 1) ||
                            (pos == 0) || (argv[arg][pos] != '\0') ||
                            (bank >= ROM_BANKS)) {
                            printf("Invalid argument \"%s\" for %s %s\n",
                                   argv[arg], argv[0], ptr);
                            goto usage;
                        }
                        break;
                    case 'D':  // debug
                        flag_debug++;
                        break;
                    case 'd':  // dump
                        flag_dump++;
                        break;
                    case 'f':  // file
                        if (++arg > argc) {
                            printf("smash %s %s requires an option\n",
                                   argv[0], ptr);
                            goto usage;
                        }
                        filename = argv[arg];
                        if (strcmp(filename, "-") == 0)
                            file_is_stdio++;
                        break;
                    case 'h':  // help
                        rc = 0;
usage:
                        if (writemode)
                            printf("%s", cmd_write_options);
                        else if (readmode)
                            printf("%s", cmd_read_options);
                        else
                            printf("%s", cmd_verify_options);
                        return (rc);
                    case 'l':  // length
                        if (++arg > argc) {
                            printf("smash %s %s requires an option\n",
                                   argv[0], ptr);
                            goto usage;
                        }
                        pos = 0;
                        if ((sscanf(argv[arg], "%x%n", &len, &pos) != 1) ||
                            (pos == 0) || (argv[arg][pos] != '\0')) {
                            printf("Invalid argument \"%s\" for %s %s\n",
                                   argv[arg], argv[0], ptr);
                            goto usage;
                        }
                        break;
                    case 's':  // swap
                        if (++arg > argc) {
                            printf("smash %s %s requires an option\n",
                                   argv[0], ptr);
                            goto usage;
                        }
                        if ((strcasecmp(argv[arg], "a3000") == 0) ||
                            (strcasecmp(argv[arg], "a4000") == 0) ||
                            (strcasecmp(argv[arg], "a3000t") == 0) ||
                            (strcasecmp(argv[arg], "a4000t") == 0) ||
                            (strcasecmp(argv[arg], "a1200") == 0)) {
                            swapmode = SWAPMODE_A3000;
                            break;
                        }
                        if ((strcasecmp(argv[arg], "a500") == 0) ||
                            (strcasecmp(argv[arg], "a600") == 0) ||
                            (strcasecmp(argv[arg], "a1000") == 0) ||
                            (strcasecmp(argv[arg], "a2000") == 0) ||
                            (strcasecmp(argv[arg], "cdtv") == 0)) {
                            swapmode = SWAPMODE_A500;
                            break;
                        }
                        pos = 0;
                        if ((sscanf(argv[arg], "%u%n", &swapmode, &pos) != 1) ||
                            (pos == 0) || (argv[arg][pos] != '\0') ||
                            ((swapmode != 0123) && (swapmode != 1032) &&
                             (swapmode != 2301) && (swapmode != 3210))) {
                            printf("Invalid argument \"%s\" for %s %s\n",
                                   argv[arg], argv[0], ptr);
                            printf("Use 1032, 2301, or 3210\n");
                            return (1);
                        }
                        break;
                    case 'v':  // verify
                        verifymode++;
                        break;
                    case 'y':  // yes
                        flag_yes++;
                        break;
                    default:
                        printf("Unknown argument %s \"-%s\"\n",
                               argv[0], ptr);
                        goto usage;
                }
            }
        } else {
            printf("Unknown argument %s \"%s\"\n",
                   argv[0], ptr);
            goto usage;
        }
    }
    if (filename == NULL) {
        if (flag_dump) {
            file_is_stdio++;
            filename = "-";
        } else {
            printf("You must supply a filename");
            if (readmode)
                printf(" or - for stdout");
            printf("\n");
            goto usage;
        }
    }
    if (bank == VALUE_UNASSIGNED) {
        printf("You must supply a bank number\n");
        goto usage;
    }
    if (flag_dump && !file_is_stdio) {
        printf("Can only dump ASCII text to stdout\n");
        return (1);
    }
    if (addr == VALUE_UNASSIGNED)
        addr = 0;

    rc = send_cmd(KS_CMD_BANK_INFO, NULL, 0, &info, sizeof (info), &rlen);
    if (rc != 0) {
        printf("Failed to get bank information: %d %s\n",
               rc, smash_err(rc));
        return (rc);
    }
    bank_sub  = info.bi_merge[bank] & 0x0f;
    bank_size = ((info.bi_merge[bank] + 0x10) & 0xf0) << 15;  // size in bytes
    if (bank_sub != 0) {
        printf("Bank %u is part of a merged bank, but is not the "
               "first (use %u)\n", bank, bank - bank_sub);
        return (1);
    }

    if (len == VALUE_UNASSIGNED) {
        if (readmode) {
            /* Get length from size of bank */
            len = bank_size;
        } else {
            /* Get length from size of file */
            len = get_file_size(filename);
            if (len == VALUE_UNASSIGNED) {
                return (1);
            }
        }
    }
    if (len > bank_size) {
        printf("Length 0x%x is greater than bank size 0x%x\n",
               len, bank_size);
        return (1);
    } else if (addr + len > bank_size) {
        printf("Length 0x%x + address overflows bank (size 0x%x)\n",
               addr + len, bank_size);
        return (1);
    }

    if (!readmode && file_is_stdio) {
        printf("STDIO not supported for this mode\n");
        return (1);
    }

    if (writemode) {
        printf("Write bank=%u addr=%x len=%x from ", bank, addr, len);
        if (file_is_stdio)
            printf("stdin");
        else
            printf("file=\"%s\"", filename);
        printf("\n");
    } else {
        if (readmode)
            printf("Read");
        else
            printf("Verify");
        printf(" bank=%u addr=%x len=%x ", bank, addr, len);
        if (readmode)
            printf("to ");
        else
            printf("matches ");
        if (file_is_stdio)
            printf("stdout");
        else
            printf("file=\"%s\"", filename);
        if (flag_dump)
            printf(" (ASCII dump)");
        printf("\n");
    }
    if ((!flag_yes) && (!file_is_stdio || (flag_dump && (len >= 0x1000))) &&
        (!are_you_sure("Proceed"))) {
        return (1);
    }

    buf = AllocMem(MAX_CHUNK, MEMF_PUBLIC);
    if (buf == NULL) {
        printf("Failed to allocate 0x%x bytes\n", MAX_CHUNK);
        return (1);
    }
    if (verifymode) {
        vbuf = AllocMem(MAX_CHUNK, MEMF_PUBLIC);
        if (vbuf == NULL) {
            printf("Failed to allocate 0x%x bytes\n", MAX_CHUNK);
            FreeMem(buf, MAX_CHUNK);
            return (1);
        }
    }

    if (file_is_stdio) {
        file = stdout;
    } else {
        char *filemode;
        if (writemode || !readmode)
            filemode = "r";
        else if (readmode && verifymode)
            filemode = "w+";
        else
            filemode = "w";

        file = fopen(filename, filemode);
        if (file == NULL) {
            printf("Failed to open \"%s\", for %s\n", filename,
                   readmode ? "write" : "read");
            rc = 1;
            goto fail_end;
        }
    }

    rc = 0;

    time_start = smash_time();

    bank += addr / ROM_WINDOW_SIZE;
    addr &= (ROM_WINDOW_SIZE - 1);

    start_bank = bank;
    start_addr = addr;
    start_len = len;

    dot_max = (len + MAX_CHUNK - 1) / MAX_CHUNK;
    while (dot_max > 50) {
        dot_max >>= 1;
        dot_iters <<= 1;
    }
    if (readmode || writemode) {
        dot_count = 0;
        if (!file_is_stdio) {
            printf("Progress [%*s]\rProgress [", dot_max, "");
            fflush(stdout);
        }
        while (len > 0) {
            uint xlen = len;
            if (xlen > MAX_CHUNK)
                xlen = MAX_CHUNK;
            if (xlen > ROM_WINDOW_SIZE - addr) {
                xlen = ROM_WINDOW_SIZE - addr;
            }

            if (writemode) {
                /* Read from file */
                bytes = fread(buf, 1, xlen, file);
                if (bytes < (int) xlen) {
                    printf("\nFailed to read %u bytes from %s\n",
                           xlen, filename);
                    rc = 1;
                    break;
                }
            } else {
                /* Read from flash */
                rc = read_from_flash(bank, addr, buf, xlen);
                if (rc != 0) {
                    printf("\nKicksmash failure %d (%s)\n", rc, smash_err(rc));
                    break;
                }
            }

            execute_swapmode(buf, xlen, SWAP_FROM_ROM, swapmode);

            if (writemode) {
                /* Write to flash */
                rc = write_to_flash(bank, addr, buf, xlen);
                if (rc != 0) {
                    printf("\nKicksmash failure %d (%s)\n", rc, smash_err(rc));
                    break;
                }
            } else {
                /* Output to file or stdout */
                if (file_is_stdio) {
                    dump_memory((uint32_t *)buf, xlen, addr);
                } else {
                    bytes = fwrite(buf, 1, xlen, file);
                    if (bytes < (int) xlen) {
                        printf("\nFailed to write all bytes to %s\n", filename);
                        rc = 1;
                        break;
                    }
                }
            }
            if ((!file_is_stdio) && (++dot_count == dot_iters)) {
                dot_count = 0;
                printf(".");
                fflush(stdout);
            }
            if (is_user_abort()) {
                printf("^C\n");
                rc = 2;
                goto fail_end;
            }

            len  -= xlen;
            addr += xlen;
            if (addr >= ROM_WINDOW_SIZE) {
                addr -= ROM_WINDOW_SIZE;
                bank++;
            }
        }
    }
    time_rw_end = smash_time();

    if (verifymode && (rc == 0)) {
        dot_count = 0;
        if (!file_is_stdio) {
            if ((rc == 0) && (readmode || writemode))
                printf("]\n");
            printf("  Verify [%*s]\r  Verify [", dot_max, "");
            fflush(stdout);
        }

        /* Restart positions */
        fseek(file, 0, SEEK_SET);
        bank = start_bank;
        addr = start_addr;
        len  = start_len;

        while (len > 0) {
            uint xlen = len;
            if (xlen > MAX_CHUNK)
                xlen = MAX_CHUNK;
            if (xlen > ROM_WINDOW_SIZE - addr) {
                xlen = ROM_WINDOW_SIZE - addr;
            }

            /* Read from file */
            bytes = fread(vbuf, 1, xlen, file);
            if (bytes < (int) xlen) {
                printf("\nFailed to read %u bytes from %s\n", xlen, filename);
                rc = 1;
                break;
            }

            /* Read from flash */
            rc = read_from_flash(bank, addr, buf, xlen);
            if (rc != 0) {
                printf("\nKicksmash failure %d (%s)\n", rc, smash_err(rc));
                break;
            }
            execute_swapmode(buf, xlen, SWAP_FROM_ROM, swapmode);

            if (memcmp(buf, vbuf, xlen) != 0) {
                uint pos;
                uint32_t *buf1 = (uint32_t *) buf;
                uint32_t *buf2 = (uint32_t *) vbuf;
                printf("\nVerify failure at bank %x address %x\n", bank, addr);
                for (pos = 0; pos < xlen / 4; pos++) {
                    if (buf1[pos] != buf2[pos]) {
                        if (rc++ < 5) {
                            printf("    %05x: %08x != file %08x\n",
                                   addr + pos * 4, buf1[pos], buf2[pos]);
                        }
                    }
                }
                printf("    %u miscompares in this block\n", rc);
                goto fail_end;
            }
            if (!file_is_stdio) {
                printf(".");
                fflush(stdout);
            }

            len  -= xlen;
            addr += xlen;
            if (addr >= ROM_WINDOW_SIZE) {
                addr -= ROM_WINDOW_SIZE;
                bank++;
            }
        }
    }
    if (!file_is_stdio && (rc == 0)) {
        time_end = smash_time();
        if (rc == 0)
            printf("]\n");
        fclose(file);
        if (readmode || writemode) {
            printf("%s complete in ", writemode ? "Write" : "Read");
            print_us_diff(time_start, time_rw_end);
        }
        if (verifymode) {
            printf("%s complete in ", "Verify");
            print_us_diff(time_rw_end, time_end);
        }
    }
fail_end:
    FreeMem(buf, MAX_CHUNK);
    if (vbuf != NULL)
        FreeMem(vbuf, MAX_CHUNK);
    return (rc);
}

static uint
get_flash_bsize(const chip_blocks_t *cb, uint flash_addr)
{
    uint flash_bsize = cb->cb_bsize << (10 + smash_cmd_shift);
    uint flash_bnum  = flash_addr / flash_bsize;
    if (flag_debug) {
        printf("Erase at %x bnum=%x: flash_bsize=%x flash_bbnum=%x\n",
               flash_addr, flash_bnum, flash_bsize, cb->cb_bbnum);
    }
    if (flash_bnum == cb->cb_bbnum) {
        /*
         * Boot block area has variable sub-block size.
         *
         * The map is 8 bits which are arranged in order such that Bit 0
         * represents the first sub-block and Bit 7 represents the last
         * sub-block.
         *
         * If a given bit is 1, this is the start of an erase block.
         * If a given bit is 0, then it is a continuation of the previous
         * bit's erase block.
         */
        uint bboff = flash_addr & (flash_bsize - 1);
        uint bsnum = (bboff / cb->cb_ssize) >> (10 + smash_cmd_shift);
        uint first_snum = bsnum;
        uint last_snum = bsnum;
        uint smap = cb->cb_map;
        if (flag_debug)
            printf(" bblock bb_off=%x snum=%x s_map=%x\n", bboff, bsnum, smap);
        flash_bsize = 0;
        /* Find first bit of this map */
        while (first_snum > 0) {
            if (smap & BIT(first_snum))  // Found base
                break;
            first_snum--;
        }
        while (++last_snum < 8) {
            if (smap & BIT(last_snum))  // Found next base
                break;
        }
        flash_bsize = (cb->cb_ssize * (last_snum - first_snum)) <<
                      (10 + smash_cmd_shift);
        if (flag_debug) {
            printf(" first_snum=%x last_snum=%x bb_ssize=%x\n",
                   first_snum, last_snum, flash_bsize);
        }
    } else if (flag_debug) {
        printf(" normal block %x\n", flash_bsize);
    }
    return (flash_bsize);
}

/*
 * cmd_erase
 * ---------
 * Erase flash
 */
int
cmd_erase(int argc, char *argv[])
{
    bank_info_t          info;
    const chip_blocks_t *cb;
    const char *ptr;
    uint64_t    time_start;
    uint64_t    time_end;
    uint32_t    flash_dev1;
    uint32_t    flash_dev2;
    const char *id1;
    const char *id2;
    uint        addr = VALUE_UNASSIGNED;
    uint        bank = VALUE_UNASSIGNED;
    uint        len  = VALUE_UNASSIGNED;
    uint        flag_yes = 0;
    uint        rc = 1;
    uint        rlen;
    uint        bank_sub;
    uint        bank_size;
    uint        flash_start_addr;
    uint        flash_end_addr;
    uint        flash_start_bsize;
    uint        flash_end_bsize;
    uint        mode = 0;
    uint        tlen = 0;
    uint        dot_count = 1;
    uint        dot_iters = 1;
    uint        dot_max;
    int         arg;
    int         pos;

    for (arg = 1; arg < argc; arg++) {
        ptr = long_to_short(argv[arg], long_to_short_erase,
                            ARRAY_SIZE(long_to_short_erase));
        if (*ptr == '-') {
            for (++ptr; *ptr != '\0'; ptr++) {
                switch (*ptr) {
                    case 'a':  // addr
                        if (++arg > argc) {
                            printf("smash %s %s requires an option\n",
                                   argv[0], ptr);
                            goto usage;
                        }
                        pos = 0;
                        if ((sscanf(argv[arg], "%x%n", &addr, &pos) != 1) ||
                            (pos == 0) || (argv[arg][pos] != '\0')) {
                            printf("Invalid argument \"%s\" for %s %s\n",
                                   argv[arg], argv[0], ptr);
                            goto usage;
                        }
                        break;
                    case 'b':  // bank
                        if (++arg > argc) {
                            printf("smash %s %s requires an option\n",
                                   argv[0], ptr);
                            goto usage;
                        }
                        pos = 0;
                        if ((sscanf(argv[arg], "%x%n", &bank, &pos) != 1) ||
                            (pos == 0) || (argv[arg][pos] != '\0') ||
                            (bank >= ROM_BANKS)) {
                            printf("Invalid argument \"%s\" for %s %s\n",
                                   argv[arg], argv[0], ptr);
                            goto usage;
                        }
                        break;
                    case 'd':  // debug
                        flag_debug++;
                        break;
                    case 'h':  // help
                        rc = 0;
usage:
                        printf("%s", cmd_erase_options);
                        return (rc);
                    case 'l':  // length
                        if (++arg > argc) {
                            printf("smash %s %s requires an option\n",
                                   argv[0], ptr);
                        }
                        pos = 0;
                        if ((sscanf(argv[arg], "%x%n", &len, &pos) != 1) ||
                            (pos == 0) || (argv[arg][pos] != '\0')) {
                            printf("Invalid argument \"%s\" for %s %s\n",
                                   argv[arg], argv[0], ptr);
                            goto usage;
                        }
                        break;
                    case 'y':  // yes
                        flag_yes++;
                        break;
                    default:
                        printf("Unknown argument %s \"-%s\"\n",
                               argv[0], ptr);
                        goto usage;
                }
            }
        } else {
            printf("Unknown argument %s \"%s\"\n",
                   argv[0], ptr);
            goto usage;
        }
    }

    if (bank == VALUE_UNASSIGNED) {
        printf("You must supply a bank number\n");
        goto usage;
    }
    if (addr == VALUE_UNASSIGNED)
        addr = 0;

    rc = send_cmd(KS_CMD_BANK_INFO, NULL, 0, &info, sizeof (info), &rlen);
    if (rc != 0) {
        printf("Failed to get bank information: %d %s\n",
               rc, smash_err(rc));
        return (rc);
    }

    bank_sub  = info.bi_merge[bank] & 0x0f;
    bank_size = ((info.bi_merge[bank] + 0x10) & 0xf0) << 15;  // size in bytes
    if (bank_sub != 0) {
        printf("Bank %u is part of a merged bank, but is not the "
               "first (use %u)\n", bank, bank - bank_sub);
        return (1);
    }

    if (len == VALUE_UNASSIGNED) {
        /* Get length from size of bank */
        len = bank_size - (addr & (bank_size - 1));
    } else if (len > bank_size) {
        printf("Specified length 0x%x is greater than bank size 0x%x\n",
               len, bank_size);
        return (1);
    } else if (addr + len > bank_size) {
        printf("Specified address + length (0x%x) overflows bank (size 0x%x)\n",
               addr + len, bank_size);
        return (1);
    }

    /* Acquire flash id to get block erase zones */
    rc = flash_id(&flash_dev1, &flash_dev2, &mode);
    if (rc != 0) {
        printf("Flash id failure %d (%s)\n", rc, smash_err(rc));
        return (rc);
    }
    id1 = ee_id_string(flash_dev1);
    id2 = ee_id_string(flash_dev2);
    if (flag_debug) {
        if (mode == 16)
            printf("    %08x %s", flash_dev1, id1);
        else
            printf("    %08x %08x %s %s", flash_dev1, flash_dev2, id1, id2);
        printf(" (%u-bit mode)\n", mode);
    }

    if (strcmp(id1, "Unknown") == 0) {
        printf("Failed to identify device 1 (%08x)\n", flash_dev1);
        rc = MSG_STATUS_BAD_DATA;
    }
    if ((mode == 32) && (strcmp(id2, "Unknown") == 0)) {
        printf("Failed to identify device 2 (%08x)\n", flash_dev2);
        rc = MSG_STATUS_BAD_DATA;
    }
    if ((mode == 32) && (flash_dev1 != flash_dev2)) {
        printf("    Failure: flash device ids differ (%08x %08x)\n",
               flash_dev1, flash_dev2);
        rc = MSG_STATUS_BAD_DATA;
    }
    if (rc != 0)
        return (rc);

    cb = get_chip_block_info(flash_dev1);
    if (cb == NULL) {
        printf("Failed to determine erase block information for %08x\n",
               flash_dev1);
        return (1);
    }
    flash_start_addr  = bank * ROM_WINDOW_SIZE + addr;
    flash_end_addr    = bank * ROM_WINDOW_SIZE + addr + len - 1;
    flash_start_bsize = get_flash_bsize(cb, flash_start_addr);
    flash_end_bsize   = get_flash_bsize(cb, flash_end_addr);

    if (flag_debug)
        printf("pre saddr=%x eaddr=%x\n", flash_start_addr, flash_end_addr);

    /* Round start address down and end address up, then compute length */
    flash_start_addr = flash_start_addr & ~(flash_start_bsize - 1);
    flash_end_addr   = (flash_end_addr | (flash_end_bsize - 1)) + 1;
    len = flash_end_addr - flash_start_addr;
    addr = addr & ~(flash_start_bsize - 1);

    if (flag_debug) {
        printf("saddr=%x sbsize=%x\n", flash_start_addr, flash_start_bsize);
        printf("eaddr=%x ebsize=%x\n", flash_end_addr, flash_end_bsize);
    }

    printf("Erase bank=%u addr=%x len=%x\n", bank, addr, len);
    if ((!flag_yes) && (!are_you_sure("Proceed"))) {
        return (1);
    }

    dot_max = (len + MAX_CHUNK - 1) / MAX_CHUNK;
    while (dot_max > 50) {
        dot_max >>= 1;
        dot_iters <<= 1;
    }
    printf("Progress [%*s]\rProgress [", dot_max, "");
    fflush(stdout);

    time_start = smash_time();

    bank += addr / ROM_WINDOW_SIZE;
    addr &= (ROM_WINDOW_SIZE - 1);

    while (len > 0) {
        uint xlen = get_flash_bsize(cb, bank * ROM_WINDOW_SIZE + addr);

        rc = erase_flash_block(bank, addr);
        if (rc != 0) {
            printf("\nKicksmash failure %d (%s)\n", rc, smash_err(rc));
            break;
        }
        if (is_user_abort()) {
            printf("^C\n");
            rc = 2;
            break;
        }

        tlen += xlen;
        len  -= xlen;
        addr += xlen;
        if (addr >= ROM_WINDOW_SIZE) {
            addr -= ROM_WINDOW_SIZE;
            bank++;
        }
        if (tlen >= MAX_CHUNK) {
            while (tlen >= MAX_CHUNK) {
                tlen -= MAX_CHUNK;
                if (++dot_count == dot_iters) {
                    dot_count = 0;
                    printf(".");
                }
            }
            fflush(stdout);
        }
    }
    if (rc == 0) {
        if ((tlen > 0) && (++dot_count == dot_iters))
            printf(".");
        printf("]\n");
        time_end = smash_time();
        printf("Erase complete in ");
        print_us_diff(time_start, time_end);
    }
    return (rc);
}

static int
OpenTimer(void)
{
    int rc;

    rc = OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ,
                    (struct IORequest *) &TimeRequest, 0L);
    if (rc != 0) {
        printf("Timer open failed\n");
        return (rc);
    }

    TimerBase = (struct Device *) TimeRequest.tr_node.io_Device;
    return (0);
}

static void
CloseTimer(void)
{
    CloseDevice((struct IORequest *) &TimeRequest);
    TimerBase = NULL;
}

static void
setsystime(uint sec, uint usec)
{
    TimeRequest.tr_node.io_Command = TR_SETSYSTIME;
    TimeRequest.tr_time.tv_secs = sec;
    TimeRequest.tr_time.tv_micro = usec;
    DoIO((struct IORequest *) &TimeRequest);
}

static uint
getsystime(uint *usec)
{
    TimeRequest.tr_node.io_Command = TR_GETSYSTIME;
    DoIO((struct IORequest *) &TimeRequest);
    *usec = TimeRequest.tr_time.tv_micro;
    return (TimeRequest.tr_time.tv_secs);
}

static void
showdatestamp(struct DateStamp *ds, uint usec)
{
    struct DateTime  dtime;
    char             datebuf[32];
    char             timebuf[32];

    dtime.dat_Stamp.ds_Days   = ds->ds_Days;
    dtime.dat_Stamp.ds_Minute = ds->ds_Minute;
    dtime.dat_Stamp.ds_Tick   = ds->ds_Tick;
    dtime.dat_Format          = FORMAT_DOS;
    dtime.dat_Flags           = 0x0;
    dtime.dat_StrDay          = NULL;
    dtime.dat_StrDate         = datebuf;
    dtime.dat_StrTime         = timebuf;
    DateToStr(&dtime);
    printf("%s %s.%06u\n", datebuf, timebuf, usec);
}

static void
showsystime(uint sec, uint usec)
{
    struct DateStamp ds;
    uint min  = sec / 60;
    uint day  = min / (24 * 60);

    ds.ds_Days   = day;
    ds.ds_Minute = min % (24 * 60);
    ds.ds_Tick   = (sec % 60) * TICKS_PER_SECOND;
    showdatestamp(&ds, usec);
}

uint
get_ks_clock(uint *sec, uint *usec)
{
    uint ks_clock[2];
    uint rc;
    uint rlen;

    rc = send_cmd(KS_CMD_CLOCK, NULL, 0, &ks_clock, sizeof (ks_clock), &rlen);
    if (rc != 0) {
        printf("Get clock failed: %d (%s)\n", rc, smash_err(rc));
        if (flag_debug)
            dump_memory(clock, sizeof (clock), DUMP_VALUE_UNASSIGNED);
        *sec = 0;
        *usec = 0;
    } else {
        *sec  = ks_clock[0];
        *usec = ks_clock[1];
    }
    return (rc);
}

uint
set_ks_clock(uint sec, uint usec, uint flags)
{
    uint cmd = KS_CMD_CLOCK | (flags ? KS_CLOCK_SET_IFNOT : KS_CLOCK_SET);
    uint ks_clock[2];
    uint rc;

    ks_clock[0] = sec;
    ks_clock[1] = usec;

    rc = send_cmd(cmd, ks_clock, sizeof (ks_clock), NULL, 0, NULL);
    if (rc != 0)
        printf("Set clock failed: %d (%s)\n", rc, smash_err(rc));
    return (rc);
}

static int
cmd_clock(int argc, char *argv[])
{
    int         arg;
    uint        rc = 1;
    uint        flag_load = 0;
    uint        flag_load_if_set = 0;
    uint        flag_save = 0;
    uint        flag_save_if_not_set = 0;
    uint        flag_show = 0;
    uint        sec;
    uint        usec;
    const char *ptr;
    for (arg = 1; arg < argc; ) {
        ptr = long_to_short(argv[arg++], long_to_short_clock,
                            ARRAY_SIZE(long_to_short_clock));
        if (*ptr == '-') {
            for (++ptr; *ptr != '\0'; ptr++) {
                switch (*ptr) {
                    case 'h':  // load
                        goto usage;
                    case 'l':  // load
                        flag_load++;
                        break;
                    case 'L':  // load if set
                        flag_load_if_set++;
                        break;
                    case 'n':  // save if not set
                        flag_save_if_not_set++;
                        break;
                    case 's':  // save
                        flag_save++;
                        break;
                    case 'S':  // show
                        flag_show++;
                        break;
                    default:
                        printf("Unknown argument %s \"-%s\"\n",
                               argv[0], ptr);
                        goto usage;
                }
            }
        } else {
            printf("Unknown argument %s \"%s\"\n",
                   argv[0], ptr);
usage:
            printf("%s", cmd_clock_options);
            return (rc);
        }
    }
    if ((flag_load == 0) && (flag_save == 0) && (flag_save_if_not_set == 0))
        flag_show++;

    if (flag_load || flag_load_if_set) {
        if ((rc = get_ks_clock(&sec, &usec)) != 0)
            return (rc);
        if ((sec == 0) && (usec == 0)) {
            if (flag_load_if_set)
                return (0);
            printf("KS does not know the current time\n");
            return (1);
        }
        showsystime(sec, usec);
        if (OpenTimer())
            return (1);
        setsystime(sec, usec);
        CloseTimer();
    }
    if (flag_save || flag_save_if_not_set) {
        if (OpenTimer())
            return (1);
        sec = getsystime(&usec);
        CloseTimer();
        showsystime(sec, usec);
        if ((rc = set_ks_clock(sec, usec, flag_save_if_not_set)) != 0)
            return (rc);
    }
    if (flag_show) {
        if ((rc = get_ks_clock(&sec, &usec)) != 0)
            return (rc);
        if ((sec == 0) && (usec == 0)) {
            printf("KS does not know the current time\n");
            return (1);
        }
        showsystime(sec, usec);
    }
    return (0);
}

int
main(int argc, char *argv[])
{
    int      arg;
    uint     loop;
    uint     loops = 1;
    uint     flag_inquiry = 0;
    uint     flag_test = 0;
    uint     flag_test_mask = 0;
    uint     flag_x_spin = 0;
    uint     flag_y_spin = 0;
    int      pos;
    uint     errs = 0;
    uint     do_multiple = 0;
    uint32_t addr;

#ifndef _DCC
    SysBase = *(struct ExecBase **)4UL;
#endif
    cpu_control_init();  // cpu_type, SysBase

    for (arg = 1; arg < argc; arg++) {
        const char *ptr;
        ptr = long_to_short(argv[arg], long_to_short_main,
                            ARRAY_SIZE(long_to_short_main));
        if (*ptr == '-') {
            for (++ptr; *ptr != '\0'; ptr++) {
                switch (*ptr) {
                    case 'b':  // bank
                        exit(cmd_bank(argc - arg, argv + arg));
                    case 'c':  // clock
                        exit(cmd_clock(argc - arg, argv + arg));
                    case 'd':  // debug
                        flag_debug++;
                        break;
                    case 'e':  // erase flash
                        exit(cmd_erase(argc - arg, argv + arg));
                        break;
                    case 'i':  // inquiry
                        flag_inquiry++;
                        break;
                    case 'l':  // loop count
                        if (++arg >= argc) {
                            printf("%s requires an argument\n", ptr);
                            exit(1);
                        }
                        loops = atoi(argv[arg]);
                        break;
                    case 'q':  // quiet
                        flag_quiet++;
                        break;
                    case 'r':  // read flash to file
                        exit(cmd_readwrite(argc - arg, argv + arg));
                        break;
                    case 's':  // spin
                        spin(MEM_LOOPS);
                        exit(0);
                    case '0':  // pattern test
                    case '1':  // loopback test
                    case '2':  // loopback perf test
                    case '3':  // Message buffer loopback test
                    case '4':  // Remote USB host message loopback test
                        flag_test_mask |= BIT(*ptr - '0');
                        flag_test++;
                        break;
                    case 't':  // test
                        flag_test++;
                        break;
                    case 'v':  // verify file with flash
                        exit(cmd_readwrite(argc - arg, argv + arg));
                        break;
                    case 'w':  // write file to flash
                        exit(cmd_readwrite(argc - arg, argv + arg));
                        break;
                    case 'x':  // spin reading address
                        if (++arg >= argc) {
                            printf("%s requires an argument\n", ptr);
                            exit(1);
                        }

                        pos = 0;
                        if ((sscanf(argv[arg], "%x%n", &addr, &pos) != 1) ||
                            (pos == 0)) {
                            printf("Invalid argument \"%s\" for -%c\n",
                                   argv[arg], *ptr);
                            exit(1);
                        }
                        flag_x_spin = 1;
                        break;
                    case 'y':  // spin reading address with ROM overlay set
                        if (++arg >= argc) {
                            printf("%s requires an argument\n", ptr);
                            exit(1);
                        }

                        pos = 0;
                        if ((sscanf(argv[arg], "%x%n", &addr, &pos) != 1) ||
                            (pos == 0)) {
                            printf("Invalid argument \"%s\" for -%c\n",
                                   argv[arg], *ptr);
                            exit(1);
                        }
                        flag_y_spin = 1;
                        break;
                    default:
                        printf("Unknown argument %s\n", ptr);
                        usage();
                        exit(1);
                }
            }
        } else if ((*ptr >= '0') && (*ptr < '4') && (ptr[1] == '\0')) {
            flag_test_mask |= BIT(*ptr - '0');
            flag_test++;
        } else {
            printf("Error: unknown argument %s\n", ptr);
            usage();
            exit(1);
        }
    }

    if ((flag_inquiry | flag_test | flag_x_spin | flag_y_spin) == 0) {
        printf("You must specify an operation to perform\n");
        usage();
        exit(1);
    }
    if (flag_test_mask == 0)
        flag_test_mask = ~0;
    if (flag_test) {
        srand32(time(NULL));
        test_loopback_buf = AllocMem(TEST_LOOPBACK_BUF * 2, MEMF_PUBLIC);
        if (flag_test_mask & (flag_test_mask - 1))
            do_multiple = 1;
    }
    if (flag_inquiry & (flag_inquiry - 1))
        do_multiple = 1;
    if (!!flag_test + !!flag_inquiry + !!flag_x_spin + !!flag_y_spin > 1)
        do_multiple = 1;

    for (loop = 0; loop < loops; loop++) {
        if (loops > 1) {
            if (flag_quiet) {
                if ((loop & 0xff) == 0) {
                    printf(".");
                    fflush(stdout);
                }
            } else {
                printf("Pass %-4u ", loop + 1);
                if (do_multiple)
                    printf("\n");
            }
        }
        if (flag_x_spin) {
            spin_memory(addr);
        }
        if (flag_y_spin) {
            spin_memory_ovl(addr);
        }
        if (flag_inquiry) {
            if ((flag_inquiry & 1) &&
                smash_identify() && ((loops == 1) || (loop > 1))) {
                errs++;
                break;
            }
            if ((flag_inquiry & 2) &&
                flash_show_id() && ((loops == 1) || (loop > 1))) {
                errs++;
                break;
            }
        }
        if (flag_test) {
            if (smash_test(flag_test_mask) && ((loops == 1) || (loop > 1))) {
                errs++;
                break;
            }
        }
        if (is_user_abort()) {
            printf("^C Abort\n");
            goto end_now;
        }
    }
    if (loop < loops) {
        printf("Failed");
end_now:
        if (loops > 1)
            printf(" at pass %u", loop + 1);
        if (errs != 0)
            printf(" (%u errors)", errs);
        printf("\n");
    } else if (flag_quiet && (errs == 0)) {
        printf("Pass %u done\n", loop);
    }
    if (test_loopback_buf != NULL)
        FreeMem(test_loopback_buf, TEST_LOOPBACK_BUF * 2);

    exit(errs ? EXIT_FAILURE : EXIT_SUCCESS);
}
