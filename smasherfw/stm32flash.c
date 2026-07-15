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

static void
flash_page_erase(uint32_t addr)
{
    flash_wait_for_done();

    FLASH_CR |= FLASH_CR_PER;
    FLASH_AR = addr;
    FLASH_CR |= FLASH_CR_STRT;

    flash_wait_for_done();

    FLASH_CR &= ~FLASH_CR_PER;
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

    flash_unlock();
    while (len > 0) {
        flash_page_erase(addr);
        elen = FL_PAGE_SIZE;
        if ((addr & (FL_PAGE_SIZE - 1)) != 0)
            elen = ((addr + FL_PAGE_SIZE - 1) & ~(FL_PAGE_SIZE - 1)) - addr;
        if (elen > len)
            elen = len;

        addr += elen;
        len  -= elen;
    }
    flash_lock();
    return (0);
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

    if ((flags & STM32FLASH_FLAG_AUTOERASE) &&
        ((addr & (FL_PAGE_SIZE - 1)) == 0)) {
        /* Write at the start of a page */
        rc = stm32flash_erase(addr, len);
        if (rc != 0)
            return (rc);
    }
    addr += FLASH_BASE;

    flash_unlock();
    while (len > 0) {
        uint plen = len;

        if (addr & 1)
            plen = 1;
        else if ((addr & 2) && (plen != 1))
            plen = 2;
        else if (plen > 4)
            plen = 4;

        switch (plen) {
            case 1: {
                uint16_t val = *(uint16_t *) addr;
                if (addr & 1)
                    val = (val & 0x00ff) | *bufp;
                else
                    val = (val & 0xff00) | *bufp;
                rc += flash_write16(addr, val);
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
