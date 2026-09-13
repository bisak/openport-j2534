/*
 * op_device.c — device session. See op_device.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "op_device.h"
#include "op_log.h"

#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static op_device g_device;

op_device *op_device_get(void) { return &g_device; }

static op_transport_factory g_factory = op_transport_open;

void op_device_set_factory(op_transport_factory f)
{
    g_factory = (f != NULL) ? f : op_usb_open;
}

long op_status_to_j2534(op_status st)
{
    switch (st) {
    case OP_OK:                return STATUS_NOERROR;
    case OP_ERR_TIMEOUT:       return ERR_TIMEOUT;
    case OP_ERR_NO_DEVICE:     return ERR_DEVICE_NOT_CONNECTED;
    case OP_ERR_MASS_STORAGE:  return ERR_DEVICE_NOT_CONNECTED;
    case OP_ERR_ACCESS:        return ERR_DEVICE_IN_USE;
    case OP_ERR_PARAM:         return ERR_NULL_PARAMETER;
    case OP_ERR_OVERFLOW:      return ERR_BUFFER_OVERFLOW;
    case OP_ERR_IO:            return ERR_FAILED;
    case OP_ERR_PROTOCOL:      return ERR_FAILED;
    default:                   return ERR_FAILED;
    }
}

static void deadline_in(struct timespec *ts, unsigned ms)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ts->tv_sec  = tv.tv_sec + (time_t)(ms / 1000u);
    ts->tv_nsec = (long)tv.tv_usec * 1000L + (long)(ms % 1000u) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

/* ---- reassembly --------------------------------------------------------- */

static void queue_push(op_channel *c, const PASSTHRU_MSG *m)
{
    if (c->qcount == OP_RXQ_DEPTH) {
        /*
         * Discard the arriving message, not the oldest one. J2534 requires it,
         * and it is also the safer answer: a caller that ignores the overflow
         * return then sees a truncated but contiguous sequence rather than one
         * with a hole punched in the middle that nothing marks. This driver
         * used to keep the freshest instead, on the reasoning that a stale
         * message is less useful — true for a live gauge, wrong for a
         * diagnostic exchange, where order and contiguity are the whole point.
         * Either way the count is reported, so the loss is never silent.
         */
        c->dropped++;
        return;
    }
    c->q[c->qtail] = *m;
    c->qtail = (c->qtail + 1u) % OP_RXQ_DEPTH;
    c->qcount++;
}

/*
 * Fold one wire frame into the channel's queue.
 *
 * The status byte is a bitfield (START, END, LOOPBACK, TX_IND). What the bits
 * mean was measured on a vehicle, not inferred, and the measurement
 * contradicts the obvious reading. Recorded in PROTOCOL.md section 7 from a
 * vehicle session of 2026-06-17:
 *
 *   - A START frame without END carries the CAN id ONLY. It is an indication
 *     that a segmented message has begun, not the first chunk of its data.
 *     The data arrives later, in END-terminated frames that repeat the id.
 *   - A single-frame message arrives as one END frame. No START precedes it.
 *   - A TX_IND frame carries the CAN id only and means "transmitted".
 *
 * So a START-only frame and a TX_IND frame are each delivered as their own
 * J2534 indication message and never merged with data. Merging was the bug:
 * it produced a message with the CAN id twice and START_OF_MESSAGE set, which
 * every J2534 consumer discards as a first-frame marker.
 */
static void indicate(op_device *d, op_channel *c, const op_reply *r, J_U32 status,
                     int is_indication, int keep_data)
{
    PASSTHRU_MSG ind;
    size_t take = keep_data ? r->data_len : 0;

    memset(&ind, 0, sizeof ind);
    ind.ProtocolID = c->protocol;
    ind.Timestamp = r->timestamp_us;
    ind.RxStatus = status;
    if (take > sizeof ind.Data) take = sizeof ind.Data;
    if (take > 0 && r->data != NULL) memcpy(ind.Data, r->data, take);
    ind.DataSize = (J_U32)take;
    /*
     * An indication reports zero here; an ordinary message reports DataSize.
     * J2534-1 A.1.5.2 spells the first out for the ISO15765 first-frame
     * indication — DataLength four, ExtraDataIndex zero. The "equal to
     * DataLength" rule means "no extra bytes follow" and belongs to real
     * messages, which is what a raw CAN frame delivered through here is.
     */
    ind.ExtraDataIndex = is_indication ? 0 : ind.DataSize;
    queue_push(c, &ind);
    pthread_cond_broadcast(&d->rx_cv);
}

static void absorb_frame(op_device *d, const op_reply *r)
{
    op_channel *c;
    PASSTHRU_MSG *p;
    size_t room, take, skip;
    int loopback;
    J_U32 wide;

    if (r->channel >= OP_MAX_CHANNELS) return;
    c = &d->ch[r->channel];

    if (!c->open) return;

    loopback = (r->status & OP_STS_LOOPBACK) != 0;
    /* The device marks a 29-bit identifier in the status byte; J2534 carries
     * that to the application as a RxStatus bit. */
    wide = (r->status & OP_STS_29BIT) ? CAN_29BIT_ID : 0;

    if (r->status & OP_STS_TX_IND) {
        /*
         * A TxDone carries no data. The device sends the CAN id with it and
         * this driver used to pass that on, but J2534 Table 13 is explicit
         * that DataLength is zero, no vendor sample reads the field, and this
         * driver serialises commands — one transmit is in flight at a time —
         * so there is never any ambiguity about which transmit completed. The
         * id is still in the log for anyone debugging the wire.
         */
        if (c->quiet_tx) {
            op_logf("channel %u: transmit indication for a periodic message, dropped "
                    "(the firmware's periodic facility produces none)", r->channel);
            return;
        }
        op_logf("channel %u: transmit indication for %s", r->channel,
                r->data_len >= 4 ? "the frame just sent" : "a transmit");
        indicate(d, c, r, TX_MSG_TYPE | TX_DONE | wide, 1, 0);
        return;
    }

    /*
     * Raw CAN carries no transport layer: one wire frame is one whole message,
     * and the firmware marks it neither START nor END. Measured on a 2012 VW
     * Caddy: both our own echo and the
     * ECU's reply arrive with status 0x00. Falling through to the reassembly
     * path below would wait for an END bit that never comes and swallow every
     * frame — which is exactly what happened until this was measured, and why
     * PassThruReadMsgs returned ERR_BUFFER_EMPTY on a bus that was answering.
     * opta-j2534-rs reaches the same conclusion independently, returning each
     * raw CAN frame as one J2534 message so a passive logger sees atomic
     * frames rather than a reassembly stream.
     */
    if (c->protocol == CAN) {
        indicate(d, c, r, (loopback ? TX_MSG_TYPE : 0) | wide, 0, 1); /* whole message */
        return;
    }

    if ((r->status & OP_STS_START) && !(r->status & OP_STS_END)) {
        /* An in-progress reassembly cannot legitimately be open here: the
         * device announces a new message only after finishing the last. If
         * one is open, the stream lost its END frame; drop the partial rather
         * than glue two messages together. */
        if (c->partial_active)
            op_logf("channel %u: new message announced with %lu bytes still "
                    "unterminated; discarding them", r->channel,
                    (unsigned long)c->partial.DataSize);
        c->partial_active = 0;
        indicate(d, c, r, (loopback ? (TX_MSG_TYPE | TX_DONE)
                                    : ISO15765_FIRST_FRAME) | wide, 1,
                 loopback ? 0 : 1);   /* the announcement carries the CAN id */
        return;
    }

    p = &c->partial;

    if (!c->partial_active) {
        memset(p, 0, sizeof *p);
        p->ProtocolID = c->protocol;
        c->partial_active = 1;
    }

    p->Timestamp = r->timestamp_us;
    if (loopback) p->RxStatus |= TX_MSG_TYPE;
    p->RxStatus |= wide;

    room = sizeof p->Data - p->DataSize;
    take = r->data_len;
    skip = 0;

    /*
     * How a reply longer than one wire frame is split is the last open receive
     * question (PROTOCOL.md §10): whether every chunk repeats the 4-byte CAN
     * id, or only the first carries it. No ECU reached so far returns enough
     * data to settle it, and the one segmented reply that was measured cannot
     * discriminate — it had a single data frame, which both readings predict.
     *
     * Rather than bet on one, use what the message itself already tells us.
     * The id is the first four bytes of whatever we have accumulated, so a
     * continuation frame that begins with those same four bytes is repeating
     * it and they are not payload. Under the repeating model this is exactly
     * right; under the other it is a no-op unless the payload coincidentally
     * matches the id on a frame boundary, which is logged when it happens.
     */
    if (p->DataSize >= 4 && take >= 4 && r->data != NULL &&
        memcmp(r->data, p->Data, 4) == 0) {
        skip = 4;
        take -= 4;
        op_logf("channel %u: continuation frame repeats the CAN id; treating "
                "those 4 bytes as a header, not payload", r->channel);
    }

    if (take > room) {
        take = room;
        op_logf("reassembly overflow on channel %u: dropping %zu bytes",
                r->channel, r->data_len - room);
    }
    if (take > 0 && r->data != NULL) {
        memcpy(p->Data + p->DataSize, r->data + skip, take);
        p->DataSize += (J_U32)take;
    }
    p->ExtraDataIndex = p->DataSize;

    if (r->status & OP_STS_END) {
        queue_push(c, p);
        c->partial_active = 0;
        pthread_cond_broadcast(&d->rx_cv);
    }
}

/* ---- reader thread ------------------------------------------------------ */

static void stash_reply(op_device *d, const op_reply *r)
{
    /*
     * Only a command that is actually waiting may claim a reply.
     *
     * When a command times out, the device may still answer afterwards. If
     * that late reply were kept, the NEXT command would consume it and every
     * result from then on would belong to the previous request — a silent
     * wrong answer rather than a visible failure. Observed for real: a
     * transmit that the device took 1.2 s to reject with ERR_TIMEOUT had its
     * reply picked up by the following StopMsgFilter.
     */
    if (!d->cmd_waiting) {
        op_logf("discarding orphan reply (kind %d) with no command waiting",
                (int)r->kind);
        return;
    }

    /* A numbered command accepts only the reply that echoes its number. A
     * frame carries none and is never a command reply; an `ari` carries none
     * and only answers the unnumbered `ati`. */
    if (d->seq_pending != 0 && r->kind != OP_REPLY_FRAME && r->kind != OP_REPLY_INIT) {
        if (r->kind == OP_REPLY_INFO || r->ntail == 0) {
            op_logf("discarding unnumbered reply (kind %d) while command %lu waits",
                    (int)r->kind, (unsigned long)d->seq_pending);
            return;
        }
        if (r->tail[r->ntail - 1] != d->seq_pending) {
            op_logf("discarding reply for command %lu while command %lu waits",
                    (unsigned long)r->tail[r->ntail - 1], (unsigned long)d->seq_pending);
            return;
        }
    }

    d->reply = *r;
    if (d->seq_pending != 0 && r->kind == OP_REPLY_ERROR) {
        /* `are <code> [<detail>] <seq>`: with the number stripped, a detail
         * is present only when two tokens followed the code. */
        d->reply.has_detail = r->ntail >= 2;
        d->reply.detail = d->reply.has_detail ? r->tail[0] : 0;
    }
    d->reply_text[0] = '\0';
    if (r->kind == OP_REPLY_INFO && r->text != NULL) {
        size_t n = r->text_len;
        if (n >= sizeof d->reply_text) n = sizeof d->reply_text - 1;
        memcpy(d->reply_text, r->text, n);
        d->reply_text[n] = '\0';
        d->reply.text     = d->reply_text;
        d->reply.text_len = n;
    } else {
        d->reply.text = NULL;
        d->reply.text_len = 0;
    }
    if (r->kind == OP_REPLY_INIT && r->data != NULL && r->data_len > 0) {
        size_t n = r->data_len;
        if (n > sizeof d->reply_data) n = sizeof d->reply_data;
        memcpy(d->reply_data, r->data, n);
        d->reply.data     = d->reply_data;
        d->reply.data_len = n;
    } else {
        d->reply.data     = NULL;
        d->reply.data_len = 0;
    }
    d->reply_valid = 1;
    pthread_cond_broadcast(&d->reply_cv);
}

static void consume_accum(op_device *d)
{
    size_t off = 0;

    while (off < d->accum_len) {
        op_reply r;
        size_t used = op_parse(d->accum + off, d->accum_len - off, &r);

        if (used == 0) break;                     /* need more bytes */

        if (r.kind == OP_REPLY_FRAME) {
            absorb_frame(d, &r);
        } else if (r.kind == OP_REPLY_JUNK) {
            /* Host and device disagree about the stream. Skip to the next
             * plausible boundary rather than consuming byte-by-byte forever. */
            size_t skip = op_resync_offset(d->accum + off, d->accum_len - off);
            op_logf("stream desync at +%zu, resyncing %zu bytes", off, skip);
            off += (skip > used) ? skip : used;
            continue;
        } else {
            stash_reply(d, &r);
        }
        off += used;
    }

    if (off > 0) {
        memmove(d->accum, d->accum + off, d->accum_len - off);
        d->accum_len -= off;
    }
    if (d->accum_len == sizeof d->accum) {
        op_logf("accumulator full with no parsable reply; discarding");
        d->accum_len = 0;
    }
}

static void *reader_main(void *arg)
{
    op_device *d = (op_device *)arg;
    uint8_t tmp[1024];

    while (!atomic_load(&d->reader_stop)) {
        size_t got = 0;
        /* 10 ms is the longest a close waits for this loop to notice the
         * stop flag; data wakes it immediately regardless. */
        op_status st = op_read(&d->t, tmp, sizeof tmp, &got, 10);

        if (st == OP_ERR_TIMEOUT) continue;
        if (st == OP_ERR_NO_DEVICE) {
            op_logf("reader: device disappeared");
            pthread_mutex_lock(&d->lock);
            /* Wake anyone blocked so they fail fast instead of hanging. */
            pthread_cond_broadcast(&d->reply_cv);
            pthread_cond_broadcast(&d->rx_cv);
            pthread_mutex_unlock(&d->lock);
            break;
        }
        if (st != OP_OK || got == 0) continue;

        pthread_mutex_lock(&d->lock);
        {
            size_t room = sizeof d->accum - d->accum_len;
            size_t take = got < room ? got : room;
            if (take < got)
                op_logf("reader: accumulator overflow, dropped %zu bytes", got - take);
            memcpy(d->accum + d->accum_len, tmp, take);
            d->accum_len += take;
            consume_accum(d);
        }
        pthread_mutex_unlock(&d->lock);
    }
    return NULL;
}


/*
 * Bring the device to a known state.
 *
 * The device answers commands in order and echoes the sequence number a
 * command carried, so a reply is accepted only when it names the command it
 * answers (stash_reply). A stale reply left over from a previous session, a
 * seven-deep backlog after an interrupted one, or noise in the stream can
 * therefore no longer shift every subsequent answer by one: it is dropped as
 * belonging to nothing. Sync is then just a numbered reset and a numbered
 * attention, each of which must come back with its number.
 *
 * Runs with the reader thread already started; it is an ordinary command
 * exchange.
 */
static op_status sync_device(op_device *d)
{
    char line[OP_CMD_MAX];
    int attempt;
    op_status st = OP_ERR_TIMEOUT;

    for (attempt = 0; attempt < 3 && st == OP_ERR_TIMEOUT; attempt++) {
        size_t n = op_cmd_reset(line, sizeof line);
        st = op_device_cmd(d, line, n, NULL, 0, 1000, NULL);
        if (st == OP_ERR_TIMEOUT)
            op_logf("sync: reset unanswered on attempt %d", attempt + 1);
    }
    if (st == OP_OK) {
        size_t n = op_cmd_attention(line, sizeof line);
        st = op_device_cmd(d, line, n, NULL, 0, 1000, NULL);
    }
    if (st == OP_OK) {
        op_logf("sync: device in step after %d attempt(s)", attempt);
        return OP_OK;
    }
    if (st == OP_ERR_TIMEOUT) {
        /* The device is present and accepting writes; it just will not
         * answer coherently. Reporting "not connected" would send the user to
         * check cables when the cable is fine. */
        return OP_ERR_PROTOCOL;
    }
    return st;
}

/* ---- lifecycle ---------------------------------------------------------- */

static void destroy_sync(op_device *d)
{
    pthread_mutex_destroy(&d->lock);
    pthread_mutex_destroy(&d->cmd_lock);
    pthread_cond_destroy(&d->reply_cv);
    pthread_cond_destroy(&d->rx_cv);
}

op_status op_device_open(op_device *d)
{
    op_status st;

    if (d == NULL) return OP_ERR_PARAM;
    if (d->open) return OP_OK;

    memset(&d->ch, 0, sizeof d->ch);
    d->accum_len   = 0;
    d->reply_valid = 0;
    d->cmd_waiting = 0;
    /* Start somewhere a previous session is unlikely to have reached, so a
     * reply it left in the pipe cannot carry a number this session is about
     * to wait for. */
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        d->seq_next = 1 + (uint32_t)((tv.tv_sec * 1000 + tv.tv_usec / 1000) % 900000);
    }
    d->seq_pending = 0;
    atomic_store(&d->reader_stop, 0);
    d->fw_version[0] = '\0';

    pthread_mutex_init(&d->lock, NULL);
    pthread_mutex_init(&d->cmd_lock, NULL);
    pthread_cond_init(&d->reply_cv, NULL);
    pthread_cond_init(&d->rx_cv, NULL);

    st = g_factory(&d->t);
    if (st != OP_OK) {
        destroy_sync(d);
        return st;
    }

    if (pthread_create(&d->reader, NULL, reader_main, d) != 0) {
        op_close(&d->t);
        destroy_sync(d);
        return OP_ERR_IO;
    }
    d->reader_started = 1;
    d->open = 1;                    /* op_device_cmd requires it */

    st = sync_device(d);
    if (st != OP_OK) {
        op_device_close(d);
        return st;
    }
    return OP_OK;
}

void op_device_close(op_device *d)
{
    if (d == NULL || !d->open) return;

    atomic_store(&d->reader_stop, 1);
    if (d->reader_started) {
        pthread_join(d->reader, NULL);
        d->reader_started = 0;
    }
    op_close(&d->t);
    destroy_sync(d);
    d->open = 0;
}

/* ---- command transaction ------------------------------------------------ */

static op_status cmd_exchange(op_device *d, const char *line, size_t line_len,
                              const uint8_t *payload, size_t payload_len,
                              unsigned timeout_ms, op_reply *reply, int wait)
{
    char numbered[OP_CMD_MAX + 16];
    uint32_t seq = 0;
    op_status st;
    struct timespec ts;
    int rc = 0;

    if (d == NULL || !d->open || line == NULL || line_len == 0)
        return OP_ERR_PARAM;
    if (payload_len > 0 && payload == NULL)
        return OP_ERR_PARAM;
    if (line_len >= sizeof numbered - 16) return OP_ERR_PARAM;
    if (timeout_ms == 0) timeout_ms = 1000;

    pthread_mutex_lock(&d->cmd_lock);

    memcpy(numbered, line, line_len);
    numbered[line_len] = '\0';
    if (op_cmd_is_numbered(numbered)) {
        size_t n;
        seq = d->seq_next++;
        if (d->seq_next == 0) d->seq_next = 1;
        n = op_cmd_number(numbered, sizeof numbered, line_len, seq);
        if (n == 0) { pthread_mutex_unlock(&d->cmd_lock); return OP_ERR_PARAM; }
        line_len = n;
    }

    /* Claim the reply slot before the command is on the wire, so a fast reply
     * cannot arrive while nothing is marked as waiting. */
    pthread_mutex_lock(&d->lock);
    d->reply_valid = 0;
    d->cmd_waiting = wait;
    d->seq_pending = seq;
    pthread_mutex_unlock(&d->lock);

    st = op_write(&d->t, (const uint8_t *)numbered, line_len, timeout_ms);
    if (st == OP_OK && payload_len > 0)
        st = op_write(&d->t, payload, payload_len, timeout_ms);

    if (st != OP_OK || !wait) {
        pthread_mutex_lock(&d->lock);
        d->cmd_waiting = 0;
        pthread_mutex_unlock(&d->lock);
        pthread_mutex_unlock(&d->cmd_lock);
        return st;
    }

    pthread_mutex_lock(&d->lock);
    deadline_in(&ts, timeout_ms);
    while (!d->reply_valid && rc != ETIMEDOUT)
        rc = pthread_cond_timedwait(&d->reply_cv, &d->lock, &ts);

    if (d->reply_valid) {
        if (reply != NULL) *reply = d->reply;
        d->reply_valid = 0;
        st = OP_OK;
    } else {
        op_logf("command %lu timed out after %u ms; its reply, if it comes, "
                "will be discarded by number", (unsigned long)seq, timeout_ms);
        st = OP_ERR_TIMEOUT;
    }
    d->cmd_waiting = 0;
    pthread_mutex_unlock(&d->lock);

    pthread_mutex_unlock(&d->cmd_lock);
    return st;
}

op_status op_device_cmd(op_device *d, const char *line, size_t line_len,
                        const uint8_t *payload, size_t payload_len,
                        unsigned timeout_ms, op_reply *reply)
{
    return cmd_exchange(d, line, line_len, payload, payload_len, timeout_ms, reply, 1);
}

op_status op_device_send_nowait(op_device *d, const char *line, size_t line_len,
                                const uint8_t *payload, size_t payload_len)
{
    return cmd_exchange(d, line, line_len, payload, payload_len, 1000, NULL, 0);
}

op_status op_device_pop(op_device *d, unsigned channel, PASSTHRU_MSG *out,
                        unsigned timeout_ms)
{
    op_channel *c;
    struct timespec ts;
    int rc = 0;
    op_status st;

    if (d == NULL || !d->open || out == NULL || channel >= OP_MAX_CHANNELS)
        return OP_ERR_PARAM;

    c = &d->ch[channel];
    pthread_mutex_lock(&d->lock);

    if (c->qcount == 0 && timeout_ms > 0) {
        deadline_in(&ts, timeout_ms);
        while (c->qcount == 0 && rc != ETIMEDOUT)
            rc = pthread_cond_timedwait(&d->rx_cv, &d->lock, &ts);
    }

    if (c->qcount > 0) {
        *out = c->q[c->qhead];
        c->qhead = (c->qhead + 1u) % OP_RXQ_DEPTH;
        c->qcount--;
        st = OP_OK;
    } else {
        st = OP_ERR_TIMEOUT;
    }

    pthread_mutex_unlock(&d->lock);
    return st;
}

unsigned op_device_take_dropped(op_device *d, unsigned channel)
{
    unsigned n;
    if (d == NULL || !d->open || channel >= OP_MAX_CHANNELS) return 0;
    pthread_mutex_lock(&d->lock);
    n = d->ch[channel].dropped;
    d->ch[channel].dropped = 0;
    pthread_mutex_unlock(&d->lock);
    return n;
}

void op_device_flush_channel(op_device *d, unsigned channel)
{
    op_channel *c;
    if (d == NULL || channel >= OP_MAX_CHANNELS) return;
    c = &d->ch[channel];
    pthread_mutex_lock(&d->lock);
    c->qhead = c->qtail = c->qcount = 0;
    c->partial_active = 0;
    c->dropped = 0;
    pthread_mutex_unlock(&d->lock);
}

void op_device_quiet_tx(op_device *d, unsigned channel, int on)
{
    if (d == NULL || channel >= OP_MAX_CHANNELS) return;
    pthread_mutex_lock(&d->lock);
    d->ch[channel].quiet_tx = on;
    pthread_mutex_unlock(&d->lock);
}
