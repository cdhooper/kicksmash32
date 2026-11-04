/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2026.
 *
 * ---------------------------------------------------------------------
 *
 * Hostsmash USB DFU (DfuSe) interface to the Kicksmash firmware updater
 * and the STM32 and GD32 ROM DFU bootloaders.
 *
 * The updater and the STM32 ROM bootloader enumerate as 0483:df11 DfuSe
 * devices. The Kicksmash updater
 * (fw/updater.c) reports manufacturer "eebugs", product "KickSmash", and
 * the same USB serial number as the Kicksmash firmware. The ROM bootloader
 * reports manufacturer "STMicroelectronics" and serial number "STM32", so
 * it is identified by its USB port path instead.
 *
 * The GigaDevice GD32 ROM bootloader enumerates as 28e9:0189. It doesn't
 * provide usable memory layout strings. Instead, its serial number string
 * descriptor holds a model code as 8-bit characters (for example "7VCB"
 * for GD32F107VC), from which the flash size and page size are derived.
 *
 * The flash alternate setting ("@Internal Flash ...") is selected, and its
 * DfuSe memory layout string is parsed for the flash base address, size,
 * and page sizes. The transfer size comes from the DFU functional
 * descriptor. Page erase, set address, data download, and the leave
 * request (set address followed by a zero-length download) are supported.
 *
 * libusb-1.0 is required. Without it (HAVE_LIBUSB not defined), the
 * functions here report that DFU is not supported.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "hostsmash.h"
#include "hostsmash_dfu.h"

/*
 * dfu_id_match() returns true if the DFU device matches the specified
 *                identifier, which may be either its USB serial number
 *                or its USB port path. A NULL or empty id matches any
 *                device.
 */
bool
dfu_id_match(const dfu_info_t *info, const char *id)
{
    if ((id == NULL) || (id[0] == '\0'))
        return (true);
    return ((strcmp(info->serial, id) == 0) ||
            (strcmp(info->usbpath, id) == 0));
}

#ifdef HAVE_LIBUSB
#include <libusb.h>
#include "hostsmash_windrv.h"

#define DFU_VID           0x0483  // DFU USB vendor ID (ST)
#define DFU_PID           0xdf11  // DFU USB product ID (ST DFU mode)
#define GD32_VID          0x28e9  // GD32 ROM DFU USB vendor ID
#define GD32_PID          0x0189  // GD32 ROM DFU USB product ID
#define GD32_FLASH_BASE   0x08000000  // GD32 internal flash address
#define DFU_TIMEOUT_MS    5000    // USB control transfer timeout
#define DFU_MAX_POLLS     50      // GETSTATUS polls per operation
#define DFU_MAX_POLL_MS   1000    // Cap on device-requested poll delay
#define DFU_STALL_RETRIES 4       // Attempts for a stalled DFU_DNLOAD
#define DFU_DEF_XFER_SIZE 2048    // Transfer size if not in descriptor
#define DFU_MAX_SEGS      8       // Maximum memory layout segments
#define DFU_FLASH_ALT     "@Internal Flash"  // Flash alternate setting

#define USB_CLASS_APP_SPEC    0xfe  // Application specific interface class
#define USB_SUBCLASS_DFU      0x01  // Device firmware upgrade subclass
#define USB_DT_DFU_FUNCTIONAL 0x21  // DFU functional descriptor type

/* DFU class requests */
#define DFU_REQ_DNLOAD    1
#define DFU_REQ_GETSTATUS 3
#define DFU_REQ_CLRSTATUS 4
#define DFU_REQ_ABORT     6

/* DFU states */
#define DFU_STATE_IDLE        2
#define DFU_STATE_DNLOAD_SYNC 3
#define DFU_STATE_DNBUSY      4
#define DFU_STATE_DNLOAD_IDLE 5
#define DFU_STATE_MANIFEST_WAIT_RESET 8
#define DFU_STATE_ERROR       10

/* DfuSe commands sent as block 0 */
#define DFUSE_CMD_SETADDR 0x21
#define DFUSE_CMD_ERASE   0x41

#define DFU_REQTYPE_OUT (LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_CLASS | \
                         LIBUSB_RECIPIENT_INTERFACE)
#define DFU_REQTYPE_IN  (LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_CLASS | \
                         LIBUSB_RECIPIENT_INTERFACE)

/* One memory layout segment: count pages of size bytes */
typedef struct {
    unsigned int count;
    unsigned int size;
} dfu_seg_t;

struct dfu_dev {
    libusb_device_handle *handle;
    dfu_info_t            info;
    unsigned int          iface;       // DFU interface number
    unsigned int          xfer_size;   // wTransferSize
    uint32_t              flash_base;  // Flash region start address
    uint32_t              flash_size;  // Flash region size
    unsigned int          nsegs;       // Number of layout segments
    dfu_seg_t             segs[DFU_MAX_SEGS];
};

static libusb_context *dfu_ctx = NULL;

bool
dfu_supported(void)
{
    return (true);
}

/*
 * dfu_init() initializes libusb, if not already done.
 */
static int
dfu_init(void)
{
    if ((dfu_ctx == NULL) && (libusb_init(&dfu_ctx) != 0)) {
        dfu_ctx = NULL;
        return (1);
    }
    return (0);
}

/*
 * dfu_usbpath() formats the USB port path of a device in the same form
 *               as Linux sysfs: <bus>-<port>[.<port>...]
 */
static void
dfu_usbpath(libusb_device *udev, char *buf, size_t buflen)
{
    uint8_t ports[8];
    int     nports;
    int     pos;
    size_t  len;

    len = snprintf(buf, buflen, "%u", libusb_get_bus_number(udev));
    nports = libusb_get_port_numbers(udev, ports, sizeof (ports));
    for (pos = 0; (pos < nports) && (len < buflen); pos++) {
        len += snprintf(buf + len, buflen - len, "%c%u",
                        (pos == 0) ? '-' : '.', ports[pos]);
    }
}

/*
 * dfu_get_string() reads a USB string descriptor as ASCII.
 */
static void
dfu_get_string(libusb_device_handle *handle, uint8_t index,
               char *buf, size_t buflen)
{
    int rc = 0;

    if (index != 0)
        rc = libusb_get_string_descriptor_ascii(handle, index,
                                                (unsigned char *) buf,
                                                buflen);
    if (rc <= 0)
        rc = 0;
    if ((size_t) rc >= buflen)
        rc = buflen - 1;
    buf[rc] = '\0';
}

/*
 * dfu_is_dfu_device() returns true if the USB device is one of the
 *                     supported DFU devices.
 */
static bool
dfu_is_dfu_device(const struct libusb_device_descriptor *desc)
{
    return (((desc->idVendor == DFU_VID) && (desc->idProduct == DFU_PID)) ||
            ((desc->idVendor == GD32_VID) && (desc->idProduct == GD32_PID)));
}

/*
 * dfu_get_gd32_model() reads the GD32 ROM bootloader model code from the
 *                      serial number string descriptor, which holds 8-bit
 *                      characters rather than UTF-16LE.
 */
static void
dfu_get_gd32_model(libusb_device_handle *handle, uint8_t index,
                   char *buf, size_t buflen)
{
    unsigned char raw[64];
    int           rc = 0;
    int           pos;
    size_t        len = 0;

    if (index != 0)
        rc = libusb_get_string_descriptor(handle, index, 0x0409,
                                          raw, sizeof (raw));
    if (rc > raw[0])
        rc = raw[0];
    for (pos = 2; (pos < rc) && (len + 1 < buflen); pos++) {
        if ((raw[pos] < ' ') || (raw[pos] > '~'))
            break;
        buf[len++] = raw[pos];
    }
    buf[len] = '\0';
}

/*
 * dfu_get_info() fills in identifying information about an open DFU device.
 */
static void
dfu_get_info(libusb_device *udev, libusb_device_handle *handle,
             const struct libusb_device_descriptor *desc, dfu_info_t *info)
{
    char manuf[64];

    memset(info, 0, sizeof (*info));
    dfu_get_string(handle, desc->iProduct,
                   info->product, sizeof (info->product));
    dfu_get_string(handle, desc->iManufacturer, manuf, sizeof (manuf));
    dfu_usbpath(udev, info->usbpath, sizeof (info->usbpath));
    if (desc->idVendor == GD32_VID) {
        info->is_gd32 = true;
        dfu_get_gd32_model(handle, desc->iSerialNumber,
                           info->serial, sizeof (info->serial));
        if (strlen(info->serial) >= 3) {
            snprintf(info->kind, sizeof (info->kind),
                     "GD32F10%c%c%c ROM DFU bootloader",
                     info->serial[0], info->serial[1], info->serial[2]);
        } else {
            snprintf(info->kind, sizeof (info->kind),
                     "GD32 ROM DFU bootloader");
        }
        return;
    }
    dfu_get_string(handle, desc->iSerialNumber,
                   info->serial, sizeof (info->serial));
    info->is_updater = (strcmp(manuf, "eebugs") == 0) &&
                       (strcmp(info->product, "KickSmash") == 0);
    snprintf(info->kind, sizeof (info->kind), "%s", info->is_updater ?
             "Kicksmash updater" : "STM32 ROM DFU bootloader");
}

/*
 * dfu_list() returns information about each attached DFU device.
 *
 * @return Number of devices found.
 */
unsigned int
dfu_list(dfu_info_t *list, unsigned int max)
{
    libusb_device **devs;
    ssize_t         count;
    ssize_t         cur;
    unsigned int    found = 0;

    if (dfu_init() != 0)
        return (0);
    count = libusb_get_device_list(dfu_ctx, &devs);
    if (count < 0)
        return (0);
    for (cur = 0; (cur < count) && (found < max); cur++) {
        struct libusb_device_descriptor desc;
        libusb_device_handle *handle;

        if ((libusb_get_device_descriptor(devs[cur], &desc) != 0) ||
            !dfu_is_dfu_device(&desc) ||
            (libusb_open(devs[cur], &handle) != 0)) {
            continue;
        }
        dfu_get_info(devs[cur], handle, &desc, &list[found++]);
        libusb_close(handle);
    }
    libusb_free_device_list(devs, 1);
    return (found);
}

/*
 * dfu_parse_layout() parses the first region of a DfuSe memory layout
 *                    string, such as "@Internal Flash  /0x08000000/128*002Kg"
 *
 * @return 0 - Success.
 * @return 1 - Layout could not be parsed.
 */
static int
dfu_parse_layout(dfu_dev_t *dev, const char *layout)
{
    const char *ptr = strchr(layout, '/');
    char       *eptr;

    if (ptr == NULL)
        return (1);
    dev->flash_base = strtoul(ptr + 1, &eptr, 0);
    if ((eptr == ptr + 1) || (*eptr != '/'))
        return (1);
    ptr = eptr + 1;
    dev->nsegs = 0;
    dev->flash_size = 0;
    while (dev->nsegs < DFU_MAX_SEGS) {
        unsigned int count;
        unsigned int size;

        count = strtoul(ptr, &eptr, 10);
        if ((eptr == ptr) || (*eptr != '*'))
            return (1);
        ptr = eptr + 1;
        size = strtoul(ptr, &eptr, 10);
        if (eptr == ptr)
            return (1);
        ptr = eptr;
        if (*ptr == 'K')
            size <<= 10;
        else if (*ptr == 'M')
            size <<= 20;
        if ((count == 0) || (size == 0))
            return (1);
        dev->segs[dev->nsegs].count = count;
        dev->segs[dev->nsegs].size  = size;
        dev->nsegs++;
        dev->flash_size += count * size;

        /* Skip unit and memory type letters */
        while ((*ptr != '\0') && (*ptr != ',') && (*ptr != '/'))
            ptr++;
        if (*ptr != ',')
            break;
        ptr++;
    }
    return (0);
}

/*
 * dfu_gd32_layout() sets the flash layout of a GD32F10x from the model
 *                   code reported by its ROM bootloader. The third
 *                   character is the flash size code. The connectivity
 *                   line (GD32F105 / GD32F107) and parts with more than
 *                   512 KB of flash have 2 KB pages; others have 1 KB.
 *
 * @return 0 - Success.
 * @return 1 - Model code not recognized.
 */
static int
dfu_gd32_layout(dfu_dev_t *dev)
{
    static const struct {
        char         code;
        unsigned int kbytes;
    } sizes[] = {
        { '4', 16 }, { '6', 32 }, { '8', 64 }, { 'B', 128 }, { 'C', 256 },
        { 'D', 384 }, { 'E', 512 }, { 'F', 768 }, { 'G', 1024 },
        { 'I', 2048 }, { 'K', 3072 },
    };
    const char  *model = dev->info.serial;
    unsigned int kbytes = 0;
    unsigned int page;
    unsigned int pos;

    if (strlen(model) < 3)
        return (1);
    for (pos = 0; pos < sizeof (sizes) / sizeof (sizes[0]); pos++)
        if (sizes[pos].code == model[2])
            kbytes = sizes[pos].kbytes;
    if (kbytes == 0)
        return (1);
    if ((model[0] == '5') || (model[0] == '7') || (kbytes > 512))
        page = 2048;
    else
        page = 1024;
    dev->flash_base = GD32_FLASH_BASE;
    dev->flash_size = kbytes << 10;
    dev->nsegs = 1;
    dev->segs[0].size = page;
    dev->segs[0].count = dev->flash_size / page;
    return (0);
}

/*
 * dfu_find_xfer_size() locates the DFU functional descriptor in a block of
 *                      extra descriptor bytes and returns wTransferSize,
 *                      or 0 if not found.
 */
static unsigned int
dfu_find_xfer_size(const unsigned char *extra, int len)
{
    int pos = 0;

    while (pos + 2 <= len) {
        unsigned int dlen = extra[pos];
        if (dlen < 2)
            break;
        if ((extra[pos + 1] == USB_DT_DFU_FUNCTIONAL) && (dlen >= 7) &&
            (pos + 7 <= len)) {
            return (extra[pos + 5] | (extra[pos + 6] << 8));
        }
        pos += dlen;
    }
    return (0);
}

/*
 * dfu_setup() locates the flash alternate setting of a DFU device, claims
 *             and selects it, and reads the memory layout and transfer
 *             size.
 *
 * @return 0 - Success.
 * @return otherwise a libusb error code.
 */
static int
dfu_setup(dfu_dev_t *dev, libusb_device *udev)
{
    struct libusb_config_descriptor *cfg;
    int          alt_num = -1;
    unsigned int xfer = 0;
    int          rc;
    int          i;
    int          a;

    rc = libusb_get_active_config_descriptor(udev, &cfg);
    if (rc != 0)
        rc = libusb_get_config_descriptor(udev, 0, &cfg);
    if (rc != 0)
        return (rc);

    xfer = dfu_find_xfer_size(cfg->extra, cfg->extra_length);
    for (i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface *intf = &cfg->interface[i];
        for (a = 0; a < intf->num_altsetting; a++) {
            const struct libusb_interface_descriptor *alt;
            unsigned int x;
            char name[128];

            alt = &intf->altsetting[a];
            if ((alt->bInterfaceClass != USB_CLASS_APP_SPEC) ||
                (alt->bInterfaceSubClass != USB_SUBCLASS_DFU)) {
                continue;
            }
            x = dfu_find_xfer_size(alt->extra, alt->extra_length);
            if (x != 0)
                xfer = x;
            if (alt_num >= 0)
                continue;
            if (dev->info.is_gd32) {
                /* GD32 flash is alternate setting 0; strings are not usable */
                if ((alt->bAlternateSetting == 0) &&
                    (dfu_gd32_layout(dev) == 0)) {
                    alt_num = 0;
                    dev->iface = alt->bInterfaceNumber;
                }
                continue;
            }
            dfu_get_string(dev->handle, alt->iInterface, name, sizeof (name));
            if ((strncmp(name, DFU_FLASH_ALT, strlen(DFU_FLASH_ALT)) == 0) &&
                (dfu_parse_layout(dev, name) == 0)) {
                alt_num = alt->bAlternateSetting;
                dev->iface = alt->bInterfaceNumber;
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    if (alt_num < 0)
        return (LIBUSB_ERROR_NOT_FOUND);
    dev->xfer_size = (xfer != 0) ? xfer : DFU_DEF_XFER_SIZE;

    rc = libusb_claim_interface(dev->handle, dev->iface);
    if (rc != 0)
        return (rc);
    rc = libusb_set_interface_alt_setting(dev->handle, dev->iface, alt_num);
    if (rc != 0) {
        libusb_release_interface(dev->handle, dev->iface);
        return (rc);
    }
    return (0);
}

/*
 * dfu_open() locates and opens a DFU device which matches the specified
 *            identifier (USB serial number or USB port path).
 *
 * @param [in]  id  - Device identifier, or NULL for any.
 * @param [out] why - Last libusb error which prevented an open (or 0).
 *
 * @return      Device, or NULL if no device was found.
 */
static dfu_dev_t *
dfu_open(const char *id, int *why)
{
    libusb_device **devs;
    dfu_dev_t      *dev = NULL;
    ssize_t         count;
    ssize_t         cur;

    *why = 0;
    if (dfu_init() != 0) {
        *why = LIBUSB_ERROR_OTHER;
        return (NULL);
    }
    count = libusb_get_device_list(dfu_ctx, &devs);
    if (count < 0) {
        *why = (int) count;
        return (NULL);
    }
    for (cur = 0; cur < count; cur++) {
        struct libusb_device_descriptor desc;
        libusb_device_handle *handle;
        int rc;

        if ((libusb_get_device_descriptor(devs[cur], &desc) != 0) ||
            !dfu_is_dfu_device(&desc)) {
            continue;
        }
        rc = libusb_open(devs[cur], &handle);
        if (rc != 0) {
            *why = rc;
            continue;
        }
        dev = calloc(1, sizeof (*dev));
        if (dev == NULL) {
            libusb_close(handle);
            *why = LIBUSB_ERROR_NO_MEM;
            break;
        }
        dev->handle = handle;
        dfu_get_info(devs[cur], handle, &desc, &dev->info);
        if (dfu_id_match(&dev->info, id)) {
            rc = dfu_setup(dev, devs[cur]);
            if (rc == 0)
                break;
            *why = rc;
        }
        libusb_close(handle);
        free(dev);
        dev = NULL;
    }
    libusb_free_device_list(devs, 1);
    return (dev);
}

/*
 * dfu_close() releases and closes the DFU device.
 */
void
dfu_close(dfu_dev_t *dev)
{
    if (dev == NULL)
        return;
    libusb_release_interface(dev->handle, dev->iface);
    libusb_close(dev->handle);
    free(dev);
}

/*
 * dfu_install_drivers() assigns the WinUSB driver to each attached
 *                       supported DFU device which has no driver, so that
 *                       libusb can access it (Windows only).
 *
 * @return true  - A driver was installed.
 * @return false - Nothing was installed.
 */
bool
dfu_install_drivers(void)
{
#ifdef _WIN32
    return (windrv_fix_dfu());
#else
    return (false);
#endif
}

/*
 * dfu_wait_open() waits up to timeout_ms for a DFU device matching the
 *                 identifier (USB serial number or USB port path) to
 *                 appear, then opens it. A NULL or empty id accepts any
 *                 device. A message is displayed on failure.
 *
 * @return Device, or NULL if it could not be opened.
 */
dfu_dev_t *
dfu_wait_open(const char *id, unsigned int timeout_ms)
{
    dfu_dev_t   *dev;
    unsigned int waited;
    int          why = 0;

    for (waited = 0; waited < timeout_ms; waited += 100) {
        dev = dfu_open(id, &why);
        if (dev != NULL)
            return (dev);
        if ((waited >= 1000) && dfu_install_drivers())
            waited = 0;  // Allow time for the device to restart
        time_delay_msec(100);
    }
    if (why == LIBUSB_ERROR_NOT_FOUND) {
        printf("DFU device has no recognized flash memory region\n");
    } else if (why != 0) {
        printf("Failed to open DFU device: %s\n", libusb_error_name(why));
        if (why == LIBUSB_ERROR_ACCESS)
            printf("Check permissions (see fw/udev/70-st-link.rules)\n");
    } else {
        printf("DFU device%s%s did not appear\n",
               ((id != NULL) && (id[0] != '\0')) ? " matching " : "",
               (id != NULL) ? id : "");
    }
    return (NULL);
}

const dfu_info_t *
dfu_dev_info(dfu_dev_t *dev)
{
    return (&dev->info);
}

unsigned int
dfu_xfer_size(dfu_dev_t *dev)
{
    return (dev->xfer_size);
}

uint32_t
dfu_flash_base(dfu_dev_t *dev)
{
    return (dev->flash_base);
}

uint32_t
dfu_flash_size(dfu_dev_t *dev)
{
    return (dev->flash_size);
}

/*
 * dfu_page_size() returns the erase page size at the specified flash
 *                 address, or 0 if the address is outside of flash.
 */
unsigned int
dfu_page_size(dfu_dev_t *dev, uint32_t addr)
{
    uint32_t     seg_base = dev->flash_base;
    unsigned int seg;

    for (seg = 0; seg < dev->nsegs; seg++) {
        uint32_t seg_len = dev->segs[seg].count * dev->segs[seg].size;
        if ((addr >= seg_base) && (addr - seg_base < seg_len))
            return (dev->segs[seg].size);
        seg_base += seg_len;
    }
    return (0);
}

/*
 * dfu_get_status() issues DFU_GETSTATUS, returning the bStatus and
 *                  bState values, and the requested poll timeout.
 *
 * @return 0 - Success.
 * @return 1 - Failure.
 */
static int
dfu_get_status(dfu_dev_t *dev, unsigned int *status, unsigned int *state,
               unsigned int *poll_ms)
{
    uint8_t buf[6];
    int     rc;

    rc = libusb_control_transfer(dev->handle, DFU_REQTYPE_IN,
                                 DFU_REQ_GETSTATUS, 0, dev->iface,
                                 buf, sizeof (buf), DFU_TIMEOUT_MS);
    if (rc != sizeof (buf))
        return (1);
    *status  = buf[0];
    *poll_ms = buf[1] | (buf[2] << 8) | (buf[3] << 16);
    *state   = buf[4];
    return (0);
}

/*
 * dfu_request() issues a DFU class request with no data stage.
 */
static int
dfu_request(dfu_dev_t *dev, unsigned int req)
{
    int rc = libusb_control_transfer(dev->handle, DFU_REQTYPE_OUT, req, 0,
                                     dev->iface, NULL, 0, DFU_TIMEOUT_MS);
    return (rc != 0);
}

/*
 * dfu_dnload_xfer() issues DFU_DNLOAD with the specified block.
 *
 *                   The GD32 ROM bootloader occasionally stalls a
 *                   DFU_DNLOAD which closely follows the DFU_GETSTATUS
 *                   that completed the previous operation, while
 *                   remaining idle with no error. In that case, the
 *                   request was not accepted, so it is sent again after
 *                   a short delay.
 *
 * @return Number of bytes transferred, or a libusb error code.
 */
static int
dfu_dnload_xfer(dfu_dev_t *dev, unsigned int blocknum, const void *data,
                unsigned int len)
{
    unsigned int status;
    unsigned int state;
    unsigned int poll_ms;
    unsigned int tries;
    int          rc = LIBUSB_ERROR_PIPE;

    for (tries = 0; tries < DFU_STALL_RETRIES; tries++) {
        /* libusb doesn't modify OUT transfer data */
        rc = libusb_control_transfer(dev->handle, DFU_REQTYPE_OUT,
                                     DFU_REQ_DNLOAD, blocknum, dev->iface,
                                     (unsigned char *) (uintptr_t) data, len,
                                     DFU_TIMEOUT_MS);
        if ((rc != LIBUSB_ERROR_PIPE) ||
            (dfu_get_status(dev, &status, &state, &poll_ms) != 0) ||
            (status != 0) ||
            ((state != DFU_STATE_IDLE) && (state != DFU_STATE_DNLOAD_IDLE))) {
            break;
        }
        time_delay_msec(2);
    }
    return (rc);
}

/*
 * dfu_download() sends a DFU_DNLOAD block, then polls DFU_GETSTATUS until
 *                the device has completed the operation.
 *
 * @return 0 - Success.
 * @return 1 - Failure (an error message is in errbuf).
 */
int
dfu_download(dfu_dev_t *dev, unsigned int blocknum, const void *data,
             unsigned int len, char *errbuf, size_t errlen)
{
    unsigned int status;
    unsigned int state;
    unsigned int poll_ms;
    unsigned int polls;
    int          rc;

    rc = dfu_dnload_xfer(dev, blocknum, data, len);
    if (rc != (int) len) {
        snprintf(errbuf, errlen, "DNLOAD: %s",
                 (rc < 0) ? libusb_error_name(rc) : "short transfer");
        return (1);
    }
    for (polls = 0; polls < DFU_MAX_POLLS; polls++) {
        if (dfu_get_status(dev, &status, &state, &poll_ms) != 0) {
            snprintf(errbuf, errlen, "GETSTATUS failed");
            return (1);
        }
        if (status != 0) {
            snprintf(errbuf, errlen, "status %u state %u", status, state);
            return (1);
        }
        if (state == DFU_STATE_DNLOAD_IDLE)
            return (0);
        if ((state != DFU_STATE_DNBUSY) && (state != DFU_STATE_DNLOAD_SYNC)) {
            snprintf(errbuf, errlen, "unexpected state %u", state);
            return (1);
        }
        if (poll_ms > DFU_MAX_POLL_MS)
            poll_ms = DFU_MAX_POLL_MS;
        if (poll_ms != 0)
            time_delay_msec(poll_ms);
    }
    snprintf(errbuf, errlen, "timeout waiting for device");
    return (1);
}

/*
 * dfu_dfuse_cmd() sends a DfuSe command (erase or set address) with a
 *                 32-bit little-endian address argument.
 */
static int
dfu_dfuse_cmd(dfu_dev_t *dev, unsigned int cmd, uint32_t addr,
              char *errbuf, size_t errlen)
{
    uint8_t buf[5];

    buf[0] = cmd;
    buf[1] = (uint8_t) addr;
    buf[2] = (uint8_t) (addr >> 8);
    buf[3] = (uint8_t) (addr >> 16);
    buf[4] = (uint8_t) (addr >> 24);
    return (dfu_download(dev, 0, buf, sizeof (buf), errbuf, errlen));
}

/*
 * dfu_erase_page() erases the flash page at the specified address.
 */
int
dfu_erase_page(dfu_dev_t *dev, uint32_t addr, char *errbuf, size_t errlen)
{
    return (dfu_dfuse_cmd(dev, DFUSE_CMD_ERASE, addr, errbuf, errlen));
}

/*
 * dfu_set_address() sets the flash address at which data block
 *                   DFU_FIRST_BLOCK is written. Each following block
 *                   is written dfu_xfer_size() bytes after the previous
 *                   block.
 */
int
dfu_set_address(dfu_dev_t *dev, uint32_t addr, char *errbuf, size_t errlen)
{
    return (dfu_dfuse_cmd(dev, DFUSE_CMD_SETADDR, addr, errbuf, errlen));
}

/*
 * dfu_recover() returns the DFU device to the idle state after an error.
 */
void
dfu_recover(dfu_dev_t *dev)
{
    unsigned int status;
    unsigned int state;
    unsigned int poll_ms;

    if ((dfu_get_status(dev, &status, &state, &poll_ms) == 0) &&
        (state == DFU_STATE_ERROR)) {
        (void) dfu_request(dev, DFU_REQ_CLRSTATUS);
    }
    (void) dfu_request(dev, DFU_REQ_ABORT);

    /*
     * The STM32 ROM bootloader doesn't complete a DFU_DNLOAD which
     * immediately follows DFU_ABORT (the status stage times out), unless
     * a DFU_GETSTATUS is issued in between.
     */
    (void) dfu_get_status(dev, &status, &state, &poll_ms);
}

/*
 * dfu_leave() tells the device that the download is complete. The
 *             address is first set to the start of flash, where the
 *             ROM bootloader starts the program. The Kicksmash updater
 *             instead resets the CPU.
 */
void
dfu_leave(dfu_dev_t *dev)
{
    unsigned int status;
    unsigned int state;
    unsigned int poll_ms;
    char         errbuf[80];

    (void) dfu_set_address(dev, dev->flash_base, errbuf, sizeof (errbuf));

    /*
     * Zero-length download of a data block (not block 0, which the ROM
     * bootloader treats as a command) requests manifestation.
     */
    (void) dfu_dnload_xfer(dev, DFU_FIRST_BLOCK, NULL, 0);

    /* Status read triggers the reset or jump; it might not complete */
    if ((dfu_get_status(dev, &status, &state, &poll_ms) != 0) ||
        dev->info.is_updater) {
        return;
    }

    /*
     * If the device is waiting for a USB reset before it starts the
     * program (dfuMANIFEST-WAIT-RESET), provide one.
     */
    if (poll_ms > DFU_MAX_POLL_MS)
        poll_ms = DFU_MAX_POLL_MS;
    time_delay_msec(poll_ms);
    if ((dfu_get_status(dev, &status, &state, &poll_ms) == 0) &&
        (state == DFU_STATE_MANIFEST_WAIT_RESET)) {
        (void) libusb_reset_device(dev->handle);
    }
}

#else /* !HAVE_LIBUSB */

bool
dfu_supported(void)
{
    return (false);
}

bool
dfu_install_drivers(void)
{
    return (false);
}

unsigned int
dfu_list(dfu_info_t *list, unsigned int max)
{
    (void) list;
    (void) max;
    return (0);
}

dfu_dev_t *
dfu_wait_open(const char *id, unsigned int timeout_ms)
{
    (void) id;
    (void) timeout_ms;
    return (NULL);
}

void
dfu_close(dfu_dev_t *dev)
{
    (void) dev;
}

const dfu_info_t *
dfu_dev_info(dfu_dev_t *dev)
{
    (void) dev;
    return (NULL);
}

unsigned int
dfu_xfer_size(dfu_dev_t *dev)
{
    (void) dev;
    return (0);
}

uint32_t
dfu_flash_base(dfu_dev_t *dev)
{
    (void) dev;
    return (0);
}

uint32_t
dfu_flash_size(dfu_dev_t *dev)
{
    (void) dev;
    return (0);
}

unsigned int
dfu_page_size(dfu_dev_t *dev, uint32_t addr)
{
    (void) dev;
    (void) addr;
    return (0);
}

void
dfu_recover(dfu_dev_t *dev)
{
    (void) dev;
}

int
dfu_erase_page(dfu_dev_t *dev, uint32_t addr, char *errbuf, size_t errlen)
{
    (void) dev;
    (void) addr;
    snprintf(errbuf, errlen, "DFU not supported");
    return (1);
}

int
dfu_set_address(dfu_dev_t *dev, uint32_t addr, char *errbuf, size_t errlen)
{
    return (dfu_erase_page(dev, addr, errbuf, errlen));
}

int
dfu_download(dfu_dev_t *dev, unsigned int blocknum, const void *data,
             unsigned int len, char *errbuf, size_t errlen)
{
    (void) blocknum;
    (void) data;
    (void) len;
    return (dfu_erase_page(dev, 0, errbuf, errlen));
}

void
dfu_leave(dfu_dev_t *dev)
{
    (void) dev;
}

#endif /* HAVE_LIBUSB */
