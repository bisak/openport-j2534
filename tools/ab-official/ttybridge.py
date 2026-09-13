#!/usr/bin/env python3
"""ttybridge: serve a tty on a TCP port so a container can reach the cable.
One client at a time; bytes are forwarded verbatim, the tty is put in raw mode.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse, os, select, socket, sys, tty
ap = argparse.ArgumentParser()
ap.add_argument("tty"); ap.add_argument("--port", type=int, default=5455); ap.add_argument("--bind", default="127.0.0.1")
a = ap.parse_args()
fd = os.open(a.tty, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK); tty.setraw(fd)
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); srv.bind((a.bind, a.port)); srv.listen(1)
print("listening on %s:%d for %s" % (a.bind, a.port, a.tty), flush=True)
while True:
    c, _ = srv.accept(); c.setblocking(False)
    try:
        while True:
            r, _, _ = select.select([c, fd], [], [])
            if c in r:
                b = c.recv(4096)
                if not b: break
                os.write(fd, b)
            if fd in r:
                try: b = os.read(fd, 4096)
                except OSError: b = b""
                if b: c.sendall(b)
    finally:
        c.close()
