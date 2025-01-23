/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2020.
 *
 * ---------------------------------------------------------------------
 *
 * STM32 USB Device Descriptor handling.
 */

#include "printf.h"
#include "board.h"
#include "utils.h"
#include <stdbool.h>
#include "usb.h"
#include "main.h"
#include "gpio.h"
#include "uart.h"
#include "timer.h"
#include "cmdline.h"

#undef DEBUG_NO_USB

#include <libopencm3/stm32/gpio.h>
#if defined(STM32F1)
#include <libopencm3/stm32/f1/gpio.h>
#include <libopencm3/stm32/f1/rcc.h>
#include <libopencm3/stm32/f1/nvic.h>
#ifdef STM32F103xE
#include <libopencm3/cm3/common.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/tools.h>
#include <libopencm3/stm32/st_usbfs.h>
#include <libopencm3/usb/usbd.h>
#endif
#elif defined(STM32F4)
#include <libopencm3/stm32/f4/rcc.h>
#include <libopencm3/stm32/f4/nvic.h>
#endif
#include <libopencm3/usb/usbstd.h>
#include <libopencm3/usb/cdc.h>
#include <libopencm3/usb/dwc/otg_fs.h>
#include <string.h>

#if defined(STM32F103xE)
#define USB_INTERRUPT NVIC_USB_LP_CAN_RX0_IRQ
#else
#define USB_INTERRUPT NVIC_OTG_FS_IRQ
#endif

#define USB_MAX_EP0_SIZE 64
#define USB_MAX_EP2_SIZE 64
#define DEVICE_CLASS_MISC 0xef
#define GPIO_OSPEED_LOW GPIO_MODE_OUTPUT_2_MHZ

#define USBD_IDX_MFC_STR           0x01U
#define USBD_IDX_PRODUCT_STR       0x02U
#define USBD_IDX_SERIAL_STR        0x03U
#define USBD_MAX_NUM_CONFIGURATION 0x01
#define USBD_MAX_STR_DESC_SIZ      0x100

#define USING_USB_INTERRUPT

/*
 * XXX: Need to register USB device ID at http://pid.codes once board files
 *      and source code have been published.
 */
#define USBD_MANUFACTURER_STRING        "eebugs"
#define USBD_VID                        0x1209
#define USBD_PID                        0x1610
#define BYTESPLIT(x)                    (uint8_t) (x), (uint8_t) ((x) >> 8)

#define DEVICE_CLASS_APPSPEC            0xfe  // Application-specific
#define DEVICE_CLASS_MISC               0xef  // Miscellaneous
#define DEVICE_SUBCLASS_MISC_COMMON     0x02

#define DEVICE_PROTOCOL_MISC_IAD        0x01  // Interface Association Descr.

#define USB_SIZE_DEVICE_DESC            0x12
#define USB_SIZE_STRING_LANGID          0x04
#define USBD_LANGID_STRING              0x409

/*
 * #define STM32F0_UDID_BASE            0x1ffff7ac
 * #define STM32F1_UDID_BASE            0x1ffff7e8
 * #define STM32F2_UDID_BASE            0x1fff7a10
 * #define STM32F3_UDID_BASE            0x1ffff7ac
 * #define STM32F4_UDID_BASE            0x1fff7a10
 * #define STM32F7_UDID_BASE            0x1ff0f420
 */
#define STM32_UDID_LEN                  12    // 96 bits
#define STM32_UDID_BASE                 DESIG_UNIQUE_ID_BASE

/* CDC includes the VCP endpoint and a Bulk transport */
#define USBD_CONFIGURATION_FS_STRING    "CDC Config"
#define USBD_INTERFACE_FS_STRING        "CDC Interface"

#define ARRAY_SIZE(x) (int)((sizeof (x) / sizeof ((x)[0])))

static bool using_usb_interrupt = false;
static uint8_t ep_send_zero = 0xff;
uint8_t usb_console_active = false;
uint  usb_drop_packets = 0;
uint  usb_drop_bytes = 0;
uint  usb_send_timeouts = 0;


/**
 * Device Descriptor for USB FS port
 */
__attribute__((aligned(4)))
static const uint8_t USBD_FS_DeviceDesc[USB_SIZE_DEVICE_DESC] = {
    USB_SIZE_DEVICE_DESC,        /* bLength */
    USB_DT_DEVICE,               /* bDescriptorType */
    BYTESPLIT(0x0200),           /* bcdUSB = 2.00 */
    DEVICE_CLASS_MISC,           /* bDeviceClass    (Misc)   */
    DEVICE_SUBCLASS_MISC_COMMON, /* bDeviceSubClass (common) */
    DEVICE_PROTOCOL_MISC_IAD,    /* bDeviceProtocol (IAD)    */
    USB_MAX_EP0_SIZE,            /* bMaxPacketSize */
    BYTESPLIT(USBD_VID),         /* idVendor */
    BYTESPLIT(USBD_PID),         /* idProduct */
    BYTESPLIT(0x0200),           /* bcdDevice rel. 2.00 */
    USBD_IDX_MFC_STR,            /* Index of manufacturer string */
    USBD_IDX_PRODUCT_STR,        /* Index of product string */
    USBD_IDX_SERIAL_STR,         /* Index of serial number string */
    USBD_MAX_NUM_CONFIGURATION   /* bNumConfigurations */
};

void
usb_mask_interrupts(void)
{
#ifdef DEBUG_NO_USB
    return;
#endif
    if (!using_usb_interrupt)
        return;
    nvic_disable_irq(USB_INTERRUPT);
}

void
usb_unmask_interrupts(void)
{
#ifdef DEBUG_NO_USB
    return;
#endif
    if (!using_usb_interrupt)
        return;
    nvic_enable_irq(USB_INTERRUPT);
}

/**
 * usbd_usr_serial() reads the STM32 Unique Device ID from the CPU's system
 *                   memory area of the Flash memory module.  It converts
 *                   the ID to a printable Unicode string which is suitable
 *                   for return response to the USB Get Serial Descriptor
 *                   request.
 *
 * @param [out] buf    - A buffer to hold the converted Unicode serial number.
 * @param [out] length - The length of the Unicode serial number.
 *
 * @return      The converted Unicode serial number.
 */
static uint8_t *
usbd_usr_serial(uint8_t *buf)
{
    uint len = 0;
    uint pos;

    for (pos = 0; pos < STM32_UDID_LEN; pos++) {
        uint8_t temp  = *ADDR8(STM32_UDID_BASE + pos);
        uint8_t ch_hi = (temp >> 4) + '0';
        uint8_t ch_lo = (temp & 0xf) + '0';

        if (temp == 0xff)
            continue;
        if ((temp >= '0') && (temp <= 'Z')) {
            /* Show ASCII directly */
            buf[len++] = temp;
            continue;
        }
        if (ch_hi > '9')
            ch_hi += 'a' - '0' - 10;
        if (ch_lo > '9')
            ch_lo += 'a' - '0' - 10;

        buf[len++] = ch_hi;
        buf[len++] = ch_lo;
    }
    buf[len++] = '\0';
    return (buf);
}

/**
 * USB descriptors
 */
/* Strings describing this USB device */
static const char *usb_strings[] = {
    USBD_MANUFACTURER_STRING,
    "KickSmash Prg",
    "",  // Serial number assigned at runtime
};

void
usb_shutdown(void)
{
}

void usb_poll(void)
{
#ifndef DEBUG_NO_USB
    if (!using_usb_interrupt)
        usbd_poll(usbd_gdev);
#endif
}

void
usb_signal_reset_to_host(int restart)
{
#ifdef DEBUG_NO_USB
    return;
#endif
#if 0
    rcc_periph_clock_enable(RCC_GPIOA);
    rcc_periph_clock_enable(RCC_GPIOB);
    rcc_periph_clock_enable(RCC_AFIO);

#endif
}

/* libopencm3 */
#if 0
static uint8_t *gbuf;                      // Start of current packet
static uint16_t glen;                      // Total length of current packet
static uint16_t gpos;                      // Progress position of current pkt
#endif

/*
 * CDC_Transmit_FS() is used to put data in the USB transmit FIFO so that
 *                   the hardware can provide the data to the host.
 *
 * Note that this function might get called for every character. The caller
 * is responsible for checking for error return (which might just mean that
 * the USB hardware is currently busy sending the previous packet).
 *
 * Do not send more than the configured maximum packet size (wMaxPacketSize)
 * as this will overrun hardware buffers.
 */
uint8_t
CDC_Transmit_FS(uint8_t *buf, uint len)
{
#ifndef DEBUG_NO_USB
    static uint usb_drops = 0;
    uint tlen = USB_MAX_EP2_SIZE;  // 64 bytes
    uint64_t timeout = 0;

    if (usb_console_active == false)
        return (-1);

    if (len == 0)
        return (USBD_OK);

#if 0
    /*
     * Transfer all data in USB_MAX_EP2_SIZE size packets.
     * If the last packet is exactly USB_MAX_EP2_SIZE size in bytes,
     * then a following zero-length packet must be sent.
     */
    while (len != 0) {
        uint count;
        if (tlen > len)
            tlen = len;
        usb_poll();
        usb_mask_interrupts();
        count = usbd_ep_write_packet(usbd_gdev, 0x82, buf, tlen);
        if ((len - count) == 0)
            ep_send_zero = 0x2;  // Callback will send final zero-length packet
        usb_unmask_interrupts();
        if (count == 0) {
            /* Wrote 0 bytes */
            if (timeout == 0) {
                timeout = timer_tick_plus_msec(50);
            } else if (timer_tick_has_elapsed(timeout)) {
                /* Try one last time */
                usb_mask_interrupts();
                count = usbd_ep_write_packet(usbd_gdev, 0x82, buf, tlen);
                if ((len - count) == 0)
                    ep_send_zero = 0x2;  // Send final zero-length packet
                usb_unmask_interrupts();
                if (count != 0)
                    goto transmit_success;
                usb_drop_packets++;
                usb_drop_bytes += tlen;
                return (-1);  // Timeout
            }
            continue;
        } else {
transmit_success:
            len -= count;
            buf += count;
            timeout = timer_tick_plus_msec(50);
        }
    }
#else
    /*
     * Transfer all data in USB_MAX_EP2_SIZE size packets.
     * If the last packet is exactly USB_MAX_EP2_SIZE size in bytes,
     * we will split that packet to avoid having to deal with the
     * terminating zero-length packet in the callback function.
     */
    while (len != 0) {
        uint count;
        if (tlen > len)
            tlen = len;
        if ((tlen == USB_MAX_EP2_SIZE) && (len == USB_MAX_EP2_SIZE)) {
            /* Last packet */
            tlen = USB_MAX_EP2_SIZE / 2;
        }
        usb_poll();
        usb_mask_interrupts();
        count = usbd_ep_write_packet(usbd_gdev, 0x82, buf, tlen);
        usb_unmask_interrupts();
        if (count == 0) {
            /* Wrote 0 bytes */
            if (timeout == 0) {
                timeout = timer_tick_plus_msec(50);
            } else if (timer_tick_has_elapsed(timeout)) {
                /* Try one last time */
                usb_mask_interrupts();
                count = usbd_ep_write_packet(usbd_gdev, 0x82, buf, tlen);
                usb_unmask_interrupts();
                if (count != 0)
                    goto transmit_success;
                usb_drop_packets++;
                usb_drop_bytes += tlen;
                if (usb_drops++ > 4)
                    usb_console_active = false;
                return (-1);  // Timeout
            }
            continue;
        } else {
transmit_success:
            len -= count;
            buf += count;
            timeout = timer_tick_plus_msec(50);
            usb_drops = 0;
        }
    }
#endif
#endif
    return (USBD_OK);
}

/*
 * This notification endpoint isn't implemented. According to CDC spec its
 * optional, but its absence causes a NULL pointer dereference in Linux
 * cdc_acm driver.
 */
static const struct usb_endpoint_descriptor comm_endp[] = {
    {
        .bLength          = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType  = USB_DT_ENDPOINT,
        .bEndpointAddress = 0x83,
        .bmAttributes     = USB_ENDPOINT_ATTR_INTERRUPT,
        .wMaxPacketSize   = 16,
        .bInterval        = 255,
    }
};

static const struct usb_endpoint_descriptor data_endp[] = {
    {
        .bLength          = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType  = USB_DT_ENDPOINT,
        .bEndpointAddress = 0x01,
        .bmAttributes     = USB_ENDPOINT_ATTR_BULK,
        .wMaxPacketSize   = 64,
        .bInterval        = 1,
    }, {
        .bLength          = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType  = USB_DT_ENDPOINT,
        .bEndpointAddress = 0x82,
        .bmAttributes     = USB_ENDPOINT_ATTR_BULK,
        .wMaxPacketSize   = 64,
        .bInterval        = 1,
    }
};

static const struct {
        struct usb_cdc_header_descriptor header;
        struct usb_cdc_call_management_descriptor call_mgmt;
        struct usb_cdc_acm_descriptor acm;
        struct usb_cdc_union_descriptor cdc_union;
} __attribute__((packed)) cdcacm_functional_descriptors = {
    .header = {
        .bFunctionLength    = sizeof(struct usb_cdc_header_descriptor),
        .bDescriptorType    = CS_INTERFACE,
        .bDescriptorSubtype = USB_CDC_TYPE_HEADER,
        .bcdCDC = 0x0110,
    },
    .call_mgmt = {
        .bFunctionLength    = sizeof(struct usb_cdc_call_management_descriptor),
        .bDescriptorType    = CS_INTERFACE,
        .bDescriptorSubtype = USB_CDC_TYPE_CALL_MANAGEMENT,
        .bmCapabilities     = 0,
        .bDataInterface     = 1,
    },
    .acm = {
        .bFunctionLength    = sizeof(struct usb_cdc_acm_descriptor),
        .bDescriptorType    = CS_INTERFACE,
        .bDescriptorSubtype = USB_CDC_TYPE_ACM,
        .bmCapabilities     = 0,
    },
    .cdc_union = {
        .bFunctionLength = sizeof(struct usb_cdc_union_descriptor),
        .bDescriptorType        = CS_INTERFACE,
        .bDescriptorSubtype     = USB_CDC_TYPE_UNION,
        .bControlInterface      = 0,
        .bSubordinateInterface0 = 1,
     }
};

static const struct usb_interface_descriptor comm_iface[] = {
    {
        .bLength = USB_DT_INTERFACE_SIZE,
        .bDescriptorType    = USB_DT_INTERFACE,
        .bInterfaceNumber   = 0,
        .bAlternateSetting  = 0,
        .bNumEndpoints      = 1,
        .bInterfaceClass    = USB_CLASS_CDC,
        .bInterfaceSubClass = USB_CDC_SUBCLASS_ACM,
        .bInterfaceProtocol = USB_CDC_PROTOCOL_AT,
        .iInterface         = 0,

        .endpoint           = comm_endp,

        .extra              = &cdcacm_functional_descriptors,
        .extralen           = sizeof (cdcacm_functional_descriptors)
    }
};

static const struct usb_interface_descriptor data_iface[] = {
    {
        .bLength = USB_DT_INTERFACE_SIZE,
        .bDescriptorType    = USB_DT_INTERFACE,
        .bInterfaceNumber   = 1,
        .bAlternateSetting  = 0,
        .bNumEndpoints      = 2,
        .bInterfaceClass    = USB_CLASS_DATA,
        .bInterfaceSubClass = 0,
        .bInterfaceProtocol = 0,
        .iInterface         = 0,

        .endpoint           = data_endp,
    }
};

static const struct usb_interface ifaces[] = {
    {
        .num_altsetting     = 1,
        .altsetting         = comm_iface,
    }, {
        .num_altsetting     = 1,
        .altsetting         = data_iface,
    }
};

static const struct usb_config_descriptor config = {
    .bLength = USB_DT_CONFIGURATION_SIZE,
    .bDescriptorType = USB_DT_CONFIGURATION,
    .wTotalLength        = 0,
    .bNumInterfaces      = 2,
    .bConfigurationValue = 1,
    .iConfiguration      = 0,
    .bmAttributes        = 0x80,
    .bMaxPower           = 0x32,

    .interface           = ifaces,
};

/* Buffer to be used for control requests. */
static uint8_t usbd_control_buffer[128];

#define USB_CDC_REQ_GET_LINE_CODING 0x21  // Not defined in libopencm3

static enum usbd_request_return_codes
cdcacm_control_request(usbd_device *usbd_dev, struct usb_setup_data *req,
                       uint8_t **buf, uint16_t *len,
                       void (**complete)(usbd_device *usbd_dev,
                       struct usb_setup_data *req))
{
    static struct usb_cdc_line_coding line_coding = {
        .dwDTERate   = 115200,
        .bCharFormat = USB_CDC_1_STOP_BITS,
        .bParityType = USB_CDC_NO_PARITY,
        .bDataBits   = 0x08,
    };
    switch (req->bRequest) {
        case USB_CDC_REQ_SET_CONTROL_LINE_STATE: {
            /*
             * This Linux cdc_acm driver requires this to be implemented
             * even though it's optional in the CDC spec, and we don't
             * advertise it in the ACM functional descriptor.
             */
            char local_buf[10];
            struct usb_cdc_notification *notif = (void *)local_buf;

            /* We echo signals back to host as notification. */
            notif->bmRequestType = 0xA1;
            notif->bNotification = USB_CDC_NOTIFY_SERIAL_STATE;
            notif->wValue = 0;
            notif->wIndex = 0;
            notif->wLength = 2;
            local_buf[8] = req->wValue & 3;
            local_buf[9] = 0;
            // usbd_ep_write_packet(usbd_gdev, 0x83, buf, 10);
            return (USBD_REQ_HANDLED);
        }
        case USB_CDC_REQ_SET_LINE_CODING:
            /* Windows 10 VCP driver requires this */
            if (*len < sizeof (struct usb_cdc_line_coding))
                return (USBD_REQ_NOTSUPP);
            memcpy(&line_coding, *buf, sizeof (struct usb_cdc_line_coding));
            return (USBD_REQ_HANDLED);
        case USB_CDC_REQ_GET_LINE_CODING:
            usbd_ep_write_packet(usbd_gdev, 0x83, &line_coding,
                                 sizeof (struct usb_cdc_line_coding));
            return (USBD_REQ_HANDLED);
    }
    return (USBD_REQ_NOTSUPP);
}

/*
 * cdcacm_rx_cb() gets called when the USB hardware has received data from
 *                the host on the data OUT endpoint (0x01).
 */
static void cdcacm_rx_cb(usbd_device *usbd_dev, uint8_t ep)
{
    char buf[64];
    int len = usbd_ep_read_packet(usbd_dev, 0x01, buf, sizeof (buf));

    if (len > 0) {
        int pos;
        usb_console_active = true;
        for (pos = 0; pos < len; pos++)
            usb_rb_put(buf[pos]);
    }
}

/*
 * cdcacm_tx_cb() gets called when the USB hardware has sent the previous
 *                frame on the IN endpoint (0x82). It can be used to continue
 *                sending queuing frames, but this code does not use it in
 *                that manner.
 */
static void cdcacm_tx_cb(usbd_device *usbd_dev, uint8_t ep)
{
    if (ep_send_zero == ep) {
        /*
         * If the final transfer was exactly a multiple of a maximum
         * packet size, then a zero length packet must be sent to
         * terminate the host request. That operation can only be
         * performed in this callback.
         */
        usbd_ep_write_packet(usbd_gdev, 0x82, NULL, 0);
        ep_send_zero = 0xff;
    }
#if 0
    if (preparing_packet)
        return;  // New transmit packet is being prepared

    if (gpos < glen) {
        uint16_t len = glen - gpos;
        if (len > 64)
            len = 64;

        if (usbd_ep_write_packet(usbd_dev, 0x82, gbuf + gpos, len) != 0) {
            uart_putchar('+');
            uart_putchar("0123456789abcdef"[gbuf[gpos] >> 4]);
            uart_putchar("0123456789abcdef"[gbuf[gpos] & 0xf]);
            gpos += len;
        }
    }
#endif
}

static void cdcacm_set_config(usbd_device *usbd_dev, uint16_t wValue)
{
    usbd_ep_setup(usbd_dev, 0x01, USB_ENDPOINT_ATTR_BULK, 64, cdcacm_rx_cb);
    usbd_ep_setup(usbd_dev, 0x82, USB_ENDPOINT_ATTR_BULK, 64, cdcacm_tx_cb);
    usbd_ep_setup(usbd_dev, 0x83, USB_ENDPOINT_ATTR_INTERRUPT, 16, NULL);

    usbd_register_control_callback(usbd_dev,
                                   USB_REQ_TYPE_CLASS | USB_REQ_TYPE_INTERFACE,
                                   USB_REQ_TYPE_TYPE | USB_REQ_TYPE_RECIPIENT,
                                   cdcacm_control_request);
}

#ifdef USING_USB_INTERRUPT
#ifdef STM32F103xE
void
usb_lp_can_rx0_isr(void)
{
    static uint16_t preg1 = 0;
    static uint16_t preg2 = 0;
    uint16_t        reg;

    usbd_poll(usbd_gdev);

    /* Detect and clear unhandled interrupt */
    reg = *USB_ISTR_REG;
    if (reg & preg1 & preg2) {
        *USB_ISTR_REG = ~(reg & preg1 & preg2);
#ifdef DEBUG_IRQS
        static bool complained = false;
        if (complained == false) {
            complained = true;
            printf("USB Unhandled IRQ %04x\n", reg & preg1 & preg2);
        }
#endif
    }

    preg2 = preg1;
    preg1 = reg;
}
#else /* STM32F4 / STM32F107 */
void
otg_fs_isr(void)
{
    usbd_poll(usbd_gdev);
}
#endif

static void
usb_enable_interrupts(void)
{
#if defined(STM32F103xE)
    *USB_CNTR_REG |= USB_CNTR_SOFM | USB_CNTR_SUSPM | USB_CNTR_RESETM |
                     USB_CNTR_ERRM | USB_CNTR_WKUPM | USB_CNTR_CTRM;
#elif defined(STM32F107xC)
    /* Is this correct? */
    OTG_FS_GOTGINT |= OTG_GOTGINT_DBCDNE | OTG_GOTGINT_ADTOCHG |
                      OTG_GOTGINT_HNGDET | OTG_GOTGINT_HNSSCHG |
                      OTG_GOTGINT_SRSSCHG | OTG_GOTGINT_SEDET;
#endif

    nvic_set_priority(USB_INTERRUPT, 0x40);
    nvic_enable_irq(USB_INTERRUPT);

    using_usb_interrupt = false;
    usb_poll();
    using_usb_interrupt = true;
}
#endif /* USING_USB_INTERRUPT */

__attribute__((aligned(4)))
uint8_t usb_serial_str[32];
usbd_device   *usbd_gdev = NULL;

void
usb_startup(void)
{
#ifdef DEBUG_NO_USB
    return;
#endif
    usbd_usr_serial(usb_serial_str);
    usb_strings[2] = (char *)usb_serial_str;

#ifdef STM32F4
    /* GPIO9 should be left as an INPUT */
    gpio_mode_setup(GPIOA, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO11 | GPIO12);
    gpio_set_af(GPIOA, GPIO_AF10, GPIO11 | GPIO12);

#define USB_DRIVER otgfs_usb_driver

#else
#ifdef STM32F103xE
#define USB_DRIVER st_usbfs_v1_usb_driver
    usb_signal_reset_to_host(1);
#else
#define USB_DRIVER stm32f107_usb_driver
#endif
#endif
    usbd_gdev = usbd_init(&USB_DRIVER,
                          (const struct usb_device_descriptor *)
                             &USBD_FS_DeviceDesc[0], &config,
                          usb_strings, ARRAY_SIZE(usb_strings),
                          usbd_control_buffer, sizeof (usbd_control_buffer));

    usbd_register_set_config_callback(usbd_gdev, cdcacm_set_config);

#ifdef USING_USB_INTERRUPT
    usb_enable_interrupts();
#else
    using_usb_interrupt = false;
#endif

#ifndef STM32F103xE
    /*
     * STM32F4 and STM32F105/STM32F107 have register with status indicating
     * that VBus was detected. This is a clearable status, so this code
     * relies on the USB interrupt handler not clearing that status.
     */
    if ((OTG_FS_GINTSTS & OTG_GINTSTS_SRQINT) == 0) {
        printf("    VBus not detected\n");
    }
#endif
}

typedef struct {
    uint16_t regs_offset;
    uint8_t  regs_start_end;
    uint8_t  regs_stride;
    char     regs_name[12];
} usb_regs_t;

#ifdef STM32F103xE
static const usb_regs_t usb_regs[] = {
    { 0x000, 0x08,    4, "EP%xR" },
    { 0x040, 0x01,    0, "CNTR" },
    { 0x044, 0x01,    0, "ISTR" },
    { 0x048, 0x01,    0, "FNR" },
    { 0x04c, 0x01,    0, "DADDR" },
    { 0x050, 0x01,    4, "BTABLE" },
};
#else /* STM32F4 / STM32F107 */
static const usb_regs_t usb_regs[] = {
    { 0x000, 0x01,    0, "GOTGCTL" },
    { 0x004, 0x01,    0, "GOTGINT" },
    { 0x008, 0x01,    0, "GAHBCFG" },
    { 0x00c, 0x01,    0, "GUSBCFG" },
    { 0x010, 0x01,    0, "GRSTCTL" },
    { 0x014, 0x01,    0, "GINTSTS" },
    { 0x018, 0x01,    0, "GINTMSK" },
    { 0x01c, 0x01,    0, "GRXSTSR" },
    { 0x024, 0x01,    0, "GRXFSIZ" },
    { 0x028, 0x01,    0, "DIEPTXF0" },
    { 0x02c, 0x01,    0, "HNPTXSTS" },
    { 0x038, 0x01,    0, "GCCFG" },
    { 0x03c, 0x01,    0, "CID" },
    { 0x100, 0x01,    0, "HPTXFSIZ" },
    { 0x104, 0x14,    4, "DIEPTXF%x" },
    { 0x800, 0x01,    0, "DCFG" },
    { 0x804, 0x01,    0, "DCTL" },
    { 0x808, 0x01,    0, "DSTS" },
    { 0x810, 0x01,    0, "DIEPMSK" },
    { 0x814, 0x01,    0, "DOEPMSK" },
    { 0x818, 0x01,    0, "DAINT" },
    { 0x81c, 0x01,    0, "DAINTMSK" },
    { 0x828, 0x01,    0, "DVBUSDIS" },
    { 0x82c, 0x01,    0, "DVBUSPULSE" },
    { 0x834, 0x01,    0, "DIEPEMPMSK" },
    { 0x900, 0x04, 0x20, "DIEPCTL%x" },
    { 0x908, 0x04, 0x20, "DIEPINT%x" },
    { 0x910, 0x04, 0x20, "DIEPTSIZ%x" },
    { 0x918, 0x04, 0x20, "DTXFSTS%x" },
    { 0xb00, 0x04, 0x20, "DOEPCTL%x" },
    { 0xb08, 0x04, 0x20, "DOEPINT%x" },
    { 0xb10, 0x04, 0x20, "DOEPTSIZ%x" },
    { 0xe00, 0x01,    0, "PCGCCTL" },
};
#endif

void
usb_show_regs(void)
{
    uint     pos;
    uint32_t base = USB_PERIPH_BASE;

    for (pos = 0; pos < ARRAY_SIZE(usb_regs); pos++) {
        uint32_t    addr  = base + usb_regs[pos].regs_offset;
        uint        start = usb_regs[pos].regs_start_end >> 4;
        uint        end   = usb_regs[pos].regs_start_end & 0xf;
        uint        cur;

        for (cur = start; cur < end; cur++) {
            char namebuf[12];
            snprintf(namebuf, sizeof (namebuf), usb_regs[pos].regs_name, cur);
            printf("  %-10s [%08lx] %08lx\n",
                   namebuf, addr, *ADDR32(addr));
            addr += usb_regs[pos].regs_stride;
        }
    }
#ifndef STM32F103xE
    uint32_t val = *ADDR32(base + 0x4); // GOTGINT
    printf("\n  GOTGINT %08lx", val);
    if (val & (1 << 19))
        printf(" DBCDNE");
    if (val & (1 << 18))
        printf(" ADTOCHG");
    if (val & (1 << 17))
        printf(" HNGDET");
    if (val & (1 << 8))
        printf(" SRSSCHG");
    if (val & (1 << 2))
        printf(" SEDET");

    val = *ADDR32(base + 0x14); // GINTSTS
    printf("\n  GINTSTS %08lx", val);
    if (val & (1 << 31))
        printf(" WKUPINT");
    if (val & (1 << 30))
        printf(" SRQINT");
    if (val & (1 << 29))
        printf(" DISCINT");
    if (val & (1 << 28))
        printf(" CIDSCHG");
    if (val & (1 << 26))
        printf(" PTXFE");
    if (val & (1 << 25))
        printf(" HCINT");
    if (val & (1 << 24))
        printf(" HPRTINT");
    if (val & (1 << 21))
        printf(" IPXFR");
    if (val & (1 << 20))
        printf(" IISOIXFR");
    if (val & (1 << 19))
        printf(" OEPINT");
    if (val & (1 << 18))
        printf(" IEPINT");
    if (val & (1 << 15))
        printf(" EOPF");
    if (val & (1 << 14))
        printf(" ISOODRP");
    if (val & (1 << 13))
        printf(" ENUMDNE");
    if (val & (1 << 12))
        printf(" USBRST");
    if (val & (1 << 11))
        printf(" USBSUSP");
    if (val & (1 << 10))
        printf(" ESUSP");
    if (val & (1 << 7))
        printf(" GONAKEFF");
    if (val & (1 << 6))
        printf(" GINAKEFF");
    if (val & (1 << 5))
        printf(" NPTXFE");
    if (val & (1 << 4))
        printf(" RXFLVL");
    if (val & (1 << 3))
        printf(" SOF");
    if (val & (1 << 2))
        printf(" OTGINT");
    if (val & (1 << 1))
        printf(" MMIS");
    if (val & (1 << 0))
        printf(" CMOD");
    printf("\n");
#endif
}

void
usb_show_stats(void)
{
    printf("interrupt=%s\n", using_usb_interrupt ? "true" : "false");
    printf("console_active=%s\n", usb_console_active ? "true" : "false");
    printf("packet drops=%u\n", usb_drop_packets);
    printf("byte drops=%u\n", usb_drop_bytes);
    printf("send timeouts=%u\n", usb_send_timeouts);
}

uint16_t
usb_current_address(void)
{
    return ((OTG_FS_DCFG & OTG_DCFG_DAD) >> 4);
}

void
usb_poll_mode(void)
{
    nvic_disable_irq(USB_INTERRUPT);
    using_usb_interrupt = false;
    usb_poll();
}
