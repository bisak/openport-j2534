/*
 * op_proto.c — OpenPort wire-protocol codec. See op_proto.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "op_proto.h"

#include <stdio.h>
#include <string.h>

uint32_t op_rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

void op_wr_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static int is_digit(uint8_t c) { return c >= '0' && c <= '9'; }

/* Parse a run of decimal digits. Returns bytes consumed, 0 if none. Saturates
 * rather than wrapping, so a hostile length field cannot alias a small value. */
static size_t parse_u32(const char *s, size_t len, uint32_t *out)
{
    size_t i = 0;
    uint64_t v = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (uint64_t)(s[i] - '0');
        if (v > 0xFFFFFFFFu) v = 0xFFFFFFFFu;
        i++;
    }
    if (i == 0) return 0;
    *out = (uint32_t)v;
    return i;
}

static size_t skip_spaces(const char *s, size_t len)
{
    size_t i = 0;
    while (i < len && s[i] == ' ') i++;
    return i;
}

/* Decode an ASCII reply line whose body (after "ar") is [body, body+n). */
/* Collect the numeric tokens that follow a reply's fixed fields. */
static void parse_tail(const char *body, size_t n, size_t i, op_reply *out)
{
    out->ntail = 0;
    for (;;) {
        uint32_t v = 0;
        size_t got;
        i += skip_spaces(body + i, n - i);
        if (i >= n) return;
        got = parse_u32(body + i, n - i, &v);
        if (got == 0) return;
        i += got;
        if (out->ntail < 3) out->tail[out->ntail] = v;
        out->ntail++;
    }
}

static void parse_ascii(const char *body, size_t n, op_reply *out)
{
    char verb;
    size_t i;

    out->kind = OP_REPLY_JUNK;
    if (n == 0) return;

    verb = body[0];
    i = 1;

    switch (verb) {
    case 'o':                                   /* aro [<seq>] */
        out->kind = OP_REPLY_OK;
        parse_tail(body, n, i, out);
        return;

    case 'e': {                                 /* are <code> [<detail>] [<seq>] */
        uint32_t code = 0;
        i += skip_spaces(body + i, n - i);
        if (parse_u32(body + i, n - i, &code) == 0) return;
        i += parse_u32(body + i, n - i, &code);
        out->kind = OP_REPLY_ERROR;
        out->error_code = code;
        parse_tail(body, n, i, out);
        out->has_detail = out->ntail >= 1;
        out->detail = out->has_detail ? out->tail[0] : 0;
        return;
    }

    case 'i':                                   /* ari <text> */
        i += skip_spaces(body + i, n - i);
        out->kind = OP_REPLY_INFO;
        out->text = body + i;
        out->text_len = n - i;
        return;

    case 'r': {                                 /* arr <pin> <millivolts> */
        uint32_t pin = 0, mv = 0;
        size_t got;
        i += skip_spaces(body + i, n - i);
        got = parse_u32(body + i, n - i, &pin);
        if (got == 0) return;
        i += got;
        i += skip_spaces(body + i, n - i);
        got = parse_u32(body + i, n - i, &mv);
        if (got == 0) return;
        i += got;
        out->kind = OP_REPLY_PIN;
        out->a = pin;
        out->b = mv;
        parse_tail(body, n, i, out);
        return;
    }

    case 'm': {                                 /* arm<ch> <msg_id> [<seq>] */
        uint32_t ch = 0, id = 0;
        size_t got;
        got = parse_u32(body + i, n - i, &ch);
        if (got == 0) return;
        i += got;
        i += skip_spaces(body + i, n - i);
        got = parse_u32(body + i, n - i, &id);
        if (got == 0) return;
        i += got;
        out->kind = OP_REPLY_PERIODIC;
        out->channel = ch;
        out->a = id;
        parse_tail(body, n, i, out);
        return;
    }

    case 'f': {                                 /* arf<ch> <filter_id> <detail> */
        uint32_t ch = 0, id = 0;
        size_t got;
        got = parse_u32(body + i, n - i, &ch);
        if (got == 0) return;
        i += got;
        i += skip_spaces(body + i, n - i);
        got = parse_u32(body + i, n - i, &id);
        if (got == 0) return;
        i += got;
        out->kind = OP_REPLY_FILTER;
        out->channel = ch;
        out->a = id;
        parse_tail(body, n, i, out);
        return;
    }

    case 'g': {                                 /* arg<ch> <param> <value> ... */
        uint32_t ch = 0, param = 0, value = 0;
        size_t got;
        got = parse_u32(body + i, n - i, &ch);
        if (got == 0) return;
        i += got;
        i += skip_spaces(body + i, n - i);
        got = parse_u32(body + i, n - i, &param);
        if (got == 0) return;
        i += got;
        i += skip_spaces(body + i, n - i);
        got = parse_u32(body + i, n - i, &value);
        if (got == 0) return;
        i += got;
        out->kind = OP_REPLY_CONFIG;
        out->channel = ch;
        out->a = param;
        out->b = value;
        parse_tail(body, n, i, out);
        return;
    }

    case 'y': {                                 /* ary<ch> <len>, then <len> raw bytes */
        uint32_t ch = 0, count = 0;
        size_t got;
        got = parse_u32(body + i, n - i, &ch);
        if (got == 0) return;
        i += got;
        i += skip_spaces(body + i, n - i);
        if (parse_u32(body + i, n - i, &count) == 0) return;
        if (count > 255) return;
        out->kind = OP_REPLY_INIT;
        out->channel = ch;
        out->a = count;
        return;
    }

    case 'w': {                                 /* arw<ch> <b> <b> ... [<seq>] */
        uint32_t ch = 0, v = 0;
        size_t got;
        got = parse_u32(body + i, n - i, &ch);
        if (got == 0) return;
        i += got;
        out->init_ntok = 0;
        for (;;) {
            i += skip_spaces(body + i, n - i);
            got = parse_u32(body + i, n - i, &v);
            if (got == 0) break;
            i += got;
            if (out->init_ntok < sizeof out->init_tok / sizeof out->init_tok[0])
                out->init_tok[out->init_ntok++] = v;
        }
        out->kind = OP_REPLY_INIT;
        out->channel = ch;
        return;
    }

    default:
        return;                                 /* leave as OP_REPLY_JUNK */
    }
}

/*
 * Whether a frame's body begins with the 4-byte timestamp.
 *
 * CAN and ISO15765 frames always carry one (measured, PROTOCOL.md §7). K-line
 * frames do not carry one on a plain data frame: every independent reading of
 * the firmware — three drivers, one of them written against a disassembly of
 * Tactrix's own DLL — puts the timestamp only on the START, END and transmit
 * indication frames, whose body is then the timestamp and nothing else. A
 * uniform rule here would have eaten the first four bytes of every K-line
 * message. Unconfirmed on this project's own hardware until a K-line vehicle
 * capture exists; `tools/car/car_capture.py --kline` records the raw bytes.
 */
int op_frame_has_timestamp(unsigned channel, uint8_t status, size_t body_len)
{
    if (channel != 3 && channel != 4) return body_len >= 4;
    if ((status & (OP_STS_START | OP_STS_END | OP_STS_TX_IND)) == 0) return 0;
    return body_len >= 4;
}

size_t op_parse(const uint8_t *buf, size_t len, op_reply *out)
{
    memset(out, 0, sizeof *out);
    out->kind = OP_REPLY_INCOMPLETE;

    if (buf == NULL || len == 0) return 0;

    /* Every reply begins "ar". Anything else is stream noise; report it as a
     * one-byte junk consumption so the caller can resynchronise deliberately. */
    if (len >= 1 && buf[0] != 'a') { out->kind = OP_REPLY_JUNK; return 1; }
    if (len < 2) return 0;
    if (buf[1] != 'r') { out->kind = OP_REPLY_JUNK; return 1; }
    if (len < 3) return 0;

    /* Binary message frame: digit channel followed by a control-range length. */
    if (is_digit(buf[2])) {
        size_t frame_len, need;
        uint8_t status;

        if (len < 4) return 0;

        /* The length field is a full byte: frames run to 4 + 255. The channel
         * digit is what separates a frame from an ASCII reply — those always
         * carry a letter here (aro/are/ari/arr/arf/arg) — so no constraint on
         * the length byte is needed, and imposing one would silently truncate
         * every frame longer than 31 bytes. */
        frame_len = buf[3];
        need = 4 + frame_len;
        if (len < need) return 0;               /* wait for the whole frame */

        if (frame_len < 1) { out->kind = OP_REPLY_JUNK; return need; }

        status = buf[4];
        out->kind    = OP_REPLY_FRAME;
        out->channel = (unsigned)(buf[2] - '0');
        out->status  = status;

        if (op_frame_has_timestamp(out->channel, status, frame_len - 1)) {
            out->timestamp_us = op_rd_be32(buf + 5);
            out->data         = buf + 9;
            out->data_len     = frame_len - 5;
        } else {
            out->timestamp_us = 0;
            out->data         = buf + 5;
            out->data_len     = frame_len - 1;
        }
        return need;
    }

    /* ASCII reply: terminated by CR LF. */
    {
        size_t i;
        for (i = 2; i + 1 < len; i++) {
            if (buf[i] == '\r' && buf[i + 1] == '\n') {
                parse_ascii((const char *)buf + 2, i - 2, out);
                if (out->kind == OP_REPLY_INIT && out->init_ntok == 0
                    && buf[2] == 'y') {
                    /* `ary` announces how many raw response bytes follow the
                     * line. They are part of this reply, not the start of the
                     * next one; wait until all of them are in. `arw` carries
                     * its bytes on the line and needs nothing further. */
                    size_t want = (size_t)out->a;
                    if (len < i + 2 + want) {
                        memset(out, 0, sizeof *out);
                        out->kind = OP_REPLY_INCOMPLETE;
                        return 0;
                    }
                    out->data = buf + i + 2;
                    out->data_len = want;
                    return i + 2 + want;
                }
                return i + 2;
            }
        }
        /* No terminator yet. Guard against a peer that never sends one. */
        if (len > OP_CMD_MAX * 4) { out->kind = OP_REPLY_JUNK; return 1; }
        return 0;
    }
}

size_t op_resync_offset(const uint8_t *buf, size_t len)
{
    size_t i;
    if (buf == NULL || len < 3) return len;
    for (i = 1; i + 2 < len; i++) {
        if (buf[i] == 'a' && buf[i + 1] == 'r') {
            uint8_t c = buf[i + 2];
            if (is_digit(c) || c == 'o' || c == 'e' || c == 'i' ||
                c == 'r' || c == 'f' || c == 'g' || c == 'y' || c == 'w' || c == 'm')
                return i;
        }
    }
    return len;
}

/* ---- command encoders -------------------------------------------------- */

/* snprintf returns the length it *would* have written; treat truncation and
 * encoding errors alike as failure so no caller ever transmits a partial line. */
static size_t emit(char *out, size_t out_sz, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#include <stdarg.h>
static size_t emit(char *out, size_t out_sz, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (out == NULL || out_sz == 0) return 0;
    va_start(ap, fmt);
    n = vsnprintf(out, out_sz, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= out_sz) { out[0] = '\0'; return 0; }
    return (size_t)n;
}

int op_cmd_is_numbered(const char *line)
{
    if (line == NULL || line[0] != 'a' || line[1] != 't' || line[2] == '\0') return 0;
    /* `ati` alone ignores a number (measured: `ati 5` answers a bare `ari`). */
    return line[2] != 'i';
}

size_t op_cmd_number(char *line, size_t out_sz, size_t len, uint32_t seq)
{
    char tail[16];
    int n;
    if (line == NULL || len < 4 || line[len - 2] != '\r' || line[len - 1] != '\n') return 0;
    n = snprintf(tail, sizeof tail, " %lu", (unsigned long)seq);
    if (n <= 0 || len - 2 + (size_t)n + 3 > out_sz) return 0;
    memcpy(line + len - 2, tail, (size_t)n);
    memcpy(line + len - 2 + (size_t)n, "\r\n", 3);
    return len + (size_t)n;
}

size_t op_cmd_close_all(char *out, size_t out_sz) { return emit(out, out_sz, "ata\r\n"); }
size_t op_cmd_reset(char *out, size_t out_sz)     { return emit(out, out_sz, "atz\r\n"); }
size_t op_cmd_version(char *out, size_t out_sz)   { return emit(out, out_sz, "ati\r\n"); }

size_t op_cmd_open(char *out, size_t out_sz, unsigned proto, uint32_t flags, uint32_t baud)
{
    /* The fourth argument is 0. The vendor DLL sends a varying value there
     * for ISO9141 (3 on the bench, 5 on a car, each time the handle of the
     * ISO14230 channel it had just closed), which is its own bookkeeping,
     * not a protocol constant; 0 is what this driver has always sent and the
     * firmware accepts (PROTOCOL.md section 4). */
    return emit(out, out_sz, "ato%u %lu %lu 0\r\n",
                proto, (unsigned long)flags, (unsigned long)baud);
}

size_t op_cmd_close(char *out, size_t out_sz, unsigned ch)
{
    return emit(out, out_sz, "atc%u\r\n", ch);
}

size_t op_cmd_read_pin(char *out, size_t out_sz, unsigned pin)
{
    /* Note the space: the device parses this one as "atr <pin>", unlike the
     * channel-suffixed commands. */
    return emit(out, out_sz, "atr %u\r\n", pin);
}

size_t op_cmd_get_config(char *out, size_t out_sz, unsigned ch, uint32_t param)
{
    return emit(out, out_sz, "atg%u %lu\r\n", ch, (unsigned long)param);
}

size_t op_cmd_set_config(char *out, size_t out_sz, unsigned ch,
                         uint32_t param, uint32_t value)
{
    return emit(out, out_sz, "ats%u %lu %lu\r\n", ch,
                (unsigned long)param, (unsigned long)value);
}

size_t op_cmd_transmit(char *out, size_t out_sz, unsigned ch,
                       size_t payload_len, uint32_t txflags, uint32_t timeout_us)
{
    if (payload_len == 0 || payload_len > OP_MSG_MAX) return 0;
    return emit(out, out_sz, "att%u %zu %lu %lu\r\n", ch, payload_len,
                (unsigned long)txflags, (unsigned long)timeout_us);
}

size_t op_cmd_filter(char *out, size_t out_sz, unsigned ch, uint32_t type,
                     uint32_t txflags, size_t each_len)
{
    /* each_len is the length of ONE of the appended messages, not their total;
     * the device rejects a total with ERR_INVALID_MSG. */
    if (each_len == 0 || each_len > 255) return 0;
    return emit(out, out_sz, "atf%u %lu %lu %zu\r\n", ch,
                (unsigned long)type, (unsigned long)txflags, each_len);
}

size_t op_cmd_stop_filter(char *out, size_t out_sz, unsigned ch, uint32_t filter_id)
{
    return emit(out, out_sz, "atk%u %lu\r\n", ch, (unsigned long)filter_id);
}

size_t op_cmd_fast_init(char *out, size_t out_sz, unsigned ch, size_t payload_len)
{
    /* A fast init with no request bytes is legal: J2534 defines a NULL input
     * as "no message transmitted", and the device accepts `aty<ch> 0 0`. */
    if (payload_len > 255) return 0;
    return emit(out, out_sz, "aty%u %zu 0\r\n", ch, payload_len);
}

size_t op_cmd_five_baud(char *out, size_t out_sz, unsigned ch, unsigned address)
{
    if (address > 255) return 0;
    return emit(out, out_sz, "atw%u %u\r\n", ch, address);
}

unsigned op_filter_msg_count(uint32_t filter_type)
{
    switch (filter_type) {
    case 1: /* PASS  */
    case 2: /* BLOCK */         return 2;
    case 3: /* FLOW_CONTROL */  return 3;
    default:                    return 0;
    }
}

int op_parse_version(const char *text, size_t text_len, char *out, size_t out_sz)
{
    /* The reply is "main code version : 1.17.4877". Split on the last ": "
     * rather than trusting a fixed offset, so a firmware that changes the
     * prefix does not silently yield garbage. */
    size_t i, start = 0;
    int found = 0;

    if (out == NULL || out_sz == 0) return 0;
    out[0] = '\0';
    if (text == NULL) return 0;

    for (i = 0; i + 1 < text_len; i++) {
        if (text[i] == ':' && text[i + 1] == ' ') { start = i + 2; found = 1; }
    }
    if (!found) return 0;

    while (start < text_len && text[start] == ' ') start++;
    {
        size_t n = text_len - start;
        while (n > 0 && (text[start + n - 1] == '\r' || text[start + n - 1] == '\n' ||
                         text[start + n - 1] == ' '))
            n--;
        if (n >= out_sz) n = out_sz - 1;
        memcpy(out, text + start, n);
        out[n] = '\0';
        return n > 0;
    }
}
