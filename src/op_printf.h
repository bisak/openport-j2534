/*
 * op_printf.h — marks a function as taking a printf format, for the
 * compiler's format checks.
 *
 * MinGW reads a plain `printf` attribute as Microsoft's printf, which has no
 * %zu, though the build links MinGW's C99 one (__USE_MINGW_ANSI_STDIO, set in
 * the Makefile). __MINGW_PRINTF_FORMAT names the one actually linked.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef OP_PRINTF_H
#define OP_PRINTF_H

#include <stdio.h>

#ifdef __MINGW_PRINTF_FORMAT
#define OP_PRINTF(fmt, args) __attribute__((format(__MINGW_PRINTF_FORMAT, fmt, args)))
#else
#define OP_PRINTF(fmt, args) __attribute__((format(printf, fmt, args)))
#endif

#endif /* OP_PRINTF_H */
