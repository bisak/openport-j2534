#!/usr/bin/env python3
"""
car_capture — settle every open question in docs/PROTOCOL.md section 10 in one
vehicle session.

READ-ONLY. It opens channels, sets filters and sends standard diagnostic
*requests* ($3E TesterPresent, $22 ReadDataByIdentifier). It never writes to an
ECU, never starts a programming session, and never applies programming voltage.

Run with the ignition ON and the engine OFF. The engine may be running; it is
not needed and changes nothing here.

  python3 tools/car/car_capture.py --out <dir>

Everything observed is written to <dir>/protocol-capture.txt for offline
analysis, so nothing depends on interpreting it correctly at the car.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import os
import select
import struct
import sys
import termios
import time

DEFAULT_DEV = next(iter(sorted(__import__("glob").glob("/dev/cu.usbmodem*"))), "/dev/cu.usbmodem1")
OUT = None

# ---------------------------------------------------------------------------
# Safety envelope
#
# On a live vehicle the only defensible thing to transmit is diagnostic
# traffic. ISO 15765-4 reserves 0x7DF (functional request) and 0x7E0-0x7EF
# (physical request/response pairs) for exactly that; 0x7F0-0x7FF is
# unallocated and safe to address because nothing answers there.
#
# Anything below 0x7DF is where a vehicle's real powertrain and chassis
# broadcasts live. Injecting a frame with one of those arbitration ids can be
# read by a listening module as genuine vehicle data. This script will not do
# it, and the check is here rather than in a comment so it cannot be forgotten.
# ---------------------------------------------------------------------------
SAFE_TX_LO = 0x7DF
SAFE_TX_HI = 0x7FF

# ISO15765 request padding. Measured on a VW Caddy (2026-09-13): the ECU
# answered a raw 8-byte CAN request but ignored an unpadded ISO-TP request
# outright. Real ECUs commonly require every CAN frame padded to 8 bytes, and
# the OpenPort only pads when the ISO15765_FRAME_PAD TxFlag (0x40) is set on
# both the transmit and the flow-control filter. Sending unpadded is why the
# whole 2009 Audi CAN session and the first Caddy attempts saw silence.
CAN_FRAME_PAD = 0x40

# ---------------------------------------------------------------------------
# The one documented exception to the range above, and it is opt-in (--tp20).
#
# VAG does not use ISO15765 for diagnostics; it uses VW TP2.0, whose channel
# setup is addressed to 0x200 by design. That id is below 0x7DF, so the blanket
# rule refuses it — but the rule's reasoning ("below 0x7DF is where real
# powertrain broadcasts live") does not apply to it on a VAG: 0x200 is the
# diagnostic channel-setup id every tester uses on every connection.
#
# The second id a TP2.0 session needs is not ours to choose at all: the module
# hands it back in its own setup response, and only that value is unlocked.
# The service whitelist below is unchanged, so only reads travel in the channel.
# ---------------------------------------------------------------------------
TP20_SETUP_ID = 0x200
TP20_EXTRA_TX = set()      # populated by --tp20 and by the module's own reply


def allow_tx(can_id, why):
    """Unlock one CAN id outside the default range, for a stated reason.

    The blanket rule refuses anything below 0x7DF because that is where a
    generic car's powertrain broadcasts live. Some modules answer diagnostics
    on their own id outside that window, and refusing those makes the
    envelope useless for the very ECU it was written to protect. Each exception is per-run, explicit on the
    command line, and recorded in the capture beside the reason.
    """
    TP20_EXTRA_TX.add(can_id)
    emit(f"ENVELOPE: transmitting on 0x{can_id:03X} permitted for this run — {why}")
    return can_id

# ---------------------------------------------------------------------------
# Service-level envelope
#
# The address guard alone is not enough. This ECU is flashed over its K-line,
# which means the physical layer we probe is the same one a programming tool
# uses. A read-only request and a "prepare to erase" request travel identically;
# only the service id distinguishes them.
#
# So the allowed services are an explicit whitelist, and the dangerous ones are
# named individually — a refusal should teach, not just refuse.
# ---------------------------------------------------------------------------
SAFE_SERVICES = {
    0x01: "OBD mode 01 - current data",
    0x02: "OBD mode 02 - freeze frame",
    0x03: "OBD mode 03 - stored DTCs",
    0x05: "OBD mode 05 - oxygen sensor test results (non-CAN)",
    0x06: "OBD mode 06 - test results",
    0x07: "OBD mode 07 - pending DTCs",
    0x09: "OBD mode 09 - vehicle information",
    0x0A: "OBD mode 0A - permanent DTCs",
    0x18: "KWP ReadDiagnosticTroubleCodesByStatus",
    0x1A: "KWP ReadEcuIdentification",
    0x21: "KWP ReadDataByLocalIdentifier",
    0x22: "ReadDataByIdentifier",
    # A read, and what memory-dump tools use — but on an ECU whose memory
    # map is not known, a read of an unmapped address can fault the ECU. So it
    # is doubly gated: whitelisted here, and only ever sent by the opt-in
    # long-read probe with an address the operator chose (see LONG_READ).
    0x23: "ReadMemoryByAddress - a read; only via --long-read with an explicit address",
    0x3E: "TesterPresent",
    0x81: "KWP StartCommunication",
    0x82: "KWP StopCommunication",
}

# Named so a mistake is caught with an explanation rather than a bare error.
FORBIDDEN_SERVICES = {
    0x04: "OBD mode 04 - CLEARS emissions DTCs and extinguishes the MIL",
    0x08: "OBD mode 08 - actuator control",
    0x10: "DiagnosticSessionControl - changes ECU state",
    0x11: "ECUReset",
    0x14: "ClearDiagnosticInformation",
    0x27: "SecurityAccess - the gateway to programming",
    0x28: "CommunicationControl - can silence bus traffic",
    0x2E: "WriteDataByIdentifier - WRITES",
    0x2F: "InputOutputControl - drives actuators",
    0x31: "RoutineControl - can start an erase routine",
    0x34: "RequestDownload - START OF A FLASH",
    0x35: "RequestUpload",
    0x36: "TransferData - FLASH PAYLOAD",
    0x37: "RequestTransferExit",
    0x38: "RequestFileTransfer",
    0x3B: "KWP WriteDataByLocalIdentifier - WRITES",
    0x3D: "WriteMemoryByAddress - WRITES",
    0x85: "ControlDTCSetting",
    0x87: "LinkControl - changes baud rate mid-session",
}


class Unsafe(Exception):
    pass


# StartDiagnosticSession is forbidden as a class. One subfunction may be
# unlocked for a run, and only one: 0x89, the VW "diagnostic session" that
# VCDS opens on every connection. It is not the programming session (0x85),
# which is what moves the EDC16 flash counters. Set by --allow-diag-session.
ALLOWED_SESSION = None


def guard_service(sid, where="request", payload=b""):
    """Refuse anything that is not a read. Applies to CAN and K-line alike."""
    if sid == 0x10 and ALLOWED_SESSION is not None and len(payload) >= 2 \
            and payload[1] == ALLOWED_SESSION:
        return sid
    if sid in FORBIDDEN_SERVICES:
        raise Unsafe(f"refusing service 0x{sid:02X} in {where}: "
                     f"{FORBIDDEN_SERVICES[sid]}")
    if sid not in SAFE_SERVICES:
        raise Unsafe(f"refusing service 0x{sid:02X} in {where}: not on the "
                     f"read-only whitelist")
    return sid


def guard_tx(can_id):
    if can_id in TP20_EXTRA_TX:
        return can_id
    if not (SAFE_TX_LO <= can_id <= SAFE_TX_HI):
        raise Unsafe(
            f"refusing to transmit CAN id 0x{can_id:03X}: outside the "
            f"diagnostic range 0x{SAFE_TX_LO:03X}-0x{SAFE_TX_HI:03X}")
    return can_id


def emit(s=""):
    print(s)
    if OUT:
        OUT.write(s + "\n")
        OUT.flush()


def hexdump(b, ind="      "):
    out = []
    for i in range(0, len(b), 16):
        c = b[i:i + 16]
        out.append(ind + f"{i:04x}  " + " ".join(f"{x:02x}" for x in c).ljust(47) +
                   "  |" + "".join(chr(x) if 32 <= x < 127 else "." for x in c) + "|")
    return "\n".join(out)


class Port:
    def __init__(self, dev):
        self.fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        a = termios.tcgetattr(self.fd)
        a[0] = a[1] = a[3] = 0
        a[2] = termios.CREAD | termios.CLOCAL | termios.CS8
        cc = list(a[6]); cc[termios.VMIN] = 0; cc[termios.VTIME] = 0; a[6] = cc
        termios.tcsetattr(self.fd, termios.TCSANOW, a)
        termios.tcflush(self.fd, termios.TCIOFLUSH)

    def close(self):
        os.close(self.fd)

    def drain(self, sec, idle=0.25):
        buf = b""
        dl = time.time() + sec
        while time.time() < dl:
            r, _, _ = select.select([self.fd], [], [], 0.04)
            if r:
                try:
                    ch = os.read(self.fd, 8192)
                except BlockingIOError:
                    continue
                if ch:
                    buf += ch
                    dl = time.time() + idle
        return buf

    def transmit(self, ch, can_id, service, wait=2.5, txflags=CAN_FRAME_PAD, show=False):
        """The only way this script puts a frame on the CAN bus."""
        guard_tx(can_id)
        if service:
            guard_service(service[0], f"CAN request to 0x{can_id:03X}")
        pl = struct.pack(">I", can_id) + service
        return self.cmd(f"att{ch} {len(pl)} {txflags}\r\n".encode(), pl, wait, show)

    def transmit_kline(self, ch, header, service, wait=3.0, show=False):
        """
        K-line transmit. The service id follows the addressing header, so the
        guard must be told where to look — getting this wrong would let a
        programming request through on the exact bus this ECU is flashed over.

        Tactrix's own DLL sends two extra arguments, a response-wait timeout in
        microseconds and a sequence number (PROTOCOL.md section 4), and is
        reported to need the timeout for slow K-line ECUs. That form is tried
        first; if the firmware rejects it the three-argument form is used and
        the rejection is recorded, which is itself an answer.
        """
        if service:
            guard_service(service[0], "K-line request", service)
        pl = header + service
        r = self.cmd(f"att{ch} {len(pl)} 0 1000000 1\r\n".encode(), pl, wait, show)
        if r.startswith(b"are"):
            emit("      five-argument att rejected; retrying with three arguments")
            r = self.cmd(f"att{ch} {len(pl)} 0\r\n".encode(), pl, wait, show)
        return r

    def cmd(self, line, payload=b"", wait=1.0, show=True):
        termios.tcflush(self.fd, termios.TCIFLUSH)
        t0 = time.time()
        os.write(self.fd, line + payload)
        r, t_first = self.drain_timed(wait)
        d = line.decode("ascii", "replace").replace("\r", "\\r").replace("\n", "\\n")
        # Every exchange is recorded losslessly, whatever the parsers above
        # make of it: the command bytes, the payload, the latency to the first
        # reply byte, and the whole reply as hex. A wrong interpretation at
        # the car costs nothing if the raw bytes survive.
        first_ms = (t_first - t0) * 1000 if t_first else -1
        emit(f"  RAW cmd={d} payload={payload.hex()} first={first_ms:.0f}ms "
             f"total={(time.time()-t0)*1000:.0f}ms reply={r.hex()}")
        if show:
            emit(f"  {d:<40} -> {len(r):>4}B")
            if r:
                emit(hexdump(r))
        return r

    def drain_timed(self, sec, idle=0.25):
        """drain() that also reports when the first byte arrived."""
        buf, t_first = b"", None
        dl = time.time() + sec
        while time.time() < dl:
            rd, _, _ = select.select([self.fd], [], [], 0.04)
            if rd:
                try:
                    ch = os.read(self.fd, 8192)
                except BlockingIOError:
                    continue
                if ch:
                    if t_first is None:
                        t_first = time.time()
                    buf += ch
                    dl = time.time() + idle
        return buf, t_first

    def resync(self):
        for _ in range(6):
            os.write(self.fd, b"\r\n" * 40); self.drain(0.3)
            os.write(self.fd, b"atz\r\n"); self.drain(0.6)
            termios.tcflush(self.fd, termios.TCIOFLUSH)
            time.sleep(0.25)
            if self.cmd(b"ata\r\n", wait=0.8, show=False).startswith(b"aro"):
                return True
        return False


def parse_frames(buf):
    """Split a stream into ASCII lines and binary frames, and describe each."""
    i, out = 0, []
    while i < len(buf):
        if (buf[i:i + 2] == b"ar" and i + 3 < len(buf)
                and 0x30 <= buf[i + 2] <= 0x39):
            ln = buf[i + 3]
            body = buf[i + 4:i + 4 + ln]
            # The uniform split (status, timestamp, data) is the CAN layout.
            # K-line frames are believed to differ (PROTOCOL.md section 7), so
            # the raw body is always kept alongside the interpretation and the
            # analyser decides which layout the recording supports.
            if len(body) >= 5:
                sts = body[0]
                ts = struct.unpack(">I", body[1:5])[0]
                out.append(("FRAME", chr(buf[i + 2]), ln, sts, ts, body[5:], body[1:]))
            elif len(body) >= 1:
                out.append(("FRAME", chr(buf[i + 2]), ln, body[0], 0, b"", body[1:]))
            else:
                out.append(("SHORT", buf[i:i + 4 + ln]))
            i += 4 + ln
            continue
        j = buf.find(b"\r\n", i)
        if j < 0:
            out.append(("TAIL", buf[i:])); break
        out.append(("LINE", buf[i:j].decode("ascii", "replace")))
        i = j + 2
    return out


def show_stream(buf, ind="      "):
    if not buf:
        emit(ind + "(nothing)")
        return
    for e in parse_frames(buf):
        if e[0] == "FRAME":
            _, ch, ln, sts, ts, data, raw = e
            bits = []
            if sts & 0x80: bits.append("START")
            if sts & 0x40: bits.append("END")
            if sts & 0x20: bits.append("LOOPBACK")
            if sts & 0x10: bits.append("BIT4")
            other = sts & ~0xF0
            if other: bits.append(f"other=0x{other:02x}")
            emit(f"{ind}FRAME ch={ch} len={ln} sts=0x{sts:02x} [{'|'.join(bits) or 'none'}] "
                 f"ts={ts} ({ts/1e6:.3f}s) data[{len(data)}]={data.hex(' ')} raw={raw.hex(' ')}")
        elif e[0] == "LINE":
            emit(f"{ind}LINE  {e[1]!r}")
        else:
            emit(f"{ind}{e[0]}  {e[1].hex(' ')}")


def preflight(p, tx, rx):
    """Refuse to continue unless the cable and the vehicle are both healthy."""
    emit("\n### Pre-flight")
    if not p.resync():
        emit("  ABORT: the cable will not synchronise.")
        return False

    r = p.cmd(b"ati\r\n", wait=1.0, show=False)
    emit(f"  firmware: {r.decode('ascii','replace').strip()!r}")

    r = p.cmd(b"atr 16\r\n", wait=1.0, show=False)
    txt = r.decode("ascii", "replace").strip()
    emit(f"  pin 16:   {txt!r}")
    mv = 0
    try:
        mv = int(txt.split()[-1])
    except (ValueError, IndexError):
        pass
    if mv < 11000:
        emit(f"  ABORT: pin 16 reads {mv} mV. Expected 11000-14500 with the")
        emit("         ignition on. Either the cable is not in the OBD port or")
        emit("         the ignition is off. Refusing to transmit onto a bus we")
        emit("         cannot confirm is powered.")
        return False
    emit(f"  battery:  {mv/1000:.2f} V — ignition is on")

    # One legislated, universally-answered, read-only request. If this gets no
    # answer, the CAN ids are wrong and every later section would be noise.
    p.cmd(b"ato6 0 500000 0\r\n", wait=1.0, show=False)
    p.cmd(b"atf6 3 64 4\r\n",
          struct.pack(">I", 0xFFFFFFFF) + struct.pack(">I", rx) + struct.pack(">I", tx),
          1.0, show=False)
    r = p.transmit(6, tx, b"\x3e\x00", wait=2.5)
    frames = [e for e in parse_frames(r) if e[0] == "FRAME"]
    emit(f"  TesterPresent to 0x{tx:03X}: {len(frames)} frame(s)")
    show_stream(r, "    ")
    if not frames:
        emit("  WARNING: no response. Try --tx 0x7DF (functional broadcast),")
        emit("           or the vehicle may use a non-standard address.")
        emit("           Continuing anyway; sections will record silence.")
    return True


def q0_generic_obd(p, rx):
    """
    Mode 01 at the functional broadcast address: the single safest request that
    exists on a road car. Legislated, read-only, and answered by every
    OBD-II/EOBD compliant vehicle regardless of make. If anything gives us real
    received frames, it is this.
    """
    emit("\n### Q0. Generic OBD-II mode 01 (legislated, read-only)")
    p.resync()
    p.cmd(b"ato6 0 500000 0\r\n", wait=1.0, show=False)
    p.cmd(b"atf6 3 64 4\r\n",
          struct.pack(">I", 0xFFFFFFFF) + struct.pack(">I", rx) + struct.pack(">I", 0x7DF),
          1.0, show=False)
    for name, svc in (("$01 $00 supported PIDs", b"\x01\x00"),
                      ("$01 $0C engine RPM", b"\x01\x0c"),
                      ("$01 $05 coolant temp", b"\x01\x05"),
                      ("$09 $02 VIN (multi-frame)", b"\x09\x02")):
        emit(f"\n  -- {name} --")
        r = p.transmit(6, 0x7DF, svc, wait=3.0)
        show_stream(r)
        extra = p.drain(1.5)
        if extra:
            emit("      [later frames]")
            show_stream(extra)


def q1_received_frames(p, tx, rx):
    emit("\n### Q1. Received-message framing with real bus data")
    emit("Sends TesterPresent and ReadDataByIdentifier and records every frame.")
    p.resync()
    p.cmd(b"ato6 0 500000 0\r\n", wait=1.0)
    mask = struct.pack(">I", 0xFFFFFFFF)
    pat = struct.pack(">I", rx)
    fc = struct.pack(">I", tx)
    p.cmd(b"atf6 3 64 4\r\n", mask + pat + fc, 1.0)

    for name, req in (("TesterPresent $3E", b"\x3e\x00"),
                      ("ReadDataByID $22 F1 90 (VIN)", b"\x22\xf1\x90"),
                      ("ReadDataByID $22 F1 8C", b"\x22\xf1\x8c")):
        emit(f"\n  -- {name} --")
        r = p.transmit(6, tx, req, wait=2.5)
        show_stream(r)
        extra = p.drain(1.5)
        if extra:
            emit("      [later frames]")
            show_stream(extra)

    # The same request behind a PASS filter instead of a flow-control filter:
    # does the device still reassemble, or deliver raw CAN frames?
    emit("\n  -- PASS filter instead of flow control: $22 F1 90 --")
    p.resync()
    p.cmd(b"ato6 0 500000 0\r\n", wait=1.0, show=False)
    p.cmd(b"atf6 1 0 4\r\n", mask + pat, 1.0, show=False)
    show_stream(p.transmit(6, tx, b"\x22\xf1\x90", wait=2.5))
    extra = p.drain(1.5)
    if extra:
        emit("      [later frames]")
        show_stream(extra)


# K-line init, in the command form two independent readings of Tactrix's DLL
# agree on (PROTOCOL.md section 4):
#   fast init  : aty<ch> <len> 0  + the StartCommunication request bytes
#   five-baud  : atw<ch> <address in decimal>, no payload (the vendor DLL's form)
# Measured on the Audi 2026-09-13: atw blocks ~2.45 s (one byte at 5 baud),
# aty returns in ~108 ms (a fast-init wake pulse). The earlier `aty<ch> 1 1`
# five-baud form never performed a five-baud init at all.
# The reply to `aty` is `ary<ch> <len>\r\n` followed by <len> raw bytes — the
# StartCommunication response. The reply to `atw` is `arw<ch> <b> <b>\r\n`,
# the keybytes in decimal on the line (third-party, HDS-verified on a Honda;
# PROTOCOL.md section 3). Either can be `are <code>` instead.
#
# Every variant is a standard tester wake-up. Two use the EOBD functional
# address 0x33 (what any scan tool does); two use the VAG engine address 0x01
# (what VCDS does), because a 2009 VAG diesel may answer only its own address.
# StartCommunication ($81) is on the read-only whitelist.
KLINE_INITS = [
    ("five33", 3, "ISO9141-2 five-baud, address 0x33 (EOBD)",          b"atw3 51\r\n", b""),
    ("fast33", 4, "ISO14230-4 fast init, C1 33 F1 81 (EOBD)",          b"aty4 4 0\r\n", b"\xc1\x33\xf1\x81"),
    ("five01", 4, "ISO14230 five-baud, address 0x01 (VAG engine)",     b"atw4 1\r\n", b""),
    ("fast01", 4, "ISO14230 fast init, 81 01 F1 81 (VAG engine)",      b"aty4 4 0\r\n", b"\x81\x01\xf1\x81"),
    ("five10", 4, "ISO14230 five-baud, address 0x10 (Bosch default)",  b"atw4 16\r\n", b""),
]


def kline_header(proto, physical, n):
    """Addressing header for an n-byte request. ISO9141-2: 68 6A F1.
    ISO14230: format byte with the length in its low six bits, then target and
    source. Functional target 0x33; physical target 0x01 (VAG engine ECU)."""
    if proto == 3:
        return b"\x68\x6a\xf1"
    if n > 63:
        raise Unsafe("K-line request longer than 63 bytes")
    return bytes([(0x80 if physical else 0xC0) | n, 0x01 if physical else 0x33, 0xF1])


def parse_init_reply(r):
    """Return ('ary', bytes) / ('arw', bytes) / ('aro', b'') / ('are', code) / ('none', b'')."""
    i = r.find(b"arw")
    if i >= 0:
        j = r.find(b"\r\n", i)
        if j > 0:
            toks = r[i + 4:j].split()
            try:
                vals = [int(t) for t in toks]
            except ValueError:
                vals = []
            return "arw", bytes(v for v in vals if 0 <= v <= 0xFF)
    i = r.find(b"ary")
    if i >= 0:
        j = r.find(b"\r\n", i)
        if j > 0:
            try:
                n = int(r[i + 4:j].split()[-1])
            except (ValueError, IndexError):
                n = 0
            return "ary", r[j + 2:j + 2 + n]
    if b"aro" in r:
        return "aro", b""
    i = r.find(b"are")
    if i >= 0:
        return "are", r[i:r.find(b"\r\n", i) if r.find(b"\r\n", i) > 0 else None]
    return "none", b""


def kline_init(p, variant):
    """Open the channel, arm a pass-all filter, run one init. Returns (proto, ok)."""
    name, proto, label, line, payload = variant
    emit(f"\n  -- {name}: {label} (protocol {proto} @ 10400) --")
    p.resync()
    r = p.cmd(f"ato{proto} 0 10400 0\r\n".encode(), wait=1.0, show=False)
    if not r.startswith(b"aro"):
        emit(f"      channel open failed: {r!r}")
        return proto, False
    # A pass-all filter: without one the firmware is reported to drop every
    # received K-line byte.
    p.cmd(f"atf{proto} 1 0 1\r\n".encode(), b"\x00\x00", 1.0, show=False)
    t0 = time.time()
    r = p.cmd(line, payload, wait=5.0, show=False)
    kind, body = parse_init_reply(r)
    emit(f"      INIT variant={name} ch={proto} reply={kind} took={int((time.time()-t0)*1000)}ms "
         f"bytes={body.hex(' ') if isinstance(body, bytes) else body!r}")
    show_stream(r)
    return proto, kind in ("ary", "arw", "aro")


def kline_requests(p, proto, physical, requests):
    for name, svc in requests:
        emit(f"      [{name}]")
        try:
            hdr = kline_header(proto, physical, len(svc))
            show_stream(p.transmit_kline(proto, hdr, svc, wait=4.0))
        except Unsafe as e:
            emit(f"      !! REFUSED: {e}")
        extra = p.drain(2.0)
        if extra:
            emit("      [later frames]")
            show_stream(extra)


def q2_kline(p, variants=None):
    """
    The one unresolved fork in the frame parser: where is the timestamp in a
    K-line frame, and how does the device answer an init?

    Only read-only requests are sent, and every one passes guard_service().
    Nothing here can begin a programming sequence — which matters, because on
    this ECU the K-line is the flashing path.
    """
    emit("\n### Q2. K-line frame layout — is the 4-byte timestamp present?")
    emit("HIGHEST VALUE: the single unresolved fork in the frame parser.")
    emit("Read-only requests only; every service id is whitelist-checked.")

    eobd = (("mode 01 PID 00 (supported PIDs)", b"\x01\x00"),
            ("mode 09 PID 02 (VIN, multi-frame)", b"\x09\x02"),
            ("TesterPresent 3E", b"\x3e"))
    vag = (("ReadEcuIdentification 1A 9B (part number, software)", b"\x1a\x9b"),
           ("ReadEcuIdentification 1A 91 (hardware number)", b"\x1a\x91"),
           ("ReadEcuIdentification 1A 9C (FLASH STATUS: programming counters)", b"\x1a\x9c"),
           ("ReadEcuIdentification 1A 86 (serial)", b"\x1a\x86"),
           ("ReadEcuIdentification 1A 9A (coding)", b"\x1a\x9a"),
           ("TesterPresent 3E", b"\x3e"))

    for variant in KLINE_INITS:
        if variants and variant[0] not in variants:
            continue
        name, proto = variant[0], variant[1]
        proto, ok = kline_init(p, variant)
        if ok:
            physical = name.endswith("01")
            kline_requests(p, proto, physical, vag if physical else eobd)
            if physical and ALLOWED_SESSION is not None:
                emit(f"      [diagnostic session 0x{ALLOWED_SESSION:02X} allowed by the "
                     "operator; re-reading the identification set inside it]")
                kline_requests(p, proto, physical,
                               (("StartDiagnosticSession 10 %02X" % ALLOWED_SESSION,
                                 bytes([0x10, ALLOWED_SESSION])),) + vag)
        p.cmd(f"atc{proto}\r\n".encode(), wait=0.5, show=False)

    emit("\n  How to read the result: analyse_capture.py decides between the")
    emit("  uniform layout (timestamp on every frame) and the asymmetric one")
    emit("  (timestamp only on start/end frames) from the raw bodies above.")


def q3_txflags(p, tx, rx):
    emit("\n### Q3. att's third argument — is it TxFlags?")
    emit("Same payload, different values. On a live bus a padded frame should")
    emit("differ observably from an unpadded one.")
    for flags in (0, 0x40, 0x100):
        p.resync()
        p.cmd(b"ato6 0 500000 0\r\n", wait=0.8, show=False)
        p.cmd(b"atf6 3 64 4\r\n",
              struct.pack(">I", 0xFFFFFFFF) + struct.pack(">I", rx) + struct.pack(">I", tx),
              0.8, show=False)
        pl = struct.pack(">I", tx) + b"\x3e\x00"
        emit(f"\n  -- txflags={flags} (0x{flags:02x}) --")
        show_stream(p.cmd(f"att6 {len(pl)} {flags}\r\n".encode(), pl, 2.0, show=False))

    # Tactrix's own DLL sends two more arguments: a response-wait timeout in
    # microseconds and a per-command sequence number (read from a disassembly
    # of op20pt32.dll by the emdzej/j2534 project). Does this firmware accept
    # them, and does the reply change?
    p.resync()
    p.cmd(b"ato6 0 500000 0\r\n", wait=0.8, show=False)
    p.cmd(b"atf6 3 64 4\r\n",
          struct.pack(">I", 0xFFFFFFFF) + struct.pack(">I", rx) + struct.pack(">I", tx),
          0.8, show=False)
    pl = struct.pack(">I", tx) + b"\x3e\x00"
    emit("\n  -- five-argument form: att6 <len> 64 1000000 1 --")
    show_stream(p.cmd(f"att6 {len(pl)} 64 1000000 1\r\n".encode(), pl, 2.0, show=False))
    # If the fourth argument really is a response-wait timeout, a 1 ms value
    # should lose the reply that a 1 s value keeps.
    emit("\n  -- five-argument form with a 1 ms timeout: att6 <len> 64 1000 2 --")
    show_stream(p.cmd(f"att6 {len(pl)} 64 1000 2\r\n".encode(), pl, 2.0, show=False))


def count_frames(buf):
    return sum(1 for e in parse_frames(buf) if e[0] == "FRAME")


def q4_periodic(p, tx):
    emit("\n### Q4. atp interval encoding")
    emit("atp<ch> <len> <interval>. 100 was rejected with 'are 5 100'; sweep for the unit.")
    emit("Largest first, and the sweep stops as soon as the unit is known.")
    guard_tx(tx)
    pl = struct.pack(">I", tx) + b"\x3e\x00"
    accepted = 0
    for iv in (65535, 1000, 256, 255, 50, 20, 10, 5, 1, 0):
        p.resync()
        p.cmd(b"ato6 0 500000 0\r\n", wait=0.6, show=False)
        r = p.cmd(f"atp6 {len(pl)} {iv}\r\n".encode(), pl, 1.0, show=False)
        s = r.decode("ascii", "replace").replace("\r", "\\r").replace("\n", "\\n")
        emit(f"  interval={iv:<6} -> {s!r}")
        if r.startswith(b"aro") or r.startswith(b"arp"):
            buf = b""
            try:
                emit("      ACCEPTED — listening up to 3 s to measure the real period:")
                t0 = time.time()
                while time.time() - t0 < 3.0:
                    buf += p.drain(0.2, idle=0.05)
                    if count_frames(buf) >= 12:
                        # Enough to measure. A small unit would otherwise keep
                        # the diagnostic id busy for the rest of the window.
                        emit("      (12 frames seen, stopping early)")
                        break
            finally:
                # A periodic message left running would keep transmitting after
                # we disconnect. Stop it, then reset the channel as a backstop.
                p.cmd(b"atn6 0\r\n", wait=0.8, show=False)
                p.cmd(b"atc6\r\n", wait=0.5, show=False)
                emit("      [stopped]")
            show_stream(buf)
            ts = [e[4] for e in parse_frames(buf) if e[0] == "FRAME" and e[4]]
            if len(ts) >= 2:
                gaps = [b - a for a, b in zip(ts, ts[1:]) if b > a]
                if gaps:
                    period_ms = sorted(gaps)[len(gaps) // 2] / 1000.0
                    emit(f"      measured period ≈ {period_ms:.1f} ms for interval={iv}"
                         + (f" → unit ≈ {period_ms/iv:.3f} ms" if iv else ""))
            accepted += 1
            if accepted >= 2:
                emit("  two accepted intervals measured; that fixes the unit. Sweep stopped.")
                break


def q5_unknown(p):
    emit("\n### Q5. atm / atw / atx on a live channel")
    emit("Verbs whose meaning is unknown. Not read-only by construction, so this")
    emit("section only runs with --unknown-verbs, ideally with the cable on a bench.")
    p.resync()
    p.cmd(b"ato6 0 500000 0\r\n", wait=0.8, show=False)
    for c in (b"atm6\r\n", b"atm6 0\r\n", b"atm6 0 0\r\n",
              b"atw6\r\n", b"atw6 0\r\n", b"atx6\r\n", b"atx6 0\r\n"):
        r = p.cmd(c, wait=0.8, show=False)
        s = r.decode("ascii", "replace").replace("\r", "\\r").replace("\n", "\\n")
        emit(f"  {c.decode().strip():<16} -> {s!r}")


def q6_protocols(p):
    emit("\n### Q6. Two-digit protocol numbers — does the parser read 10 or stop at 1?")
    p.resync()
    for c in (b"ato10 0 500000 0\r\n", b"ato1 0 500000 0\r\n",
              b"ato11 0 500000 0\r\n", b"ato0 0 500000 0\r\n",
              b"ato6 0 500000 0 1\r\n"):     # the DLL's five-argument form
        r = p.cmd(c, wait=0.8, show=False)
        s = r.decode("ascii", "replace").replace("\r", "\\r").replace("\n", "\\n")
        emit(f"  {c.decode().strip():<24} -> {s!r}")
        if r.startswith(b"aro"):
            p.cmd(b"atc10\r\n", wait=0.5, show=False)
            p.cmd(b"atc6\r\n", wait=0.5, show=False)


def q7_pins(p):
    emit("\n### Q7. Pin readings with a vehicle attached")
    emit("Pin 16 should now read battery voltage (~12000-14500 mV) rather than a")
    emit("floating ~130 mV. That confirms the millivolt scaling.")
    p.resync()
    for pin in (0, 6, 9, 11, 12, 13, 14, 15, 16, 17, 18, 20):
        r = p.cmd(f"atr {pin}\r\n".encode(), wait=0.5, show=False)
        s = r.decode("ascii", "replace").replace("\r", "\\r").replace("\n", "\\n")
        emit(f"  pin {pin:<3} -> {s!r}")


def q8_bus_errors(p, tx):
    emit("\n### Q8. Bus error reporting")
    emit("Transmit to an address nothing answers, and to a likely-invalid one.")
    emit("How does the firmware report a bus that is present but silent?")
    p.resync()
    p.cmd(b"ato6 0 500000 0\r\n", wait=0.8, show=False)
    p.cmd(b"atf6 3 64 4\r\n",
          struct.pack(">I", 0xFFFFFFFF) + struct.pack(">I", 0x7FF) + struct.pack(">I", 0x7FE),
          0.8, show=False)
    # 0x7FE and 0x7FF only: inside the diagnostic range, so nothing on the
    # vehicle answers and nothing mistakes them for real vehicle data.
    for target in (0x7FE, 0x7FF):
        emit(f"\n  -- transmit to 0x{target:03X} (in-range, no responder expected) --")
        t0 = time.time()
        r = p.transmit(6, target, b"\x3e\x00", wait=4.0)
        emit(f"      took {(time.time()-t0)*1000:.0f} ms")
        show_stream(r)


def live_request(p, tx, rx, payload):
    """One guarded ISO15765 request, for use at the car: open, flow-control
    filter, transmit, record everything, close."""
    emit(f"\n### LIVE. request {payload.hex(' ')} to 0x{tx:03X}, reply from 0x{rx:03X}")
    guard_tx(tx)
    guard_service(payload[0], f"live request to 0x{tx:03X}")
    p.resync()
    p.cmd(b"ato6 0 500000 0\r\n", wait=1.0, show=False)
    p.cmd(b"atf6 3 64 4\r\n",
          struct.pack(">I", 0xFFFFFFFF) + struct.pack(">I", rx) + struct.pack(">I", tx),
          1.0, show=False)
    show_stream(p.transmit(6, tx, payload, wait=3.0))
    extra = p.drain(1.5)
    if extra:
        emit("      [later frames]")
        show_stream(extra)
    p.cmd(b"atc6\r\n", wait=0.5, show=False)


def live_kline(p, variant_name, payload):
    """One K-line init of the named variant followed by one guarded request."""
    variants = [v for v in KLINE_INITS if v[0] == variant_name]
    if not variants:
        raise Unsafe(f"unknown K-line init variant {variant_name!r}; "
                     f"choose from {[v[0] for v in KLINE_INITS]}")
    emit(f"\n### LIVE. K-line {variant_name} then request {payload.hex(' ')}")
    proto, ok = kline_init(p, variants[0])
    if ok and payload:
        physical = variant_name.endswith("01")
        reqs = (("live request", payload),)
        if ALLOWED_SESSION is not None:
            reqs = (("StartDiagnosticSession 10 %02X" % ALLOWED_SESSION,
                     bytes([0x10, ALLOWED_SESSION])),) + reqs
        kline_requests(p, proto, physical, reqs)
    p.cmd(f"atc{proto}\r\n".encode(), wait=0.5, show=False)


def long_read(p, tx, rx, addr, kline_variant):
    """
    Opt-in. One ReadMemoryByAddress of 255 bytes — the only request that can
    produce a reply longer than one wire frame, which is what settles how the
    device chunks long messages. Sent to CAN, and to the K-line after the named
    wake-up. The address must be one the operator knows is mapped memory on
    this ECU: an unmapped address can raise a bus fault inside the ECU.
    """
    emit(f"\n### LONGREAD. ReadMemoryByAddress 255 bytes @0x{addr:06X} (opt-in)")
    a = addr.to_bytes(3, "big")
    reqs = (("$23 KWP form", b"\x23" + a + b"\xff"),
            ("$23 UDS form (ALFID 13)", b"\x23\x13" + a + b"\xff"))
    guard_tx(tx)
    p.resync()
    p.cmd(b"ato6 0 500000 0\r\n", wait=1.0, show=False)
    p.cmd(b"atf6 3 64 4\r\n",
          struct.pack(">I", 0xFFFFFFFF) + struct.pack(">I", rx) + struct.pack(">I", tx),
          1.0, show=False)
    for name, req in reqs:
        emit(f"\n  -- CAN {name} --")
        show_stream(p.transmit(6, tx, req, wait=3.0))
        extra = p.drain(2.0)
        if extra:
            emit("      [later frames]")
            show_stream(extra)
    p.cmd(b"atc6\r\n", wait=0.5, show=False)
    if kline_variant:
        variants = [v for v in KLINE_INITS if v[0] == kline_variant]
        if not variants:
            raise Unsafe(f"unknown K-line init variant {kline_variant!r}")
        proto, ok = kline_init(p, variants[0])
        if ok:
            kline_requests(p, proto, kline_variant.endswith("01"), (("$23 KWP form", reqs[0][1]),))
        p.cmd(f"atc{proto}\r\n".encode(), wait=0.5, show=False)


# Channel parameters: block size 15, T1 100 ms, T3 10 ms. The conservative of
# the two values the reference implementations use; a shorter T3 is reported to
# degrade the channel on some modules.
TP20_PARAMS = bytes((0xA0, 0x0F, 0x8A, 0xFF, 0x4A, 0xFF))
# Read granularity inside a channel. Both are small because the module drops an
# idle channel after roughly a second: on the first live attempt the parameter
# request arrived about 1.2 s after the module granted the channel, by which
# time it had already gone. Measured on the Audi, 2026-09-13.
TP20_IDLE = 0.03
TP20_SLICE = 0.06
# Our answer to a channel test the module sends us. Measured on the Audi: the
# ECU pings with A3 between messages and expects the parameter response back.
TP20_PARAMS_RESP = bytes((0xA1, 0x0F, 0x8A, 0xFF, 0x4A, 0xFF))


def tp20_split(payload):
    """A KWP message as TP2.0 data frames. The message is prefixed with its
    own 16-bit length, then cut into 7-byte chunks; the last chunk asks for an
    ACK (opcode 1), earlier ones say more follow (opcode 2). Pure, so it can be
    checked without a car."""
    body = struct.pack(">H", len(payload)) + bytes(payload)
    chunks = [body[i:i + 7] for i in range(0, len(body), 7)] or [b""]
    return [(0x10 if i == len(chunks) - 1 else 0x20, c) for i, c in enumerate(chunks)]


def tp20_parse_setup(d):
    """(rx_id, tx_id) from a channel-setup response, or None. Each id is a low
    byte plus a prefix byte whose high nibble flags it invalid."""
    if len(d) < 7 or d[1] != 0xD0:
        return None
    if (d[3] & 0xF0) or (d[5] & 0xF0):
        return None
    return (((d[3] & 0x0F) << 8) | d[2], ((d[5] & 0x0F) << 8) | d[4])


class Tp20:
    """One VW TP2.0 channel over the driver's raw CAN channel. Read-only: every
    KWP payload passes guard_service() before it is wrapped."""

    def __init__(self, p, dest=0x01, ch=5):
        self.p, self.dest, self.ch = p, dest, ch
        self.rx_id = self.tx_id = None
        self.tx_seq = 0
        self.buf = b""
        self.last_tx = 0.0
        self.opened = False

    # -- wire --------------------------------------------------------------
    def _send(self, can_id, data, wait=0.0):
        """Write one raw CAN frame. Draining is the caller's business: a module
        tears a TP2.0 channel down after about a second of silence, so the
        handshake cannot afford to sit in an idle window between frames."""
        guard_tx(can_id)
        pl = struct.pack(">I", can_id) + bytes(data)
        os.write(self.p.fd, f"att{self.ch} {len(pl)} 0\r\n".encode() + pl)
        emit(f"      TP20 tx 0x{can_id:03X} {bytes(data).hex(' ')}")
        self.last_tx = time.time()
        if wait:
            self.buf += self.p.drain(wait, idle=TP20_IDLE)

    def _pump(self, wait):
        """Every raw CAN frame that has arrived, keeping any partial tail."""
        self.buf += self.p.drain(wait, idle=TP20_IDLE)
        out, rest = [], b""
        for e in parse_frames(self.buf):
            if e[0] == "FRAME" and e[1] == str(self.ch) and len(e[5]) >= 4:
                out.append((struct.unpack(">I", e[5][:4])[0], e[5][4:]))
            elif e[0] == "TAIL":
                rest = e[1]
        self.buf = rest
        return out

    def _await(self, rx_id, pred, timeout=2.0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            for cid, d in self._pump(TP20_SLICE):
                emit(f"      TP20 rx 0x{cid:03X} {d.hex(' ')}")
                if (rx_id is None or cid == rx_id) and d and pred(d):
                    return d
        return None

    # -- channel -----------------------------------------------------------
    def open(self):
        emit(f"\n  -- TP2.0 channel setup, module 0x{self.dest:02X} --")
        self.p.resync()
        r = self.p.cmd(f"ato{self.ch} 0 500000 0\r\n".encode(), wait=1.0, show=False)
        if not r.startswith(b"aro"):
            emit(f"      raw CAN channel would not open: {r!r}")
            return False
        self.p.cmd(f"atf{self.ch} 1 0 4\r\n".encode(),
                   struct.pack(">I", 0) + struct.pack(">I", 0), 1.0, show=False)

        forms = (("ECU picks its receive id", bytes([self.dest, 0xC0, 0x00, 0x10, 0x00, 0x03, 0x01])),
                 ("both ids named",           bytes([self.dest, 0xC0, 0x00, 0x03, 0x00, 0x03, 0x01])))
        refused = False
        # A module that still holds a channel from an earlier tester refuses a
        # fresh setup, typically with 0xD7. It drops an idle channel by itself
        # after about a second, so waiting it out is the cure — and it needs no
        # widening of the id envelope, unlike tearing the old channel down.
        for attempt in range(3):
            if attempt:
                emit(f"      attempt {attempt + 1}: waiting out a stale channel")
                time.sleep(1.5)
            for label, setup in forms:
                emit(f"      setup form: {label}")
                self._send(TP20_SETUP_ID, setup)
                d = self._await(TP20_SETUP_ID + self.dest,
                                lambda x: len(x) >= 7 and x[1] in (0xD0, 0xD6, 0xD7, 0xD8), 1.5)
                if d is None:
                    emit("      no reply to this form")
                    continue
                if d[1] != 0xD0:
                    refused = True
                    emit(f"      module refused the channel (opcode 0x{d[1]:02X}) — "
                         "usually a channel left open by an earlier tester")
                    continue
                ids = tp20_parse_setup(d)
                if ids is None:
                    emit(f"      setup reply marks an id invalid: {d.hex(' ')}")
                    continue
                self.rx_id, self.tx_id = ids
                TP20_EXTRA_TX.add(self.tx_id)
                emit(f"      channel open: we receive on 0x{self.rx_id:03X}, "
                     f"transmit to 0x{self.tx_id:03X} (the module's own choice)")
                break
            if self.tx_id is not None:
                break
            if not refused:
                break                      # silence, not a refusal: retrying will not help
        if self.tx_id is None:
            emit("      no module answered a channel setup"
                 + (" (it refused every attempt)" if refused else ""))
            return False

        self._send(self.tx_id, TP20_PARAMS)          # immediately: the clock is running
        d = self._await(self.rx_id, lambda x: x[0] == 0xA1, 1.5)
        if d is None:
            emit("      no channel-parameter response")
            return False
        emit(f"      parameters agreed: {d.hex(' ')}")
        self.opened = True
        return True

    def keepalive(self):
        """The module drops an idle channel after about a second."""
        if self.opened and time.time() - self.last_tx > 0.4:
            self._send(self.tx_id, b"\xa3")
            self._await(self.rx_id, lambda x: x[0] == 0xA1, 0.5)

    def request(self, payload, timeout=4.0):
        guard_service(payload[0], "TP2.0 request", payload)
        self.keepalive()
        for op, chunk in tp20_split(payload):
            self._send(self.tx_id, bytes([op | (self.tx_seq & 0x0F)]) + chunk)
            self.tx_seq = (self.tx_seq + 1) & 0x0F

        want, acc, t0 = None, b"", time.time()
        while time.time() - t0 < timeout:
            for cid, d in self._pump(TP20_SLICE):
                emit(f"      TP20 rx 0x{cid:03X} {d.hex(' ')}")
                if cid != self.rx_id or not d:
                    continue
                op, seq = d[0] >> 4, d[0] & 0x0F
                if op == 0xA:
                    if d[0] == 0xA8:
                        emit("      module closed the channel")
                        self.opened = False
                        return None
                    if d[0] == 0xA3:               # the module is testing us
                        self._send(self.tx_id, TP20_PARAMS_RESP)
                    continue                       # channel test / parameter reply
                if op in (0xB, 0x9):
                    continue                       # acknowledgement of our own frame
                if op not in (0x0, 0x1, 0x2, 0x3):
                    continue
                part = d[1:]
                if want is None:
                    if len(part) < 2:
                        continue
                    # 16-bit big-endian length — but the top bit of the high
                    # byte is set on the module's responsePending messages
                    # (measured: 80 03 for a three-byte body, against 00 30 for
                    # a real answer). Its meaning is unknown; masking it gives
                    # the right length in every case observed.
                    want, part = (((part[0] & 0x7F) << 8) | part[1]), part[2:]
                acc += part
                if op in (0x0, 0x1):               # the module is waiting for an ACK
                    self._send(self.tx_id, bytes([0xB0 | ((seq + 1) & 0x0F)]))
                if want is not None and len(acc) >= want:
                    msg = acc[:want]
                    if len(msg) >= 3 and msg[0] == 0x7F and msg[2] == 0x78:
                        emit(f"      responsePending ({msg.hex(' ')}) — the module "
                             "asked for more time; still listening")
                        want, acc = None, b""
                        t0 = time.time()
                        continue
                    return msg
        emit(f"      no complete answer in {timeout:.0f}s (got {len(acc)} of {want} bytes)")
        return None

    def close(self):
        try:
            if self.tx_id is not None:
                self._send(self.tx_id, b"\xa8", wait=0.1)
        except Exception as e:
            emit(f"      (disconnect: {e!r})")
        self.p.cmd(f"atc{self.ch}\r\n".encode(), wait=0.5, show=False)
        self.opened = False


def show_flash_status(resp):
    """The 1A 9C record carries the OBD programming counters. Its layout is not
    documented, so print every reading and let the known values identify
    themselves."""
    rec = resp[2:] if len(resp) > 2 and resp[0] == 0x5A else resp
    emit(f"      FLASH STATUS bytes: {rec.hex(' ')}")
    emit("        as 8-bit : " + " ".join(str(x) for x in rec))
    emit("        as 16-bit: " + " ".join(str(int.from_bytes(rec[i:i + 2], "big"))
                                          for i in range(0, len(rec) - 1, 2)))


TP20_READS = (
    ("ReadEcuIdentification 1A 9B (part number, software)", b"\x1a\x9b"),
    ("ReadEcuIdentification 1A 91 (hardware number)",       b"\x1a\x91"),
    ("ReadEcuIdentification 1A 9C (FLASH STATUS)",          b"\x1a\x9c"),
    ("ReadEcuIdentification 1A 86 (serial)",                b"\x1a\x86"),
    ("ReadEcuIdentification 1A 9A (coding)",                b"\x1a\x9a"),
    ("TesterPresent 3E",                                    b"\x3e"),
)


TP20_MODULES = {0x01: "engine", 0x19: "CAN gateway", 0x17: "instruments", 0x03: "ABS"}


def q12_tp20(p, dests, extra=None):
    emit("\n### Q12. VW TP2.0 over raw CAN — the transport VAG actually uses")
    emit("ISO15765 is not how a VAG module is addressed. This opens a TP2.0")
    emit("channel on raw CAN and sends only whitelisted read services inside it.")
    # If the engine stays silent but another module answers, the transport is
    # proven and the remaining question is only which address the engine is on.
    t = None
    for dest in dests:
        cand = Tp20(p, dest=dest)
        emit(f"\n  == module 0x{dest:02X} ({TP20_MODULES.get(dest, 'unknown')}) ==")
        if cand.open():
            t = cand
            break
        cand.close()
    if t is None:
        emit("\n  no module answered a TP2.0 channel setup at any address tried.")
        return
    try:
        emit(f"\n  -- reads inside the channel to 0x{t.dest:02X} --")
        reqs = list(TP20_READS)
        if ALLOWED_SESSION is not None:
            reqs.insert(0, ("StartDiagnosticSession 10 %02X" % ALLOWED_SESSION,
                            bytes([0x10, ALLOWED_SESSION])))
        if extra:
            reqs.append(("operator request", extra))
        for name, req in reqs:
            emit(f"\n  -- {name} --")
            try:
                resp = t.request(req)
            except Unsafe as e:
                emit(f"      !! REFUSED: {e}")
                continue
            if resp is None:
                continue
            emit(f"      RESPONSE {resp.hex(' ')}")
            printable = "".join(chr(c) if 32 <= c < 127 else "." for c in resp)
            emit(f"      as text  {printable}")
            if req[:2] == b"\x1a\x9c" and resp[:1] == b"\x5a":
                show_flash_status(resp)
            if not t.opened:
                break
    finally:
        t.close()


def safe_shutdown(p):
    """Whatever happened — Ctrl-C included — leave the cable transmitting
    nothing: stop any periodic message, close every channel, reset."""
    try:
        p.cmd(b"atn6 0\r\n", wait=0.4, show=False)
        for ch in (3, 4, 5, 6):
            p.cmd(f"atc{ch}\r\n".encode(), wait=0.3, show=False)
        p.resync()
    except Exception as e:      # the port may already be gone
        emit(f"  (shutdown: {e!r})")


def q9_loopback(p, tx, rx):
    """LOOPBACK=1 on a live bus: the shape of a transmit echo has only ever
    been guessed (PROTOCOL.md section 7). One TesterPresent, echo recorded,
    LOOPBACK back off."""
    emit("\n### Q9. Transmit echo shape with LOOPBACK=1")
    p.resync()
    p.cmd(b"ato6 0 500000 0\r\n", wait=1.0, show=False)
    p.cmd(b"atf6 3 64 4\r\n",
          struct.pack(">I", 0xFFFFFFFF) + struct.pack(">I", rx) + struct.pack(">I", tx),
          1.0, show=False)
    emit("  " + p.cmd(b"ats6 3 1\r\n", wait=0.8, show=False).decode("ascii", "replace").strip())
    emit("  " + p.cmd(b"atg6 3\r\n", wait=0.8, show=False).decode("ascii", "replace").strip())
    try:
        show_stream(p.transmit(6, tx, b"\x3e\x00", wait=2.5))
        extra = p.drain(1.5)
        if extra:
            emit("      [later frames]")
            show_stream(extra)
    finally:
        p.cmd(b"ats6 3 0\r\n", wait=0.8, show=False)
        p.cmd(b"atc6\r\n", wait=0.5, show=False)


def q10_raw_can_listen(p):
    """Raw CAN (protocol 5), receive only, pass-all filter: what does a raw
    CAN frame look like on the wire, and what traffic reaches the OBD port?
    Nothing is transmitted. SNIFF_MODE (0x10000000) is tried second: the
    vendor header says it listens without acknowledging."""
    emit("\n### Q10. Raw CAN listen (no transmit): frame format and bus traffic")
    for flags, label in ((0, "normal"), (0x10000000, "SNIFF_MODE")):
        emit(f"\n  -- {label}: ato5 {flags} 500000 0 --")
        p.resync()
        r = p.cmd(f"ato5 {flags} 500000 0\r\n".encode(), wait=1.0, show=False)
        emit("  open -> " + r.decode("ascii", "replace").strip())
        if not r.startswith(b"aro"):
            continue
        r = p.cmd(b"atf5 1 0 4\r\n", struct.pack(">I", 0) + struct.pack(">I", 0), 1.0, show=False)
        emit("  pass-all filter -> " + r.decode("ascii", "replace").strip())
        buf = p.drain(3.0, idle=3.0)
        frames = count_frames(buf)
        emit(f"  {len(buf)} bytes, {frames} frame(s) in 3 s")
        show_stream(buf[:4000])
        if len(buf) > 4000:
            emit(f"      ... {len(buf) - 4000} more bytes in the RAW record")
        emit(f"  RAW listen={buf.hex()}")
        p.cmd(b"atc5\r\n", wait=0.5, show=False)


def q11_config_readback(p):
    """GET_CONFIG of every parameter id the vendor header names, on a live
    ISO15765 channel and on a K-line channel. Read-only; answers which ids
    the firmware knows and their defaults."""
    emit("\n### Q11. Configuration read-back (atg) on live channels")
    ids = list(range(0x01, 0x26)) + [0x8000]
    for proto, open_cmd in ((6, b"ato6 0 500000 0\r\n"), (4, b"ato4 0 10400 0\r\n")):
        emit(f"\n  -- channel {proto} --")
        p.resync()
        r = p.cmd(open_cmd, wait=1.0, show=False)
        if not r.startswith(b"aro"):
            emit(f"  open failed: {r!r}")
            continue
        for pid in ids:
            r = p.cmd(f"atg{proto} {pid}\r\n".encode(), wait=0.4, show=False)
            emit(f"  atg{proto} {pid:<6} -> {r.decode('ascii', 'replace').strip()!r}")
        p.cmd(f"atc{proto}\r\n".encode(), wait=0.5, show=False)


def main():
    global OUT
    ap = argparse.ArgumentParser(description="Read-only OpenPort protocol capture on a vehicle")
    ap.add_argument("--dev", default=DEFAULT_DEV)
    ap.add_argument("--out", default="protocol-capture.txt")
    ap.add_argument("--tx", type=lambda s: int(s, 0), default=0x7E0,
                    help="request CAN id (default 0x7E0, the engine ECU)")
    ap.add_argument("--rx", type=lambda s: int(s, 0), default=0x7E8)
    ap.add_argument("--only", default=None, help="run one section, e.g. q2")
    ap.add_argument("--kline", action="store_true",
                    help="also probe ISO9141/ISO14230 on pin 7 (highest-value "
                         "open question; sends a standard init pattern)")
    ap.add_argument("--kline-variants", default=None,
                    help="comma-separated subset of " + ",".join(v[0] for v in KLINE_INITS))
    ap.add_argument("--unknown-verbs", action="store_true",
                    help="run Q5 (atm/atw/atx, meaning unknown) — bench only")
    ap.add_argument("--request", default=None,
                    help="live mode: one guarded ISO15765 request as hex, e.g. 22F190")
    ap.add_argument("--kline-init", default=None,
                    help="live mode: run one K-line init variant, e.g. fast01")
    ap.add_argument("--kline-request", default="",
                    help="live mode: request hex to send after --kline-init, e.g. 1A9B")
    ap.add_argument("--no-preflight", action="store_true",
                    help="skip the voltage/TesterPresent pre-flight (bench use only)")
    ap.add_argument("--tp20", action="store_true",
                    help="open a VW TP2.0 channel over raw CAN and run the read set inside it; "
                         "this permits transmitting on 0x200 and on the id the module returns")
    ap.add_argument("--tp20-dest", default="0x01,0x19",
                    help="TP2.0 module logical addresses to try in order "
                         "(default the engine 0x01, then the gateway 0x19: if the engine is "
                         "silent but the gateway answers, the transport is proven)")
    ap.add_argument("--allow-diag-session", action="store_true",
                    help="permit exactly one session change, 10 89 (the VW diagnostic session "
                         "VCDS opens); programming sessions stay refused")
    ap.add_argument("--long-read", type=lambda s: int(s, 0), default=None, metavar="ADDR",
                    help="opt-in: one $23 read of 255 bytes at ADDR on CAN (and on the K-line "
                         "with --long-read-kline); ADDR must be mapped memory on this ECU")
    ap.add_argument("--long-read-kline", default=None, metavar="VARIANT",
                    help="K-line init variant to use for --long-read, e.g. fast01")
    args = ap.parse_args()

    if not os.path.exists(args.dev):
        print(f"device {args.dev} not found; is the cable plugged in?", file=sys.stderr)
        return 2

    OUT = open(args.out, "w")
    emit("OpenPort protocol capture — READ ONLY")
    emit(f"device={args.dev} tx=0x{args.tx:03X} rx=0x{args.rx:03X} "
         f"time={time.strftime('%Y-%m-%d %H:%M:%S')}")
    emit("Nothing here writes to an ECU or applies programming voltage.")
    emit(f"safe CAN ids 0x{SAFE_TX_LO:03X}-0x{SAFE_TX_HI:03X}; services "
         + " ".join(f"{k:02X}" for k in sorted(SAFE_SERVICES)))

    variants = args.kline_variants.split(",") if args.kline_variants else None
    global ALLOWED_SESSION
    if args.allow_diag_session:
        ALLOWED_SESSION = 0x89
        emit("session 10 89 (VW diagnostic session) ALLOWED for this run by the operator")
    live = (args.request is not None or args.kline_init is not None
            or args.long_read is not None or args.tp20)
    if args.tp20:
        TP20_EXTRA_TX.add(TP20_SETUP_ID)
        emit(f"TP2.0 ENABLED: transmitting on 0x{TP20_SETUP_ID:03X} (channel setup) is "
             "permitted for this run, plus whichever id the module returns. The "
             "read-only service whitelist is unchanged.")

    p = Port(args.dev)
    # Ordered cheapest-and-safest first, so an early abort still leaves the
    # most valuable data captured.
    sections = [("q7", q7_pins, ()),
                ("q0", q0_generic_obd, (args.rx,)),
                ("q1", q1_received_frames, (args.tx, args.rx)),
                ("q2", q2_kline, (variants,)),
                ("q3", q3_txflags, (args.tx, args.rx)),
                ("q8", q8_bus_errors, (args.tx,)),
                ("q9", q9_loopback, (args.tx, args.rx)),
                ("q10", q10_raw_can_listen, ()),
                ("q11", q11_config_readback, ()),
                ("q4", q4_periodic, (args.tx,)),
                ("q5", q5_unknown, ()),
                ("q6", q6_protocols, ())]
    rc = 0
    try:
        need_preflight = not args.no_preflight and (live or not args.only)
        if need_preflight and not preflight(p, args.tx, args.rx):
            emit("\nAborted at pre-flight. Nothing was transmitted onto the bus.")
            return 3
        if live:
            try:
                if args.request is not None and not args.tp20:
                    live_request(p, args.tx, args.rx, bytes.fromhex(args.request))
                if args.kline_init is not None:
                    live_kline(p, args.kline_init, bytes.fromhex(args.kline_request))
                if args.long_read is not None:
                    long_read(p, args.tx, args.rx, args.long_read, args.long_read_kline)
                if args.tp20:
                    q12_tp20(p, [int(x, 0) for x in args.tp20_dest.split(",")],
                             bytes.fromhex(args.request) if args.request else None)
            except Unsafe as e:
                emit(f"  !! REFUSED: {e}")
                rc = 4
            return rc
        for name, fn, fnargs in sections:
            if args.only and args.only != name:
                continue
            if name == "q2" and not args.kline:
                emit("\n### Q2. K-line — skipped (pass --kline to run it)")
                continue
            if name == "q5" and not args.unknown_verbs:
                emit("\n### Q5. unknown verbs — skipped (bench only; pass --unknown-verbs)")
                continue
            try:
                fn(p, *fnargs)
            except Unsafe as e:
                emit(f"  !! REFUSED: {e}")
            except Exception as e:
                emit(f"  !! {name} raised {e!r} — continuing")
    except KeyboardInterrupt:
        emit("\n!! interrupted — shutting the cable down cleanly")
        rc = 130
    finally:
        safe_shutdown(p)
        p.close()
        emit(f"\nsaved to {args.out}")
        OUT.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())
