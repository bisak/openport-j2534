/*
 * op_usb.c — USB bulk transport for the OpenPort family.
 *
 * Device facts this depends on, all read back from a real cable by
 * tools/probe/op_probe.c rather than assumed (see docs/PROTOCOL.md):
 *
 *   0403:cc4d, USB 1.1, one configuration, two interfaces.
 *     interface 0  class 0x02 (CDC comm)  — interrupt IN 0x81, notifications
 *     interface 1  class 0x0a (CDC data)  — bulk OUT 0x02, bulk IN 0x82
 *
 * macOS binds AppleUSBCDCACM to BOTH interfaces. Claiming interface 0 fails
 * with LIBUSB_ERROR_ACCESS and detaching is not permitted either, but the data
 * interface can still be claimed — so we claim only the interface that carries
 * the bulk pair and never touch the comm interface. That is why this driver
 * needs no sudo and no kext surgery.
 *
 * Linux binds cdc_acm to the comm interface, and cdc_acm claims the data
 * interface itself. Claiming the data interface needs its kernel driver
 * detached, and detaching either interface tears the whole ACM device down,
 * /dev/ttyACM* included. Close therefore reattaches every interface the kernel
 * had at open, lowest number first: re-probing the comm interface is what
 * brings the tty back.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "op_transport.h"
#include "op_log.h"

#include <libusb.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OP_VID 0x0403
#define OP_PID 0xCC4D

/* libusb treats timeout 0 as "block forever". We never want that: a wedged
 * cable must surface as a timeout, not a hang. A caller asking for 0 means
 * "poll", which we serve with the shortest interval the stack can express. */
#define OP_MIN_TIMEOUT_MS 1u

typedef struct {
    libusb_context       *ctx;
    libusb_device_handle *handle;
    int                   interface_num;
    int                   claimed;
    uint32_t              kernel_bound;   /* interfaces with a kernel driver at open */
    uint8_t               ep_in;
    uint8_t               ep_out;
    uint16_t              max_packet_in;
} op_usb;

static _Thread_local char g_detail[256];

static void set_detail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void set_detail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_detail, sizeof g_detail, fmt, ap);
    va_end(ap);
}

const char *op_transport_last_detail(void)
{
    return g_detail[0] != '\0' ? g_detail : NULL;
}

static op_status from_libusb(int r)
{
    switch (r) {
    case LIBUSB_SUCCESS:             return OP_OK;
    case LIBUSB_ERROR_TIMEOUT:       return OP_ERR_TIMEOUT;
    case LIBUSB_ERROR_NO_DEVICE:     return OP_ERR_NO_DEVICE;
    case LIBUSB_ERROR_NOT_FOUND:     return OP_ERR_NO_DEVICE;
    case LIBUSB_ERROR_ACCESS:        return OP_ERR_ACCESS;
    case LIBUSB_ERROR_BUSY:          return OP_ERR_ACCESS;
    case LIBUSB_ERROR_OVERFLOW:      return OP_ERR_OVERFLOW;
    case LIBUSB_ERROR_INVALID_PARAM: return OP_ERR_PARAM;
    default:                         return OP_ERR_IO;
    }
}

/* Walk every configuration/interface/altsetting looking for an interface that
 * exposes a bulk IN and a bulk OUT. Records whether any interface claims the
 * mass-storage class so the caller can name the microSD problem exactly. */
static int find_bulk_interface(libusb_device *dev, int *out_intf,
                               uint8_t *out_in, uint8_t *out_out,
                               uint16_t *out_max_in, int *saw_mass_storage)
{
    struct libusb_device_descriptor dd;
    uint8_t c;

    *saw_mass_storage = 0;

    if (libusb_get_device_descriptor(dev, &dd) != LIBUSB_SUCCESS) return 0;
    if (dd.bDeviceClass == LIBUSB_CLASS_MASS_STORAGE) *saw_mass_storage = 1;

    for (c = 0; c < dd.bNumConfigurations; c++) {
        struct libusb_config_descriptor *cd = NULL;
        uint8_t i;

        if (libusb_get_config_descriptor(dev, c, &cd) != LIBUSB_SUCCESS) continue;

        for (i = 0; i < cd->bNumInterfaces; i++) {
            int alt;
            for (alt = 0; alt < cd->interface[i].num_altsetting; alt++) {
                const struct libusb_interface_descriptor *id =
                    &cd->interface[i].altsetting[alt];
                uint8_t e;
                int have_in = 0, have_out = 0;
                uint8_t ep_in = 0, ep_out = 0;
                uint16_t mps_in = 64;

                if (id->bInterfaceClass == LIBUSB_CLASS_MASS_STORAGE)
                    *saw_mass_storage = 1;

                for (e = 0; e < id->bNumEndpoints; e++) {
                    const struct libusb_endpoint_descriptor *ed = &id->endpoint[e];
                    if ((ed->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) !=
                        LIBUSB_TRANSFER_TYPE_BULK)
                        continue;
                    if (ed->bEndpointAddress & LIBUSB_ENDPOINT_IN) {
                        if (!have_in) {
                            ep_in = ed->bEndpointAddress;
                            mps_in = ed->wMaxPacketSize ? ed->wMaxPacketSize : 64;
                            have_in = 1;
                        }
                    } else if (!have_out) {
                        ep_out = ed->bEndpointAddress;
                        have_out = 1;
                    }
                }

                if (have_in && have_out) {
                    *out_intf    = id->bInterfaceNumber;
                    *out_in      = ep_in;
                    *out_out     = ep_out;
                    *out_max_in  = mps_in;
                    libusb_free_config_descriptor(cd);
                    return 1;
                }
            }
        }
        libusb_free_config_descriptor(cd);
    }
    return 0;
}

/* Which interfaces of the active configuration have a kernel driver bound,
 * as a bitmask by interface number. */
static uint32_t kernel_bound_interfaces(libusb_device_handle *h, libusb_device *dev)
{
    struct libusb_config_descriptor *cd = NULL;
    uint32_t bound = 0;
    uint8_t i;

    if (libusb_get_active_config_descriptor(dev, &cd) != LIBUSB_SUCCESS) return 0;
    for (i = 0; i < cd->bNumInterfaces; i++) {
        int num = cd->interface[i].num_altsetting > 0
                      ? cd->interface[i].altsetting[0].bInterfaceNumber : -1;
        if (num >= 0 && num < 32 && libusb_kernel_driver_active(h, num) == 1)
            bound |= 1u << num;
    }
    libusb_free_config_descriptor(cd);
    return bound;
}

static op_status usb_write(op_transport *t, const uint8_t *buf, size_t len,
                           unsigned timeout_ms)
{
    op_usb *u;
    size_t sent = 0;

    if (t == NULL || t->impl == NULL || (buf == NULL && len > 0))
        return OP_ERR_PARAM;
    if (len > (size_t)INT_MAX) return OP_ERR_PARAM;
    u = (op_usb *)t->impl;
    if (timeout_ms == 0) timeout_ms = OP_MIN_TIMEOUT_MS;

    op_log_bytes("TX", buf, len);

    /* Bulk writes can be short. Keep going until the whole command is on the
     * wire or the transfer genuinely fails — a half-written command line would
     * desynchronise the device's parser. */
    while (sent < len) {
        int moved = 0;
        int r = libusb_bulk_transfer(u->handle, u->ep_out,
                                     (unsigned char *)(uintptr_t)(buf + sent),
                                     (int)(len - sent), &moved, timeout_ms);
        if (r != LIBUSB_SUCCESS) {
            op_logf("usb_write: %s after %zu/%zu bytes",
                    libusb_error_name(r), sent, len);
            if (r == LIBUSB_ERROR_TIMEOUT && moved > 0) {
                sent += (size_t)moved;
                continue;
            }
            return from_libusb(r);
        }
        if (moved <= 0) return OP_ERR_IO;
        sent += (size_t)moved;
    }
    return OP_OK;
}

static op_status usb_read(op_transport *t, uint8_t *buf, size_t cap,
                          size_t *got, unsigned timeout_ms)
{
    op_usb *u;
    int moved = 0;
    int r;

    if (got != NULL) *got = 0;
    if (t == NULL || t->impl == NULL || buf == NULL || got == NULL || cap == 0)
        return OP_ERR_PARAM;
    if (cap > (size_t)INT_MAX) cap = (size_t)INT_MAX;
    u = (op_usb *)t->impl;
    if (timeout_ms == 0) timeout_ms = OP_MIN_TIMEOUT_MS;

    r = libusb_bulk_transfer(u->handle, u->ep_in, buf, (int)cap, &moved,
                             timeout_ms);
    if (r == LIBUSB_ERROR_TIMEOUT) {
        /* A timeout can still have delivered a partial packet. */
        if (moved > 0) { *got = (size_t)moved; op_log_bytes("RX", buf, *got); return OP_OK; }
        return OP_ERR_TIMEOUT;
    }
    if (r != LIBUSB_SUCCESS) {
        op_logf("usb_read: %s", libusb_error_name(r));
        return from_libusb(r);
    }
    if (moved < 0) return OP_ERR_IO;
    *got = (size_t)moved;
    op_log_bytes("RX", buf, *got);
    return OP_OK;
}

static void usb_close(op_transport *t)
{
    op_usb *u;
    if (t == NULL || t->impl == NULL) return;
    u = (op_usb *)t->impl;
    if (u->handle != NULL) {
        int num;
        if (u->claimed) libusb_release_interface(u->handle, u->interface_num);
        /* Give the kernel back what it had. Lowest number first: on Linux the
         * comm interface's driver claims the data interface as it probes, so
         * the data interface's own attach then finds it already bound. On
         * macOS nothing was detached and nothing is attached. */
        for (num = 0; num < 32; num++) {
            if (!(u->kernel_bound & (1u << num))) continue;
            if (libusb_kernel_driver_active(u->handle, num) != 0) continue;
            if (libusb_attach_kernel_driver(u->handle, num) == LIBUSB_SUCCESS)
                op_logf("usb close: kernel driver reattached on interface %d", num);
        }
        libusb_close(u->handle);
    }
    if (u->ctx != NULL) libusb_exit(u->ctx);
    free(u);
    t->impl = NULL;
    t->ops  = NULL;
}

static const op_transport_ops g_usb_ops = {
    "usb-bulk", usb_write, usb_read, usb_close
};

op_status op_usb_open(op_transport *out)
{
    libusb_context *ctx = NULL;
    libusb_device **list = NULL;
    ssize_t n, i;
    int r;
    op_usb *u = NULL;
    op_status st = OP_ERR_NO_DEVICE;
    int mass_storage_seen = 0;

    g_detail[0] = '\0';
    if (out == NULL) return OP_ERR_PARAM;
    out->ops = NULL;
    out->impl = NULL;

    r = libusb_init(&ctx);
    if (r != LIBUSB_SUCCESS) {
        set_detail("libusb_init failed: %s", libusb_error_name(r));
        return from_libusb(r);
    }

    n = libusb_get_device_list(ctx, &list);
    if (n < 0) {
        set_detail("libusb_get_device_list failed: %s", libusb_error_name((int)n));
        libusb_exit(ctx);
        return from_libusb((int)n);
    }

    for (i = 0; i < n; i++) {
        struct libusb_device_descriptor dd;
        int intf = -1, ms = 0;
        uint8_t ep_in = 0, ep_out = 0;
        uint16_t mps = 64;
        uint32_t bound;
        libusb_device_handle *h = NULL;

        if (libusb_get_device_descriptor(list[i], &dd) != LIBUSB_SUCCESS) continue;
        if (dd.idVendor != OP_VID || dd.idProduct != OP_PID) continue;

        if (!find_bulk_interface(list[i], &intf, &ep_in, &ep_out, &mps, &ms)) {
            mass_storage_seen |= ms;
            continue;
        }
        mass_storage_seen |= ms;

        r = libusb_open(list[i], &h);
        if (r != LIBUSB_SUCCESS) {
            set_detail("found %04x:%04x but libusb_open failed: %s",
                       OP_VID, OP_PID, libusb_error_name(r));
            st = from_libusb(r);
            continue;
        }

        /* Recorded before anything is detached; close reattaches these. */
        bound = kernel_bound_interfaces(h, list[i]);

        /* Best-effort. macOS refuses this for CDC interfaces and does not need
         * it: the data interface can be claimed regardless. Linux needs it.
         * Only a claim failure is fatal. */
        (void)libusb_detach_kernel_driver(h, intf);

        r = libusb_claim_interface(h, intf);
        if (r != LIBUSB_SUCCESS) {
            set_detail("interface %d busy: %s. Another program may have the "
                       "cable open.", intf, libusb_error_name(r));
            libusb_close(h);
            st = from_libusb(r);
            continue;
        }

        u = (op_usb *)calloc(1, sizeof *u);
        if (u == NULL) {
            libusb_release_interface(h, intf);
            libusb_close(h);
            libusb_free_device_list(list, 1);
            libusb_exit(ctx);
            set_detail("out of memory");
            return OP_ERR_IO;
        }
        u->ctx           = ctx;
        u->handle        = h;
        u->interface_num = intf;
        u->claimed       = 1;
        u->kernel_bound  = bound;
        u->ep_in         = ep_in;
        u->ep_out        = ep_out;
        u->max_packet_in = mps;

        out->ops  = &g_usb_ops;
        out->impl = u;

        op_logf("usb open: intf %d, bulk in 0x%02x out 0x%02x, mps %u",
                intf, ep_in, ep_out, (unsigned)mps);
        libusb_free_device_list(list, 1);
        return OP_OK;
    }

    libusb_free_device_list(list, 1);
    libusb_exit(ctx);

    if (st == OP_ERR_NO_DEVICE && mass_storage_seen) {
        set_detail("The OpenPort enumerated as USB mass storage. Remove the "
                   "microSD card and re-plug the cable.");
        return OP_ERR_MASS_STORAGE;
    }
    if (st == OP_ERR_NO_DEVICE && g_detail[0] == '\0')
        set_detail("No OpenPort (%04x:%04x) found on any USB bus.", OP_VID, OP_PID);
    return st;
}

/*
 * Transport selection. OPENPORT_DEVICE names a character device to use
 * instead of USB — the cable's own CDC-ACM node, or a pty backed by a
 * simulator. Unset (the normal case) means USB, so nothing about the
 * hardware path changes.
 */
op_status op_transport_open(op_transport *out)
{
    const char *dev = getenv("OPENPORT_DEVICE");
    if (dev != NULL && dev[0] != '\0') {
        op_logf("OPENPORT_DEVICE set: using serial transport %s", dev);
        return op_serial_open(out, dev);
    }
    return op_usb_open(out);
}
