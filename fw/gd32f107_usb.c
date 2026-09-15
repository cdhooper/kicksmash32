/*
 * This file is mostly code taken directly from usb_dwc_common.c of
 * the libopencm3 project as of July 19, 2026. The changes are for
 * support of GD32F107, specifically handling of the hardware's broken
 * USB OUT FIFO interface.
 *
 * Copyright (C) 2011 Gareth McMullin <gareth@blacksphere.co.nz>
 * Copyright (C) 2024-2025 Rachel Mant <git@dragonmux.network>
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include <libopencm3/cm3/common.h>
#include <libopencm3/stm32/tools.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/usb/usbd.h>
#include <libopencm3/usb/bos.h>
#include <libopencm3/usb/dwc/otg_fs.h>
#include "usb_private.h"
#include "usb_dwc_common.h"
#include "gd32f107_usb.h"

#define RX_FIFO_SIZE 38U /* 152 bytes */

#define dev_base_address (usbd_dev->driver->base_address)
#define REBASE(x)        MMIO32((x) + (dev_base_address))

#define USB_FIFO_BASE ((volatile uint32_t *) 0x50020000)

static void dwc_rx_fifo_reset(void);
static uint32_t dwc_rx_fifo_pop(usbd_device *const usbd_dev, int is_header);

void uart_putchar(int ch);
void uart_puts(const char *str);
int sprintf(char *buf, const char *fmt, ...);

/*
 * These two globals would ideally be in the usbd_device stucture,
 * so that one could instantiate multiple devices on hardware which
 * supports them. Those definitions are owned by libopencm3, so it's
 * not possible without modifying libopencm3.
 */
static uint16_t rxfifo_cons;
static uint16_t rxfifo_cons_next = 0xffff;

static void
dwc_rx_fifo_reset(void)
{
    rxfifo_cons = 0;
    rxfifo_cons_next = 0xffff;
}

#if 0
static uint32_t
dwc_rx_fifo_pop(usbd_device *const usbd_dev, int is_header)
{
    if (is_header)
        return (REBASE(OTG_GRXSTSP));
    else
        return (REBASE(OTG_FIFO(0)));
}
#else
static uint32_t
dwc_rx_fifo_pop(usbd_device *const usbd_dev, int is_header)
{
    uint16_t rx_fifo_size = usbd_dev->driver->rx_fifo_size;
    uint32_t popped;
    uint32_t value2;
    uint32_t rvalue;

    if (is_header) {
        popped = REBASE(OTG_GRXSTSP);
        /* Align to previously calculated end of the last packet */
        if ((rxfifo_cons_next != 0xffff) &&
            (rxfifo_cons != rxfifo_cons_next)) {
            rxfifo_cons = rxfifo_cons_next;
        }
        rvalue = USB_FIFO_BASE[rxfifo_cons];
        if (rvalue != popped) {
            value2 = USB_FIFO_BASE[rxfifo_cons];
{
            char buf[80];
            sprintf(buf, " E %x %08x %08x %08x\n", rxfifo_cons, rvalue, popped, value2);
            uart_puts(buf);
}
            if (popped == value2) {
                /* Use the original popped value */
                rvalue = value2;
            }
        }

        uint16_t rxbcnt = (rvalue & OTG_GRXSTSP_BCNT_MASK) >> 4U;
        uint16_t words  = 1 + ((rxbcnt + 3) >> 2);   // status + data words

        rxfifo_cons_next = rxfifo_cons + words;
        if (rxfifo_cons_next >= rx_fifo_size)
            rxfifo_cons_next -= rx_fifo_size;
    } else {
        rvalue = USB_FIFO_BASE[rxfifo_cons];
        popped = REBASE(OTG_FIFO(0));
        if (popped != rvalue) {
            value2 = USB_FIFO_BASE[rxfifo_cons];
            if (popped == value2) {
                /* Use the original popped value */
                rvalue = value2;
            }
        }
    }

    /* one word consumed (status or data) */
    if (++rxfifo_cons >= rx_fifo_size)
        rxfifo_cons = 0;
    return (rvalue);
}
#endif

static void dwc_flush_txfifo(usbd_device *usbd_dev, uint8_t ep);

static void
gd32_dwc_endpoints_reset(usbd_device *const usbd_dev)
{
    dwc_rx_fifo_reset();
    dwc_endpoints_reset(usbd_dev);
}

static uint16_t gd32_dwc_ep_write_packet(
	usbd_device *const usbd_dev, const uint8_t endpoint_address, const void *const buffer, const uint16_t length)
{
	const uint8_t ep = endpoint_address & 0x7fU;
	/* Return if endpoint is already enabled */
	if ((REBASE(OTG_DIEPCTL(ep)) & (OTG_DIEPCTL0_EPENA | OTG_DIEPCTL0_EPDIS | OTG_DIEPCTL0_NAKSTS)) ==
		OTG_DIEPCTL0_EPENA) {
		return 0U;
	}
	/* If it's still enabled but being NAK'd, flush FIFO and reset */
	if ((REBASE(OTG_DIEPCTL(ep)) & OTG_DIEPCTL0_EPENA) != 0U) {
		dwc_flush_txfifo(usbd_dev, ep);
		/* Disable the endpoint and wait for it to become actually disabled */
		REBASE(OTG_DIEPCTL(ep)) |= OTG_DIEPCTL0_EPDIS;
		while ((REBASE(OTG_DIEPINT(ep)) & OTG_DIEPINTX_EPDISD) == 0U)
			continue;
		REBASE(OTG_DIEPINT(ep)) = OTG_DIEPINTX_EPDISD;
	}
#if 1
        /* Trigger flush of endpoint's transmit FIFO -- required for GD32F107 */
	const uint32_t fifo = (REBASE(OTG_DIEPCTL(ep)) & OTG_DIEPCTL0_TXFNUM_MASK) >> 22;
	REBASE(OTG_GRSTCTL) = (fifo << 6U) | OTG_GRSTCTL_TXFFLSH;
#endif

	/* Configure the endpoint to accept the new packet */
	if (ep == 0U)
		REBASE(OTG_DIEPTSIZ0) = OTG_DIEPSIZ0_PKTCNT | (length & OTG_DIEPSIZ0_XFRSIZ_MASK);
	else
		REBASE(OTG_DIEPTSIZ(ep)) = OTG_DIEPSIZX_MCNT_1 | OTG_DIEPSIZX_PKTCNT(1) | (length & OTG_DIEPSIZX_XFRSIZ_MASK);
	/* Arm the endpoint for send */
	REBASE(OTG_DIEPCTL(ep)) |= OTG_DIEPCTL0_EPENA | OTG_DIEPCTL0_CNAK;

	/* Figure out how many bytes can be written as u32 chunks */
	const size_t aligned_length = length & ~3U;
#ifdef __ARM_ARCH_6M__
	if (((uintptr_t)buffer & 0x3U) == 0U) {
#endif
		/* Copy what we can into the FIFO for this endpoint in u32 blocks */
		for (size_t offset = 0U; offset < aligned_length; offset += 4U)
			REBASE(OTG_FIFO(ep)) = ((const uint32_t *)buffer)[offset >> 2U];
#ifdef __ARM_ARCH_6M__
	} else {
		const uint8_t *const buffer8 = buffer;
		/* Copy the data into the FIFO for this endpoint in u32 blocks using memcpy to work around alignment issues */
		for (size_t offset = 0U; offset < aligned_length; offset += 4U) {
			uint32_t data;
			memcpy(&data, buffer8 + offset, 4U);
			REBASE(OTG_FIFO(ep)) = data;
		}
	}
#endif
	/* If there's some data left over at the end, do the final copy */
	if (length - aligned_length) {
		/* Prepare the data block for the FIFO */
		uint32_t data = 0U;
		memcpy(&data, (const uint8_t *)buffer + aligned_length, length - aligned_length);
		/* Push the prepared data into the FIFO to complete transfer setup */
		REBASE(OTG_FIFO(ep)) = data;
	}

	/* Return that we wrote the whole packet out */
	return length;
}

static uint16_t gd32_dwc_ep_read_packet(
	usbd_device *const usbd_dev, const uint8_t endpoint_address, void *const buffer, const uint16_t length)
{
	/* We do not need to know the endpoint address since there is only one receive FIFO for all endpoints. */
	(void)endpoint_address;
	/* Figure out how many bytes to read, and how many can be read as u32 chunks */
	const size_t count = MIN(length, usbd_dev->rxbcnt);
	const size_t aligned_count = count & ~3U;

	/* ARMv7-M and newer supports non-word-aligned accesses, ARMv6-M does not. */
#ifdef __ARM_ARCH_6M__
	if (((uintptr_t)buffer & 0x3U) == 0U) {
#endif
		/* Copy the data out of the FIFO for this endpoint in u32 blocks */
		for (size_t offset = 0U; offset < aligned_count; offset += 4U) {
			const uint32_t data = dwc_rx_fifo_pop(usbd_dev, 0);
			((uint32_t *)buffer)[offset >> 2U] = data;
                }
#ifdef __ARM_ARCH_6M__
	} else {
		uint8_t *const buffer8 = buffer;
		/* Copy the data out of the FIFO for this endpoint in u32 blocks using memcpy to work around alignment issues */
		for (size_t offset = 0U; offset < aligned_count; offset += 4U) {
			const uint32_t data = REBASE(OTG_FIFO(0U));
			memcpy(buffer8 + offset, &data, 4U);
		}
	}
#endif

	/* If theres some data left over at the end, do the final copy */
	if (count - aligned_count) {
		/* Extract the last data block from the FIFO */
		const uint32_t data = dwc_rx_fifo_pop(usbd_dev, 0);
		/* Copy the data for this final transfer into the target location in the buffer */
		memcpy((uint8_t *)buffer + aligned_count, &data, count - aligned_count);
		/* Because of how unloading works, we unload a bit more than this would ideally want */
		if (usbd_dev->rxbcnt <= aligned_count + 4U)
			usbd_dev->rxbcnt = 0U; /* If we exhausted the data, set to 0 */
		else
			usbd_dev->rxbcnt -= count + 4U;
	} else
		/* All's said and done, so drop the read count by the amount read and return */
		usbd_dev->rxbcnt -= count;
	return count;
}

static void dwc_flush_txfifo(usbd_device *const usbd_dev, const uint8_t ep)
{
	/* Mark the endpoint to NAK and wait for it to become active */
	REBASE(OTG_DIEPCTL(ep)) |= OTG_DIEPCTL0_SNAK;
	while ((REBASE(OTG_DIEPINT(ep)) & OTG_DIEPINTX_INEPNE) == 0U) {
	}
	/* Figure out which FIFO is in use for this endpoint */
	const uint32_t fifo = (REBASE(OTG_DIEPCTL(ep)) & OTG_DIEPCTL0_TXFNUM_MASK) >> 22;
	/* Wait for core to idle */
	while ((REBASE(OTG_GRSTCTL) & OTG_GRSTCTL_AHBIDL) == 0U) {
	}
	/* Flush the FIFO in quest */
	REBASE(OTG_GRSTCTL) = (fifo << 6U) | OTG_GRSTCTL_TXFFLSH;
	while ((REBASE(OTG_GRSTCTL) & OTG_GRSTCTL_TXFFLSH) != 0U) {
		/* idle */
	}
	/* Reset packet queing size information */
	REBASE(OTG_DIEPTSIZ(ep)) = 0U;
}

static void gd32_dwc_poll(usbd_device *const usbd_dev)
{
	const uint32_t status = REBASE(OTG_GINTSTS) & REBASE(OTG_GINTMSK);
	/* First check to see if we're here for a USB reset event */
	if (status & OTG_GINTSTS_USBRST) {
		/* Do an endpoint reset, make sure EP0 is set up, and clear the condition */
                usbd_dev->driver->ep_reset(usbd_dev);
		_usbd_reset(usbd_dev);
		REBASE(OTG_GINTSTS) = OTG_GINTSTS_USBRST;
		/* Exit early as we're done here */
		return;
	}

	/* Now check to see if we're here for an enumeration done event */
	if (status & OTG_GINTSTS_ENUMDNE) {
		/* There's nothing much to do here, this interrupt just indicates that the link speed is now set */
		REBASE(OTG_GINTSTS) = OTG_GINTSTS_ENUMDNE;
		return;
	}

	/*
	 * There is not always a global interrupt flag for transmit complete.
	 * The XFRC bit must be checked in each OTG_DIEPINT(x).
	 *
	 * Iterate over the IN endpoints, triggering any post-transmit actions.
	 */
	if (status & OTG_GINTSTS_IEPINT) {
		for (size_t ep = 0U; ep < ENDPOINT_COUNT; ++ep) {
			/* If this endpoint has a completion, process it */
			if (REBASE(OTG_DIEPINT(ep)) & OTG_DIEPINTX_XFRC) {
				/* Mark the endpoint for NAK so we don't cause a protocol error */
				REBASE(OTG_DIEPCTL(ep)) |= OTG_DIEPCTL0_SNAK;
				/* Call any callback that might be available */
				if (usbd_dev->user_callback_ctr[ep][USB_TRANSACTION_IN]) {
					usbd_dev->user_callback_ctr[ep][USB_TRANSACTION_IN](usbd_dev, ep);
				}
			}
			/* Clear any and all interrupt notifications on this endpoint */
			REBASE(OTG_DIEPINT(ep)) = REBASE(OTG_DIEPINT(ep));
		}
	}

	/* Handle OUT packet reception */
	while (REBASE(OTG_GINTSTS) & OTG_GINTSTS_RXFLVL) {
		/* Pop the RX packet status from the stack and decode */
		const uint32_t rx_status = dwc_rx_fifo_pop(usbd_dev, 1);
		const uint32_t phase = rx_status & OTG_GRXSTSP_PKTSTS_MASK;
		const uint8_t ep = rx_status & OTG_GRXSTSP_EPNUM_MASK;
		usbd_dev->rxbcnt = (rx_status & OTG_GRXSTSP_BCNT_MASK) >> 4U;

		switch (phase) {
		case OTG_GRXSTSP_PKTSTS_SETUP_COMP:
			/* Packet is for completion of a SETUP transaction, call the callback for this */
			if (usbd_dev->user_callback_ctr[ep][USB_TRANSACTION_SETUP])
				usbd_dev->user_callback_ctr[ep][USB_TRANSACTION_SETUP](usbd_dev, ep);
			else
				usbd_dev->user_callback_ctr[0][USB_TRANSACTION_SETUP](usbd_dev, 0);
			break;
		case OTG_GRXSTSP_PKTSTS_SETUP:
			/* Packet is a SETUP packet, check if there's anything stuck in the TX FIFO to flush */
			if ((REBASE(OTG_DIEPTSIZ(ep)) & OTG_DIEPSIZ0_PKTCNT) != 0U) {
				dwc_flush_txfifo(usbd_dev, ep);
			}
			/* Having made sure we're in a sensible state, now dequeue the data */
			usbd_dev->driver->ep_read_packet(usbd_dev, ep, &usbd_dev->control_state.req, sizeof(usbd_dev->control_state.req));
			break;
		case OTG_GRXSTSP_PKTSTS_OUT:
			/* Call the user's handler if present */
			if (usbd_dev->user_callback_ctr[ep][USB_TRANSACTION_OUT]) {
				usbd_dev->user_callback_ctr[ep][USB_TRANSACTION_OUT](usbd_dev, ep);
			}
			break;
		default:
			break;
		}

		/* Discard any straggling data for this packet that wasn't yet handled */
		for (size_t offset = 0; offset < usbd_dev->rxbcnt; offset += 4U) {
			/* There is only one receive FIFO, so use OTG_FS_FIFO(0) */
			(void)REBASE(OTG_FIFO(0));
		}
		usbd_dev->rxbcnt = 0U;

		/* If this is for a completion, re-arm the endpoint, preserving ACK state */
		if (phase == OTG_GRXSTSP_PKTSTS_SETUP_COMP || phase == OTG_GRXSTSP_PKTSTS_OUT_COMP) {
			REBASE(OTG_DOEPTSIZ(ep)) = usbd_dev->doeptsiz[ep];
			REBASE(OTG_DOEPCTL(ep)) |=
				OTG_DOEPCTL0_EPENA | (usbd_dev->force_nak[ep] ? OTG_DOEPCTL0_SNAK : OTG_DOEPCTL0_CNAK);
		}
	}

	/* Deal with any endpoint interrupts that are outstanding */
	const uint32_t endpoints_status = REBASE(OTG_DAINT) & REBASE(OTG_DAINTMSK);
	/* Handle the OUT endpoints */
	if (status & OTG_GINTSTS_OEPINT) {
		uint16_t endpoints_mask = (uint16_t)(endpoints_status >> 16U);
		uint8_t ep = 0U;
		while (endpoints_mask != 0U) {
			/* If there's an interrupt set on this endpoint */
			if (endpoints_mask & 1U) {
				/* Clear it */
				REBASE(OTG_DOEPINT(ep)) = REBASE(OTG_DOEPINT(ep));
			}

			/* Advance to the next endpoint */
			endpoints_mask >>= 1U;
			++ep;
		}
	}

	/* Process suspend and wakeup interrupts */
	if (status & OTG_GINTSTS_USBSUSP) {
		if (usbd_dev->user_callback_suspend) {
			usbd_dev->user_callback_suspend();
		}
		REBASE(OTG_GINTSTS) = OTG_GINTSTS_USBSUSP;
	}
	if (status & OTG_GINTSTS_WKUPINT) {
		if (usbd_dev->user_callback_resume) {
			usbd_dev->user_callback_resume();
		}
		REBASE(OTG_GINTSTS) = OTG_GINTSTS_WKUPINT;
	}

	/* Handle SOF notifications */
	if (status & OTG_GINTSTS_SOF) {
		if (usbd_dev->user_callback_sof) {
			usbd_dev->user_callback_sof();
		}
		REBASE(OTG_GINTSTS) = OTG_GINTSTS_SOF;
	}

#if !defined(STM32H7) && !defined(STM32U5)
	if (usbd_dev->user_callback_sof) {
		REBASE(OTG_GINTMSK) |= OTG_GINTMSK_SOFM;
	} else {
		REBASE(OTG_GINTMSK) &= ~OTG_GINTMSK_SOFM;
	}
#endif
}

struct _usbd_driver gd32f107_usb_driver = {
    .init = NULL,
    .set_address = dwc_set_address,
    .ep_setup = dwc_ep_setup,
    .ep_reset = gd32_dwc_endpoints_reset,
    .ep_stall_set = dwc_ep_stall_set,
    .ep_stall_get = dwc_ep_stall_get,
    .ep_nak_set = dwc_ep_nak_set,
    .ep_write_packet = gd32_dwc_ep_write_packet,
    .ep_read_packet = gd32_dwc_ep_read_packet,
    .poll = gd32_dwc_poll,
    .disconnect = dwc_disconnect,
    .base_address = USB_OTG_FS_BASE,
    .set_address_before_status = true,
    .rx_fifo_size = RX_FIFO_SIZE,
};

void
gd32f107_usb_init(void)
{
    gd32f107_usb_driver.init = stm32f107_usb_driver.init;
}
