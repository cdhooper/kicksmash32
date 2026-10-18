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
#include <stdint.h>
#include "smash_cmd.h"
#include "sm_msg.h"
#include "med_cmdline.h"
#include "smash.h"
#include "smash_flash.h"
#include "serial.h"
#include "timer.h"
#include "util.h"
#include "cpu_control.h"

#define send_cmd_core esend_cmd_core

#define TEXT_TO_RAM  __attribute__((section(".text_to_ram")))
#define CONST_TO_RAM const __attribute__((section(".data")))

#undef DEBUG_FLASH_SEQUENCE
#ifdef DEBUG_FLASH_SEQUENCE
uint32_t flash_debugdata[8];
uint32_t flash_debugaddr[8];
uint8_t  flash_debugcount;
#endif

#define cia_spin ecia_spin

/*
 * flash_cmd_core
 * --------------
 * Must be called with interrupts and cache disabled
 *
 * This function will send a command to the flash after setting the
 * transaction up with the Kicksmash CPU.
 */
TEXT_TO_RAM
static int
flash_cmd_core(uint32_t cmd, void *arg, uint argsize)
{
    uint32_t addr;
    uint32_t addrs[64];
    uint     retry = 0;
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

    cia_spin(CIA_USEC(20));

    addr = ROM_BASE + ((addrs[0] << smash_cmd_shift) & 0x7ffff);
    (void) *VADDR32(addr);  // Generate OE strobe to kick off DMA
    cia_spin(CIA_USEC(1));

    for (pos = 0; pos < num_addr; pos++) {
        uint32_t addr = ROM_BASE + ((addrs[pos] << smash_cmd_shift) & 0x7ffff);
        uint32_t val = *VADDR32(addr);  // Generate address on the bus
        if ((pos == 0) && (val == 0xffffffff)) {
            if (retry++ > 5) {
                rc = MSG_STATUS_NO_REPLY;  // Timed out waiting for Kicksmash
                goto flash_cmd_cleanup;
            }
#ifdef DEBUG_FLASH_SEQUENCE
            if (*ADDR32(0x77820) < retry)
                *ADDR32(0x77820) = retry;
#endif
            pos--;
            continue;
        }
#ifdef DEBUG_FLASH_SEQUENCE
        /* Debug this particular command sequence */
        if ((cmd == KS_CMD_FLASH_ID) || (cmd == KS_CMD_FLASH_CMD)) {
            if (pos < ARRAY_SIZE(flash_debugdata)) {
                *ADDR32(0x77800 + pos * 4) = addr;
                *ADDR32(0x77810 + pos * 4) = val;
                flash_debugdata[pos] = val;
                flash_debugaddr[pos] = addr;
                flash_debugcount = pos + 1;
            }
        }
#endif
    }

flash_cmd_cleanup:
    cia_spin(CIA_USEC(1));
    if (rc != 0) {
        /* Attempt to drain data and wait for Kicksmash to enable flash */
        for (pos = 0; pos < 500; pos++) {
            (void) *VADDR32(ROM_BASE + 4);
            cia_spin(CIA_USEC(10));
        }
        /* Wait 10 ms longer */
        for (pos = 0; pos < 10; pos++)
            cia_spin(CIA_USEC(1000));
    }
    return (rc);
}

/*
 * rom_wait_normal() waits until ROM is back in normal mode.
 */
TEXT_TO_RAM
static uint
rom_wait_normal(void)
{
    uint timeout = 0;
    while (*VADDR32(ROM_BASE) != rom_header_expected) {
        if (timeout++ > 100)
            return (1);  // Took too long.
        cia_spin(CIA_USEC(10));
    }
    return (0);
}

/*
 * flash_read_mode() returns the flash to normal read mode.
 *
 * expect_rom_signature says whether to expect the Kickstart ROM signature
 * at offset 0.
 */
TEXT_TO_RAM
static int
flash_read_mode(uint expect_rom_signature)
{
    uint count;
    uint pos;
    int rc = flash_cmd_core(KS_CMD_FLASH_READ, NULL, 0);
    if ((rc != 0) || (expect_rom_signature && rom_wait_normal()))  {
        /* Try harder to restore flash to normal read mode */
        for (count = 0; count < 10; count++) {
            for (pos = 0; pos < 10; pos++)
                (void) *VADDR16(ROM_BASE + 8);
            cia_spin(CIA_USEC(1000));
            if ((flash_cmd_core(KS_CMD_FLASH_READ, NULL, 0) == 0) &&
                ((expect_rom_signature == 0) || (rom_wait_normal() == 0))) {
                rc = 0;
                break;
            }
        }
    }
    return (rc);
}

TEXT_TO_RAM
int
flash_id_data(uint32_t *data, uint data_count)
{
    uint     pos;
    int      rc1;
    int      rc2;

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

#undef TEST_CMD_FLASH_CMD
#ifdef TEST_CMD_FLASH_CMD
    uint32_t values[] = { 0x00555, 0x002aa, 0x00555,
                          0x00aa00aa, 0x00550055, 0x00900090 };
    rc1 = flash_cmd_core(KS_CMD_FLASH_CMD, values, sizeof (values));
#else
    rc1 = flash_cmd_core(KS_CMD_FLASH_ID, NULL, 0);
#endif
    /* Workaround for cold STM32 not releasing OEWE in time */
    (void) *VADDR16(ROM_BASE + 32);
    (void) *VADDR16(ROM_BASE + 36);
    (void) *VADDR16(ROM_BASE + 40);
    (void) *VADDR16(ROM_BASE + 44);
    cia_spin(CIA_USEC(5));

    /* Get flash ID data */
    for (pos = 0; pos < data_count; pos++)
        data[pos] = *VADDR32(ROM_BASE + pos * 4);

    rc2 = flash_read_mode(1);

    CACHE_FLUSH();
    MMU_RESTORE();
    CACHE_RESTORE_STATE();
    INTERRUPTS_ENABLE();
    SUPERVISOR_STATE_EXIT();
    return (rc1 ? rc1 : rc2);
}

/* Set up pointer to function */
int (*eflash_id_data)(uint32_t *data, uint data_count) = flash_id_data;
