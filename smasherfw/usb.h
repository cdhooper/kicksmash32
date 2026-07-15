/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2020.
 *
 * ---------------------------------------------------------------------
 *
 * USB handling.
 */

#ifndef _MY_USBD_DESC_H
#define _MY_USBD_DESC_H

#include <libopencm3/usb/usbd.h>
#define USBD_OK 0U
extern usbd_device *usbd_gdev;

void usb_shutdown(void);
void usb_startup(void);
void usb_signal_reset_to_host(int restart);
void usb_poll(void);
void usb_poll_mode(void);
void usb_mask_interrupts(void);
void usb_unmask_interrupts(void);
void usb_show_regs(void);
void usb_show_stats(void);
uint16_t usb_current_address(void);

uint8_t CDC_Transmit_FS(uint8_t *buf, unsigned int len);

extern uint8_t usb_console_active;
extern unsigned int usb_send_timeouts;

/* libopencm3 */
// #include <libopencm3/stm32/memorymap.h>
#if defined(STM32F103xE)
#define USB_PERIPH_BASE USB_DEV_FS_BASE
#else
#define USB_PERIPH_BASE USB_OTG_FS_BASE
#endif

#endif /* _MY_USBD_DESC_H */
