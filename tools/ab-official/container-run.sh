#!/bin/bash
# Runs INSIDE the ab-official container: drive the vendor DLL under Wine
# against either the protocol simulator or a cable bridged in over TCP.
#
#   container-run.sh sim   <out-dir> [-- j2534_trace args...] [--sim-args "..."]
#   container-run.sh cable <out-dir> <host:port> [-- j2534_trace args...]
#
# Expects /repo (this repository, read-only), /dll/op20pt32.dll and
# /out (writable). Writes vendor.jsonl, vendor.tap, vendor.sim.log, vendor.err.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -u
mode=$1; out=$2; shift 2
target=""; simargs=""; traceargs=()
if [[ $mode == cable ]]; then target=$1; shift; fi
while [[ $# -gt 0 ]]; do
  case $1 in
    --sim-args) simargs=$2; shift 2 ;;
    --) shift; traceargs=("$@"); break ;;
    *) traceargs+=("$1"); shift ;;
  esac
done

export WINEPREFIX=${WINEPREFIX:-/wine} WINEDEBUG=-all WINEDLLOVERRIDES=winemenubuilder.exe=d
# box64's dynarec mis-executes the DLL's protector (self-modifying code plus
# privileged-instruction SEH tricks); the interpreter runs it correctly.
export BOX64_DYNAREC=0
IFACE='usb#vid_0403&pid_cc4d#op20ab001#{6d1781b7-c987-4f6c-8d4f-1efc098bea67}'
work=$(mktemp -d)
cleanup() { box64 wineserver -k 2>/dev/null; [[ -n ${TAP:-} ]] && kill "$TAP" 2>/dev/null; [[ -n ${SIM:-} ]] && kill "$SIM" 2>/dev/null; [[ -n ${BRIDGE:-} ]] && kill "$BRIDGE" 2>/dev/null; }
trap cleanup EXIT

if [[ $mode == sim ]]; then
  # shellcheck disable=SC2086
  python3 /repo/tests/sim/openport_sim.py --log "$out/vendor.sim.log" --pty-file "$work/pty" $simargs > /dev/null 2>&1 & SIM=$!
  for _ in $(seq 1 50); do [[ -s $work/pty ]] && break; sleep 0.1; done
  dev=$(cat "$work/pty")
else
  socat "PTY,link=$work/cable,raw,echo=0" "TCP:$target" > "$out/vendor.bridge.log" 2>&1 & BRIDGE=$!
  for _ in $(seq 1 50); do [[ -e $work/cable ]] && break; sleep 0.1; done
  dev=$work/cable
fi
python3 /repo/tools/ab-official/ptytap.py "$dev" --log "$out/vendor.tap" --pty-file "$work/tap" & TAP=$!
for _ in $(seq 1 50); do [[ -s $work/tap ]] && break; sleep 0.1; done
ln -sfn "$(cat "$work/tap")" "$WINEPREFIX/dosdevices/$IFACE"

cp /dll/op20pt32.dll "$work/op20pt32.dll"
cp /out/j2534_trace.exe "$work/"
cd "$work"
{ echo "box64: $(box64 --version 2>&1 | head -1)"; echo "wine: $(box64 wine --version 2>/dev/null)"; } > "$out/vendor.env"
box64 wine j2534_trace.exe op20pt32.dll "Z:$out/vendor.jsonl" ${traceargs[@]+"${traceargs[@]}"} 2> "$out/vendor.err"
rc=$?
grep -v BOX64 "$out/vendor.err" | head -20
echo "vendor run exit=$rc"
exit $rc
