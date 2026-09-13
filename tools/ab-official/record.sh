#!/bin/bash
# Record what a J2534 application asks this driver to do, against the simulator.
#
#   tools/ab-official/record.sh <recording> [--sim-args "..."] -- <command...>
#
# Starts a simulator, points the driver at it (OPENPORT_DEVICE) and turns on the
# driver's call recorder (OPENPORT_RECORD) for the command, whose stdin is
# left connected so a tool that asks for confirmation can be answered. The
# recording is then a script tools/ab-official/ab.sh --replay can run through
# both this driver and the vendor's.
#
#   tools/ab-official/record.sh out/app.rec \
#       --sim-args "--ecu-model model.py:Ecu:image.bin" -- \
#       python3 my_tool.py --lib libj2534.dylib
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; root="$(cd "$here/../.." && pwd)"
rec=$1; shift; simargs=""
while [[ $# -gt 0 ]]; do case $1 in --sim-args) simargs=$2; shift 2 ;; --) shift; break ;; *) echo "unknown option $1" >&2; exit 2 ;; esac; done
[[ $# -gt 0 ]] || { echo "no command given" >&2; exit 2; }
tmp=$(mktemp -d); trap 'kill "$SIM" 2>/dev/null; wait "$SIM" 2>/dev/null; rm -rf "$tmp"' EXIT
# shellcheck disable=SC2086
python3 "$root/tests/sim/openport_sim.py" --pty-file "$tmp/pty" --log "${rec%.rec}.sim.log" $simargs > /dev/null 2>&1 & SIM=$!
for _ in $(seq 1 50); do [[ -s $tmp/pty ]] && break; sleep 0.1; done
rm -f "$rec"
OPENPORT_DEVICE="$(cat "$tmp/pty")" OPENPORT_RECORD="$rec" "$@"
rc=$?
echo "recorded $(wc -l < "$rec") lines to $rec (command exit $rc)"
exit $rc
