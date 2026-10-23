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
 * Receiving: the cable sends each record as its own short packet and holds
 * about 1.1 KB of them, some 17 ms of a dense stream, for a host that is not
 * asking; past that it discards new CAN frames and marks nothing (measured
 * 2026-10-23, PROTOCOL.md section 1). So the bulk IN pipe always has
 * USB_RX_TRANSFERS reads posted, resubmitted by a thread that does nothing
 * else and copies what arrives into a ring that usb_read drains. One read
 * completes per packet, so their count is how long that thread may stall. On
 * macOS a paging stall outlasts that, which is why op_transport_open prefers
 * the cable's serial node there.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE /* pthread_set_qos_class_self_np */
#endif
#include "op_transport.h"
#include "op_log.h"
#include "op_time.h"

#include <errno.h>
#include <libusb.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif

#define OP_VID            0x0403
#define OP_PID            0xCC4D
/* With a microSD card inserted the cable enumerates as a storage composite
 * with no serial link (user report, PROTOCOL.md section 12); in its
 * bootloader it is cc4b (vendor INF). Neither can be driven. */
#define OP_PID_STORAGE    0xCC4C
#define OP_PID_BOOTLOADER 0xCC4B

/* libusb treats timeout 0 as "block forever". We never want that: a wedged
 * cable must surface as a timeout, not a hang. A caller asking for 0 means
 * "poll", which we serve with the shortest interval the stack can express. */
#define OP_MIN_TIMEOUT_MS 1u

/* 256 packets is at least 60 ms of a dense stream, on top of the cable's own
 * 17 ms. A read's buffer is a multiple of the 64-byte packet. */
#define USB_RX_TRANSFERS 256u
#define USB_RX_LEN       256
#define USB_RING_BYTES   (1024u * 1024u)

typedef struct op_usb op_usb;

typedef struct {
    op_usb                 *u;
    struct libusb_transfer *x;
    int                     flying;   /* submitted, callback not yet run */
} rx_slot;

struct op_usb {
    libusb_context       *ctx;
    libusb_device_handle *handle;
    int                   interface_num;
    int                   claimed;
    uint32_t              kernel_bound;   /* interfaces with a kernel driver at open */
    uint8_t               ep_in;
    uint8_t               ep_out;

    /* Receive side, guarded by lock. A read that ended in an error is not
     * resubmitted until usb_read has reported the error, so a failing pipe
     * is retried at the reader's pace rather than in a tight loop. */
    rx_slot               rx[USB_RX_TRANSFERS];
    unsigned              rx_flying;
    /* Reads that completed while the event thread was away, and the most at
     * once: all USB_RX_TRANSFERS means the pipe went unpolled and the cable
     * may have discarded frames. Close logs the most. */
    unsigned              rx_batch, rx_batch_most;
    op_status             rx_error;
    int                   stopping;
    uint8_t              *ring;
    size_t                ring_head, ring_used, ring_dropped;
    pthread_mutex_t       lock;
    pthread_cond_t        arrived;
    pthread_t             events;
    int                   events_started;
    atomic_int            events_stop;
};

static _Thread_local char g_detail[256];

__attribute__((format(printf, 1, 2)))
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

                /* A storage interface has a bulk pair too; claiming it would
                 * send `atz` to a SCSI endpoint and report a protocol fault
                 * instead of the microSD advice below. */
                if (id->bInterfaceClass == LIBUSB_CLASS_MASS_STORAGE) {
                    *saw_mass_storage = 1;
                    continue;
                }

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
            /* The firmware NAKs every OUT packet. Seen 2026-10-12 with the IN
             * queue empty; a USB reset did not clear it, a replug did. */
            if (r == LIBUSB_ERROR_TIMEOUT) return OP_ERR_STALLED;
            return from_libusb(r);
        }
        if (moved <= 0) return OP_ERR_IO;
        sent += (size_t)moved;
    }
    return OP_OK;
}

/* ---- receive ------------------------------------------------------------ */

/* A read whose bytes do not fit is dropped whole and the loss logged by the
 * next usb_read; the reader resynchronises at the next record. */
static void ring_put(op_usb *u, const uint8_t *src, size_t n)
{
    size_t tail, first;
    if (n > USB_RING_BYTES - u->ring_used) {
        u->ring_dropped += n;
        return;
    }
    tail = (u->ring_head + u->ring_used) % USB_RING_BYTES;
    first = USB_RING_BYTES - tail;
    if (first > n) first = n;
    memcpy(u->ring + tail, src, first);
    memcpy(u->ring, src + first, n - first);
    u->ring_used += n;
}

static size_t ring_get(op_usb *u, uint8_t *dst, size_t cap)
{
    size_t n = u->ring_used < cap ? u->ring_used : cap;
    size_t first = USB_RING_BYTES - u->ring_head;
    if (first > n) first = n;
    memcpy(dst, u->ring + u->ring_head, first);
    memcpy(dst + first, u->ring, n - first);
    u->ring_head = (u->ring_head + n) % USB_RING_BYTES;
    u->ring_used -= n;
    return n;
}

static void rx_fail(op_usb *u, op_status st)
{
    if (u->rx_error != OP_ERR_NO_DEVICE) u->rx_error = st;
    pthread_cond_broadcast(&u->arrived);
}

/* Under lock. */
static void rx_submit(rx_slot *s)
{
    op_usb *u = s->u;
    int r = libusb_submit_transfer(s->x);
    if (r == LIBUSB_SUCCESS) {
        s->flying = 1;
        u->rx_flying++;
        return;
    }
    op_logf("usb: a read could not be posted: %s", libusb_error_name(r));
    rx_fail(u, from_libusb(r));
}

static void LIBUSB_CALL rx_done(struct libusb_transfer *x)
{
    rx_slot *s = (rx_slot *)x->user_data;
    op_usb *u = s->u;

    pthread_mutex_lock(&u->lock);
    s->flying = 0;
    u->rx_flying--;
    u->rx_batch++;
    switch (x->status) {
    case LIBUSB_TRANSFER_COMPLETED:
        if (x->actual_length > 0) {
            ring_put(u, x->buffer, (size_t)x->actual_length);
            pthread_cond_broadcast(&u->arrived);
        }
        if (!u->stopping) rx_submit(s);
        break;
    case LIBUSB_TRANSFER_CANCELLED:
        break;
    case LIBUSB_TRANSFER_NO_DEVICE:
        rx_fail(u, OP_ERR_NO_DEVICE);
        break;
    default:
        op_logf("usb: a read ended with transfer status %d", (int)x->status);
        rx_fail(u, OP_ERR_IO);
        break;
    }
    if (u->stopping) pthread_cond_broadcast(&u->arrived);
    pthread_mutex_unlock(&u->lock);
}

static void *rx_events(void *arg)
{
    op_usb *u = (op_usb *)arg;
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    while (!atomic_load(&u->events_stop)) {
        struct timeval tv = { 0, 100000 };
        libusb_handle_events_timeout_completed(u->ctx, &tv, NULL);
        pthread_mutex_lock(&u->lock);
        if (u->rx_batch > u->rx_batch_most) u->rx_batch_most = u->rx_batch;
        u->rx_batch = 0;
        pthread_mutex_unlock(&u->lock);
    }
    return NULL;
}

static op_status rx_start(op_usb *u)
{
    unsigned i;
    op_status st;

    u->ring = malloc(USB_RING_BYTES);
    if (u->ring == NULL) return OP_ERR_IO;
    for (i = 0; i < USB_RX_TRANSFERS; i++) {
        rx_slot *s = &u->rx[i];
        uint8_t *buf = malloc(USB_RX_LEN);
        s->u = u;
        s->x = libusb_alloc_transfer(0);
        if (s->x == NULL || buf == NULL) { free(buf); return OP_ERR_IO; }
        libusb_fill_bulk_transfer(s->x, u->handle, u->ep_in, buf, USB_RX_LEN, rx_done, s, 0);
        s->x->flags = LIBUSB_TRANSFER_FREE_BUFFER;
    }
    if (pthread_create(&u->events, NULL, rx_events, u) != 0) return OP_ERR_IO;
    u->events_started = 1;

    pthread_mutex_lock(&u->lock);
    for (i = 0; i < USB_RX_TRANSFERS && u->rx_error == OP_OK; i++) rx_submit(&u->rx[i]);
    st = u->rx_error;
    pthread_mutex_unlock(&u->lock);
    return st;
}

/* Cancel every posted read and wait for its callback before the event thread
 * stops; a read whose callback never came is left allocated, not freed under
 * libusb. */
static void rx_stop(op_usb *u)
{
    struct timespec deadline;
    unsigned i;
    int rc = 0;

    pthread_mutex_lock(&u->lock);
    u->stopping = 1;
    op_logf("usb: at most %u of the %u posted reads completed before the event thread ran",
            u->rx_batch_most, USB_RX_TRANSFERS);
    for (i = 0; i < USB_RX_TRANSFERS; i++)
        if (u->rx[i].flying) libusb_cancel_transfer(u->rx[i].x);
    op_deadline_in(&deadline, 1000);
    while (u->rx_flying > 0 && u->events_started && rc != ETIMEDOUT)
        rc = pthread_cond_timedwait(&u->arrived, &u->lock, &deadline);
    pthread_mutex_unlock(&u->lock);

    if (u->events_started) {
        atomic_store(&u->events_stop, 1);
        pthread_join(u->events, NULL);
        u->events_started = 0;
    }
    if (u->rx_flying > 0) op_logf("usb close: %u reads never returned", u->rx_flying);
    for (i = 0; i < USB_RX_TRANSFERS; i++)
        if (!u->rx[i].flying) libusb_free_transfer(u->rx[i].x);
    free(u->ring);
}

static op_status usb_read(op_transport *t, uint8_t *buf, size_t cap,
                          size_t *got, unsigned timeout_ms)
{
    op_usb *u;
    struct timespec deadline;
    op_status st = OP_ERR_TIMEOUT;
    size_t dropped;
    unsigned i;
    int rc = 0;

    if (got != NULL) *got = 0;
    if (t == NULL || t->impl == NULL || buf == NULL || got == NULL || cap == 0)
        return OP_ERR_PARAM;
    u = (op_usb *)t->impl;
    op_deadline_in(&deadline, timeout_ms ? timeout_ms : OP_MIN_TIMEOUT_MS);

    pthread_mutex_lock(&u->lock);
    while (u->ring_used == 0 && u->rx_error == OP_OK && rc != ETIMEDOUT)
        rc = pthread_cond_timedwait(&u->arrived, &u->lock, &deadline);
    if (u->ring_used > 0) {
        *got = ring_get(u, buf, cap);
        st = OP_OK;
    } else if (u->rx_error != OP_OK) {
        st = u->rx_error;
        if (st != OP_ERR_NO_DEVICE) {
            u->rx_error = OP_OK;
            for (i = 0; i < USB_RX_TRANSFERS && u->rx_error == OP_OK; i++)
                if (!u->rx[i].flying) rx_submit(&u->rx[i]);
        }
    }
    dropped = u->ring_dropped;
    u->ring_dropped = 0;
    pthread_mutex_unlock(&u->lock);

    if (dropped > 0) op_logf("usb: receive ring full; %zu bytes dropped", dropped);
    if (st == OP_OK) op_log_bytes("RX", buf, *got);
    return st;
}

static void usb_close(op_transport *t)
{
    op_usb *u;
    if (t == NULL || t->impl == NULL) return;
    u = (op_usb *)t->impl;
    rx_stop(u);
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
    pthread_cond_destroy(&u->arrived);
    pthread_mutex_destroy(&u->lock);
    free(u);
    t->impl = NULL;
    t->ops  = NULL;
}

static const op_transport_ops g_usb_ops = {
    usb_write, usb_read, usb_close
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
        if (dd.idVendor != OP_VID) continue;
        if (dd.idProduct == OP_PID_STORAGE) { mass_storage_seen = 1; continue; }
        if (dd.idProduct == OP_PID_BOOTLOADER) {
            set_detail("The OpenPort is in its bootloader (%04x:%04x): a firmware "
                       "update was interrupted. Tactrix's Windows driver recovers it.",
                       OP_VID, OP_PID_BOOTLOADER);
            st = OP_ERR_NO_DEVICE;
            continue;
        }
        if (dd.idProduct != OP_PID) continue;

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
        pthread_mutex_init(&u->lock, NULL);
        pthread_cond_init(&u->arrived, NULL);

        out->ops  = &g_usb_ops;
        out->impl = u;
        libusb_free_device_list(list, 1);

        st = rx_start(u);
        if (st != OP_OK) {
            set_detail("could not post the cable's reads");
            usb_close(out);
            return st;
        }
        op_logf("usb open: intf %d, bulk in 0x%02x out 0x%02x, mps %u, %u reads posted",
                intf, ep_in, ep_out, (unsigned)mps, USB_RX_TRANSFERS);
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
 * Transport selection. OPENPORT_DEVICE names a character device (the cable's
 * CDC-ACM node, or a simulator's pty), or "usb" for this file's libusb path.
 * Unset, macOS uses the cable's serial node: the kernel's CDC-ACM driver keeps
 * reading the bulk IN pipe while this process is stalled by paging, which
 * freezes the libusb path's threads too, and a 300 ms freeze then lost the
 * rest of a readout (measured 2026-10-23, PROTOCOL.md section 1). A cable with
 * no serial node (storage mode, bootloader) goes to libusb, which names the
 * problem.
 */
op_status op_transport_open(op_transport *out)
{
    const char *dev = getenv("OPENPORT_DEVICE");
    char node[256];
    if (dev != NULL && strcmp(dev, "usb") == 0) return op_usb_open(out);
    if (dev != NULL && dev[0] != '\0') {
        op_logf("OPENPORT_DEVICE set: using serial transport %s", dev);
        return op_serial_open(out, dev);
    }
    if (op_serial_find(OP_VID, OP_PID, node, sizeof node) == OP_OK) return op_serial_open(out, node);
    return op_usb_open(out);
}
