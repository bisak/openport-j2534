/*
 * op_serial.c — character-device transport.
 *
 * Two uses, both real:
 *
 *  1. The OpenPort is a CDC-ACM device, so macOS and Linux expose it as a
 *     character device (/dev/cu.usbmodem* , /dev/ttyACM*). Talking to it that
 *     way needs no libusb and no interface claim at all. That is a genuine
 *     fallback if a future OS revision stops letting us claim the data
 *     interface.
 *
 *  2. Pointed at a pty, it lets the real library — and every application built
 *     on it, unmodified — talk to a simulated device. That is what makes the
 *     hardware-free test rig possible.
 *
 * Selected by setting OPENPORT_DEVICE to a device path. Unset, the USB
 * transport is used, so the default path for real hardware is untouched.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/* cfmakeraw and CRTSCTS are BSD/GNU extensions that strict POSIX hides. Rather
 * than widen the feature set, the raw flags are set explicitly below — that is
 * portable to both platforms with no extension at all. */
#include "op_transport.h"
#include "op_log.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

typedef struct { int fd; } op_serial;

static op_status from_errno(int e)
{
    switch (e) {
    case ENOENT:  return OP_ERR_NO_DEVICE;
    case EACCES:  return OP_ERR_ACCESS;
    case EBUSY:   return OP_ERR_ACCESS;
    case EAGAIN:  return OP_ERR_TIMEOUT;
    case EINVAL:  return OP_ERR_PARAM;
    default:      return OP_ERR_IO;
    }
}

/* Wait for readiness with a deadline, so no call can block indefinitely. */
static op_status wait_ready(int fd, short events, unsigned timeout_ms)
{
    struct pollfd p;
    int r;

    p.fd = fd;
    p.events = events;
    p.revents = 0;

    do {
        r = poll(&p, 1, (int)timeout_ms);
    } while (r < 0 && errno == EINTR);

    if (r < 0)  return from_errno(errno);
    if (r == 0) return OP_ERR_TIMEOUT;
    if (p.revents & (POLLERR | POLLNVAL)) return OP_ERR_IO;
    /* POLLHUP with data still buffered is normal on a pty whose peer closed;
     * let the read drain it and report end-of-stream on the next call. */
    if ((p.revents & POLLHUP) && !(p.revents & POLLIN)) return OP_ERR_NO_DEVICE;
    return OP_OK;
}

static op_status ser_write(op_transport *t, const uint8_t *buf, size_t len,
                           unsigned timeout_ms)
{
    op_serial *s;
    size_t sent = 0;

    if (t == NULL || t->impl == NULL || (buf == NULL && len > 0))
        return OP_ERR_PARAM;
    s = (op_serial *)t->impl;
    if (timeout_ms == 0) timeout_ms = 1;

    op_log_bytes("TX", buf, len);

    while (sent < len) {
        ssize_t n;
        op_status st = wait_ready(s->fd, POLLOUT, timeout_ms);
        if (st != OP_OK) return st;

        n = write(s->fd, buf + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            op_logf("serial write: %s", strerror(errno));
            return from_errno(errno);
        }
        if (n == 0) return OP_ERR_IO;
        sent += (size_t)n;
    }
    return OP_OK;
}

static op_status ser_read(op_transport *t, uint8_t *buf, size_t cap,
                          size_t *got, unsigned timeout_ms)
{
    op_serial *s;
    ssize_t n;
    op_status st;

    if (got != NULL) *got = 0;
    if (t == NULL || t->impl == NULL || buf == NULL || got == NULL || cap == 0)
        return OP_ERR_PARAM;
    s = (op_serial *)t->impl;

    st = wait_ready(s->fd, POLLIN, timeout_ms);
    if (st != OP_OK) return st;

    do {
        n = read(s->fd, buf, cap);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        if (errno == EAGAIN) return OP_ERR_TIMEOUT;
        op_logf("serial read: %s", strerror(errno));
        return from_errno(errno);
    }
    if (n == 0) return OP_ERR_NO_DEVICE;   /* peer closed */

    *got = (size_t)n;
    op_log_bytes("RX", buf, *got);
    return OP_OK;
}

static void ser_close(op_transport *t)
{
    op_serial *s;
    if (t == NULL || t->impl == NULL) return;
    s = (op_serial *)t->impl;
    if (s->fd >= 0) close(s->fd);
    free(s);
    t->impl = NULL;
    t->ops  = NULL;
}

static const op_transport_ops g_serial_ops = {
    "serial", ser_write, ser_read, ser_close
};

op_status op_serial_open(op_transport *out, const char *path)
{
    op_serial *s;
    int fd;
    struct termios tio;

    if (out == NULL || path == NULL) return OP_ERR_PARAM;
    out->ops = NULL;
    out->impl = NULL;

    fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        op_logf("serial open %s: %s", path, strerror(errno));
        return from_errno(errno);
    }

    /* Raw: no line discipline, no echo, no CR/LF translation, no flow control.
     * The protocol is binary in both directions and any translation corrupts
     * a frame's length byte or payload. */
    if (tcgetattr(fd, &tio) == 0) {
        /* Explicit raw mode: no input translation (a stray CR would corrupt a
         * frame's length byte), no canonical processing, no echo — a pty would
         * otherwise echo our commands straight back as if the device replied. */
        tio.c_iflag &= (tcflag_t)~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR |
                                   IGNCR  | ICRNL  | IXON   | IXOFF  | IXANY);
        tio.c_oflag &= (tcflag_t)~OPOST;
        tio.c_lflag &= (tcflag_t)~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
        tio.c_cflag &= (tcflag_t)~(PARENB | CSTOPB | CSIZE);
        tio.c_cflag |= (CLOCAL | CREAD | CS8);
#ifdef CRTSCTS
        tio.c_cflag &= (tcflag_t)~CRTSCTS;
#endif
        tio.c_cc[VMIN]  = 0;
        tio.c_cc[VTIME] = 0;
        if (tcsetattr(fd, TCSANOW, &tio) != 0)
            op_logf("serial tcsetattr %s: %s", path, strerror(errno));
        tcflush(fd, TCIOFLUSH);
    } else {
        /* A pty master has no termios in some configurations; that is fine. */
        op_logf("serial %s is not a tty (%s) — continuing raw", path,
                strerror(errno));
    }

    s = (op_serial *)calloc(1, sizeof *s);
    if (s == NULL) { close(fd); return OP_ERR_IO; }
    s->fd = fd;

    out->ops  = &g_serial_ops;
    out->impl = s;
    op_logf("serial open: %s", path);
    return OP_OK;
}
