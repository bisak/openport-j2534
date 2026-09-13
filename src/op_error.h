/*
 * op_error.h — last-error text.
 *
 * J2534 requires PassThruGetLastError to describe the most recent failure.
 * The description is per-thread so two threads cannot overwrite each other's.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef OP_ERROR_H
#define OP_ERROR_H

/* J2534 specifies an 80-byte caller buffer. */
#define OP_ERR_TEXT_MAX 80

void        op_err_clear(void);
void        op_err_set(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const char *op_err_get(void);

/* Human name for a J2534 return code, e.g. "ERR_TIMEOUT". */
const char *op_err_name(long code);

#endif /* OP_ERROR_H */
