#!/usr/bin/env python3
"""
analyse_capture — turn a car-session capture into verdicts.

Reads the text produced by car_capture.py and answers the open questions from
docs/PROTOCOL.md section 10 directly, so the session's value does not depend on
anyone squinting at hex afterwards.

  python3 tools/car/analyse_capture.py car-session-*/4-protocol-capture.txt

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import re
import sys

FRAME_RE = re.compile(
    r"FRAME ch=(\d) len=(\d+) sts=0x([0-9a-f]{2}) \[([^\]]*)\] ts=(\d+) "
    r"\([\d.]+s\) data\[(\d+)\]=([0-9a-f ]*?)(?: raw=([0-9a-f ]*))?$")
SECTION_RE = re.compile(r"^### (\S+)\.?\s*(.*)$")
INIT_RE = re.compile(r"INIT variant=(\S+) ch=(\d) reply=(\S+) took=(\d+)ms bytes=(.*)$")


def parse(path):
    """Group every decoded frame under the section it appeared in."""
    sections, cur = {}, "preamble"
    for line in open(path, encoding="utf-8", errors="replace"):
        m = SECTION_RE.match(line.strip())
        if m:
            cur = m.group(1).rstrip(".")
            sections.setdefault(cur, {"title": m.group(2), "frames": [], "lines": []})
            continue
        sections.setdefault(cur, {"title": "", "frames": [], "lines": []})
        m = INIT_RE.search(line)
        if m:
            sections[cur].setdefault("inits", []).append({
                "variant": m.group(1), "ch": int(m.group(2)), "reply": m.group(3),
                "took_ms": int(m.group(4)), "bytes": m.group(5).strip()})
            continue
        f = FRAME_RE.search(line)
        if f:
            sections[cur]["frames"].append({
                "ch": int(f.group(1)), "len": int(f.group(2)),
                "sts": int(f.group(3), 16), "bits": f.group(4),
                "ts": int(f.group(5)), "dlen": int(f.group(6)),
                "data": bytes.fromhex(f.group(7).replace(" ", "")) if f.group(7).strip() else b"",
                "raw": (bytes.fromhex(f.group(8).replace(" ", "")) if f.group(8) is not None
                        else None),
            })
        else:
            sections[cur]["lines"].append(line.rstrip())
    return sections


def verdict(name, text):
    print(f"\n{name}\n  {text}")


def q_kline(sections):
    """
    The decisive question: where is the timestamp in a K-line frame?

    Two candidate layouts, and the recording must pick one:

      uniform    every frame is [status][timestamp:4][data...], as on CAN.
                 This was the driver's original assumption.
      asymmetric data frames (status 0x00/0x20) are [status][data...] with no
                 timestamp; start/end/tx-indication frames (0x80/0xA0/0x40/
                 0x60/0x10) are [status][timestamp:4] with no data. This is
                 what three independent implementations describe and what the
                 driver now implements (PROTOCOL.md section 7).

    Under 'uniform' the first four body bytes advance monotonically on EVERY
    frame. Under 'asymmetric' they advance only on start/end frames, and a
    data frame's first byte is a K-line header (0x48/0x68 ISO9141, 0x80-0xBF
    or 0xC0-0xFF ISO14230 format bytes) rather than the high byte of a counter.
    """
    s = sections.get("Q2")
    if not s:
        return verdict("K-line frame layout", "section not present in this capture")
    inits = s.get("inits", [])
    if inits:
        print("\nK-line init replies")
        for i in inits:
            print(f"    {i['variant']:<7} ch={i['ch']} reply={i['reply']:<4} "
                  f"{i['took_ms']:>5} ms  bytes={i['bytes'] or '-'}")
        answered = [i for i in inits if i["reply"] in ("ary", "arw") and i["bytes"]]
        if answered:
            print("  an `ary`/`arw` reply carrying bytes is the device's init response: the")
            print("  StartCommunication answer (`ary` + raw bytes) or the five-baud keybytes")
            print("  (`arw`, decimal on the line). That settles the reply shape (PROTOCOL.md")
            print("  section 3) as [V].")
    # The VW flash-status record (1A 9C -> 5A 9C ...) carries the OBD
    # programming counters. Its layout is not documented here; the operator
    # knows the true values (attempts > successes), so print every reading of
    # the bytes and let the numbers identify themselves.
    for f in s["frames"]:
        if f["ch"] in (3, 4) and not f["sts"] & 0xD0:
            b = f["raw"] if f["raw"] is not None else f["data"]
            i = b.find(b"\x5a\x9c")
            if i >= 0:
                rec = b[i + 2:]
                print("\nFlash status record (1A 9C)")
                print(f"    bytes: {rec.hex(' ')}")
                print("    as 8-bit: " + " ".join(str(x) for x in rec))
                print("    as 16-bit BE: " + " ".join(str(int.from_bytes(rec[j:j+2], 'big'))
                                                    for j in range(0, len(rec) - 1, 2)))
                print("    match the two known counters (attempts, successes) against these.")
            elif b[:3] == b"\x7f\x1a\x80" or b.find(b"\x7f\x1a\x80") >= 0:
                print("\nFlash status record: 1A refused with NRC 0x80 (not in this session). "
                      "Re-run with --allow-diag-session to open VCDS's diagnostic session 10 89.")
    kl = [f for f in s["frames"] if f["ch"] in (3, 4)]
    if not kl:
        return verdict("K-line frame layout",
                       "UNRESOLVED — no K-line frames captured. Either pin 7 has "
                       "no responder on this vehicle, or the init did not "
                       "complete. Check the [init kind] lines for a response.")

    def body(f):
        if f["raw"] is not None:
            return f["raw"]
        return f["ts"].to_bytes(4, "big") + f["data"]     # older capture format

    print(f"\nK-line frame layout\n  {len(kl)} K-line frame(s) captured")
    for f in kl[:12]:
        print(f"    ch={f['ch']} sts=0x{f['sts']:02x} body={body(f).hex(' ')}")

    marker = [f for f in kl if f["sts"] & 0xD0]          # start / end / tx-ind
    data = [f for f in kl if not f["sts"] & 0xD0]        # 0x00 / 0x20

    def monotonic(frames):
        vals = [int.from_bytes(body(f)[:4], "big") for f in frames]
        if len(vals) < 2:
            return None
        return all(b >= a for a, b in zip(vals, vals[1:])) and (max(vals) - min(vals)) > 1000

    # Under the uniform layout EVERY frame carries the counter, so every data
    # frame must be at least 4 bytes and the counter must advance across all
    # frames in order. Under the asymmetric layout the marker frames carry the
    # counter and nothing else, and data frames are exempt. A short data frame
    # rules the uniform layout out on its own; it cannot hold a timestamp.
    short_data = [f for f in data if len(body(f)) < 4]
    uniform_fits = (not short_data) and monotonic(kl)
    asym_fits = (all(len(body(f)) in (0, 4) for f in marker)
                 and (monotonic([f for f in marker if len(body(f)) == 4]) in (True, None)))
    # Tie-break when both readings survive: a K-line data frame begins with an
    # addressing/format byte (0x48/0x68 ISO9141, 0x80+ ISO14230), which a
    # microsecond counter's high byte almost never is.
    headers = bool(data) and all(
        len(body(f)) >= 1 and (body(f)[0] in (0x48, 0x68) or body(f)[0] >= 0x80)
        for f in data)

    if not data:
        verdict("  VERDICT",
                "INCONCLUSIVE — only start/end frames were captured, no data frames. "
                "The counter advanced on the markers, which both layouts predict. "
                "A request that gets an answer is needed; check the init replies above.")
    elif short_data or (asym_fits and not uniform_fits):
        verdict("  VERDICT",
                "ASYMMETRIC layout, as implemented. Data frames carry no counter "
                f"({len(short_data)} of {len(data)} are shorter than a timestamp); "
                "start/end frames carry the counter and nothing else. Promote "
                "PROTOCOL.md section 7 K-line from [P] to [V] and cite this capture.")
    elif uniform_fits and not asym_fits:
        verdict("  VERDICT",
                "UNIFORM layout. The four bytes after the status byte advance "
                "monotonically on every frame, data frames included, and marker "
                "frames carry data beyond the timestamp. The sourced asymmetric rule "
                "in op_frame_has_timestamp() is WRONG for this firmware; revert to the "
                "uniform split and correct PROTOCOL.md section 7.")
    elif uniform_fits and asym_fits:
        verdict("  VERDICT",
                ("ASYMMETRIC (tie-break): both layouts fit the counters, but every "
                 "data frame begins with a K-line header byte, not a counter."
                 if headers else
                 "UNIFORM (tie-break): both layouts fit the counters, and the data "
                 "frames do not begin with K-line header bytes.")
                + " Confirm by eye from the bodies above before changing anything.")
    else:
        verdict("  VERDICT",
                f"INCONCLUSIVE — uniform-fits={uniform_fits} asymmetric-fits={asym_fits} "
                f"short-data-frames={len(short_data)} data-look-like-kline={headers}. "
                "Look at the bodies above; if neither layout fits, both readings are wrong.")


def q_chunking(sections):
    """A reply longer than one 250-byte wire frame: does every chunk repeat
    the CAN id, or only the first? Looks for START-announced messages whose
    data spans more than one END/middle frame."""
    frames = []
    for k in ("Q1", "Q2"):
        if k in sections:
            frames += sections[k]["frames"]
    long_frames = [f for f in frames if f["len"] >= 250]
    if not long_frames:
        return verdict("Long-message chunking",
                       "no wire frame reached 250 bytes; no reply was longer than one "
                       "frame. Still [P]. (The $23 reads were refused or short.)")
    print("\nLong-message chunking")
    for f in long_frames[:6]:
        print(f"    ch={f['ch']} sts=0x{f['sts']:02x} len={f['len']} first bytes={f['data'][:8].hex(' ')}")
    mids = [f for f in frames if f["ch"] == 6 and f["sts"] in (0x00, 0x40) and f["len"] >= 250]
    if mids:
        ids = {f["data"][:4].hex() for f in mids if len(f["data"]) >= 4}
        verdict("  VERDICT",
                f"chunks of 250+ bytes seen; their first four bytes are {sorted(ids)}. "
                "If every chunk begins with the CAN id, the id is repeated per chunk "
                "(the opta-j2534-rs reading) and absorb_frame() must strip it from "
                "every frame after the first; if only the first does, the current "
                "id_first_only model holds.")


def q_echo(sections):
    s = sections.get("Q9")
    if not s:
        return
    lb = [f for f in s["frames"] if f["sts"] & 0x20]
    print("\nTransmit echo (LOOPBACK=1)")
    if not lb:
        return verdict("  VERDICT", "no frame with the LOOPBACK bit arrived; either the "
                       "config did not take (see the ats6/atg6 lines) or echo is not "
                       "delivered on this firmware.")
    for f in lb:
        print(f"    sts=0x{f['sts']:02x} len={f['len']} data={f['data'].hex(' ')}")
    statuses = sorted({f["sts"] for f in lb})
    verdict("  VERDICT",
            f"echo statuses {[hex(x) for x in statuses]}. 0xA0 with the id only then "
            "0x60 with id+data = mirrors the receive framing (current model); a single "
            "0x60 or 0xE0 with id+data = one-frame echo. Update the simulator's "
            "echo_shape and PROTOCOL.md section 7 accordingly.")


def q_raw_can(sections):
    s = sections.get("Q10")
    if not s:
        return
    fr = [f for f in s["frames"] if f["ch"] == 5]
    print("\nRaw CAN listen")
    if not fr:
        return verdict("  VERDICT", "no raw CAN frames in 3 s: the OBD port carries no "
                       "broadcast traffic (gateway-isolated diagnostic CAN), or the "
                       "pass-all filter form is wrong. Check the 'pass-all filter ->' lines.")
    statuses = sorted({f["sts"] for f in fr})
    lens = sorted({f["dlen"] for f in fr})
    ids = sorted({f["data"][:4].hex() for f in fr if len(f["data"]) >= 4})[:12]
    verdict("  VERDICT",
            f"{len(fr)} frames; status bytes {[hex(x) for x in statuses]}; data lengths "
            f"{lens} (id + 0..8 bytes expected); ids seen {ids}. This is the raw CAN "
            "receive format for protocol 5, unmeasured until now.")


def q_config(sections):
    s = sections.get("Q11")
    if not s:
        return
    print("\nConfiguration read-back")
    known = {}
    for line in s["lines"]:
        m = re.match(r"\s*atg(\d) (\d+)\s+-> '(arg\d (\d+) (\d+) \d+)'", line)
        if m:
            known.setdefault(m.group(1), []).append((int(m.group(2)), int(m.group(5))))
    for ch, items in sorted(known.items()):
        print(f"    channel {ch}: {len(items)} parameter ids answered: "
              + ", ".join(f"{pid}={val}" for pid, val in items))
    if not known:
        print("    no parameter answered")


def q_received(sections):
    frames = []
    for k in ("Q0", "Q1"):
        if k in sections:
            frames += [f for f in sections[k]["frames"] if f["ch"] == 6]
    if not frames:
        return verdict("Received CAN framing",
                       "no ISO15765 frames captured — check the CAN ids")

    statuses = sorted({f["sts"] for f in frames})
    multi = [f for f in frames if f["sts"] & 0x80 and not f["sts"] & 0x40]
    print(f"\nReceived CAN framing\n  {len(frames)} frame(s)")
    print(f"  status bytes seen: {', '.join(f'0x{s:02x}' for s in statuses)}")
    known = {0x80, 0x40, 0x20, 0x10}
    unknown_bits = set()
    for s in statuses:
        for b in range(8):
            if s & (1 << b) and (1 << b) not in known:
                unknown_bits.add(1 << b)
    if unknown_bits:
        verdict("  NEW BITS",
                "status bits not in our model: " +
                ", ".join(f"0x{b:02x}" for b in sorted(unknown_bits)) +
                " — PROTOCOL.md section 7 needs updating.")
    else:
        verdict("  VERDICT", "every status bit seen is already modelled "
                             "(START 0x80 / END 0x40 / LOOPBACK 0x20 / 0x10).")
    if multi:
        verdict("  MULTI-FRAME",
                f"{len(multi)} frame(s) opened a message without closing it, "
                "so genuine multi-frame reassembly was exercised.")
    else:
        verdict("  MULTI-FRAME",
                "every message fitted in one frame. Multi-frame reassembly "
                "was NOT exercised against hardware; try mode 09 PID 02 (VIN) "
                "or a longer $22 identifier.")


def q_pins(sections):
    s = sections.get("Q7")
    if not s:
        return
    readings = {}
    for line in s["lines"]:
        m = re.search(r"pin (\d+)\s+-> 'arr (\d+) (\d+)", line)
        if m:
            readings[int(m.group(2))] = int(m.group(3))
    if not readings:
        return verdict("Pin readings", "none captured")
    print("\nPin readings")
    for pin, mv in sorted(readings.items()):
        print(f"    pin {pin:<3} = {mv:>6} mV ({mv/1000:.2f} V)")
    vb = readings.get(16)
    if vb and 11000 <= vb <= 15000:
        verdict("  VERDICT",
                f"pin 16 reads {vb/1000:.2f} V with the vehicle attached, "
                "confirming READ_VBATT is millivolts. PROTOCOL.md section 8 "
                "can drop its [U] marker.")
    elif vb:
        verdict("  VERDICT", f"pin 16 reads {vb} mV — unexpected for a "
                             "connected vehicle; treat scaling as unconfirmed.")


def q_periodic(sections):
    s = sections.get("Q4")
    if not s:
        return
    accepted = [l for l in s["lines"] if "ACCEPTED" in l]
    rejects = [l for l in s["lines"] if "interval=" in l]
    print(f"\nPeriodic message interval encoding\n  {len(rejects)} value(s) tried")
    for l in rejects[:12]:
        print("   " + l.strip())
    verdict("  VERDICT",
            "an interval was accepted — measure the frame spacing above to get "
            "the unit" if accepted else
            "every interval was rejected; the host-side scheduler stays the "
            "right implementation.")


def q_unknown(sections):
    for key, label in (("Q5", "atm / atw / atx / aty"),
                       ("Q6", "two-digit protocol numbers")):
        s = sections.get(key)
        if not s:
            continue
        lines = [l for l in s["lines"] if "->" in l]
        if lines:
            print(f"\n{label}")
            for l in lines[:14]:
                print("   " + l.strip())


def main():
    ap = argparse.ArgumentParser(description="Interpret a car-session capture")
    ap.add_argument("capture", nargs="+", help="one or more capture files; sections merge")
    args = ap.parse_args()

    sections = {}
    for path in args.capture:
        for name, sec in parse(path).items():
            dst = sections.setdefault(name, {"title": sec["title"], "frames": [], "lines": []})
            dst["frames"] += sec["frames"]
            dst["lines"] += sec["lines"]
            if "inits" in sec:
                dst.setdefault("inits", []).extend(sec["inits"])
    print(f"=== {', '.join(args.capture)} ===")
    print(f"sections: {', '.join(k for k in sections if k.startswith('Q'))}")

    q_kline(sections)
    q_chunking(sections)
    q_echo(sections)
    q_raw_can(sections)
    q_config(sections)
    q_received(sections)
    q_pins(sections)
    q_periodic(sections)
    q_unknown(sections)

    print("\nNext: fold confirmed answers into docs/PROTOCOL.md section 10 and "
          "replace the simulator's MODELLED behaviours with measured ones.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
