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

#include <stdint.h>
#include <stdbool.h>
#include <memory.h>
#include "sm_msg.h"
#include "crc32.h"
#include "timer.h"
#include "utils.h"
#include "smash_cmd.h"
#include "host_cmd.h"
#include "printf.h"
#include "config.h"
#include "ee_kicksmash.h"

#define ROM_BASE         0x00f80000  /* Base address of Kickstart ROM */

#define SWAP16(x)   __builtin_bswap16(x)
#define SWAP32(x)   __builtin_bswap32(x)
#define SWAP64(x)   __builtin_bswap64(x)

uint smash_cmd_shift = 2;
extern uint flag_debug;

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

uint (*esend_cmd_core)(uint16_t cmd, void *arg, uint16_t arglen,
                       void *reply, uint replymax, uint *replyalen) =
                      &send_cmd_core;

/*
 * rom_wait_recover
 * ----------------
 * Wait until ROM has recovered (Kicksmash is no longer driving data.
 */
TEXT_TO_RAM
static void
rom_wait_recover(void)
{
    timer_delay_usec(1000);
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
    timer_delay_usec(50);
}

/*
 * send_cmd_core
 * -------------
 * Sends a message to KickSmash. This is done by generating a "magic"
 * sequence of reads at the ROM address, followed by the CRC-protected
 * message.
 *
 * This function assumes interrupts and cache are already disabled
 * by the caller.
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
    uint16_t  sm_magic[] = { 0x0204, 0x1017, 0x0119, 0x0117 };  // on stack
    //        Decimal        516     4119    281     279
    uint32_t  data;
    uint16_t  addr;
    uint32_t  rombase_value;
    (void) ee_read(0, &rombase_value, 1);

    for (pos = 0; pos < ARRAY_SIZE(sm_magic); pos++)
        (void) ee_read(sm_magic[pos], &data, 1);

    (void) ee_read(arglen, &data, 1);
    crc = crc32r(0, &arglen, sizeof (arglen));
    crc = crc32r(crc, &cmd, sizeof (cmd));
    crc = crc32(crc, argbuf, arglen);
    (void) ee_read(cmd, &data, 1);

    /* Send message payload */
    for (pos = 0; pos < (arglen + 1) / sizeof (uint16_t); pos++) {
        addr = SWAP16(argbuf[pos]);
        (void) ee_read(addr, &data, 1);
    }
    if (pos & 1) {
        /* Pad to 32-bit alignment */
        (void) ee_read(0xaaaa, &data, 1);
    }

    /* CRC high and low words */
    (void) ee_read(crc >> 16, &data, 1);
    (void) ee_read(crc & 0xffff, &data, 1);

    /*
     * Delay to prevent reads before Kicksmash has set up DMA hardware
     * with the data to send. This is necessary so that the two DMA
     * engines on 32-bit Amigas are started in a synchronized manner.
     * Might need more delay on a faster CPU.
     *
     * A3000 68030-25:  10 spins minimum
     * A3000 A3660 50M: 30 spins minimum
     */
    timer_delay_usec((arglen >> 3) + (replymax >> 5) + 10);
//  timer_delay_usec(100);  // XXX Debug delay for brief KS output

    /*
     * Find reply magic, length, and status.
     *
     * The below code must handle both a 32-bit reply and a 16-bit reply
     * where data begins in the lower 16 bits.
     *
     *            hi16bits lo16bits hi16bits lo16bits hi16bits lo16bits
     * Example 1: 0x1017   0x0204   0x0117   0x0119   len      status
     * Example 2: ?        0x0119   0x0117   0x0204   0x1017   len
     */
#define WAIT_FOR_MAGIC_LOOPS 128
    for (word = 0; word < WAIT_FOR_MAGIC_LOOPS; word++) {
        // XXX: This code might need to change for 16-bit Amigas
        if (word & 1) {
            val = (uint16_t) val32;
        } else {
            (void) ee_read(0x555, &val32, 1);
            if (flag_debug > 2)
                printf(">%08lx", val32);
            if ((val32 == 0x10170204) && (magic == 0) &&
                (config.ee_mode == EE_MODE_AUTO)) {
                if ((ee_mode == EE_MODE_32) || (ee_mode == EE_MODE_AUTO))
                    ee_mode = EE_MODE_32_SWAP;
                else
                    ee_mode = EE_MODE_32;
                val32 = (val32 >> 16) | (val32 << 16);
            }
            val = val32 >> 16;
        }
        if ((flag_debug > 1) && (replybuf != NULL) && (word < (replymax / 2))) {
            replybuf[word] = val;  // Just for debug on failure (-d flag)
        }

        if (magic < ARRAY_SIZE(sm_magic)) {
            if (val != sm_magic[magic]) {
                magic = 0;
                timer_delay_usec(word);
                continue;
            }
        } else if (magic < ARRAY_SIZE(sm_magic) + 1) {
            replylen = val;     // Reply length
            crc = crc32r(0, &val, sizeof (val));
#undef SM_MSG_DEBUG
#ifdef SM_MSG_DEBUG
            printf(" crcL=%08lx\n", crc);
#endif
        } else if (magic < ARRAY_SIZE(sm_magic) + 2) {
            replystatus = val;  // Reply status
            crc = crc32r(crc, &val, sizeof (val));
#ifdef SM_MSG_DEBUG
            printf(" crcS=%08lx\n", crc);
#endif
            word++;
            break;
        }
        magic++;
    }

    if (word >= WAIT_FOR_MAGIC_LOOPS) {
        /* Did not see reply magic */
        replystatus = MSG_STATUS_NO_REPLY;
        if (replyalen != NULL) {
            *replyalen = word * 2;
            if (*replyalen > replymax)
                *replyalen = replymax;
        }
        rom_wait_recover();  // Wait until ROM is accessible again
        goto scc_cleanup;
    }

    if (replyalen != NULL)
        *replyalen = replylen;

#ifdef SM_MSG_DEBUG
    printf(" // ");
    for (pos = 0; pos < ARRAY_SIZE(sm_magic); pos++)
        printf(" %04x", sm_magic[pos]);
    printf(" %04x %04x", replylen, replystatus);
#endif
    replyround = (replylen + 3) & ~3;  // Round up reply length to long

    if (replyround > replymax) {
printf("replyround=%x %d replylen=%x %d replymax=%x %d\n", replyround, replyround, replylen, replylen, replymax, replymax);
        replystatus = MSG_STATUS_BAD_LENGTH;
        if (replyalen != NULL) {
            *replyalen = replylen;
            if (*replyalen > replymax)
                *replyalen = replymax;
        }
        goto scc_cleanup;
    }

    /* Response is valid so far; read data */
    if (replybuf == NULL) {
        pos = 0;
    } else {
        uint replymin = (replymax < replylen) ? replymax : replylen;
        for (pos = 0; pos < replymin; pos += 2, word++) {
            if (word & 1) {
                val = (uint16_t) val32;
            } else {
                (void) ee_read(0, &val32, 1);
                if (flag_debug > 2)
                    printf("|%08lx", val32);
                val = val32 >> 16;
            }
            val = SWAP16(val);
#ifdef SM_MSG_DEBUG
            printf(" %04x", val);
#endif
            *(replybuf++) = val;
        }
    }
    if (pos < replylen) {
        /* Discard data that doesn't fit */
        for (; pos < replylen; pos += 4)
            (void) ee_read(0, &val32, 1);
    }

    /* Read CRC */
    (void) ee_read(0, &replycrc, 1);
    if (flag_debug > 2)
        printf("/%08lx/", val32);

#ifdef SM_MSG_DEBUG
    printf(" %04lx %04lx\n", replycrc >> 16, replycrc & 0xffff);
#endif

scc_cleanup:
    if ((replystatus & 0xffffff00) != 0) {
        rom_wait_recover();  // Wait until ROM is accessible again
    }

    if (((replystatus & 0xffff0000) == 0) && (replystatus != KS_STATUS_CRC)) {
#if 0
{
uint32_t tcrc = crc;
uint rpos;
for (rpos = 0; rpos < replylen; rpos += 2) {
    tcrc = crc32r(tcrc, reply + rpos, 2);
    printf(" %x tcrc=%08lx\n", rpos, tcrc);
}
}
printf(" crcpre=%08x\n", crc);
#endif
        crc = crc32(crc, reply, replylen);
        if (crc != replycrc) {
#ifdef SM_MSG_DEBUG
            printf("\nCRC mismatch %08lx != calc %08lx", replycrc, crc);
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
