#!/usr/bin/env python3
"""
driver_multiframe — prove multi-frame ISO15765 reassembly through libj2534.

This is the gate to clear before trusting the driver with an ECU whose replies
matter. It goes through the shipped library's own C ABI, not through any
script's parser, and it judges the result the way a J2534 consumer does:

  * a segmented reply must arrive as a first-frame indication (4 bytes, the CAN
    id, ISO15765_FIRST_FRAME set) followed by exactly one clean message;
  * that message must carry the CAN id once, then the whole payload;
  * nothing else may survive a consumer's acceptance rule.

Read-only: the CAN id and the service both pass the capture tool's envelope.

  python3 tools/car/driver_multiframe.py --request 0902
  python3 tools/car/driver_multiframe.py --tx 0x7B5 --rx 0x7B4 --request 1A87

Exit status is the verdict, so a wrapper cannot mistake one outcome for another:
  0  one clean reassembled message, preceded by a first-frame indication
  3  the reply was dropped, duplicated or never arrived
  4  refused by the read-only envelope before anything was transmitted
  5  the ECU refused the request (transport worked; reassembly NOT proven)
  6  the reply fitted one CAN frame, so reassembly was NOT exercised

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import ctypes
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from car_capture import (guard_tx, guard_service, Unsafe, CAN_FRAME_PAD,  # noqa: E402
                         allow_tx)
import car_capture as _cc  # noqa: E402

U = ctypes.c_ulong
ISO15765, FLOW_CONTROL_FILTER, SET_CONFIG, CLEAR_RX_BUFFER = 6, 3, 0x02, 0x08
ISO15765_BS, ISO15765_STMIN = 0x1E, 0x1F
TX_MSG_TYPE, ISO15765_FIRST_FRAME, ISO15765_PADDING_ERROR = 0x01, 0x02, 0x10


class MSG(ctypes.Structure):
    _fields_ = [("ProtocolID", U), ("RxStatus", U), ("TxFlags", U),
                ("Timestamp", U), ("DataSize", U), ("ExtraDataIndex", U),
                ("Data", ctypes.c_ubyte * 4128)]


class SC(ctypes.Structure):
    _fields_ = [("Parameter", U), ("Value", U)]


class SCL(ctypes.Structure):
    _fields_ = [("NumOfParams", U), ("ConfigPtr", ctypes.POINTER(SC))]


def mk(cid, payload=b"", flags=CAN_FRAME_PAD):
    m = MSG()
    m.ProtocolID, m.TxFlags = ISO15765, flags
    data = struct.pack(">I", cid) + payload
    m.DataSize = len(data)
    for i, b in enumerate(data):
        m.Data[i] = b
    return m


def main():
    ap = argparse.ArgumentParser(description="Driver-level multi-frame ISO15765 check")
    ap.add_argument("--lib", default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                  "..", "..", "libj2534.dylib"))
    ap.add_argument("--tx", type=lambda s: int(s, 0), default=0x7E0)
    ap.add_argument("--rx", type=lambda s: int(s, 0), default=0x7E8)
    ap.add_argument("--flow", type=lambda s: int(s, 0), default=None,
                    help="CAN id the device sends flow control to. Defaults to --tx, but a "
                         "functional request (0x7DF) must be flow-controlled at the ECU's "
                         "PHYSICAL id or every multi-frame reply stalls after the first frame")
    ap.add_argument("--request", required=True, help="request bytes as hex, e.g. 0902")
    ap.add_argument("--baud", type=lambda s: int(s, 0), default=500000)
    ap.add_argument("--allow-tx", action="append", default=[], metavar="ID",
                    help="permit a transmit id outside the default 0x7DF-0x7FF range, "
                         "for a module that answers diagnostics on its own id. Repeatable")
    args = ap.parse_args()

    _cc.OUT = None
    for cid in args.allow_tx:
        allow_tx(int(cid, 0), "named on the command line by the operator")

    payload = bytes.fromhex(args.request)
    try:
        guard_tx(args.tx)
        guard_service(payload[0], f"driver multi-frame read to 0x{args.tx:03X}", payload)
    except Unsafe as e:
        print(f"REFUSED: {e}")
        return 4

    d = ctypes.CDLL(os.path.abspath(args.lib))
    d.PassThruReadMsgs.argtypes = [U, ctypes.POINTER(MSG), ctypes.POINTER(U), U]
    dev, ch, fid = U(), U(), U()
    if d.PassThruOpen(None, ctypes.byref(dev)) != 0:
        print("PassThruOpen failed"); return 1
    if d.PassThruConnect(dev, ISO15765, 0, args.baud, ctypes.byref(ch)) != 0:
        print("Connect failed"); d.PassThruClose(dev); return 1

    cfg = (SC * 2)(SC(ISO15765_BS, 0), SC(ISO15765_STMIN, 0))
    d.PassThruIoctl(ch, SET_CONFIG, ctypes.byref(SCL(2, cfg)), None)
    # A functional request is broadcast, but flow control is a point-to-point
    # answer to one ECU: it must go to that ECU's physical id. Defaulting this
    # to --tx makes every segmented reply to a 0x7DF request disappear.
    flow_id = args.flow if args.flow is not None else args.tx
    if args.flow is None and args.tx == 0x7DF:
        flow_id = args.rx - 8            # 0x7E8 -> 0x7E0, the conventional pair
        print(f"note: functional request; flow control directed to 0x{flow_id:03X}")
    guard_tx(flow_id)
    mask, pat, flow = mk(0xFFFFFFFF), mk(args.rx), mk(flow_id)
    d.PassThruStartMsgFilter(ch, FLOW_CONTROL_FILTER, ctypes.byref(mask),
                             ctypes.byref(pat), ctypes.byref(flow), ctypes.byref(fid))
    d.PassThruIoctl(ch, CLEAR_RX_BUFFER, None, None)

    buf, cnt = (MSG * 16)(), U(16)
    # An ECU has been seen to leave the first request of a session unanswered
    # and answer the same request on the next attempt; consuming tools retry
    # for that reason. One repeat of a read-only request separates a cold ECU
    # from a broken driver.
    for attempt in (1, 2):
        m, n = mk(args.tx, payload), U(1)
        rc = d.PassThruWriteMsgs(ch, ctypes.byref(m), ctypes.byref(n), 1000)
        print(f"request {payload.hex(' ')} to 0x{args.tx:03X}  WriteMsgs rc={rc}")
        cnt = U(16)
        rc = d.PassThruReadMsgs(ch, buf, ctypes.byref(cnt), 3000)
        print(f"ReadMsgs rc={rc}  {cnt.value} message(s)\n")
        if cnt.value or attempt == 2:
            break
        print("nothing arrived; repeating the request once\n")
        d.PassThruIoctl(ch, CLEAR_RX_BUFFER, None, None)

    indications, accepted, longest = 0, [], 0
    pending, negative = [], []
    for i in range(cnt.value):
        msg = buf[i]
        data = bytes(msg.Data[:msg.DataSize])
        kind = ("own transmit" if msg.RxStatus & TX_MSG_TYPE else
                "FIRST-FRAME indication" if msg.RxStatus & ISO15765_FIRST_FRAME else
                "received data")
        print(f"  RxStatus=0x{msg.RxStatus:08x} DataSize={msg.DataSize:<5} "
              f"ExtraDataIndex={msg.ExtraDataIndex:<5} [{kind}]")
        print(f"     {data.hex(' ')}")
        if msg.RxStatus & ISO15765_FIRST_FRAME and not (msg.RxStatus & TX_MSG_TYPE):
            indications += 1
        # the consumer's rule
        if msg.RxStatus & (TX_MSG_TYPE | ISO15765_FIRST_FRAME | ISO15765_PADDING_ERROR):
            continue
        if msg.DataSize < 4 or data[:4] != struct.pack(">I", args.rx):
            continue
        body = data[4:]
        # ISO 14229 / KWP2000: 0x7F <sid> 0x78 is requestCorrectlyReceived-
        # ResponsePending, an interim telling the tester to keep waiting. It is
        # not the answer, and counting it as one would make a healthy exchange
        # look like two replies. Consuming tools loop on it too.
        if len(body) >= 3 and body[0] == 0x7F and body[2] == 0x78:
            pending.append(body)
            continue
        if len(body) >= 3 and body[0] == 0x7F:
            negative.append(body)
            continue
        accepted.append(body)
        longest = max(longest, int(msg.DataSize))

    d.PassThruDisconnect(ch)
    d.PassThruClose(dev)

    print()
    ok = True
    for b in pending:
        print(f"  interim: responsePending (0x78) to service 0x{b[1]:02X} — the ECU "
              "asked for more time; not an answer.")
    for b in negative:
        print(f"  NEGATIVE RESPONSE to service 0x{b[1]:02X}: NRC 0x{b[2]:02X} "
              f"({b.hex(' ')}). The request reached the ECU and was refused.")
    if negative and not accepted:
        print("VERDICT: the ECU refused the request. Transport worked; the service "
              "or its arguments did not. Reassembly NOT proven.")
        return 5
    if not accepted:
        print("VERDICT: no message survived a consumer's acceptance rule — "
              "the reply was dropped or never arrived."); ok = False
    elif len(accepted) > 1:
        print(f"VERDICT: {len(accepted)} messages survived; a single request "
              "should yield one."); ok = False
    else:
        body = accepted[0]
        print(f"VERDICT: one clean message, {len(body)} payload bytes, "
              f"CAN id present once.")
        print(f"  payload: {body.hex(' ')}")
        text = "".join(chr(c) if 32 <= c < 127 else "." for c in body)
        print(f"  as text: {text}")
        if indications:
            print(f"  preceded by {indications} first-frame indication(s): the reply "
                  "was segmented, so reassembly was genuinely exercised.")
        else:
            print("  no first-frame indication: this reply fitted a single CAN frame, "
                  "so multi-frame reassembly was NOT exercised.")
            return 6
        if longest > 250:
            print(f"  the message exceeded one wire frame ({longest} bytes) — this also "
                  "settles how the device chunks a long reply.")
    return 0 if ok else 3


if __name__ == "__main__":
    sys.exit(main())
