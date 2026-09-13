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

#include <pthread.h>
#include <limits.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define OP_DEVICE_ID        1UL
#define OP_DEFAULT_CMD_MS   1000u
#define OP_MAX_PERIODIC     8

#define OP_API_VERSION      "04.04"
#define OP_DLL_VERSION      "openport-j2534 " OPENPORT_VERSION

/* Pins the firmware exposes to `atr`, established by probing every pin
 * number: everything else answers ERR_PIN_INVALID. See docs/PROTOCOL.md. */
#define OP_PIN_PROG_VOLTAGE 12
#define OP_PIN_VBATT        16

/* J2534's sentinel for "stop supplying programming voltage". */
#define OP_VOLTAGE_OFF      0xFFFFFFFFUL

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

static op_channel *channel_of(op_device *d, J_U32 id)
{
    if (!channel_valid(id)) return NULL;
    if (!d->ch[id].open) return NULL;
    return &d->ch[id];
}

/* ---- periodic messages --------------------------------------------------
 * Driven from the host. The firmware has a working periodic facility
 * (`atm`/`atn`, PROTOCOL.md section 4, measured 2026-09-13), but a keep-alive
 * that silently stops is exactly the failure this driver exists to avoid, so
 * they are scheduled here, where the behaviour is ours to guarantee. Moving
 * to `atm` is a choice, not a necessity.
 */
typedef struct {
    int          used;
    J_U32        channel;
    J_U32        interval_ms;
    PASSTHRU_MSG msg;
    struct timeval next;
    unsigned     missed;      /* periods skipped to avoid a catch-up burst */
    int          in_flight;   /* a transmit for this slot is on the wire now */
} op_periodic;

static op_periodic     g_periodic[OP_MAX_PERIODIC];
static pthread_mutex_t g_periodic_lock = PTHREAD_MUTEX_INITIALIZER;
/* Signalled when a periodic transmit finishes, so a stop can wait one out. */
static pthread_cond_t  g_periodic_idle = PTHREAD_COND_INITIALIZER;
static pthread_t       g_periodic_thread;
static int             g_periodic_running;
static atomic_int      g_periodic_stop;

static long write_one(op_device *d, J_U32 channel, const PASSTHRU_MSG *m,
                      unsigned timeout_ms);

static void tv_add_ms(struct timeval *tv, J_U32 ms)
{
    tv->tv_sec  += (time_t)(ms / 1000u);
    tv->tv_usec += (suseconds_t)((ms % 1000u) * 1000u);
    if (tv->tv_usec >= 1000000) { tv->tv_sec++; tv->tv_usec -= 1000000; }
}

static void *periodic_main(void *arg)
{
    op_device *d = (op_device *)arg;

    while (!atomic_load(&g_periodic_stop)) {
        struct timeval now;
        int i;

        gettimeofday(&now, NULL);

        pthread_mutex_lock(&g_periodic_lock);
        for (i = 0; i < OP_MAX_PERIODIC; i++) {
            op_periodic *p = &g_periodic[i];
            PASSTHRU_MSG copy;
            J_U32 ch;

            if (!p->used) continue;
            if (now.tv_sec < p->next.tv_sec ||
                (now.tv_sec == p->next.tv_sec && now.tv_usec < p->next.tv_usec))
                continue;

            copy = p->msg;
            ch   = p->channel;
            p->in_flight = 1;

            /*
             * Advance from the previous deadline, not from now, so a period is
             * the interval the caller asked for rather than the interval plus
             * however long the transmit took.
             *
             * If that puts the next deadline already in the past — a transmit
             * blocked for over a second, say — resynchronise instead of firing
             * a burst to catch up. A clump of frames put on a vehicle bus to
             * make up for lost time is worse than a skipped keep-alive.
             */
            tv_add_ms(&p->next, p->interval_ms);
            if (p->next.tv_sec < now.tv_sec ||
                (p->next.tv_sec == now.tv_sec && p->next.tv_usec < now.tv_usec)) {
                p->next = now;
                tv_add_ms(&p->next, p->interval_ms);
                if (p->missed < UINT_MAX) p->missed++;
            }
            pthread_mutex_unlock(&g_periodic_lock);

            /* Outside the periodic lock: write_one takes the device command
             * lock, and holding both would invert the order taken by
             * StopPeriodicMsg. */
            /* A CAN transmit that gets no bus acknowledgement takes over a
             * second to be rejected, so a short deadline here would time out
             * every time and generate a stream of orphan replies. */
            /* The device answers a transmit with its indication frame before
             * the acknowledgement, so the window around the command covers it. */
            op_device_quiet_tx(d, (unsigned)ch, 1);
            (void)write_one(d, ch, &copy, OP_DEFAULT_CMD_MS + 500u);
            op_device_quiet_tx(d, (unsigned)ch, 0);

            pthread_mutex_lock(&g_periodic_lock);
            p->in_flight = 0;
            pthread_cond_broadcast(&g_periodic_idle);
            /* One transmit per pass. Several periodics coming due together
             * would otherwise go out back-to-back with no spacing at all. */
            break;
        }
        pthread_mutex_unlock(&g_periodic_lock);

        usleep(2000);
    }
    return NULL;
}

/* Decided under the lock: two concurrent StartPeriodicMsg calls must not each
 * see g_periodic_running == 0 and start a second scheduler thread. The new
 * thread blocks on this same lock until we release it, which is harmless. */
static int periodic_start_thread(op_device *d)
{
    int ok = 1;
    pthread_mutex_lock(&g_periodic_lock);
    if (!g_periodic_running) {
        atomic_store(&g_periodic_stop, 0);
        if (pthread_create(&g_periodic_thread, NULL, periodic_main, d) == 0)
            g_periodic_running = 1;
        else
            ok = 0;
    }
    pthread_mutex_unlock(&g_periodic_lock);
    return ok;
}

static void periodic_stop_all(void)
{
    pthread_t thread;
    int running;
    int i;

    pthread_mutex_lock(&g_periodic_lock);
    for (i = 0; i < OP_MAX_PERIODIC; i++) g_periodic[i].used = 0;
    running = g_periodic_running;
    thread  = g_periodic_thread;
    g_periodic_running = 0;
    atomic_store(&g_periodic_stop, 1);
    pthread_mutex_unlock(&g_periodic_lock);

    /* Joined outside the lock: the scheduler thread takes it on every pass,
     * so joining while holding it would deadlock. */
    if (running) pthread_join(thread, NULL);
}

static void periodic_drop_channel(J_U32 channel)
{
    int i;
    pthread_mutex_lock(&g_periodic_lock);
    for (i = 0; i < OP_MAX_PERIODIC; i++)
        if (g_periodic[i].used && g_periodic[i].channel == channel)
            g_periodic[i].used = 0;
    /* Same reasoning as StopPeriodicMsg: do not report the channel quiet while
     * one of its frames is still on the wire. */
    for (i = 0; i < OP_MAX_PERIODIC; i++)
        while (g_periodic[i].in_flight && g_periodic[i].channel == channel)
            pthread_cond_wait(&g_periodic_idle, &g_periodic_lock);
    pthread_mutex_unlock(&g_periodic_lock);
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

    periodic_stop_all();

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

    op_err_clear();
    rec_line("connect %lu %lu %lu %lu", (unsigned long)DeviceID, (unsigned long)ProtocolID, (unsigned long)Flags, (unsigned long)BaudRate);
    if (!d->open) return fail(ERR_DEVICE_NOT_CONNECTED, "PassThruConnect");
    if (DeviceID != OP_DEVICE_ID)
        return fail(ERR_INVALID_DEVICE_ID, "PassThruConnect");
    if (pChannelID == NULL)
        return fail(ERR_NULL_PARAMETER, "PassThruConnect(pChannelID)");
    if (!channel_valid(ProtocolID))
        return fail(ERR_INVALID_PROTOCOL_ID, "PassThruConnect");
    if (d->ch[ProtocolID].open)
        return fail(ERR_CHANNEL_IN_USE, "PassThruConnect");
    if (BaudRate == 0)
        return fail(ERR_INVALID_BAUDRATE, "PassThruConnect");

    n = op_cmd_open(line, sizeof line, (unsigned)ProtocolID,
                    (uint32_t)Flags, (uint32_t)BaudRate);
    if (n == 0) return fail(ERR_FAILED, "PassThruConnect: command too long");

    rc = simple_cmd(d, line, n, NULL, 0, OP_DEFAULT_CMD_MS, NULL);
    if (rc != STATUS_NOERROR) return fail(rc, "PassThruConnect");

    /* The device names channels by protocol id — proven by opening 6, seeing
     * a second open rejected with ERR_CHANNEL_IN_USE, closing `atc6`, and
     * finding the open accepted again. So the channel id we hand back is the
     * protocol id, which is also what the device expects on every later
     * command. */
    memset(&d->ch[ProtocolID], 0, sizeof d->ch[ProtocolID]);
    d->ch[ProtocolID].open     = 1;
    d->ch[ProtocolID].protocol = (uint32_t)ProtocolID;
    d->ch[ProtocolID].flags    = (uint32_t)Flags;
    d->ch[ProtocolID].baud     = (uint32_t)BaudRate;

    *pChannelID = ProtocolID;
    rec_line("= %lu", (unsigned long)ProtocolID);
    op_logf("connect protocol %lu baud %lu -> channel %lu",
            (unsigned long)ProtocolID, (unsigned long)BaudRate,
            (unsigned long)ProtocolID);
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
    if (!d->open) return fail(ERR_DEVICE_NOT_CONNECTED, "PassThruDisconnect");
    if (channel_of(d, ChannelID) == NULL)
        return fail(ERR_INVALID_CHANNEL_ID, "PassThruDisconnect");

    periodic_drop_channel(ChannelID);

    n = op_cmd_close(line, sizeof line, (unsigned)ChannelID);
    rc = simple_cmd(d, line, n, NULL, 0, OP_DEFAULT_CMD_MS, NULL);

    d->ch[ChannelID].open = 0;
    op_device_flush_channel(d, (unsigned)ChannelID);

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
    if (!d->open) return fail(ERR_DEVICE_NOT_CONNECTED, "PassThruReadMsgs");
    if (pNumMsgs == NULL)
        return fail(ERR_NULL_PARAMETER, "PassThruReadMsgs(pNumMsgs)");
    want = *pNumMsgs;
    *pNumMsgs = 0;
    if (pMsg == NULL && want > 0)
        return fail(ERR_NULL_PARAMETER, "PassThruReadMsgs(pMsg)");
    if (channel_of(d, ChannelID) == NULL)
        return fail(ERR_INVALID_CHANNEL_ID, "PassThruReadMsgs");
    if (want == 0) return STATUS_NOERROR;

    /* The caller's Timeout is the caller's. It is not scaled, floored or
     * doubled here: a caller that asked for 50 ms gets 50 ms, and one that
     * needs longer for a slow K-line init can say so. */
    gettimeofday(&start, NULL);
    for (;;) {
        unsigned remaining;
        long elapsed_ms;

        if (op_device_pop(d, (unsigned)ChannelID, &pMsg[got], 0) == OP_OK) {
            got++;
            if (got >= want) break;
            continue;
        }

        gettimeofday(&now, NULL);
        elapsed_ms = (long)((now.tv_sec - start.tv_sec) * 1000L +
                            (now.tv_usec - start.tv_usec) / 1000L);
        if (elapsed_ms < 0) elapsed_ms = 0;
        if ((J_U32)elapsed_ms >= Timeout) break;

        remaining = (unsigned)(Timeout - (J_U32)elapsed_ms);
        if (remaining > 20u) remaining = 20u;   /* stay responsive to stop */
        if (op_device_pop(d, (unsigned)ChannelID, &pMsg[got], remaining) == OP_OK) {
            got++;
            if (got >= want) break;
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
}

/* ---- 6. PassThruWriteMsgs ----------------------------------------------- */

static long write_one(op_device *d, J_U32 channel, const PASSTHRU_MSG *m,
                      unsigned timeout_ms)
{
    char line[OP_CMD_MAX];
    size_t n;

    if (m->DataSize == 0 || m->DataSize > sizeof m->Data)
        return ERR_INVALID_MSG;

    /*
     * J2534 gives a minimum and maximum message size per protocol. Only the
     * bounds that are certain for this hardware are enforced: the minima, and
     * the maxima that follow from the wire format itself. ISO14230's maximum
     * depends on connect flags this driver does not track, so it is left
     * alone — wrongly rejecting a valid message would be worse than letting
     * the device reject it.
     */
    {
        J_U32 proto = d->ch[channel].protocol;
        size_t lo = 1, hi = sizeof m->Data;
        if (proto == CAN)            { lo = 4; hi = 12; }   /* id + up to 8 */
        else if (proto == ISO15765)  { lo = (m->TxFlags & ISO15765_ADDR_TYPE) ? 6 : 5;
                                       hi = 4100; }
        else if (proto == ISO14230)  { lo = 2; }
        else if (proto == ISO9141)   { lo = 1; }
        if (m->DataSize < lo || m->DataSize > hi) {
            op_err_set("message of %lu bytes is outside the %zu-%zu range this "
                       "protocol allows", (unsigned long)m->DataSize, lo, hi);
            return ERR_INVALID_MSG;
        }
    }

    /* ISO 15765-4 clause 8.1 requires a DLC of eight on every diagnostic CAN
     * frame and says a receiver shall ignore a shorter one. The device pads
     * only when ISO15765_FRAME_PAD is set. TxFlags belong to the application,
     * so this does not rewrite them — it records the likely cause of the
     * silence the caller is about to see. */
    if (d->ch[channel].protocol == ISO15765 &&
        !(m->TxFlags & ISO15765_FRAME_PAD) && m->DataSize < 12)
        op_logf("channel %lu: ISO15765 transmit without ISO15765_FRAME_PAD; a "
                "conforming ECU ignores a diagnostic frame with DLC < 8",
                (unsigned long)channel);

    /* The firmware's transmit budget, as the vendor DLL sets it: the caller's
     * Timeout in microseconds, one second when the caller passed 0. */
    n = op_cmd_transmit(line, sizeof line, (unsigned)channel,
                        (size_t)m->DataSize, (uint32_t)m->TxFlags,
                        timeout_ms ? (uint32_t)timeout_ms * 1000u : 1000000u);
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
    if (!d->open) return fail(ERR_DEVICE_NOT_CONNECTED, "PassThruWriteMsgs");
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

long PassThruStartPeriodicMsg(J_U32 ChannelID, const PASSTHRU_MSG *pMsg,
                              J_U32 *pMsgID, J_U32 TimeInterval)
{
    op_device *d = op_device_get();
    int i, slot = -1;

    op_err_clear();
    if (rec_on()) {
        char head[64];
        snprintf(head, sizeof head, "startp %lu %lu", (unsigned long)ChannelID, (unsigned long)TimeInterval);
        rec_msgs(head, pMsg, 1);
    }
    if (!d->open) return fail(ERR_DEVICE_NOT_CONNECTED, "PassThruStartPeriodicMsg");
    if (pMsg == NULL || pMsgID == NULL)
        return fail(ERR_NULL_PARAMETER, "PassThruStartPeriodicMsg");
    if (channel_of(d, ChannelID) == NULL)
        return fail(ERR_INVALID_CHANNEL_ID, "PassThruStartPeriodicMsg");
    /* J2534 bounds the interval at 5..65535 ms. */
    if (TimeInterval < 5 || TimeInterval > 65535)
        return fail(ERR_INVALID_TIME_INTERVAL, "PassThruStartPeriodicMsg");
    if (pMsg->DataSize == 0 || pMsg->DataSize > sizeof pMsg->Data)
        return fail(ERR_INVALID_MSG, "PassThruStartPeriodicMsg");

    pthread_mutex_lock(&g_periodic_lock);
    for (i = 0; i < OP_MAX_PERIODIC; i++)
        if (!g_periodic[i].used) { slot = i; break; }

    if (slot < 0) {
        pthread_mutex_unlock(&g_periodic_lock);
        return fail(ERR_EXCEEDED_LIMIT, "PassThruStartPeriodicMsg");
    }

    g_periodic[slot].used        = 1;
    g_periodic[slot].channel     = ChannelID;
    g_periodic[slot].interval_ms = TimeInterval;
    g_periodic[slot].msg         = *pMsg;
    g_periodic[slot].missed      = 0;
    gettimeofday(&g_periodic[slot].next, NULL);
    pthread_mutex_unlock(&g_periodic_lock);

    if (!periodic_start_thread(d)) {
        pthread_mutex_lock(&g_periodic_lock);
        g_periodic[slot].used = 0;
        pthread_mutex_unlock(&g_periodic_lock);
        return fail(ERR_FAILED, "PassThruStartPeriodicMsg: no thread");
    }

    *pMsgID = (J_U32)slot + 1u;
    rec_line("= %lu", (unsigned long)slot + 1UL);
    op_logf("periodic %d started on channel %lu every %lu ms",
            slot, (unsigned long)ChannelID, (unsigned long)TimeInterval);
    return STATUS_NOERROR;
}

long PassThruStopPeriodicMsg(J_U32 ChannelID, J_U32 MsgID)
{
    op_device *d = op_device_get();
    int slot;

    op_err_clear();
    rec_line("stopp %lu %lu", (unsigned long)ChannelID, (unsigned long)MsgID);
    if (!d->open) return fail(ERR_DEVICE_NOT_CONNECTED, "PassThruStopPeriodicMsg");
    if (channel_of(d, ChannelID) == NULL)
        return fail(ERR_INVALID_CHANNEL_ID, "PassThruStopPeriodicMsg");
    if (MsgID == 0 || MsgID > OP_MAX_PERIODIC)
        return fail(ERR_INVALID_MSG_ID, "PassThruStopPeriodicMsg");

    slot = (int)MsgID - 1;
    pthread_mutex_lock(&g_periodic_lock);
    if (!g_periodic[slot].used || g_periodic[slot].channel != ChannelID) {
        pthread_mutex_unlock(&g_periodic_lock);
        return fail(ERR_INVALID_MSG_ID, "PassThruStopPeriodicMsg");
    }
    g_periodic[slot].used = 0;
    /*
     * Marking the slot unused stops future sends, but the scheduler releases
     * the periodic lock before transmitting — it must, or it would take the
     * device command lock in the opposite order to everyone else. So a frame
     * for this message may already be on the wire. Returning now would tell
     * the caller nothing is transmitting while a frame is still going out, and
     * a caller that then closes the channel or unplugs would be wrong about
     * the state of the bus. Wait the in-flight transmit out.
     */
    while (g_periodic[slot].in_flight)
        pthread_cond_wait(&g_periodic_idle, &g_periodic_lock);
    pthread_mutex_unlock(&g_periodic_lock);
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
    if (!d->open) return fail(ERR_DEVICE_NOT_CONNECTED, "PassThruStartMsgFilter");
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
    /* J2534-1 DEC2004 section 7.2.9.2: a flow-control message supplied for a
     * PASS or BLOCK filter is an error, not something to ignore. Accepting it
     * silently would let a caller believe flow control had been configured on
     * a filter type that has none. */
    if (need == 2 && pFlowControlMsg != NULL)
        return fail(ERR_INVALID_MSG,
                    "PassThruStartMsgFilter: flow control on a PASS/BLOCK filter");

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
    d->ch[ChannelID].filters |= (1u << (r.a & 31u));
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
    if (!d->open) return fail(ERR_DEVICE_NOT_CONNECTED, "PassThruStopMsgFilter");
    if (channel_of(d, ChannelID) == NULL)
        return fail(ERR_INVALID_CHANNEL_ID, "PassThruStopMsgFilter");

    n = op_cmd_stop_filter(line, sizeof line, (unsigned)ChannelID,
                           (uint32_t)FilterID);
    rc = simple_cmd(d, line, n, NULL, 0, OP_DEFAULT_CMD_MS, NULL);
    if (rc != STATUS_NOERROR) return fail(rc, "PassThruStopMsgFilter");

    d->ch[ChannelID].filters &= ~(1u << (FilterID & 31u));
    return STATUS_NOERROR;
}

/* ---- 11. PassThruSetProgrammingVoltage ---------------------------------- */

long PassThruSetProgrammingVoltage(J_U32 DeviceID, J_U32 PinNumber, J_U32 Voltage)
{
    op_device *d = op_device_get();
    char line[OP_CMD_MAX];
    int n;
    const char *gate;

    op_err_clear();
    rec_line("progv %lu %lu %lu", (unsigned long)DeviceID, (unsigned long)PinNumber, (unsigned long)Voltage);
    if (!d->open)
        return fail(ERR_DEVICE_NOT_CONNECTED, "PassThruSetProgrammingVoltage");
    if (DeviceID != OP_DEVICE_ID)
        return fail(ERR_INVALID_DEVICE_ID, "PassThruSetProgrammingVoltage");

    /* This energises a pin on the vehicle connector. It is real and it works,
     * but an application that calls it by accident can put voltage on a pin
     * that is wired to something else, so applying voltage requires a
     * deliberate opt-in. Turning it OFF is always allowed. */
    gate = getenv("OPENPORT_ENABLE_PROG_VOLTAGE");
    if (Voltage != OP_VOLTAGE_OFF && (gate == NULL || gate[0] != '1')) {
        op_err_set("Programming voltage is gated; set "
                   "OPENPORT_ENABLE_PROG_VOLTAGE=1");
        op_logf("refused programming voltage %lu mV on pin %lu: gate not set",
                (unsigned long)Voltage, (unsigned long)PinNumber);
        return ERR_NOT_SUPPORTED;
    }

    n = snprintf(line, sizeof line, "atv %lu %lu\r\n",
                 (unsigned long)PinNumber, (unsigned long)Voltage);
    if (n < 0 || (size_t)n >= sizeof line)
        return fail(ERR_FAILED, "PassThruSetProgrammingVoltage");

    op_logf("programming voltage: pin %lu <- %lu mV",
            (unsigned long)PinNumber, (unsigned long)Voltage);
    {
        long rc = simple_cmd(d, line, (size_t)n, NULL, 0, OP_DEFAULT_CMD_MS, NULL);
        if (rc != STATUS_NOERROR)
            return fail(rc, "PassThruSetProgrammingVoltage");
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
    if (!d->open) return fail(ERR_DEVICE_NOT_CONNECTED, "PassThruReadVersion");
    if (DeviceID != OP_DEVICE_ID)
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

static long ioctl_get_config(op_device *d, J_U32 ChannelID, SCONFIG_LIST *list)
{
    J_U32 i;
    if (list == NULL || (list->NumOfParams > 0 && list->ConfigPtr == NULL))
        return ERR_NULL_PARAMETER;

    for (i = 0; i < list->NumOfParams; i++) {
        char line[OP_CMD_MAX];
        size_t n;
        op_reply r;
        long rc;

        n = op_cmd_get_config(line, sizeof line, (unsigned)ChannelID,
                              (uint32_t)list->ConfigPtr[i].Parameter);
        rc = simple_cmd(d, line, n, NULL, 0, OP_DEFAULT_CMD_MS, &r);
        if (rc != STATUS_NOERROR) return rc;
        if (r.kind != OP_REPLY_CONFIG) return ERR_FAILED;
        list->ConfigPtr[i].Value = r.b;
    }
    return STATUS_NOERROR;
}

static long ioctl_set_config(op_device *d, J_U32 ChannelID,
                             const SCONFIG_LIST *list)
{
    J_U32 i;
    if (list == NULL || (list->NumOfParams > 0 && list->ConfigPtr == NULL))
        return ERR_NULL_PARAMETER;

    for (i = 0; i < list->NumOfParams; i++) {
        char line[OP_CMD_MAX];
        size_t n;
        long rc;

        n = op_cmd_set_config(line, sizeof line, (unsigned)ChannelID,
                              (uint32_t)list->ConfigPtr[i].Parameter,
                              (uint32_t)list->ConfigPtr[i].Value);
        rc = simple_cmd(d, line, n, NULL, 0, OP_DEFAULT_CMD_MS, NULL);
        if (rc != STATUS_NOERROR) return rc;
    }
    return STATUS_NOERROR;
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
    if (!d->open) return fail(ERR_DEVICE_NOT_CONNECTED, "PassThruIoctl");

    switch (IoctlID) {

    case GET_CONFIG:
        if (channel_of(d, ChannelID) == NULL)
            return fail(ERR_INVALID_CHANNEL_ID, "PassThruIoctl(GET_CONFIG)");
        /* J2534 declares pInput const, yet GET_CONFIG is defined to write
         * each Value back into the caller's SCONFIG list. The cast is the
         * standard's contradiction, not a discarded qualifier of ours. */
        return ioctl_get_config(d, ChannelID, (SCONFIG_LIST *)(uintptr_t)pInput);

    case SET_CONFIG:
        if (channel_of(d, ChannelID) == NULL)
            return fail(ERR_INVALID_CHANNEL_ID, "PassThruIoctl(SET_CONFIG)");
        return ioctl_set_config(d, ChannelID, (const SCONFIG_LIST *)pInput);

    case READ_VBATT:
        /* Device-scoped, so it is valid before any channel exists. */
        return ioctl_read_pin(d, OP_PIN_VBATT, (J_U32 *)pOutput);

    case READ_PROG_VOLTAGE:
        return ioctl_read_pin(d, OP_PIN_PROG_VOLTAGE, (J_U32 *)pOutput);

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
         * application reads from lives here. `atl` exists on the device but
         * what it clears is unmeasured, and the vendor never sends it. */
        if (channel_of(d, ChannelID) == NULL)
            return fail(ERR_INVALID_CHANNEL_ID, "PassThruIoctl(CLEAR_*_BUFFER)");
        op_device_flush_channel(d, (unsigned)ChannelID);
        return STATUS_NOERROR;

    case CLEAR_PERIODIC_MSGS:
        if (channel_of(d, ChannelID) == NULL)
            return fail(ERR_INVALID_CHANNEL_ID, "PassThruIoctl(CLEAR_PERIODIC_MSGS)");
        periodic_drop_channel(ChannelID);
        return STATUS_NOERROR;

    case CLEAR_MSG_FILTERS: {
        unsigned id;
        if (channel_of(d, ChannelID) == NULL)
            return fail(ERR_INVALID_CHANNEL_ID, "PassThruIoctl(CLEAR_MSG_FILTERS)");
        for (id = 0; id < 32u; id++) {
            if ((d->ch[ChannelID].filters & (1u << id)) == 0) continue;
            n = op_cmd_stop_filter(line, sizeof line, (unsigned)ChannelID, id);
            (void)simple_cmd(d, line, n, NULL, 0, OP_DEFAULT_CMD_MS, NULL);
        }
        d->ch[ChannelID].filters = 0;
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
