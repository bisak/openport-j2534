#!/bin/bash
# Run both J2534 libraries through the same sequence and diff what they did.
#
#   run_diff.sh              # no-hardware section only
#   run_diff.sh --hardware   # full sequence, needs a cable
#
# Produces, under tests/differential/out/:
#   {old,new}.result   what each library returned
#   {old,new}.trace    what each library put on the wire
#   results.diff / wire.diff
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="$here/out"
mkdir -p "$out"

OLD_DRIVER="${OLD_DRIVER:?set OLD_DRIVER to the reference dylib}"
NEW_DRIVER="${NEW_DRIVER:?set NEW_DRIVER to the new dylib}"
RUNNER="$here/diff_runner"
TAP="$root/tools/usbtap.dylib"
ARGS=("$@")

if [[ ! -x "$RUNNER" ]]; then echo "build first: make diff-tools" >&2; exit 2; fi


# The old driver blocks forever on most transfers (timeout 0), so every run is
# bounded externally. A run that has to be killed is itself a finding.
run_one() {
    local label="$1" lib="$2"
    if [[ ! -f "$lib" ]]; then
        echo "### library missing: $lib" > "$out/$label.result"
        : > "$out/$label.trace"
        return
    fi
    : > "$out/$label.trace"

    # The injection is applied to the runner's own exec and to nothing else.
    # bash, env and sleep are all arm64e on Apple Silicon, while libusb is
    # arm64-only, so the tap cannot be fat; any arm64e exec that inherits
    # DYLD_INSERT_LIBRARIES aborts before it starts.
    # DYLD_LIBRARY_PATH is deliberately NOT set. It overrides even an
    # absolute dlopen path by leaf name, and both libraries are called
    # libj2534.dylib, so setting it makes both runs load the same file and
    # report a perfect, meaningless match. Both libraries reference libusb by
    # absolute path, so nothing needs it.
    (
        USBTAP_OUT="$out/$label.trace" \
        DYLD_INSERT_LIBRARIES="$TAP" \
        exec "$RUNNER" "$lib" ${ARGS[@]:+"${ARGS[@]}"}
    ) > "$out/$label.result" 2>"$out/$label.stderr" &
    local pid=$!

    # The old driver blocks forever on most transfers (timeout 0), so bound
    # every run externally. A run that must be killed is itself a finding.
    ( sleep "${DIFF_TIMEOUT:-25}"; kill -9 "$pid" 2>/dev/null ) &
    local watchdog=$!

    wait "$pid" 2>/dev/null
    local rc=$?
    kill -9 "$watchdog" 2>/dev/null
    wait "$watchdog" 2>/dev/null

    if [[ $rc -eq 137 ]]; then
        echo "### RUN TIMED OUT after ${DIFF_TIMEOUT:-25} s (library hung)" >> "$out/$label.result"
    fi

    # A run that produced nothing is a failed experiment, not a matching
    # result. Reporting "identical" for two crashed runs would be exactly the
    # silent success this project exists to eliminate.
    if [[ $rc -eq 4 ]]; then
        echo "FATAL: $label loaded the wrong library:" >&2
        sed -n '1,6p' "$out/$label.stderr" >&2
        exit 4
    fi
    if [[ ! -s "$out/$label.result" ]]; then
        echo "FATAL: $label produced no output; see $out/$label.stderr" >&2
        sed -n '1,20p' "$out/$label.stderr" >&2
        exit 3
    fi
}

echo "== reference: $OLD_DRIVER"
run_one old "$OLD_DRIVER"
echo "== candidate: $NEW_DRIVER"
run_one new "$NEW_DRIVER"

# Normalise the things that legitimately differ run to run before diffing:
# timestamps, the library path banner, and the DLL version string.
norm_result() { sed -E -e '/^### (requested|resolved):/d' \
                       -e 's/dll=[^ ]*/dll=<version>/' "$1"; }
norm_trace()  { sed -E -e 's/^ *[0-9]+\.[0-9]+ //' "$1"; }

norm_result "$out/old.result" > "$out/old.result.norm"
norm_result "$out/new.result" > "$out/new.result.norm"
norm_trace  "$out/old.trace"  > "$out/old.trace.norm"
norm_trace  "$out/new.trace"  > "$out/new.trace.norm"

diff -u "$out/old.result.norm" "$out/new.result.norm" > "$out/results.diff"
rdiff=$?
diff -u "$out/old.trace.norm" "$out/new.trace.norm" > "$out/wire.diff"
wdiff=$?

# Both runs must have actually exercised the device before a comparison means
# anything. An empty trace on a hardware run means the tap never saw a transfer.
if [[ " ${ARGS[*]:-} " == *" --hardware "* ]]; then
    for label in old new; do
        if [[ ! -s "$out/$label.trace" ]]; then
            echo "FATAL: $label transferred nothing on the wire" >&2
            exit 3
        fi
    done
fi

echo
echo "===================== RETURN CODES ====================="
if [[ $rdiff -eq 0 ]]; then echo "identical"; else cat "$out/results.diff"; fi
echo
echo "===================== WIRE TRAFFIC ====================="
if [[ $wdiff -eq 0 ]]; then
    echo "byte-for-byte identical"
else
    echo "--- differences (old vs new) ---"
    cat "$out/wire.diff"
fi
echo
echo "artifacts in $out"
