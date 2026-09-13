#!/usr/bin/env python3
"""
run_scenarios — drive the real libj2534 against the simulator.

This is the layer that replaces a vehicle. The library under test is the
shipped dylib, loaded exactly as an application loads it; only the transport is
pointed at a pty instead of USB. Everything above the transport — reassembly,
reply matching, filters, timeouts, the entry points themselves — is the real
code on the real path.

It covers what a bench with one cable cannot reach at all: a successful
transmit, a received message, multi-frame reassembly. And then it covers what a
vehicle cannot reach either, because a working car never misbehaves on demand:
stale reply backlogs, dropped replies, truncated frames, stream garbage, and a
cable that vanishes mid-transfer.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import contextlib
import ctypes
import struct
import io
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from openport_sim import spawn, Faults          # noqa: E402

U32 = ctypes.c_ulong

ISO15765            = 6
FLOW_CONTROL_FILTER = 3
SET_CONFIG          = 2
READ_VBATT          = 3
CLEAR_RX_BUFFER     = 8
ISO15765_BS         = 0x1E
ISO15765_STMIN      = 0x1F
ISO15765_FRAME_PAD  = 0x40
TX_MSG_TYPE         = 0x01
START_OF_MESSAGE    = 0x02

STATUS_NOERROR           = 0
ERR_NOT_SUPPORTED        = 1
ERR_FAILED               = 7
ERR_DEVICE_NOT_CONNECTED = 8
ERR_TIMEOUT              = 9
ERR_BUFFER_EMPTY         = 16


class PASSTHRU_MSG(ctypes.Structure):
    _fields_ = [("ProtocolID", U32), ("RxStatus", U32), ("TxFlags", U32),
                ("Timestamp", U32), ("DataSize", U32), ("ExtraDataIndex", U32),
                ("Data", ctypes.c_ubyte * 4128)]


class SCONFIG(ctypes.Structure):
    _fields_ = [("Parameter", U32), ("Value", U32)]


class SCONFIG_LIST(ctypes.Structure):
    _fields_ = [("NumOfParams", U32), ("ConfigPtr", ctypes.POINTER(SCONFIG))]


def load(lib_path):
    d = ctypes.CDLL(lib_path)
    for n in ("PassThruOpen", "PassThruClose", "PassThruConnect", "PassThruDisconnect",
              "PassThruReadMsgs", "PassThruWriteMsgs", "PassThruStartMsgFilter",
              "PassThruStopMsgFilter", "PassThruIoctl", "PassThruReadVersion",
              "PassThruGetLastError", "PassThruStartPeriodicMsg",
              "PassThruStopPeriodicMsg", "PassThruSetProgrammingVoltage"):
        getattr(d, n).restype = ctypes.c_long
    d.PassThruOpen.argtypes = [ctypes.c_void_p, ctypes.POINTER(U32)]
    d.PassThruConnect.argtypes = [U32, U32, U32, U32, ctypes.POINTER(U32)]
    d.PassThruReadMsgs.argtypes = [U32, ctypes.POINTER(PASSTHRU_MSG), ctypes.POINTER(U32), U32]
    d.PassThruWriteMsgs.argtypes = [U32, ctypes.POINTER(PASSTHRU_MSG), ctypes.POINTER(U32), U32]
    d.PassThruStartMsgFilter.argtypes = [U32, U32, ctypes.POINTER(PASSTHRU_MSG),
                                         ctypes.POINTER(PASSTHRU_MSG),
                                         ctypes.POINTER(PASSTHRU_MSG), ctypes.POINTER(U32)]
    d.PassThruIoctl.argtypes = [U32, U32, ctypes.c_void_p, ctypes.c_void_p]
    d.PassThruGetLastError.argtypes = [ctypes.c_char_p]
    return d


PASS = 0
FAIL = 0
NOTES = []


def check(cond, what, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"    ok    {what}")
    else:
        FAIL += 1
        print(f"    FAIL  {what} {detail}")


def msg(data, txflags=0):
    m = PASSTHRU_MSG()
    m.ProtocolID = ISO15765
    m.TxFlags = txflags
    m.DataSize = len(data)
    for i, b in enumerate(data):
        m.Data[i] = b
    return m


def connect(d, dev):
    ch = U32()
    rc = d.PassThruConnect(dev, ISO15765, 0, 500000, ctypes.byref(ch))
    if rc != 0:
        return None, rc
    cfg = (SCONFIG * 2)(SCONFIG(ISO15765_BS, 0), SCONFIG(ISO15765_STMIN, 0))
    lst = SCONFIG_LIST(2, cfg)
    d.PassThruIoctl(ch, SET_CONFIG, ctypes.byref(lst), None)
    mask = msg(b"\xff\xff\xff\xff")
    pat  = msg(b"\x00\x00\x07\xe8")
    fc   = msg(b"\x00\x00\x07\xe0")
    fid = U32()
    d.PassThruStartMsgFilter(ch, FLOW_CONTROL_FILTER, ctypes.byref(mask),
                             ctypes.byref(pat), ctypes.byref(fc), ctypes.byref(fid))
    return ch, 0


ERR_TIMEOUT = 9


def read_ok(rc):
    """A read that asks for more messages than arrive returns ERR_TIMEOUT with
    the messages that did arrive, per J2534. Both are healthy outcomes here."""
    return rc in (0, ERR_TIMEOUT)


def last_error(d):
    buf = ctypes.create_string_buffer(80)
    d.PassThruGetLastError(buf)
    return buf.value.decode("ascii", "replace")


def scenario(name, ecu=None, faults=None):
    """Start a simulator, point the library at it, yield (lib, deviceid)."""
    print(f"\n== {name} ==")
    path, stop, th, sim = spawn(ecu=ecu, faults=faults)
    os.environ["OPENPORT_DEVICE"] = path
    return path, stop, th, sim


LIB = os.environ.get("OPENPORT_LIB",
                     os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                  "..", "..", "libj2534.dylib"))


def fresh_lib():
    """A new handle each scenario: the library holds one device at a time."""
    return load(os.path.abspath(LIB))


# ---------------------------------------------------------------------------

def s_happy_path():
    ecu = {b"\x3e\x00": b"\x7e\x00"}
    path, stop, th, sim = scenario("happy path: transmit, ECU answers, we receive", ecu=ecu)
    d = fresh_lib()
    dev = U32()
    check(d.PassThruOpen(None, ctypes.byref(dev)) == 0, "PassThruOpen")

    fw = ctypes.create_string_buffer(80); dl = ctypes.create_string_buffer(80)
    ap = ctypes.create_string_buffer(80)
    d.PassThruReadVersion(dev, fw, dl, ap)
    check(fw.value == b"1.17.4877", "firmware version", fw.value)

    v = U32()
    check(d.PassThruIoctl(dev, READ_VBATT, None, ctypes.byref(v)) == 0, "READ_VBATT")
    check(v.value == 12480, "battery reads 12480 mV", v.value)

    ch, rc = connect(d, dev)
    check(ch is not None, "Connect + SET_CONFIG + filter", rc)

    n = U32(1)
    m = msg(b"\x00\x00\x07\xe0\x3e\x00", ISO15765_FRAME_PAD)
    rc = d.PassThruWriteMsgs(ch, ctypes.byref(m), ctypes.byref(n), 1000)
    check(rc == 0, "WriteMsgs succeeds when the bus acknowledges", rc)
    check(n.value == 1, "one message reported sent")

    inbuf = (PASSTHRU_MSG * 4)()
    cnt = U32(4)
    rc = d.PassThruReadMsgs(ch, inbuf, ctypes.byref(cnt), 1000)
    check(read_ok(rc), "ReadMsgs returns the ECU reply", f"rc={rc} {last_error(d)}")
    check(cnt.value >= 1, "at least one message", cnt.value)
    if cnt.value:
        got = bytes(inbuf[0].Data[:inbuf[0].DataSize])
        check(got == b"\x00\x00\x07\xe8\x7e\x00",
              "payload is CAN id + positive response", got.hex(" "))
        check(inbuf[0].RxStatus == 0, "a complete message carries no indication bits",
              hex(inbuf[0].RxStatus))
        check(inbuf[0].Timestamp > 0, "timestamp populated")
    d.PassThruClose(dev)
    stop.set()


def s_multiframe():
    body = bytes((i * 7 + 3) & 0xFF for i in range(600))
    ecu = {b"\x22\x01\x00": body}
    path, stop, th, sim = scenario("multi-frame: 600-byte response reassembles", ecu=ecu)
    d = fresh_lib()
    dev = U32(); d.PassThruOpen(None, ctypes.byref(dev))
    ch, _ = connect(d, dev)
    n = U32(1)
    m = msg(b"\x00\x00\x07\xe0\x22\x01\x00", ISO15765_FRAME_PAD)
    d.PassThruWriteMsgs(ch, ctypes.byref(m), ctypes.byref(n), 1000)

    inbuf = (PASSTHRU_MSG * 4)(); cnt = U32(4)
    rc = d.PassThruReadMsgs(ch, inbuf, ctypes.byref(cnt), 1500)
    check(read_ok(rc), "ReadMsgs returns", f"rc={rc}")
    check(cnt.value == 2, "one announcement, then 600 bytes as ONE message", cnt.value)
    if cnt.value == 2:
        ind, data = inbuf[0], inbuf[1]
        check(ind.RxStatus == START_OF_MESSAGE and ind.DataSize == 4,
              "announcement is a first-frame indication carrying the CAN id only",
              f"status={ind.RxStatus:#x} size={ind.DataSize}")
        got = bytes(data.Data[:data.DataSize])
        check(data.RxStatus == 0, "data message carries no indication bits", hex(data.RxStatus))
        check(len(got) == 604, "length = 4-byte CAN id once + 600", len(got))
        check(got[4:] == body, "payload reassembled in order and intact")
        check(data.ExtraDataIndex == data.DataSize, "ExtraDataIndex set")
    d.PassThruClose(dev)
    stop.set()


def s_no_ack():
    f = Faults(); f.no_ack = True; f.tx_reject_delay = 0.3
    path, stop, th, sim = scenario("no ACK (a bench with one node): must not claim success",
                                   faults=f)
    d = fresh_lib()
    dev = U32(); d.PassThruOpen(None, ctypes.byref(dev))
    ch, _ = connect(d, dev)
    n = U32(1)
    m = msg(b"\x00\x00\x07\xe0\x3e\x00", ISO15765_FRAME_PAD)
    rc = d.PassThruWriteMsgs(ch, ctypes.byref(m), ctypes.byref(n), 1500)
    check(rc == ERR_TIMEOUT, "WriteMsgs reports ERR_TIMEOUT", rc)
    check(n.value == 0, "zero messages reported sent", n.value)
    inbuf = (PASSTHRU_MSG * 2)(); cnt = U32(2)
    rc = d.PassThruReadMsgs(ch, inbuf, ctypes.byref(cnt), 200)
    check(rc == ERR_BUFFER_EMPTY, "ReadMsgs reports the buffer empty", rc)
    check(cnt.value == 0, "and invents no messages", cnt.value)
    d.PassThruClose(dev)
    stop.set()


def s_backlog():
    ecu = {b"\x3e\x00": b"\x7e\x00"}
    f = Faults(); f.startup_backlog = 7
    path, stop, th, sim = scenario("stale reply backlog: 7 deep, as seen on real hardware",
                                   ecu=ecu, faults=f)
    d = fresh_lib()
    dev = U32()
    rc = d.PassThruOpen(None, ctypes.byref(dev))
    check(rc == 0, "PassThruOpen drains the backlog and succeeds", rc)
    fw = ctypes.create_string_buffer(80); dl = ctypes.create_string_buffer(80)
    ap = ctypes.create_string_buffer(80)
    d.PassThruReadVersion(dev, fw, dl, ap)
    check(fw.value == b"1.17.4877", "version is THIS command's answer, not a stale one", fw.value)
    v = U32()
    d.PassThruIoctl(dev, READ_VBATT, None, ctypes.byref(v))
    check(v.value == 12480, "battery is this command's answer", v.value)
    d.PassThruClose(dev)
    stop.set()


def s_dropped_reply():
    ecu = {b"\x3e\x00": b"\x7e\x00"}
    f = Faults(); f.drop_reply_every = 4
    path, stop, th, sim = scenario("device swallows every 4th reply: no result may shift",
                                   ecu=ecu, faults=f)
    d = fresh_lib()
    dev = U32()
    rc = d.PassThruOpen(None, ctypes.byref(dev))
    if rc != 0:
        check(True, "Open failed cleanly rather than hanging", rc)
        stop.set(); return
    results = []
    for _ in range(8):
        v = U32(0)
        r = d.PassThruIoctl(dev, READ_VBATT, None, ctypes.byref(v))
        results.append((r, v.value))
    bad = [r for r in results if r[0] == 0 and r[1] != 12480]
    check(not bad, "every successful read returned ITS OWN value", bad)
    check(any(r[0] != 0 for r in results), "the dropped replies surfaced as failures")
    d.PassThruClose(dev)
    stop.set()


def s_disconnect():
    f = Faults(); f.disconnect_after = 6
    path, stop, th, sim = scenario("cable vanishes mid-session: must not hang", faults=f)
    d = fresh_lib()
    dev = U32()
    t0 = time.time()
    rc = d.PassThruOpen(None, ctypes.byref(dev))
    for _ in range(4):
        v = U32()
        rc = d.PassThruIoctl(dev, READ_VBATT, None, ctypes.byref(v))
        if rc != 0:
            break
    dt = time.time() - t0
    check(rc != 0, "an operation after the disconnect fails", rc)
    check(rc in (ERR_TIMEOUT, ERR_DEVICE_NOT_CONNECTED, ERR_FAILED),
          "with a sensible code", rc)
    check(dt < 20, f"and returns promptly ({dt:.1f}s)", dt)
    d.PassThruClose(dev)
    stop.set()


def s_truncated():
    ecu = {b"\x3e\x00": b"\x7e\x00"}
    f = Faults(); f.truncate_frames = True
    path, stop, th, sim = scenario("truncated frames: no crash, no fabricated message",
                                   ecu=ecu, faults=f)
    d = fresh_lib()
    dev = U32()
    rc = d.PassThruOpen(None, ctypes.byref(dev))
    if rc != 0:
        check(True, "Open failed cleanly on a corrupt stream", rc)
        stop.set(); return
    ch, _ = connect(d, dev)
    if ch is not None:
        n = U32(1)
        m = msg(b"\x00\x00\x07\xe0\x3e\x00", ISO15765_FRAME_PAD)
        d.PassThruWriteMsgs(ch, ctypes.byref(m), ctypes.byref(n), 800)
        inbuf = (PASSTHRU_MSG * 4)(); cnt = U32(4)
        rc = d.PassThruReadMsgs(ch, inbuf, ctypes.byref(cnt), 600)
        check(rc in (0, ERR_BUFFER_EMPTY, ERR_TIMEOUT), "ReadMsgs returns a valid code", rc)
        for i in range(cnt.value):
            check(inbuf[i].DataSize <= 4128, "no message claims an impossible length",
                  inbuf[i].DataSize)
    check(True, "survived a truncated-frame stream without crashing")
    d.PassThruClose(dev)
    stop.set()


def s_garbage():
    ecu = {b"\x3e\x00": b"\x7e\x00"}
    f = Faults(); f.garbage_bytes = 5
    path, stop, th, sim = scenario("stream garbage before every reply: must resync",
                                   ecu=ecu, faults=f)
    d = fresh_lib()
    dev = U32()
    rc = d.PassThruOpen(None, ctypes.byref(dev))
    # A stream this corrupt is genuinely unusable; what matters is that the
    # driver says so accurately rather than blaming the cable.
    check(rc in (0, ERR_FAILED, ERR_TIMEOUT),
          "Open reports a protocol failure, not 'device not connected'", rc)
    if rc == ERR_FAILED:
        check("not answering the protocol" in last_error(d),
              "and the message points at the link, not the cable", last_error(d))
    if rc == 0:
        v = U32(0)
        r = d.PassThruIoctl(dev, READ_VBATT, None, ctypes.byref(v))
        check(r != 0 or v.value == 12480,
              "any successful read is still correct despite the noise", v.value)
        d.PassThruClose(dev)
    check(True, "survived a noisy stream without crashing")
    stop.set()


def s_loopback():
    ecu = {}
    path, stop, th, sim = scenario("transmit echo is flagged, not mistaken for a reply")
    d = fresh_lib()
    dev = U32(); d.PassThruOpen(None, ctypes.byref(dev))
    ch, _ = connect(d, dev)
    cfg = (SCONFIG * 1)(SCONFIG(3, 1))          # LOOPBACK on
    lst = SCONFIG_LIST(1, cfg)
    d.PassThruIoctl(ch, SET_CONFIG, ctypes.byref(lst), None)
    n = U32(1)
    m = msg(b"\x00\x00\x07\xe0\x3e\x00", ISO15765_FRAME_PAD)
    d.PassThruWriteMsgs(ch, ctypes.byref(m), ctypes.byref(n), 1000)
    inbuf = (PASSTHRU_MSG * 4)(); cnt = U32(4)
    rc = d.PassThruReadMsgs(ch, inbuf, ctypes.byref(cnt), 800)
    check(read_ok(rc) and cnt.value >= 1, "loopback frame received",
          f"rc={rc} n={cnt.value}")
    for i in range(cnt.value):
        check(inbuf[i].RxStatus & TX_MSG_TYPE, "TX_MSG_TYPE set on every echo message",
              hex(inbuf[i].RxStatus))
        check(not (inbuf[i].RxStatus & START_OF_MESSAGE),
              "an echo is never a first-frame marker", hex(inbuf[i].RxStatus))
    d.PassThruClose(dev)
    stop.set()


def s_periodic_pacing():
    """
    A periodic keep-alive must keep its period, and must never fire a burst.

    Two failure modes, both real before this was fixed:
      - drift: scheduling the next send as now+interval AFTER the transmit
        returns makes each period interval+transmit_time, so a 50 ms keep-alive
        silently becomes 90 ms and an ECU times the session out.
      - stampede: if a transmit blocks and the deadline passes several times
        over, "catching up" puts a clump of frames on a vehicle bus at once.
    """
    ecu = {b"\x3e\x00": b"\x7e\x00"}
    f = Faults(); f.reply_delay = 0.030      # a transmit takes real time
    path, stop, th, sim = scenario("periodic keep-alive: keeps its period, never bursts",
                                   ecu=ecu, faults=f)
    d = fresh_lib()
    dev = U32(); d.PassThruOpen(None, ctypes.byref(dev))
    ch, _ = connect(d, dev)

    m = msg(b"\x00\x00\x07\xe0\x3e\x00", ISO15765_FRAME_PAD)
    mid = U32()
    d.PassThruStartPeriodicMsg.argtypes = [U32, ctypes.POINTER(PASSTHRU_MSG),
                                           ctypes.POINTER(U32), U32]
    d.PassThruStopPeriodicMsg.argtypes = [U32, U32]
    rc = d.PassThruStartPeriodicMsg(ch, ctypes.byref(m), ctypes.byref(mid), 50)
    check(rc == 0, "StartPeriodicMsg at 50 ms", rc)

    sim.tx_times.clear()
    time.sleep(1.2)
    d.PassThruStopPeriodicMsg(ch, mid)
    times = list(sim.tx_times)

    check(len(times) >= 8, f"transmitted repeatedly ({len(times)} in 1.2 s)", len(times))
    if len(times) >= 3:
        gaps = [(b - a) * 1000.0 for a, b in zip(times, times[1:])]
        gaps.sort()
        median = gaps[len(gaps) // 2]
        fastest = gaps[0]
        check(35 <= median <= 75,
              f"median period is near 50 ms (got {median:.0f} ms)", f"{median:.0f}")
        # A stampede shows up as gaps far below the interval.
        check(fastest >= 10,
              f"no burst: closest pair {fastest:.0f} ms apart", f"{fastest:.0f}")

    time.sleep(0.3)
    before = len(sim.tx_times)
    time.sleep(0.4)
    check(len(sim.tx_times) == before, "nothing transmits after Stop",
          len(sim.tx_times) - before)
    d.PassThruClose(dev)
    stop.set()


# ---------------------------------------------------------------------------
# A real consumer's acceptance rule. The receive framing was once modelled
# wrongly and every in-repo test agreed with the model; the only check that
# could have caught it is the rule a consuming application applies to what
# PassThruReadMsgs returns. Replicated here, over the exchange a 2012 VW Caddy
# actually produced, so the check always runs.
# ---------------------------------------------------------------------------
CONSUMER_TX_ID = b"\x00\x00\x07\xe0"
CONSUMER_RX_ID = b"\x00\x00\x07\xe8"
ISO15765_PADDING_ERROR = 0x10

CONSUMER_ECU = {
    b"\x3e\x00": b"\x7e\x00",
    b"\x22\xf1\x90": b"\x62\xf1\x90" + b"WVWZZZ1JZ3W386752",
    b"\x22\xf1\x8c": b"\x62\xf1\x8c" + b"00000000000000",
    b"\x23\x00\x80\x00\x40": b"\x7f\x23\x11",
}


def consumer_read_one(d, ch, timeout_ms=1000):
    """One ReadMsgs, filtered the way a consumer filters: no echoes, no
    indications, at least a CAN id, and only the id the flow-control filter
    was armed for."""
    m = PASSTHRU_MSG(); n = U32(1)
    rc = d.PassThruReadMsgs(ch, ctypes.byref(m), ctypes.byref(n), timeout_ms)
    if rc != 0 or n.value == 0:
        return None
    size = int(m.DataSize)
    data = bytes(m.Data[:size])
    if m.RxStatus & (TX_MSG_TYPE | START_OF_MESSAGE | ISO15765_PADDING_ERROR):
        return None
    if size < 4:
        return None
    if data[:4] != CONSUMER_RX_ID:
        return None
    return data[4:]


def consumer_request(d, ch, payload):
    """Drain, write, read until the positive or negative response."""
    while consumer_read_one(d, ch, 20) is not None:
        pass
    n = U32(1)
    m = msg(CONSUMER_TX_ID + payload, ISO15765_FRAME_PAD)
    rc = d.PassThruWriteMsgs(ch, ctypes.byref(m), ctypes.byref(n), 1000)
    if rc != 0:
        return None
    deadline = time.time() + 2.0
    while time.time() < deadline:
        resp = consumer_read_one(d, ch)
        if resp is None:
            continue
        if resp[0] == 0x7F or resp[0] == payload[0] + 0x40:
            return resp
    return None


def s_consumer_rule():
    path, stop, th, sim = scenario("real consumer: the acceptance rule over a Caddy exchange", ecu=CONSUMER_ECU)
    d = fresh_lib()
    dev = U32(); d.PassThruOpen(None, ctypes.byref(dev))
    ch = U32()
    d.PassThruConnect(dev, ISO15765, 0, 500000, ctypes.byref(ch))
    cfg = (SCONFIG * 2)(SCONFIG(ISO15765_BS, 0), SCONFIG(ISO15765_STMIN, 0))
    lst = SCONFIG_LIST(2, cfg)
    d.PassThruIoctl(ch, SET_CONFIG, ctypes.byref(lst), None)
    f = ISO15765_FRAME_PAD
    mask, pat, fc = msg(b"\xff\xff\xff\xff", f), msg(CONSUMER_RX_ID, f), msg(CONSUMER_TX_ID, f)
    fid = U32()
    d.PassThruStartMsgFilter(ch, FLOW_CONTROL_FILTER, ctypes.byref(mask), ctypes.byref(pat),
                             ctypes.byref(fc), ctypes.byref(fid))
    for req, want in CONSUMER_ECU.items():
        got = consumer_request(d, ch, req)
        check(got == want, f"{req.hex(' ')} -> the consumer sees the {len(want)}-byte response",
              None if got is None else got.hex(" "))
    d.PassThruClose(dev)
    stop.set()


# ---------------------------------------------------------------------------
# VW TP2.0. The Audi answered nothing on ISO15765 because VAG does not use it;
# the capture script speaks TP2.0 over the driver's raw CAN channel instead.
# That layer must not reach a car untested, so it runs here against a simulated
# module: channel setup, parameter exchange, a single-frame read, a multi-frame
# read that has to be reassembled, and the refusal of anything that writes.
# ---------------------------------------------------------------------------
def s_tp20():
    counters = bytes.fromhex("5a9c0007000300000000")
    ident = b"\x5a\x9b" + b"03G906021LR  1.9 TDI  0001".ljust(30, b"\x20")
    ecu = {b"\x1a\x9c": counters, b"\x1a\x9b": ident, b"\x3e": b"\x7e"}
    path, stop, th, sim = scenario("VW TP2.0: channel setup, reads, reassembly", ecu=ecu)
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    "..", "..", "tools", "car"))
    import car_capture as C
    C.OUT = None
    C.TP20_EXTRA_TX.clear()
    C.TP20_EXTRA_TX.add(C.TP20_SETUP_ID)

    p = C.Port(path)
    t = C.Tp20(p)
    log = io.StringIO()
    try:
        with contextlib.redirect_stdout(log):
            opened = t.open()
            single = t.request(b"\x1a\x9c") if opened else None
            multi = t.request(b"\x1a\x9b") if opened else None
            try:
                t.request(b"\x2e\x01\x00")
                refused = False
            except C.Unsafe:
                refused = True
            try:
                t.request(b"\x34\x00\x00\x00")
                refused_dl = False
            except C.Unsafe:
                refused_dl = True
            t.close()
        check(opened, "channel opens against a simulated VAG module")
        check(t.rx_id == 0x300 and t.tx_id == 0x740,
              "the module names the id pair", f"rx={t.rx_id} tx={t.tx_id}")
        check(single == counters, "single-frame read round-trips",
              single.hex(" ") if single else None)
        check(multi == ident, "multi-frame read is reassembled intact",
              f"{len(multi) if multi else 0} of {len(ident)} bytes")
        check(refused, "a write service is refused inside the channel")
        check(refused_dl, "RequestDownload is refused inside the channel")
        check(C.TP20_EXTRA_TX == {C.TP20_SETUP_ID, 0x740},
              "only the setup id and the module's own id were unlocked",
              sorted(hex(x) for x in C.TP20_EXTRA_TX))
    finally:
        p.close()
        stop.set()


def s_tp20_silent_module():
    """No module at the requested address: the channel must fail cleanly rather
    than hang or half-open, and nothing may be unlocked beyond the setup id."""
    path, stop, th, sim = scenario("VW TP2.0: no module at that address", ecu={})
    sim.tp20_modules = set()
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    "..", "..", "tools", "car"))
    import car_capture as C
    C.OUT = None
    C.TP20_EXTRA_TX.clear()
    C.TP20_EXTRA_TX.add(C.TP20_SETUP_ID)
    p = C.Port(path)
    t = C.Tp20(p)
    log = io.StringIO()
    try:
        t0 = time.time()
        with contextlib.redirect_stdout(log):
            opened = t.open()
            t.close()
        check(not opened, "open() reports failure")
        check(t.tx_id is None, "no transmit id was adopted")
        check(C.TP20_EXTRA_TX == {C.TP20_SETUP_ID}, "nothing extra was unlocked",
              sorted(hex(x) for x in C.TP20_EXTRA_TX))
        check(time.time() - t0 < 15, "it gives up promptly", f"{time.time()-t0:.1f}s")
    finally:
        p.close()
        stop.set()


# ---------------------------------------------------------------------------
# ISO 15765-4 clause 8.1: every diagnostic CAN frame carries a DLC of eight,
# and a receiver shall ignore one that does not. The OpenPort pads only when
# ISO15765_FRAME_PAD is set, so forgetting that flag makes a real ECU silent —
# which is exactly what happened on a 2009 Audi and a 2012 VW Caddy before the
# flag was set. This proves the simulated ECU enforces the rule and that the
# capture tool now satisfies it, so the mistake cannot come back unnoticed.
# ---------------------------------------------------------------------------
def s_frame_pad():
    ecu = {b"\x01\x00": b"\x41\x00\x98\x3b\xa0\x13"}
    path, stop, th, sim = scenario("ISO 15765-4: an unpadded request is ignored", ecu=ecu)
    d = fresh_lib()
    dev = U32(); d.PassThruOpen(None, ctypes.byref(dev))
    ch, _ = connect(d, dev)

    def ask(txflags):
        n = U32(1)
        m = msg(b"\x00\x00\x07\xe0\x01\x00", txflags)
        d.PassThruWriteMsgs(ch, ctypes.byref(m), ctypes.byref(n), 1000)
        buf = (PASSTHRU_MSG * 4)(); cnt = U32(4)
        rc = d.PassThruReadMsgs(ch, buf, ctypes.byref(cnt), 700)
        for i in range(cnt.value):
            body = bytes(buf[i].Data[:buf[i].DataSize])
            if not (buf[i].RxStatus & (TX_MSG_TYPE | START_OF_MESSAGE)) and len(body) > 4:
                return body[4:]
        return None

    check(ask(0) is None, "unpadded request gets no answer, as the standard requires")
    got = ask(ISO15765_FRAME_PAD)
    check(got == b"\x41\x00\x98\x3b\xa0\x13",
          "the same request padded is answered", got.hex(" ") if got else None)
    d.PassThruClose(dev)
    stop.set()


def s_capture_tool_pads():
    """The capture tool's own default, against an ECU that enforces the rule.
    This is the regression for the defect that made a live session look dead."""
    ecu = {b"\x01\x00": b"\x41\x00\x98\x3b\xa0\x13"}
    path, stop, th, sim = scenario("the capture tool pads by default", ecu=ecu)
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    "..", "..", "tools", "car"))
    import car_capture as C
    C.OUT = None
    p = C.Port(path)
    log = io.StringIO()
    try:
        with contextlib.redirect_stdout(log):
            p.resync()
            p.cmd(b"ato6 0 500000 0\r\n", wait=1.0, show=False)
            p.cmd(b"atf6 3 64 4\r\n",
                  struct.pack(">I", 0xFFFFFFFF) + struct.pack(">I", 0x7E8)
                  + struct.pack(">I", 0x7E0), 1.0, show=False)
            raw = p.transmit(6, 0x7E0, b"\x01\x00", wait=1.0)
        frames = [e for e in C.parse_frames(raw)
                  if e[0] == "FRAME" and not (e[3] & 0x10)]
        check(C.CAN_FRAME_PAD == 0x40, "the tool knows the pad flag")
        check(bool(frames), "a default transmit is answered by a conforming ECU",
              f"{len(frames)} data frame(s)")
        if frames:
            check(frames[0][5][4:] == b"\x41\x00\x98\x3b\xa0\x13",
                  "and the reply is intact", frames[0][5].hex(" "))
    finally:
        p.close()
        stop.set()


# ---------------------------------------------------------------------------
# How a reply longer than one wire frame (250 bytes) is chunked is the last
# open receive question, and no ECU reached so far will produce one. It can
# still be reasoned about: on the Caddy a segmented reply arrived as a START
# frame carrying the CAN id alone and an END frame carrying the id AGAIN plus
# the data, so within one message the id is repeated across frames. If that
# holds for every frame, a >250-byte reply repeats the id on each chunk and
# this driver — which concatenates frame bodies verbatim — would splice
# spurious 4-byte ids into the middle of the payload. opta-j2534-rs reads the
# wire that way too, stripping four bytes from every frame after the first.
# The driver therefore does not bet on either: a continuation frame beginning
# with the same four bytes already at the front of the message is repeating the
# id, and those bytes are dropped. Both models then reassemble identically,
# which is what this scenario asserts. A capture of a genuinely long reply
# would still be worth having, but no longer decides whether the data is right.
# ---------------------------------------------------------------------------
def s_chunking_models():
    body = bytes((i * 7 + 3) & 0xFF for i in range(600))
    for model, expect_clean in (("id_first_only", True), ("id_every_chunk", True)):
        ecu = {b"\x22\x01\x00": body}
        path, stop, th, sim = scenario(f"600-byte reply, chunking={model}", ecu=ecu)
        sim.wire.chunking = model
        d = fresh_lib()
        dev = U32(); d.PassThruOpen(None, ctypes.byref(dev))
        ch, _ = connect(d, dev)
        n = U32(1)
        m = msg(b"\x00\x00\x07\xe0\x22\x01\x00", ISO15765_FRAME_PAD)
        d.PassThruWriteMsgs(ch, ctypes.byref(m), ctypes.byref(n), 1000)
        inbuf = (PASSTHRU_MSG * 8)(); cnt = U32(8)
        d.PassThruReadMsgs(ch, inbuf, ctypes.byref(cnt), 2000)
        data = None
        for i in range(cnt.value):
            if not (inbuf[i].RxStatus & (TX_MSG_TYPE | START_OF_MESSAGE)):
                data = bytes(inbuf[i].Data[:inbuf[i].DataSize])
        clean = data is not None and data == b"\x00\x00\x07\xe8" + body
        if expect_clean:
            check(clean, f"{model}: reassembles to the id once plus 600 bytes",
                  f"{len(data) if data else 0} bytes")
        else:
            check(clean, f"{model}: also reassembles cleanly", f"{len(data) if data else 0} bytes")
        d.PassThruClose(dev)
        stop.set()
        time.sleep(0.05)


SCENARIOS = [s_happy_path, s_multiframe, s_no_ack, s_backlog, s_dropped_reply,
             s_disconnect, s_truncated, s_garbage, s_loopback,
             s_periodic_pacing, s_consumer_rule, s_tp20, s_tp20_silent_module,
             s_frame_pad, s_capture_tool_pads,
             s_chunking_models]


def main():
    print(f"library under test: {os.path.abspath(LIB)}")
    only = sys.argv[1] if len(sys.argv) > 1 else None
    for s in SCENARIOS:
        if only and only not in s.__name__:
            continue
        try:
            s()
        except Exception as e:                      # a scenario must not abort the run
            global FAIL
            FAIL += 1
            print(f"    FAIL  {s.__name__} raised {e!r}")
        finally:
            os.environ.pop("OPENPORT_DEVICE", None)
            time.sleep(0.05)
    print(f"\n{PASS} passed, {FAIL} failed")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
