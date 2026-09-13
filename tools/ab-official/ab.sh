#!/bin/bash
# A/B the vendor's Windows op20pt32.dll against libj2534.dylib, on this Mac.
#
#   tools/ab-official/ab.sh                       # both against the simulator
#   tools/ab-official/ab.sh --cable /dev/cu.usbmodemXXXX   # both against the cable
#   tools/ab-official/ab.sh -- open iso15765 --repeat 3    # choose scenarios
#   tools/ab-official/ab.sh --replay out/app.rec --sim-args "--ecu-model model.py:Ecu:image.bin"
#                                                # replay a recorded application through both
#
# The vendor DLL runs inside a Docker container (box64 + Wine, see Dockerfile)
# with its device handle spliced onto a pty; the driver under test runs natively
# through the same pty tap. With --cable the tty is served to the container over
# TCP by ttybridge.py and our side runs on the device afterwards. Both traces and both wire captures land in --out
# (default tools/ab-official/out) with a Markdown report from ab_diff.py.
#
# Environment: AB_DLL (path to op20pt32.dll, required), AB_IMAGE, AB_PORT, AB_TIMEOUT.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="$here/out"; cable=""; simargs="--respond 220100=6201+gen600"; nobuild=0; traceargs=(); replay=""
dll="${AB_DLL:-}"
image="${AB_IMAGE:-openport-ab-official}"; port="${AB_PORT:-5455}"; budget="${AB_TIMEOUT:-900}"
while [[ $# -gt 0 ]]; do
  case $1 in
    --cable) cable=$2; shift 2 ;;
    --out) out=$2; shift 2 ;;
    --dll) dll=$2; shift 2 ;;
    --sim-args) simargs=$2; shift 2 ;;
    --image) image=$2; shift 2 ;;
    --no-build) nobuild=1; shift ;;
    --replay) replay=$2; shift 2 ;;
    --) shift; traceargs=("$@"); break ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
mkdir -p "$out"; out="$(cd "$out" && pwd)"    # Docker needs an absolute path for a bind mount
die() { echo "FATAL: $*" >&2; exit 3; }
[[ -n $dll && -f $dll ]] || die "vendor DLL not found: '$dll' (set AB_DLL or --dll to your op20pt32.dll)"
[[ -n $cable && ! -e $cable ]] && die "cable device not found: $cable"

# ---- build everything ------------------------------------------------------
if [[ $nobuild -eq 0 ]]; then
  make -C "$root" libj2534.dylib > /dev/null || die "make failed"
  cc -O2 -Wall -I"$root/include" -o "$here/j2534_trace" "$here/j2534_trace.c" || die "native harness build failed"
  command -v i686-w64-mingw32-gcc > /dev/null || die "i686-w64-mingw32-gcc missing: brew install mingw-w64"
  i686-w64-mingw32-gcc -O1 -Wall -I"$root/include" -o "$here/j2534_trace.exe" "$here/j2534_trace.c" "$here/wineshim.c" || die "win32 harness build failed"
  docker image inspect "$image" > /dev/null 2>&1 || docker build -t "$image" "$here" || die "image build failed"
fi
cp "$here/j2534_trace.exe" "$out/"
# A replay is one recorded application (OPENPORT_RECORD, see docs/AB-OFFICIAL.md)
# executed by both libraries; the sim image it needs is copied next to it so the
# container sees both under /out.
vendor_trace=(); ours_trace=()
if [[ -n $replay ]]; then
  [[ -f $replay ]] || die "recording not found: $replay"
  cp "$replay" "$out/replay.rec"
  vendor_trace=(--replay "Z:/out/replay.rec"); ours_trace=(--replay "$out/replay.rec")
  [[ $budget -lt 3600 ]] && budget=3600
  # An ECU model (FILE.py:CLASS[:IMAGE]) lives outside the repo; copy the file
  # and its image next to the recording so the container sees them under /out.
  if [[ $simargs == *--ecu-model* ]]; then
    spec=$(sed -E 's/.*--ecu-model +([^ ]+).*/\1/' <<<"$simargs")
    IFS=: read -r mfile mclass mimage <<<"$spec"
    cp "$mfile" "$out/ecu-model.py"
    vspec="/out/ecu-model.py:$mclass"; ospec="$out/ecu-model.py:$mclass"
    if [[ -n ${mimage:-} ]]; then
      cp "$mimage" "$out/ecu-image.bin"; vspec+=":/out/ecu-image.bin"; ospec+=":$out/ecu-image.bin"
    fi
    vendor_simargs=$(sed -E "s|--ecu-model +[^ ]+|--ecu-model $vspec|" <<<"$simargs")
    simargs=$(sed -E "s|--ecu-model +[^ ]+|--ecu-model $ospec|" <<<"$simargs")
  fi
fi
vendor_simargs=${vendor_simargs:-$simargs}

# ---- vendor side (container) ----------------------------------------------
cleanup() { [[ -n ${LISTEN:-} ]] && kill "$LISTEN" 2>/dev/null; [[ -n ${SIM:-} ]] && kill "$SIM" 2>/dev/null; [[ -n ${TAP:-} ]] && kill "$TAP" 2>/dev/null; }
trap cleanup EXIT
run_bounded() { # run "$@" with a wall-clock budget; a hung DLL is a finding, not a hang
  "$@" & local pid=$!
  ( sleep "$budget"; kill -9 "$pid" 2>/dev/null ) & local wd=$!
  wait "$pid"; local rc=$?; kill "$wd" 2>/dev/null; wait "$wd" 2>/dev/null
  [[ $rc -eq 137 ]] && echo "### vendor run killed after ${budget}s" >> "$out/vendor.err"
  return $rc
}
echo "== vendor: $dll"
if [[ -z $cable ]]; then
  run_bounded docker run --rm -v "$root:/repo:ro" -v "$dll:/dll/op20pt32.dll:ro" -v "$out:/out" "$image" \
      sim /out --sim-args "$vendor_simargs" -- ${vendor_trace[@]+"${vendor_trace[@]}"} ${traceargs[@]+"${traceargs[@]}"}
else
  python3 "$here/ttybridge.py" "$cable" --port "$port" > "$out/bridge.log" 2>&1 & LISTEN=$!
  sleep 0.5
  run_bounded docker run --rm -v "$root:/repo:ro" -v "$dll:/dll/op20pt32.dll:ro" -v "$out:/out" "$image" \
      cable /out "host.docker.internal:$port" -- ${vendor_trace[@]+"${vendor_trace[@]}"} ${traceargs[@]+"${traceargs[@]}"}
  kill "$LISTEN" 2>/dev/null; wait "$LISTEN" 2>/dev/null; LISTEN=""
fi
[[ -s $out/vendor.jsonl ]] || die "vendor produced no trace; see $out/vendor.err"

# ---- our side (native) ----------------------------------------------------
echo "== ours: $root/libj2534.dylib"
if [[ -z $cable ]]; then
  # shellcheck disable=SC2086
  python3 "$root/tests/sim/openport_sim.py" --log "$out/ours.sim.log" --pty-file "$out/.pty" $simargs > /dev/null 2>&1 & SIM=$!
  for _ in $(seq 1 50); do [[ -s $out/.pty ]] && break; sleep 0.1; done
  dev=$(cat "$out/.pty")
else
  dev=$cable
fi
python3 "$here/ptytap.py" "$dev" --log "$out/ours.tap" --pty-file "$out/.tap" & TAP=$!
for _ in $(seq 1 50); do [[ -s $out/.tap ]] && break; sleep 0.1; done
OPENPORT_DEVICE="$(cat "$out/.tap")" run_bounded "$here/j2534_trace" "$root/libj2534.dylib" "$out/ours.jsonl" ${ours_trace[@]+"${ours_trace[@]}"} ${traceargs[@]+"${traceargs[@]}"} 2> "$out/ours.err"
kill "$TAP" 2>/dev/null; wait "$TAP" 2>/dev/null; TAP=""
[[ -n ${SIM:-} ]] && { kill "$SIM" 2>/dev/null; wait "$SIM" 2>/dev/null; SIM=""; }
rm -f "$out/.pty" "$out/.tap"
[[ -s $out/ours.jsonl ]] || die "our driver produced no trace; see $out/ours.err"

# ---- compare ---------------------------------------------------------------
python3 "$here/ab_diff.py" "$out/vendor.jsonl" "$out/ours.jsonl" --tap "$out/vendor.tap" "$out/ours.tap" --md "$out/report.md"
echo
echo "artifacts in $out"
