/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2024.
 *
 * ---------------------------------------------------------------------
 *
 * Erase and write STM32 internal flash memory
 */

#include "printf.h"
#include "board.h"
#include "main.h"
#include "stm32flash.h"
#include "cmdline.h"
#include <string.h>
#include <libopencm3/cm3/cortex.h>
#include <libopencm3/stm32/flash.h>
#include <libopencm3/stm32/common/flash_common_idcache.h>

#define FL_PAGE_SIZE 2048

#define flash_lock()         FLASH_CR |= FLASH_CR_LOCK;
#define flash_unlock()       FLASH_KEYR = FLASH_KEYR_KEY1; \
                             FLASH_KEYR = FLASH_KEYR_KEY2;

// flash_get_status_flags()
#define flash_status_flags() (FLASH_SR & (FLASH_SR_PGERR | FLASH_SR_EOP | \
                                          FLASH_SR_WRPRTERR | FLASH_SR_BSY))

static void
flash_wait_for_done(void)
{
    while ((FLASH_SR & FLASH_SR_BSY) == FLASH_SR_BSY)
        ;
}

/*
 * flash_page_erase_ram
 * --------------------
 * Performs the page erase. This runs from RAM with interrupts masked,
 * because the GD32F107 stalls forever if the CPU fetches from flash while
 * a page erase is in progress.
 */
__attribute__((noinline, long_call, section(".ramtext")))
static void
flash_page_erase_ram(uint32_t page)
{
    while ((FLASH_SR & FLASH_SR_BSY) == FLASH_SR_BSY)
        ;
    FLASH_CR |= FLASH_CR_PER;
    FLASH_AR = page;
    FLASH_CR |= FLASH_CR_STRT;
    while ((FLASH_SR & FLASH_SR_BSY) == FLASH_SR_BSY)
        ;
    FLASH_CR &= ~FLASH_CR_PER;
}

/*
 * flash_page_erase
 * ----------------
 * Erases the flash page containing the specified offset, and returns
 * non-zero if the page was not erased. FLASH_AR requires the bus address
 * of the page. The GD32F107 does not erase anything if given the offset.
 */
static int
flash_page_erase(uint32_t addr)
{
    uint32_t *ptr;
    uint32_t  page = (addr & ~(FL_PAGE_SIZE - 1)) | FLASH_BASE;
    uint32_t  irq_mask;

    irq_mask = cm_mask_interrupts(1);
    flash_page_erase_ram(page);
    cm_mask_interrupts(irq_mask);

    for (ptr = (uint32_t *) page; ptr < (uint32_t *) (page + FL_PAGE_SIZE);
         ptr++)
        if (*ptr != 0xffffffff)
            return (1);
    return (0);
}

static int
flash_write16(uint32_t addr, uint16_t data)
{
    if (*(uint16_t *) addr == data)
        return (0);  // Data already matches

    flash_wait_for_done();

    FLASH_CR |= FLASH_CR_PG;
    MMIO16(addr) = data;

    flash_wait_for_done();

    FLASH_CR &= ~FLASH_CR_PG;
    return (MMIO16(addr) != data);
}

static int
flash_write32(uint32_t addr, uint32_t data)
{
    return (flash_write16(addr + 0, (uint16_t) data) |
            flash_write16(addr + 2, (uint16_t) (data >> 16)));
}

int
stm32flash_erase(uint32_t addr, uint len)
{
    uint elen;
    int  rc = 0;

    if ((addr >= 0x40000) || (len > 0x40000 - addr))
        return (RC_BAD_PARAM);

    flash_unlock();
    while (len > 0) {
        if (flash_page_erase(addr) != 0)
            rc = RC_FAILURE;
        elen = FL_PAGE_SIZE;
        if ((addr & (FL_PAGE_SIZE - 1)) != 0)
            elen = ((addr + FL_PAGE_SIZE - 1) & ~(FL_PAGE_SIZE - 1)) - addr;
        if (elen > len)
            elen = len;

        addr += elen;
        len  -= elen;
    }
    flash_lock();
    return (rc);
}

int
stm32flash_read(uint32_t addr, uint len, void *buf)
{
    if (addr + len > 0x40000)
        return (RC_BAD_PARAM);
    memcpy(buf, (void *)((uint)addr | FLASH_BASE), len);
    return (0);
}

int
stm32flash_write(uint32_t addr, uint len, void *buf, uint flags)
{
    int      rc   = 0;
    uint8_t *bufp = (uint8_t *) buf;

    if (addr + len > 0x40000)
        return (RC_BAD_PARAM);

    if (flags & STM32FLASH_FLAG_AUTOERASE) {
        /*
         * Erase each page which begins within the area to be written.
         * A page which begins before the write area is not erased, as
         * it may hold data written by a previous call.
         */
        uint32_t page = (addr + FL_PAGE_SIZE - 1) & ~(FL_PAGE_SIZE - 1);
        if (page < addr + len) {
            rc = stm32flash_erase(page, addr + len - page);
            if (rc != 0)
                return (rc);
        }
    }
    addr += FLASH_BASE;

    flash_unlock();
    while (len > 0) {
        uint plen;

        if (addr & 1)
            plen = 1;
        else if (((addr & 2) == 0) && (len >= 4))
            plen = 4;
        else if (len >= 2)
            plen = 2;
        else
            plen = 1;

        switch (plen) {
            case 1: {
                /*
                 * Flash can only be programmed a halfword at a time, at
                 * an even address. Merge the byte into its halfword
                 * (little-endian: an odd address is the high byte).
                 */
                uint32_t haddr = addr & ~1;
                uint16_t val = *(uint16_t *) haddr;
                if (addr & 1)
                    val = (val & 0x00ff) | (*bufp << 8);
                else
                    val = (val & 0xff00) | *bufp;
                rc += flash_write16(haddr, val);
                break;
            }
            case 2:
                rc += flash_write16(addr, *(uint16_t *) bufp);
                break;
            case 4:
                rc += flash_write32(addr, *(uint32_t *) bufp);
                break;
        }
        addr += plen;
        bufp += plen;
        len  -= plen;
    }
    flash_lock();

    FLASH_ACR &= ~FLASH_ACR_DCEN;  // flash_dcache_disable
    FLASH_ACR |= FLASH_ACR_DCRST;  // flash_dcache_reset
    FLASH_ACR |= FLASH_ACR_DCEN;   // flash_dcache_enable

    return (rc);
}
