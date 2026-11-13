/*
 * op_transport.h — byte-pipe abstraction to a device.
 *
 * The J2534 and protocol layers only ever see read/write/close, so a second
 * device (OpenPort 3.0) or a recorded-trace backend for testing can be added
 * without touching anything above this line.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef OP_TRANSPORT_H
#define OP_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    OP_OK = 0,
    OP_ERR_TIMEOUT,       /* no data within the caller's deadline */
    OP_ERR_IO,            /* transfer failed */
    OP_ERR_NO_DEVICE,     /* nothing matching is attached */
    OP_ERR_ACCESS,        /* found it, could not claim it */
    OP_ERR_MASS_STORAGE,  /* enumerated as USB storage: microSD is inserted */
    OP_ERR_PARAM,         /* bad argument from our own caller */
    OP_ERR_OVERFLOW,      /* would not fit in the caller's buffer */
    OP_ERR_PROTOCOL,      /* device is present but will not talk sense */
    OP_ERR_STALLED        /* device takes no bytes: a write timed out with nothing sent */
} op_status;

typedef struct op_transport op_transport;

typedef struct {
    op_status (*write)(op_transport *t, const uint8_t *buf, size_t len,
                       unsigned timeout_ms);
    /* Reads at most cap bytes. On OP_OK *got is > 0. OP_ERR_TIMEOUT means no
     * bytes arrived before the deadline, which is not an error to the caller
     * of a polling read. */
    op_status (*read)(op_transport *t, uint8_t *buf, size_t cap, size_t *got,
                      unsigned timeout_ms);
    void      (*close)(op_transport *t);
} op_transport_ops;

struct op_transport {
    const op_transport_ops *ops;
    void                   *impl;
};

/* A command goes to the cable whole or not at all: a part of one leaves the
 * cable reading the next command as the rest of its payload. The cable can
 * hold a long payload back while it works, waiting for an ISO 15765 flow
 * control for as long as the budget the command itself gives it, so a write
 * that has started is carried on this long past the caller's timeout. Only a
 * write that has sent nothing gives up at the timeout. Measured 2026-11-12: a
 * 4099-byte ISO 15765 transmit with no ECU answering, cut at 100 ms, made the
 * next command time out. */
#define OP_WRITE_GRACE_MS 2000u

/* Open the first attached OpenPort over USB bulk endpoints. Not on Windows,
 * where the build has no libusb. */
op_status op_usb_open(op_transport *out);

/* Open a character device (the cable's CDC-ACM node, or a pty for testing).
 * On Windows, a COM port by name ("COM7") or a device path. */
op_status op_serial_open(op_transport *out, const char *path);

/* The serial device node of the attached USB device vid:pid, into path.
 * macOS, and Windows (the cable's COM port); elsewhere, and when there is
 * none, OP_ERR_NO_DEVICE. */
op_status op_serial_find(uint16_t vid, uint16_t pid, char *path, size_t cap);

/* Whichever the environment selects: OPENPORT_DEVICE names a character
 * device, or "usb" for libusb. Otherwise, on macOS the cable's own serial
 * node when it has one, on Windows Tactrix's device interface or else the
 * cable's COM port (op_win.c), and libusb everywhere else. */
op_status op_transport_open(op_transport *out);

/* Human-readable detail about the most recent open failure on this thread. */
const char *op_transport_last_detail(void);

static inline op_status op_write(op_transport *t, const uint8_t *b, size_t n,
                                 unsigned to)
{
    if (t == NULL || t->ops == NULL) return OP_ERR_PARAM;
    return t->ops->write(t, b, n, to);
}
static inline op_status op_read(op_transport *t, uint8_t *b, size_t cap,
                                size_t *got, unsigned to)
{
    if (t == NULL || t->ops == NULL) return OP_ERR_PARAM;
    return t->ops->read(t, b, cap, got, to);
}
static inline void op_close(op_transport *t)
{
    if (t != NULL && t->ops != NULL && t->ops->close != NULL) t->ops->close(t);
}

#endif /* OP_TRANSPORT_H */
