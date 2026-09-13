/*
 * fuzz_parse — drive op_parse over arbitrary bytes.
 *
 * The codec is pure, so it can be fuzzed at millions of executions per minute.
 * This is stronger evidence about the parser than any amount of driving: a
 * vehicle produces well-formed frames, whereas a wedged cable, a desynchronised
 * stream or a partially-read USB packet produces exactly the malformed input
 * this explores.
 *
 * The invariants asserted are the ones the caller depends on:
 *   - op_parse never reads past the buffer it was given (ASAN enforces)
 *   - it either consumes >0 bytes or reports INCOMPLETE, so a caller loop
 *     always terminates
 *   - a FRAME's data pointer and length stay inside the input
 *   - resync never points outside the buffer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "op_proto.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    size_t off = 0;
    unsigned guard = 0;

    /* Copy into an exactly-sized buffer so ASAN traps a one-byte overread,
     * which it would not if we indexed into the fuzzer's larger allocation. */
    uint8_t *buf = (uint8_t *)malloc(size ? size : 1);
    if (buf == NULL) return 0;
    if (size) memcpy(buf, data, size);

    while (off < size) {
        op_reply r;
        size_t used = op_parse(buf + off, size - off, &r);

        if (used == 0) {
            /* Must be an explicit "need more", not a silent stall. */
            assert(r.kind == OP_REPLY_INCOMPLETE);
            break;
        }
        assert(used <= size - off);

        if (r.kind == OP_REPLY_FRAME) {
            assert(r.data >= buf + off);
            assert(r.data + r.data_len <= buf + size);
            assert(r.data_len <= used);
        }
        if (r.kind == OP_REPLY_INFO && r.text_len > 0) {
            assert((const uint8_t *)r.text >= buf + off);
            assert((const uint8_t *)r.text + r.text_len <= buf + size);
        }

        off += used;

        /* A caller loop must terminate; a parser that consumes nothing while
         * claiming progress would hang the reader thread. */
        assert(++guard <= size + 1);
    }

    {
        size_t rs = op_resync_offset(buf, size);
        assert(rs <= size);
    }

    /* The version extractor takes attacker-influenced text. */
    {
        char out[16];
        op_parse_version((const char *)buf, size, out, sizeof out);
        assert(strlen(out) < sizeof out);
    }

    free(buf);
    return 0;
}
