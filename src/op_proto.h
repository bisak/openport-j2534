/*
 * op_proto.h — OpenPort wire-protocol codec.
 *
 * Pure functions: they encode commands into caller-supplied buffers and parse
 * replies out of caller-supplied buffers. No I/O, no globals, no allocation.
 * Everything here is exercised by the unit tests with no hardware attached.
 *
 * Wire format (verified against an OpenPort 2.0, firmware 1.17.4877 — see
 * docs/PROTOCOL.md):
 *
 *   Host -> device : "at<verb>[<channel>] <args>\r\n"  [+ binary payload]
 *   Device -> host : "ar<verb> <args>\r\n"             (ASCII reply)
 *                  : 'a' 'r' <ch> <len> <status> <ts:be32> <data...>
 *                                                      (binary message frame)
 *
 * An ASCII reply always has a letter at index 2 (o/e/i/r/f/g/y); a binary frame
 * always has an ASCII digit there. That is the whole discriminator, and it is
 * unambiguous because the device accepts single-digit channels only. The
 * length byte spans the full 0..255 range and must not be used to tell the two
 * apart.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef OP_PROTO_H
#define OP_PROTO_H

#include <stddef.h>
#include <stdint.h>

/* Longest command line we ever emit, excluding any binary payload. */
#define OP_CMD_MAX          64
/* A frame's length field is one byte, so a frame never exceeds this. */
#define OP_FRAME_MAX        (4 + 255)
/* Reassembly ceiling: one J2534 message. */
#define OP_MSG_MAX          4128

/* Message-frame status byte. Verified as a bitfield, not an enum. */
#define OP_STS_START        0x80u   /* first frame of a message          */
#define OP_STS_END          0x40u   /* last frame of a message           */
#define OP_STS_LOOPBACK     0x20u   /* transmit echo rather than receive */
#define OP_STS_TX_IND       0x10u   /* transmit indication (see PROTOCOL.md) */
/* Measured 2026-09-13: set exactly when the frame's CAN id is 29-bit. Two
 * third-party drivers read this bit as J2534's START_OF_MESSAGE; on this
 * firmware it is not (PROTOCOL.md §7). */
#define OP_STS_29BIT        0x02u

typedef enum {
    OP_REPLY_INCOMPLETE = 0, /* need more bytes before this can be decided */
    OP_REPLY_OK,             /* aro                                       */
    OP_REPLY_ERROR,          /* are <code> [<detail>]                     */
    OP_REPLY_INFO,           /* ari <text>                                */
    OP_REPLY_PIN,            /* arr <pin> <millivolts>                    */
    OP_REPLY_FILTER,         /* arf<ch> <filter_id> <detail>              */
    OP_REPLY_CONFIG,         /* arg<ch> <param> <value> <detail>          */
    OP_REPLY_FRAME,          /* binary message frame                      */
    OP_REPLY_INIT,           /* ary<ch> <len> + <len> raw bytes (K-line init) */
    OP_REPLY_PERIODIC,       /* arm<ch> <msg_id> <n> (firmware periodic)      */
    OP_REPLY_JUNK            /* a complete line we do not recognise       */
} op_reply_kind;

typedef struct {
    op_reply_kind kind;

    /* OP_REPLY_ERROR */
    uint32_t      error_code;    /* the device emits raw J2534 codes */
    int           has_detail;
    uint32_t      detail;

    /* OP_REPLY_PIN / OP_REPLY_CONFIG / OP_REPLY_FILTER / OP_REPLY_PERIODIC */
    uint32_t      a, b;          /* pin/param/filter-id/msg-id, value */

    /* Numeric tokens after a text reply's fixed fields, in order. The device
     * echoes the sequence number a command carried as the LAST of them
     * (PROTOCOL.md section 3); for `are` the first may be a detail value.
     * The device layer, which knows whether the command was numbered, decides
     * which is which. */
    unsigned      ntail;
    uint32_t      tail[3];

    /* OP_REPLY_INFO: points into the caller's buffer, not NUL-terminated */
    const char   *text;
    size_t        text_len;

    /* OP_REPLY_FRAME */
    unsigned      channel;       /* decoded from the ASCII digit */
    uint8_t       status;
    uint32_t      timestamp_us;
    const uint8_t *data;         /* points into the caller's buffer */
    size_t        data_len;
} op_reply;

/*
 * Decode the first reply in [buf, buf+len).
 *
 * Returns the number of bytes consumed, or 0 when the buffer does not yet hold
 * a complete reply (out->kind is then OP_REPLY_INCOMPLETE and the caller must
 * read more). Never reads past buf+len.
 */
size_t op_parse(const uint8_t *buf, size_t len, op_reply *out);

/*
 * Find the next plausible reply boundary at or after buf[1], for resynchronising
 * after the device and host disagree about a binary payload length. Returns the
 * offset of the candidate, or len when there is none.
 */
/* Whether a frame body (everything after the status byte) starts with the
 * 4-byte timestamp. CAN: always. K-line: only on START/END/TX_IND frames. */
int op_frame_has_timestamp(unsigned channel, uint8_t status, size_t body_len);

size_t op_resync_offset(const uint8_t *buf, size_t len);

/* ---- Command encoders -------------------------------------------------
 * Each writes a NUL-terminated command line into out[0..out_sz) and returns
 * its length, or 0 if it would not fit. Binary payloads are the caller's to
 * append; the encoder reports how many bytes the device will expect.
 */
size_t op_cmd_attention(char *out, size_t out_sz);                 /* ata */
size_t op_cmd_reset(char *out, size_t out_sz);                     /* atz */
size_t op_cmd_version(char *out, size_t out_sz);                   /* ati */
size_t op_cmd_open(char *out, size_t out_sz,
                   unsigned proto, uint32_t flags, uint32_t baud); /* ato */
size_t op_cmd_close(char *out, size_t out_sz, unsigned ch);        /* atc */
size_t op_cmd_read_pin(char *out, size_t out_sz, unsigned pin);    /* atr */
size_t op_cmd_get_config(char *out, size_t out_sz,
                         unsigned ch, uint32_t param);             /* atg */
size_t op_cmd_set_config(char *out, size_t out_sz,
                         unsigned ch, uint32_t param, uint32_t value); /* ats */
/* att<ch> <len> <txflags> <timeout_us>: the fourth argument is the firmware's
 * budget for getting the message onto the bus, in microseconds. The vendor
 * DLL sends the application's WriteMsgs Timeout there (5000000 for 5000 ms)
 * and 1000000 for a Timeout of 0; without it the firmware gives up after
 * about a second, which is too short for a 257-byte ISO-TP transfer paced by a
 * slow ECU (measured: tools/ab-official replay of a reflash tool). */
size_t op_cmd_transmit(char *out, size_t out_sz, unsigned ch,
                       size_t payload_len, uint32_t txflags,
                       uint32_t timeout_us);                       /* att */
size_t op_cmd_filter(char *out, size_t out_sz, unsigned ch,
                     uint32_t type, uint32_t txflags, size_t each_len); /* atf */
size_t op_cmd_stop_filter(char *out, size_t out_sz,
                          unsigned ch, uint32_t filter_id);        /* atk */
/* K-line init. Fast init: "aty<ch> <len> 0" followed by the StartCommunication
 * request bytes. Five-baud: "atw<ch> <address>", the address in decimal on
 * the command line and no payload, which is what Tactrix's own DLL sends
 * (`atw3 51` for 0x33; tools/ab-official, 2026-09-13). The earlier form
 * "atw<ch> 1" + one raw byte made the firmware address ECU 1 and then
 * misread the byte as the start of the next command. The device answers
 * "ary<ch> <n>" + n raw response bytes (PROTOCOL.md section 4). */
size_t op_cmd_fast_init(char *out, size_t out_sz, unsigned ch,
                        size_t payload_len);                       /* aty */
size_t op_cmd_five_baud(char *out, size_t out_sz, unsigned ch,
                        unsigned address);                         /* atw */

/*
 * Command sequence numbers. The firmware echoes a trailing decimal argument
 * back in its reply (`ata 2` -> `aro 2`, `atr 16 3` -> `arr 16 108 3`), which
 * is how a reply is tied to the command it answers instead of by position
 * alone. op_cmd_is_numbered says whether a verb takes one: every verb but
 * `ati`, which ignores it. `aty`/`atw` echo it on failure (`are 7 10`); their
 * success reply `ary` has not been measured with one, so the device layer
 * accepts an init reply with or without it.
 * op_cmd_number inserts " <seq>" before the terminating CR LF of a line of
 * `len` bytes and returns the new length, or 0 if it would not fit.
 */
int    op_cmd_is_numbered(const char *line);
size_t op_cmd_number(char *line, size_t out_sz, size_t len, uint32_t seq);

/*
 * How many messages a filter of this type requires the host to append.
 * PASS/BLOCK take mask+pattern; FLOW_CONTROL additionally takes the flow
 * control message. Returns 0 for an unknown type.
 */
unsigned op_filter_msg_count(uint32_t filter_type);

/* Extract the version from an `ari` reply body, e.g.
 * "main code version : 1.17.4877" -> "1.17.4877". Writes at most out_sz bytes
 * including the NUL. Returns 1 on success, 0 if no delimiter was found. */
int op_parse_version(const char *text, size_t text_len, char *out, size_t out_sz);

/* Big-endian helpers — the device is big-endian on the wire throughout. */
uint32_t op_rd_be32(const uint8_t *p);
void     op_wr_be32(uint8_t *p, uint32_t v);

#endif /* OP_PROTO_H */
