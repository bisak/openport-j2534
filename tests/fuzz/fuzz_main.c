/*
 * fuzz_main — standalone mutation fuzzer.
 *
 * libFuzzer is the better engine, but Apple's clang does not ship it, and a
 * fuzzing layer that only runs where someone happens to have Homebrew LLVM is
 * a fuzzing layer that does not run. This driver calls the same
 * LLVMFuzzerTestOneInput entry point, so the identical target is used by both:
 * libFuzzer where available (CI on Linux), this everywhere else.
 *
 * Deterministic by seed, so a crash is always reproducible.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define MAX_INPUT 4096
#define MAX_SEEDS 256

static uint8_t  g_seed_buf[MAX_SEEDS][MAX_INPUT];
static size_t   g_seed_len[MAX_SEEDS];
static unsigned g_seeds;

/* xorshift64*, so a run is reproducible from its seed alone. */
static uint64_t g_state;
static uint64_t rnd(void)
{
    g_state ^= g_state >> 12;
    g_state ^= g_state << 25;
    g_state ^= g_state >> 27;
    return g_state * 2685821657736338717ULL;
}
static size_t rnd_below(size_t n) { return n ? (size_t)(rnd() % n) : 0; }

static void load_corpus(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (d == NULL) { fprintf(stderr, "no corpus dir %s\n", dir); return; }
    while ((e = readdir(d)) != NULL && g_seeds < MAX_SEEDS) {
        char path[1024];
        FILE *f;
        size_t n;
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        f = fopen(path, "rb");
        if (f == NULL) continue;
        n = fread(g_seed_buf[g_seeds], 1, MAX_INPUT, f);
        fclose(f);
        g_seed_len[g_seeds++] = n;
    }
    closedir(d);
}

/* Mutations chosen for a length-prefixed, line-oriented binary protocol:
 * flipping the length byte and truncating mid-frame are the interesting cases. */
static size_t mutate(uint8_t *out, size_t len)
{
    switch (rnd() % 8u) {
    case 0:                                     /* flip a bit */
        if (len) out[rnd_below(len)] ^= (uint8_t)(1u << (rnd() % 8u));
        break;
    case 1:                                     /* set a byte to an edge value */
        if (len) {
            static const uint8_t edge[] = { 0x00, 0x01, 0x05, 0x1f, 0x20, 0x7f,
                                            0x80, 0xa0, 0xc0, 0xff,
                                            'a', 'r', '0', '6', '\r', '\n' };
            out[rnd_below(len)] = edge[rnd() % (sizeof edge)];
        }
        break;
    case 2:                                     /* truncate */
        if (len) len = rnd_below(len);
        break;
    case 3:                                     /* grow with noise */
        if (len < MAX_INPUT) {
            size_t add = 1 + rnd_below(16);
            size_t i;
            if (add > MAX_INPUT - len) add = MAX_INPUT - len;
            for (i = 0; i < add; i++) out[len + i] = (uint8_t)rnd();
            len += add;
        }
        break;
    case 4:                                     /* splice in a second seed */
        if (g_seeds > 0 && len < MAX_INPUT) {
            unsigned s = (unsigned)rnd_below(g_seeds);
            size_t take = g_seed_len[s];
            if (take > MAX_INPUT - len) take = MAX_INPUT - len;
            memcpy(out + len, g_seed_buf[s], take);
            len += take;
        }
        break;
    case 5:                                     /* forge a frame header */
        if (len >= 4) {
            size_t at = rnd_below(len - 3);
            out[at] = 'a'; out[at + 1] = 'r';
            out[at + 2] = (uint8_t)('0' + (rnd() % 10u));
            out[at + 3] = (uint8_t)rnd();       /* arbitrary length byte */
        }
        break;
    case 6:                                     /* duplicate a span */
        if (len > 2 && len < MAX_INPUT) {
            size_t at = rnd_below(len);
            size_t n  = 1 + rnd_below(len - at);
            if (n > MAX_INPUT - len) n = MAX_INPUT - len;
            memmove(out + at + n, out + at, len - at);
            len += n;
        }
        break;
    default:                                    /* delete a span */
        if (len > 1) {
            size_t at = rnd_below(len);
            size_t n  = 1 + rnd_below(len - at);
            memmove(out + at, out + at + n, len - at - n);
            len -= n;
        }
        break;
    }
    return len;
}

int main(int argc, char **argv)
{
    const char *corpus = (argc > 1) ? argv[1] : "tests/fuzz/corpus";
    unsigned long iters = (argc > 2) ? strtoul(argv[2], NULL, 10) : 200000;
    unsigned long i;
    uint8_t buf[MAX_INPUT];

    g_state = (argc > 3) ? strtoull(argv[3], NULL, 10) : 0x9E3779B97F4A7C15ULL;
    if (g_state == 0) g_state = 1;

    load_corpus(corpus);
    printf("fuzz: %u seeds, %lu iterations, seed=%llu\n",
           g_seeds, iters, (unsigned long long)g_state);

    /* Every seed, unmutated, first — a corpus that no longer parses is itself
     * a regression. */
    for (i = 0; i < g_seeds; i++)
        LLVMFuzzerTestOneInput(g_seed_buf[i], g_seed_len[i]);

    for (i = 0; i < iters; i++) {
        unsigned s = g_seeds ? (unsigned)rnd_below(g_seeds) : 0;
        size_t len = g_seeds ? g_seed_len[s] : 0;
        unsigned rounds = 1 + (unsigned)(rnd() % 4u);
        unsigned k;
        if (g_seeds) memcpy(buf, g_seed_buf[s], len);
        for (k = 0; k < rounds; k++) len = mutate(buf, len);
        LLVMFuzzerTestOneInput(buf, len);
    }
    printf("fuzz: %lu iterations, no crashes\n", iters);
    return 0;
}
