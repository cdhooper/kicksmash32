/*
 * smash_flash
 * -----------
 * Flash read, write, verify, and erase commands for the smash utility.
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
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <clib/dos_protos.h>
#include <dos/dos.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include "smash_cmd.h"
#include "sm_msg.h"
#include "cpu_control.h"
#include "smash.h"
#include "smash_flash.h"

static const char cmd_read_options[] =
    "smash read options\n"
    "   addr <hex>   starting address (-a)\n"
    "   bank <num>   flash bank on which to operate (-b)\n"
    "   dump         save hex/ASCII instead of binary (-d)\n"
    "   file <name>  file where to save content (-f)\n"
    "   len <hex>    length to read in bytes (-l)\n"
    "   swap <mode>  byte swap mode (auto, 1032, 2301, 3210) (-s)\n"
    "   yes          skip prompt (-y)\n";

static const char cmd_write_options[] =
    "smash write options\n"
    "   addr <hex>   starting address (-a)\n"
    "   bank <num>   flash bank on which to operate (-b)\n"
    "   cons         conservative flash write timing mode (-C)\n"
//  "   dump         save hex/ASCII instead of binary (-d)\n"
    "   file <name>  file from which to read (-f)\n"
    "   len <hex>    length to program in bytes (-l)\n"
    "   swap <mode>  byte swap mode (auto, 1032, 2301, 3210) (-s)\n"
    "   yes          skip prompt (-y)\n";

static const char cmd_verify_options[] =
    "smash verify options\n"
    "   addr <hex>   starting address (-a)\n"
    "   bank <num>   flash bank on which to operate (-b)\n"
//  "   dump         save hex/ASCII instead of binary (-d)\n"
    "   file <name>  file to verify against (-f)\n"
    "   len <hex>    length to read in bytes (-l)\n"
    "   swap <mode>  byte swap mode (auto, 1032, 2301, 3210) (-s)\n"
    "   yes          skip prompt (-y)\n";

static const char cmd_erase_options[] =
    "smash erase options\n"
    "   addr <hex>   starting address (-a)\n"
    "   bank <num>   flash bank on which to operate (-b)\n"
    "   len <hex>    length to erase in bytes (-l)\n"
    "   yes          skip prompt (-y)\n";

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
    { "-C", "cons" },
    { "-C", "conservative" },
    { "-D", "debug" },
    { "-d", "dump" },
    { "-f", "file" },
    { "-h", "?" },
    { "-h", "help" },
    { "-l", "len" },
    { "-l", "length" },
    { "-s", "swap" },
    { "-y", "yes" },
    { "-r", "read" },
    { "-v", "verify" },
    { "-w", "write" },
};

static uint8_t flag_flash_write_bug = 1;

void
local_memcpy(void *dst, void *src, size_t len)
{
    uint32_t *dst32 = dst;
    uint32_t *src32 = src;

    if ((((uintptr_t) dst | (uintptr_t) src) & 3) == 0) {
        /* fast mode: both buffers are 32-bit aligned */
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

static uint
send_cmd_core_retry(uint16_t cmd, void *arg, uint16_t arglen,
                    void *reply, uint replymax, uint *replyalen)
{
    uint rc;
    uint tries = 5;

    do {
        rc = send_cmd_core(cmd, arg, arglen, reply, replymax, replyalen);
        if ((rc != 0) &&
            (cmd != KS_CMD_FLASH_READ) && (cmd != KS_CMD_FLASH_CMD) &&
            (cmd != KS_CMD_FLASH_ERASE) && (cmd != KS_CMD_FLASH_WRITE)) {
            flash_read_mode(1);
        }
        if ((rc != MSG_STATUS_BAD_CRC) &&
            (rc != MSG_STATUS_NO_REPLY) &&
            (rc != KS_STATUS_CRC)) {
            break;
        }
    } while (--tries > 0);
    return (rc);
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
        if (is_user_abort())
            return (FALSE);
        if ((ch == 'y') || (ch == 'Y'))
            return (TRUE);
        if ((ch == 'n') || (ch == 'N'))
            return (FALSE);
        if (!isspace(ch) && (ch != '\n'))
            goto ask_again;
    }
    return (FALSE);
}

static uint
get_file_size(const char *filename)
{
    /* Examine() requires a longword-aligned FileInfoBlock */
    static struct FileInfoBlock fb __attribute__((aligned(4)));
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

#define SWAPMODE_AUTO  0xAAAA  // Automatic 32-bit ROM swapping

#define SWAP_TO_ROM    0  // Bytes originated in a file (to be written in ROM)
#define SWAP_FROM_ROM  1  // Bytes originated in ROM (to be written to a file)

/*
 * detect_swapmode() determines the byte swap operation to perform on a ROM
 *                   image when one of the automatic swap mode
 *                   (SWAPMODE_AUTO) has been requested.
 *                   The operation is chosen from the Kickstart ROM header
 *                   at the start of the image, so this function must only
 *                   be called once for an image, with the first bytes of
 *                   that image. The swap mode which is returned is then
 *                   used with execute_swapmode() for all of the image.
 *
 * @param  [in]  buf      - First four bytes of the ROM image.
 * @param  [in]  dir      - Image swap direction (SWAP_TO_ROM or SWAP_FROM_ROM)
 * @param  [in]  swapmode - Requested swap mode (1032, 3210, auto, etc)
 * @return       Swap operation to perform (0123, 1032, 2301, or 3210).
 * @return       VALUE_UNASSIGNED - ROM image format was not recognized.
 */
static void execute_swapmode(uint8_t *buf, uint len, uint swapmode);

/*
 * is_rom_header() reports whether the four bytes are a Kickstart ROM
 *                 header in the order required for the swap direction.
 *                 512K ROMs begin with 0x11144ef9 and 256K ROMs begin
 *                 with 0x11114ef9 (as read by the Amiga CPU).
 */
static int
is_rom_header(const uint8_t *buf, uint dir)
{
    if (dir == SWAP_TO_ROM)
        return ((buf[0] == 0xf9) && (buf[1] == 0x4e) &&
                ((buf[2] == 0x14) || (buf[2] == 0x11)) && (buf[3] == 0x11));
    return ((buf[0] == 0x11) && ((buf[1] == 0x14) || (buf[1] == 0x11)) &&
            (buf[2] == 0x4e) && (buf[3] == 0xf9));
}

static uint
detect_swapmode(const uint8_t *buf, uint dir, uint swapmode)
{
    static const uint16_t modes[] = { 123, 3210, 2301, 1032 };
    uint8_t tbuf[4];
    uint    pos;

    if (swapmode != SWAPMODE_AUTO)
        return (swapmode);  // Not an automatic mode

    for (pos = 0; pos < ARRAY_SIZE(modes); pos++) {
        memcpy(tbuf, buf, sizeof (tbuf));
        execute_swapmode(tbuf, sizeof (tbuf), modes[pos]);
        if (is_rom_header(tbuf, dir)) {
            if (modes[pos] != 123)
                printf("Swap mode %u\n", modes[pos]);
            return (modes[pos]);
        }
    }

    printf("Unrecognized Amiga ROM format: %02x %02x %02x %02x\n"
           "Specify the swap mode (0123, 1032, 2301, or 3210)\n",
           buf[0], buf[1], buf[2], buf[3]);
    return (VALUE_UNASSIGNED);
}

/*
 * execute_swapmode() swaps bytes in the specified buffer according to the
 *                    specified swap operation. An automatic swap mode must
 *                    first be converted to a swap operation by
 *                    detect_swapmode().
 *
 * @param  [io]  buf      - Buffer to modify.
 * @param  [in]  len      - Length of data in the buffer.
 * @param  [in]  swapmode - Swap operation to perform (0123, 3210, etc)
 * @return       None.
 */
static void
execute_swapmode(uint8_t *buf, uint len, uint swapmode)
{
    uint    pos;
    uint8_t temp;

    switch (swapmode) {
        case 0:
        case 123:
        case 0123:
        default:
            return;  // Normal (no swap)
        case 1032:
            /* Swap adjacent bytes in 16-bit words */
            for (pos = 0; pos + 1 < len; pos += 2) {
                temp         = buf[pos + 0];
                buf[pos + 0] = buf[pos + 1];
                buf[pos + 1] = temp;
            }
            return;
        case 2301:
            /* Swap adjacent (16-bit) words */
            for (pos = 0; pos + 3 < len; pos += 4) {
                temp         = buf[pos + 0];
                buf[pos + 0] = buf[pos + 2];
                buf[pos + 2] = temp;
                temp         = buf[pos + 1];
                buf[pos + 1] = buf[pos + 3];
                buf[pos + 3] = temp;
            }
            return;
        case 3210:
            /* Swap bytes in 32-bit longs */
            for (pos = 0; pos + 3 < len; pos += 4) {
                temp         = buf[pos + 0];
                buf[pos + 0] = buf[pos + 3];
                buf[pos + 3] = temp;
                temp         = buf[pos + 1];
                buf[pos + 1] = buf[pos + 2];
                buf[pos + 2] = temp;
            }
            return;
    }
}

static uint
read_from_flash(uint bank, uint addr, void *buf, uint len)
{
    uint rc;
    uint rc2;
    uint16_t bankarg = bank;
    SUPERVISOR_STATE_ENTER();
    INTERRUPTS_DISABLE();
    CACHE_DISABLE_DATA();
    MMU_DISABLE();

#undef USE_OVERLAY
#ifdef USE_OVERLAY
    *CIAA_PRA |= CIAA_PRA_OVERLAY | CIAA_PRA_LED;
#endif
    rc = send_cmd_core_retry(KS_CMD_BANK_SET | KS_BANK_SETTEMP,
                             &bankarg, sizeof (bankarg), NULL, 0, NULL);
    cia_spin(6);
#ifdef USE_OVERLAY
    local_memcpy(buf, (void *) (addr), len);
    *CIAA_PRA &= ~(CIAA_PRA_OVERLAY | CIAA_PRA_LED);
#else
    local_memcpy(buf, (void *) (ROM_BASE + addr), len);
#endif
    /* Don't OR status codes together; that produces a meaningless code */
    rc2 = send_cmd_core_retry(KS_CMD_BANK_SET | KS_BANK_UNSETTEMP,
                              &bankarg, sizeof (bankarg), NULL, 0, NULL);
    if (rc == 0)
        rc = rc2;
    cia_spin(CIA_USEC(1000));

    CACHE_FLUSH();
    MMU_RESTORE();
    CACHE_RESTORE_STATE();
    INTERRUPTS_ENABLE();
    SUPERVISOR_STATE_EXIT();
    return (rc);
}

static int
wait_for_flash_done(uint addr, uint erase_mode, uint32_t expected)
{
    uint32_t status;
    uint32_t cstatus = 0;
    uint32_t lstatus;
    uint     spins = erase_mode ? 1000000 : 50000; // 1 sec or 50ms
    uint     spin_count = 0;
    int      same_count = 0;
    int      see_fail_count = 0;

    cia_spin(2);
    lstatus = *VADDR32(addr);
    while (spin_count < spins) {
        status = *VADDR32(addr);

        cstatus = status;
        /* Filter out checking of status which is already done */
        if (((cstatus ^ lstatus) & 0x0000ffff) == 0)
            cstatus &= ~0x0000ffff;
        if (((cstatus ^ lstatus) & 0xffff0000) == 0)
            cstatus &= ~0xffff0000;

        if (status == lstatus) {
            if (same_count++ >= 1) {
                /* Same for 2 tries */
                if (status == expected) {
                    return (0);  // Done
                }
                /* Something went wrong -- block protected? */
                return (MSG_STATUS_PRG_FAIL);
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
    uint rc2;
    uint xlen;
    uint cmd_tries = 0;
    uint write_tries = 0;
    uint8_t *xbuf = buf;
    uint16_t bankarg = bank;
    uint fail_data = 0;
    uint32_t wdata;

    SUPERVISOR_STATE_ENTER();
    INTERRUPTS_DISABLE();
    CACHE_DISABLE_DATA();
    MMU_DISABLE();

    /* Switch to flash bank to be programmed */
    rc = send_cmd_core_retry(KS_CMD_BANK_SET | KS_BANK_SETTEMP,
                             &bankarg, sizeof (bankarg), NULL, 0, NULL);
    cia_spin(CIA_USEC(100));
    if (rc != 0) {
        goto write_give_up;
    }

    /* Write flash data */
    while (len > 0) {
        xlen = len;
        if (xlen > 4)
            xlen = 4;

        /*
         * Always program a full 32-bit value. A final partial value is
         * padded with the bytes already in flash, which programming leaves
         * unchanged. This keeps the comparison and status polling below
         * from using bytes beyond the end of the source buffer.
         */
        if (xlen < 4) {
            wdata = *VADDR32(ROM_BASE + addr);
            memcpy(&wdata, xbuf, xlen);
        } else {
            wdata = *ADDR32(xbuf);
        }

        /*
         * A typical Kickstart ROM has 0xffffffff values. This can
         * improve flash write performance by about 4%.
         */
        if (wdata == *VADDR32(ROM_BASE + addr))
            goto skip_write;  // Destination already has value

        rc = flash_cmd_core(KS_CMD_FLASH_WRITE, &wdata, sizeof (wdata));
        if (rc != 0) {
            uint count;
            for (count = 0; count < 10; count++) {
                *VADDR32(ROM_BASE + addr);  // Generate address for write
                cia_spin(10);
            }
            flash_read_mode(0);
            if (cmd_tries++ < 5)
                continue;  // Try again
            break;         // Give up
        }
        cmd_tries = 0;

        *VADDR32(ROM_BASE + addr);  // Generate address for write
        *VADDR32(ROM_BASE + addr);  // Generate address for write
        rc = wait_for_flash_done(ROM_BASE + addr, 0, wdata);
        if (rc != 0) {
            if (write_tries++ < 5) {
                /*
                 * Try again after forcing read mode and verifying that
                 * the same or more bits are set in the destination.
                 */
                flash_read_mode(1);  // Force "try hard"
                cia_spin(10);
                if ((wdata & *VADDR32(ROM_BASE + addr)) == wdata) {
                    continue;  // Retry
                }
            }
            break;
        }
        if (flag_flash_write_bug &&
            ((addr & 0xff) == 0x54)) {  // 0x1554 and variants
            /*
             * Some flash parts have a bug where writes to the 0xAAA
             * offset and variants could get interpreted as a command,
             * potentially corrupting flash. Work around this by
             * stomping on the false "command" with a READ command.
             */
            flash_read_mode(0);
        }
        if (flag_conservative_mode &&
            ((*VADDR32(ROM_BASE + addr) != wdata) ||
             (*VADDR32(ROM_BASE + addr) != wdata))) {
            rc = MSG_STATUS_MISMATCH;
            break;
        }
        write_tries = 0;

skip_write:
        len  -= xlen;
        xbuf += xlen;
        addr += xlen;
    }
    cia_spin(CIA_USEC(10));
    if (rc != 0)
        fail_data = *VADDR32(ROM_BASE + addr);

    /* Restore flash to read mode */
    rc2 = flash_read_mode(1);  // Force "try harder"
    if (rc == 0)
        rc = rc2;
    cia_spin(CIA_USEC(10));

write_give_up:
    /* Return to "current" flash bank */
    rc2 = send_cmd_core_retry(KS_CMD_BANK_SET | KS_BANK_UNSETTEMP,
                              &bankarg, sizeof (bankarg), NULL, 0, NULL);
    if (rc == 0)
        rc = rc2;
    cia_spin(CIA_USEC(1000));

    CACHE_FLUSH();
    MMU_RESTORE();
    CACHE_RESTORE_STATE();
    INTERRUPTS_ENABLE();
    SUPERVISOR_STATE_EXIT();

    if (rc != 0) {
        printf("\nWrite failure at %05x readback=%08x file=%08x ",
               addr, fail_data, *VADDR32(xbuf));
    }
    return (rc);
}

static uint
erase_flash_block(uint bank, uint addr)
{
    uint rc;
    uint rc1;
    uint cmd_tries = 0;
    uint erase_tries = 0;
    uint16_t bankarg = bank;

    SUPERVISOR_STATE_ENTER();
    INTERRUPTS_DISABLE();
    CACHE_DISABLE_DATA();
    MMU_DISABLE();

    /* Switch to flash bank to be erased */
    rc = send_cmd_core_retry(KS_CMD_BANK_SET | KS_BANK_SETTEMP,
                             &bankarg, sizeof (bankarg), NULL, 0, NULL);
    cia_spin(CIA_USEC(100));
    if (rc != 0)
        goto erase_give_up;  // Never erase if the bank could not be selected

    /* Send erase command */
try_erase_again:
    while (cmd_tries++ < 5) {
        rc = flash_cmd_core(KS_CMD_FLASH_ERASE, NULL, 0);
        if (rc == 0)
            break;
        uint count;
        for (count = 0; count < 10; count++) {
            *VADDR32(ROM_BASE + addr);  // Generate address for erase
            cia_spin(5);
        }
        flash_read_mode(0);
    }

    if (rc == 0) {
        *VADDR32(ROM_BASE + addr);  // Generate address for erase
        rc = wait_for_flash_done(ROM_BASE + addr, 1, 0xffffffff);
        if (rc != 0) {
            if (erase_tries++ < 5) {
                /*
                 * Try again after forcing read mode.
                 */
                (void) flash_read_mode(0);
                cia_spin(10);
                cmd_tries = 0;
                goto try_erase_again;
            }
        }
    }
    /* Restore flash to read mode */
    rc1 = flash_read_mode(1);  // Force "try harder"
    if (rc == 0)
        rc = rc1;

    cia_spin(CIA_USEC(10));

erase_give_up:
    /* Return to "current" flash bank */
    rc1 = send_cmd_core_retry(KS_CMD_BANK_SET | KS_BANK_UNSETTEMP,
                              &bankarg, sizeof (bankarg), NULL, 0, NULL);
    if (rc == 0)
        rc = rc1;
    cia_spin(CIA_USEC(1000));

    CACHE_FLUSH();
    MMU_RESTORE();
    CACHE_RESTORE_STATE();
    INTERRUPTS_ENABLE();
    SUPERVISOR_STATE_EXIT();
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

static int
erase_flash(uint bank, uint addr, uint len, uint flag_yes)
{
    bank_info_t info;
    uint        rc;
    uint        rlen;
    uint        bank_sub;
    uint        bank_size;
    uint32_t    flash_dev1;
    uint32_t    flash_dev2;
    uint        flash_start_addr;
    uint        flash_end_addr;
    uint        flash_start_bsize;
    uint        flash_end_bsize;
    uint        dot_count = 0;
    uint        dot_iters = 1;
    uint        dot_max;
    uint        mode = 0;
    uint        tlen = 0;
    uint64_t    time_start;
    uint64_t    time_end;
    const char *id1;
    const char *id2;
    const chip_blocks_t *cb;
    const chip_blocks_t *cb2;

    rc = send_cmd(KS_CMD_BANK_INFO, NULL, 0, &info, sizeof (info), &rlen);
    if (rc != 0) {
        printf("Failed to get bank information: %s\n", smash_err(rc));
        return (rc);
    }

    bank_sub  = info.bi_merge[bank] & 0x0f;
    bank_size = ((info.bi_merge[bank] + 0x10) & 0xf0) << 15;  // size in bytes
    if (bank_sub != 0) {
        printf("Bank %u is part of a merged bank, but is not the "
               "first (use %u)\n", bank, bank - bank_sub);
        return (1);
    }

    if (addr >= bank_size) {
        /* Without this, the erase would land in the following bank */
        printf("Address 0x%x is beyond the end of the bank (size 0x%x)\n",
               addr, bank_size);
        return (1);
    }
    if (len == VALUE_UNASSIGNED) {
        /* Get length from size of bank */
        len = bank_size - addr;
    } else if (len == 0) {
        /* A zero length would compute an end address before the start */
        printf("Nothing to erase (length is 0)\n");
        return (1);
    } else if (len > bank_size) {
        printf("Specified length 0x%x is greater than bank size 0x%x\n",
               len, bank_size);
        return (1);
    } else if (len > bank_size - addr) {  // No 32-bit overflow of addr + len
        printf("Specified address + length (0x%x) overflows bank (size 0x%x)\n",
               addr + len, bank_size);
        return (1);
    }

    /* Acquire flash id to get block erase zones */
    rc = flash_id(&flash_dev1, &flash_dev2, &mode);
    if (rc != 0) {
        printf("Flash id failure (%s)\n", smash_err(rc));
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
    if (rc != 0)
        return (rc);

    cb = get_chip_block_info(flash_dev1);
    if (cb == NULL) {
        printf("Failed to determine erase block information for %08x\n",
               flash_dev1);
        return (MSG_STATUS_BAD_DATA);
    }
    if (mode == 32) {
        cb2 = get_chip_block_info(flash_dev2);
        if (cb2 == NULL) {
            printf("Failed to determine erase block information for %08x\n",
                   flash_dev2);
            return (MSG_STATUS_BAD_DATA);
        }
        if ((cb->cb_bbnum != cb2->cb_bbnum) ||
            (cb->cb_bsize != cb2->cb_bsize) ||
            (cb->cb_ssize != cb2->cb_ssize) ||
            (cb->cb_map != cb2->cb_map)) {
            printf("    Failure: flash device IDs are not compatible "
                   "(%08x %08x)\n", flash_dev1, flash_dev2);
            return (MSG_STATUS_BAD_DATA);
        }
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
            printf("\nKicksmash failure (%s)\n", smash_err(rc));
            break;
        }
        if (is_user_abort()) {
            rc = 2;
            break;
        }

        tlen += xlen;
        if (xlen > len)
            xlen = len;  // Never underflow the remaining length
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
        while (tlen >= MAX_CHUNK) {
            tlen -= MAX_CHUNK;
            if (++dot_count == dot_iters) {
                dot_count = 0;
                printf(".");
            }
        }
        time_end = smash_time();
        printf("]\nErase complete in ");
        print_us_diff(time_start, time_end);
    }
    return (rc);
}

/*
 * probe_swapmode() converts an automatic swap mode (SWAPMODE_AUTO) to
 *                  the swap operation to use for an entire read, write,
 *                  or verify. The ROM header at the start of the image
 *                  is probed once, here. Following chunks of the image
 *                  do not start with a ROM header, so they must not be
 *                  probed.
 *
 *                  A read probes the image which is in flash. A write or
 *                  verify probes the image which is in the file.
 *
 * @param  [in]  readmode - Non-zero if flash is being read to a file.
 * @param  [in]  filename - File to probe when not reading from flash.
 * @param  [in]  bank     - Flash bank to probe when reading from flash.
 * @param  [in]  addr     - Starting address within the flash bank.
 * @param  [in]  swapmode - Requested swap mode (0123, 3210, auto, etc)
 * @return       Swap operation to perform (0123, 1032, 2301, or 3210).
 * @return       VALUE_UNASSIGNED - failure, or ROM image was not recognized.
 */
static uint
probe_swapmode(uint readmode, const char *filename, uint bank, uint addr,
               uint swapmode)
{
    uint32_t hdr = 0;  // read_from_flash() copies 32-bit values
    uint     rc;

    if (swapmode != SWAPMODE_AUTO)
        return (swapmode);  // Not an automatic mode

    if (readmode) {
        /* The ROM header is at the start of the (first) bank */
        (void) addr;
        rc = read_from_flash(bank, 0, &hdr, sizeof (hdr));
        if (rc != 0) {
            printf("Kicksmash failure (%s)\n", smash_err(rc));
            return (VALUE_UNASSIGNED);
        }
    } else {
        FILE *file = fopen(filename, "r");
        int   bytes;

        if (file == NULL) {
            printf("Failed to open \"%s\", for read\n", filename);
            return (VALUE_UNASSIGNED);
        }
        bytes = fread(&hdr, 1, sizeof (hdr), file);
        fclose(file);
        if (bytes < (int) sizeof (hdr)) {
            printf("Failed to read %u bytes from %s\n",
                   (uint) sizeof (hdr), filename);
            return (VALUE_UNASSIGNED);
        }
    }
    swapmode = detect_swapmode((uint8_t *) &hdr, SWAP_FROM_ROM, swapmode);
    if ((swapmode == VALUE_UNASSIGNED) && readmode) {
        /*
         * A bank which is blank or does not hold a Kickstart image
         * can still be read (backed up). Copy it as-is.
         */
        printf("Reading without byte swap (swap 0123)\n");
        swapmode = 123;
    }
    return (swapmode);
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
    uint64_t    time_end;
    int         arg;
    int         pos;
    int         bytes;
    uint        flag_dump = 0;
    uint        addr = VALUE_UNASSIGNED;
    uint        bank = VALUE_UNASSIGNED;
    uint        len  = VALUE_UNASSIGNED;
    uint        start_addr;
    uint        start_bank;
    uint        start_len;
    uint        orig_addr;
    uint        file_is_stdio = 0;
    uint        rlen;
    uint        rc = 1;
    uint        bank_sub;
    uint        bank_size;
    FILE       *file = NULL;
    uint        swapmode = SWAPMODE_AUTO;  // auto swap
    uint        writemode = 0;
    uint        verifymode = 0;
    uint        readmode = 0;
    uint        dot_count = 1;
    uint        dot_iters = 1;
    uint        dot_max;
    uint8_t    *buf;
    uint8_t    *vbuf = NULL;

    /* First Just determine the read/write/verify mode */
    for (arg = 0; arg < argc; arg++) {
        ptr = long_to_short(argv[arg], long_to_short_readwrite,
                            ARRAY_SIZE(long_to_short_readwrite));
        if (*ptr == '-') {
            for (++ptr; *ptr != '\0'; ptr++) {
                switch (*ptr) {
                    case 'r':  // read
                        readmode = 1;
                        break;
                    case 'v':  // verify
                        verifymode = 1;
                        break;
                    case 'w':  // write
                        writemode = 1;
                        break;
                }
            }
        }
    }

    for (arg = 1; arg < argc; arg++) {
        ptr = long_to_short(argv[arg], long_to_short_readwrite,
                            ARRAY_SIZE(long_to_short_readwrite));
        if (*ptr == '-') {
            for (++ptr; *ptr != '\0'; ptr++) {
                switch (*ptr) {
                    case 'a':  // addr
                        if (++arg >= argc) {
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
                        if (++arg >= argc) {
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
                    case 'C':  // conservative
                        flag_conservative_mode ^= 1;
                        break;
                    case 'D':  // debug
                        flag_debug++;
                        break;
                    case 'd':  // dump
                        flag_dump++;
                        break;
                    case 'f':  // file
                        if (++arg >= argc) {
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
                        if (++arg >= argc) {
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
                        if (++arg >= argc) {
                            printf("smash %s %s requires an option\n",
                                   argv[0], ptr);
                            goto usage;
                        }
                        if (strcasecmp(argv[arg], "auto") == 0) {
                            swapmode = SWAPMODE_AUTO;
                            break;
                        }
                        pos = 0;
                        if ((sscanf(argv[arg], "%u%n", &swapmode, &pos) != 1) ||
                            (pos == 0) || (argv[arg][pos] != '\0') ||
                            ((swapmode != 123) && (swapmode != 1032) &&
                             (swapmode != 2301) && (swapmode != 3210))) {
                            printf("Invalid argument \"%s\" for %s %s\n",
                                   argv[arg], argv[0], ptr);
                            printf("Use auto, 0123, 1032, 2301, or 3210\n");
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

    if (addr >= bank_size) {
        printf("Address 0x%x is beyond the end of the bank (size 0x%x)\n",
               addr, bank_size);
        return (1);
    }
    if (writemode && ((addr & 3) != 0)) {
        /* Flash is programmed in 32-bit units */
        printf("Write address 0x%x must be a multiple of 4\n", addr);
        return (1);
    }
    if (len == VALUE_UNASSIGNED) {
        if (readmode) {
            /* Get length from the remaining size of the bank */
            len = bank_size - addr;
        } else {
            /* Get length from size of file */
            len = get_file_size(filename);
            if (len == VALUE_UNASSIGNED) {
                return (1);
            }
        }
    }
    if (len == 0) {
        /* For write, erase_flash() can't handle a zero length */
        printf("Nothing to do (length is 0)\n");
        return (1);
    } else if (len > bank_size) {
        printf("Length 0x%x is greater than bank size 0x%x\n",
               len, bank_size);
        return (1);
    } else if (len > bank_size - addr) {  // No 32-bit overflow of addr + len
        printf("Length 0x%x + address overflows bank (size 0x%x)\n",
               addr + len, bank_size);
        return (1);
    }

    if ((!readmode || verifymode) && file_is_stdio) {
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

    /*
     * An automatic swap mode is converted here, once, to the swap operation
     * which is then used for every chunk of the read, write, and verify.
     */
    swapmode = probe_swapmode(readmode, filename, bank, addr, swapmode);
    if (swapmode == VALUE_UNASSIGNED)
        return (1);

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


    if (writemode) {
        /*
         * erase_flash() requires the first bank of a merged range and
         * the offset within that range, so it must be called before
         * bank and addr are converted to a ROM window bank below.
         */
        if (flag_yes || are_you_sure("Erase area before write")) {
            if (erase_flash(bank, addr, len, 1)) {
                rc = 1;
                goto fail_end;
            }
        }
    }

    orig_addr = addr;  // Offset within the (possibly merged) bank
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
        time_start = smash_time();
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
                    printf("\nKicksmash failure (%s)\n", smash_err(rc));
                    break;
                }
            }

            execute_swapmode(buf, xlen, swapmode);

            if (writemode) {
                /* Write to flash */
                rc = write_to_flash(bank, addr, buf, xlen);
                if (rc != 0) {
                    printf("\nKicksmash failure (%s)\n", smash_err(rc));
                    break;
                }
            } else {
                /* Output to file or stdout */
                if (file_is_stdio) {
                    /* Offset continues past each 512K ROM window */
                    dump_memory((uint32_t *)buf, xlen,
                                orig_addr + (start_len - len));
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
        if (!file_is_stdio && (rc == 0)) {
            time_end = smash_time();
            printf("]\n%s complete in ", writemode ? "Write" : "Read");
            print_us_diff(time_start, time_end);
        }
    }

    if (verifymode && (rc == 0)) {
        time_start = smash_time();
        dot_count = 0;
        if (!file_is_stdio) {
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
                printf("\nKicksmash failure (%s)\n", smash_err(rc));
                break;
            }
            execute_swapmode(buf, xlen, swapmode);

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
                /* Compare a trailing partial longword byte by byte */
                for (pos = xlen & ~3; pos < xlen; pos++) {
                    if (buf[pos] != vbuf[pos]) {
                        if (rc++ < 5) {
                            printf("    %05x: %02x != file %02x\n",
                                   addr + pos, buf[pos], vbuf[pos]);
                        }
                    }
                }
                printf("    %u miscompares in this block\n", rc);
                if (rc == 0)
                    rc = 1;  // memcmp() differed; never report success
                goto fail_end;
            }
            if ((!file_is_stdio) && (++dot_count == dot_iters)) {
                dot_count = 0;
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
        if (!file_is_stdio && (rc == 0)) {
            time_end = smash_time();
            printf("]\n%s complete in ", "Verify");
            print_us_diff(time_start, time_end);
        }
    }

fail_end:
    if ((file != NULL) && !file_is_stdio)
        fclose(file);
    FreeMem(buf, MAX_CHUNK);
    if (vbuf != NULL)
        FreeMem(vbuf, MAX_CHUNK);
    return (rc);
}

/*
 * cmd_erase
 * ---------
 * Erase flash
 */
int
cmd_erase(int argc, char *argv[])
{
    const char *ptr;
    uint        addr = VALUE_UNASSIGNED;
    uint        bank = VALUE_UNASSIGNED;
    uint        len  = VALUE_UNASSIGNED;
    uint        rc = 1;
    int         arg;
    int         pos;

    for (arg = 1; arg < argc; arg++) {
        ptr = long_to_short(argv[arg], long_to_short_erase,
                            ARRAY_SIZE(long_to_short_erase));
        if (*ptr == '-') {
            for (++ptr; *ptr != '\0'; ptr++) {
                switch (*ptr) {
                    case 'a':  // addr
                        if (++arg >= argc) {
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
                        if (++arg >= argc) {
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
                        if (++arg >= argc) {
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

    return (erase_flash(bank, addr, len, flag_yes));
}
