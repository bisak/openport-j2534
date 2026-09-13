/*
 * op_log.h — opt-in diagnostic log.
 *
 * Off unless OPENPORT_LOG names a destination, so a production caller pays
 * nothing. "-" and "stderr" write to stderr; anything else is a file path.
 * OPENPORT_LOG_HEX=1 additionally dumps every byte transferred.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef OP_LOG_H
#define OP_LOG_H

#include <stddef.h>
#include <stdint.h>

void op_log_init(void);
void op_log_shutdown(void);
int  op_log_enabled(void);

void op_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void op_log_bytes(const char *tag, const uint8_t *buf, size_t len);

#endif /* OP_LOG_H */
