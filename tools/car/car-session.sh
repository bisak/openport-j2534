#!/bin/bash
# One vehicle session, everything captured. READ-ONLY throughout.
#
#   tools/car/car-session.sh [outdir]
#
# Ignition ON, engine off. Nothing here writes to an ECU, starts a programming
# session, or applies programming voltage.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -u

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="${1:-$root/car-session-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$out"
cd "$root"

DEV="${OPENPORT_DEV:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"
OLD_DRIVER="${OLD_DRIVER:-}"   # reference libj2534.dylib for step 5; skipped when unset

say() { printf '\n=== %s ===\n' "$1" | tee -a "$out/session.log"; }
run() { echo "\$ $*" >> "$out/session.log"; "$@" >> "$out/session.log" 2>&1; echo "  -> exit $?" >> "$out/session.log"; }

say "environment"
{ date; sw_vers; uname -m; echo "device: $DEV"; git -C "$root" rev-parse --short HEAD; } | tee -a "$out/session.log"

say "0. build"
run make clean
run make all smoke kline diff-tools

say "1. USB descriptors"
./tools/op_probe 2>&1 | tee "$out/1-descriptors.txt" | tail -20

say "2. smoke test (new driver, USB transport)"
DYLD_LIBRARY_PATH="$root" ./examples/op_smoke 2>&1 | tee "$out/2-smoke-usb.txt" | tail -20

say "3. smoke test (new driver, CDC-ACM transport)"
OPENPORT_DEVICE="$DEV" DYLD_LIBRARY_PATH="$root" ./examples/op_smoke 2>&1 \
  | tee "$out/3-smoke-serial.txt" | tail -6

say "4. protocol capture — every open question in PROTOCOL.md section 10"
python3 tools/car/car_capture.py --dev "$DEV" --out "$out/4-protocol-capture.txt" 2>&1 | tail -30

say "4b. K-line capture — four standard wake-ups, read-only requests"
python3 tools/car/car_capture.py --dev "$DEV" --kline --only q2 --no-preflight \
  --out "$out/4b-kline-capture.txt" 2>&1 | tail -30

say "5. differential vs a reference driver, on a live bus"
if [[ -n "$OLD_DRIVER" ]]; then
  make differential OLD_DRIVER="$OLD_DRIVER" DIFF_ARGS=--hardware 2>&1 | tail -40 \
    | tee "$out/5-differential.txt"
  cp -R tests/differential/out "$out/5-differential-artifacts" 2>/dev/null || true
else
  echo "skipped (set OLD_DRIVER to a reference libj2534.dylib)" | tee -a "$out/session.log"
fi

say "6. hex-level log of one full session"
OPENPORT_LOG="$out/6-wire.log" OPENPORT_LOG_HEX=1 DYLD_LIBRARY_PATH="$root" \
  ./examples/op_smoke --tx > "$out/6-smoke-tx.txt" 2>&1
tail -5 "$out/6-smoke-tx.txt"

say "7. driver-level K-line: FAST_INIT and a read through libj2534 (VAG address, then EOBD)"
DYLD_LIBRARY_PATH="$root" ./examples/op_kline 2>&1 | tee "$out/7-kline-driver-vag.txt" | tail -12
DYLD_LIBRARY_PATH="$root" ./examples/op_kline --eobd 2>&1 | tee "$out/7-kline-driver-eobd.txt" | tail -6

say "8. analysis"
python3 tools/car/analyse_capture.py "$out/4-protocol-capture.txt" "$out/4b-kline-capture.txt" \
  2>&1 | tee "$out/8-analysis.txt" | tail -40

say "done"
tar czf "$out.tgz" -C "$(dirname "$out")" "$(basename "$out")" && echo "archive: $out.tgz"
echo "everything in: $out"
ls -la "$out"
