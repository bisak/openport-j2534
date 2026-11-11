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

#include "op_printf.h"
#include <stddef.h>
#include <stdint.h>

void op_log_init(void);

void op_logf(const char *fmt, ...) OP_PRINTF(1, 2);
void op_log_bytes(const char *tag, const uint8_t *buf, size_t len);

#endif /* OP_LOG_H */
