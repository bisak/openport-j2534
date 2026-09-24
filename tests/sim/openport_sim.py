#!/usr/bin/env python3
"""
openport_sim — a simulated Tactrix OpenPort 2.0 behind a pty.

Why this exists
---------------
The one thing a bench cannot provide is a second CAN node that acknowledges a
frame. A lone CAN controller cannot ACK its own transmission, so on a bench
every transmit fails and the whole receive path stays unexercised.

This simulator supplies that missing peer at the protocol level, and then goes
further than a vehicle ever could: it can withhold an acknowledgement, answer
late, desynchronise the stream, truncate a frame, or vanish mid-transfer.

Fidelity
--------
Every command response here was measured against a real OpenPort 2.0
(firmware 1.17.4877) — see docs/PROTOCOL.md. tools/ab-official runs one
command sequence against the cable and against this simulator, through both the
vendor DLL and this driver, which is how drift gets caught.

Anything the simulator does that the cable was never observed doing is marked
MODELLED below, and is our reading of the protocol rather than a measurement.
Where several readings compete, the `Wire` switches let a consumer be run
against each candidate in turn (tools/ab-official does this with the vendor's
own DLL).

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import os
import pty
import select
import struct
import sys
import threading
import time

FW_VERSION = "1.17.4877"

# J2534 error numbers, which is what the device actually emits.
ERR_NOT_SUPPORTED       = 1
ERR_INVALID_PROTOCOL_ID = 3
ERR_INVALID_IOCTL_VALUE = 5
ERR_FAILED              = 7
ERR_TIMEOUT             = 9
ERR_INVALID_MSG         = 10
ERR_INVALID_MSG_ID      = 13
ERR_PIN_INVALID         = 19
ERR_CHANNEL_IN_USE      = 20
ERR_INVALID_FILTER_ID   = 22

SUPPORTED_PROTOCOLS = {3, 4, 5, 6, 7, 8, 9}
KLINE_PROTOCOLS     = {3, 4}

# Configuration parameters per protocol, PROTOCOL.md section 8. ISO15765 and
# ISO14230 were swept on the cable; ISO9141 and the L-line and jack channels
# 7-9 are MODELLED as ISO14230's set, CAN as ISO15765's set without the ISO-TP
# parameters. Values are the J2534-1 defaults; 0x9000 is Tactrix's stop bits,
# read back as 1 on channels 3 and 9.
_ISO15765_PARAMS = {1: 500000, 3: 0, 23: 80, 24: 1, 30: 0, 31: 0, 34: 0, 35: 0, 37: 0}
_KLINE_PARAMS    = {1: 10400, 3: 0, 7: 20, 10: 55, 12: 5, 14: 300, 15: 20, 16: 20,
                    17: 50, 18: 300, 19: 300, 20: 25, 21: 50, 22: 0, 25: 300, 32: 8, 33: 0,
                    0x9000: 1}
_CAN_PARAMS      = {1: 500000, 3: 0, 23: 80, 24: 1}
PARAMS_BY_PROTOCOL = {3: _KLINE_PARAMS, 4: _KLINE_PARAMS, 5: _CAN_PARAMS,
                      6: _ISO15765_PARAMS, 7: _KLINE_PARAMS, 8: _KLINE_PARAMS, 9: _KLINE_PARAMS}
SUPPORTED_PARAMS    = _ISO15765_PARAMS
READABLE_PINS       = {8: 0, 12: 0, 16: 12480, 17: 5751}
# `atv` pins and limits, measured 2026-09-16. Pin 0 is the 2.5 mm jack, which
# drives pin 12 while no plug is inserted; all voltage pins share one supply.
VOLTAGE_PINS        = {0, 1, 3, 9, 11, 12, 13}
GROUND_PINS         = VOLTAGE_PINS | {7, 10, 15}
VOLTAGE_MIN_MV      = 5000
VOLTAGE_MAX_MV      = 20000
VADJ_IDLE_MV        = 5751

STS_START    = 0x80
STS_END      = 0x40
STS_LOOPBACK = 0x20
ISO15765_FRAME_PAD = 0x40    # J2534 TxFlag: pad to a full CAN frame
TP20_SETUP_ID = 0x200        # VW TP2.0 channel-setup broadcast id
STS_TX_IND   = 0x10

# Payload bytes per frame. The length field is one byte and carries a status
# byte plus a 4-byte timestamp, so 250 is the ceiling.
MAX_FRAME_PAYLOAD = 250

# Argument counts this project has measured per verb (PROTOCOL.md section 4).
# Anything beyond them is logged prominently: the vendor DLL is reported to
# send more (a response timeout and a sequence number).
KNOWN_ARGS = {"o": 3, "t": 3, "f": 3, "k": 1, "g": 1, "s": 2, "r": 1, "v": 2,
              "y": 2, "w": 1, "p": 2, "m": 4, "n": 1, "c": 0, "l": 0, "a": 0, "z": 0, "i": 0}
CHANNEL_VERBS = set("octfkgslypnm")

RX_FRAMINGS   = ("measured", "old", "start_end_combined")
CHUNKINGS     = ("id_first_only", "id_every_chunk", "old")
KLINE_LAYOUTS = ("asymmetric", "uniform")
ECHO_SHAPES   = ("mirror_rx", "data_only", "combined")


class Channel:
    def __init__(self, proto, flags, baud):
        self.proto, self.flags, self.baud = proto, flags, baud
        self.config = dict(PARAMS_BY_PROTOCOL.get(proto, _CAN_PARAMS))
        self.config[1] = baud
        self.filters = {}          # id -> (type, mask, pattern, flowcontrol)
        self.periodic = {}         # id -> (interval, payload)


class Tp20State:
    """One simulated VW TP2.0 channel. VAG modules are not addressed with
    ISO15765 at all: a tester opens a channel on 0x200 and the module names the
    pair of CAN ids to use. Modelled from the published protocol (jazdw.net,
    OVMS) and from two open implementations; no capture of a real exchange
    exists in this project yet, so the behaviour here is MODELLED."""
    def __init__(self):
        self.dest = self.rx_id = self.tx_id = None
        self.tx_seq = 0
        self.pending = b""
        self.want = None


class Faults:
    """Everything that can go wrong, made deliberate and repeatable."""
    def __init__(self):
        self.no_ack          = False   # transmits fail with ERR_TIMEOUT, as on a bench
        self.tx_reject_delay = 1.2     # how long a no-ACK transmit takes to fail
        self.reply_delay     = 0.0     # answer every command this late
        self.startup_backlog = 0       # stale replies queued before we start
        self.desync_after    = 0       # emit a malformed frame after N transmits
        self.drop_reply_every= 0       # swallow every Nth reply entirely
        self.truncate_frames = False   # cut frames mid-way
        self.disconnect_after= 0       # close the pty after N commands
        self.garbage_bytes   = 0       # inject N random bytes before each reply


class Wire:
    """
    The device-to-host framing where the cable has not been measured, made
    switchable. The defaults are this project's current reading; the
    alternatives are the competing readings from other drivers, so a consumer
    can be run against each to find out which one it was written for.
    """
    def __init__(self):
        # Receive framing of one logical message (PROTOCOL.md section 7):
        #   measured            one CAN frame -> single END frame with id+data;
        #                       segmented    -> START frame with the id only,
        #                                       then END frame(s) with id+data
        #   old                 START carries id + first data chunk, END the
        #                       rest, the id is never repeated
        #   start_end_combined  anything that fits one wire frame is a single
        #                       START|END (0xC0) frame with id+data
        self.rx_framing = "measured"
        # How a message longer than one wire frame (250 bytes) is chunked:
        #   id_every_chunk  every 0x00/0x40 chunk repeats the 4-byte CAN id
        #   id_first_only   chunks of (id+data); only the first chunk has the id
        #   old             START carries id + first chunk, 0x00 middles, END last
        # id_every_chunk is the layout Tactrix's own DLL reassembles: fed
        # id_first_only it drops four bytes per continuation chunk, fed
        # id_every_chunk it returns the 600-byte reply intact (tools/ab-official,
        # 2026-09-13). The driver strips the id from every continuation chunk
        # the same way, so the other two models are kept only to show what a
        # consumer would see under them. The cable was captured doing it, in
        # 70-byte chunks (can_frames, PROTOCOL.md 7.6).
        self.chunking = "id_every_chunk"
        # K-line (channels 3, 4) frame layout:
        #   asymmetric  0x00/0x20 data frames carry no timestamp; START/END/0x10
        #               frames carry the 4-byte timestamp and nothing else
        #   uniform     every frame carries the timestamp, as on CAN
        self.kline_layout = "asymmetric"
        # Shape of a K-line transmit echo when LOOPBACK=1 (CAN and ISO15765
        # are measured, see OpenPortSim._loopback_echo):
        #   mirror_rx  the receive framing with 0x20 set on every frame
        #   data_only  a single 0x60 frame with id+data, never an announcement
        #   combined   a single 0xE0 frame with id+data
        self.echo_shape = "mirror_rx"
        # Emit a 0x10 transmit indication after every successful transmit.
        self.tx_done = False
        # Answer verbs this project has never seen answered with `aro` instead
        # of the cable's measured silence, so a consumer that sends them does
        # not stall. Every such answer is logged.
        self.answer_unknown = False
        # ISO 15765-4 clause 8.1: the DLC of every diagnostic CAN frame shall
        # be eight, and a diagnostic CAN frame with a DLC below eight shall be
        # ignored by the receiving entity. The OpenPort only pads when the
        # caller sets ISO15765_FRAME_PAD, so a request sent without that flag
        # goes out short and a conforming ECU drops it — measured on a 2012 VW
        # Caddy, which answered a padded request and ignored the same request
        # unpadded. Modelled here so a tool that forgets the flag fails a test
        # instead of failing at a car.
        self.enforce_frame_pad = True
        # What a K-line init (`atw`/`aty`) is answered with: "aro"; "ary" for
        # `ary<ch> <len>\r\n` + the ECU's response bytes; or "arw" for five-baud
        # answered `arw<ch> <b> <b>\r\n` with the key bytes in decimal on the
        # line (third-party, HDS-verified on a Honda; PROTOCOL.md section 3).
        self.init_reply = "aro"


def _split(data, n):
    return [data[i:i + n] for i in range(0, len(data), n)] or [b""]


def can_frames(payload, framing="measured", chunking="id_every_chunk"):
    """
    The (status, body) frames one logical CAN/ISO15765 message becomes under a
    framing model. `payload` is the 4-byte CAN id followed by the data. Pure,
    so the models can be inspected and unit-tested on their own.
    """
    payload = bytes(payload)
    can_id, body = payload[:4], payload[4:]
    frames = []
    if framing == "measured" and chunking == "id_every_chunk" and len(body) > 7:
        # Measured 2026-09-24 over 5 000+ segmented replies (PROTOCOL.md 7.6): the
        # announcement, then the data as it arrives, the first frame's 6 bytes and
        # nine consecutive frames (69), then ten at a time (70), each chunk
        # repeating the id; the 250-byte frame limit is never reached.
        chunks = [body[:69]] + _split(body[69:], 70) if len(body) > 69 else [body]
        frames.append((STS_START, can_id))
        frames += [(0x00, can_id + c) for c in chunks[:-1]]
        frames.append((STS_END, can_id + chunks[-1]))
        return frames
    if len(payload) <= MAX_FRAME_PAYLOAD:
        segmented = len(body) > 7                  # needed ISO-TP on the bus
        if framing == "measured":
            if segmented:
                frames.append((STS_START, can_id))
            frames.append((STS_END, payload))
        elif framing == "old":
            if segmented:
                frames += [(STS_START, payload), (STS_END, b"")]
            else:
                frames.append((STS_END, payload))
        elif framing == "start_end_combined":
            frames.append((STS_START | STS_END, payload))
        elif framing == "no_announce":
            frames.append((STS_END, payload))
        else:
            raise ValueError(framing)
        return frames

    if framing in ("measured", "no_announce") and chunking != "old":
        if framing == "measured":
            frames.append((STS_START, can_id))
        if chunking == "id_first_only":
            chunks = _split(payload, MAX_FRAME_PAYLOAD)
        elif chunking == "id_every_chunk":
            chunks = [can_id + c for c in _split(body, MAX_FRAME_PAYLOAD - 4)]
        else:
            raise ValueError(chunking)
        frames += [(0x00, c) for c in chunks[:-1]]
        frames.append((STS_END, chunks[-1]))
        return frames

    chunks = _split(body, MAX_FRAME_PAYLOAD - 4)
    frames.append((STS_START, can_id + chunks[0]))
    frames += [(0x00, c) for c in chunks[1:-1]]
    frames.append((STS_END, chunks[-1]))
    return frames


class OpenPortSim:
    def __init__(self, ecu=None, faults=None, log=None, wire=None):
        self.channels = {}
        self.ecu = ecu or {}           # {request_bytes: response_bytes}
        self.ecu_model = None          # or a callable request_bytes -> response_bytes|None
        self.faults = faults or Faults()
        self.wire = wire or Wire()
        self.log = log
        self.t0 = time.monotonic()
        self.tx_count = 0
        self.tp20 = Tp20State()
        self.tp20_modules = {0x01}      # logical addresses that answer a setup
        self.tx_times = []   # arrival time of each transmit, for pacing tests
        self.cmd_count = 0
        self._seq = None
        self.closed = False
        # Measured on the cable 2026-09-24: filter ids count up across every
        # channel and restart at `ata`/`atz`; periodic ids count up across
        # channels and sessions alike, never restarting while powered.
        self.next_filter_id = 0
        self.next_periodic_id = 0
        self._out = b""
        self._lock = threading.Lock()
        self._rng = 0x2545F4914F6CDD1D

    # -- helpers ----------------------------------------------------------
    def _log(self, msg):
        if self.log:
            self.log(f"{time.monotonic() - self.t0:9.4f} {msg}")

    def _rand(self):
        self._rng ^= (self._rng << 13) & 0xFFFFFFFFFFFFFFFF
        self._rng ^= self._rng >> 7
        self._rng ^= (self._rng << 17) & 0xFFFFFFFFFFFFFFFF
        return self._rng & 0xFF

    def _ts(self):
        """Microseconds since power-on, as the device reports."""
        return int((time.monotonic() - self.t0) * 1e6) & 0xFFFFFFFF

    def _emit(self, data):
        with self._lock:
            self._out += data

    def _reply(self, text):
        if self.faults.drop_reply_every:
            self.cmd_count += 1
            if self.cmd_count % self.faults.drop_reply_every == 0:
                self._log(f"  [fault] dropping reply {text!r}")
                return
        if self.faults.garbage_bytes:
            self._emit(bytes(self._rand() for _ in range(self.faults.garbage_bytes)))
        if self.faults.reply_delay:
            time.sleep(self.faults.reply_delay)
        data = text.encode() if isinstance(text, str) else text
        self._log(f"-> {data!r}")
        self._emit(data)

    def _ok(self):
        # The cable echoes a trailing sequence number in its acknowledgement:
        # `ato6 0 500000 0 1` answered `aro 1` on hardware (PROTOCOL.md section
        # 10). The vendor DLL numbers every command this way and waits for the
        # echo, so an unnumbered `aro` is not an acknowledgement to it.
        self._reply(self._with_seq("aro"))

    def _with_seq(self, line):
        """Append the command's sequence number to a text reply. Measured for
        `aro`; the trailing field of `arf`/`arg` reads 0 without one and echoes
        the last extra argument with one; measured on the cable 2026-09-13 for
        `aro`, `arr`, `arf`, `arg`, `arm` and `are` alike (PROTOCOL.md
        section 3). The vendor DLL waits five seconds for it on `arr`."""
        return f"{line} {self._seq}\r\n" if self._seq is not None else f"{line}\r\n"
    def _err(self, code, detail=None):
        self._reply(self._with_seq(f"are {code}" if detail is None else f"are {code} {detail}"))

    def _raw_frame(self, ch, status, body):
        """One wire frame: 'a' 'r' <digit> <len> <status> <body>."""
        body = bytes([status]) + bytes(body)
        if len(body) > 255:
            raise ValueError("frame body exceeds the one-byte length field")
        f = b"ar" + str(ch).encode() + bytes([len(body)]) + body
        if self.faults.truncate_frames:
            f = f[: max(1, len(f) // 2)]
            self._log("  [fault] truncating frame")
        self._log(f"-> ar{ch} sts=0x{status:02x} len={len(body)} | {f.hex(' ')}")
        self._emit(f)

    def frame(self, ch, status, payload):
        """A CAN-layout frame: status, 4-byte big-endian timestamp, payload."""
        self._raw_frame(ch, status, struct.pack(">I", self._ts()) + bytes(payload))

    def send_message(self, ch, payload, loopback=False):
        """
        Deliver one logical message. CAN/ISO15765 channels follow
        `Wire.rx_framing` and `Wire.chunking`; the default is what the device
        was MEASURED to do (PROTOCOL.md section 7; vehicle session of
        2026-06-17):

          * a message that needed ISO-TP segmentation on the bus is first
            announced by a START frame carrying the CAN id and nothing else;
          * the data then arrives in END-terminated frames that carry the
            CAN id again followed by the payload;
          * a message that fit one CAN frame is a single END frame, with no
            announcement.

        Measured since (PROTOCOL.md 7.6, 7.7): long CAN/ISO15765 payloads come in
        70-byte chunks, and the echo is `_loopback_echo`. MODELLED, because no
        capture shows it: the K-line layout (`Wire.kline_layout`), which follows
        three independent drivers.
        """
        lb = STS_LOOPBACK if loopback else 0
        payload = bytes(payload)
        if ch in KLINE_PROTOCOLS:
            return self._send_kline(ch, payload, lb)
        framing, chunking = self.wire.rx_framing, self.wire.chunking
        if loopback:
            framing = {"mirror_rx": framing, "data_only": "no_announce",
                       "combined": "start_end_combined"}[self.wire.echo_shape]
        for status, body in can_frames(payload, framing, chunking):
            self.frame(ch, status | lb, body)

    def _send_kline(self, ch, data, lb):
        ts = struct.pack(">I", self._ts())
        uniform = self.wire.kline_layout == "uniform"
        self._raw_frame(ch, STS_START | lb, ts)
        for chunk in _split(data, MAX_FRAME_PAYLOAD if uniform else 254):
            self._raw_frame(ch, lb, (ts + chunk) if uniform else chunk)
        self._raw_frame(ch, STS_END | lb, ts)

    def _loopback_echo(self, ch, payload):
        """LOOPBACK's echo of one transmit. CAN and ISO15765 alike: a 0x20 frame
        of four zero bytes on raw CAN channel 5 (PROTOCOL.md section 7.7;
        measured on raw CAN 2026-09-16, on ISO15765 against the bench ECU
        2026-09-24, where the flow-control frames the cable sent were echoed
        too, id and data intact; this simulator sends none). K-line is
        unmeasured and follows `Wire.echo_shape`."""
        if ch in (5, 6):
            self._raw_frame(5, STS_LOOPBACK, struct.pack(">I", self._ts()) + b"\0\0\0\0")
        else:
            self.send_message(ch, payload, loopback=True)

    def _tx_indication(self, ch, payload):
        if ch in KLINE_PROTOCOLS:
            self._raw_frame(ch, STS_TX_IND, struct.pack(">I", self._ts()))
        else:
            self.frame(ch, STS_TX_IND, payload[:4])

    # -- command handling -------------------------------------------------
    def handle(self, line, payload):
        s = line.decode("ascii", "replace").strip()
        self._log(f"<- {s!r}" + (f" +{len(payload)}B {bytes(payload).hex(' ')}" if payload else ""))

        self.cmd_count += 1
        if self.faults.disconnect_after and self.cmd_count >= self.faults.disconnect_after:
            self._log("  [fault] disconnecting")
            self.closed = True
            return

        if not s.startswith("at") or len(s) < 3:
            return                                     # unknown: silence
        verb, rest = s[2], s[3:]
        self._seq = self._trailing_seq(verb, rest)
        self._note_extra_args(verb, rest)

        # `ata` closes every open channel, exactly like `atz` (measured
        # 2026-09-13: a filter on a channel opened before `ata` answers
        # `are 2` after it).
        if verb in ("a", "z"):
            # Both also switch every voltage output off (measured 2026-09-16).
            self.channels.clear()
            self.next_filter_id = 0
            READABLE_PINS[12] = 0
            READABLE_PINS[17] = VADJ_IDLE_MV
            return self._ok()
        if verb == "i":  return self._reply(f"ari main code version : {FW_VERSION}\r\n")

        if verb == "o":  return self._cmd_open(rest)
        if verb == "c":  return self._cmd_close(rest)
        if verb == "r":  return self._cmd_read_pin(rest)
        if verb == "v":  return self._cmd_voltage(rest)
        if verb == "g":  return self._cmd_get_config(rest)
        if verb == "s":  return self._cmd_set_config(rest)
        if verb == "f":  return self._cmd_filter(rest, payload)
        if verb == "k":  return self._cmd_stop_filter(rest)
        if verb == "l":  return self._cmd_clear_periodic(rest)
        if verb == "t":  return self._cmd_transmit(rest, payload)
        if verb == "y":  return self._cmd_init(rest, payload, five_baud=False)
        if verb == "w":  return self._cmd_init(rest, payload, five_baud=True)
        if verb == "p":  return self._cmd_pin_verb_p(rest, payload)
        if verb == "m":  return self._cmd_periodic_vendor(rest, payload)
        if verb == "n":  return self._cmd_periodic_stop(rest)
        if self.wire.answer_unknown:
            self._log(f"  !! unknown verb at{verb!s}: answering aro (MODELLED; the cable is silent)")
            return self._ok()
        return                                          # silence

    def _trailing_seq(self, verb, rest):
        """The sequence number the vendor DLL appends after a verb's known
        arguments, or None. Only a decimal beyond the known count counts."""
        known = KNOWN_ARGS.get(verb)
        if known is None:
            return None
        args = self._split_ch(rest)[1] if verb in CHANNEL_VERBS else rest.strip().split()
        if len(args) > known and args[-1].isdigit():
            return int(args[-1])
        return None

    def _note_extra_args(self, verb, rest):
        known = KNOWN_ARGS.get(verb)
        if known is None:
            return
        args = self._split_ch(rest)[1] if verb in CHANNEL_VERBS else rest.strip().split()
        if len(args) > known:
            self._log(f"  !! at{verb} carries {len(args)} args, {known} known: "
                      f"extra {args[known:]}")

    def _split_ch(self, rest):
        """Channel digit is fused to the verb; args follow a space."""
        i = 0
        while i < len(rest) and rest[i].isdigit():
            i += 1
        if i == 0:
            return None, rest.strip().split()
        return int(rest[:i]), rest[i:].strip().split()

    def _cmd_open(self, rest):
        ch, args = self._split_ch(rest)
        if ch is None or len(args) < 3:            return self._err(ERR_FAILED)
        if ch not in SUPPORTED_PROTOCOLS:          return self._err(ERR_INVALID_PROTOCOL_ID)
        if ch in self.channels:                    return self._err(ERR_CHANNEL_IN_USE)
        # Protocols sharing a line are mutually exclusive, with
        # ERR_INVALID_PROTOCOL_ID rather than IN_USE: ISO9141/ISO14230 on K
        # (3/4), and on L (7/8), measured 2026-09-13 and identified through
        # Tactrix's DLL 2026-09-16. There is no cap on the number of channels.
        for a, b in ((3, 4), (7, 8)):
            if ch in (a, b) and (a in self.channels or b in self.channels):
                return self._err(ERR_INVALID_PROTOCOL_ID)
        try:
            flags, baud = int(args[0]), int(args[1])
        except ValueError:                         return self._err(ERR_FAILED)
        self.channels[ch] = Channel(ch, flags, baud)
        self._ok()

    def _cmd_close(self, rest):
        ch, _ = self._split_ch(rest)
        if ch is None: return self._err(ERR_FAILED)
        self.channels.pop(ch, None)
        self._ok()                                  # lenient, as the device is

    def _cmd_read_pin(self, rest):
        args = rest.strip().split()
        if not args: return self._err(ERR_FAILED)
        try: pin = int(args[0])
        except ValueError: return self._err(ERR_FAILED)
        if pin not in READABLE_PINS: return self._err(ERR_PIN_INVALID)
        self._reply(self._with_seq(f"arr {pin} {READABLE_PINS[pin]}"))

    def _cmd_voltage(self, rest):
        args = rest.strip().split()
        if len(args) < 2: return self._err(ERR_INVALID_MSG)
        try: pin, mv = int(args[0]), int(args[1]) & 0xFFFFFFFF   # the DLL prints -1/-2
        except ValueError: return self._err(ERR_INVALID_MSG)
        if mv in (0xFFFFFFFF, 0xFFFFFFFE):
            if pin not in GROUND_PINS: return self._err(ERR_PIN_INVALID)
            if pin in (0, 12): READABLE_PINS[12] = 0
            return self._ok()
        if pin not in VOLTAGE_PINS: return self._err(ERR_PIN_INVALID)
        if mv < VOLTAGE_MIN_MV: return self._err(120)
        if mv > VOLTAGE_MAX_MV: return self._err(119)
        READABLE_PINS[17] = mv
        if pin in (0, 12): READABLE_PINS[12] = mv
        self._ok()

    def _cmd_get_config(self, rest):
        ch, args = self._split_ch(rest)
        if ch is None or not args: return self._err(ERR_FAILED)
        c = self.channels.get(ch)
        if c is None: return self._err(ERR_FAILED)
        try: param = int(args[0])
        except ValueError: return self._err(ERR_FAILED)
        if param not in c.config: return self._err(ERR_NOT_SUPPORTED)
        self._reply(self._with_seq(f"arg{ch} {param} {c.config.get(param, 0)}") if self._seq is not None
                    else f"arg{ch} {param} {c.config.get(param, 0)} 0\r\n")

    def _cmd_set_config(self, rest):
        ch, args = self._split_ch(rest)
        if ch is None or len(args) < 2: return self._err(ERR_FAILED)
        c = self.channels.get(ch)
        if c is None: return self._err(ERR_FAILED)
        try: param, value = int(args[0]), int(args[1])
        except ValueError: return self._err(ERR_FAILED)
        if param not in c.config: return self._err(ERR_NOT_SUPPORTED)
        c.config[param] = value
        self._ok()

    def _cmd_filter(self, rest, payload):
        ch, args = self._split_ch(rest)
        if ch is None or len(args) < 3: return self._err(ERR_FAILED)
        c = self.channels.get(ch)
        if c is None: return self._err(ERR_FAILED)
        try: ftype, _txf, each = int(args[0]), int(args[1]), int(args[2])
        except ValueError: return self._err(ERR_FAILED)
        need = {1: 2, 2: 2, 3: 3}.get(ftype, 0)
        if need == 0:                   # types 0 and 4, measured 2026-09-24
            return self._err(ERR_INVALID_FILTER_ID)
        if len(payload) != need * each:
            return self._err(ERR_INVALID_MSG)
        msgs = [payload[i * each:(i + 1) * each] for i in range(need)]
        if len(c.filters) >= 10:        # the eleventh on a channel, measured 2026-09-24
            return self._err(12)
        fid = self.next_filter_id
        self.next_filter_id += 1
        c.filters[fid] = (ftype, *msgs) if need == 3 else (ftype, msgs[0], msgs[1], None)
        self._reply(self._with_seq(f"arf{ch} {fid}") if self._seq is not None else f"arf{ch} {fid} 0\r\n")

    def _cmd_stop_filter(self, rest):
        ch, args = self._split_ch(rest)
        if ch is None or not args: return self._err(ERR_FAILED)
        c = self.channels.get(ch)
        if c is None: return self._err(ERR_FAILED)
        try: fid = int(args[0])
        except ValueError: return self._err(ERR_FAILED)
        if fid == -1:                 # every filter on the channel, measured 2026-09-24
            c.filters.clear()
            return self._ok()
        if fid not in c.filters: return self._err(ERR_INVALID_FILTER_ID)
        del c.filters[fid]
        self._ok()

    def _ecu_response(self, ch, payload):
        """The ECU's answer to a transmit, already prefixed for the channel."""
        kline = ch in KLINE_PROTOCOLS
        req = bytes(payload) if kline else bytes(payload[4:])
        if self.ecu_model is not None:
            resp = self.ecu_model(req)
        else:
            resp = self.ecu.get(req)
            if resp is None:
                resp = self.ecu.get(b"*")
        if resp is None or kline:
            return resp
        rx_id = b"\x00\x00\x07\xe8"
        c = self.channels.get(ch)
        for f in (c.filters.values() if c else ()):
            if f[0] == 3 and f[2] is not None:
                rx_id = bytes(f[2])               # the filter's pattern id
                break
        return rx_id + resp

    def _cmd_transmit(self, rest, payload):
        ch, args = self._split_ch(rest)
        if ch is None or len(args) < 1: return self._err(ERR_FAILED)
        c = self.channels.get(ch)
        if c is None:
            # Measured: with no armed channel the device answers with a
            # transmit indication (status 0x10, CAN id only) and reports
            # success without touching the bus.
            self._tx_indication(ch, payload)
            return self._ok()
        try: declared = int(args[0])
        except ValueError: return self._err(ERR_INVALID_MSG)
        if declared != len(payload): return self._err(ERR_INVALID_MSG)
        try: txflags = int(args[1]) if len(args) > 1 else 0
        except ValueError: txflags = 0

        self.tx_count += 1
        self.tx_times.append(time.monotonic())

        # A short, unpadded diagnostic frame is invisible to a conforming ECU.
        # The transmit itself still succeeds: the frame reaches the bus, nothing
        # answers it. That is exactly what a real car does.
        if (self.wire.enforce_frame_pad and ch == 6
                and not (txflags & ISO15765_FRAME_PAD)
                and len(payload) < 12):        # 4-byte CAN id + under 8 data
            self._log(f"  !! unpadded ISO15765 request ({len(payload) - 4} data bytes): "
                      "ISO 15765-4 clause 8.1 says a conforming ECU ignores a "
                      "diagnostic frame with DLC < 8. Staying silent.")
            self._ok()
            return

        if self.faults.no_ack:
            # A bench with no second node: the controller never gets an ACK.
            time.sleep(self.faults.tx_reject_delay)
            return self._err(ERR_TIMEOUT)

        if self.faults.desync_after and self.tx_count >= self.faults.desync_after:
            self._log("  [fault] emitting a malformed frame")
            self._emit(b"ar" + b"6" + bytes([200]) + b"\x00\x01\x02")

        # Loopback echo of what we transmitted, then the ECU's answer.
        if c.config.get(3):                       # LOOPBACK enabled
            self._loopback_echo(ch, payload)
        self._ok()
        if self.wire.tx_done:
            self._tx_indication(ch, payload)

        if self._tp20_handle(ch, payload):
            return

        resp = self._ecu_response(ch, payload)
        if resp is not None:
            threading.Timer(0.01, lambda: self.send_message(ch, resp)).start()

    # -- VW TP2.0 ---------------------------------------------------------
    def _tp20_send(self, ch, can_id, data):
        # Raw CAN frames carry status 0x00: no transport layer, so the firmware
        # marks neither START nor END (measured on a 2012 VW Caddy).
        self._log(f"  TP20 -> 0x{can_id:03X} {bytes(data).hex(' ')}")
        self.frame(ch, 0x00, struct.pack(">I", can_id) + bytes(data))

    def _tp20_reply(self, ch, req):
        """Answer one KWP request inside the channel, splitting it the way the
        protocol requires: a 16-bit length, then 7-byte frames, the last asking
        for an acknowledgement."""
        t = self.tp20
        resp = self.ecu.get(bytes(req)) or self.ecu.get(b"*")
        if resp is None:
            resp = bytes([0x7F, req[0], 0x11])       # serviceNotSupported
        body = struct.pack(">H", len(resp)) + bytes(resp)
        chunks = [body[i:i + 7] for i in range(0, len(body), 7)] or [b""]
        for i, c in enumerate(chunks):
            op = 0x10 if i == len(chunks) - 1 else 0x20
            self._tp20_send(ch, t.rx_id, bytes([op | (t.tx_seq & 0x0F)]) + c)
            t.tx_seq = (t.tx_seq + 1) & 0x0F

    def _tp20_handle(self, ch, payload):
        """True when the frame was consumed as TP2.0."""
        if ch != 5 or len(payload) < 5:
            return False
        can_id = int.from_bytes(bytes(payload[:4]), "big")
        d = bytes(payload[4:])
        t = self.tp20

        if can_id == TP20_SETUP_ID and len(d) == 7 and d[1] == 0xC0:
            dest = d[0]
            if dest not in self.tp20_modules:
                self._log(f"  TP20 no module at 0x{dest:02X}; staying silent")
                return True
            t.dest, t.rx_id, t.tx_id = dest, 0x300, 0x740
            t.tx_seq, t.pending, t.want = 0, b"", None
            self._tp20_send(ch, TP20_SETUP_ID + dest,
                            bytes([0x00, 0xD0,
                                   t.rx_id & 0xFF, (t.rx_id >> 8) & 0x0F,
                                   t.tx_id & 0xFF, (t.tx_id >> 8) & 0x0F, 0x01]))
            return True

        if t.tx_id is None or can_id != t.tx_id or not d:
            return False

        if d[0] in (0xA0, 0xA3):                      # parameters / keepalive
            self._tp20_send(ch, t.rx_id, bytes((0xA1, 0x0F, 0x8A, 0xFF, 0x4A, 0xFF)))
            return True
        if d[0] == 0xA8:                              # disconnect
            self._tp20_send(ch, t.rx_id, b"\xa8")
            t.tx_id = t.rx_id = None
            return True

        op, seq = d[0] >> 4, d[0] & 0x0F
        if op in (0xB, 0x9):                          # the tester acknowledged us
            return True
        if op not in (0x0, 0x1, 0x2, 0x3):
            return True
        t.pending += d[1:]
        if op in (0x0, 0x1):                          # the tester wants an ACK
            self._tp20_send(ch, t.rx_id, bytes([0xB0 | ((seq + 1) & 0x0F)]))
        if t.want is None and len(t.pending) >= 2:
            t.want, t.pending = (t.pending[0] << 8) | t.pending[1], t.pending[2:]
        if t.want is not None and len(t.pending) >= t.want:
            req, t.pending, t.want = t.pending[:t.want], b"", None
            self._tp20_reply(ch, req)
        return True

    def _cmd_init(self, rest, payload, five_baud=False):
        """K-line init. Two verbs (PROTOCOL.md §4): `atw<ch> <address>` with
        the address in decimal and no payload is five-baud (the vendor DLL's
        form, ~2.45 s on the wire); `aty<ch> <len> 0` + request is fast init
        (~108 ms). The REPLY is still MODELLED — no successful round-trip has
        been captured."""
        ch, args = self._split_ch(rest)
        verb = "atw" if five_baud else "aty"
        if five_baud and args and args[0].isdigit():
            payload = bytes([int(args[0]) & 0xFF])     # the address, as the ECU table key
        self._log(f"  !! {verb} received (K-line {'five-baud' if five_baud else 'fast'} "
                  f"init): ch={ch} args={args} payload={bytes(payload).hex(' ')} "
                  f"— reply is MODELLED")
        if ch is None or ch not in self.channels:
            return self._err(ERR_FAILED)
        resp = self.ecu.get(bytes(payload)) if payload else None
        if self.wire.init_reply == "ary":
            body = resp or b""
            self._reply(f"ary{ch} {len(body)}\r\n".encode() + body)
            return
        if self.wire.init_reply == "arw" and five_baud:
            toks = "".join(f" {b}" for b in (resp or b""))
            self._reply(self._with_seq(f"arw{ch}{toks}"))
            return
        self._ok()
        if resp is not None:
            threading.Timer(0.01, lambda: self.send_message(ch, resp)).start()

    def _cmd_pin_verb_p(self, rest, payload):
        """`atp <pin> <value>`: a pin verb sharing `atv`'s argument shape, not
        a periodic message. Measured 2026-09-13 with the cable: every
        (pin, value) tried answers `are 10`, except value 0 which answers
        `are 5`; the reply takes ~0.5 s. Its function is unknown and the
        vendor DLL is not seen sending it."""
        args = rest.strip().split()
        if len(args) < 2: return self._err(ERR_FAILED)
        try: value = int(args[1])
        except ValueError: return self._err(ERR_FAILED)
        return self._err(ERR_INVALID_IOCTL_VALUE if value == 0 else ERR_INVALID_MSG)

    def _cmd_periodic_vendor(self, rest, payload):
        """`atm<ch> <interval_us> 0 <txflags> <len> <seq>` + payload, the
        periodic-message command Tactrix's DLL and this driver send. Measured
        on the cable (PROTOCOL.md section 10): `arm<ch> <id> <seq>`, ids from
        0; ten per channel, the eleventh `are 12`; `atn<ch> <id>` stops it,
        `are 13` for an unknown id; the transmits keep their interval, produce
        no transmit indication, and stop on `atc`/`ata`/`atz`."""
        ch, args = self._split_ch(rest)
        c = self.channels.get(ch)
        if c is None or len(args) < 4:
            return self._err(ERR_FAILED)
        try:
            interval_us, txflags, length = int(args[0]), int(args[2]), int(args[3])
        except ValueError:
            return self._err(ERR_FAILED)
        if length != len(payload):
            return self._err(ERR_INVALID_MSG)
        if len(c.periodic) >= 10:
            return self._err(12)
        pid = self.next_periodic_id
        self.next_periodic_id += 1
        c.periodic[pid] = (interval_us / 1e6, bytes(payload), txflags)
        threading.Thread(target=self._periodic_run, args=(ch, c, pid), daemon=True).start()
        self._reply(self._with_seq(f"arm{ch} {pid}"))

    def _periodic_run(self, ch, c, pid):
        while not self.closed and self.channels.get(ch) is c:
            entry = c.periodic.get(pid)
            if entry is None:
                break
            interval, payload, txflags = entry
            time.sleep(interval)
            if self.channels.get(ch) is not c or pid not in c.periodic:
                break
            self.tx_count += 1
            self.tx_times.append(time.monotonic())
            if c.config.get(3):                       # LOOPBACK enabled
                self._loopback_echo(ch, payload)
            resp = self._ecu_response(ch, payload)
            if resp is not None:
                self.send_message(ch, resp)

    def _cmd_clear_periodic(self, rest):
        """`atl<ch>`: stops every periodic message on the channel; `atn` of an
        old id then answers `are 13`. The vendor DLL's CLEAR_PERIODIC_MSGS,
        measured on the cable 2026-09-24. Without a channel digit, `aro`."""
        ch, _ = self._split_ch(rest)
        c = self.channels.get(ch) if ch is not None else None
        if c is not None:
            c.periodic.clear()
        self._ok()

    def _cmd_periodic_stop(self, rest):
        ch, args = self._split_ch(rest)
        if ch is None or not args: return self._err(ERR_FAILED)
        c = self.channels.get(ch)
        if c is None: return self._err(ERR_FAILED)
        try: pid = int(args[0])
        except ValueError: return self._err(ERR_FAILED)
        if pid not in c.periodic: return self._err(ERR_INVALID_MSG_ID)
        del c.periodic[pid]
        self._ok()

    # -- pty pump ---------------------------------------------------------
    def serve(self, master_fd, stop_evt):
        line, pend, want = b"", b"", 0
        while not stop_evt.is_set() and not self.closed:
            r, w, _ = select.select([master_fd], [master_fd], [], 0.02)
            if master_fd in r:
                try: data = os.read(master_fd, 4096)
                except OSError: break
                if not data: break
                for b in data:
                    if want:
                        pend += bytes([b])
                        want -= 1
                        if want == 0:
                            self.handle(line, pend); line, pend = b"", b""
                        continue
                    if b == 0x0A:
                        want = self._declared_payload(line)
                        if want == 0:
                            self.handle(line, b""); line = b""
                        else:
                            pend = b""
                        continue
                    if b != 0x0D:
                        line += bytes([b])
            with self._lock:
                out, self._out = self._out, b""
            if out:
                try: os.write(master_fd, out)
                except OSError: break
        try: os.close(master_fd)
        except OSError: pass

    @staticmethod
    def _declared_payload(line):
        try: s = line.decode("ascii").strip()
        except UnicodeDecodeError: return 0
        if not s.startswith("at") or len(s) < 3: return 0
        verb = s[2]
        rest = s[3:]
        i = 0
        while i < len(rest) and rest[i].isdigit(): i += 1
        args = rest[i:].strip().split()
        try:
            if verb in "typ" and len(args) >= 1: return int(args[0])
            if verb == "m" and len(args) >= 4: return int(args[3])   # atm: interval 0 txflags len
            if verb == "f" and len(args) >= 3:
                return {1: 2, 2: 2, 3: 3}.get(int(args[0]), 0) * int(args[2])
        except ValueError:
            return 0
        return 0


def spawn(ecu=None, faults=None, log=None, wire=None):
    """Start a simulator on a pty. Returns (slave_path, stop_event, thread, sim)."""
    master, slave = pty.openpty()
    path = os.ttyname(slave)
    # The slave fd is deliberately kept open. Closing it deallocates the pty on
    # macOS, so the driver's later open() of the same path fails with ENOENT.
    # We never read from it; the driver opens its own descriptor on the path.
    sim = OpenPortSim(ecu=ecu, faults=faults, log=log, wire=wire)
    if faults and faults.startup_backlog:
        # Replies left over from an interrupted session — the condition that
        # silently shifts every later result by one.
        sim._emit(b"aro\r\n" * faults.startup_backlog)
    stop = threading.Event()
    th = threading.Thread(target=sim.serve, args=(master, stop), daemon=True)
    th.start()
    sim._slave_fd = slave
    return path, stop, th, sim


def _gen(n):
    """The 600-byte test body used by run_scenarios: a recognisable ramp."""
    return bytes((i * 7 + 3) & 0xFF for i in range(n))


def parse_response_spec(spec):
    """`REQHEX=RESPHEX`; REQ may be `*` for the default; RESP may end in
    `+gen<N>` to append N generated bytes (e.g. `6201+gen600`)."""
    req, _, resp = spec.partition("=")
    key = b"*" if req.strip() == "*" else bytes.fromhex(req)
    if "+gen" in resp:
        head, _, n = resp.partition("+gen")
        value = bytes.fromhex(head) + _gen(int(n))
    else:
        value = bytes.fromhex(resp)
    return key, value


def load_ecu_model(spec, log):
    """FILE.py:CLASS[:IMAGE] -> an instance of CLASS(image_bytes_or_None, log=log)."""
    import importlib.util
    parts = spec.split(":")
    if len(parts) not in (2, 3):
        sys.exit("--ecu-model expects FILE.py:CLASS[:IMAGE]")
    module_spec = importlib.util.spec_from_file_location("ecu_model", parts[0])
    module = importlib.util.module_from_spec(module_spec)
    module_spec.loader.exec_module(module)
    image = open(parts[2], "rb").read() if len(parts) == 3 else None
    return getattr(module, parts[1])(image, log=log)


def main():
    ap = argparse.ArgumentParser(description="Simulated OpenPort 2.0 on a pty")
    ap.add_argument("--verbose", action="store_true", help="log to stderr")
    ap.add_argument("--log", metavar="FILE", help="log every command and frame here")
    ap.add_argument("--pty-file", metavar="FILE", help="also write the pty path here")
    ap.add_argument("--no-ack", action="store_true",
                    help="model a bench with no second CAN node")
    ap.add_argument("--backlog", type=int, default=0)
    ap.add_argument("--reply-delay", type=float, default=0.0)
    ap.add_argument("--rx-framing", choices=RX_FRAMINGS, default="measured")
    ap.add_argument("--chunking", choices=CHUNKINGS, default="id_every_chunk")
    ap.add_argument("--kline", choices=KLINE_LAYOUTS, default="asymmetric")
    ap.add_argument("--echo", choices=ECHO_SHAPES, default="mirror_rx")
    ap.add_argument("--tx-done", action="store_true")
    ap.add_argument("--answer-unknown", action="store_true")
    ap.add_argument("--init-reply", choices=("aro", "ary", "arw"), default="aro")
    ap.add_argument("--respond", action="append", default=[], metavar="REQHEX=RESPHEX",
                    help="ECU response; repeatable; REQ '*' is the default answer; "
                         "RESP may end in +gen<N>")
    ap.add_argument("--no-default-ecu", action="store_true")
    ap.add_argument("--ecu-model", metavar="FILE.py:CLASS[:IMAGE]",
                    help="answer as a scripted ECU: CLASS from FILE.py is constructed "
                         "as CLASS(image_bytes_or_None, log=...) and installed as "
                         "sim.ecu_model")
    args = ap.parse_args()

    f = Faults()
    f.no_ack = args.no_ack
    f.startup_backlog = args.backlog
    f.reply_delay = args.reply_delay
    w = Wire()
    w.rx_framing, w.chunking, w.kline_layout = args.rx_framing, args.chunking, args.kline
    w.echo_shape, w.tx_done = args.echo, args.tx_done
    w.answer_unknown, w.init_reply = args.answer_unknown, args.init_reply

    ecu = {} if args.no_default_ecu else {b"\x3e\x00": b"\x7e\x00", b"*": b"\x7f\x22\x11"}
    for spec in args.respond:
        k, v = parse_response_spec(spec)
        ecu[k] = v

    sinks = []
    if args.verbose:
        sinks.append(lambda m: print(m, file=sys.stderr, flush=True))
    if args.log:
        fh = open(args.log, "w", buffering=1)
        sinks.append(lambda m: fh.write(m + "\n"))
    log = (lambda m: [s(m) for s in sinks]) if sinks else None

    path, stop, th, sim = spawn(ecu=ecu, faults=f, log=log, wire=w)
    if args.ecu_model:
        sim.ecu_model = load_ecu_model(args.ecu_model, log or (lambda m: None))
    if log:
        log(f"# openport_sim pty={path} rx_framing={w.rx_framing} chunking={w.chunking} "
            f"kline={w.kline_layout} echo={w.echo_shape} tx_done={w.tx_done} "
            f"answer_unknown={w.answer_unknown} init_reply={w.init_reply} "
            f"ecu={{{', '.join(k.hex() + ':' + v[:16].hex() + ('..' if len(v) > 16 else '') for k, v in ecu.items())}}}")
    if args.pty_file:
        with open(args.pty_file, "w") as pf:
            pf.write(path + "\n")
    print(path, flush=True)
    try:
        while th.is_alive():
            th.join(0.5)
    except KeyboardInterrupt:
        stop.set()


if __name__ == "__main__":
    main()
