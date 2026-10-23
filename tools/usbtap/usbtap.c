/*
 * usbtap — record every libusb bulk transfer a J2534 library performs.
 *
 * Loaded with DYLD_INSERT_LIBRARIES, it interposes libusb_bulk_transfer and,
 * for the reads a library keeps posted asynchronously, libusb_submit_transfer,
 * and writes a line-oriented trace to $USBTAP_OUT (default /tmp/usbtap.trace).
 * Both the old and the new library link the same libusb, so the same tap
 * observes both and the traces are directly comparable.
 *
 * The trace is the strongest equivalence evidence available: it is what the
 * cable actually sees, independent of how either library is written.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <libusb.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static FILE           *g_out;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct timeval  g_t0;

static void tap_open(void)
{
    const char *path;
    if (g_out != NULL) return;
    path = getenv("USBTAP_OUT");
    if (path == NULL) path = "/tmp/usbtap.trace";
    g_out = fopen(path, "w");
    if (g_out == NULL) g_out = stderr;
    setvbuf(g_out, NULL, _IOLBF, 0);
    gettimeofday(&g_t0, NULL);
}

/* Render a transfer as one line: direction, endpoint, result and payload.
 * Printable bytes are shown as text so command lines are readable at a glance,
 * with everything else hex-escaped. */
static void emit(const char *dir, unsigned char ep, int rc, int len,
                 const unsigned char *buf, int timeout)
{
    struct timeval now;
    double t;
    int i;

    gettimeofday(&now, NULL);
    t = (double)(now.tv_sec - g_t0.tv_sec) +
        (double)(now.tv_usec - g_t0.tv_usec) / 1e6;

    fprintf(g_out, "%8.4f %s ep=0x%02x rc=%d timeout=%d len=%d |", t, dir, ep,
            rc, timeout, len);
    for (i = 0; i < len; i++) {
        unsigned char c = buf[i];
        if (c == '\r')      fputs("\\r", g_out);
        else if (c == '\n') fputs("\\n", g_out);
        else if (c >= 0x20 && c < 0x7f) fputc(c, g_out);
        else fprintf(g_out, "\\x%02x", c);
    }
    fputs("|\n", g_out);
}

static int tap_bulk_transfer(struct libusb_device_handle *h,
                             unsigned char endpoint, unsigned char *data,
                             int length, int *transferred, unsigned int timeout)
{
    int local = 0;
    int rc;

    pthread_mutex_lock(&g_lock);
    tap_open();
    if ((endpoint & LIBUSB_ENDPOINT_IN) == 0)
        emit("OUT", endpoint, 0, length, data, (int)timeout);
    pthread_mutex_unlock(&g_lock);

    rc = libusb_bulk_transfer(h, endpoint, data, length,
                              transferred ? transferred : &local, timeout);

    if (endpoint & LIBUSB_ENDPOINT_IN) {
        int n = transferred ? *transferred : local;
        pthread_mutex_lock(&g_lock);
        tap_open();
        emit("IN ", endpoint, rc, n < 0 ? 0 : n, data, (int)timeout);
        pthread_mutex_unlock(&g_lock);
    }
    return rc;
}

/* An asynchronous bulk IN read is logged when it completes: its callback is
 * swapped for tap_read_done, which logs, puts the caller's back and calls it,
 * so a resubmission from that callback is wrapped again. */
#define TAP_READS 1024

static struct { struct libusb_transfer *x; libusb_transfer_cb_fn cb; } g_reads[TAP_READS];

static libusb_transfer_cb_fn caller_cb(struct libusb_transfer *x)
{
    int i;
    for (i = 0; i < TAP_READS; i++)
        if (g_reads[i].x == x) return g_reads[i].cb;
    return NULL;
}

static void LIBUSB_CALL tap_read_done(struct libusb_transfer *x)
{
    libusb_transfer_cb_fn cb;
    pthread_mutex_lock(&g_lock);
    tap_open();
    if (x->status == LIBUSB_TRANSFER_COMPLETED && x->actual_length > 0)
        emit("IN ", x->endpoint, 0, x->actual_length, x->buffer, (int)x->timeout);
    cb = caller_cb(x);
    pthread_mutex_unlock(&g_lock);
    x->callback = cb;
    if (cb != NULL) cb(x);
}

static int tap_submit_transfer(struct libusb_transfer *x)
{
    int i, free_at = -1;
    if (x->type == LIBUSB_TRANSFER_TYPE_BULK && (x->endpoint & LIBUSB_ENDPOINT_IN)
        && x->callback != tap_read_done) {
        pthread_mutex_lock(&g_lock);
        for (i = 0; i < TAP_READS && g_reads[i].x != x; i++)
            if (free_at < 0 && g_reads[i].x == NULL) free_at = i;
        if (i == TAP_READS) i = free_at;
        if (i >= 0) {
            g_reads[i].x = x;
            g_reads[i].cb = x->callback;
            x->callback = tap_read_done;
        }
        pthread_mutex_unlock(&g_lock);
    }
    return libusb_submit_transfer(x);
}

typedef struct { const void *replacement; const void *replacee; } interpose_t;

__attribute__((used))
static const interpose_t g_interposers[]
__attribute__((section("__DATA,__interpose"))) = {
    { (const void *)(uintptr_t)&tap_bulk_transfer,
      (const void *)(uintptr_t)&libusb_bulk_transfer },
    { (const void *)(uintptr_t)&tap_submit_transfer,
      (const void *)(uintptr_t)&libusb_submit_transfer },
};
