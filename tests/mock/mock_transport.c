/*
 * mock_transport.c — scripted in-memory transport. See mock_transport.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "mock_transport.h"
#include "op_proto.h"

#include <pthread.h>
#include <sys/time.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MOCK_CAP 65536

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv   = PTHREAD_COND_INITIALIZER;

static uint8_t  g_rx[MOCK_CAP];
static size_t   g_rx_len;
static uint8_t  g_tx[MOCK_CAP];
static size_t   g_tx_len;

/* Partial command accumulation, so the responder sees whole lines. */
static uint8_t  g_pending[MOCK_CAP];
static size_t   g_pending_len;
/* When a command declares a binary payload, how many bytes still to gather. */
static size_t   g_await_payload;
static uint8_t  g_line[512];
static size_t   g_line_len;

static mock_responder g_responder;

/* The sequence number the last command carried, as the device would echo it
 * (PROTOCOL.md section 3): the last decimal token of a numbered verb's line. */
static char g_last_seq[16];

static void note_seq(const char *line, size_t len)
{
    size_t end = len, start;
    g_last_seq[0] = '\0';
    if (!op_cmd_is_numbered(line)) return;
    while (end > 0 && (line[end - 1] == '\r' || line[end - 1] == '\n')) end--;
    start = end;
    while (start > 0 && line[start - 1] >= '0' && line[start - 1] <= '9') start--;
    if (start == end || start == 0 || line[start - 1] != ' ') return;
    if (end - start >= sizeof g_last_seq) return;
    memcpy(g_last_seq, line + start, end - start);
    g_last_seq[end - start] = '\0';
}

void mock_push_reply(const char *text)
{
    char out[256];
    snprintf(out, sizeof out, "%s%s%s\r\n", text, g_last_seq[0] ? " " : "", g_last_seq);
    mock_push_str(out);
}

static unsigned  g_tx_delay_ms;
static int       g_fail_writes;
static op_status g_fail_write_st;
static int       g_fail_reads;
static op_status g_fail_read_st;

void mock_reset(void)
{
    pthread_mutex_lock(&g_lock);
    g_rx_len = g_tx_len = g_pending_len = g_line_len = 0;
    g_await_payload = 0;
    g_responder = NULL;
    g_tx_delay_ms = 0;
    g_fail_writes = g_fail_reads = 0;
    pthread_mutex_unlock(&g_lock);
}

void mock_set_responder(mock_responder fn)
{
    pthread_mutex_lock(&g_lock);
    g_responder = fn;
    pthread_mutex_unlock(&g_lock);
}

static void push_locked(const void *bytes, size_t len)
{
    if (len > MOCK_CAP - g_rx_len) len = MOCK_CAP - g_rx_len;
    memcpy(g_rx + g_rx_len, bytes, len);
    g_rx_len += len;
    pthread_cond_broadcast(&g_cv);
}

void mock_push(const void *bytes, size_t len)
{
    pthread_mutex_lock(&g_lock);
    push_locked(bytes, len);
    pthread_mutex_unlock(&g_lock);
}

void mock_push_str(const char *s) { mock_push(s, strlen(s)); }

/* The driver writes from its own threads, so reading the capture needs the
 * lock too — otherwise the test harness itself races the code under test. */
const uint8_t *mock_tx(size_t *len)
{
    pthread_mutex_lock(&g_lock);
    if (len != NULL) *len = g_tx_len;
    pthread_mutex_unlock(&g_lock);
    return g_tx;
}

void mock_clear_tx(void)
{
    pthread_mutex_lock(&g_lock);
    g_tx_len = 0;
    pthread_mutex_unlock(&g_lock);
}

void mock_fail_writes(int n, op_status st)
{
    pthread_mutex_lock(&g_lock);
    g_fail_writes = n; g_fail_write_st = st;
    pthread_mutex_unlock(&g_lock);
}

void mock_delay_transmits(unsigned ms)
{
    pthread_mutex_lock(&g_lock);
    g_tx_delay_ms = ms;
    pthread_mutex_unlock(&g_lock);
}

void mock_fail_reads(int n, op_status st)
{
    pthread_mutex_lock(&g_lock);
    g_fail_reads = n; g_fail_read_st = st;
    pthread_mutex_unlock(&g_lock);
}

/* Decide whether a command line announces a trailing binary payload, and how
 * long it is. Mirrors the device: att/atf/atp/aty/atw carry appended bytes. */
static size_t declared_payload(const char *line, size_t len)
{
    char buf[128];
    unsigned a = 0, b = 0, c = 0, n = 0;
    size_t copy = len < sizeof buf - 1 ? len : sizeof buf - 1;

    memcpy(buf, line, copy);
    buf[copy] = '\0';

    if (copy > 3 && buf[0] == 'a' && buf[1] == 't' && buf[2] == 't') {
        if (sscanf(buf + 3, "%u %u %u", &a, &b, &c) >= 2) return b;
    }
    if (copy > 3 && buf[0] == 'a' && buf[1] == 't' && buf[2] == 'y') {
        /* aty<ch> <len> 0: <len> StartCommunication request bytes follow. */
        if (sscanf(buf + 3, "%u %u %u", &a, &b, &c) == 3) return b;
    }
    if (copy > 3 && buf[0] == 'a' && buf[1] == 't' && buf[2] == 'f') {
        unsigned type = 0, each = 0;
        if (sscanf(buf + 3, "%u %u %u %u", &a, &type, &b, &each) == 4) {
            n = op_filter_msg_count(type);
            return (size_t)each * n;
        }
    }
    return 0;
}

static void feed_locked(const uint8_t *buf, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        uint8_t ch = buf[i];

        if (g_await_payload > 0) {
            if (g_pending_len < sizeof g_pending) g_pending[g_pending_len++] = ch;
            if (--g_await_payload == 0) {
                mock_responder fn = g_responder;
                if (fn != NULL) {
                    note_seq((const char *)g_line, g_line_len);
                    pthread_mutex_unlock(&g_lock);
                    fn((const char *)g_line, g_line_len, g_pending, g_pending_len);
                    pthread_mutex_lock(&g_lock);
                }
                g_line_len = g_pending_len = 0;
            }
            continue;
        }

        if (ch == '\n') {
            size_t want;
            /* strip a trailing CR */
            if (g_line_len > 0 && g_line[g_line_len - 1] == '\r') g_line_len--;
            want = declared_payload((const char *)g_line, g_line_len);
            if (want > 0) {
                g_await_payload = want;
                g_pending_len = 0;
            } else {
                mock_responder fn = g_responder;
                if (fn != NULL) {
                    note_seq((const char *)g_line, g_line_len);
                    pthread_mutex_unlock(&g_lock);
                    fn((const char *)g_line, g_line_len, NULL, 0);
                    pthread_mutex_lock(&g_lock);
                }
                g_line_len = 0;
            }
            continue;
        }
        if (g_line_len < sizeof g_line) g_line[g_line_len++] = ch;
    }
}

static op_status mock_write(op_transport *t, const uint8_t *buf, size_t len,
                            unsigned timeout_ms)
{
    (void)t; (void)timeout_ms;
    pthread_mutex_lock(&g_lock);
    if (g_fail_writes > 0) {
        op_status st = g_fail_write_st;
        g_fail_writes--;
        pthread_mutex_unlock(&g_lock);
        return st;
    }
    if (len <= MOCK_CAP - g_tx_len) {
        memcpy(g_tx + g_tx_len, buf, len);
        g_tx_len += len;
    }
    feed_locked(buf, len);
    pthread_mutex_unlock(&g_lock);
    return OP_OK;
}

static op_status mock_read(op_transport *t, uint8_t *buf, size_t cap,
                           size_t *got, unsigned timeout_ms)
{
    struct timespec ts;
    op_status st;

    (void)t;
    *got = 0;
    pthread_mutex_lock(&g_lock);

    if (g_fail_reads > 0) {
        st = g_fail_read_st;
        g_fail_reads--;
        pthread_mutex_unlock(&g_lock);
        return st;
    }

    if (g_rx_len == 0 && timeout_ms > 0) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        ts.tv_sec  = tv.tv_sec + (time_t)(timeout_ms / 1000u);
        ts.tv_nsec = (long)tv.tv_usec * 1000L + (long)(timeout_ms % 1000u) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        (void)pthread_cond_timedwait(&g_cv, &g_lock, &ts);
    }

    if (g_rx_len == 0) {
        pthread_mutex_unlock(&g_lock);
        return OP_ERR_TIMEOUT;
    }

    *got = g_rx_len < cap ? g_rx_len : cap;
    memcpy(buf, g_rx, *got);
    memmove(g_rx, g_rx + *got, g_rx_len - *got);
    g_rx_len -= *got;
    pthread_mutex_unlock(&g_lock);
    return OP_OK;
}

static void mock_close_(op_transport *t) { (void)t; }

static const op_transport_ops g_ops = { "mock", mock_write, mock_read, mock_close_ };

op_status mock_open(op_transport *out)
{
    if (out == NULL) return OP_ERR_PARAM;
    out->ops  = &g_ops;
    out->impl = (void *)1;      /* non-NULL: the layers above only test it */
    return OP_OK;
}

/* ---- a stand-in OpenPort ------------------------------------------------ */

static void openport_responder(const char *line, size_t len,
                               const uint8_t *payload, size_t payload_len)
{
    char buf[128];
    size_t copy = len < sizeof buf - 1 ? len : sizeof buf - 1;
    unsigned ch = 0, a = 0, b = 0;

    (void)payload; (void)payload_len;
    memcpy(buf, line, copy);
    buf[copy] = '\0';

    if (strncmp(buf, "ata", 3) == 0 || strncmp(buf, "atz", 3) == 0) {
        mock_push_reply("aro");
    } else if (strncmp(buf, "ati", 3) == 0) {
        mock_push_str("ari main code version : 1.17.4877\r\n");
    } else if (strncmp(buf, "atw", 3) == 0) {
        /* Five-baud init (`atw<ch> <address>`, no payload): answers with the
         * two keybytes as `ary<ch> <n>` + raw bytes. */
        char hdr[32];
        if (sscanf(buf + 3, "%u %u", &ch, &a) == 2) {
            snprintf(hdr, sizeof hdr, "ary%u 2\r\n", ch);
            mock_push_str(hdr);
            mock_push("\x55\x08", 2);
        }
    } else if (strncmp(buf, "aty", 3) == 0) {
        /* Fast init (`aty<ch> <n> 0` + request): answers with a three-byte
         * StartCommunication response, same `ary<ch> <n>` + raw bytes form. */
        char hdr[32];
        if (sscanf(buf + 3, "%u", &ch) == 1) {
            snprintf(hdr, sizeof hdr, "ary%u 3\r\n", ch);
            mock_push_str(hdr);
            mock_push("\xc1\xef\x8f", 3);
        }
    } else if (strncmp(buf, "ato", 3) == 0) {
        mock_push_reply("aro");
    } else if (strncmp(buf, "atc", 3) == 0) {
        mock_push_reply("aro");
    } else if (strncmp(buf, "atr", 3) == 0) {
        if (sscanf(buf + 3, " %u", &a) == 1 && a == 16) mock_push_reply("arr 16 12480");
        else if (a == 12) mock_push_reply("arr 12 0");
        else mock_push_reply("are 19");
    } else if (strncmp(buf, "atf", 3) == 0) {
        char r[32];
        if (sscanf(buf + 3, "%u", &ch) == 1) {
            snprintf(r, sizeof r, "arf%u 0", ch);
            mock_push_reply(r);
        }
    } else if (strncmp(buf, "atk", 3) == 0) {
        mock_push_reply("aro");
    } else if (strncmp(buf, "atg", 3) == 0) {
        char r[64];
        if (sscanf(buf + 3, "%u %u", &ch, &a) == 2) {
            b = (a == 1) ? 500000u : 0u;
            snprintf(r, sizeof r, "arg%u %u %u", ch, a, b);
            mock_push_reply(r);
        }
    } else if (strncmp(buf, "ats", 3) == 0) {
        mock_push_reply("aro");
    } else if (strncmp(buf, "att", 3) == 0) {
        unsigned d;
        pthread_mutex_lock(&g_lock);
        d = g_tx_delay_ms;
        pthread_mutex_unlock(&g_lock);
        if (d) usleep(d * 1000u);
        mock_push_reply("aro");
    } else if (strncmp(buf, "atv", 3) == 0) {
        mock_push_reply("aro");
    } else {
        mock_push_reply("are 7");
    }
}

void mock_install_openport_responder(void) { mock_set_responder(openport_responder); }
