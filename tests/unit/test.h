/* Minimal test harness. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TEST_H
#define TEST_H
#include <stdio.h>
#include <string.h>
#include <time.h>

extern int g_tests, g_fails;

/* Sub-second sleeps for timing tests. The build selects POSIX.1-2008, which
 * has nanosleep and not usleep. */
static inline void sleep_us(unsigned long us)
{
    struct timespec ts;
    ts.tv_sec  = (time_t)(us / 1000000UL);
    ts.tv_nsec = (long)(us % 1000000UL) * 1000L;
    nanosleep(&ts, NULL);
}

#define CHECK(cond, ...) do {                                   \
    g_tests++;                                                  \
    if (!(cond)) {                                              \
        g_fails++;                                              \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);           \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
    }                                                           \
} while (0)

#define CHECK_EQ(a, b, what) do {                               \
    long _a = (long)(a), _b = (long)(b);                        \
    g_tests++;                                                  \
    if (_a != _b) {                                             \
        g_fails++;                                              \
        printf("  FAIL %s:%d: %s: got %ld, want %ld\n",         \
               __FILE__, __LINE__, (what), _a, _b);             \
    }                                                           \
} while (0)

#define CHECK_STR(a, b, what) do {                              \
    g_tests++;                                                  \
    if (strcmp((a), (b)) != 0) {                                \
        g_fails++;                                              \
        printf("  FAIL %s:%d: %s: got \"%s\", want \"%s\"\n",   \
               __FILE__, __LINE__, (what), (a), (b));           \
    }                                                           \
} while (0)

#define SUITE(name) printf("== %s ==\n", name)

void test_proto(void);
void test_frames(void);
void test_j2534(void);
void test_golden(void);

#endif
