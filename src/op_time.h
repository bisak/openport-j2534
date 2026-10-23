/*
 * op_time.h — absolute deadlines for pthread_cond_timedwait, whose clock is
 * the wall clock on both platforms.
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

#endif /* OP_TIME_H */
