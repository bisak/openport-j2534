/*
 * op_j2534.c — the fourteen SAE J2534-1 entry points.
 *
 * Rules this file holds to, because the caller may be mid-erase on an ECU that
 * cannot be replaced:
 *   - every transport call's status is inspected and propagated;
 *   - every wait has a bound, so a wedged cable yields ERR_TIMEOUT, not a hang;
 *   - nothing reports success it did not achieve;
 *   - pointer arguments are checked before they are dereferenced.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "j2534/j2534.h"
#include "op_device.h"
#include "op_error.h"
#include "op_log.h"
#include "op_proto.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define OP_DEVICE_ID        1UL
#define OP_DEFAULT_CMD_MS   1000u

#define OP_API_VERSION      "04.04"
#define OP_DLL_VERSION      "openport-j2534 " OPENPORT_VERSION

/* The firmware answers `atr` for pins 8, 12, 16 and 17 and ERR_PIN_INVALID
 * for every other (docs/PROTOCOL.md section 8). READ_PROG_VOLTAGE reads pin 12
 * unless the caller names a pin, as Tactrix's DLL lets it. */
#define OP_PIN_PROG_VOLTAGE 12
#define OP_PIN_VBATT        16
#define OP_PIN_K            7
#define OP_PIN_L            15

/* ---- helpers ------------------------------------------------------------ */


/* ---- call recording -------------------------------------------------------
 * OPENPORT_RECORD=<file> appends one line per entry-point call with every
 * input, in a form tools/ab-official/j2534_trace.c can replay against another
 * J2534 library: what an application asked for, independent of what this
 * driver did with it. Ids are recorded as the application saw them; the
 * replayer maps them onto the ids the other library hands out.
 */
static FILE *g_rec;

static void rec_hex(const unsigned char *d, size_t n)
{
    size_t i;
    if (n == 0) { fputc('-', g_rec); return; }
    for (i = 0; i < n; i++) fprintf(g_rec, "%02x", d[i]);
}

static void rec_msg(const PASSTHRU_MSG *m)
{
    if (m == NULL) { fputs(" - - -", g_rec); return; }
    fprintf(g_rec, " %lu %lu ", (unsigned long)m->ProtocolID, (unsigned long)m->TxFlags);
    rec_hex(m->Data, m->DataSize > J2534_MSG_DATA_MAX ? J2534_MSG_DATA_MAX : m->DataSize);
}

static int rec_on(void)
{
    if (g_rec == NULL) {
        const char *path = getenv("OPENPORT_RECORD");
        if (path != NULL && *path != '\0' && (g_rec = fopen(path, "a")) != NULL)
            setvbuf(g_rec, NULL, _IOLBF, 0);
    }
    return g_rec != NULL;
}

static void rec_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void rec_line(const char *fmt, ...)
{
    va_list ap;
    if (!rec_on()) return;
    va_start(ap, fmt);
    vfprintf(g_rec, fmt, ap);
    va_end(ap);
    fputc('\n', g_rec);
}

static void rec_msgs(const char *head, const PASSTHRU_MSG *m, J_U32 n)
{
    J_U32 i;
    if (!rec_on()) return;
    fputs(head, g_rec);
    if (m == NULL) n = 0;
    for (i = 0; i < n && i < 64; i++) rec_msg(&m[i]);
    fputc('\n', g_rec);
}

static long fail(long code, const char *what)
{
    op_err_set("%s: %s", what, op_err_name(code));
    return code;
}

/* Translate an `are <n>` from the device. The firmware emits J2534 codes
 * directly — verified by probing: invalid pin -> 19, channel in use -> 20,
 * bad filter id -> 22, unsupported config -> 1 — so the mapping is identity
 * for every code the standard defines. */
static long device_error(uint32_t code)
{
    /*
     * `are <n>` carries a literal J2534 return code, so the only correct
     * mapping is the identity. The ceiling here used to be the highest code
     * this header names (ERR_INVALID_DEVICE_ID), which quietly collapsed every
     * later one — ERR_INIT_FAILED and ERR_IOCTL_PARAM_ID_NOT_SUPPORTED among
     * them — into ERR_FAILED, and flatly contradicted DIFFERENTIAL.md's claim
     * that device errors pass through unchanged. A code this build does not
     * name is still the device's answer and is more use to a caller than
     * ERR_FAILED. Tactrix's own 0x77/0x78 for a programming voltage out of
     * range fall out of the same rule.
     */
    if (code <= ERR_J2534_HIGHEST || code == ERR_OEM_VOLTAGE_TOO_HIGH ||
        code == ERR_OEM_VOLTAGE_TOO_LOW)
        return (long)code;
    op_logf("device reported error %u, outside every range this build knows",
            code);
    return ERR_FAILED;
}

static long reply_to_status(const op_reply *r)
{
    if (r->kind == OP_REPLY_ERROR) return device_error(r->error_code);
    return STATUS_NOERROR;
}

/* Run one command and require a non-error reply. */
static long simple_cmd(op_device *d, const char *line, size_t len,
                       const uint8_t *payload, size_t payload_len,
                       unsigned timeout_ms, op_reply *out)
{
    op_reply r;
    op_status st = op_device_cmd(d, line, len, payload, payload_len,
                                 timeout_ms, &r);
    if (st != OP_OK) return op_status_to_j2534(st);
    if (out != NULL) *out = r;
    return reply_to_status(&r);
}

static int channel_valid(J_U32 id) { return id < OP_MAX_CHANNELS; }

/*
 * Whether a channel of this protocol drives the pin. The firmware grounds K or
 * L on request while ISO9141 is open on it (measured 2026-09-16), and so does
 * Tactrix's DLL, which silently kills the channel. J2534-1 lets a K-line
 * channel use L for initialisation unless opened ISO9141_K_LINE_ONLY, but
 * whether this firmware drives L on channels 3 and 4 is unmeasured, so L is
 * guarded only on the J2534-2 L-line channels, which certainly use it.
 */
static int protocol_uses_pin(J_U32 protocol, J_U32 pin)
{
    switch (op_protocol_channel(protocol)) {
    case 3: case 4:
        return pin == OP_PIN_K;
    case 7: case 8:
        return pin == OP_PIN_L;
    default:
        return 0;
    }
}

static op_channel *channel_of(op_device *d, J_U32 id)
{
    if (!channel_valid(id)) return NULL;
    if (!d->ch[id].open) return NULL;
    return &d->ch[id];
}

/* Stop one firmware periodic message and forget it. The firmware keeps them
 * across a crashed host (PROTOCOL.md section 10), which is why open and close
 * both reset the device. */
static long periodic_stop(op_device *d, op_channel *c, J_U32 channel, unsigned slot)
{
    char line[OP_CMD_MAX];
    size_t n = op_cmd_periodic_stop(line, sizeof line, (unsigned)channel, c->periodic[slot]);
    long rc = simple_cmd(d, line, n, NULL, 0, OP_DEFAULT_CMD_MS, NULL);

    /* Gone either way: the device stopped it, or says it no longer exists. */
    if (rc == STATUS_NOERROR || rc == ERR_INVALID_MSG_ID)
        c->periodic[slot] = c->periodic[--c->nperiodic];
    return rc;
}

/* ---- 1. PassThruOpen ---------------------------------------------------- */

long PassThruOpen(const void *pName, J_U32 *pDeviceID)
{
    op_device *d = op_device_get();
    op_status st;
    op_reply r;
    long rc;
    char line[OP_CMD_MAX];
    size_t n;

    (void)pName;                       /* single-device library */
    op_log_init();
    op_err_clear();
    rec_line("open");

    if (pDeviceID == NULL)
        return fail(ERR_NULL_PARAMETER, "PassThruOpen(pDeviceID)");

    if (d->open)
        return fail(ERR_DEVICE_IN_USE, "PassThruOpen");

    st = op_device_open(d);
    if (st != OP_OK) {
        const char *detail = op_transport_last_detail();
        long code;

        switch (st) {
        case OP_ERR_ACCESS:
            code = ERR_DEVICE_IN_USE;
            break;
        case OP_ERR_PROTOCOL:
            /* Found and opened, but it will not synchronise. Distinct from
             * "not connected" so the user debugs the link, not the cable. */
            code = ERR_FAILED;
            op_err_set("Device found but it is not answering the protocol "
                       "correctly (stream may be corrupt).");
            detail = NULL;
            break;
        default:
            code = ERR_DEVICE_NOT_CONNECTED;
            break;
        }
        if (detail != NULL) op_err_set("%s", detail);
        else if (code != ERR_FAILED) op_err_set("PassThruOpen: %s", op_err_name(code));
        op_logf("open failed: %s", op_err_get());
        return code;
    }

    /* op_device_open has already synchronised the link and proved the device
     * answers, so go straight to the version. A reply of the wrong kind here
     * would mean the pipe is out of step, which is a hard failure: every later
     * result would belong to the wrong command. */
    n = op_cmd_version(line, sizeof line);
    rc = simple_cmd(d, line, n, NULL, 0, OP_DEFAULT_CMD_MS, &r);
    if (rc != STATUS_NOERROR) {
        op_device_close(d);
        return fail(rc, "PassThruOpen: device did not identify");
    }
    if (r.kind != OP_REPLY_INFO) {
        op_device_close(d);
        op_err_set("PassThruOpen: device replies are out of step");
        return ERR_FAILED;
    }
    if (!op_parse_version(r.text, r.text_len, d->fw_version, sizeof d->fw_version)) {
        op_device_close(d);
        op_err_set("PassThruOpen: unrecognised version reply");
        return ERR_FAILED;
    }

    *pDeviceID = OP_DEVICE_ID;
    rec_line("= %lu", (unsigned long)OP_DEVICE_ID);
    op_logf("open ok, firmware '%s'", d->fw_version);
    return STATUS_NOERROR;
}

/* ---- 2. PassThruClose --------------------------------------------------- */

long PassThruClose(J_U32 DeviceID)
{
    op_device *d = op_device_get();
    char line[OP_CMD_MAX];
    size_t n;

    op_err_clear();
    rec_line("close %lu", (unsigned long)DeviceID);
    /* A device id that names nothing open is an invalid id: the standard's
     * code, and the vendor's (docs/DIFFERENTIAL.md). */
    if (!d->open || DeviceID != OP_DEVICE_ID)
        return fail(ERR_INVALID_DEVICE_ID, "PassThruClose");

    /* atz stops every periodic message, closes every channel and releases
     * every pin (measured, PROTOCOL.md sections 8 and 10). */
    n = op_cmd_reset(line, sizeof line);
    (void)simple_cmd(d, line, n, NULL, 0, OP_DEFAULT_CMD_MS, NULL);

    op_device_close(d);
    op_logf("closed");
    return STATUS_NOERROR;
}

/* ---- 3. PassThruConnect ------------------------------------------------- */

long PassThruConnect(J_U32 DeviceID, J_U32 ProtocolID, J_U32 Flags,
                     J_U32 BaudRate, J_U32 *pChannelID)
{
    op_device *d = op_device_get();
    char line[OP_CMD_MAX];
    size_t n;
    long rc;
    int fw;

    op_err_clear();
    rec_line("connect %lu %lu %lu %lu", (unsigned long)DeviceID, (unsigned long)ProtocolID, (unsigned long)Flags, (unsigned long)BaudRate);
    if (!d->open || DeviceID != OP_DEVICE_ID)
        return fail(ERR_INVALID_DEVICE_ID, "PassThruConnect");
    if (pChannelID == NULL)
        return fail(ERR_NULL_PARAMETER, "PassThruConnect(pChannelID)");
    fw = op_protocol_channel(ProtocolID);
    if (fw < 0)
        return fail(ERR_INVALID_PROTOCOL_ID, "PassThruConnect");
    if (d->ch[fw].open)
        return fail(ERR_CHANNEL_IN_USE, "PassThruConnect");
    if (Flags & SNIFF_MODE)
        op_logf("SNIFF_MODE requested: firmware 1.17.4877 accepts the flag but "
                "still acknowledges frames (PROTOCOL.md section 10)");
    if (BaudRate == 0)
        return fail(ERR_INVALID_BAUDRATE, "PassThruConnect");
    {
        J_U32 pin;
        for (pin = 0; pin < 32; pin++) {
            if ((d->pins_grounded & (1u << pin)) &&
                protocol_uses_pin(ProtocolID, pin)) {
                op_err_set("PassThruConnect: pin %lu is shorted to ground",
                           (unsigned long)pin);
                return ERR_CHANNEL_IN_USE;
            }
        }
    }

    n = op_cmd_open(line, sizeof line, (unsigned)fw,
                    (uint32_t)Flags, (uint32_t)BaudRate);
    if (n == 0) return fail(ERR_FAILED, "PassThruConnect: command too long");

    rc = simple_cmd(d, line, n, NULL, 0, OP_DEFAULT_CMD_MS, NULL);
    if (rc != STATUS_NOERROR) return fail(rc, "PassThruConnect");

    /* The firmware channel number is the channel id; messages carry the
     * protocol id the caller connected with. */
    if (op_device_open_channel(d, (unsigned)fw, (uint32_t)ProtocolID,
                               (uint32_t)Flags, (uint32_t)BaudRate) != OP_OK) {
        op_err_set("PassThruConnect: no memory for the receive queue");
        return ERR_FAILED;
    }

    *pChannelID = (J_U32)fw;
    rec_line("= %d", fw);
    op_logf("connect protocol %lu baud %lu -> channel %d",
            (unsigned long)ProtocolID, (unsigned long)BaudRate, fw);
    return STATUS_NOERROR;
}

/* ---- 4. PassThruDisconnect ---------------------------------------------- */

long PassThruDisconnect(J_U32 ChannelID)
{
    op_device *d = op_device_get();
    char line[OP_CMD_MAX];
    size_t n;
    long rc;

    op_err_clear();
    rec_line("disconnect %lu", (unsigned long)ChannelID);
    if (!d->open) return fail(ERR_INVALID_DEVICE_ID, "PassThruDisconnect");
    if (channel_of(d, ChannelID) == NULL)
        return fail(ERR_INVALID_CHANNEL_ID, "PassThruDisconnect");

    /* atc stops the channel's periodic messages and drops its filters
     * (measured, PROTOCOL.md section 10). */
    n = op_cmd_close(line, sizeof line, (unsigned)ChannelID);
    rc = simple_cmd(d, line, n, NULL, 0, OP_DEFAULT_CMD_MS, NULL);

    op_device_close_channel(d, (unsigned)ChannelID);

    if (rc != STATUS_NOERROR) return fail(rc, "PassThruDisconnect");
    return STATUS_NOERROR;
}

/* ---- 5. PassThruReadMsgs ------------------------------------------------ */

long PassThruReadMsgs(J_U32 ChannelID, PASSTHRU_MSG *pMsg, J_U32 *pNumMsgs,
                      J_U32 Timeout)
{
    op_device *d = op_device_get();
    J_U32 want, got = 0;
    struct timeval start, now;

    op_err_clear();
    rec_line("read %lu %lu %lu", (unsigned long)ChannelID, pNumMsgs ? (unsigned long)*pNumMsgs : 0UL, (unsigned long)Timeout);
    if (!d->open) return fail(ERR_INVALID_DEVICE_ID, "PassThruReadMsgs");
    if (pNumMsgs == NULL)
        return fail(ERR_NULL_PARAMETER, "PassThruReadMsgs(pNumMsgs)");
    want = *pNumMsgs;
    *pNumMsgs = 0;
    if (pMsg == NULL && want > 0)
        return fail(ERR_NULL_PARAMETER, "PassThruReadMsgs(pMsg)");
    if (channel_of(d, ChannelID) == NULL)
        return fail(ERR_INVALID_CHANNEL_ID, "PassThruReadMsgs");
    if (want == 0) return STATUS_NOERROR;

    /* The caller's Timeout is honoured as given: not scaled, floored or
     * doubled. */
    gettimeofday(&start, NULL);
    for (;;) {
        unsigned remaining;
        long elapsed_ms;
        op_status st;

        st = op_device_pop(d, (unsigned)ChannelID, &pMsg[got], 0);
        if (st == OP_OK) {
            got++;
            if (got >= want) break;
            continue;
        }
        if (st != OP_ERR_TIMEOUT) goto closed;

        gettimeofday(&now, NULL);
        elapsed_ms = (long)((now.tv_sec - start.tv_sec) * 1000L +
                            (now.tv_usec - start.tv_usec) / 1000L);
        if (elapsed_ms < 0) elapsed_ms = 0;
        if ((J_U32)elapsed_ms >= Timeout) break;

        remaining = (unsigned)(Timeout - (J_U32)elapsed_ms);
        if (remaining > 20u) remaining = 20u;
        st = op_device_pop(d, (unsigned)ChannelID, &pMsg[got], remaining);
        if (st == OP_OK) {
            got++;
            if (got >= want) break;
        } else if (st != OP_ERR_TIMEOUT) {
            goto closed;
        }
    }

    *pNumMsgs = got;

    /*
     * If the queue overran while the caller was away, say so. Dropping
     * messages and returning success would be a silent loss of vehicle data —
     * the caller would have no way to know its view of the bus has a hole in
     * it. The messages that survived are still delivered in pMsg and counted
     * in pNumMsgs; a caller must consult pNumMsgs on every return code.
     */
    {
        unsigned lost = op_device_take_dropped(d, (unsigned)ChannelID);
        if (lost > 0) {
            op_err_set("PassThruReadMsgs: %u message(s) dropped; the receive "
                       "queue overran. Read more often or in larger batches.",
                       lost);
            op_logf("channel %lu overran: %u message(s) lost",
                    (unsigned long)ChannelID, lost);
            return ERR_BUFFER_OVERFLOW;
        }
    }

    if (got == 0) {
        op_err_set("PassThruReadMsgs: no messages within %lu ms",
                   (unsigned long)Timeout);
        return ERR_BUFFER_EMPTY;
    }

    /*
     * Fewer messages than asked for, with a real timeout to wait out, means
     * the deadline expired part-way. J2534 wants ERR_TIMEOUT for that, with
     * the messages that did arrive still delivered and counted — the same
     * shape as the overflow return above. Reporting success instead would
     * leave a caller unable to tell an exchange that finished early from an
     * ECU that stopped answering. A Timeout of zero asks only for whatever is
     * already queued, so a short read there is the expected outcome.
     */
    if (Timeout != 0 && got < want) {
        op_err_set("PassThruReadMsgs: %lu of %lu message(s) within %lu ms",
                   (unsigned long)got, (unsigned long)want,
                   (unsigned long)Timeout);
        return ERR_TIMEOUT;
    }
    return STATUS_NOERROR;

closed:
    /* PassThruClose ran on another thread while this call was waiting. */
    *pNumMsgs = got;
    return fail(ERR_INVALID_DEVICE_ID, "PassThruReadMsgs: device closed");
}

/* ---- 6. PassThruWriteMsgs ----------------------------------------------- */

/* The size limits of J2534-1 Figure 42, where they are certain for this
 * hardware; ISO14230's maximum depends on the checksum flag and is left to
 * the device. Checked for periodic messages too: the firmware refuses a short
 * `att` itself but runs a 3-byte `atm`, a CAN id it cannot form, every
 * interval (bench cable, 2026-09-24). */
static long msg_size_ok(op_device *d, J_U32 channel, const PASSTHRU_MSG *m)
{
    J_U32 proto = op_protocol_base(d->ch[channel].protocol);
    size_t lo = 1, hi = sizeof m->Data;
    if (proto == CAN)            { lo = 4; hi = 12; }
    else if (proto == ISO15765)  { lo = (m->TxFlags & ISO15765_ADDR_TYPE) ? 5 : 4;
                                   hi = (m->TxFlags & ISO15765_ADDR_TYPE) ? 4100 : 4099; }
    if (m->DataSize < lo || m->DataSize > hi) {
        op_err_set("message of %lu bytes is outside the %zu-%zu range this "
                   "protocol allows", (unsigned long)m->DataSize, lo, hi);
        return ERR_INVALID_MSG;
    }
    return STATUS_NOERROR;
}

static long write_one(op_device *d, J_U32 channel, const PASSTHRU_MSG *m,
                      unsigned timeout_ms)
{
    char line[OP_CMD_MAX];
    size_t n;
    uint32_t budget_us = 1000000u;

    if (m->DataSize == 0 || m->DataSize > sizeof m->Data)
        return ERR_INVALID_MSG;
    if (msg_size_ok(d, channel, m) != STATUS_NOERROR)
        return ERR_INVALID_MSG;

    /* ISO 15765-4 clause 8.1: a receiver ignores a diagnostic frame with a
     * DLC below eight, and the device pads only on ISO15765_FRAME_PAD. TxFlags
     * belong to the application; this only names the likely cause of the
     * silence the caller is about to see. */
    if (op_protocol_base(d->ch[channel].protocol) == ISO15765 &&
        !(m->TxFlags & ISO15765_FRAME_PAD) && m->DataSize < 12)
        op_logf("channel %lu: ISO15765 transmit without ISO15765_FRAME_PAD; a "
                "conforming ECU ignores a diagnostic frame with DLC < 8",
                (unsigned long)channel);

    /* The firmware's transmit budget, as the vendor DLL sets it: the caller's
     * Timeout in microseconds, one second for a Timeout of 0. */
    if (timeout_ms != 0)
        budget_us = timeout_ms > 4294967u ? 4294967295u : timeout_ms * 1000u;
    n = op_cmd_transmit(line, sizeof line, (unsigned)channel,
                        (size_t)m->DataSize, (uint32_t)m->TxFlags, budget_us);
    if (n == 0) return ERR_INVALID_MSG;

    if (timeout_ms == 0)
        return op_status_to_j2534(op_device_send_nowait(d, line, n, m->Data,
                                                        (size_t)m->DataSize));
    return simple_cmd(d, line, n, m->Data, (size_t)m->DataSize,
                      timeout_ms, NULL);
}

long PassThruWriteMsgs(J_U32 ChannelID, const PASSTHRU_MSG *pMsg,
                       J_U32 *pNumMsgs, J_U32 Timeout)
{
    op_device *d = op_device_get();
    J_U32 want, sent = 0;

    op_err_clear();
    if (rec_on()) {
        char head[64];
        snprintf(head, sizeof head, "write %lu %lu %lu", (unsigned long)ChannelID,
                 pNumMsgs ? (unsigned long)*pNumMsgs : 0UL, (unsigned long)Timeout);
        rec_msgs(head, pMsg, pNumMsgs ? *pNumMsgs : 0);
    }
    if (!d->open) return fail(ERR_INVALID_DEVICE_ID, "PassThruWriteMsgs");
    if (pNumMsgs == NULL)
        return fail(ERR_NULL_PARAMETER, "PassThruWriteMsgs(pNumMsgs)");
    want = *pNumMsgs;
    *pNumMsgs = 0;
    if (pMsg == NULL && want > 0)
        return fail(ERR_NULL_PARAMETER, "PassThruWriteMsgs(pMsg)");
    if (channel_of(d, ChannelID) == NULL)
        return fail(ERR_INVALID_CHANNEL_ID, "PassThruWriteMsgs");
    if (want == 0) return STATUS_NOERROR;

    /*
     * Timeout bounds the whole call, not each message. Handing every message
     * the full value would let a ten-message write take ten times as long as
     * the caller allowed — and on this device a transmit that gets no bus
     * acknowledgement takes over a second to fail, so the overrun is real
     * rather than theoretical.
     */
    {
        struct timeval start, now;
        gettimeofday(&start, NULL);

        while (sent < want) {
            long elapsed_ms, remaining;
            long rc;

            gettimeofday(&now, NULL);
            elapsed_ms = (long)((now.tv_sec - start.tv_sec) * 1000L +
                                (now.tv_usec - start.tv_usec) / 1000L);
            if (elapsed_ms < 0) elapsed_ms = 0;

            if (Timeout == 0) {
                /* J2534: queue the message and return at once. The transmit
                 * is numbered like any other, so its reply is recognised and
                 * dropped whenever it arrives instead of being read as the
                 * answer to the next command. */
                rc = write_one(d, ChannelID, &pMsg[sent], 0);
                if (rc != STATUS_NOERROR) {
                    *pNumMsgs = sent;
                    return fail(rc, "PassThruWriteMsgs");
                }
                sent++;
                continue;
            } else {
                remaining = (long)Timeout - elapsed_ms;
                if (remaining <= 0) {
                    *pNumMsgs = sent;
                    op_err_set("PassThruWriteMsgs: %lu of %lu sent before the "
                               "%lu ms budget expired",
                               (unsigned long)sent, (unsigned long)want,
                               (unsigned long)Timeout);
                    return ERR_TIMEOUT;
                }
            }

            rc = write_one(d, ChannelID, &pMsg[sent], (unsigned)remaining);
            if (rc != STATUS_NOERROR) {
                *pNumMsgs = sent;
                return fail(rc, "PassThruWriteMsgs");
            }
            sent++;
        }
    }
    *pNumMsgs = sent;
    return STATUS_NOERROR;
}

/* ---- 7/8. Periodic messages --------------------------------------------- */

/* Scheduled by the firmware (`atm`/`atn`), as Tactrix's DLL does: the
 * interval is kept to 0.1 ms and nothing on the host has to be awake. */
long PassThruStartPeriodicMsg(J_U32 ChannelID, const PASSTHRU_MSG *pMsg,
                              J_U32 *pMsgID, J_U32 TimeInterval)
{
    op_device *d = op_device_get();
    op_channel *c;
    char line[OP_CMD_MAX];
    size_t n;
    op_reply r;
    long rc;

    op_err_clear();
    if (rec_on()) {
        char head[64];
        snprintf(head, sizeof head, "startp %lu %lu", (unsigned long)ChannelID, (unsigned long)TimeInterval);
        rec_msgs(head, pMsg, 1);
    }
    if (!d->open) return fail(ERR_INVALID_DEVICE_ID, "PassThruStartPeriodicMsg");
    if (pMsg == NULL || pMsgID == NULL)
        return fail(ERR_NULL_PARAMETER, "PassThruStartPeriodicMsg");
    c = channel_of(d, ChannelID);
    if (c == NULL)
        return fail(ERR_INVALID_CHANNEL_ID, "PassThruStartPeriodicMsg");
    /* Any interval the firmware's microsecond field can carry. J2534 asks for
     * 5-65535 ms; Tactrix's DLL forwards 4 ms and 65 536 s alike and the
     * firmware runs them (1 ms held, PROTOCOL.md section 10). */
    if (TimeInterval > 0xFFFFFFFFu / 1000u)
        return fail(ERR_INVALID_TIME_INTERVAL, "PassThruStartPeriodicMsg");
    if (pMsg->DataSize == 0 || pMsg->DataSize > sizeof pMsg->Data ||
        msg_size_ok(d, ChannelID, pMsg) != STATUS_NOERROR)
        return fail(ERR_INVALID_MSG, "PassThruStartPeriodicMsg");
    if (c->nperiodic >= OP_PERIODIC_PER_CH)
        return fail(ERR_EXCEEDED_LIMIT, "PassThruStartPeriodicMsg");

    n = op_cmd_periodic_start(line, sizeof line, (unsigned)ChannelID,
                              (uint32_t)TimeInterval * 1000u,
                              (uint32_t)pMsg->TxFlags, (size_t)pMsg->DataSize);
    if (n == 0) return fail(ERR_INVALID_MSG, "PassThruStartPeriodicMsg");

    rc = simple_cmd(d, line, n, pMsg->Data, (size_t)pMsg->DataSize,
                    OP_DEFAULT_CMD_MS, &r);
    if (rc != STATUS_NOERROR) return fail(rc, "PassThruStartPeriodicMsg");
    if (r.kind != OP_REPLY_PERIODIC)
        return fail(ERR_FAILED, "PassThruStartPeriodicMsg: unexpected reply");

    c->periodic[c->nperiodic++] = r.a;
    *pMsgID = r.a;
    rec_line("= %lu", (unsigned long)r.a);
    op_logf("periodic %lu started on channel %lu every %lu ms",
            (unsigned long)r.a, (unsigned long)ChannelID, (unsigned long)TimeInterval);
    return STATUS_NOERROR;
}

long PassThruStopPeriodicMsg(J_U32 ChannelID, J_U32 MsgID)
{
    op_device *d = op_device_get();
    op_channel *c;
    unsigned i;
    long rc;

    op_err_clear();
    rec_line("stopp %lu %lu", (unsigned long)ChannelID, (unsigned long)MsgID);
    if (!d->open) return fail(ERR_INVALID_DEVICE_ID, "PassThruStopPeriodicMsg");
    c = channel_of(d, ChannelID);
    if (c == NULL)
        return fail(ERR_INVALID_CHANNEL_ID, "PassThruStopPeriodicMsg");
    for (i = 0; i < c->nperiodic && c->periodic[i] != (uint32_t)MsgID; i++)
        ;
    if (i == c->nperiodic)
        return fail(ERR_INVALID_MSG_ID, "PassThruStopPeriodicMsg");

    rc = periodic_stop(d, c, ChannelID, i);
    if (rc != STATUS_NOERROR) return fail(rc, "PassThruStopPeriodicMsg");
    return STATUS_NOERROR;
}

/* ---- 9. PassThruStartMsgFilter ------------------------------------------ */

long PassThruStartMsgFilter(J_U32 ChannelID, J_U32 FilterType,
                            const PASSTHRU_MSG *pMaskMsg,
                            const PASSTHRU_MSG *pPatternMsg,
                            const PASSTHRU_MSG *pFlowControlMsg,
                            J_U32 *pFilterID)
{
    op_device *d = op_device_get();
    unsigned need;
    uint8_t payload[3 * J2534_MSG_DATA_MAX];
    size_t used = 0, each;
    char line[OP_CMD_MAX];
    size_t n;
    op_reply r;
    long rc;
    const PASSTHRU_MSG *msgs[3];
    unsigned i;

    op_err_clear();
    if (rec_on()) {
        fprintf(g_rec, "filter %lu %lu", (unsigned long)ChannelID, (unsigned long)FilterType);
        rec_msg(pMaskMsg); rec_msg(pPatternMsg); rec_msg(pFlowControlMsg);
        fputc('\n', g_rec);
    }
    if (!d->open) return fail(ERR_INVALID_DEVICE_ID, "PassThruStartMsgFilter");
    if (channel_of(d, ChannelID) == NULL)
        return fail(ERR_INVALID_CHANNEL_ID, "PassThruStartMsgFilter");
    if (pFilterID == NULL)
        return fail(ERR_NULL_PARAMETER, "PassThruStartMsgFilter(pFilterID)");

    need = op_filter_msg_count((uint32_t)FilterType);
    if (need == 0)
        return fail(ERR_INVALID_MSG, "PassThruStartMsgFilter: filter type");

    /* Checked before any dereference: a caller passing NULL must get
     * ERR_NULL_PARAMETER, not a crash inside the driver. */
    if (pMaskMsg == NULL || pPatternMsg == NULL)
        return fail(ERR_NULL_PARAMETER, "PassThruStartMsgFilter(mask/pattern)");
    if (need == 3 && pFlowControlMsg == NULL)
        return fail(ERR_NULL_PARAMETER, "PassThruStartMsgFilter(flow control)");
    /* A flow-control message given with a PASS or BLOCK filter is ignored, as
     * Tactrix's DLL ignores it (tools/ab-official, 2026-09-24). J2534-1
     * DEC2004 section 7.2.9.2 calls it an error, but refusing it failed
     * applications that pass a zeroed message instead of NULL. */
    if (need == 2)
        pFlowControlMsg = NULL;

    msgs[0] = pMaskMsg;
    msgs[1] = pPatternMsg;
    msgs[2] = pFlowControlMsg;

    each = (size_t)pMaskMsg->DataSize;
    if (each == 0 || each > 255)
        return fail(ERR_INVALID_MSG, "PassThruStartMsgFilter: mask length");

    /* The device takes ONE length and applies it to every appended message,
     * so they must agree; a mismatch would silently shift the pattern. */
    for (i = 0; i < need; i++) {
        if ((size_t)msgs[i]->DataSize != each)
            return fail(ERR_INVALID_MSG,
                        "PassThruStartMsgFilter: message lengths differ");
        if (each > sizeof payload - used)
            return fail(ERR_EXCEEDED_LIMIT, "PassThruStartMsgFilter");
        memcpy(payload + used, msgs[i]->Data, each);
        used += each;
    }

    /*
     * The device takes one TxFlags for the filter, and for a flow-control
     * filter the flow-control message is the one it will actually transmit —
     * so that message's flags are the ones that govern the frames going out.
     * Taking them from the mask, which is never transmitted, meant an
     * application that set ISO15765_FRAME_PAD where the standard puts it (on
     * the flow-control message) got unpadded flow-control frames. ISO 15765-4
     * clause 8.1 requires an ECU to ignore those, so every segmented receive
     * would stall after the first frame with no error reported anywhere.
     * PASS and BLOCK filters transmit nothing, so the mask's flags stand.
     */
    n = op_cmd_filter(line, sizeof line, (unsigned)ChannelID, (uint32_t)FilterType,
                      (uint32_t)(pFlowControlMsg != NULL ? pFlowControlMsg->TxFlags
                                                         : pMaskMsg->TxFlags), each);
    if (n == 0) return fail(ERR_FAILED, "PassThruStartMsgFilter: command too long");

    rc = simple_cmd(d, line, n, payload, used, OP_DEFAULT_CMD_MS, &r);
    if (rc != STATUS_NOERROR) return fail(rc, "PassThruStartMsgFilter");

    if (r.kind != OP_REPLY_FILTER)
        return fail(ERR_FAILED, "PassThruStartMsgFilter: unexpected reply");

    *pFilterID = r.a;
    rec_line("= %lu", (unsigned long)r.a);
    op_logf("filter %lu set on channel %lu", (unsigned long)r.a,
            (unsigned long)ChannelID);
    return STATUS_NOERROR;
}

/* ---- 10. PassThruStopMsgFilter ------------------------------------------ */

long PassThruStopMsgFilter(J_U32 ChannelID, J_U32 FilterID)
{
    op_device *d = op_device_get();
    char line[OP_CMD_MAX];
    size_t n;
    long rc;

    op_err_clear();
    rec_line("stopf %lu %lu", (unsigned long)ChannelID, (unsigned long)FilterID);
    if (!d->open) return fail(ERR_INVALID_DEVICE_ID, "PassThruStopMsgFilter");
    if (channel_of(d, ChannelID) == NULL)
        return fail(ERR_INVALID_CHANNEL_ID, "PassThruStopMsgFilter");

    n = op_cmd_stop_filter(line, sizeof line, (unsigned)ChannelID,
                           (uint32_t)FilterID);
    rc = simple_cmd(d, line, n, NULL, 0, OP_DEFAULT_CMD_MS, NULL);
    if (rc != STATUS_NOERROR) return fail(rc, "PassThruStopMsgFilter");
    return STATUS_NOERROR;
}

/* ---- 11. PassThruSetProgrammingVoltage ---------------------------------- */

long PassThruSetProgrammingVoltage(J_U32 DeviceID, J_U32 PinNumber, J_U32 Voltage)
{
    op_device *d = op_device_get();
    char line[OP_CMD_MAX];
    int n;

    op_err_clear();
    rec_line("progv %lu %lu %lu", (unsigned long)DeviceID, (unsigned long)PinNumber, (unsigned long)Voltage);
    if (!d->open || DeviceID != OP_DEVICE_ID)
        return fail(ERR_INVALID_DEVICE_ID, "PassThruSetProgrammingVoltage");

    /* J2534-1 section 7.2.11: one pin at a time, ground excepted. On this
     * cable every voltage pin is fed from one supply, so a second pin would
     * silently move the first (measured 2026-09-16). */
    if (Voltage != VOLTAGE_OFF && Voltage != SHORT_TO_GROUND && PinNumber < 32 &&
        (d->pins_powered & ~(1u << PinNumber)) != 0) {
        op_err_set("PassThruSetProgrammingVoltage: another pin already has "
                   "voltage, and all pins share one supply; switch it off first");
        return ERR_PIN_INVALID;
    }
    if (Voltage == SHORT_TO_GROUND) {
        unsigned i;
        for (i = 0; i < OP_MAX_CHANNELS; i++) {
            if (d->ch[i].open &&
                protocol_uses_pin(d->ch[i].protocol, PinNumber)) {
                op_err_set("PassThruSetProgrammingVoltage: pin %lu carries "
                           "open channel %u", (unsigned long)PinNumber, i);
                return ERR_CHANNEL_IN_USE;
            }
        }
    }

    /* Printed signed, as Tactrix's DLL sends it: VOLTAGE_OFF and
     * SHORT_TO_GROUND go out as -1 and -2, the form the vendor exercises on
     * every firmware. 1.17.4877 also accepts the unsigned form. */
    n = snprintf(line, sizeof line, "atv %lu %ld\r\n",
                 (unsigned long)PinNumber, (long)(int32_t)(uint32_t)Voltage);
    if (n < 0 || (size_t)n >= sizeof line)
        return fail(ERR_FAILED, "PassThruSetProgrammingVoltage");

    op_logf("programming voltage: pin %lu <- %lu mV",
            (unsigned long)PinNumber, (unsigned long)Voltage);
    {
        long rc = simple_cmd(d, line, (size_t)n, NULL, 0, OP_DEFAULT_CMD_MS, NULL);
        if (rc != STATUS_NOERROR)
            return fail(rc, "PassThruSetProgrammingVoltage");
    }
    if (PinNumber < 32) {
        uint32_t bit = 1u << PinNumber;
        d->pins_grounded &= ~bit;
        d->pins_powered  &= ~bit;
        if (Voltage == SHORT_TO_GROUND)  d->pins_grounded |= bit;
        else if (Voltage != VOLTAGE_OFF) d->pins_powered  |= bit;
    }
    return STATUS_NOERROR;
}

/* ---- 12. PassThruReadVersion -------------------------------------------- */

long PassThruReadVersion(J_U32 DeviceID, char *pFirmwareVersion,
                         char *pDllVersion, char *pApiVersion)
{
    op_device *d = op_device_get();

    op_err_clear();
    rec_line("version %lu", (unsigned long)DeviceID);
    if (!d->open || DeviceID != OP_DEVICE_ID)
        return fail(ERR_INVALID_DEVICE_ID, "PassThruReadVersion");
    if (pFirmwareVersion == NULL || pDllVersion == NULL || pApiVersion == NULL)
        return fail(ERR_NULL_PARAMETER, "PassThruReadVersion");

    /* J2534 specifies 80-byte caller buffers for all three. */
    snprintf(pFirmwareVersion, 80, "%s",
             d->fw_version[0] ? d->fw_version : "unknown");
    snprintf(pDllVersion, 80, "%s", OP_DLL_VERSION);
    snprintf(pApiVersion, 80, "%s", OP_API_VERSION);
    return STATUS_NOERROR;
}

/* ---- 13. PassThruGetLastError ------------------------------------------- */

long PassThruGetLastError(char *pErrorDescription)
{
    rec_line("lasterr");
    if (pErrorDescription == NULL) return ERR_NULL_PARAMETER;
    snprintf(pErrorDescription, OP_ERR_TEXT_MAX, "%s", op_err_get());
    return STATUS_NOERROR;
}

/* ---- 14. PassThruIoctl -------------------------------------------------- */

/*
 * GET_CONFIG and SET_CONFIG go through the whole list and return the status
 * of its last parameter, as Tactrix's DLL does (tools/ab-official, 2026-09-24:
 * [3, 127] -> ERR_NOT_SUPPORTED, [127, 3] -> 0, every parameter sent). An
 * application written against it that lists a parameter this firmware lacks
 * still gets the rest applied; stopping at the first failure left them unset
 * and turned its success into an error. An earlier failure goes to the log.
 */
static long ioctl_get_config(op_device *d, J_U32 ChannelID, SCONFIG_LIST *list)
{
    J_U32 i;
    long rc = STATUS_NOERROR;
    if (list == NULL || (list->NumOfParams > 0 && list->ConfigPtr == NULL))
        return ERR_NULL_PARAMETER;

    for (i = 0; i < list->NumOfParams; i++) {
        char line[OP_CMD_MAX];
        size_t n;
        op_reply r;

        n = op_cmd_get_config(line, sizeof line, (unsigned)ChannelID,
                              (uint32_t)list->ConfigPtr[i].Parameter);
        rc = simple_cmd(d, line, n, NULL, 0, OP_DEFAULT_CMD_MS, &r);
        if (rc == STATUS_NOERROR && r.kind != OP_REPLY_CONFIG) rc = ERR_FAILED;
        if (rc == STATUS_NOERROR) list->ConfigPtr[i].Value = r.b;
        else op_logf("GET_CONFIG parameter %lu: %s", (unsigned long)list->ConfigPtr[i].Parameter, op_err_name(rc));
    }
    return rc;
}

static long ioctl_set_config(op_device *d, J_U32 ChannelID,
                             const SCONFIG_LIST *list)
{
    J_U32 i;
    long rc = STATUS_NOERROR;
    if (list == NULL || (list->NumOfParams > 0 && list->ConfigPtr == NULL))
        return ERR_NULL_PARAMETER;

    for (i = 0; i < list->NumOfParams; i++) {
        char line[OP_CMD_MAX];
        size_t n;

        n = op_cmd_set_config(line, sizeof line, (unsigned)ChannelID,
                              (uint32_t)list->ConfigPtr[i].Parameter,
                              (uint32_t)list->ConfigPtr[i].Value);
        rc = simple_cmd(d, line, n, NULL, 0, OP_DEFAULT_CMD_MS, NULL);
        if (rc != STATUS_NOERROR)
            op_logf("SET_CONFIG parameter %lu = %lu: %s", (unsigned long)list->ConfigPtr[i].Parameter,
                    (unsigned long)list->ConfigPtr[i].Value, op_err_name(rc));
    }
    return rc;
}

static long ioctl_read_pin(op_device *d, unsigned pin, J_U32 *out)
{
    char line[OP_CMD_MAX];
    size_t n;
    op_reply r;
    long rc;

    if (out == NULL) return ERR_NULL_PARAMETER;
    n = op_cmd_read_pin(line, sizeof line, pin);
    rc = simple_cmd(d, line, n, NULL, 0, OP_DEFAULT_CMD_MS, &r);
    if (rc != STATUS_NOERROR) return rc;
    if (r.kind != OP_REPLY_PIN) return ERR_FAILED;
    *out = r.b;
    return STATUS_NOERROR;
}

/*
 * FIVE_BAUD_INIT and FAST_INIT. J2534 gives them different argument types:
 * five-baud takes an SBYTE_ARRAY holding the address and returns the keybytes
 * in another; fast init takes the StartCommunication request as a PASSTHRU_MSG
 * and returns the ECU's response as one. The wire form is the one two
 * independent readings of Tactrix's DLL agree on (PROTOCOL.md §4).
 */
static long ioctl_init(op_device *d, J_U32 ChannelID, int five_baud,
                       const void *pInput, void *pOutput)
{
    char line[OP_CMD_MAX];
    const uint8_t *payload = NULL;
    size_t payload_len = 0, n;
    long rc;
    op_reply r;

    if (five_baud) {
        const SBYTE_ARRAY *in = (const SBYTE_ARRAY *)pInput;
        if (in == NULL || in->BytePtr == NULL || in->NumOfBytes < 1)
            return ERR_NULL_PARAMETER;
        n = op_cmd_five_baud(line, sizeof line, (unsigned)ChannelID, in->BytePtr[0]);
    } else {
        /* J2534 defines a NULL input to FAST_INIT as "no message
         * transmitted" — the wake-up pattern alone. Refusing it denied a
         * capability the firmware has: the device answers `aty<ch> 0 0`. */
        const PASSTHRU_MSG *in = (const PASSTHRU_MSG *)pInput;
        if (in == NULL) {
            payload = NULL;
            payload_len = 0;
        } else {
            if (in->DataSize > 255) return ERR_INVALID_MSG;
            payload = in->Data;
            payload_len = (size_t)in->DataSize;
        }
        n = op_cmd_fast_init(line, sizeof line, (unsigned)ChannelID, payload_len);
    }
    if (n == 0) return ERR_FAILED;

    rc = simple_cmd(d, line, n, payload, payload_len, 5000u, &r);
    if (rc != STATUS_NOERROR) return rc;   /* K-line init is slow by design */

    /*
     * An initialisation has only happened if the ECU answered it: five-baud
     * returns key bytes, fast init the StartCommunication response. The device
     * signals that with `arw<ch> <b> <b>` (five-baud, the key bytes in
     * decimal on the line) or `ary<ch> <n>` and n raw bytes (fast init) —
     * PROTOCOL.md section 3. A bare `aro` means the
     * command was accepted and nothing came back — reporting success for that
     * would tell a caller a dead K-line had woken up, which is precisely the
     * class of lie this driver exists to avoid, and what it did until now.
     * On failure the caller's output is left untouched.
     */
    if (r.kind != OP_REPLY_INIT || r.data_len == 0) {
        op_err_set("K-line init: no answer from the ECU (device %s)",
                   r.kind == OP_REPLY_INIT ? "returned an empty result"
                                           : "acknowledged without initialising");
        return ERR_INIT_FAILED;
    }

    if (five_baud) {
        SBYTE_ARRAY *out = (SBYTE_ARRAY *)pOutput;
        if (out != NULL && out->BytePtr != NULL) {
            size_t take = r.data_len;
            if (take > out->NumOfBytes) take = out->NumOfBytes;
            memcpy(out->BytePtr, r.data, take);
            out->NumOfBytes = (J_U32)take;
        }
    } else {
        PASSTHRU_MSG *out = (PASSTHRU_MSG *)pOutput;
        if (out != NULL) {
            size_t take = r.data_len;
            memset(out, 0, sizeof *out);
            out->ProtocolID = d->ch[ChannelID].protocol;
            if (take > sizeof out->Data) take = sizeof out->Data;
            memcpy(out->Data, r.data, take);
            out->DataSize = (J_U32)take;
            out->ExtraDataIndex = out->DataSize;
        }
    }
    return STATUS_NOERROR;
}

long PassThruIoctl(J_U32 ChannelID, J_U32 IoctlID, const void *pInput,
                   void *pOutput)
{
    op_device *d = op_device_get();
    char line[OP_CMD_MAX];
    size_t n;

    op_err_clear();
    if (rec_on()) {
        fprintf(g_rec, "ioctl %lu %lu", (unsigned long)ChannelID, (unsigned long)IoctlID);
        if ((IoctlID == SET_CONFIG || IoctlID == GET_CONFIG) && pInput != NULL) {
            const SCONFIG_LIST *l = (const SCONFIG_LIST *)pInput;
            J_U32 i;
            for (i = 0; l->ConfigPtr != NULL && i < l->NumOfParams && i < 32; i++)
                fprintf(g_rec, " %lu=%lu", (unsigned long)l->ConfigPtr[i].Parameter,
                        IoctlID == SET_CONFIG ? (unsigned long)l->ConfigPtr[i].Value : 0UL);
        }
        fputc('\n', g_rec);
    }
    if (!d->open) return fail(ERR_INVALID_DEVICE_ID, "PassThruIoctl");

    switch (IoctlID) {

    case GET_CONFIG:
        if (channel_of(d, ChannelID) == NULL)
            return fail(ERR_INVALID_CHANNEL_ID, "PassThruIoctl(GET_CONFIG)");
        /* J2534 declares pInput const, yet GET_CONFIG is defined to write
         * each Value back into the caller's SCONFIG list. The cast is the
         * standard's contradiction, not a discarded qualifier of ours. */
        {
            long rc = ioctl_get_config(d, ChannelID, (SCONFIG_LIST *)(uintptr_t)pInput);
            return rc == STATUS_NOERROR ? rc : fail(rc, "PassThruIoctl(GET_CONFIG)");
        }

    case SET_CONFIG:
        if (channel_of(d, ChannelID) == NULL)
            return fail(ERR_INVALID_CHANNEL_ID, "PassThruIoctl(SET_CONFIG)");
        {
            long rc = ioctl_set_config(d, ChannelID, (const SCONFIG_LIST *)pInput);
            return rc == STATUS_NOERROR ? rc : fail(rc, "PassThruIoctl(SET_CONFIG)");
        }

    case READ_VBATT:
        /* Device-scoped, so it is valid before any channel exists. */
        return ioctl_read_pin(d, OP_PIN_VBATT, (J_U32 *)pOutput);

    case READ_PROG_VOLTAGE:
        /* J2534-1 passes NULL. Tactrix's DLL reads the pin number through
         * pInput and sends `atr <pin>` (measured under emulation, 2026-09-16);
         * without it the vendor returns -1 and touches nothing. */
        return ioctl_read_pin(d, pInput != NULL ? (unsigned)*(const J_U32 *)pInput
                                                : OP_PIN_PROG_VOLTAGE,
                              (J_U32 *)pOutput);

    case FIVE_BAUD_INIT:
        if (channel_of(d, ChannelID) == NULL)
            return fail(ERR_INVALID_CHANNEL_ID, "PassThruIoctl(FIVE_BAUD_INIT)");
        {
            long rc = ioctl_init(d, ChannelID, 1, pInput, pOutput);
            return rc == STATUS_NOERROR ? rc : fail(rc, "PassThruIoctl(FIVE_BAUD_INIT)");
        }

    case FAST_INIT:
        if (channel_of(d, ChannelID) == NULL)
            return fail(ERR_INVALID_CHANNEL_ID, "PassThruIoctl(FAST_INIT)");
        {
            long rc = ioctl_init(d, ChannelID, 0, pInput, pOutput);
            return rc == STATUS_NOERROR ? rc : fail(rc, "PassThruIoctl(FAST_INIT)");
        }

    case CLEAR_TX_BUFFER:
    case CLEAR_RX_BUFFER:
        /* Host-side only, as the vendor DLL does it: the queue the
         * application reads from lives here. */
        if (channel_of(d, ChannelID) == NULL)
            return fail(ERR_INVALID_CHANNEL_ID, "PassThruIoctl(CLEAR_*_BUFFER)");
        op_device_flush_channel(d, (unsigned)ChannelID);
        return STATUS_NOERROR;

    case CLEAR_PERIODIC_MSGS: {
        op_channel *c = channel_of(d, ChannelID);
        long rc;
        if (c == NULL)
            return fail(ERR_INVALID_CHANNEL_ID, "PassThruIoctl(CLEAR_PERIODIC_MSGS)");
        n = op_cmd_clear_periodic(line, sizeof line, (unsigned)ChannelID);
        rc = simple_cmd(d, line, n, NULL, 0, OP_DEFAULT_CMD_MS, NULL);
        if (rc != STATUS_NOERROR) return fail(rc, "PassThruIoctl(CLEAR_PERIODIC_MSGS)");
        c->nperiodic = 0;
        return STATUS_NOERROR;
    }

    case CLEAR_MSG_FILTERS: {
        long rc;
        if (channel_of(d, ChannelID) == NULL)
            return fail(ERR_INVALID_CHANNEL_ID, "PassThruIoctl(CLEAR_MSG_FILTERS)");
        /* One command for all of them. The firmware never reuses a filter id
         * within a session, so ids outgrow any fixed table the host could
         * keep (tools/ab-official, 2026-09-24). */
        n = op_cmd_clear_filters(line, sizeof line, (unsigned)ChannelID);
        rc = simple_cmd(d, line, n, NULL, 0, OP_DEFAULT_CMD_MS, NULL);
        if (rc != STATUS_NOERROR) return fail(rc, "PassThruIoctl(CLEAR_MSG_FILTERS)");
        return STATUS_NOERROR;
    }

    /* The functional-message lookup table is a J1850 facility. This device
     * does not implement J1850 at all — it rejects protocols 1 and 2 — so
     * claiming support would be a lie the caller cannot detect. */
    case CLEAR_FUNCT_MSG_LOOKUP_TABLE:
    case ADD_TO_FUNCT_MSG_LOOKUP_TABLE:
    case DELETE_FROM_FUNCT_MSG_LOOKUP_TABLE:
        return fail(ERR_NOT_SUPPORTED, "PassThruIoctl: functional message table");

    default:
        return fail(ERR_INVALID_IOCTL_ID, "PassThruIoctl");
    }
}
