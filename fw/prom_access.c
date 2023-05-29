/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2020.
 *
 * ---------------------------------------------------------------------
 *
 * EEPROM high level access code for MX29F1615 programmer.
 */

#include <stdbool.h>
#include <string.h>
#include "main.h"
#include "cmdline.h"
#include "prom_access.h"
#include "m29f160xt.h"
#include "printf.h"
#include "uart.h"
#include "timer.h"
#include "crc32.h"

#define DATA_CRC_INTERVAL 256

static rc_t
prom_read_32(uint32_t addr, uint width, uint8_t *buf)
{
    uint8_t tbuf[4];

    /* Handle odd start address */
    if (addr & 3) {
        uint copylen = 4 - (addr & 3);

        if (ee_read(addr >> 2, tbuf, 1))
            return (RC_FAILURE);
        if (copylen > width)
            copylen = width;
        memcpy(buf, tbuf + (addr & 3), copylen);
        addr  += copylen;
        buf   += copylen;
        width -= copylen;
    }

    /* Handle main body of data */
    if (ee_read(addr >> 2, buf, width >> 2))
        return (RC_FAILURE);

    /* Handle trailing byte(s) */
    if (width & 3) {
        addr += (width & ~3);
        buf  += (width & ~3);
        if (ee_read(addr >> 2, tbuf, 1))
            return (RC_FAILURE);
        memcpy(buf, tbuf, width & 3);
    }

    return (RC_SUCCESS);
}

static rc_t
prom_read_16(uint32_t addr, uint width, uint8_t *buf)
{
    uint8_t tbuf[2];

    if (addr & 1) {
        /* Handle odd start address */
        if (ee_read(addr >> 1, tbuf, 1))
            return (RC_FAILURE);
        *(buf++) = tbuf[1];
        addr++;
        width--;
    }

    /* Handle main body of data */
    if (ee_read(addr >> 1, buf, width >> 1))
        return (RC_FAILURE);

    if (width & 1) {
        /* Handle odd trailing byte */
        buf  += (width & ~1);
        addr += (width & ~1);
        if (ee_read(addr >> 1, tbuf, 1))
            return (RC_FAILURE);
        *buf = tbuf[0];
    }

    return (RC_SUCCESS);
}

rc_t
prom_read(uint32_t addr, uint width, void *bufp)
{
    uint8_t *buf = (uint8_t *) bufp;

#undef DEBUG_PROM_READ
#ifdef DEBUG_PROM_READ
    int pos;
    for (pos = 0; pos < width; pos++)
        buf[pos] = (uint8_t) (pos + addr);
    return (RC_SUCCESS);
#endif

    ee_enable();

    /* Handle 32-bit mode separate from 16-bit modes */
    if (ee_mode == EE_MODE_32)
        return (prom_read_32(addr, width, buf));
    else
        return (prom_read_16(addr, width, buf));
}

static rc_t
prom_write_16(uint32_t addr, uint width, uint8_t *buf)
{
    uint8_t tbuf[2];
    if (addr & 1) {
        /* Handle odd start address */
        if (ee_read(addr >> 1, tbuf, 1))
            return (RC_FAILURE);
        tbuf[1] = *buf;
        if (ee_write(addr >> 1, tbuf, 1))
            return (RC_FAILURE);
        buf++;
        addr++;
        width--;
    }

    /* Handle main body of data */
    if (ee_write(addr >> 1, buf, width >> 1))
        return (RC_FAILURE);

    if (width & 1) {
        /* Handle odd trailing byte */
        buf  += width - 1;
        addr += width;
        if (ee_read(addr >> 1, tbuf, 1))
            return (RC_FAILURE);
        tbuf[0] = *buf;
        if (ee_write(addr >> 1, tbuf, 1))
            return (RC_FAILURE);
    }

    return (RC_SUCCESS);
}

static rc_t
prom_write_32(uint32_t addr, uint width, uint8_t *buf)
{
    uint8_t tbuf[2];

    if (addr & 3) {
        /* Handle odd start address */
        uint copylen = 3 - (addr & 3);

        if (ee_read(addr >> 2, tbuf, 1))
            return (RC_FAILURE);
        if (copylen > width)
            copylen = width;
        memcpy(tbuf + (addr & 3), buf, copylen);
        if (ee_write(addr >> 2, tbuf, 1))
            return (RC_FAILURE);
        addr  += copylen;
        buf   += copylen;
        width -= copylen;
    }

    /* Handle main body of data */
    if (ee_write(addr >> 2, buf, width >> 2))
        return (RC_FAILURE);

    /* Handle trailing byte(s) */
    if (width & 3) {
        addr += width;
        buf  += (width & ~3);
        if (ee_read(addr >> 2, tbuf, 1))
            return (RC_FAILURE);
        memcpy(tbuf, buf, width & 3);
        if (ee_write(addr >> 2, tbuf, 1))
            return (RC_FAILURE);
    }

    return (RC_SUCCESS);
}

rc_t
prom_write(uint32_t addr, uint width, void *bufp)
{
    ee_enable();
    if (ee_mode == EE_MODE_32)
        return (prom_write_32(addr, width, bufp));
    else
        return (prom_write_16(addr, width, bufp));
}

rc_t
prom_erase(uint mode, uint32_t addr, uint32_t len)
{
    ee_enable();
    return (ee_erase(mode, addr >> 1, len >> 1, 1));
}

void
prom_cmd(uint32_t addr, uint16_t cmd)
{
    ee_enable();
    ee_cmd(addr, cmd);
}

void
prom_id(void)
{
    uint32_t part1;
    uint32_t part2;
    ee_enable();
    ee_id(&part1, &part2);

    switch (ee_mode) {
        case EE_MODE_16_LOW:
        case EE_MODE_16_HIGH:
            printf("%08lx\n", part1);
            break;
        case EE_MODE_32:
            printf("%08lx %08lx\n", part1, part2);
            break;
    }
}

void
prom_status(void)
{
    char status[64];
    ee_enable();
    printf("%04x %s\n", ee_status_read(status, sizeof (status)), status);
}

void
prom_status_clear(void)
{
    ee_enable();
    ee_status_clear();
}

static int
getchar_wait(uint pos)
{
    int      ch;
    uint64_t timeout = timer_tick_plus_msec(200);

    while ((ch = getchar()) == -1)
        if (timer_tick_has_elapsed(timeout))
            break;

    return (ch);
}

static int
check_crc(uint32_t crc, uint spos, uint epos, bool send_rc)
{
    int      ch;
    size_t   pos;
    uint32_t compcrc;

    for (pos = 0; pos < sizeof (compcrc); pos++) {
        ch = getchar_wait(200);
        if (ch == -1) {
            printf("Receive timeout waiting for CRC %08lx at 0x%x\n",
                   crc, epos);
            return (RC_TIMEOUT);
        }
        ((uint8_t *)&compcrc)[pos] = ch;
    }
    if (crc != compcrc) {
        printf("Received CRC %08lx doesn't match %08lx at 0x%x-0x%x\n",
               compcrc, crc, spos, epos);
        return (1);
    }
    return (0);
}

static int
check_rc(uint pos)
{
    int ch = getchar_wait(200);
    if (ch == -1) {
        printf("Receive timeout waiting for rc at 0x%x\n", pos);
        return (RC_TIMEOUT);
    }
    if (ch != 0) {
        printf("Remote sent error %d at 0x%x\n", ch, pos);
        return (RC_FAILURE);
    }
    return (RC_SUCCESS);
}

/*
 * prom_read_binary() reads data from an EEPROM and writes it to the host.
 *                    Every 256 bytes, a rolling CRC value is expected back
 *                    from the host.
 */
rc_t
prom_read_binary(uint32_t addr, uint32_t len)
{
    rc_t     rc;
    uint8_t  buf[256];
    uint32_t crc = 0;
    uint     crc_next = DATA_CRC_INTERVAL;
    uint32_t cap_pos[4];
    uint     cap_count = 0;
    uint     cap_prod  = 0;  // producer
    uint     cap_cons  = 0;  // consumer
    uint     pos = 0;

    ee_enable();
    while (len > 0) {
        uint32_t tlen = sizeof (buf);
        if (tlen > len)
            tlen = len;
        if (tlen > crc_next)
            tlen = crc_next;
        rc = prom_read(addr, tlen, buf);
        if (puts_binary(&rc, 1)) {
            printf("Status send timeout at %lx\n", addr + pos);
            return (RC_TIMEOUT);
        }
        if (rc != RC_SUCCESS)
            return (rc);
        if (puts_binary(buf, tlen)) {
            printf("Data send timeout at %lx\n", addr + pos);
            return (RC_TIMEOUT);
        }

        crc = crc32(crc, buf, tlen);
        crc_next -= tlen;
        addr     += tlen;
        len      -= tlen;
        pos      += tlen;

        if (cap_count >= ARRAY_SIZE(cap_pos)) {
            /* Verify received RC */
            cap_count--;
            if (check_rc(cap_pos[cap_cons]))
                return (RC_FAILURE);
            if (++cap_cons >= ARRAY_SIZE(cap_pos))
                cap_cons = 0;
        }

        if (crc_next == 0) {
            /* Send and record the current CRC value */
            if (puts_binary(&crc, sizeof (crc))) {
                printf("Data send CRC timeout at %lx\n", addr + pos);
                return (RC_TIMEOUT);
            }
            cap_pos[cap_prod] = pos;
            if (++cap_prod >= ARRAY_SIZE(cap_pos))
                cap_prod = 0;
            cap_count++;
            crc_next = DATA_CRC_INTERVAL;
        }
    }
    if (crc_next != DATA_CRC_INTERVAL) {
        /* Send CRC for last partial segment */
        if (puts_binary(&crc, sizeof (crc)))
            return (RC_TIMEOUT);
    }

    /* Verify trailing CRC packets */
    cap_prod += ARRAY_SIZE(cap_pos) - cap_count;
    if (cap_prod >= ARRAY_SIZE(cap_pos))
        cap_prod -= ARRAY_SIZE(cap_pos);

    while (cap_count-- > 0) {
        if (check_rc(cap_pos[cap_cons]))
            return (1);
        if (++cap_cons >= ARRAY_SIZE(cap_pos))
            cap_cons = 0;
    }

    if (crc_next != DATA_CRC_INTERVAL) {
        /* Verify CRC for last partial segment */
        if (check_rc(pos))
            return (RC_FAILURE);
    }
    return (RC_SUCCESS);
}

/*
 * prom_write_binary() takes binary input from an application via the serial
 *                     console and writes that to the EEPROM. Every 256 bytes,
 *                     a rolling 8-bit CRC value is sent back to the host.
 *                     This is so the host knows that the data was received
 *                     correctly. Incorrectly received data will still be
 *                     written to the EEPROM.
 */
rc_t
prom_write_binary(uint32_t addr, uint32_t len)
{
    uint8_t  buf[128];
    int      ch;
    rc_t     rc;
    uint32_t crc = 0;
    uint32_t saddr = addr;
    uint     crc_next = DATA_CRC_INTERVAL;

    ee_enable();
    while (len > 0) {
        uint32_t tlen    = len;
        uint32_t rem     = addr & (sizeof (buf) - 1);
        uint64_t timeout = timer_tick_plus_msec(1000);
        uint32_t pos;
        uint8_t *ptr = buf;

        if (tlen > sizeof (buf) - rem)
            tlen = sizeof (buf) - rem;

        for (pos = 0; pos < tlen; pos++) {
            while ((ch = getchar()) == -1)
                if (timer_tick_has_elapsed(timeout)) {
                    printf("Data receive timeout at %lx\n", addr + pos);
                    rc = RC_TIMEOUT;
                    goto fail;
                }
            timeout = timer_tick_plus_msec(1000);
            *(ptr++) = ch;
            crc = crc32(crc, ptr - 1, 1);
            if (--crc_next == 0) {
                if (check_crc(crc, saddr, addr + pos + 1, false)) {
                    rc = RC_FAILURE;
                    goto fail;
                }
                rc = RC_SUCCESS;
                if (puts_binary(&rc, 1)) {
                    rc = RC_TIMEOUT;
                    goto fail;
                }
                crc_next = DATA_CRC_INTERVAL;
                saddr = addr + pos + 1;
            }
        }
        rc = prom_write(addr, tlen, buf);
        if (rc != RC_SUCCESS) {
fail:
            (void) puts_binary(&rc, 1);  // Inform remote side
            timeout = timer_tick_plus_msec(2000);
            while (!timer_tick_has_elapsed(timeout))
                (void) getchar();  // Discard input
            return (rc);
        }
        addr += tlen;
        len  -= tlen;
    }
    if (crc_next != DATA_CRC_INTERVAL) {
        if (check_crc(crc, saddr, addr, false)) {
            rc = RC_FAILURE;
            goto fail;
        }
        rc = RC_SUCCESS;
        if (puts_binary(&rc, 1)) {
            rc = RC_TIMEOUT;
            goto fail;
        }
    }
    return (RC_SUCCESS);
}

void
prom_disable(void)
{
    ee_disable();
}

int
prom_verify(int verbose)
{
    return (ee_verify(verbose));
}

void
prom_show_mode(void)
{
    const char *mode;
    switch (ee_mode) {
        case 0:
            mode = "32-bit";
            break;
        case 1:
            mode = "16-bit low";
            break;
        case 2:
            mode = "16-bit high";
            break;
        default:
            mode = "unknown";
            break;
    }
    printf("%d = %s\n", ee_mode, mode);
}

void
prom_mode(uint mode)
{
    ee_mode = mode;
    ee_disable();
}

void
prom_snoop(void)
{
    printf("Press any key to exit\n");
    ee_snoop();
}
