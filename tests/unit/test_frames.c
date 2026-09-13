/*
 * Binary message-frame parsing: layout, bounds, truncation, resync.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test.h"
#include "op_proto.h"

/* The frame captured from a real cable during protocol work:
 *   'a' 'r' '6' len=0x09 status=0x10 ts=0x1950B80C data=00 00 07 E0        */
static const uint8_t k_real_frame[] = {
    0x61, 0x72, 0x36, 0x09, 0x10, 0x19, 0x50, 0xB8, 0x0C,
    0x00, 0x00, 0x07, 0xE0
};

static void layout(void)
{
    op_reply r;
    size_t n = op_parse(k_real_frame, sizeof k_real_frame, &r);

    CHECK_EQ(n, sizeof k_real_frame, "frame consumed whole");
    CHECK_EQ(r.kind, OP_REPLY_FRAME, "frame kind");
    CHECK_EQ(r.channel, 6, "channel from the ASCII digit");
    CHECK_EQ(r.status, 0x10, "status byte");
    CHECK_EQ(r.timestamp_us, 0x1950B80Cu, "microsecond timestamp");
    CHECK_EQ(r.data_len, 4, "payload length = len - status - timestamp");
    CHECK(r.data != NULL && r.data[3] == 0xE0, "payload is the CAN id 0x7E0");
}

static void status_bits(void)
{
    /* The status byte is a bitfield. Verify each combination decodes to the
     * same byte we will later interpret, and that the bit constants are the
     * ones the wire uses. */
    CHECK_EQ(OP_STS_START,    0x80, "START bit");
    CHECK_EQ(OP_STS_END,      0x40, "END bit");
    CHECK_EQ(OP_STS_LOOPBACK, 0x20, "LOOPBACK bit");

    CHECK_EQ(0xA0 & OP_STS_START,    OP_STS_START,    "0xA0 is a start");
    CHECK_EQ(0xA0 & OP_STS_LOOPBACK, OP_STS_LOOPBACK, "0xA0 is a loopback");
    CHECK_EQ(0x60 & OP_STS_END,      OP_STS_END,      "0x60 is an end");
    CHECK_EQ(0x60 & OP_STS_LOOPBACK, OP_STS_LOOPBACK, "0x60 is a loopback");
    CHECK_EQ(0x00 & (OP_STS_START | OP_STS_END | OP_STS_LOOPBACK), 0,
             "0x00 is a plain data frame");
}

static void truncation(void)
{
    op_reply r;
    size_t i;

    /* Every prefix must report "need more", never a short read past the end.
     * This is the case the old driver's data[len] arithmetic could not see. */
    for (i = 1; i < sizeof k_real_frame; i++) {
        size_t n = op_parse(k_real_frame, i, &r);
        CHECK_EQ(n, 0, "truncated frame must not be consumed");
        CHECK_EQ(r.kind, OP_REPLY_INCOMPLETE, "truncated frame kind");
    }
}

static void bounds(void)
{
    op_reply r;

    /* A CAN frame shorter than status+timestamp cannot carry a timestamp;
     * the parser must not read past the body for one. */
    {
        const uint8_t f[] = { 'a', 'r', '6', 0x03, 0x00, 0x11, 0x22 };
        size_t n = op_parse(f, sizeof f, &r);
        CHECK_EQ(n, 7, "undersized frame consumed");
        CHECK_EQ(r.kind, OP_REPLY_FRAME, "undersized frame is still a frame");
        CHECK_EQ(r.timestamp_us, 0, "no timestamp invented");
        CHECK_EQ(r.data_len, 2, "body delivered as data");
    }
    /* A frame with no body at all: status byte only. */
    {
        const uint8_t f[] = { 'a', 'r', '6', 0x01, 0x40 };
        size_t n = op_parse(f, sizeof f, &r);
        CHECK_EQ(n, 5, "status-only frame consumed");
        CHECK_EQ(r.kind, OP_REPLY_FRAME, "status-only frame accepted");
        CHECK_EQ(r.data_len, 0, "status-only frame has no payload");
    }
    /* A zero-length frame is malformed. */
    {
        const uint8_t f[] = { 'a', 'r', '6', 0x00 };
        size_t n = op_parse(f, sizeof f, &r);
        CHECK_EQ(n, 4, "empty frame consumed");
        CHECK_EQ(r.kind, OP_REPLY_JUNK, "empty frame rejected");
    }
    /* Exactly the minimum: status + timestamp, no payload. */
    {
        const uint8_t f[] = { 'a', 'r', '6', 0x05, 0x40, 0, 0, 0, 1 };
        size_t n = op_parse(f, sizeof f, &r);
        CHECK_EQ(n, 9, "minimum frame consumed");
        CHECK_EQ(r.kind, OP_REPLY_FRAME, "minimum frame accepted");
        CHECK_EQ(r.data_len, 0, "minimum frame has no payload");
        CHECK_EQ(r.timestamp_us, 1, "minimum frame timestamp");
    }
    /* The largest a one-byte length can describe. */
    {
        uint8_t f[4 + 255];
        op_reply q;
        size_t n;
        memset(f, 0xAB, sizeof f);
        f[0] = 'a'; f[1] = 'r'; f[2] = '3'; f[3] = 255; f[4] = 0x80;
        n = op_parse(f, sizeof f, &q);
        CHECK_EQ(n, sizeof f, "maximum frame consumed");
        CHECK_EQ(q.kind, OP_REPLY_FRAME, "maximum frame accepted");
        CHECK_EQ(q.data_len, 250, "maximum payload length");
    }
}

static void kline_framing(void)
{
    /* K-line (channels '3' and '4'): a plain data frame carries no timestamp;
     * the START and END frames carry the timestamp and nothing else. This is
     * the reading of every independent implementation (PROTOCOL.md §7); the
     * fixture bytes are the ones those implementations test against. */
    op_reply r;
    {
        const uint8_t f[] = { 'a', 'r', '3', 0x06, 0x00, 0x80, 0xF0, 0x10, 0x01, 0xFF };
        size_t n = op_parse(f, sizeof f, &r);
        CHECK_EQ(n, sizeof f, "K-line data frame consumed");
        CHECK_EQ(r.kind, OP_REPLY_FRAME, "K-line data frame kind");
        CHECK_EQ(r.timestamp_us, 0, "K-line data frame has no timestamp");
        CHECK_EQ(r.data_len, 5, "all five body bytes are data");
        CHECK(r.data[0] == 0x80 && r.data[4] == 0xFF, "K-line header and checksum preserved");
    }
    {
        const uint8_t f[] = { 'a', 'r', '3', 0x05, 0x80, 0x00, 0x01, 0x02, 0x03 };
        size_t n = op_parse(f, sizeof f, &r);
        CHECK_EQ(n, sizeof f, "K-line START frame consumed");
        CHECK_EQ(r.timestamp_us, 0x00010203u, "K-line START frame carries the timestamp");
        CHECK_EQ(r.data_len, 0, "K-line START frame carries no data");
    }
    {
        const uint8_t f[] = { 'a', 'r', '4', 0x05, 0x40, 0x00, 0x01, 0x02, 0x04 };
        size_t n = op_parse(f, sizeof f, &r);
        CHECK_EQ(n, sizeof f, "K-line END frame consumed");
        CHECK_EQ(r.timestamp_us, 0x00010204u, "K-line END frame carries the timestamp");
        CHECK_EQ(r.data_len, 0, "K-line END frame carries no data");
    }
    {
        /* One implementation's fixture has an END frame with no body at all;
         * that must parse as a frame too, not as junk. */
        const uint8_t f[] = { 'a', 'r', '3', 0x01, 0x40 };
        size_t n = op_parse(f, sizeof f, &r);
        CHECK_EQ(n, sizeof f, "bare K-line END frame consumed");
        CHECK_EQ(r.kind, OP_REPLY_FRAME, "bare K-line END frame is a frame");
        CHECK_EQ(r.data_len, 0, "bare K-line END frame has no data");
    }
    {
        /* CAN keeps the uniform layout: a data frame carries the timestamp. */
        const uint8_t f[] = { 'a', 'r', '5', 0x0A, 0x00, 0x00, 0x01, 0x02, 0x03,
                              0x80, 0xF0, 0x10, 0x01, 0xFF };
        size_t n = op_parse(f, sizeof f, &r);
        CHECK_EQ(n, sizeof f, "CAN data frame consumed");
        CHECK_EQ(r.timestamp_us, 0x00010203u, "CAN data frame timestamp");
        CHECK_EQ(r.data_len, 5, "CAN data after the timestamp");
    }
}

static void init_reply(void)
{
    /* `ary<ch> <n>` is followed by n raw bytes that belong to the reply. The
     * parser must hold the whole thing back until they are all present, and
     * must never hand the raw bytes to the next reply. */
    op_reply r;
    const uint8_t full[] = { 'a','r','y','3',' ','2','\r','\n', 0x55, 0x08, 'a','r','o','\r','\n' };
    size_t n;

    n = op_parse(full, 8, &r);
    CHECK_EQ(n, 0, "init reply without its bytes is incomplete");
    CHECK_EQ(r.kind, OP_REPLY_INCOMPLETE, "incomplete kind");
    n = op_parse(full, 9, &r);
    CHECK_EQ(n, 0, "init reply with one of two bytes is incomplete");
    n = op_parse(full, sizeof full, &r);
    CHECK_EQ(n, 10, "init reply consumed with its two bytes, not the aro after it");
    CHECK_EQ(r.kind, OP_REPLY_INIT, "init reply kind");
    CHECK_EQ(r.channel, 3, "init reply channel");
    CHECK_EQ(r.data_len, 2, "init reply byte count");
    CHECK(r.data[0] == 0x55 && r.data[1] == 0x08, "init reply bytes");
    n = op_parse(full + 10, sizeof full - 10, &r);
    CHECK_EQ(r.kind, OP_REPLY_OK, "the aro after it is still intact");
    CHECK_EQ(n, 5, "aro length");

    {
        const uint8_t none[] = { 'a','r','y','4',' ','0','\r','\n' };
        n = op_parse(none, sizeof none, &r);
        CHECK_EQ(n, 8, "zero-length init reply consumed");
        CHECK_EQ(r.kind, OP_REPLY_INIT, "zero-length init reply kind");
        CHECK_EQ(r.data_len, 0, "zero-length init reply has no bytes");
    }
    {
        char line[OP_CMD_MAX];
        n = op_cmd_five_baud(line, sizeof line, 3, 0x33);
        CHECK(n > 0 && strcmp(line, "atw3 51\r\n") == 0, "five-baud init: address in decimal, no payload (the vendor's form)");
        n = op_cmd_fast_init(line, sizeof line, 4, 4);
        CHECK(n > 0 && strcmp(line, "aty4 4 0\r\n") == 0, "fast init command form");
        /* J2534 defines a NULL fast-init input as "no message transmitted",
         * so an empty request is a legal command, not a malformed one. */
        n = op_cmd_fast_init(line, sizeof line, 4, 0);
        CHECK(n > 0 && strcmp(line, "aty4 0 0\r\n") == 0,
              "fast init with no request bytes is legal");
        CHECK_EQ(op_cmd_five_baud(line, sizeof line, 3, 256), 0, "five-baud address is one byte");
    }
}

static void interleaving(void)
{
    /* The device interleaves asynchronous frames with command replies on one
     * pipe; both must come out of a single buffer in order. */
    uint8_t s[64];
    size_t off = 0, n;
    op_reply r;

    memcpy(s + off, k_real_frame, sizeof k_real_frame); off += sizeof k_real_frame;
    memcpy(s + off, "aro\r\n", 5); off += 5;

    n = op_parse(s, off, &r);
    CHECK_EQ(r.kind, OP_REPLY_FRAME, "frame first");
    CHECK_EQ(n, sizeof k_real_frame, "frame length");

    n = op_parse(s + n, off - n, &r);
    CHECK_EQ(r.kind, OP_REPLY_OK, "reply second");
    CHECK_EQ(n, 5, "reply length");
}

static void resync(void)
{
    /* A malformed binary payload desynchronises the device's parser — observed
     * on real hardware. Recovery means finding the next plausible boundary. */
    const uint8_t junk[] = { 0xDE, 0xAD, 0xBE, 'a', 'r', 'o', '\r', '\n' };
    size_t off = op_resync_offset(junk, sizeof junk);
    CHECK_EQ(off, 3, "resync finds the next reply");

    {
        const uint8_t none[] = { 1, 2, 3, 4, 5 };
        CHECK_EQ(op_resync_offset(none, sizeof none), sizeof none,
                 "no boundary reports the whole buffer");
    }
    CHECK_EQ(op_resync_offset(NULL, 8), 8, "NULL is handled");
}

static void be32(void)
{
    uint8_t b[4];
    op_wr_be32(b, 0x12345678u);
    CHECK_EQ(b[0], 0x12, "big-endian byte 0");
    CHECK_EQ(b[3], 0x78, "big-endian byte 3");
    CHECK_EQ(op_rd_be32(b), 0x12345678u, "round trip");
    {
        const uint8_t max[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
        CHECK_EQ(op_rd_be32(max), 0xFFFFFFFFu, "0xFFFFFFFF reads back");
    }
}

void test_frames(void)
{
    SUITE("message frames");
    layout();
    status_bits();
    truncation();
    bounds();
    kline_framing();
    init_reply();
    interleaving();
    resync();
    be32();
}
