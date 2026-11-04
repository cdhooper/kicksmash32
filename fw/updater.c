/*
 * Stand-alone DFU programmer for STM32F1xx parts.
 *
 * The original version of this file was taken from an example in the
 * libopencm3 project by Gareth McMullin <gareth@blacksphere.co.nz>.
 * It has been heavily modified for KickSmash, but still retains the
 * GNU Lesser General Public License.
 */

typedef unsigned int uint;

#include <string.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/flash.h>
#include <libopencm3/cm3/scb.h>
#include <libopencm3/usb/usbd.h>
#include <libopencm3/usb/dfu.h>
#include <libopencm3/usb/bos.h>
#include <libopencm3/usb/microsoft.h>
#include <libopencm3/stm32/usart.h>
#include "clock.h"
#include "timer.h"
#include "gd32f107_usb.h"
#include "persist.h"
#include "printf.h"
#include "uart.h"
#include "gpio.h"
#include "msg.h"

#define CONSOLE_USART USART1

#define BIT(x)      (1U << (x))
#define ADDR8(x)    ((uint8_t *)  ((uintptr_t)(x)))
#define STM32_UDID_LEN                  12    // 96 bits
#define STM32_UDID_BASE                 DESIG_UNIQUE_ID_BASE

uint8_t usb_serial_str[32];

#define UART_DEBUG
#ifdef UART_DEBUG
int
sprintf(char *buf, const char *fmt, ...)
{
    strcpy(buf, fmt);
    return (0);
}

__attribute__((format(__printf__, 1, 2)))
int
printf(const char *fmt, ...)
{
    uart_puts(fmt);
    return (0);
}

static void
uart_wait_done(uint32_t usart)
{
    /* Wait until the data has been transferred into the shift register. */
    int count = 0;

    while ((USART_SR(usart) & USART_SR_TC) == 0)
        if (count++ == 2000)
            break;  // Misconfigured hardware?
}

static void
uart_wait_send_ready(uint32_t usart)
{
    /* Wait until the data has been transferred into the shift register. */
    int count = 0;

    while ((USART_SR(usart) & USART_SR_TXE) == 0)
        if (count++ == 1000)
            break;  // Misconfigured hardware?
}

static void
uart_send(uint32_t usart, uint16_t data)
{
    USART_DR(usart) = (data & USART_DR_MASK);
}

static void
uart_send_blocking(uint32_t usart, uint16_t data)
{
    uart_wait_send_ready(usart);
    uart_send(usart, data);
}

static void
uart_putc(int ch)
{
    if (ch == '\n')
        uart_send_blocking(CONSOLE_USART, '\r');
    uart_send_blocking(CONSOLE_USART, (uint16_t) ch);
}

void
uart_putchar(int ch)
{
    uart_putc(ch);
}

void
uart_puts(const char *str)
{
    while (*str != '\0')
        uart_putc(*(str++));
}

static int
uart_getchar(void)
{
    if (USART_SR(CONSOLE_USART) & (USART_SR_RXNE | USART_SR_ORE)) {
        return (USART_DR(CONSOLE_USART) & USART_DR_MASK);
    }
    return (-1);
}

void
uart_puthex(uint32_t x)
{
    uint32_t digit;
    char buf[32];
    char *ptr = buf + sizeof (buf) - 1;
    *ptr = '\0';
    for (digit = 0; digit < 8; digit++) {
        *(--ptr) = "0123456789abcdef"[x & 0xf];
        x >>= 4;
    }
    uart_puts(ptr);
}

void
uart_init(void)
{
    gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_50_MHZ,
                  GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO9); // CONS_TX
    gpio_set_mode(GPIOA, GPIO_MODE_INPUT,
                  GPIO_CNF_INPUT_FLOAT, GPIO10);          // CONS_RX

    rcc_periph_clock_enable(RCC_AFIO);
    rcc_periph_clock_enable(RCC_USART1);
    rcc_periph_clock_enable(RCC_GPIOB);

    /* Use PB6 for CONS_TX and PB7 for CONS_RX */
    AFIO_MAPR |= AFIO_MAPR_USART1_REMAP;

    gpio_set_mode(GPIOB, GPIO_MODE_OUTPUT_50_MHZ,
                  GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO6); // CONS_TX
    /* CONS_RX */
    gpio_set_mode(GPIOB, GPIO_MODE_INPUT, GPIO_CNF_INPUT_PULL_UPDOWN, GPIO7);

    /* Setup UART parameters. */
    usart_set_baudrate(CONSOLE_USART, 115200);
    usart_set_databits(CONSOLE_USART, 8);
    usart_set_stopbits(CONSOLE_USART, USART_STOPBITS_1);
    usart_set_mode(CONSOLE_USART, USART_MODE_TX_RX);
    usart_set_parity(CONSOLE_USART, USART_PARITY_NONE);
    usart_set_flow_control(CONSOLE_USART, USART_FLOWCONTROL_NONE);
    usart_enable(CONSOLE_USART);
}
#endif /* UART_DEBUG */

static void
set_flashled(int state)
{
    if (state) {
        gpio_set(GPIOB, GPIO9);
    } else {
        gpio_clear(GPIOB, GPIO9);
    }
}

static void
set_powerled(int state)
{
    if (state) {
        gpio_clear(GPIOB, GPIO8);
    } else {
        gpio_set(GPIOB, GPIO8);
    }
}

/* Commands sent with wBlockNum == 0 as per ST implementation. */
#define CMD_SETADDR 0x21
#define CMD_ERASE   0x41

/* We need a special large control buffer for this device: */
static uint8_t usbd_control_buffer[2048];

static enum dfu_state usbdfu_state = STATE_DFU_IDLE;

static struct {
    uint8_t buf[sizeof (usbd_control_buffer)];
    uint16_t len;
    uint32_t addr;
    uint16_t blocknum;
} prog;

const struct usb_device_descriptor dev = {
    .bLength = USB_DT_DEVICE_SIZE,
    .bDescriptorType = USB_DT_DEVICE,
    .bcdUSB = 0x0201,   // 2.01 = host will read the BOS descriptor
    .bDeviceClass = 0,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = 64,
    .idVendor = 0x0483,
    .idProduct = 0xDF11,
    .bcdDevice = 0x0201,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

const struct usb_dfu_descriptor dfu_function = {
    .bLength = sizeof (struct usb_dfu_descriptor),
    .bDescriptorType = DFU_FUNCTIONAL,
    .bmAttributes = USB_DFU_CAN_DOWNLOAD | USB_DFU_WILL_DETACH,
    .wDetachTimeout = 255,
    .wTransferSize = 2048,
    .bcdDFUVersion = 0x011A,
};

const struct usb_interface_descriptor iface = {
    .bLength = USB_DT_INTERFACE_SIZE,
    .bDescriptorType = USB_DT_INTERFACE,
    .bInterfaceNumber = 0,
    .bAlternateSetting = 0,
    .bNumEndpoints = 0,
    .bInterfaceClass = 0xFE, /* Device Firmware Upgrade */
    .bInterfaceSubClass = 1,
    .bInterfaceProtocol = 2,

    /*
     * The ST Microelectronics DfuSe application needs this string.
     * The format isn't documented...
     */
    .iInterface = 4,

    .extra = &dfu_function,
    .extralen = sizeof (dfu_function),
};

const struct usb_interface ifaces[] = {
    {
        .num_altsetting = 1,
        .altsetting = &iface,
    }
};

const struct usb_config_descriptor config = {
    .bLength = USB_DT_CONFIGURATION_SIZE,
    .bDescriptorType = USB_DT_CONFIGURATION,
    .wTotalLength = 0,
    .bNumInterfaces = 1,
    .bConfigurationValue = 1,
    .iConfiguration = 0,
    .bmAttributes = 0xC0,
    .bMaxPower = 0x32,

    .interface = ifaces,
};

/*
 * Microsoft OS 2.0 descriptors
 * ----------------------------
 * These cause Windows 8.1 and newer to automatically bind the WinUSB
 * driver to the updater, so that libusb (hostsmash --fwwrite) can
 * access it without the user first installing a driver with Zadig.
 * Windows only reads the BOS descriptor when bcdUSB is at least 2.01.
 * It then fetches the descriptor set below using a vendor request with
 * bRequest = MS_OS_VENDOR_CODE and wIndex = 7 (MS_OS_20_DESCRIPTOR_INDEX).
 *
 * The descriptor set has no configuration or function subsets because
 * the updater is not a composite device, so the features apply to the
 * whole device.
 */
#define MS_OS_VENDOR_CODE      0x01
#define MS_OS_WINDOWS_VERSION  0x06030000  // Windows 8.1
#define MS_OS_SET_LEN          162         // sizeof (ms_os_20_desc_set)
#define LE16(x) ((x) & 0xff), (((x) >> 8) & 0xff)
#define LE32(x) LE16((x) & 0xffff), LE16((x) >> 16)
#define U16(c)  (c), 0                     // UTF-16LE ASCII character

static const microsoft_os_descriptor_set_information ms_os_set_info = {
    .dwWindowsVersion = MS_OS_WINDOWS_VERSION,
    .wMSOSDescriptorSetTotalLength = MS_OS_SET_LEN,
    .bMS_VendorCode = MS_OS_VENDOR_CODE,
    .bAltEnumCode = 0,
};

static const usb_platform_device_capability_descriptor ms_os_platform = {
    .device_capability_descriptor = {
        .bLength = USB_DCT_PLATFORM_SIZE +
                   MICROSOFT_OS_DESCRIPTOR_SET_INFORMATION_SIZE,
        .bDescriptorType = USB_DT_DEVICE_CAPABILITY,
        .bDevCapabilityType = USB_DCT_PLATFORM,
    },
    .bReserved = 0,
    .PlatformCapabilityUUID = MICROSOFT_OS_DESCRIPTOR_PLATFORM_CAPABILITY_ID,
    .CapabilityData = &ms_os_set_info,
};

static const usb_bos_descriptor bos = {
    .bLength = USB_DT_BOS_SIZE,
    .bDescriptorType = USB_DT_BOS,
    .wTotalLength = 0,  // Computed by libopencm3
    .bNumDeviceCaps = 1,
    .device_capability_descriptors = &ms_os_platform,
};

static const uint8_t ms_os_20_desc_set[] = {
    /* Descriptor set header */
    LE16(10),                       // wLength
    LE16(MICROSOFT_OS_SET_HEADER),  // wDescriptorType
    LE32(MS_OS_WINDOWS_VERSION),    // dwWindowsVersion
    LE16(MS_OS_SET_LEN),            // wTotalLength

    /* Compatible ID: bind the WinUSB driver */
    LE16(20),                                   // wLength
    LE16(MICROSOFT_OS_FEATURE_COMPATIBLE_ID),   // wDescriptorType
    'W', 'I', 'N', 'U', 'S', 'B', 0, 0,         // CompatibleID
    0, 0, 0, 0, 0, 0, 0, 0,                     // SubCompatibleID

    /* Registry property: DeviceInterfaceGUIDs (REG_MULTI_SZ) */
    LE16(10 + 42 + 80),                         // wLength
    LE16(MICROSOFT_OS_FEATURE_REG_PROPERTY),    // wDescriptorType
    LE16(REG_MULTI_SZ),                         // wPropertyDataType
    LE16(42),                                   // wPropertyNameLength
    U16('D'), U16('e'), U16('v'), U16('i'), U16('c'), U16('e'),
    U16('I'), U16('n'), U16('t'), U16('e'), U16('r'), U16('f'),
    U16('a'), U16('c'), U16('e'), U16('G'), U16('U'), U16('I'),
    U16('D'), U16('s'), U16(0),
    LE16(80),                                   // wPropertyDataLength
    /* {B53EAB80-04C3-4486-AAFB-7393460808A0} = Kicksmash updater */
    U16('{'), U16('B'), U16('5'), U16('3'), U16('E'), U16('A'),
    U16('B'), U16('8'), U16('0'), U16('-'), U16('0'), U16('4'),
    U16('C'), U16('3'), U16('-'), U16('4'), U16('4'), U16('8'),
    U16('6'), U16('-'), U16('A'), U16('A'), U16('F'), U16('B'),
    U16('-'), U16('7'), U16('3'), U16('9'), U16('3'), U16('4'),
    U16('6'), U16('0'), U16('8'), U16('0'), U16('8'), U16('A'),
    U16('0'), U16('}'), U16(0), U16(0),
};
/* Compile error here means MS_OS_SET_LEN does not match ms_os_20_desc_set */
typedef char ms_os_set_len_check[(sizeof (ms_os_20_desc_set) ==
                                  MS_OS_SET_LEN) ? 1 : -1];

static const char *usb_strings[] = {
    "eebugs",           // Manufacturer
    "KickSmash",        // Product
    "",                 // Serial filled at runtime
    /* Below required by dfu-util and ST-Micro DfuSe utility (for STM32F107) */
    "@Internal Flash  /0x08000000/128*002Kg",  // 128 * 2 KB = 256 KB
};

/*
 * Real STM32F107 in DFU mode
 * -----------------------------------------------
 * idVendor           0x0483 STMicroelectronics
 * idProduct          0xdf11 STM Device in DFU Mode
 * bcdDevice           22.00
 * iManufacturer           1 STMicroelectronics
 * iProduct                2 STM32 0x418 DFU Bootloade
 * iSerial                 3 STM32
 *    Interface Descriptor:
 *    bLength                 9
 *    bDescriptorType         4
 *    bInterfaceNumber        0
 *    bAlternateSetting       0
 *    bNumEndpoints           0
 *    bInterfaceClass       254 Application Specific Interface
 *    bInterfaceSubClass      1 Device Firmware Update
 *    bInterfaceProtocol      2
 *    iInterface              4 @Internal Flash  /0x08000000/128*002Kg
 *  Interface Descriptor:
 *    bLength                 9
 *    bDescriptorType         4
 *    bInterfaceNumber        0
 *    bAlternateSetting       1
 *    bNumEndpoints           0
 *    bInterfaceClass       254 Application Specific Interface
 *    bInterfaceSubClass      1 Device Firmware Update
 *    bInterfaceProtocol      2
 *    iInterface              5 @Option Bytes  /0x1FFFF800/01*016 e
 *  Device Firmware Upgrade Interface Descriptor:
 *    bLength                             9
 *    bDescriptorType                    33
 *    bmAttributes                       11
 *      Will Detach
 *      Manifestation Intolerant
 *      Upload Supported
 *      Download Supported
 *    wDetachTimeout                    255 milliseconds
 *    wTransferSize                    2048 bytes
 *    bcdDFUVersion                   1.1a
 */

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
    unsigned int len = 0;
    unsigned int pos;

    for (pos = 0; pos < STM32_UDID_LEN; pos++) {
        uint8_t temp  = *ADDR8(STM32_UDID_BASE + pos);
        uint8_t ch_hi = (temp >> 4) + '0';
        uint8_t ch_lo = (temp & 0xf) + '0';

        if (temp == 0xff)
            continue;
        if (((temp >= '0') && (temp <= '9')) ||
            ((temp >= 'A') && (temp <= 'Z')) ||
            ((temp >= 'a') && (temp <= 'z'))) {
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

static uint8_t
usbdfu_getstatus(uint32_t *bwPollTimeout)
{
    switch (usbdfu_state) {
        case STATE_DFU_DNLOAD_SYNC:
            /*
             * The operation is performed in usbdfu_getstatus_complete()
             * and the next request isn't answered until it is done, so
             * the host may poll early without harm. Request roughly the
             * typical time for the operation, so the host doesn't sit
             * idle after it has finished.
             */
            usbdfu_state = STATE_DFU_DNBUSY;
            if (prog.blocknum != 0) {
                /* Program half-words: ~53 usec each (STM32F1 typical) */
                *bwPollTimeout = (prog.len / 2) * 53 / 1000 + 1;
            } else if (prog.buf[0] == CMD_ERASE) {
                *bwPollTimeout = 25;  // Page erase (STM32F1 20-40 ms)
            } else {
                *bwPollTimeout = 0;   // Set address: nothing to wait for
            }
            return (DFU_STATUS_OK);
        case STATE_DFU_MANIFEST_SYNC:
            /* Device will reset when read is complete. */
            usbdfu_state = STATE_DFU_MANIFEST;
            return (DFU_STATUS_OK);
        default:
            return (DFU_STATUS_OK);
    }
}

static void
usbdfu_getstatus_complete(usbd_device *usbd_dev, struct usb_setup_data *req)
{
    static uint8_t lstate;
    int i;
    (void) req;
    (void) usbd_dev;

    switch (usbdfu_state) {
        case STATE_DFU_DNBUSY:
            flash_unlock();
            if (prog.blocknum == 0) {
                switch (prog.buf[0]) {
                    case CMD_ERASE: {
                        uint32_t *dat = (uint32_t *)(prog.buf + 1);
                        set_powerled(0);
                        set_flashled(1);
                        if (lstate != 1)
                            uart_puts("\nErase   ");
                        else
                            uart_putc('.');
                        lstate = 1;
                        flash_erase_page(*dat);
                        set_flashled(0);
                        break;
                    }
                    case CMD_SETADDR: {
                        uint32_t *dat = (uint32_t *)(prog.buf + 1);
                        prog.addr = *dat;
                        break;
                    }
                }
            } else {
                uint32_t baseaddr = prog.addr + ((prog.blocknum - 2) *
                                                dfu_function.wTransferSize);
                set_powerled(0);
                set_flashled(1);
                if (lstate != 2)
                    uart_puts("\nProgram ");
                else
                    uart_putc('.');
                lstate = 2;
                for (i = 0; i < prog.len; i += 2) {
                    uint16_t *dat = (uint16_t *)(prog.buf + i);
                    flash_program_half_word(baseaddr + i, *dat);
                }
                set_flashled(0);
            }
            flash_lock();

            /* Jump straight to dfuDNLOAD-IDLE, skipping dfuDNLOAD-SYNC. */
            usbdfu_state = STATE_DFU_DNLOAD_IDLE;
            break;
        case STATE_DFU_MANIFEST:
            /* USB device must detach, we just reset... */
            uart_puts("\nReset\n");
            msg_shutdown();
            uart_wait_done(CONSOLE_USART);
            scb_reset_system();
            break; /* Will never return. */
        case STATE_DFU_IDLE:
            break;
        case STATE_DFU_DNLOAD_IDLE:
            break;
        case STATE_DFU_ERROR:
            uart_puts("\nDFU ERROR\n");
            break;
        default:
            uart_puts("Unknown ");
            uart_puthex(usbdfu_state);
            uart_puts("\n");
            break;
    }
}

static enum usbd_request_return_codes
usbdfu_control_request(usbd_device *usbd_dev, struct usb_setup_data *req,
                       uint8_t **buf, uint16_t *len,
                       void (**complete)(usbd_device *usbd_dev,
                                         struct usb_setup_data *req))
{
    (void) usbd_dev;

    if ((req->bmRequestType & 0x7F) != 0x21)
        return (USBD_REQ_NOTSUPP); /* Only accept class request. */

    switch (req->bRequest) {
        case DFU_DNLOAD:
            if ((len == NULL) || (*len == 0)) {
                usbdfu_state = STATE_DFU_MANIFEST_SYNC;
            } else {
                /* Copy download data for use on GET_STATUS. */
                prog.blocknum = req->wValue;
                prog.len = *len;
                memcpy(prog.buf, *buf, *len);
                usbdfu_state = STATE_DFU_DNLOAD_SYNC;
            }
            return (USBD_REQ_HANDLED);
        case DFU_CLRSTATUS:
            uart_puts("CLRSTATUS\n");
            /* Clear error and return to dfuIDLE. */
            if (usbdfu_state == STATE_DFU_ERROR)
                usbdfu_state = STATE_DFU_IDLE;
            return (USBD_REQ_HANDLED);
        case DFU_ABORT:
            /* Abort returns to dfuIDLE state. */
            if (usbdfu_state != STATE_DFU_IDLE) {
                usbdfu_state = STATE_DFU_IDLE;
                uart_puts("\nDone");
            }
            return (USBD_REQ_HANDLED);
        case DFU_UPLOAD:
            /* Upload not supported for now. */
            return (USBD_REQ_NOTSUPP);
        case DFU_GETSTATUS: {
            uint32_t bwPollTimeout = 0; /* 24-bit integer in DFU class spec */
            (*buf)[0] = usbdfu_getstatus(&bwPollTimeout);
            (*buf)[1] = bwPollTimeout & 0xFF;
            (*buf)[2] = (bwPollTimeout >> 8) & 0xFF;
            (*buf)[3] = (bwPollTimeout >> 16) & 0xFF;
            (*buf)[4] = usbdfu_state;
            (*buf)[5] = 0; /* iString not used here */
            *len = 6;
            *complete = usbdfu_getstatus_complete;
            return (USBD_REQ_HANDLED);
        }
        case DFU_GETSTATE:
            /* Return state with no state transision. */
            *buf[0] = usbdfu_state;
            *len = 1;
            return (USBD_REQ_HANDLED);
    }

    return (USBD_REQ_NOTSUPP);
}

/*
 * ms_os_control_request() returns the Microsoft OS 2.0 descriptor set
 *                         when Windows requests it.
 */
static enum usbd_request_return_codes
ms_os_control_request(usbd_device *usbd_dev, struct usb_setup_data *req,
                      uint8_t **buf, uint16_t *len,
                      usbd_control_complete_callback *complete)
{
    (void) usbd_dev;
    (void) complete;

    if ((req->bRequest != MS_OS_VENDOR_CODE) ||
        (req->wIndex != MICROSOFT_GET_DESCRIPTOR_SET) ||
        ((req->bmRequestType & USB_REQ_TYPE_DIRECTION) == 0)) {
        return (USBD_REQ_NEXT_CALLBACK);
    }
    *buf = (uint8_t *) ms_os_20_desc_set;
    if (*len > sizeof (ms_os_20_desc_set))
        *len = sizeof (ms_os_20_desc_set);
    return (USBD_REQ_HANDLED);
}

static void
ms_os_register(usbd_device *usbd_dev)
{
    usbd_register_control_callback(
                usbd_dev,
                USB_REQ_TYPE_VENDOR | USB_REQ_TYPE_DEVICE,
                USB_REQ_TYPE_TYPE | USB_REQ_TYPE_RECIPIENT,
                ms_os_control_request);
}

static void
usbdfu_set_config(usbd_device *usbd_dev, uint16_t wValue)
{
    (void) wValue;

    /* SET_CONFIGURATION clears all control callbacks */
    ms_os_register(usbd_dev);

    usbd_register_control_callback(
                usbd_dev,
                USB_REQ_TYPE_CLASS | USB_REQ_TYPE_INTERFACE,
                USB_REQ_TYPE_TYPE | USB_REQ_TYPE_RECIPIENT,
                usbdfu_control_request);
}

/*
 * ee_address_override
 * -------------------
 * Override the Flash A17, A18 and A19 address
 *
 * override = 0  Record new override
 * override = 1  Temporarily disable override
 * override = 2  Restore previous override
 *
 * bits
 *   0    1=Drive A17
 *   1    1=Drive A18
 *   2    1=Drive A19
 *   3    Unused
 *   4    A17 driven value
 *   5    A18 driven value
 *   6    A19 driven value
 *   7    Unused
 */
static void
address_override(uint8_t bits)
{
    uint           bit;
    static const uint32_t ports[] =
                          { FLASH_A17_PORT, FLASH_A18_PORT, FLASH_A19_PORT };
    static const uint32_t pins[]  =
                          { FLASH_A17_PIN,  FLASH_A18_PIN,  FLASH_A19_PIN };

    for (bit = 0; bit < 3; bit++) {
        uint32_t port  = ports[bit];
        uint16_t pin   = pins[bit];
        if (bits & BIT(bit)) {
            /* Drive address pin */
            uint     shift = (bits & BIT(bit + 4)) ? 0 : 16;
            gpio_set_mode(port, GPIO_MODE_OUTPUT_50_MHZ,
                          GPIO_CNF_OUTPUT_PUSHPULL, pin);
            GPIO_BSRR(port) = pin << shift;
        } else {
            /* Disable drive */
            gpio_set_mode(port, GPIO_MODE_INPUT, GPIO_CNF_INPUT_FLOAT, pin);
        }
    }
}

static void
set_bank(uint8_t bank)
{
    /*
     * sp_bank_merge tells what bits of A17, A18, and A19 to force.
     * If the width of the bank is 8, then none are forced.
     * If the width of the bank is 4, then A19 is forced.
     * If the width of the bank is 2, then A19 and A18 are forced.
     * If the width of the bank is 1, then A19, A18, and A17 are forced.
     */
    switch (persist.sp_bank_merge >> 4) {
        case 0:  // 512KB bank (width 1)
            address_override((bank << 4) | 0x7);
            break;
        case 1:  // 1MB bank (width 2)
            address_override((bank << 4) | 0x6);
            break;
        case 2:  // 2MB bank (width 4)
            address_override((bank << 4) | 0x4);
            break;
        case 3:  // 4MB bank (width 8)
            address_override((bank << 4) | 0x0);
            break;
    }
}

void
main(void)
{
    uint run_count = 0;
    usbd_device *usbd_dev;

    rcc_periph_clock_enable(RCC_GPIOA);
    rcc_periph_clock_enable(RCC_GPIOB);
    rcc_periph_clock_enable(RCC_GPIOC);
    rcc_periph_clock_enable(RCC_GPIOD);
    rcc_periph_clock_enable(RCC_GPIOE);

    persist_init();
    set_bank(persist.sp_bank_current);
    clock_init();
    timer_init();  // Needed for KS_CMD_UPTIME, msg timeouts, and delays

    rcc_periph_clock_enable(RCC_AFIO);

    uart_init();
    uart_puts("DFU waiting.");

    /* POWER_LED, FLASH_OEWE, FLASH_OE, FLASH_WE */
    gpio_set_mode(GPIOB, GPIO_MODE_OUTPUT_50_MHZ,
                  GPIO_CNF_OUTPUT_PUSHPULL, GPIO8 | GPIO9 | GPIO14);
    gpio_set(GPIOB, GPIO14);  // FLASH_WE
#if 0
    gpio_set_mode(GPIOB, GPIO_MODE_OUTPUT_50_MHZ,
                  GPIO_CNF_OUTPUT_PUSHPULL, GPIO13);
    gpio_set(GPIOB, GPIO13);  // FLASH_OE (flash read LED will be dim)
#endif
    set_flashled(0);          // Flash write LED off
    set_powerled(1);          // Power LED on
    msg_init();

    rcc_periph_clock_enable(RCC_OTGFS);

    usbd_usr_serial(usb_serial_str);
    usb_strings[2] = (char *)usb_serial_str;

    const usbd_driver *driver;
    if (SCB_CPUID == 0x412fc231) {
        /* GD32F1xx */
        gd32f107_usb_init();
        driver = &gd32f107_usb_driver;
    } else {
        /* STM32F1xx */
        driver = &stm32f107_usb_driver;
    }
    usbd_dev = usbd_init(driver, &dev, &config, usb_strings,
                         4, usbd_control_buffer, sizeof (usbd_control_buffer));
    usbd_register_set_config_callback(usbd_dev, usbdfu_set_config);
    usbd_register_bos_descriptor(usbd_dev, &bos);
    ms_os_register(usbd_dev);  // Windows requests this before SET_CONFIG

    while (1) {
        set_powerled((run_count & 0x000000ff) ? 0 : 1);  // flicker
        usbd_poll(usbd_dev);
        msg_poll();
        if ((run_count & 0x000000ff) == 0) {
            if (uart_getchar() == 0x03) {  // ^C
                uart_puts("^C Abort.\n");
                goto finish;
            }
        }
        run_count++;
    }
finish:
    msg_shutdown();
    uart_wait_done(CONSOLE_USART);
    scb_reset_system();
}
