/*
 * op_device.c — device session. See op_device.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "op_device.h"
#include "op_log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static op_device g_device = {
    .lock     = PTHREAD_MUTEX_INITIALIZER,
    .cmd_lock = PTHREAD_MUTEX_INITIALIZER,
    .reply_cv = PTHREAD_COND_INITIALIZER,
    .rx_cv    = PTHREAD_COND_INITIALIZER,
};

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

/*
 * Measured by opening each id through Tactrix's DLL against the cable
 * (docs/PROTOCOL.md section 5). The DLL sends `ato0` for J1850 and SCI, which
 * the firmware refuses; firmware channels 7, 8 and 9 are the L line and the
 * 2.5 mm jack, not SCI.
 */
int op_protocol_channel(J_U32 protocol)
{
    switch (protocol) {
    case ISO9141:  case ISO9141_CH1:  return 3;
    case ISO14230: case ISO14230_CH1: return 4;
    case CAN:      case CAN_CH1:      return 5;
    case ISO15765: case ISO15765_CH1: return 6;
    case ISO9141_CH2:                 return 7;
    case ISO14230_CH2:                return 8;
    case ISO9141_CH3:                 return 9;
    default:                          return -1;
    }
}

J_U32 op_protocol_base(J_U32 protocol)
{
    switch (protocol) {
    case ISO9141_CH1: case ISO9141_CH2: case ISO9141_CH3: return ISO9141;
    case ISO14230_CH1: case ISO14230_CH2:                 return ISO14230;
    case CAN_CH1:                                         return CAN;
    case ISO15765_CH1:                                    return ISO15765;
    default:                                              return protocol;
    }
}

/* A queued message: the six PASSTHRU_MSG header fields, then DataSize bytes. */
typedef struct { uint32_t protocol, rx_status, tx_flags, timestamp, size, extra; } op_qhdr;

static void ring_put(op_channel *c, const uint8_t *src, size_t n)
{
    size_t first = OP_RXQ_BYTES - c->rq_tail;
    if (first > n) first = n;
    memcpy(c->rq + c->rq_tail, src, first);
    memcpy(c->rq, src + first, n - first);
    c->rq_tail = (c->rq_tail + n) % OP_RXQ_BYTES;
}

static void ring_get(op_channel *c, uint8_t *dst, size_t n)
{
    size_t first = OP_RXQ_BYTES - c->rq_head;
    if (first > n) first = n;
    memcpy(dst, c->rq + c->rq_head, first);
    memcpy(dst + first, c->rq, n - first);
    c->rq_head = (c->rq_head + n) % OP_RXQ_BYTES;
}

static void queue_push(op_channel *c, const PASSTHRU_MSG *m)
{
    op_qhdr h;
    size_t size = m->DataSize <= sizeof m->Data ? (size_t)m->DataSize : sizeof m->Data;

    if (c->rq == NULL || c->rq_used + sizeof h + size > OP_RXQ_BYTES) {
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
    h.protocol  = (uint32_t)m->ProtocolID;
    h.rx_status = (uint32_t)m->RxStatus;
    h.tx_flags  = (uint32_t)m->TxFlags;
    h.timestamp = (uint32_t)m->Timestamp;
    h.size      = (uint32_t)size;
    h.extra     = (uint32_t)m->ExtraDataIndex;
    ring_put(c, (const uint8_t *)&h, sizeof h);
    ring_put(c, m->Data, size);
    c->rq_used += sizeof h + size;
    c->qcount++;
}

/*
 * Queue one frame as its own message. Indications (J2534-1 section 8.6) report
 * ExtraDataIndex zero; a whole message reports ExtraDataIndex = DataSize.
 */
static void queue_frame(op_device *d, op_channel *c, J_U32 protocol, const op_reply *r,
                        J_U32 status, int is_indication, int keep_data)
{
    PASSTHRU_MSG ind;
    size_t take = keep_data ? r->data_len : 0;

    memset(&ind, 0, sizeof ind);
    ind.ProtocolID = protocol;
    ind.Timestamp = r->timestamp_us;
    ind.RxStatus = status;
    if (take > sizeof ind.Data) take = sizeof ind.Data;
    if (take > 0 && r->data != NULL) memcpy(ind.Data, r->data, take);
    ind.DataSize = (J_U32)take;
    ind.ExtraDataIndex = is_indication ? 0 : ind.DataSize;
    queue_push(c, &ind);
    pthread_cond_broadcast(&d->rx_cv);
}

static void indicate(op_device *d, op_channel *c, const op_reply *r, J_U32 status,
                     int is_indication, int keep_data)
{
    queue_frame(d, c, c->protocol, r, status, is_indication, keep_data);
}

/*
 * Fold one wire frame into the channel's queue. The frame shapes are the
 * measured ones (PROTOCOL.md section 7): a START frame without END announces
 * a segmented message and carries the CAN id only; the data follows in
 * END-terminated frames that carry the id again; a message that fit one CAN
 * frame is a single END frame; raw CAN frames carry neither bit.
 */
static void absorb_frame(op_device *d, const op_reply *r)
{
    op_channel *c;
    PASSTHRU_MSG *p;
    J_U32 base, wide = 0;
    size_t room, take, skip = 0;
    int loopback;

    if (r->channel >= OP_MAX_CHANNELS) return;
    c = &d->ch[r->channel];
    if (!c->open) {
        /* With LOOPBACK on an ISO15765 channel the firmware reports the echoes
         * on raw CAN channel 5: the frames the cable put on the bus, a
         * single-frame request as four zero bytes (bench ECU, 2026-09-24).
         * Tactrix's DLL delivers them to the ISO15765 channel as reported,
         * ProtocolID CAN. While channel 5 is open they are delivered there. */
        int can = op_protocol_channel(CAN), iso = op_protocol_channel(ISO15765);
        if ((int)r->channel == can && (r->status & OP_STS_LOOPBACK) && d->ch[iso].open)
            queue_frame(d, &d->ch[iso], CAN, r,
                        TX_MSG_TYPE | ((r->status & OP_STS_29BIT) ? CAN_29BIT_ID : 0), 0, 1);
        return;
    }

    base = op_protocol_base(c->protocol);
    loopback = (r->status & OP_STS_LOOPBACK) != 0;
    /* Measured on CAN only; what the low nibble carries on K-line is unread. */
    if ((base == CAN || base == ISO15765) && (r->status & OP_STS_29BIT))
        wide = CAN_29BIT_ID;

    if (r->status & OP_STS_TX_IND) {
        /* J2534-1 section 8.6: a TxDone carries the CAN id of the message
         * just sent, which is what the frame holds. */
        indicate(d, c, r, TX_MSG_TYPE | TX_DONE | wide, 1, 1);
        return;
    }

    if (base == CAN) {
        indicate(d, c, r, (loopback ? TX_MSG_TYPE : 0) | wide, 0, 1);
        return;
    }

    if ((r->status & OP_STS_START) && !(r->status & OP_STS_END)) {
        /* The device announces a new message only after finishing the last,
         * so an open reassembly here lost its END frame. */
        if (c->partial_active)
            op_logf("channel %u: new message announced with %lu bytes still "
                    "unterminated; discarding them", r->channel,
                    (unsigned long)c->partial.DataSize);
        c->partial_active = 0;
        indicate(d, c, r, (loopback ? (TX_MSG_TYPE | TX_DONE)
                                    : ISO15765_FIRST_FRAME) | wide, 1,
                 loopback ? 0 : 1);
        return;
    }

    p = &c->partial;
    if (!c->partial_active) {
        memset(p, 0, sizeof *p);
        p->ProtocolID = c->protocol;
        c->partial_active = 1;
    } else if (base == ISO15765) {
        /* Every chunk after the first repeats the 4-byte CAN id: the vendor
         * DLL reassembles only that layout intact (docs/AB-OFFICIAL.md). */
        if (r->data_len >= 4)
            skip = 4;
        else
            op_logf("channel %u: continuation frame of %zu bytes carries no CAN id",
                    r->channel, r->data_len);
    }

    p->Timestamp = r->timestamp_us;
    if (loopback) p->RxStatus |= TX_MSG_TYPE;
    p->RxStatus |= wide;

    take = r->data_len - skip;
    room = sizeof p->Data - p->DataSize;
    if (take > room) {
        op_logf("reassembly overflow on channel %u: dropping %zu bytes",
                r->channel, take - room);
        take = room;
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
    } else if (r->kind == OP_REPLY_INIT && r->init_ntok > 0) {
        /* `arw<ch> <b> <b> ... [<seq>]`: the five-baud key bytes as decimal
         * tokens. Every token is a byte except the echoed number, which is
         * last and is not data. */
        unsigned ntok = r->init_ntok, k;
        size_t n = 0;
        if (d->seq_pending != 0) {
            if (r->init_tok[ntok - 1] != d->seq_pending) {
                op_logf("discarding init reply for command %lu while command %lu waits",
                        (unsigned long)r->init_tok[ntok - 1],
                        (unsigned long)d->seq_pending);
                return;
            }
            ntok--;
        }
        for (k = 0; k < ntok; k++) {
            if (r->init_tok[k] > 0xFF) {
                op_logf("init reply token %lu is not a byte; dropping the result",
                        (unsigned long)r->init_tok[k]);
                n = 0;
                break;
            }
            d->reply_data[n++] = (uint8_t)r->init_tok[k];
        }
        d->reply.data     = n ? d->reply_data : NULL;
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
 * Bring the device to a known state: a numbered reset and a numbered
 * close-all, each answered by number. Replies are matched by sequence number
 * (stash_reply), so whatever a previous session left in the pipe is dropped
 * as belonging to nothing. Runs with the reader thread already started.
 */
static op_status sync_device(op_device *d)
{
    char line[OP_CMD_MAX];
    op_reply r;
    int attempt;
    op_status st = OP_ERR_TIMEOUT;

    /* Terminate a partial line a previous session may have left in the
     * device's parser, as the vendor DLL does before its first command. */
    (void)op_write(&d->t, (const uint8_t *)"\r\n\r\n", 4, 200);

    for (attempt = 0; attempt < 3 && st == OP_ERR_TIMEOUT; attempt++) {
        size_t n = op_cmd_reset(line, sizeof line);
        st = op_device_cmd(d, line, n, NULL, 0, 1000, NULL);
        if (st == OP_ERR_TIMEOUT)
            op_logf("sync: reset unanswered on attempt %d", attempt + 1);
    }
    if (st == OP_OK) {
        size_t n = op_cmd_close_all(line, sizeof line);
        st = op_device_cmd(d, line, n, NULL, 0, 1000, &r);
        if (st == OP_OK && r.kind != OP_REPLY_OK) {
            op_logf("sync: close-all answered kind %d, not aro", (int)r.kind);
            st = OP_ERR_PROTOCOL;
        }
    }
    if (st == OP_OK) {
        op_logf("sync: device in step after %d attempt(s)", attempt);
        return OP_OK;
    }
    if (st == OP_ERR_TIMEOUT) {
        /* Present and accepting writes, but not answering coherently: a
         * link problem, not a missing cable. */
        return OP_ERR_PROTOCOL;
    }
    return st;
}

/* ---- lifecycle ---------------------------------------------------------- */

op_status op_device_open(op_device *d)
{
    op_status st;

    if (d == NULL) return OP_ERR_PARAM;
    if (d->open) return OP_OK;

    {
        unsigned i;
        /* Keep each channel's queue buffer across sessions; the rest is reset. */
        for (i = 0; i < OP_MAX_CHANNELS; i++) {
            uint8_t *rq = d->ch[i].rq;
            memset(&d->ch[i], 0, sizeof d->ch[i]);
            d->ch[i].rq = rq;
        }
    }
    d->pins_grounded = 0;
    d->pins_powered  = 0;
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

    st = g_factory(&d->t);
    if (st != OP_OK) return st;

    if (pthread_create(&d->reader, NULL, reader_main, d) != 0) {
        op_close(&d->t);
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

    /* Closed first, under the lock, so a thread waiting in op_device_pop or
     * cmd_exchange wakes and returns instead of sleeping on a dead device. */
    pthread_mutex_lock(&d->lock);
    d->open = 0;
    pthread_cond_broadcast(&d->reply_cv);
    pthread_cond_broadcast(&d->rx_cv);
    pthread_mutex_unlock(&d->lock);

    atomic_store(&d->reader_stop, 1);
    if (d->reader_started) {
        pthread_join(d->reader, NULL);
        d->reader_started = 0;
    }
    op_close(&d->t);
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

    if (d == NULL || line == NULL || line_len == 0)
        return OP_ERR_PARAM;
    if (payload_len > 0 && payload == NULL)
        return OP_ERR_PARAM;
    if (line_len >= sizeof numbered - 16) return OP_ERR_PARAM;
    if (!d->open) return OP_ERR_NO_DEVICE;
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
    while (!d->reply_valid && d->open && rc != ETIMEDOUT)
        rc = pthread_cond_timedwait(&d->reply_cv, &d->lock, &ts);

    if (d->reply_valid) {
        if (reply != NULL) *reply = d->reply;
        d->reply_valid = 0;
        st = OP_OK;
    } else if (!d->open) {
        st = OP_ERR_NO_DEVICE;
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

    if (d == NULL || out == NULL || channel >= OP_MAX_CHANNELS)
        return OP_ERR_PARAM;

    c = &d->ch[channel];
    pthread_mutex_lock(&d->lock);

    if (c->qcount == 0 && d->open && timeout_ms > 0) {
        deadline_in(&ts, timeout_ms);
        while (c->qcount == 0 && d->open && rc != ETIMEDOUT)
            rc = pthread_cond_timedwait(&d->rx_cv, &d->lock, &ts);
    }

    if (!d->open) {
        st = OP_ERR_NO_DEVICE;
    } else if (c->qcount > 0) {
        op_qhdr h;
        ring_get(c, (uint8_t *)&h, sizeof h);
        memset(out, 0, sizeof *out);
        out->ProtocolID     = h.protocol;
        out->RxStatus       = h.rx_status;
        out->TxFlags        = h.tx_flags;
        out->Timestamp      = h.timestamp;
        out->DataSize       = h.size;
        out->ExtraDataIndex = h.extra;
        ring_get(c, out->Data, h.size);
        c->rq_used -= sizeof h + h.size;
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

static void flush_locked(op_channel *c)
{
    c->rq_head = c->rq_tail = c->rq_used = 0;
    c->qcount = 0;
    c->partial_active = 0;
    c->dropped = 0;
}

void op_device_flush_channel(op_device *d, unsigned channel)
{
    if (d == NULL || channel >= OP_MAX_CHANNELS) return;
    pthread_mutex_lock(&d->lock);
    flush_locked(&d->ch[channel]);
    pthread_mutex_unlock(&d->lock);
}

op_status op_device_open_channel(op_device *d, unsigned channel, uint32_t protocol,
                                 uint32_t flags, uint32_t baud)
{
    op_channel *c;
    uint8_t *rq;

    if (d == NULL || channel >= OP_MAX_CHANNELS) return OP_ERR_PARAM;
    c = &d->ch[channel];
    pthread_mutex_lock(&d->lock);
    rq = c->rq != NULL ? c->rq : malloc(OP_RXQ_BYTES);
    memset(c, 0, sizeof *c);
    c->rq = rq;
    if (rq != NULL) {
        c->open     = 1;
        c->protocol = protocol;
        c->flags    = flags;
        c->baud     = baud;
    }
    pthread_mutex_unlock(&d->lock);
    return rq != NULL ? OP_OK : OP_ERR_IO;
}

void op_device_close_channel(op_device *d, unsigned channel)
{
    op_channel *c;
    if (d == NULL || channel >= OP_MAX_CHANNELS) return;
    c = &d->ch[channel];
    pthread_mutex_lock(&d->lock);
    c->open = 0;
    c->nperiodic = 0;
    flush_locked(c);
    pthread_mutex_unlock(&d->lock);
}
