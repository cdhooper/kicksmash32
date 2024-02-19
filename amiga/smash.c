/*
 * smash
 * ------
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
const char *version = "\0$VER: smash 0.1 ("__DATE__") © Chris Hooper";

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <exec/types.h>
#include <devices/trackdisk.h>
#include <devices/scsidisk.h>
#include <libraries/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <clib/exec_protos.h>
#include <clib/alib_protos.h>
#include <clib/dos_protos.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <libraries/configregs.h>
#include "smash_cmd.h"
#include "crc32.h"

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
typedef struct { unsigned long hi; unsigned long lo;} uint64_t;
#define __packed
#else
#include <devices/cd.h>
#include <inline/timer.h>
#include <inline/exec.h>
#include <inline/dos.h>
#include <inttypes.h>
struct ExecBase *SysBase;
struct ExecBase *DOSBase;
struct Device   *TimerBase;
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

#define ADDR8(x)    ((volatile uint8_t *)  ((uintptr_t)(x)))
#define ADDR16(x)   ((volatile uint16_t *) ((uintptr_t)(x)))
#define ADDR32(x)   ((volatile uint32_t *) ((uintptr_t)(x)))

#define SUPERVISOR_STATE_ENTER()    { \
                                      APTR old_stack = SuperState()
#define SUPERVISOR_STATE_EXIT()       UserState(old_stack); \
                                    }
#define CACHE_DISABLE_DATA() \
        { \
            uint32_t oldcachestate = \
            CacheControl(0L, CACRF_EnableD) & (CACRF_EnableD | CACRF_DBE)
#define CACHE_RESTORE_STATE() \
            CacheControl(oldcachestate, CACRF_EnableD | CACRF_DBE); \
        }

/* MMU_DISABLE() and MMU_RESTORE() must be called from Supervisor state */
#define MMU_DISABLE() \
        { \
            uint32_t oldmmustate = 0; \
            if (cpu_type == 68030) { \
                oldmmustate = mmu_get_tc_030(); \
                mmu_set_tc_030(oldmmustate & ~BIT(31)); \
            } else if ((cpu_type == 68040) || (cpu_type == 68060)) { \
                oldmmustate = mmu_get_tc_040(); \
                mmu_set_tc_040(oldmmustate & ~BIT(15)); \
            }
#define MMU_RESTORE() \
            if (cpu_type == 68030) { \
                mmu_set_tc_030(oldmmustate); \
            } else if ((cpu_type == 68040) || (cpu_type == 68060)) { \
                mmu_set_tc_040(oldmmustate); \
            } \
        }

#define INTERRUPTS_DISABLE() if (irq_disabled++ == 0) \
                                 Disable()  /* Disable interrupts */
#define INTERRUPTS_ENABLE()  if (--irq_disabled == 0) \
                                 Enable()   /* Enable Interrupts */

#define ROM_BASE         0x00f80000  /* Base address of Kickstart ROM */
#define AMIGA_PPORT_DIR  0x00bfe301  /* Amiga parallel port dir register */
#define AMIGA_PPORT_DATA 0x00bfe101  /* Amiga parallel port data reg. */

#define CIAA_PRA         ADDR8(0xbfe001)
#define CIAA_PRA_OVERLAY BIT(0)
#define CIAA_PRA_LED     BIT(1)

#define VALUE_UNASSIGNED 0xffffffff

#define MEM_LOOPS        1000000
#define ROM_WINDOW_SIZE  (512 << 10)  // 512 KB

static const char cmd_options[] =
    "usage: smash <options>\n"
    "   -b <opt>     ROM bank operations (?, show, ...)\n"
    "   -d           show debug output\n"
    "   -e <opt>     erase flash (?, bank, ...)\n"
    "   -i           identify Kicksmash and Flash parts\n"
    "   -r <opt>     read from flash (?, bank, file, ...)\n"
    "   -w <opt>     write to flash (?, bank, file, ...)\n"
    "   -x <addr>    spin loop reading addr\n"
    "   -y <addr>    spin loop reading addr with ROM OVL set\n"
    "   -t           do pattern test\n";

static const char cmd_read_options[] =
    "smash -r options\n"
    "   addr <hex>  - starting address (-a)\n"
    "   bank <num>  - flash bank on which to operate (-b)\n"
    "   dump        - save hex/ASCII instead of binary (-d)\n"
    "   file <name> - file where to save content (-f)\n"
    "   len <hex>   - length to read in bytes (-l)\n"
    "   swap <mode> - byte swap mode (1032, 2301, 3210) (-s)\n"
    "   yes         - skip prompt (-y)\n";

static const char cmd_write_options[] =
    "smash -w options\n"
    "   addr <hex>  - starting address (-a)\n"
    "   bank <num>  - flash bank on which to operate (-b)\n"
//  "   dump        - save hex/ASCII instead of binary (-d)\n"
    "   file <name> - file from which to read (-f)\n"
    "   len <hex>   - length to program in bytes (-l)\n"
    "   swap <mode> - byte swap mode (1032, 2301, 3210) (-s)\n"
    "   yes         - skip prompt (-y)\n";

static const char cmd_erase_options[] =
    "smash -e options\n"
    "   addr <hex>  - starting address (-a)\n"
    "   bank <num>  - flash bank on which to operate (-b)\n"
    "   len <hex>   - length to erase in bytes (-l)\n"
    "   yes         - skip prompt (-y)\n";

uint32_t mmu_get_type(void);
uint32_t mmu_get_tc_030(void);
uint32_t mmu_get_tc_040(void);
// void     mmu_set_tc_030(uint32_t tc);
void     mmu_set_tc_040(uint32_t tc);

BOOL __check_abort_enabled = 0;       // Disable gcc clib2 ^C break handling
uint irq_disabled = 0;
uint flag_debug = 0;
uint flag_quiet = 0;
static uint cpu_type = 0;

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

static char
printable_ascii(uint8_t ch)
{
    if (ch >= ' ' && ch <= '~')
        return (ch);
    if (ch == '\t' || ch == '\r' || ch == '\n' || ch == '\0')
        return (' ');
    return ('.');
}

static void
dump_memory(uint32_t *src, uint len, uint dump_base)
{
    uint pos;
    uint strpos;
    char str[20];
    len = (len + 3) / 4;
    if (dump_base != VALUE_UNASSIGNED)
        printf("%05x:", dump_base);
    for (strpos = 0, pos = 0; pos < len; pos++) {
        uint32_t val = src[pos];
        printf(" %08x", val);
        str[strpos++] = printable_ascii(val >> 24);
        str[strpos++] = printable_ascii(val >> 16);
        str[strpos++] = printable_ascii(val >> 8);
        str[strpos++] = printable_ascii(val);
        if ((pos & 3) == 3) {
            str[strpos] = '\0';
            strpos = 0;
            printf(" %s\n", str);
            if ((dump_base != VALUE_UNASSIGNED) && ((pos + 1) < len)) {
                printf("%05x:", dump_base);
                dump_base += 16;
            }
        }
    }
    if ((pos & 3) != 0) {
        str[strpos] = '\0';
        printf("%*s%s\n", (4 - (pos & 3)) * 5, "", str);
    }
}

uint
get_cpu(void)
{
    UWORD attnflags = SysBase->AttnFlags;

    if (attnflags & 0x80)
        return (68060);
    if (attnflags & AFF_68040)
        return (68040);
    if (attnflags & AFF_68030)
        return (68030);
    if (attnflags & AFF_68020)
        return (68020);
    if (attnflags & AFF_68010)
        return (68010);
    return (68000);
}

/*
 * mmu_get_tc_030
 * --------------
 * This function only works on the 68030.
 * 68040 and 68060 have different MMU instructions.
 */

__attribute__ ((noinline))
uint32_t
mmu_get_tc_030(void)
{
    register uint32_t result;
    __asm__ __volatile__("subq.l #4,sp \n\t"
                         ".long 0xf0174200 \n\t"  // pmove tc,(sp)
                         "move.l (sp)+,d0"
                         : "=d" (result));
    return (result);
}

/*
 * mmu_set_tc_030
 * --------------
 * This function only works on the 68030.
 */
__attribute__ ((noinline))
void mmu_set_tc_030(register uint32_t tc asm("%d0"))
{
#if 0
    __asm__ __volatile__("adda.l #4,sp \n\t"
                         ".long 0xf0174000 \n\t"  // pmove.l (sp),tc
                         "suba.l #4,sp"
                         : "=d" (tc));
#elif 1
    __asm__ __volatile__("move.l %0,-(sp) \n\t"
                         ".long 0xf0174000 \n\t"  // pmove.l (sp),tc
                         "adda.l #4,sp \n\t"
                         : "=d" (tc));
#else
    __asm__ __volatile__(".long 0xf0174000 \n\t"  // pmove.l (sp),tc
                         : "=d" (tc));
#endif
}

/*
 * mmu_get_tc_040
 * --------------
 * This function only works on 68040 and 68060.
 */
uint32_t
mmu_get_tc_040(void)
{
    register uint32_t result;
    __asm__ __volatile__(".long 0x4e7a0003"  // movec.l tc,d0
                         : "=d" (result));
    return (result);
}

/*
 * mmu_set_tc_040
 * --------------
 * This function only works on 68040 and 68060.
 */
void mmu_set_tc_040(uint32_t tc)
{
    __asm__ __volatile__("move.l 4(sp),d0 \n\t"
                         ".long 0x4e7b0003 \n\t"  // movec.l d0,tc
                         : "=d" (tc));
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


/* Status codes from local message handling */
#define MSG_STATUS_SUCCESS    0           // No error
#define MSG_STATUS_NO_REPLY   0xfffffff9  // Did not get reply from Kicksmash
#define MSG_STATUS_BAD_LENGTH 0xfffffff8  // Bad length detected
#define MSG_STATUS_BAD_CRC    0xfffffff7  // CRC failure detected
#define MSG_STATUS_BAD_DATA   0xfffffff6  // Invalid data
#define MSG_STATUS_PRG_TMOUT  0xfffffff5  // Programming timeout
#define MSG_STATUS_PRG_FAIL   0xfffffff4  // Programming failure

static const char *
smash_err(uint code)
{
    switch (code) {
        case KS_STATUS_OK:
            return ("Success");
        case KS_STATUS_FAIL:
            return ("KS Failure");
        case KS_STATUS_BADARG:
            return ("KS reports bad command argument");
        case KS_STATUS_BADLEN:
            return ("KS reports bad length");
        case KS_STATUS_CRC:
            return ("KS reports CRC bad");
        case KS_STATUS_UNKCMD:
            return ("KS detected unknown command");
        case MSG_STATUS_NO_REPLY:
            return ("No Reply");
        case MSG_STATUS_BAD_LENGTH:
            return ("Smash detected bad length");
        case MSG_STATUS_BAD_CRC:
            return ("Smash detected bad CRC");
        case MSG_STATUS_BAD_DATA:
            return ("Invalid data");
        default:
            return ("Unknown");
    }
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


static const uint16_t sm_magic[] = { 0x0117, 0x0119, 0x1017, 0x0204 };
uint smash_cmd_shift = 2;

/*
 * send_cmd_core
 * -------------
 * This function assumes interrupts and cache are already disabled
 * by the caller.
 */
uint
send_cmd_core(uint16_t cmd, void *arg, uint16_t arglen,
              void *reply, uint replymax, uint *replyalen)
{
    uint      pos;
    uint32_t  crc;
    uint32_t  replycrc = 0;
    uint16_t *replybuf = (uint16_t *) reply;
    uint      word = 0;
    uint      magic = 0;
    uint      replylen = 0;
    uint      replystatus = 0;
    uint16_t *argbuf = arg;
    uint16_t  val;
    uint32_t  val32 = 0;

    for (pos = 0; pos < ARRAY_SIZE(sm_magic); pos++)
        (void) *ADDR32(ROM_BASE + (sm_magic[pos] << smash_cmd_shift));

    (void) *ADDR32(ROM_BASE + (arglen << smash_cmd_shift));
    crc = crc32(0, &arglen, sizeof (arglen));
    crc = crc32(crc, &cmd, sizeof (cmd));
    (void) *ADDR32(ROM_BASE + (cmd << smash_cmd_shift));

    for (pos = 0; pos < arglen / sizeof (uint16_t); pos++) {
        (void) *ADDR32(ROM_BASE + (argbuf[pos] << smash_cmd_shift));
        crc = crc32(crc, &argbuf[pos], 2);
    }
    if (arglen & 1) {
        /* Odd byte at end */
        (void) *ADDR32(ROM_BASE + (argbuf[pos] << (smash_cmd_shift + 8)));
        crc = crc32(crc, &argbuf[pos], 1);
    }

    /* CRC high and low words */
    (void) *ADDR32(ROM_BASE + ((crc >> 16) << smash_cmd_shift));
    (void) *ADDR32(ROM_BASE + ((crc & 0xffff) << smash_cmd_shift));

    /*
     * Delay to prevent reads before Kicksmash has set up DMA hardware
     * with the data to send. This is necessary so that the two DMA
     * engines on 32-bit Amigas are started in a synchronized manner.
     * Might need more spin iterations on a faster CPU, or maybe
     * a real timer delay here.
     *
     * A3000 68030: 180 spins minimum
     */
    spin(300);

    /*
     * Find reply magic, length, and status.
     *
     * The below code must handle both a 32-bit reply and a 16-bit reply
     * where data begins in the lower 16 bits.
     *
     *            hi16bits lo16bits hi16bits lo16bits hi16bits lo16bits
     * Example 1: 0x0119   0x0117   0x0204   0x1017   len      status
     * Example 2: ?        0x0119   0x0117   0x0204   0x1017   len
     */
#define WAIT_FOR_MAGIC_LOOPS 32
    for (word = 0; word < WAIT_FOR_MAGIC_LOOPS; word++) {
        if (word & 1) {
            val = (uint16_t) val32;
        } else {
            val32 = *ADDR32(ROM_BASE + 0x1554); // remote addr 0x0555 or 0x0aaa
//          *ADDR32(0x7770000 + word * 2) = val;
            val = val32 >> 16;
        }
        if (flag_debug && (replybuf != NULL) && (word < (replymax / 2))) {
            replybuf[word] = val;  // Just for debug on failure (-d flag)
        }

        if (magic < ARRAY_SIZE(sm_magic)) {
            if (val != sm_magic[magic]) {
                magic = 0;
                spin(20);
                continue;
            }
        } else if (magic < ARRAY_SIZE(sm_magic) + 1) {
            replylen = val;     // Reply length
            crc = crc32(0, &val, sizeof (val));
        } else if (magic < ARRAY_SIZE(sm_magic) + 2) {
            replystatus = val;  // Reply status
            crc = crc32(crc, &val, sizeof (val));
            word++;
            break;
        }
        magic++;
    }

    if (word >= WAIT_FOR_MAGIC_LOOPS) {
        replystatus = MSG_STATUS_NO_REPLY;
        if (replyalen != NULL) {
            *replyalen = word * 2;
            if (*replyalen > replymax)
                *replyalen = replymax;
        }
        /* Ensure Kicksmash firmware has returned ROM to normal state */
        for (pos = 0; pos < 1000; pos++)
            (void) *ADDR32(ROM_BASE + 0x15554); // remote addr 0x5555 or 0xaaaa
        spin(500000);
        goto scc_cleanup;
    }

    if (replyalen != NULL)
        *replyalen = replylen;

    replylen = (replylen + 1) & ~1;  // Round up reply length to word

    if (replylen > replymax) {
        if (replystatus == 0)
            replystatus = MSG_STATUS_BAD_LENGTH;
        if (replyalen != NULL) {
            *replyalen = word * 2;
            if (*replyalen > replymax)
                *replyalen = replymax;
        }
        goto scc_cleanup;
    }

    replylen /= 2;  // Round up to 16-bit word count

    /* Response is valid so far. Read data */
    for (pos = 0; pos < replylen; pos++, word++) {
        if (word & 1) {
            val = (uint16_t) val32;
        } else {
            val32 = *ADDR32(ROM_BASE);
            val = val32 >> 16;
#if 0
            if (cmd == KS_CMD_FLASH_ID) {
                *ADDR32(0x7780000 + pos * 2) = val32;
                (*ADDR32(0x7790000))++;
            }
#endif
        }
        if (replybuf != NULL)
            *(replybuf++) = val;
    }

    /* Read CRC */
    if (word & 1) {
        replycrc = (val32 << 16) | *ADDR16(ROM_BASE);
    } else {
        replycrc = *ADDR32(ROM_BASE);
    }

scc_cleanup:
    if (replystatus != 0) {
        /* Ensure Kicksmash firmware has returned ROM to normal state */
        for (pos = 0; pos < 100; pos++)
            (void) *ADDR32(ROM_BASE + 0x15554); // remote addr 0x5555 or 0xaaaa
        spin(10000);
        spin(500000);  // Kicksmash might emit debug output
    }
    if ((replystatus < 0x10000) && (replystatus != KS_STATUS_CRC)) {
        crc = crc32(crc, reply, replylen * 2);
        if (crc != replycrc)
            return (MSG_STATUS_BAD_CRC);
    }

    return (replystatus);
}

/*
 * send_cmd
 * --------
 * Sends a command to the STM32 ARM CPU running on Kicksmash.
 * All messages are protected by CRC. Message format:
 *     Magic (64 bits)
 *        0x0117, 0x0119, 0x1017, 0x0204
 *     Length (16 bits)
 *        The length specifies the number of payload bytes (not including
 *        magic, length, command, or CRC bytes at end). This number may be
 *        zero (0) if only a command is present.
 *     Command or status code (16 bits)
 *        KS_CMD_*
 *     Additional data (if any)
 *     CRC (32 bits)
 *        CRC is over all content except magic (includes length and command)
 */
uint
send_cmd(uint16_t cmd, void *arg, uint16_t arglen,
         void *reply, uint replymax, uint *replyalen)
{
    uint rc;
    SUPERVISOR_STATE_ENTER();
    INTERRUPTS_DISABLE();
    CACHE_DISABLE_DATA();
    MMU_DISABLE();

    rc = send_cmd_core(cmd, arg, arglen, reply, replymax, replyalen);

    MMU_RESTORE();
    CACHE_RESTORE_STATE();
    INTERRUPTS_ENABLE();
    SUPERVISOR_STATE_EXIT();
    return (rc);
}

#if 0
static uint
smash_nop(void)
{
    return (send_cmd(KS_CMD_NOP, NULL, 0, NULL, 0, NULL));
}
#endif

#if 0
static uint
smash_identify(void)
{
    uint32_t reply_buf[30];
    uint rlen;
    uint rc;
    memset(reply_buf, 0, sizeof (reply_buf));
    rc = send_cmd(KS_CMD_ID, NULL, 0, reply_buf, sizeof (reply_buf), &rlen);

    if (rc != 0) {
        printf("Reply message failure: %d (%s)\n", (int) rc, smash_err(rc));
        if (flag_debug)
            dump_memory(reply_buf, rlen, VALUE_UNASSIGNED);
        return (rc);
    }
    printf("ID\n");
    dump_memory(reply_buf, rlen, VALUE_UNASSIGNED);
    return (0);
}
#endif

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
smash_test(void)
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
            dump_memory(reply_buf, rlen, VALUE_UNASSIGNED);
        show_test_state("Test pattern", rc);
        return (rc);
    }

    for (start = 0; start < ARRAY_SIZE(reply_buf); start++)
        if (reply_buf[start] == test_pattern[0])  // Found first data
            break;

    if (start == ARRAY_SIZE(reply_buf)) {
        printf("No test pattern marker [%08x] in reply\n", test_pattern[0]);
fail:
        dump_memory(reply_buf, sizeof (reply_buf), VALUE_UNASSIGNED);
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
        dump_memory(reply_buf, sizeof (reply_buf), VALUE_UNASSIGNED);
    show_test_state("Test pattern", 0);
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
    uint32_t addrs[64];
    uint32_t data[64];
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

    *CIAA_PRA |= CIAA_PRA_OVERLAY;
    spin(500);
    for (pos = 0; pos < num_addr; pos++) {
//      uint32_t addr = ROM_BASE + ((addrs[pos] << smash_cmd_shift) & 0x7ffff);
        uint32_t addr = ((addrs[pos] << smash_cmd_shift) & 0x7ffff);
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

    *CIAA_PRA &= ~CIAA_PRA_OVERLAY;
    spin(200);

#if 0
    // below is for debug, to ensure all Kicksmash DMA is drained.
    for (pos = 0; pos < 10000; pos++) {
        (void) *ADDR32(ROM_BASE);
    }
#endif

flash_cmd_cleanup:
    if (rc == 0) {
        if (flag_debug && !irq_disabled) {
            printf("Flash sequence\n");
            for (pos = 0; pos < num_addr; pos++)
                printf("    %08x = %08x\n", addrs[pos], data[pos]);
        }
#if 0
    if (cmd == KS_CMD_FLASH_ID)
        for (pos = 0; pos < num_addr; pos++)
            *ADDR32(0x7780010 + pos * 4) = data[pos];
#endif
    } else {
        /* Attempt to drain data and wait for Kicksmash to enable flash */
        for (pos = 0; pos < 1000; pos++) {
            (void) *ADDR32(ROM_BASE);
        }
        spin(100000);
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

    SUPERVISOR_STATE_ENTER();
    INTERRUPTS_DISABLE();
    CACHE_DISABLE_DATA();
    MMU_DISABLE();

    rc1 = flash_cmd_core(KS_CMD_FLASH_ID, NULL, 0);
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
                dump_memory(data, sizeof (data) / 2, VALUE_UNASSIGNED);
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

    if (mode == 16) {
        printf("    %08x %s", flash_dev1, id1);
    } else {
        printf("    %08x %08x %s %s", flash_dev1, flash_dev2, id1, id2);
    }
    printf(" (%u-bit mode)\n", mode);
    if ((mode == 32) && (flash_dev1 != flash_dev2)) {
        printf("    Warning: flash device ids differ\n");
        rc = MSG_STATUS_NO_REPLY;
    }

#if 0
    if (flag_debug > 1) {
        uint pos;
        uint32_t *addrs = (uint32_t *)0x7780000;
        uint32_t *data = (uint32_t *)0x7780010;
        printf("Flash sequence\n");
        for (pos = 0; pos < 3; pos++)
            printf("    %08x = %08x\n", addrs[pos], data[pos]);
    }
#endif

    return (rc);
}

static int
set_rom_bank(uint mode, uint bank)
{
    int      rc;
    uint     rlen;
    uint16_t arg;
    uint32_t reply_buf[2];

    arg = mode | (bank << 8);
    rc = send_cmd(KS_CMD_ROMSEL, &arg, sizeof (arg), reply_buf, sizeof (reply_buf), &rlen);
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
    printf("Bank  Comment         Merge LongReset  PowerOn  Current  "
           "NextReset\n");
    for (bank = 0; bank < ROM_BANKS; bank++) {
        uint aspaces = 2;
        uint pos;
        uint banks_add = info.bi_merge[bank] >> 4;
        uint bank_sub  = info.bi_merge[bank] & 0xf;
        printf("%-5u %-15s ", bank, info.bi_desc[bank]);

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
    uint8_t  bank_start = 0;
    uint8_t  bank_end   = 0;
    uint     flag_bank_merge       = 0;
    uint     flag_bank_unmerge     = 0;
    uint     flag_bank_longreset   = 0;
    uint     flag_set_current_bank = 0;
    uint     flag_set_reset_bank   = 0;
    uint     flag_set_poweron_bank = 0;
    uint     flag_set_bank_comment = 0;
    uint     bank;
    uint     rc;
    uint     opt = 0;
    uint16_t arg;

    if (argc < 2) {
        printf("-b requires an argument\n");
        printf("One of: ?, show, comment, longreset, nextreset, "
               "poweron, merge, unmerge\n");
        return (1);
    }
    if (strcmp(argv[1], "?") == 0) {
        printf("  show                       Display all ROM bank information\n"
               "  merge <start> <end>        Merge banks for larger ROMs\n"
               "  unmerge <start> <end>      Unmerge banks\n"
               "  longreset <bank>[,<bank>]  Banks to sequence at long reset\n"
               "  poweron <bank> [reboot]    Default bank at poweron\n"
               "  current <bank> [reboot]    Force new bank immediately\n"
               "  nextreset <bank> [reboot]  Force new bank at next reset\n"
               "  comment <bank> <text>      Add bank description comment\n");
    } else if (strcmp(argv[1], "show") == 0) {
        rom_bank_show();
        return (0);
    } else if (strcmp(argv[1], "current") == 0) {
        flag_set_current_bank++;
        opt = KS_BANK_SETCURRENT;
    } else if (strcmp(argv[1], "poweron") == 0) {
        flag_set_poweron_bank++;
        opt = KS_BANK_SETPOWERON;
    } else if (strcmp(argv[1], "longreset") == 0) {
        flag_bank_longreset++;
        opt = KS_CMD_BANK_LRESET;
    } else if (strcmp(argv[1], "nextreset") == 0) {
        flag_set_reset_bank++;
        opt = KS_BANK_SETRESET;
    } else if (strcmp(argv[1], "merge") == 0) {
        flag_bank_merge++;
    } else if (strcmp(argv[1], "unmerge") == 0) {
        flag_bank_unmerge++;
        opt = KS_BANK_UNMERGE;
    } else if (strcmp(argv[1], "comment") == 0) {
        flag_set_bank_comment++;
    } else {
        printf("Unknown argument -b '%s'\n", argv[1]);
        return (1);
    }
    if (flag_set_bank_comment) {
        char argbuf[64];
        if (argc != 4) {
            printf("-b %s requires a <bank> number and \"comment text\"\n",
                   argv[1]);
            return (1);
        }
        bank = atoi(argv[2]);
        if (bank >= ROM_BANKS) {
            printf("Bank %u is invalid (maximum bank is %u)\n",
                   bank, ROM_BANKS - 1);
            return (1);
        }
        arg = bank;
        memcpy(argbuf, &arg, 2);
        strncpy(argbuf + 2, argv[3], sizeof (argbuf) - 3);
        argbuf[sizeof (argbuf) - 1] = '\0';
        rc = send_cmd(KS_CMD_BANK_COMMENT, argbuf,
                      strlen(argbuf + 2) + 3, NULL, 0, NULL);
        if (rc != 0)
            printf("Bank comment set failed: %d %s\n", rc, smash_err(rc));
        return (rc);
    }
    if (flag_bank_longreset) {
        bank_info_t info;
        uint8_t     banks[ROM_BANKS];
        uint        cur;
        uint        rlen;

        rc = send_cmd(KS_CMD_BANK_INFO, NULL, 0, &info, sizeof (info), &rlen);
        if (rc != 0) {
            printf("Failed to get bank information: %d %s\n",
                   rc, smash_err(rc));
            return (rc);
        }

        rc = 0;
        for (cur = 0; cur < ROM_BANKS; cur++) {
            if (cur + 2 < (uint) argc) {
                uint sub;
                bank = atoi(argv[cur + 2]);
                if (bank >= ROM_BANKS) {
                    printf("Bank %u is invalid (maximum bank is %u)\n",
                           bank, ROM_BANKS - 1);
                    rc++;
                    continue;
                }
                sub = info.bi_merge[bank] & 0x0f;
                if (sub != 0) {
                    printf("Bank %u is part of a merged bank, but is not the "
                           "first (use %u)\n", bank, bank - sub);
                    rc++;
                }
                banks[cur] = bank;
            } else {
                banks[cur] = 0xff;
            }
        }
        if (rc != 0)
            return (rc);

        rc = send_cmd(KS_CMD_BANK_LRESET, &banks, sizeof (banks),
                      NULL, 0, NULL);
        if (rc != 0)
            printf("Bank longreset failed: %d %s\n", rc, smash_err(rc));
        return (rc);
    }
    if (flag_set_current_bank || flag_set_poweron_bank || flag_set_reset_bank) {
        if ((argc != 3) && (argc != 4)) {
            printf("-b %s requires a <bank> number to set\n", argv[1]);
            return (1);
        }
        if (argc == 4) {
            if (strcmp(argv[3], "reboot") == 0) {
                opt |= KS_BANK_REBOOT;
            } else {
                printf("-b %s only accepts \"reboot\" as an option "
                       "after bank number\n", argv[1]);
                return (1);
            }
        }

        bank = atoi(argv[2]);
        if (bank >= ROM_BANKS) {
            printf("Bank %u is invalid (maximum bank is %u)\n",
                   bank, ROM_BANKS - 1);
            return (1);
        }

        arg = bank;
        rc = send_cmd(KS_CMD_BANK_SET | opt, &arg, sizeof (arg), NULL, 0, NULL);
        if (rc != 0)
            printf("Bank set failed: %d %s\n", rc, smash_err(rc));
        return (rc);
    }
    if (flag_bank_merge || flag_bank_unmerge) {
        bank_info_t info;
        uint        count;
        uint        rlen;
        if (argc != 4) {
            printf("-b %s requires <start> and <end> bank numbers (range)\n",
                   argv[1]);
            return (1);
        }
        bank_start = atoi(argv[2]);
        bank_end   = atoi(argv[3]);
        count = bank_end - bank_start + 1;
        if (bank_start > bank_end) {
            printf("bank %u is not less than end %u\n", bank_start, bank_end);
            return (1);
        }
        if (bank_end >= ROM_BANKS) {
            printf("Bank %u is invalid (maximum bank is %u)\n",
                   bank_end, ROM_BANKS - 1);
            return (1);
        }
        if ((count != 1) && (count != 2) && (count != 4) && (count != 8)) {
            printf("Bank sizes must be a power of 2 (1, 2, 4, or 8 banks)\n");
            return (1);
        }
        if ((count == 2) && (bank_start & 1)) {
            printf("Two-bank ranges must start with an even bank number "
                   "(0, 2, 4, or 6)\n");
            return (1);
        }
        if ((count == 4) && (bank_start != 0) && (bank_start != 4)) {
            printf("Four-bank ranges must start with either bank 0 or "
                   "bank 4\n");
            return (1);
        }
        if ((count == 8) && (bank_start != 0)) {
            printf("Eight-bank ranges must start with bank 0\n");
            return (1);
        }

        rc = send_cmd(KS_CMD_BANK_INFO, NULL, 0, &info, sizeof (info), &rlen);
        if (rc != 0) {
            printf("Failed to get bank information: %d %s\n",
                   rc, smash_err(rc));
            return (rc);
        }
        for (bank = bank_start; bank <= bank_end; bank++) {
            if (flag_bank_merge && (info.bi_merge[bank] != 0)) {
                uint banks = (info.bi_merge[bank] >> 4) + 1;
                printf("Bank %u is already part of a%s %u bank range\n",
                       bank, (banks == 8) ? "n" : "", banks);
                return (1);
            }
            if (flag_bank_unmerge && (info.bi_merge[bank] == 0)) {
                printf("Bank %u is not part of a bank range\n", bank);
                return (1);
            }
        }

        arg = bank_start | (bank_end << 8);
        rc = send_cmd(KS_CMD_BANK_MERGE | opt, &arg, sizeof (arg),
                      NULL, 0, NULL);
        if (rc != 0) {
            printf("Bank %smerge failed: %d %s\n",
                   (opt != 0) ? "un" : "", rc, smash_err(rc));
            return (rc);
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
    spin(10000);
#ifdef USE_OVERLAY
    local_memcpy(buf, (void *) (addr), len);
    *CIAA_PRA &= ~(CIAA_PRA_OVERLAY | CIAA_PRA_LED);
#else
    local_memcpy(buf, (void *) (ROM_BASE + addr), len);
#endif
    rc |= send_cmd_core(KS_CMD_BANK_SET | KS_BANK_UNSETTEMP,
                        &bankarg, sizeof (bankarg), NULL, 0, NULL);
    spin(10000);

    MMU_RESTORE();
    CACHE_RESTORE_STATE();
    INTERRUPTS_ENABLE();
    SUPERVISOR_STATE_EXIT();
    return (rc);
}

static int
wait_for_flash_done(uint timeout_usec)
{
    uint32_t status;
    uint32_t cstatus = 0;
    uint32_t lstatus = 0;
    uint     spins = timeout_usec * 10;
    uint     spin_count = 0;
    int      same_count = 0;
    int      see_fail_count = 0;

    while (spin_count < spins) {
        __asm__ __volatile__("nop");
        status = *ADDR32(ROM_BASE);

        cstatus = status;
        /* Filter out checking of status which is already done */
        if (((cstatus ^ lstatus) & 0x0000ffff) == 0)
            cstatus &= ~0x0000ffff;
        if (((cstatus ^ lstatus) & 0xffff0000) == 0)
            cstatus &= ~0xffff0000;

        if (status == lstatus) {
            if (same_count++ >= 1) {
                /* Same for 2 tries */
                return (0);
            }
        } else {
            same_count = 0;
            lstatus = status;
        }

        if (cstatus & (BIT(5) | BIT(5 + 16)))  // Program / erase failure
            if (see_fail_count++ > 5)
                break;
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
    spin(10000);

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
            rc = wait_for_flash_done(360);
            if (rc != 0)
                break;

            len  -= xlen;
            xbuf += xlen;
            addr += xlen;
        }
    }
    spin(10000);

    /* Restore flash to read mode */
    rc |= flash_cmd_core(KS_CMD_FLASH_READ, NULL, 0);
    spin(10000);

    /* Return to "current" flash bank */
    rc |= send_cmd_core(KS_CMD_BANK_SET | KS_BANK_UNSETTEMP,
                        &bankarg, sizeof (bankarg), NULL, 0, NULL);
    spin(10000);

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
    uint16_t bankarg = bank;

    SUPERVISOR_STATE_ENTER();
    INTERRUPTS_DISABLE();
    CACHE_DISABLE_DATA();
    MMU_DISABLE();

    /* Switch to flash bank to be programmed */
    rc = send_cmd_core(KS_CMD_BANK_SET | KS_BANK_SETTEMP,
                       &bankarg, sizeof (bankarg), NULL, 0, NULL);
    spin(10000);

    /* Send erase command */
    rc = flash_cmd_core(KS_CMD_FLASH_ERASE, NULL, 0);
    if (rc == 0) {
        *ADDR32(ROM_BASE + addr);  // Generate address for erase
        rc = wait_for_flash_done(10000);  // about 1 second
    }
    spin(10000);

    /* Restore flash to read mode */
    rc |= flash_cmd_core(KS_CMD_FLASH_READ, NULL, 0);
    spin(10000);

    /* Return to "current" flash bank */
    rc |= send_cmd_core(KS_CMD_BANK_SET | KS_BANK_UNSETTEMP,
                        &bankarg, sizeof (bankarg), NULL, 0, NULL);
    spin(10000);

    MMU_RESTORE();
    CACHE_RESTORE_STATE();
    INTERRUPTS_ENABLE();
    SUPERVISOR_STATE_EXIT();
    return (0);
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
    int         arg;
    int         pos;
    int         bytes;
    uint        flag_dump = 0;
    uint        flag_yes = 0;
    uint        addr = VALUE_UNASSIGNED;
    uint        bank = VALUE_UNASSIGNED;
    uint        len  = VALUE_UNASSIGNED;
    uint        file_is_stdio = 0;
    uint        rlen;
    uint        rc = 1;
    uint        bank_sub;
    uint        bank_size;
    FILE       *file;
    uint        swapmode = 0123;  // no swap
    uint        writemode = 0;
    uint8_t    *buf;

    if ((strcmp(argv[0], "-w") == 0) || (strcmp(argv[0], "write") == 0))
        writemode = 1;

    for (arg = 1; arg < argc; ) {
        ptr = argv[arg++];
        if ((strcmp(ptr, "?") == 0) || (strcmp(ptr, "help") == 0)) {
            rc = 0;
usage:
            if (writemode)
                printf("%s", cmd_write_options);
            else
                printf("%s", cmd_read_options);
            return (rc);
        } else if ((strncmp(ptr, "addr", 4) == 0) || (strcmp(ptr, "-a") == 0)) {
            if (arg + 1 > argc) {
                printf("smash %s %s requires an option\n", argv[0], ptr);
                goto usage;
            }
            pos = 0;
            if ((sscanf(argv[arg], "%x%n", &addr, &pos) != 1) || (pos == 0)) {
                printf("Invalid argument \"%s\" for %s %s\n",
                       argv[arg], argv[0], ptr);
                goto usage;
            }
            arg++;
        } else if ((strcmp(ptr, "bank") == 0) || (strcmp(ptr, "-b") == 0)) {
            if (arg + 1 > argc) {
                printf("smash %s %s requires an option\n", argv[0], ptr);
                goto usage;
            }
            pos = 0;
            if ((sscanf(argv[arg], "%x%n", &bank, &pos) != 1) || (pos == 0) ||
                (bank >= ROM_BANKS)) {
                printf("Invalid argument \"%s\" for %s %s\n",
                       argv[arg], argv[0], ptr);
                goto usage;
            }
            arg++;
        } else if ((strncmp(ptr, "deb", 3) == 0) || (strcmp(ptr, "-D") == 0)) {
            flag_debug++;
        } else if ((strcmp(ptr, "dump") == 0) || (strcmp(ptr, "-d") == 0)) {
            flag_dump++;
        } else if ((strcmp(ptr, "file") == 0) || (strcmp(ptr, "-f") == 0)) {
            if (arg + 1 > argc) {
                printf("smash %s %s requires an option\n", argv[0], ptr);
                goto usage;
            }
            filename = argv[arg];
            if (strcmp(filename, "-") == 0)
                file_is_stdio++;
            arg++;
        } else if ((strncmp(ptr, "len", 3) == 0) || (strcmp(ptr, "-l") == 0)) {
            if (arg + 1 > argc) {
                printf("smash %s %s requires an option\n", argv[0], ptr);
                goto usage;
            }
            pos = 0;
            if ((sscanf(argv[arg], "%x%n", &len, &pos) != 1) ||
                (pos == 0) || (argv[arg][pos] != '\0')) {
                printf("Invalid argument \"%s\" for %s %s\n",
                       argv[arg], argv[0], ptr);
                goto usage;
            }
            arg++;
        } else if ((strncmp(ptr, "swap", 3) == 0) || (strcmp(ptr, "-s") == 0)) {
            if (arg + 1 > argc) {
                printf("smash %s %s requires an option\n", argv[0], ptr);
                goto usage;
            }
            pos = 0;
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
            if ((sscanf(argv[arg], "%u%n", &swapmode, &pos) != 1) ||
                (pos == 0) || (argv[arg][pos] != '\0') ||
                ((swapmode != 0123) && (swapmode != 1032) &&
                 (swapmode != 2301) && (swapmode != 3210))) {
                printf("Invalid argument \"%s\" for %s %s\n",
                       argv[arg], argv[0], ptr);
                printf("Use 1032, 2301, or 3210\n");
                return (1);
            }
            arg++;
        } else if ((strncmp(ptr, "yes", 3) == 0) || (strcmp(ptr, "-y") == 0)) {
            flag_yes++;
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
            printf("You must supply a filename or - for stdout\n");
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
        if (writemode) {
            /* Get length from size of file */
            len = get_file_size(filename);
            if (len == VALUE_UNASSIGNED) {
                return (1);
            }
        } else {
            /* Get length from size of bank */
            len = bank_size;
        }
    } else if (len > bank_size) {
        printf("Specified length 0x%x is greater than bank size 0x%x\n",
               len, bank_size);
        return (1);
    } else if (addr + len > bank_size) {
        printf("Specified address + length 0x%x is overflows bank "
               "(size 0x%x)\n",
               addr + len, bank_size);
        return (1);
    }

    if (writemode && file_is_stdio) {
        printf("STDIO input not supported\n");
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
        printf("Read bank=%u addr=%x len=%x to ", bank, addr, len);
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

#define MAX_CHUNK (64 << 10)   // 64 KB
    buf = AllocMem(MAX_CHUNK, MEMF_PUBLIC);
    if (buf == NULL) {
        printf("Failed to allocate 0x%x bytes\n", MAX_CHUNK);
        return (1);
    }

    if (file_is_stdio) {
        file = stdout;
    } else {
        if (writemode) {
            file = fopen(filename, "r");
            if (file == NULL) {
                printf("Failed to open \"%s\", for read\n", filename);
                rc = 1;
                goto fail_end;
            }
        } else {
            file = fopen(filename, "w");
            if (file == NULL) {
                printf("Failed to open \"%s\", for write\n", filename);
                rc = 1;
                goto fail_end;
            }
        }
    }

    rc = 0;
    if (!file_is_stdio) {
        printf("Progress [%*s]\rProgress [",
               (len + MAX_CHUNK - 1) / MAX_CHUNK, "");
        fflush(stdout);
    }
    bank += addr / ROM_WINDOW_SIZE;
    addr &= (ROM_WINDOW_SIZE - 1);

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
                printf("\nFailed to read %u bytes from %s\n", xlen, filename);
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
    if (!file_is_stdio) {
        if (rc == 0)
            printf("]\n");
        fclose(file);
    }
fail_end:
    FreeMem(buf, MAX_CHUNK);
    return (rc);
}

static uint
get_flash_bsize(const chip_blocks_t *cb, uint flash_addr)
{
    uint flash_bsize = cb->cb_bsize << (10 + smash_cmd_shift);
    uint flash_bnum  = flash_addr / flash_bsize;
#undef ERASE_DEBUG
#ifdef ERASE_DEBUG
    printf("bbnum=%x flash_bsize=%x flash_addr=%x flash_bnum=%x\n",
           cb->cb_bbnum, flash_bsize, flash_addr, flash_bnum);
#endif
    if (flash_bnum == cb->cb_bbnum) {
        /*
         * Boot block has variable block size.
         *
         * This code does not find the actual block size. It finds the
         * size from the current location until the end of the block
         * and captures that as flash_bsize. This allows following code
         * to know where the next erase boundary begins.
         * */
        uint bboff = flash_addr & (flash_bsize - 1);
        uint bsnum = (bboff / cb->cb_ssize) >> (10 + smash_cmd_shift);
        uint snum;
        uint smap = cb->cb_map;
#ifdef ERASE_DEBUG
        printf("bblock bb_off=%x snum=%x s_map=%x\n", bboff, bsnum, smap);
#endif
        flash_bsize = 0;
        /* Find first bit of this map */
        while (bsnum > 0) {
            if (smap & BIT(bsnum))  // Found base
                break;
            bsnum--;
        }
        for (snum = bsnum + 1; snum < 8; snum++)
            if (smap & BIT(snum))
                break;  // At next block
        flash_bsize = (cb->cb_ssize * (snum - bsnum)) << (10 + smash_cmd_shift);
#ifdef ERASE_DEBUG
        printf(" bsnum=%x esnum=%x bb ssize %x\n", bsnum, snum, flash_bsize);
#endif
    }
#ifdef ERASE_DEBUG
    else {
        printf(" normal block %x\n", flash_bsize);
    }
#endif
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
    bank_info_t info;
    const chip_blocks_t *cb;
    const char *ptr;
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
    uint        mode;
    uint        tlen = 0;
    int         arg;
    int         pos;

    for (arg = 1; arg < argc; ) {
        ptr = argv[arg++];
        if ((strcmp(ptr, "?") == 0) || (strcmp(ptr, "help") == 0)) {
            rc = 0;
usage:
            printf("%s", cmd_erase_options);
            return (rc);
        } else if ((strncmp(ptr, "addr", 4) == 0) || (strcmp(ptr, "-a") == 0)) {
            if (arg + 1 > argc) {
                printf("smash %s %s requires an option\n", argv[0], ptr);
                goto usage;
            }
            pos = 0;
            if ((sscanf(argv[arg], "%x%n", &addr, &pos) != 1) || (pos == 0)) {
                printf("Invalid argument \"%s\" for %s %s\n",
                       argv[arg], argv[0], ptr);
                goto usage;
            }
            arg++;
        } else if ((strcmp(ptr, "bank") == 0) || (strcmp(ptr, "-b") == 0)) {
            if (arg + 1 > argc) {
                printf("smash %s %s requires an option\n", argv[0], ptr);
                goto usage;
            }
            pos = 0;
            if ((sscanf(argv[arg], "%x%n", &bank, &pos) != 1) || (pos == 0) ||
                (bank >= ROM_BANKS)) {
                printf("Invalid argument \"%s\" for %s %s\n",
                       argv[arg], argv[0], ptr);
                goto usage;
            }
            arg++;
        } else if ((strncmp(ptr, "deb", 3) == 0) || (strcmp(ptr, "-d") == 0)) {
            flag_debug++;
        } else if ((strncmp(ptr, "len", 3) == 0) || (strcmp(ptr, "-l") == 0)) {
            if (arg + 1 > argc) {
                printf("smash %s %s requires an option\n", argv[0], ptr);
                goto usage;
            }
            pos = 0;
            if ((sscanf(argv[arg], "%x%n", &len, &pos) != 1) ||
                (pos == 0) || (argv[arg][pos] != '\0')) {
                printf("Invalid argument \"%s\" for %s %s\n",
                       argv[arg], argv[0], ptr);
                goto usage;
            }
            arg++;
        } else if ((strncmp(ptr, "yes", 3) == 0) || (strcmp(ptr, "-y") == 0)) {
            flag_yes++;
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
        rc = MSG_STATUS_NO_REPLY;
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

#ifdef ERASE_DEBUG
    printf("pre saddr=%x eaddr=%x\n", flash_start_addr, flash_end_addr);
#endif
    /* Round start address down and end address up, then compute length */
    flash_start_addr = flash_start_addr & ~(flash_start_bsize - 1);
    flash_end_addr   = (flash_end_addr | (flash_end_bsize - 1)) + 1;
    len = flash_end_addr - flash_start_addr;
    addr = addr & ~(flash_start_bsize - 1);

#ifdef ERASE_DEBUG
    printf("saddr=%x sbsize=%x\n", flash_start_addr, flash_start_bsize);
    printf("eaddr=%x ebsize=%x\n", flash_end_addr, flash_end_bsize);
#endif

    printf("Erase bank=%u addr=%x len=%x\n", bank, addr, len);
    if ((!flag_yes) && (!are_you_sure("Proceed"))) {
        return (1);
    }

    printf("Progress [%*s]\rProgress [",
           (len + MAX_CHUNK - 1) / MAX_CHUNK, "");
    fflush(stdout);
    bank += addr / ROM_WINDOW_SIZE;
    addr &= (ROM_WINDOW_SIZE - 1);

    while (len > 0) {
        uint xlen = get_flash_bsize(cb, bank * ROM_WINDOW_SIZE + addr);

        rc = erase_flash_block(bank, addr);
        if (rc != 0) {
            printf("\nKicksmash failure %d (%s)\n", rc, smash_err(rc));
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
                printf(".");
            }
            fflush(stdout);
        }
    }
    if (rc == 0) {
        if (tlen > 0)
            printf(".");
        printf("]\n");
    }

    return (rc);
}

int
main(int argc, char *argv[])
{
    int      arg;
    char    *arg1;
    uint     loop;
    uint     loops = 1;
    uint     flag_bank = 0;
    uint     flag_inquiry = 0;
    uint     flag_test = 0;
    uint     flag_x_spin = 0;
    uint     flag_y_spin = 0;
    uint     bank   = 0;
    int      pos;
    uint     rc = 0;
    uint32_t addr;

#ifndef _DCC
    SysBase = *(struct ExecBase **)4UL;
#endif
    cpu_type = get_cpu();

    for (arg = 1; arg < argc; arg++) {
        char *ptr = argv[arg];
        if (*ptr == '-') {
            for (++ptr; *ptr != '\0'; ptr++) {
                switch (*ptr) {
                    case 'b':  // bank
                        exit(cmd_bank(argc - arg, argv + arg));
                        break;
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
                    case 't':  // test
                        flag_test++;
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
        } else if (arg1 == NULL) {
            arg1 = ptr;
        } else {
            printf("Error: unknown argument %s\n", ptr);
            usage();
            exit(1);
        }
    }

    if ((flag_bank | flag_inquiry | flag_test | flag_x_spin |
         flag_y_spin) == 0) {
        printf("You must specify an operation to perform\n");
        usage();
        exit(1);
    }

    for (loop = 0; loop < loops; loop++) {
        if (loops > 1) {
            if (flag_quiet) {
                if ((loop & 0xff) == 0) {
                    printf(".");
                    fflush(stdout);
                }
            } else {
                printf("Pass %-4u", loop + 1);
            }
        }
        if (flag_bank) {
            if (set_rom_bank(flag_bank, bank) && ((loops == 1) || (loop > 1))) {
                rc++;
                break;
            }
        }
        if (flag_x_spin) {
            spin_memory(addr);
        }
        if (flag_y_spin) {
            spin_memory_ovl(addr);
        }
        if (flag_inquiry) {
#if 0
            if (smash_identify() && ((loops == 1) || (loop > 1))) {
                rc++;
                break;
            }
#endif
            if (flash_show_id() && ((loops == 1) || (loop > 1))) {
                rc++;
                break;
            }
        }
        if (flag_test) {
            if (smash_test() && ((loops == 1) || (loop > 1))) {
                rc++;
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
        if (rc != 0)
            printf(" due to error %d (%s)", (int) rc, smash_err(rc));
        printf("\n");
    } else if (flag_quiet && (rc == 0)) {
        printf("Pass %u done\n", loop);
    }

    exit(rc);
}
