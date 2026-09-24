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
# A pty holds only a few KB: a 4 KB transmit fills the far side's buffer before
# it drains, so bytes are queued per direction and written as each fd accepts
# them, never dropped and never blocking the other direction.
os.set_blocking(master, False)
pend = {tfd: b"", master: b""}
while True:
    r, w, _ = select.select([master, tfd], [fd for fd in pend if pend[fd]], [])
    for src, dst, d in ((master, tfd, "H>D"), (tfd, master, "D>H")):
        if src in r:
            try: b = os.read(src, 4096)
            except OSError: b = b""
            if b: emit(d, b); pend[dst] += b
    for fd in w:
        try: pend[fd] = pend[fd][os.write(fd, pend[fd]):]
        except BlockingIOError: pass
