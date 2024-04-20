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
#include <exec/execbase.h>
#include "crc32.h"
#include "msg.h"
#include "smash_cmd.h"
#include "cpu_control.h"

#define ARRAY_SIZE(x) ((sizeof (x) / sizeof ((x)[0])))

#define ROM_BASE         0x00f80000  /* Base address of Kickstart ROM */

uint smash_cmd_shift = 2;
extern uint flag_debug;

static const uint16_t sm_magic[] = { 0x0204, 0x1017, 0x0119, 0x0117 };

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
    uint      replyround;

    for (pos = 0; pos < ARRAY_SIZE(sm_magic); pos++)
        (void) *ADDR32(ROM_BASE + (sm_magic[pos] << smash_cmd_shift));

    (void) *ADDR32(ROM_BASE + (arglen << smash_cmd_shift));
    crc = crc32(0, &arglen, sizeof (arglen));
    crc = crc32(crc, &cmd, sizeof (cmd));
    crc = crc32(crc, argbuf, arglen);
    (void) *ADDR32(ROM_BASE + (cmd << smash_cmd_shift));

    for (pos = 0; pos < arglen / sizeof (uint16_t); pos++) {
        (void) *ADDR32(ROM_BASE + (argbuf[pos] << smash_cmd_shift));
    }
    if (arglen & 1) {
        /* Odd byte at end */
        (void) *ADDR32(ROM_BASE + (argbuf[pos] << smash_cmd_shift));
    }

    /* CRC high and low words */
    (void) *ADDR32(ROM_BASE + ((crc >> 16) << smash_cmd_shift));
    (void) *ADDR32(ROM_BASE + ((crc & 0xffff) << smash_cmd_shift));

    /*
     * Delay to prevent reads before Kicksmash has set up DMA hardware
     * with the data to send. This is necessary so that the two DMA
     * engines on 32-bit Amigas are started in a synchronized manner.
     * Might need more delay on a faster CPU.
     *
     * A3000 68030-25:  10 spins minimum
     * A3000 A3660 50M: 30 spins minimum
     */
    cia_spin((arglen >> 3) + (replymax >> 4) + 30);
//  cia_spin(100);  // XXX Debug delay for brief KS output

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
                cia_spin(10);
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
        /* Did not see reply magic */
        replystatus = MSG_STATUS_NO_REPLY;
        if (replyalen != NULL) {
            *replyalen = word * 2;
            if (*replyalen > replymax)
                *replyalen = replymax;
        }
        /* Ensure Kicksmash firmware has returned ROM to normal state */
        for (pos = 0; pos < 1000; pos++)
            (void) *ADDR32(ROM_BASE + 0x15554); // remote addr 0x5555 or 0xaaaa
        cia_spin(CIA_USEC_LONG(100000));        // 100 msec
        goto scc_cleanup;
    }

    if (replyalen != NULL)
        *replyalen = replylen;

    replyround = (replylen + 1) & ~1;  // Round up reply length to word

    if (replyround > replymax) {
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
                val32 = *ADDR32(ROM_BASE);
                val = val32 >> 16;
            }
            *(replybuf++) = val;
        }
    }
    if (pos < replylen) {
        /* Discard data that doesn't fit */
        for (; pos < replylen; pos += 4)
            val32 = *ADDR32(ROM_BASE);
    }

    /* Read CRC */
    if (word & 1) {
        replycrc = (val32 << 16) | *ADDR16(ROM_BASE);
    } else {
        replycrc = *ADDR32(ROM_BASE);
    }

scc_cleanup:
#if 0
    /* Debug cleanup */
    for (pos = 0; pos < 4; pos++)
        *ADDR32(0x7770010 + pos * 4) = *ADDR32(ROM_BASE);
#endif

    if ((replystatus & 0xffffff00) != 0) {
        /* Ensure Kicksmash firmware has returned ROM to normal state */
        cia_spin(CIA_USEC(30));
        for (pos = 0; pos < 100; pos++)
            (void) *ADDR32(ROM_BASE + 0x15554); // remote addr 0x5555 or 0xaaaa
// XXX: implement new recovery function which will loop until
//      value at ROM address is consistent for 100 iterations.
//      cia_spin(10) in between.
        cia_spin(CIA_USEC(4000U));
    }
    if (((replystatus & 0xffff0000) == 0) && (replystatus != KS_STATUS_CRC)) {
        crc = crc32(crc, reply, replylen);
        if (crc != replycrc) {
//          *ADDR32(0x7770000) = crc;
//          *ADDR32(0x7770004) = replycrc;
            return (MSG_STATUS_BAD_CRC);
        }
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


void
msg_init(void)
{
    cpu_control_init();
}
