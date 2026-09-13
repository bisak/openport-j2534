/*
 * op_log.c — opt-in diagnostic log. See op_log.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "op_log.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static FILE           *g_log;
static int             g_hex;
static int             g_init;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void log_init_locked(void)
{
    const char *dest, *hex;

    if (g_init) return;
    g_init = 1;

    dest = getenv("OPENPORT_LOG");
    if (dest == NULL || dest[0] == '\0') return;

    if (strcmp(dest, "-") == 0 || strcmp(dest, "stderr") == 0) {
        g_log = stderr;
    } else {
        g_log = fopen(dest, "a");
        if (g_log == NULL) {
            fprintf(stderr, "openport: cannot open log '%s': %s\n",
                    dest, strerror(errno));
            return;
        }
        setvbuf(g_log, NULL, _IOLBF, 0);
    }

    hex = getenv("OPENPORT_LOG_HEX");
    g_hex = (hex != NULL && hex[0] == '1');
}

void op_log_init(void)
{
    pthread_mutex_lock(&g_lock);
    log_init_locked();
    pthread_mutex_unlock(&g_lock);
}

void op_log_shutdown(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_log != NULL && g_log != stderr) fclose(g_log);
    g_log = NULL;
    g_init = 0;
    pthread_mutex_unlock(&g_lock);
}

int op_log_enabled(void)
{
    int on;
    pthread_mutex_lock(&g_lock);
    log_init_locked();
    on = (g_log != NULL);
    pthread_mutex_unlock(&g_lock);
    return on;
}

static void stamp(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    fprintf(g_log, "[%lld.%06d] ", (long long)tv.tv_sec, (int)tv.tv_usec);
}

void op_logf(const char *fmt, ...)
{
    va_list ap;
    pthread_mutex_lock(&g_lock);
    log_init_locked();
    if (g_log != NULL) {
        stamp();
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fputc('\n', g_log);
    }
    pthread_mutex_unlock(&g_lock);
}

void op_log_bytes(const char *tag, const uint8_t *buf, size_t len)
{
    size_t i;
    pthread_mutex_lock(&g_lock);
    log_init_locked();
    if (g_log != NULL && g_hex && buf != NULL) {
        stamp();
        fprintf(g_log, "%s [%zu]", tag, len);
        for (i = 0; i < len; i++) {
            if ((i % 16) == 0) fprintf(g_log, "\n    ");
            fprintf(g_log, "%02x ", buf[i]);
        }
        fputc('\n', g_log);
    }
    pthread_mutex_unlock(&g_lock);
}
