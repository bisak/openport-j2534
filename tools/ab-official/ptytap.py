#!/usr/bin/env python3
"""ptytap: expose a pty that forwards to another tty, logging every byte both ways
with timestamps. The Wine-side equivalent of tools/usbtap.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import os, pty, select, sys, time, tty, termios, argparse
ap = argparse.ArgumentParser()
ap.add_argument("target"); ap.add_argument("--log", required=True); ap.add_argument("--pty-file", required=True)
a = ap.parse_args()
master, slave = pty.openpty(); tty.setraw(slave); tty.setraw(master)
tfd = os.open(a.target, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
try: tty.setraw(tfd)
except termios.error: pass
open(a.pty_file, "w").write(os.ttyname(slave) + "\n")
log = open(a.log, "w", buffering=1); t0 = time.monotonic()
def emit(d, b): log.write("%9.4f %s %s |%s|\n" % (time.monotonic() - t0, d, b.hex(" "), "".join(chr(c) if 32 <= c < 127 else "." for c in b)))
while True:
    r, _, _ = select.select([master, tfd], [], [])
    if master in r:
        try: b = os.read(master, 4096)
        except OSError: b = b""
        if b: emit("H>D", b); os.write(tfd, b)
    if tfd in r:
        try: b = os.read(tfd, 4096)
        except OSError: b = b""
        if b: emit("D>H", b); os.write(master, b)
