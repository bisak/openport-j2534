#!/usr/bin/env python3
"""
ab_diff — compare two j2534_trace runs (the vendor DLL and this driver) call
by call and on the wire, and print the divergences that matter.

  ab_diff.py <vendor.jsonl> <ours.jsonl> [--tap vendor.tap ours.tap] [--md report.md]

Three views:
  1. Return codes and outputs per step: same scenario and step name, so the
     two runs line up by construction.
  2. Wire: the command verbs each library sent, normalised (sequence numbers,
     timestamps and channel-independent noise stripped), diffed per scenario.
  3. Timing: elapsed microseconds per step, both sides. The vendor side runs
     under emulation (box64 interpreter + Wine), so absolute numbers there are
     only meaningful for waits the DLL chooses (timeouts, retry intervals),
     not for CPU-bound work.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import json
import re
import statistics
import sys
from collections import defaultdict


def load(path):
    header, steps, order = None, {}, []
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if not line:
            continue
        o = json.loads(line)
        if o.get("header"):
            header = o
        elif "step" in o:
            key = (o["scenario"], o["step"])
            steps.setdefault(key, []).append(o)
            if key not in order:
                order.append(key)
    return header, steps, order


CMD_RE = re.compile(rb"^(at[a-z])(\d*)\s*(.*)$")


def parse_tap(path):
    """Host->device lines of the tap -> list of (t, verb, args) ; device->host
    text replies -> list of (t, text). Binary frames are counted, not diffed."""
    cmds, replies = [], []
    if not path:
        return cmds, replies
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"\s*([\d.]+) (H>D|D>H) ([0-9a-f ]*) \|", line)
        if not m:
            continue
        t, d, hexs = float(m.group(1)), m.group(2), m.group(3)
        data = bytes.fromhex(hexs.replace(" ", ""))
        if d == "H>D":
            for chunk in data.split(b"\r\n"):
                cm = CMD_RE.match(chunk.strip())
                if cm:
                    cmds.append((t, cm.group(1).decode(), cm.group(3).decode("latin-1")))
        else:
            head = data.split(b"\r\n")[0]
            if head[:2] == b"ar" and len(head) > 2 and chr(head[2]) in "oeifrgy" and all(32 <= c < 127 for c in head):
                replies.append((t, head.decode()))
            else:
                replies.append((t, "<frame %dB>" % len(data)))
    return cmds, replies


PAYLOAD_VERBS = {"att": 0, "atm": 3, "aty": 0}   # index of the length argument; atw carries none


def parse_stream(path):
    """Reassemble the host->device byte stream of a tap into a command list:
    (verb, normalised args, payload bytes). Payload lengths come from the
    command's own arguments, so a driver that writes line and payload in one
    transfer and one that writes them in two parse identically."""
    data = b"".join(bytes.fromhex(m.group(1).replace(" ", ""))
                    for line in open(path, encoding="utf-8", errors="replace")
                    for m in [re.match(r"\s*[\d.]+ H>D ([0-9a-f ]*) \|", line)] if m)
    cmds, i = [], 0
    while i < len(data):
        j = data.find(b"\r\n", i)
        if j < 0:
            break
        line = data[i:j].decode("latin-1"); i = j + 2
        if not line.strip():
            continue
        cm = CMD_RE.match(line.encode("latin-1"))
        if not cm:
            cmds.append(("?", line, b"")); continue
        verb, args = cm.group(1).decode(), cm.group(3).decode("latin-1")
        a = args.split()
        n = 0
        if verb in PAYLOAD_VERBS and len(a) > PAYLOAD_VERBS[verb]:
            n = int(a[PAYLOAD_VERBS[verb]])
        elif verb == "atf" and len(a) >= 3:
            n = int(a[2]) * (3 if a[0] == "3" else 2)
        payload = data[i:i + n]; i += n
        cmds.append((verb, norm_args(verb, args), payload))
    return cmds


def wire_equivalence(vendor_tap, ours_tap, P):
    stats["budget"] = 0
    v = parse_stream(vendor_tap); vbudget = stats["budget"]
    stats["budget"] = 0
    o = parse_stream(ours_tap); obudget = stats["budget"]
    handshake = {"ati", "ata", "atz"}
    # `atl` is what this driver sends for CLEAR_RX_BUFFER; the vendor clears
    # only its host-side buffer. Harmless on the device (measured `aro`), so it
    # is set aside like the handshake and reported, not hidden.
    aside = handshake | {"atl"}
    vb = [c for c in v if c[0] not in aside]; ob = [c for c in o if c[0] not in aside]
    vatl = sum(1 for c in v if c[0] == "atl"); oatl = sum(1 for c in o if c[0] == "atl")
    P("## 2b. Wire equivalence (every command with its payload, in order)\n")
    P("| | vendor | ours |")
    P("|---|---|---|")
    P("| commands on the wire | %d | %d |" % (len(v), len(o)))
    P("| of which open/close handshake (`ati`/`ata`/`atz`) | %d | %d |" % (sum(1 for c in v if c[0] in handshake), sum(1 for c in o if c[0] in handshake)))
    P("| of which `atl` (CLEAR_RX_BUFFER) | %d | %d |" % (vatl, oatl))
    P("| transmits carrying the vendor's constant 1000000 budget argument | %d | %d |" % (vbudget, obudget))
    P("| compared (everything else) | %d | %d |" % (len(vb), len(ob)))
    P("| payload bytes | %d | %d |" % (sum(len(c[2]) for c in v), sum(len(c[2]) for c in o)))
    if vb == ob:
        P("\n**IDENTICAL**: apart from the handshake, `atl`, and the budget argument (all known and documented in PROTOCOL.md section 4), both drivers sent the same %d commands with the same arguments and the same %d payload bytes, in the same order.\n"
          % (len(vb), sum(len(c[2]) for c in vb)))
        return True
    k = next((i for i, (x, y) in enumerate(zip(vb, ob)) if x != y), min(len(vb), len(ob)))
    P("\n**DIFFERENT**: first divergence at command %d of %d/%d:\n" % (k + 1, len(vb), len(ob)))
    for i in range(max(0, k - 2), min(k + 3, max(len(vb), len(ob)))):
        x = vb[i] if i < len(vb) else None; y = ob[i] if i < len(ob) else None
        fmt = lambda c: "`%s %s` + %dB %s" % (c[0], c[1], len(c[2]), c[2][:12].hex(" ")) if c else "-"
        P("- %d: vendor %s / ours %s" % (i + 1, fmt(x), fmt(y)))
    P("")
    return False


KNOWN_ARGS = {"ato": 3, "att": 2, "atf": 3, "ats": 2, "atk": 1, "atc": 0, "atg": 1, "atz": 0, "ata": 0,
              "atr": 1, "atv": 2, "atp": 2, "atm": 4, "atn": 1, "atl": 0, "aty": 2, "atw": 1}
BUDGET = "1000000"     # the vendor DLL's constant fourth att argument (PROTOCOL.md section 4)
stats = {"budget": 0}


def norm_args(verb, args):
    """Strip what legitimately differs between two correct drivers: the
    trailing sequence number (both number their commands, from different
    starts) and the vendor's constant transmit-budget argument, which is
    counted rather than compared."""
    a = args.split()
    k = KNOWN_ARGS.get(verb, len(a))
    if len(a) > k:
        extra = a[k:]
        a = a[:k]
        if verb == "att" and extra and extra[0] == BUDGET:
            stats["budget"] += 1
            extra = extra[1:]
        if extra:
            a = a + extra[:-1] + ["<seq>"]
    return " ".join(a)


def fmt_rc(o):
    s = "rc=%d" % o["rc"]
    if o.get("err"):
        s += " (%s)" % o["err"]
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("vendor"); ap.add_argument("ours")
    ap.add_argument("--tap", nargs=2, metavar=("VENDOR_TAP", "OURS_TAP"))
    ap.add_argument("--md")
    args = ap.parse_args()
    vh, vs, vorder = load(args.vendor)
    oh, os_, oorder = load(args.ours)
    out = []
    P = out.append

    P("# Vendor DLL vs this driver\n")
    P("vendor: %s (%s%s)" % (vh.get("library"), vh.get("platform"), ", under Wine" if vh.get("wine") else ""))
    P("ours:   %s (%s)\n" % (oh.get("library"), oh.get("platform")))

    # 1. return codes and outputs
    P("## 1. Return codes and outputs\n")
    P("| scenario | step | vendor | ours | note |")
    P("|---|---|---|---|---|")
    diverge = 0
    keys = [k for k in vorder] + [k for k in oorder if k not in vs]
    for key in keys:
        v = vs.get(key, [None])[0]; o = os_.get(key, [None])[0]
        if v is None or o is None:
            P("| %s | %s | %s | %s | only one side ran this step |" % (key[0], key[1], fmt_rc(v) if v else "-", fmt_rc(o) if o else "-"))
            continue
        note = []
        if v["rc"] != o["rc"]:
            note.append("**rc differs**")
        for f in ("received", "sent", "vbatt", "firmware", "api", "ch", "filter", "msgid"):
            if f in v and f in o and v[f] != o[f]:
                note.append("%s %r vs %r" % (f, v[f], o[f]))
        vm, om = v.get("msgs", []), o.get("msgs", [])
        if vm or om:
            def shape(ms): return ["rx=%#x size=%d data=%s" % (m["rx"], m["size"], m["data"][:24] + ("…" if len(m["data"]) > 24 else "")) for m in ms]
            if shape(vm) != shape(om):
                note.append("msgs: vendor %s / ours %s" % (shape(vm), shape(om)))
        if note:
            diverge += 1
        P("| %s | %s | %s | %s | %s |" % (key[0], key[1], fmt_rc(v), fmt_rc(o), "; ".join(note)))
    P("\n%d step(s) diverge.\n" % diverge)

    # 2. wire
    if args.tap:
        P("## 2. Wire (commands sent, normalised)\n")
        vc, vr = parse_tap(args.tap[0]); oc, orp = parse_tap(args.tap[1])
        vseq = ["%s %s" % (v, norm_args(v, a)) for _, v, a in vc]
        oseq = ["%s %s" % (v, norm_args(v, a)) for _, v, a in oc]
        vcount = defaultdict(int); ocount = defaultdict(int)
        for s in vseq: vcount[s.split()[0]] += 1
        for s in oseq: ocount[s.split()[0]] += 1
        P("| verb | vendor count | ours count |")
        P("|---|---|---|")
        for verb in sorted(set(vcount) | set(ocount)):
            P("| %s | %d | %d |" % (verb, vcount[verb], ocount[verb]))
        vonly = sorted(set(vseq) - set(oseq)); oonly = sorted(set(oseq) - set(vseq))
        if vonly:
            P("\nCommand forms only the vendor sends:\n")
            for s in vonly: P("- `%s`" % s)
        if oonly:
            P("\nCommand forms only ours sends:\n")
            for s in oonly: P("- `%s`" % s)
        P("\nFirst 12 commands, in order:\n")
        P("| # | vendor | ours |")
        P("|---|---|---|")
        for i in range(12):
            P("| %d | `%s` | `%s` |" % (i + 1, vseq[i] if i < len(vseq) else "", oseq[i] if i < len(oseq) else ""))
        P("")

    if args.tap:
        wire_equivalence(args.tap[0], args.tap[1], P)

    # 3. timing
    P("## 3. Timing per step (microseconds; vendor under emulation)\n")
    P("| scenario | step | vendor us | ours us | ratio |")
    P("|---|---|---|---|---|")
    for key in keys:
        v = vs.get(key); o = os_.get(key)
        if not v or not o:
            continue
        vu = statistics.median(x["us"] for x in v); ou = statistics.median(x["us"] for x in o)
        ratio = (vu / ou) if ou else float("inf")
        P("| %s | %s | %.0f | %.0f | %.1f |" % (key[0], key[1], vu, ou, ratio))
    text = "\n".join(out)
    print(text)
    if args.md:
        open(args.md, "w").write(text + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
