#!/bin/bash
# The Windows DLL and the native library, driven through the same J2534 calls
# against the same simulator, compared call by call and on the wire.
#
#   tests/windows/run_wine.sh [32|64] [-- scenario...]
#   tests/windows/run_wine.sh 32 --cable /dev/ttyACM0 [-- scenario...]
#
# The DLL runs under Wine, which hands it the simulator's pty (or the cable's
# serial device) as COM7; the native library opens the same device directly.
# What this exercises that `make test` cannot is src/op_win.c: overlapped
# reads and writes on a serial handle, as Wine implements them. It is not a
# test on Windows.
#
# Scenarios are tools/ab-official/j2534_trace.c's; none given runs its default
# set. Output goes to build/wine-sim/<bits>: windows.jsonl, native.jsonl, the
# two wire captures and report.md. Exit status 0 means no call differed.
#
# Needs wine, the MinGW-w64 compiler for the chosen width, and Python 3. The
# Wine prefix is this script's own (build/wine-sim/prefix), not ~/.wine.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
ab="$root/tools/ab-official"
bits=32; cable=""; simargs="--respond 220100=6201+gen600"; traceargs=()
while [[ $# -gt 0 ]]; do
  case $1 in
    32|64) bits=$1; shift ;;
    --cable) cable=$2; shift 2 ;;
    --sim-args) simargs=$2; shift 2 ;;
    --) shift; traceargs=("$@"); break ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
die() { echo "FATAL: $*" >&2; exit 3; }
[[ $bits == 64 ]] && cc=x86_64-w64-mingw32-gcc || cc=i686-w64-mingw32-gcc
command -v "$cc" > /dev/null || die "$cc missing: install mingw-w64"
command -v wine > /dev/null || die "wine missing"
[[ -n $cable && ! -e $cable ]] && die "cable device not found: $cable"

out="$root/build/wine-sim/$bits"; mkdir -p "$out"
dll="$root/build/win$bits/openport-j2534-$bits.dll"
case $(uname -s) in Darwin) native="$root/libj2534.dylib" ;; *) native="$root/libj2534.so" ;; esac

make -C "$root" CC="$cc" > /dev/null || die "Windows build failed"
make -C "$root" > /dev/null || die "native build failed"
cc -O2 -Wall -I"$root/include" -o "$out/j2534_trace" "$ab/j2534_trace.c" -ldl || die "native harness build failed"
"$cc" -O1 -Wall -I"$root/include" -o "$out/j2534_trace.exe" "$ab/j2534_trace.c" "$ab/wineshim.c" || die "Windows harness build failed"
cp "$dll" "$out/"

# COM7 is a link this script retargets, so the prefix is configured once: Wine
# reads its port table when its server starts.
export WINEPREFIX="$root/build/wine-sim/prefix" WINEDEBUG=-all WINEDLLOVERRIDES=winemenubuilder.exe=d
port="$root/build/wine-sim/com7"
if [[ ! -d $WINEPREFIX ]]; then
  wineboot -i > /dev/null 2>&1 || die "could not create the Wine prefix"
  wine reg add 'HKLM\Software\Wine\Ports' /v COM7 /t REG_SZ /d "$port" /f > /dev/null 2>&1 || die "could not map COM7"
  wineserver -k; wineserver -w
fi

cleanup() { [[ -n ${SIM:-} ]] && kill "$SIM" 2>/dev/null; [[ -n ${TAP:-} ]] && kill "$TAP" 2>/dev/null; }
trap cleanup EXIT

# One side: a fresh simulator (or the cable) behind a tap, then the harness.
side() { # side <name> <command...>
  local name=$1 dev; shift
  rm -f "$out/.pty" "$out/.tap"
  if [[ -z $cable ]]; then
    # shellcheck disable=SC2086
    python3 "$root/tests/sim/openport_sim.py" --log "$out/$name.sim.log" --pty-file "$out/.pty" $simargs > /dev/null 2>&1 & SIM=$!
    for _ in $(seq 1 50); do [[ -s $out/.pty ]] && break; sleep 0.1; done
    dev=$(cat "$out/.pty")
  else
    dev=$cable
  fi
  python3 "$ab/ptytap.py" "$dev" --log "$out/$name.tap" --pty-file "$out/.tap" & TAP=$!
  for _ in $(seq 1 50); do [[ -s $out/.tap ]] && break; sleep 0.1; done
  ln -sfn "$(cat "$out/.tap")" "$port"
  "$@" 2> "$out/$name.err"
  echo "$name run exit=$?"
  kill "$TAP" 2>/dev/null; wait "$TAP" 2>/dev/null; TAP=""
  [[ -n ${SIM:-} ]] && { kill "$SIM" 2>/dev/null; wait "$SIM" 2>/dev/null; SIM=""; }
  [[ -s $out/$name.jsonl ]] || die "$name produced no trace; see $out/$name.err"
}

echo "== windows: $dll"
( cd "$out" && OPENPORT_DEVICE=COM7 side windows wine j2534_trace.exe "openport-j2534-$bits.dll" windows.jsonl ${traceargs[@]+"${traceargs[@]}"} )  || exit 3
echo "== native: $native"
OPENPORT_DEVICE="$port" side native "$out/j2534_trace" "$native" "$out/native.jsonl" ${traceargs[@]+"${traceargs[@]}"} || exit 3

# ab_diff.py was written for Tactrix's DLL against this driver: its "vendor"
# column is the Windows DLL here and its "ours" column the native library.
python3 "$ab/ab_diff.py" "$out/windows.jsonl" "$out/native.jsonl" --tap "$out/windows.tap" "$out/native.tap" --md "$out/report.md"
echo; echo "artifacts in $out (in report.md, \"vendor\" is the Windows DLL and \"ours\" the native library)"

# The report is for reading; the verdict is every step's result the same on
# both sides and the same commands on the wire.
python3 - "$out/report.md" <<'PY'
import sys
rows = bad = 0; wire = False; section = ""
for line in open(sys.argv[1], encoding="utf-8"):
    if line.startswith("## "): section = line
    if "**IDENTICAL**" in line: wire = True
    cells = [c.strip() for c in line.strip().strip("|").split("|")]
    if section.startswith("## 1.") and len(cells) == 5 and cells[0] not in ("scenario", "---"):
        rows += 1
        if cells[2] != cells[3] or cells[4]:
            bad += 1; print("DIFFERS:", line.strip())
print("%d steps compared, %d differ; wire %s" % (rows, bad, "identical" if wire else "DIFFERENT"))
sys.exit(0 if rows and not bad and wire else 1)
PY
