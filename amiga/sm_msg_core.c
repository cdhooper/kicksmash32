/*
 * msg
 * ---
 * Functions for AmigaOS to send messages to and receive messages from
 * Kicksmash.
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

#include <stdio.h>
#include <stdint.h>
#ifndef STANDALONE
#include <exec/execbase.h>
#endif
#include <memory.h>
#include "crc32.h"
#include "sm_msg.h"
#include "smash_cmd.h"
#include "host_cmd.h"
#include "cpu_control.h"

#define CIAA_TBLO        ADDR8(0x00bfe601)
#define CIAA_TBHI        ADDR8(0x00bfe701)
#define ROM_BASE         0x00f80000  /* Base address of Kickstart ROM */

#if defined(ROMFS) || defined(STANDALONE)
#define SM_ROM_BASE      ROM_BASE
#else
/*
 * Address through which Kicksmash is accessed. This is the Kickstart ROM
 * address, unless sm_rom_alias_open() has set up a cache-inhibited MMU
 * alias of the ROM, in which case it is the alias address. See
 * sm_rom_alias.c.
 */
uintptr_t sm_rom_base = ROM_BASE;
#define SM_ROM_BASE      sm_rom_base
#endif

#define ARRAY_SIZE(x) ((sizeof (x) / sizeof ((x)[0])))

uint smash_cmd_shift = 2;
extern uint flag_debug;

#if defined(ROMFS) || defined(STANDALONE)
#define SMASH_REPLY_WAIT 0
#else
/*
 * Additional time (CIA ticks) to wait for a Kicksmash reply, beyond the
 * normal reply wait. This is set by a caller which issues a command that
 * Kicksmash takes a long time to complete before replying, such as
 * KS_CMD_FW_WRITE (flash page erase and program).
 */
uint smash_reply_wait = 0;
#define SMASH_REPLY_WAIT smash_reply_wait
#endif

#ifdef ROMFS
#define crc32 lcrc32
#define cia_ticks lcia_ticks
#define cia_spin lcia_spin

#define TEXT_TO_RAM  __attribute__((section(".text_to_ram")))
#define CONST_TO_RAM const __attribute__((section(".data")))
#else
#define TEXT_TO_RAM
#define CONST_TO_RAM
#endif

#ifdef ROMFS
/*
 * STM32 CRC polynomial (also used in ethernet, SATA, MPEG-2, and ZMODEM)
 *      x^32 + x^26 + x^23 + x^22 + x^16 + x^12 + x^11 + x^10 + x^8 +
 *      x^7 + x^5 + x^4 + x^2 + x + 1
 *
 * The table implements the normal form of 0x04C11DB7. It is in the BSS
 * (RAM, as the ROM is unavailable while talking to Kicksmash), and is
 * filled by lcrc32_init(), which saves 1K of ROM space over a constant
 * table.
 */
uint32_t lcrc32_table[256];

void
lcrc32_init(void)
{
    uint     pos;
    uint     bit;
    uint32_t crc;

    for (pos = 0; pos < 256; pos++) {
        crc = pos << 24;
        for (bit = 0; bit < 8; bit++)
            crc = (crc << 1) ^ ((crc & 0x80000000) ? 0x04c11db7 : 0);
        lcrc32_table[pos] = crc;
    }
}

uint (*esend_cmd_core)(uint16_t cmd, void *arg, uint16_t arglen,
                       void *reply, uint replymax, uint *replyalen) =
                      &send_cmd_core;


/*
 * crc32() calculates the STM32 32-bit CRC. The advantage of this function
 *         over using hardware available in some STM32 processors is that
 *         this function may be called repeatedly to calculate incremental
 *         CRC values.
 *
 * @param [in]  crc - Initial value which can be used for repeated calls
 *                    or specify 0 to start new calculation.
 * @param [in]  buf - pointer to buffer holding data.
 * @param [in]  len - length of buffer.
 *
 * @return      CRC-32 value.
 */
TEXT_TO_RAM
uint32_t
crc32(uint32_t crc, const void *buf, size_t len)
{
    uint8_t *ptr = (uint8_t *) buf;

    while (len--) {
        /* Normal form calculation */
        crc = (crc << 8) ^ lcrc32_table[(crc >> 24) ^ *(ptr++)];
    }

    return (crc);
}

TEXT_TO_RAM
uint
cia_ticks(void)
{
    uint8_t hi1;
    uint8_t hi2;
    uint8_t lo;

    hi1 = *CIAA_TBHI;
    lo  = *CIAA_TBLO;
    hi2 = *CIAA_TBHI;

    /*
     * The below operation will provide the same effect as:
     *     if (hi2 != hi1)
     *         lo = 0xff;  // rollover occurred
     */
    lo |= (hi2 - hi1);  // rollover of hi forces lo to 0xff value

    return (lo | (hi2 << 8));
}

TEXT_TO_RAM
void
cia_spin(unsigned int ticks)
{
    uint16_t start = cia_ticks();
    uint16_t now;
    uint16_t diff;

    while (ticks != 0) {
        now = cia_ticks();

        diff = start - now;
        if (diff >= ticks)
            break;
        ticks -= diff;
        start = now;
        __asm__ __volatile__("nop");
        __asm__ __volatile__("nop");
    }
}
#endif
void (*ecia_spin)(unsigned int ticks) = cia_spin;

/*
 * rom_wait_recover
 * ----------------
 * Wait until ROM has recovered (Kicksmash is no longer driving data.
 */
TEXT_TO_RAM
static void
rom_wait_recover(uint32_t romval)
{
    const uintptr_t rom_base = SM_ROM_BASE;
    uint     pos;
    uint32_t last = 0;
    uint32_t cur;

    /* Ensure Kicksmash firmware has returned ROM to normal state */
    uint     timeout = 0;
    cia_spin(CIA_USEC(30));

    /* Wait until Kickstart ROM data is consistent for 5 ms */
    for (pos = 0; pos < 100; pos++) {
        cur = *VADDR32(rom_base + 0x15554); // remote addr 0x5555 or 0xaaaa
        if ((last != cur) || (*VADDR32(rom_base) != romval)) {
            if (timeout++ > 200000)
                break;  // Give up after 2 seconds
            pos = 0;
            last = cur;
        }
        cia_spin(CIA_USEC(50));
    }
}

/*
 * rom_wait_normal
 * ---------------
 * Wait until Kicksmash has re-enabled normal ROM access.
 */
TEXT_TO_RAM
static void
rom_wait_normal(uint32_t romval)
{
    const uintptr_t rom_base = SM_ROM_BASE;
    uint timeout = 0;
    while (*VADDR32(rom_base) != romval) {
        if (timeout++ > 1000)
            break;  // Took too long. Oh well!
        cia_spin(2);
    }
}

/*
 * send_cmd_core
 * -------------
 * Sends a message to KickSmash. This is done by generating a "magic"
 * sequence of reads at the ROM address, followed by the CRC-protected
 * message.
 *
 * This function assumes interrupts are already disabled by the caller,
 * and that rom_base reaches the physical ROM without the data cache:
 * either through the cache-inhibited alias set up by sm_rom_alias_open(),
 * or with the data cache and MMU disabled by the caller.
 *
 * cmd is the message command to send.
 * arg is a pointer to optional data to send.
 * arglen is the length of optional data to send.
 * reply is a pointer to a buffer for optional reply data.
 *     If reply is NULL, reply data will be received and discarded.
 * replylen is the length of the reply buffer.
 *
 */
TEXT_TO_RAM
uint
send_cmd_core(uint16_t cmd, void *arg, uint16_t arglen,
              void *reply, uint replymax, uint *replyalen)
{
    const uintptr_t rom_base = SM_ROM_BASE;
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
    uint      replyround;
    uint32_t  rombase_value = *VADDR32(rom_base);
    uint16_t  sm_magic[] = { 0x0204, 0x1017, 0x0119, 0x0117 };  // on stack
    //        Decimal        516     4119    281     279

#if 1
    for (pos = 0; pos < ARRAY_SIZE(sm_magic); pos++)
        (void) *VADDR32(rom_base + (sm_magic[pos] << smash_cmd_shift));
#else
    (void) *VADDR32(rom_base + (sm_magic[0] << smash_cmd_shift));
    (void) *VADDR32(rom_base + (sm_magic[1] << smash_cmd_shift));
    (void) *VADDR32(rom_base + (sm_magic[2] << smash_cmd_shift));
    (void) *VADDR32(rom_base + (sm_magic[3] << smash_cmd_shift));
#endif

    (void) *VADDR32(rom_base + (arglen << smash_cmd_shift));
    crc = crc32(0, &arglen, sizeof (arglen));
    crc = crc32(crc, &cmd, sizeof (cmd));
    crc = crc32(crc, argbuf, arglen);
    (void) *VADDR32(rom_base + (cmd << smash_cmd_shift));

    /* Send message payload */
    for (pos = 0; pos < (arglen + 1) / sizeof (uint16_t); pos++) {
        (void) *VADDR32(rom_base + (argbuf[pos] << smash_cmd_shift));
    }
    if (pos & 1) {
        /* Pad to 32-bit alignment */
        (void) *VADDR32(rom_base + (0xaaaa << smash_cmd_shift));
    }

    /* CRC high and low words */
    (void) *VADDR32(rom_base + ((crc >> 16) << smash_cmd_shift));
    (void) *VADDR32(rom_base + ((crc & 0xffff) << smash_cmd_shift));

    /* Extra read to kick off handling */
    cia_spin(2);
    val32 = *VADDR32(rom_base + 0x1554); // remote addr 0x0555 or 0x0aaa

    /*
     * Delay to prevent reads before Kicksmash has set up DMA hardware
     * with the data to send. This is necessary so that the two DMA
     * engines on 32-bit Amigas are started in a synchronized manner.
     * Might need more delay on a faster CPU.
     *
     * A3000 68030-25:  10 spins minimum
     * A3000 A3660 50M: 30 spins minimum
     */
    cia_spin((arglen >> 3) + (replymax >> 5));
//  cia_spin(100);  // XXX Debug delay for brief KS output

    /*
     * Find reply magic, length, and status.
     *
     * The below code must handle both a 32-bit reply and a 16-bit reply
     * where data begins in the lower 16 bits.
     *
     *            hi16bits lo16bits hi16bits lo16bits hi16bits lo16bits
     * Example 1: 0x1017   0x0204   0x0117   0x0119   len      status
     * Example 2: ?        0x1017   0x0204   0x0117   0x0119   len
     */
#define WAIT_FOR_MAGIC_LOOPS 128
    uint extra_wait = SMASH_REPLY_WAIT;
    uint got_magic = 0;
    for (word = 0; (word < WAIT_FOR_MAGIC_LOOPS) || (extra_wait != 0);
         word++) {
        if ((word >= WAIT_FOR_MAGIC_LOOPS) && (extra_wait != 0))
            extra_wait--;  // Bound the extended wait in all cases
        // A 16-bit Amiga reply may begin in the low 16 bits (Example 2)
        if (word & 1) {
            val = (uint16_t) val32;
        } else {
            val32 = *VADDR32(rom_base + 0x1110); // remote addr 0x0444 or 0x0888
#undef SM_MSG_DEBUG
#ifdef SM_MSG_DEBUG
            *VADDR32(0x77780 + word * 2) = val32;
#endif
            val = val32 >> 16;
        }
        if ((flag_debug > 2) && (replybuf != NULL) && (word < (replymax / 2))) {
            replybuf[word] = val;  // Just for debug on failure (-d flag)
        }

        if (magic < ARRAY_SIZE(sm_magic)) {
            if (val != sm_magic[magic]) {
                /* This word might begin a new magic sequence */
                magic = (val == sm_magic[0]) ? 1 : 0;
                if (magic == 0) {
                    if (word < WAIT_FOR_MAGIC_LOOPS) {
                        cia_spin(word);
                    } else {
                        /* Extended wait for a slow reply */
                        uint ticks = (extra_wait < WAIT_FOR_MAGIC_LOOPS) ?
                                     extra_wait : WAIT_FOR_MAGIC_LOOPS;
                        cia_spin(ticks);
                        extra_wait -= ticks;
                    }
                }
                continue;
            }
        } else if (magic < ARRAY_SIZE(sm_magic) + 1) {
            replylen = val;     // Reply length
            crc = crc32(0, &val, sizeof (val));
        } else if (magic < ARRAY_SIZE(sm_magic) + 2) {
            replystatus = val;  // Reply status
            crc = crc32(crc, &val, sizeof (val));
            word++;
            got_magic = 1;
            break;
        }
        magic++;
    }

    if (!got_magic) {
        /* Did not see reply magic */
        replystatus = MSG_STATUS_NO_REPLY;
        if (replyalen != NULL) {
            *replyalen = word * 2;
            if (*replyalen > replymax)
                *replyalen = replymax;
        }
        rom_wait_recover(rombase_value);  // Wait until ROM is accessible again
        goto scc_cleanup;
    }

    if (replyalen != NULL)
        *replyalen = replylen;

#ifdef SM_MSG_DEBUG
    uint16_t *dptr = (uint16_t *) ADDR16(0x77700);
    for (pos = 0; pos < ARRAY_SIZE(sm_magic); pos++)
        *(dptr++) = sm_magic[pos];
    *(dptr++) = replylen;
    *(dptr++) = replystatus;
#endif
    replyround = (replylen + 3) & ~3;  // Round up reply length to long

    if ((replybuf != NULL) && (replyround > replymax)) {
        replystatus = MSG_STATUS_BAD_LENGTH;
        if (replyalen != NULL) {
            *replyalen = replylen;
            if (*replyalen > replymax)
                *replyalen = replymax;
        }
        goto scc_cleanup;
    }

    /*
     * Response is valid so far; read data, which is padded to a 32-bit
     * boundary, then the 32-bit CRC. A reply which began in the low 16 bits
     * of a read (16-bit Amiga) stays that way, so take every value 16 bits
     * at a time.
     */
    {
        uint replymin = (replybuf == NULL) ? 0 :
                        (replymax < replylen) ? replymax : replylen;
        for (pos = 0; pos < replyround + 4; pos += 2, word++) {
            if (word & 1) {
                val = (uint16_t) val32;
            } else {
                val32 = *VADDR32(rom_base);
                val = val32 >> 16;
            }
#ifdef SM_MSG_DEBUG
            *(dptr++) = val;
#endif
            if (pos < replymin)
                *(replybuf++) = val;
            else if (pos >= replyround)
                replycrc = (replycrc << 16) | val;  // CRC
        }
    }

scc_cleanup:
    if ((replystatus & 0xffffff00) != 0) {
        rom_wait_recover(rombase_value);  // Wait until ROM is accessible again
    }

    if (((replystatus & 0xffff0000) == 0) && (replystatus != KS_STATUS_CRC)) {
        crc = crc32(crc, reply, replylen);
        if (crc != replycrc) {
#ifdef SM_MSG_DEBUG
            *VADDR32(0x7770000) = crc;
            *VADDR32(0x7770004) = replycrc;
            *(dptr++) = crc >> 16;
            *(dptr++) = crc;
            *(dptr++) = 0;
            *(dptr++) = 0;
            *(dptr++) = 0xdead;
            *(dptr++) = 0xdead;
#endif
            rom_wait_normal(rombase_value);
            return (MSG_STATUS_BAD_CRC);
        }
    }

    /* Wait for ROM to be normal unless it's a flash operation */
    if ((cmd & 0xf0) != KS_CMD_FLASH_READ)
        rom_wait_normal(rombase_value);
    return (replystatus);
}
