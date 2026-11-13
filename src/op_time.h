/*
 * op_time.h — absolute deadlines for pthread_cond_timedwait, whose clock is
 * the wall clock on both platforms, and a monotonic millisecond clock for
 * deadlines kept by the transports.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef OP_TIME_H
#define OP_TIME_H

#include <sys/time.h>
#include <time.h>

static inline void op_deadline_in(struct timespec *ts, unsigned ms)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ts->tv_sec  = tv.tv_sec + (time_t)(ms / 1000u);
    ts->tv_nsec = (long)tv.tv_usec * 1000L + (long)(ms % 1000u) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

static inline unsigned long long op_mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000u + (unsigned long long)ts.tv_nsec / 1000000u;
}

/* Milliseconds left until a deadline on op_mono_ms(), at least 1 while any
 * is left, 0 once it has passed. */
static inline unsigned op_ms_left(unsigned long long deadline)
{
    unsigned long long now = op_mono_ms();
    if (now >= deadline) return 0;
    return deadline - now > 0x7FFFFFFFu ? 0x7FFFFFFFu : (unsigned)(deadline - now);
}

#endif /* OP_TIME_H */
