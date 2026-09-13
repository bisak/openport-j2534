/*
 * Command encoding, ASCII reply decoding, version extraction.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test.h"
#include "op_proto.h"

static void encoders(void)
{
    char b[OP_CMD_MAX];

    CHECK_EQ(op_cmd_close_all(b, sizeof b), 5, "ata length");
    CHECK_STR(b, "ata\r\n", "ata");

    op_cmd_reset(b, sizeof b);   CHECK_STR(b, "atz\r\n", "atz");
    op_cmd_version(b, sizeof b); CHECK_STR(b, "ati\r\n", "ati");

    /* The device wants the protocol digit fused to the verb and a literal
     * trailing zero; a space after `ato` is a different command. */
    op_cmd_open(b, sizeof b, 6, 0, 500000);
    CHECK_STR(b, "ato6 0 500000 0\r\n", "ato ISO15765 500k");

    op_cmd_open(b, sizeof b, 5, 0x100, 250000);
    CHECK_STR(b, "ato5 256 250000 0\r\n", "ato CAN 29-bit");

    op_cmd_close(b, sizeof b, 6);  CHECK_STR(b, "atc6\r\n", "atc");

    /* atr is the one command with a space before its argument. */
    op_cmd_read_pin(b, sizeof b, 16); CHECK_STR(b, "atr 16\r\n", "atr");

    op_cmd_get_config(b, sizeof b, 6, 1);
    CHECK_STR(b, "atg6 1\r\n", "atg");
    op_cmd_set_config(b, sizeof b, 6, 0x1E, 8);
    CHECK_STR(b, "ats6 30 8\r\n", "ats ISO15765_BS");

    op_cmd_transmit(b, sizeof b, 6, 6, 0x40, 1000000);
    CHECK_STR(b, "att6 6 64 1000000\r\n", "att with FRAME_PAD and a 1 s budget");
    op_cmd_transmit(b, sizeof b, 6, 261, 0x40, 5000000);
    CHECK_STR(b, "att6 261 64 5000000\r\n", "att carries the caller's timeout in microseconds");

    op_cmd_filter(b, sizeof b, 6, 3, 0, 4);
    CHECK_STR(b, "atf6 3 0 4\r\n", "atf flow control");

    op_cmd_stop_filter(b, sizeof b, 6, 0);
    CHECK_STR(b, "atk6 0\r\n", "atk");

    /* Refuse rather than truncate: a half-written line desyncs the device. */
    CHECK_EQ(op_cmd_open(b, 4, 6, 0, 500000), 0, "ato into a short buffer");
    CHECK_EQ(op_cmd_transmit(b, sizeof b, 6, 0, 0, 1000000), 0, "att with empty payload");
    CHECK_EQ(op_cmd_transmit(b, sizeof b, 6, OP_MSG_MAX + 1, 0, 1000000), 0, "att oversize");
    CHECK_EQ(op_cmd_filter(b, sizeof b, 6, 3, 0, 256), 0, "atf length > 255");
    CHECK_EQ(op_cmd_filter(b, sizeof b, 6, 3, 0, 0), 0, "atf zero length");
}

static void filter_counts(void)
{
    CHECK_EQ(op_filter_msg_count(1), 2, "PASS_FILTER messages");
    CHECK_EQ(op_filter_msg_count(2), 2, "BLOCK_FILTER messages");
    CHECK_EQ(op_filter_msg_count(3), 3, "FLOW_CONTROL_FILTER messages");
    CHECK_EQ(op_filter_msg_count(0), 0, "unknown filter type");
    CHECK_EQ(op_filter_msg_count(99), 0, "out-of-range filter type");
}

static void replies(void)
{
    op_reply r;
    size_t n;

    n = op_parse((const uint8_t *)"aro\r\n", 5, &r);
    CHECK_EQ(n, 5, "aro consumed");
    CHECK_EQ(r.kind, OP_REPLY_OK, "aro kind");

    n = op_parse((const uint8_t *)"are 9\r\n", 7, &r);
    CHECK_EQ(n, 7, "are consumed");
    CHECK_EQ(r.kind, OP_REPLY_ERROR, "are kind");
    CHECK_EQ(r.error_code, 9, "are code");
    CHECK_EQ(r.has_detail, 0, "are without detail");

    n = op_parse((const uint8_t *)"are 10 6\r\n", 10, &r);
    CHECK_EQ(r.kind, OP_REPLY_ERROR, "are+detail kind");
    CHECK_EQ(r.error_code, 10, "are+detail code");
    CHECK_EQ(r.has_detail, 1, "are+detail present");
    CHECK_EQ(r.detail, 6, "are detail value");

    n = op_parse((const uint8_t *)"arr 16 12480\r\n", 14, &r);
    CHECK_EQ(r.kind, OP_REPLY_PIN, "arr kind");
    CHECK_EQ(r.a, 16, "arr pin");
    CHECK_EQ(r.b, 12480, "arr millivolts");

    /* Sequence-number echoes, measured on the cable 2026-09-13 (PROTOCOL.md
     * section 3): the number is the last trailing token of every text reply. */
    n = op_parse((const uint8_t *)"aro 2\r\n", 7, &r);
    CHECK_EQ(r.kind, OP_REPLY_OK, "aro with sequence: kind");
    CHECK_EQ(r.ntail, 1, "aro with sequence: one trailing token");
    CHECK_EQ(r.tail[0], 2, "aro with sequence: value");
    n = op_parse((const uint8_t *)"arr 16 108 3\r\n", 14, &r);
    CHECK_EQ(r.kind, OP_REPLY_PIN, "arr with sequence: kind");
    CHECK_EQ(r.b, 108, "arr with sequence: millivolts");
    CHECK_EQ(r.tail[0], 3, "arr with sequence: value");
    n = op_parse((const uint8_t *)"are 1 9\r\n", 9, &r);
    CHECK_EQ(r.error_code, 1, "are with sequence: code");
    CHECK_EQ(r.ntail, 1, "are with sequence: one trailing token, detail or sequence");
    n = op_parse((const uint8_t *)"are 5 100 7\r\n", 13, &r);
    CHECK_EQ(r.ntail, 2, "are with detail and sequence: two trailing tokens");
    CHECK_EQ(r.tail[0], 100, "are detail first");
    CHECK_EQ(r.tail[1], 7, "are sequence last");
    n = op_parse((const uint8_t *)"arm6 1 12\r\n", 11, &r);
    CHECK_EQ(r.kind, OP_REPLY_PERIODIC, "arm kind");
    CHECK_EQ(r.channel, 6, "arm channel");
    CHECK_EQ(r.a, 1, "arm message id");
    CHECK_EQ(r.tail[0], 12, "arm sequence");
    {
        char line[64];
        size_t k = op_cmd_reset(line, sizeof line);
        k = op_cmd_number(line, sizeof line, k, 17);
        CHECK(k == 8 && memcmp(line, "atz 17\r\n", 8) == 0, "op_cmd_number inserts before CR LF");
        CHECK(op_cmd_is_numbered("ato6 0 500000 0\r\n"), "ato is numbered");
        CHECK(!op_cmd_is_numbered("ati\r\n"), "ati is not");
        CHECK(op_cmd_is_numbered("aty3 4 0\r\n"), "aty is numbered: the echo was measured on its failure reply");
    }

    n = op_parse((const uint8_t *)"arf6 0 0\r\n", 10, &r);
    CHECK_EQ(r.kind, OP_REPLY_FILTER, "arf kind");
    CHECK_EQ(r.channel, 6, "arf channel");
    CHECK_EQ(r.a, 0, "arf filter id");

    n = op_parse((const uint8_t *)"arg6 1 500000 0\r\n", 17, &r);
    CHECK_EQ(r.kind, OP_REPLY_CONFIG, "arg kind");
    CHECK_EQ(r.channel, 6, "arg channel");
    CHECK_EQ(r.a, 1, "arg param");
    CHECK_EQ(r.b, 500000, "arg value");

    n = op_parse((const uint8_t *)"ari main code version : 1.17.4877\r\n", 35, &r);
    CHECK_EQ(r.kind, OP_REPLY_INFO, "ari kind");
    CHECK_EQ(r.text_len, 29, "ari text length");
    (void)n;
}

static void truncated_and_malformed(void)
{
    op_reply r;

    /* Every prefix of a valid reply must ask for more, never mis-decode. */
    {
        const char *full = "arr 16 12480\r\n";
        size_t i;
        for (i = 1; i < strlen(full); i++) {
            size_t n = op_parse((const uint8_t *)full, i, &r);
            CHECK(n == 0 || r.kind != OP_REPLY_PIN,
                  "prefix of length %zu decoded early", i);
        }
    }

    CHECK_EQ(op_parse(NULL, 10, &r), 0, "NULL buffer");
    CHECK_EQ(op_parse((const uint8_t *)"", 0, &r), 0, "empty buffer");

    /* Junk is consumed one byte at a time so the caller can resync. */
    {
        size_t n = op_parse((const uint8_t *)"xyz\r\n", 5, &r);
        CHECK_EQ(n, 1, "leading junk consumed");
        CHECK_EQ(r.kind, OP_REPLY_JUNK, "junk kind");
    }
    {
        size_t n = op_parse((const uint8_t *)"arQ\r\n", 5, &r);
        CHECK_EQ(n, 5, "unknown verb consumes the line");
        CHECK_EQ(r.kind, OP_REPLY_JUNK, "unknown verb kind");
    }
    /* A number far beyond 32 bits must saturate, not wrap into a small value. */
    {
        op_parse((const uint8_t *)"are 99999999999999\r\n", 20, &r);
        CHECK_EQ(r.kind, OP_REPLY_ERROR, "huge error code still an error");
        CHECK_EQ(r.error_code, 0xFFFFFFFFu, "huge error code saturates");
    }
}

static void versions(void)
{
    char v[32];

    CHECK(op_parse_version("main code version : 1.17.4877", 29, v, sizeof v),
          "version parsed");
    CHECK_STR(v, "1.17.4877", "version value");

    /* The old driver hardcoded byte offset 24, which is exactly
     * strlen("ari main code version : "). Splitting on the delimiter gets the
     * same answer without depending on the prefix never changing. */
    CHECK(op_parse_version("other prefix here : 2.0.1", 25, v, sizeof v),
          "version parsed with a different prefix");
    CHECK_STR(v, "2.0.1", "version with a different prefix");

    CHECK_EQ(op_parse_version("no delimiter", 12, v, sizeof v), 0,
             "missing delimiter reports failure");
    CHECK_STR(v, "", "failed parse leaves an empty string");

    /* Must not overflow a small caller buffer. */
    {
        char tiny[4];
        op_parse_version("x : 123456789", 13, tiny, sizeof tiny);
        CHECK(strlen(tiny) < sizeof tiny, "version truncated safely");
    }
}

void test_proto(void)
{
    SUITE("protocol codec");
    encoders();
    filter_counts();
    replies();
    truncated_and_malformed();
    versions();
}
