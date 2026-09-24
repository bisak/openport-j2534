/*
 * op_device.h — device session: channels, reassembly, RX queues, command
 * serialisation.
 *
 * A background reader thread owns the inbound byte stream. This exists because
 * the device interleaves asynchronous message frames with command replies on
 * the same pipe, so a caller that read replies inline would lose a reply behind
 * bus traffic. The reader demultiplexes: frames go to per-channel queues,
 * everything else goes to the command-reply slot.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef OP_DEVICE_H
#define OP_DEVICE_H

#include "j2534/j2534.h"
#include "op_proto.h"
#include "op_transport.h"

#include <pthread.h>
#include <stdatomic.h>

/* The device accepts single-digit protocol/channel ids only. */
#define OP_MAX_CHANNELS   10
/* Receive queue per channel, in bytes. Messages are stored at their own size:
 * ~29,000 raw CAN frames (24-byte header + 12) or ~250 of the largest ISO15765 messages. Tactrix's
 * DLL held 1,460 CAN frames left unread for 29 s without a loss (measured
 * 2026-09-16); a 64-message queue lost 1,438 of them. */
#define OP_RXQ_BYTES      (1024u * 1024u)
#define OP_ACCUM_CAP      (OP_MSG_MAX + OP_FRAME_MAX)
#define OP_VERSION_MAX    64
/* J2534-1 requires at least ten periodic messages per channel; the firmware
 * holds ten (measured 2026-09-16). */
#define OP_PERIODIC_PER_CH 10

typedef struct {
    int          open;
    uint32_t     protocol;
    uint32_t     flags;
    uint32_t     baud;

    PASSTHRU_MSG partial;        /* in-progress reassembly */
    int          partial_active;

    uint8_t     *rq;             /* OP_RXQ_BYTES ring, allocated once, kept for the process */
    size_t       rq_head, rq_tail, rq_used;
    unsigned     qcount;         /* messages in the ring */
    unsigned     dropped;        /* queue overruns since last read */

    uint32_t     periodic[OP_PERIODIC_PER_CH]; /* live firmware periodic ids */
    unsigned     nperiodic;
} op_channel;

typedef struct {
    op_transport    t;
    int             open;        /* written under lock; waiters re-check it */

    /* Statically initialised and never destroyed: a thread may be waiting on
     * them when the device closes. */
    pthread_mutex_t lock;
    pthread_cond_t  reply_cv;
    pthread_cond_t  rx_cv;

    pthread_mutex_t cmd_lock;    /* serialises whole command transactions */

    pthread_t       reader;
    int             reader_started;
    /* Read by the reader thread, written by whoever closes the device.
     * `volatile` conveys no ordering between threads and is a data race;
     * an atomic is the only thing that makes this well-defined. */
    atomic_int      reader_stop;

    /* command reply slot, guarded by lock */
    int             cmd_waiting;   /* a command is awaiting its reply */
    int             reply_valid;
    /* Sequence numbers (PROTOCOL.md section 3): every numbered command gets
     * the next one and only a reply echoing it is accepted for that command.
     * A reply carrying another number, or none when one was expected, is
     * stale or belongs to an unwaited command and is dropped. */
    uint32_t        seq_next;
    uint32_t        seq_pending;   /* number the waiting command carries, 0 = unnumbered */
    op_reply        reply;
    char            reply_text[192];
    uint8_t         reply_data[255];  /* raw bytes of an init reply */

    uint8_t         accum[OP_ACCUM_CAP];
    size_t          accum_len;

    op_channel      ch[OP_MAX_CHANNELS];
    /* Pins this session shorted to ground or put a voltage on and has not
     * released. Opening sends `atz` and `ata`, and either one switches every
     * voltage output off (measured 2026-09-16), so a new session starts clear.
     * That they release a ground too is assumed: no readable pin shows it. */
    uint32_t        pins_grounded;
    uint32_t        pins_powered;
    char            fw_version[OP_VERSION_MAX];
} op_device;

op_device *op_device_get(void);

/* The firmware channel a J2534 protocol id is opened on, or -1 when the
 * firmware has none. J2534-2 ids share channels with their J2534-1 twins. */
int op_protocol_channel(J_U32 protocol);

/* The J2534-1 protocol whose framing a channel of this id uses. */
J_U32 op_protocol_base(J_U32 protocol);

/*
 * Test seam. The unit tests substitute a scripted in-memory transport so that
 * every layer above op_transport — reassembly, command/reply matching, the
 * J2534 entry points — is exercised with no cable attached. Passing NULL
 * restores the USB factory.
 */
typedef op_status (*op_transport_factory)(op_transport *);
void op_device_set_factory(op_transport_factory f);

op_status op_device_open(op_device *d);
void      op_device_close(op_device *d);

/*
 * Run one command transaction: send the line (plus optional binary payload)
 * and wait up to timeout_ms for the device's reply.
 *
 * Returns OP_OK when a reply arrived; *reply then holds it, with any text
 * copied into the device's own storage so it stays valid after return.
 */
op_status op_device_cmd(op_device *d, const char *line, size_t line_len,
                        const uint8_t *payload, size_t payload_len,
                        unsigned timeout_ms, op_reply *reply);

/*
 * Send a command and do not wait for its reply. The line is numbered like any
 * other, so the reply, whenever it arrives, is recognised as belonging to this
 * command and dropped rather than mistaken for the answer to a later one.
 * This is what J2534 asks for from PassThruWriteMsgs with Timeout 0.
 */
op_status op_device_send_nowait(op_device *d, const char *line, size_t line_len,
                                const uint8_t *payload, size_t payload_len);

/* Pop one message from a channel's queue. OP_ERR_TIMEOUT when none arrives,
 * OP_ERR_NO_DEVICE when the device is closed, including by another thread
 * while this call was waiting. */
op_status op_device_pop(op_device *d, unsigned channel, PASSTHRU_MSG *out,
                        unsigned timeout_ms);

void op_device_flush_channel(op_device *d, unsigned channel);

/* Mark a channel open for a new connection, with an empty queue. Allocates
 * the queue on first use; OP_ERR_IO when that allocation fails. */
op_status op_device_open_channel(op_device *d, unsigned channel, uint32_t protocol,
                                 uint32_t flags, uint32_t baud);
void      op_device_close_channel(op_device *d, unsigned channel);

/* Number of messages the channel's queue has dropped since this was last
 * called, and reset. Reported to the caller so an overrun is never silent. */
unsigned op_device_take_dropped(op_device *d, unsigned channel);

/* Map a transport/device status onto the J2534 return code. */
long op_status_to_j2534(op_status st);

#endif /* OP_DEVICE_H */
