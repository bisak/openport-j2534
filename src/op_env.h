/*
 * op_env.h — reads the OPENPORT_* environment variables.
 *
 * On Windows from the process's own environment, not the C library's copy of
 * it. A DLL whose C library differs from its host's (msvcrt.dll inside a
 * Python, which uses the UCRT) has a copy made when it loaded, and never sees
 * a variable the host sets afterwards: os.environ["OPENPORT_LOG"] = ... before
 * PassThruOpen would be ignored.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef OP_ENV_H
#define OP_ENV_H

#include <stddef.h>

#ifdef _WIN32
#include <windows.h>

/* The value in buf, or NULL when unset or longer than buf. */
static inline const char *op_getenv(const char *name, char *buf, size_t cap)
{
    DWORD n = GetEnvironmentVariableA(name, buf, (DWORD)cap);
    return n > 0 && n < cap ? buf : NULL;
}
#else
#include <stdlib.h>

static inline const char *op_getenv(const char *name, char *buf, size_t cap)
{
    (void)buf; (void)cap;
    return getenv(name);
}
#endif

#endif /* OP_ENV_H */
