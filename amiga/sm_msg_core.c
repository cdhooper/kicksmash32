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
#include <memory.h>
#include "crc32.h"
#include "sm_msg.h"
#include "smash_cmd.h"
#include "host_cmd.h"
#include "cpu_control.h"

#define crc32 lcrc32
#define cia_ticks lcia_ticks
#define cia_spin lcia_spin

#define CIAA_TBLO        ADDR8(0x00bfe601)
#define CIAA_TBHI        ADDR8(0x00bfe701)

#define ARRAY_SIZE(x) ((sizeof (x) / sizeof ((x)[0])))

#define ROM_BASE         0x00f80000  /* Base address of Kickstart ROM */

extern uint smash_cmd_shift;
extern uint flag_debug;

#ifdef ROMFS
#define TEXT_TO_RAM  __attribute__((section(".text_to_ram")))
#define CONST_TO_RAM const __attribute__((section(".data")))
#else
#define TEXT_TO_RAM
#define CONST_TO_RAM
#endif

/*
 * STM32 CRC polynomial (also used in ethernet, SATA, MPEG-2, and ZMODEM)
 *      x^32 + x^26 + x^23 + x^22 + x^16 + x^12 + x^11 + x^10 + x^8 +
 *      x^7 + x^5 + x^4 + x^2 + x + 1
 *
 * The below table implements the normal form of 0x04C11DB7.
 * It may be found here, among other places on the internet:
 *     https://github.com/Michaelangel007/crc32
 */
// uint32_t  // Place this table in the data section
CONST_TO_RAM uint32_t
lcrc32_table[] = {
    0x00000000, 0x04c11db7, 0x09823b6e, 0x0d4326d9,
    0x130476dc, 0x17c56b6b, 0x1a864db2, 0x1e475005,
    0x2608edb8, 0x22c9f00f, 0x2f8ad6d6, 0x2b4bcb61,
    0x350c9b64, 0x31cd86d3, 0x3c8ea00a, 0x384fbdbd,
    0x4c11db70, 0x48d0c6c7, 0x4593e01e, 0x4152fda9,
    0x5f15adac, 0x5bd4b01b, 0x569796c2, 0x52568b75,
    0x6a1936c8, 0x6ed82b7f, 0x639b0da6, 0x675a1011,
    0x791d4014, 0x7ddc5da3, 0x709f7b7a, 0x745e66cd,
    0x9823b6e0, 0x9ce2ab57, 0x91a18d8e, 0x95609039,
    0x8b27c03c, 0x8fe6dd8b, 0x82a5fb52, 0x8664e6e5,
    0xbe2b5b58, 0xbaea46ef, 0xb7a96036, 0xb3687d81,
    0xad2f2d84, 0xa9ee3033, 0xa4ad16ea, 0xa06c0b5d,
    0xd4326d90, 0xd0f37027, 0xddb056fe, 0xd9714b49,
    0xc7361b4c, 0xc3f706fb, 0xceb42022, 0xca753d95,
    0xf23a8028, 0xf6fb9d9f, 0xfbb8bb46, 0xff79a6f1,
    0xe13ef6f4, 0xe5ffeb43, 0xe8bccd9a, 0xec7dd02d,
    0x34867077, 0x30476dc0, 0x3d044b19, 0x39c556ae,
    0x278206ab, 0x23431b1c, 0x2e003dc5, 0x2ac12072,
    0x128e9dcf, 0x164f8078, 0x1b0ca6a1, 0x1fcdbb16,
    0x018aeb13, 0x054bf6a4, 0x0808d07d, 0x0cc9cdca,
    0x7897ab07, 0x7c56b6b0, 0x71159069, 0x75d48dde,
    0x6b93dddb, 0x6f52c06c, 0x6211e6b5, 0x66d0fb02,
    0x5e9f46bf, 0x5a5e5b08, 0x571d7dd1, 0x53dc6066,
    0x4d9b3063, 0x495a2dd4, 0x44190b0d, 0x40d816ba,
    0xaca5c697, 0xa864db20, 0xa527fdf9, 0xa1e6e04e,
    0xbfa1b04b, 0xbb60adfc, 0xb6238b25, 0xb2e29692,
    0x8aad2b2f, 0x8e6c3698, 0x832f1041, 0x87ee0df6,
    0x99a95df3, 0x9d684044, 0x902b669d, 0x94ea7b2a,
    0xe0b41de7, 0xe4750050, 0xe9362689, 0xedf73b3e,
    0xf3b06b3b, 0xf771768c, 0xfa325055, 0xfef34de2,
    0xc6bcf05f, 0xc27dede8, 0xcf3ecb31, 0xcbffd686,
    0xd5b88683, 0xd1799b34, 0xdc3abded, 0xd8fba05a,
    0x690ce0ee, 0x6dcdfd59, 0x608edb80, 0x644fc637,
    0x7a089632, 0x7ec98b85, 0x738aad5c, 0x774bb0eb,
    0x4f040d56, 0x4bc510e1, 0x46863638, 0x42472b8f,
    0x5c007b8a, 0x58c1663d, 0x558240e4, 0x51435d53,
    0x251d3b9e, 0x21dc2629, 0x2c9f00f0, 0x285e1d47,
    0x36194d42, 0x32d850f5, 0x3f9b762c, 0x3b5a6b9b,
    0x0315d626, 0x07d4cb91, 0x0a97ed48, 0x0e56f0ff,
    0x1011a0fa, 0x14d0bd4d, 0x19939b94, 0x1d528623,
    0xf12f560e, 0xf5ee4bb9, 0xf8ad6d60, 0xfc6c70d7,
    0xe22b20d2, 0xe6ea3d65, 0xeba91bbc, 0xef68060b,
    0xd727bbb6, 0xd3e6a601, 0xdea580d8, 0xda649d6f,
    0xc423cd6a, 0xc0e2d0dd, 0xcda1f604, 0xc960ebb3,
    0xbd3e8d7e, 0xb9ff90c9, 0xb4bcb610, 0xb07daba7,
    0xae3afba2, 0xaafbe615, 0xa7b8c0cc, 0xa379dd7b,
    0x9b3660c6, 0x9ff77d71, 0x92b45ba8, 0x9675461f,
    0x8832161a, 0x8cf30bad, 0x81b02d74, 0x857130c3,
    0x5d8a9099, 0x594b8d2e, 0x5408abf7, 0x50c9b640,
    0x4e8ee645, 0x4a4ffbf2, 0x470cdd2b, 0x43cdc09c,
    0x7b827d21, 0x7f436096, 0x7200464f, 0x76c15bf8,
    0x68860bfd, 0x6c47164a, 0x61043093, 0x65c52d24,
    0x119b4be9, 0x155a565e, 0x18197087, 0x1cd86d30,
    0x029f3d35, 0x065e2082, 0x0b1d065b, 0x0fdc1bec,
    0x3793a651, 0x3352bbe6, 0x3e119d3f, 0x3ad08088,
    0x2497d08d, 0x2056cd3a, 0x2d15ebe3, 0x29d4f654,
    0xc5a92679, 0xc1683bce, 0xcc2b1d17, 0xc8ea00a0,
    0xd6ad50a5, 0xd26c4d12, 0xdf2f6bcb, 0xdbee767c,
    0xe3a1cbc1, 0xe760d676, 0xea23f0af, 0xeee2ed18,
    0xf0a5bd1d, 0xf464a0aa, 0xf9278673, 0xfde69bc4,
    0x89b8fd09, 0x8d79e0be, 0x803ac667, 0x84fbdbd0,
    0x9abc8bd5, 0x9e7d9662, 0x933eb0bb, 0x97ffad0c,
    0xafb010b1, 0xab710d06, 0xa6322bdf, 0xa2f33668,
    0xbcb4666d, 0xb8757bda, 0xb5365d03, 0xb1f740b4
};

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

/*
 * rom_wait_normal
 * ---------------
 * Wait until ROM has recovered (Kicksmash is no longer driving data.
 */
TEXT_TO_RAM
static void
rom_wait_normal(void)
{
    uint     pos;
    uint32_t last = 0;
    uint32_t cur;

    /* Ensure Kicksmash firmware has returned ROM to normal state */
    uint     timeout = 0;
    cia_spin(CIA_USEC(30));

    /* Wait until Kickstart ROM data is consistent for 2 ms */
    for (pos = 0; pos < 100; pos++) {
        cur = *ADDR32(ROM_BASE + 0x15554); // remote addr 0x5555 or 0xaaaa
        if ((last != cur) || (*ADDR32(ROM_BASE) != 0x11144ef9)) {
            if (timeout++ > 200000)
                break;  // Give up after 2 seconds
            pos = 0;
            last = cur;
        }
        cia_spin(CIA_USEC(20));
    }
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

#if 1
    for (pos = 0; pos < ARRAY_SIZE(sm_magic); pos++)
        (void) *ADDR32(ROM_BASE + (sm_magic[pos] << smash_cmd_shift));
#else
    (void) *ADDR32(ROM_BASE + (sm_magic[0] << smash_cmd_shift));
    (void) *ADDR32(ROM_BASE + (sm_magic[1] << smash_cmd_shift));
    (void) *ADDR32(ROM_BASE + (sm_magic[2] << smash_cmd_shift));
    (void) *ADDR32(ROM_BASE + (sm_magic[3] << smash_cmd_shift));
#endif

    (void) *ADDR32(ROM_BASE + (arglen << smash_cmd_shift));
    crc = crc32(0, &arglen, sizeof (arglen));
    crc = crc32(crc, &cmd, sizeof (cmd));
    crc = crc32(crc, argbuf, arglen);
    (void) *ADDR32(ROM_BASE + (cmd << smash_cmd_shift));

    /* Send message payload */
    for (pos = 0; pos < (arglen + 1) / sizeof (uint16_t); pos++) {
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
    cia_spin((arglen >> 3) + (replymax >> 5) + 10);
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
#ifdef SM_MSG_DEBUG
            *ADDR32(0x7770030 + word * 2) = val32;
#endif
            val = val32 >> 16;
        }
        if ((flag_debug > 2) && (replybuf != NULL) && (word < (replymax / 2))) {
            replybuf[word] = val;  // Just for debug on failure (-d flag)
        }

        if (magic < ARRAY_SIZE(sm_magic)) {
            if (val != sm_magic[magic]) {
                magic = 0;
                cia_spin(word);
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
        rom_wait_normal();  // Wait until ROM is accessible again
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
#ifdef SM_MSG_DEBUG
                *ADDR32(0x7770030 + word * 2) = val32;
#endif
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
        rom_wait_normal();  // Wait until ROM is accessible again
    }

    if (((replystatus & 0xffff0000) == 0) && (replystatus != KS_STATUS_CRC)) {
        crc = crc32(crc, reply, replylen);
        if (crc != replycrc) {
#ifdef SM_MSG_DEBUG
            *ADDR32(0x7770000) = crc;
            *ADDR32(0x7770004) = replycrc;
#endif
            return (MSG_STATUS_BAD_CRC);
        }
    }
    return (replystatus);
}
