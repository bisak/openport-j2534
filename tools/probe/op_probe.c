/*
 * op_probe — enumerate the OpenPort cable and dump its full USB descriptor tree.
 *
 * Independent observation tool: everything printed here is read from the device
 * itself, so it is the ground truth that docs/PROTOCOL.md is written against.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libusb.h>

#define OP_VID 0x0403
#define OP_PID 0xCC4D

static const char *xfer_name(uint8_t attr)
{
    switch (attr & LIBUSB_TRANSFER_TYPE_MASK) {
    case LIBUSB_TRANSFER_TYPE_CONTROL:     return "control";
    case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS: return "isochronous";
    case LIBUSB_TRANSFER_TYPE_BULK:        return "bulk";
    case LIBUSB_TRANSFER_TYPE_INTERRUPT:   return "interrupt";
    default:                               return "unknown";
    }
}

static const char *class_name(uint8_t c)
{
    switch (c) {
    case LIBUSB_CLASS_PER_INTERFACE: return "per-interface";
    case LIBUSB_CLASS_AUDIO:         return "audio";
    case LIBUSB_CLASS_COMM:          return "comm";
    case LIBUSB_CLASS_HID:           return "HID";
    case LIBUSB_CLASS_MASS_STORAGE:  return "MASS STORAGE (microSD inserted?)";
    case LIBUSB_CLASS_HUB:           return "hub";
    case LIBUSB_CLASS_DATA:          return "data";
    case LIBUSB_CLASS_VENDOR_SPEC:   return "vendor-specific";
    default:                         return "other";
    }
}

static void dump_string(libusb_device_handle *h, uint8_t idx, const char *label)
{
    unsigned char buf[256];
    int r;
    if (idx == 0) {
        printf("  %-16s (none)\n", label);
        return;
    }
    r = libusb_get_string_descriptor_ascii(h, idx, buf, sizeof buf);
    if (r < 0) {
        printf("  %-16s <error %s>\n", label, libusb_error_name(r));
        return;
    }
    printf("  %-16s \"%s\"\n", label, buf);
}

int main(void)
{
    libusb_context *ctx = NULL;
    libusb_device **list = NULL;
    ssize_t n, i;
    int r, found = 0;

    r = libusb_init(&ctx);
    if (r != LIBUSB_SUCCESS) {
        fprintf(stderr, "libusb_init: %s\n", libusb_error_name(r));
        return 1;
    }

    n = libusb_get_device_list(ctx, &list);
    if (n < 0) {
        fprintf(stderr, "libusb_get_device_list: %s\n", libusb_error_name((int)n));
        libusb_exit(ctx);
        return 1;
    }

    for (i = 0; i < n; i++) {
        struct libusb_device_descriptor dd;
        libusb_device_handle *h = NULL;
        uint8_t cfg_i;

        if (libusb_get_device_descriptor(list[i], &dd) != LIBUSB_SUCCESS)
            continue;
        if (dd.idVendor != OP_VID || dd.idProduct != OP_PID)
            continue;

        found = 1;
        printf("=== DEVICE %04x:%04x  bus %u addr %u ===\n",
               dd.idVendor, dd.idProduct,
               libusb_get_bus_number(list[i]),
               libusb_get_device_address(list[i]));
        printf("  bcdUSB           %04x\n", dd.bcdUSB);
        printf("  bcdDevice        %04x\n", dd.bcdDevice);
        printf("  bDeviceClass     0x%02x (%s)\n", dd.bDeviceClass, class_name(dd.bDeviceClass));
        printf("  bDeviceSubClass  0x%02x\n", dd.bDeviceSubClass);
        printf("  bDeviceProtocol  0x%02x\n", dd.bDeviceProtocol);
        printf("  bMaxPacketSize0  %u\n", dd.bMaxPacketSize0);
        printf("  bNumConfigs      %u\n", dd.bNumConfigurations);

        r = libusb_open(list[i], &h);
        if (r == LIBUSB_SUCCESS) {
            dump_string(h, dd.iManufacturer, "iManufacturer");
            dump_string(h, dd.iProduct,      "iProduct");
            dump_string(h, dd.iSerialNumber, "iSerialNumber");
        } else {
            printf("  <libusb_open failed: %s>\n", libusb_error_name(r));
        }

        for (cfg_i = 0; cfg_i < dd.bNumConfigurations; cfg_i++) {
            struct libusb_config_descriptor *cd = NULL;
            uint8_t ifc;

            r = libusb_get_config_descriptor(list[i], cfg_i, &cd);
            if (r != LIBUSB_SUCCESS) {
                printf("  config[%u]: <error %s>\n", cfg_i, libusb_error_name(r));
                continue;
            }
            printf("  config[%u] bConfigurationValue=%u bNumInterfaces=%u "
                   "bmAttributes=0x%02x MaxPower=%umA\n",
                   cfg_i, cd->bConfigurationValue, cd->bNumInterfaces,
                   cd->bmAttributes, cd->MaxPower * 2);

            for (ifc = 0; ifc < cd->bNumInterfaces; ifc++) {
                const struct libusb_interface *itf = &cd->interface[ifc];
                int alt;
                for (alt = 0; alt < itf->num_altsetting; alt++) {
                    const struct libusb_interface_descriptor *id = &itf->altsetting[alt];
                    uint8_t ep;
                    printf("    intf %u alt %u: class 0x%02x (%s) sub 0x%02x proto 0x%02x, %u endpoints\n",
                           id->bInterfaceNumber, id->bAlternateSetting,
                           id->bInterfaceClass, class_name(id->bInterfaceClass),
                           id->bInterfaceSubClass, id->bInterfaceProtocol,
                           id->bNumEndpoints);
                    if (h) {
                        int kd = libusb_kernel_driver_active(h, id->bInterfaceNumber);
                        printf("      kernel_driver_active=%s\n",
                               kd == 1 ? "YES" : kd == 0 ? "no" :
                               kd == LIBUSB_ERROR_NOT_SUPPORTED ? "n/a (macOS)" :
                               libusb_error_name(kd));
                    }
                    for (ep = 0; ep < id->bNumEndpoints; ep++) {
                        const struct libusb_endpoint_descriptor *ed = &id->endpoint[ep];
                        printf("      EP 0x%02x  %-11s %-3s wMaxPacketSize=%u bInterval=%u\n",
                               ed->bEndpointAddress,
                               xfer_name(ed->bmAttributes),
                               (ed->bEndpointAddress & LIBUSB_ENDPOINT_IN) ? "IN" : "OUT",
                               ed->wMaxPacketSize, ed->bInterval);
                    }
                }
            }
            libusb_free_config_descriptor(cd);
        }

        if (h) {
            int cfg = -1;
            if (libusb_get_configuration(h, &cfg) == LIBUSB_SUCCESS)
                printf("  active configuration = %d\n", cfg);
            libusb_close(h);
        }
        printf("\n");
    }

    libusb_free_device_list(list, 1);
    libusb_exit(ctx);

    if (!found) {
        fprintf(stderr, "No %04x:%04x device found.\n", OP_VID, OP_PID);
        return 2;
    }
    return 0;
}
