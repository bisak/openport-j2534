/*
 * mock_transport.h — scripted in-memory transport for hardware-free tests.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MOCK_TRANSPORT_H
#define MOCK_TRANSPORT_H

#include "op_transport.h"
#include <stddef.h>
#include <stdint.h>

/* Called with each complete command line the driver writes. Push replies with
 * mock_push(). Returning without pushing models a device that stays silent. */
typedef void (*mock_responder)(const char *line, size_t len,
                               const uint8_t *payload, size_t payload_len);

void mock_reset(void);
void mock_set_responder(mock_responder fn);

/* Queue bytes for the driver to read. */
void mock_push(const void *bytes, size_t len);
void mock_push_str(const char *s);
/* Push a text reply with the sequence number of the last command appended,
 * as the device echoes it; `text` carries no CR LF. */
void mock_push_reply(const char *text);

/* Everything the driver has written, in order. */
const uint8_t *mock_tx(size_t *len);
void           mock_clear_tx(void);

/* Force the next N transport calls to fail, to exercise error propagation. */
void mock_fail_writes(int n, op_status st);
void mock_fail_reads(int n, op_status st);

/* Delay the reply to `att` (transmit) commands only, in milliseconds. Models a
 * bus where writes succeed but take real time — the case that distinguishes a
 * per-call timeout budget from a per-message one. Open/Connect are unaffected. */
void mock_delay_transmits(unsigned ms);

/* Transport factory to hand to op_device_set_factory(). */
op_status mock_open(op_transport *out);

/* A responder that behaves like an OpenPort 2.0 for the commands the tests
 * use: ata/atz -> aro, ati -> version, ato/atc -> aro, atr -> arr, etc. */
void mock_install_openport_responder(void);

#endif /* MOCK_TRANSPORT_H */
