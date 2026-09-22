# Vehicle session — checklist

One session, everything captured, nothing risked. This is **confirmation**, not
qualification: no claim in `docs/PROTOCOL.md` and nothing in CI depends on it.

**Status.** Run on 2026-09-13 on the Audi below (raw CAN and TP2.0 worked, the
K-line answered nothing) and on a 2012 VW Caddy (ISO15765). What each settled
is in `PROTOCOL.md` §7 and §11; the table at the end says which questions are
closed. The session that still matters is one on a vehicle that answers on
K-line, or a bench OBD simulator that does (`PROTOCOL.md` §12).

## Which car

Recommended: **the 2009 Audi 1.9 TDI (EDC16C34)**.

| Candidate | Verdict |
|---|---|
| **Audi 2009 1.9 TDI** | **Use this.** Pre-dates gateway authentication and secure onboard communication, so nothing we send can trip a security counter or leave a logged event. Its **K-line is confirmed live — the owner has read and written this ECU through it with a Galletto 1260**, which flashes EDC16 over the OBD K-line at the VAG engine address — and K-line framing is the single unresolved question in the parser. Not the project car. |
| VW 2012 | Fine as a second opinion. Slightly more likely to have a gateway between the OBD port and the powertrain bus. |
| Toyota 2021 | **Avoid.** Modern gateway and message-authentication designs may log or reject unexpected diagnostic traffic. Nothing here would harm it, but there is no upside and a needless chance of a stored event. |

## What is known about this ECU, and what it changes

Researched 2026-09-11 so the envelope is argued from the hardware, not
assumed. Sources at the end of this section.

- **EDC16C34 / EDC16U34 is a Freescale MPC562 PowerPC with external flash**
  (no on-chip flash on the MPC562) and a serial EEPROM. Bosch used the MPC5xx
  across EDC16. The K-line is on ECU pin 72, wired to OBD pin 7.
- **The Galletto 1260 is a K-line-only FTDI cable.** That the ECU was read and
  written with it proves pin 7 reaches the ECU and that the ECU speaks
  KWP2000 there — and that this K-line is the flash path.
- **The ECU keeps two counters in EEPROM: OBD programming attempts and
  successful OBD programmings.** They move only when a programming session is
  entered; bench/boot reads do not touch them and neither does diagnostics.
  Nothing this script sends can start a programming session (`$10` is refused
  by name), so the counters stay where the Galletto left them.
- **Why `$23 ReadMemoryByAddress` is not sent by default.** It is a read, but
  its address is the ECU's own address space and the EDC16 memory map is not
  established here (external flash is commonly reported at 0x800000; address 0
  may be unmapped on a flashless MPC562). A read of an unmapped address can
  raise a bus fault inside the ECU. So the long-read probe is opt-in
  (`--long-read ADDR --long-read-kline fast01`), needs an address the operator
  can vouch for, and is doubly gated.
- **What the requests mean to this ECU.** Modes `$01`/`$09` at 0x7DF are the
  legislated EOBD it must answer. `$22`, `$3E` at 0x7E0 and the KWP
  `$1A`/`$3E`/`$81` on the K-line are what VCDS and any scan tool send; the
  worst case is a negative response, which is recorded as a result. None of
  them write EEPROM, set a DTC, or change session state.
- **Battery.** Ignition on, engine off, on a diesel draws a few amps
  (dash, pumps priming, glow control). Fifteen minutes is fine on a healthy
  battery; on a doubtful one, connect a charger rather than run the engine.

Sources: [Autotuner EDC16C34 (MPC562)](https://www.autotuner.com/pages/ecu/bosch-edc16c34-mpc562),
[MPC5xx family](https://en.wikipedia.org/wiki/MPC5xx),
[EDC16U34 pinout](https://ecu.design/ecu-pinout/pinout-bosch-edc16u34-xrom-mpc562-egpt-vag/),
[Flashtec: EDC16 VAG counters](https://www.flashtec.ch/Files/counters_eng.pdf),
[digital-kaos on the counters](https://www.digital-kaos.co.uk/forums/showthread.php/426090-vag-ecu-flash-counter),
[Galletto 1260, K-line only](https://www.astools.eu/products/galletto-1260-eobd-programmer-obd2-usb-interface-cable),
[MPC561/563 Reference Manual](https://www.nxp.com/docs/en/data-sheet/MPC561RM.pdf).

## Safety, as enforced by the script

Not promises — checks in `tools/car/car_capture.py` that run every time.

1. **A hard CAN-id envelope.** Every transmit goes through `guard_tx()`, which
   permits only `0x7DF`–`0x7FF`: the ISO 15765-4 diagnostic range. Below
   `0x7DF` is where real powertrain and chassis broadcasts live, and a frame
   injected with one of those arbitration ids can be read by a listening module
   as genuine vehicle data. The script refuses, and there is a unit check for it.
2. **Pre-flight abort.** It reads pin 16 first and refuses to transmit anything
   at all unless it sees 11–14.5 V. On the bench it aborts before putting a
   single frame on the wire — verified.
3. **Read-only services only.** `$3E` TesterPresent, `$01`/`$09` legislated OBD
   modes, `$22` ReadDataByIdentifier. No write services, no session changes, no
   security access, no routine control.
4. **No programming voltage.** The script never sends `atv` and never calls
   `PassThruSetProgrammingVoltage`.
5. **Periodic messages are always stopped**, in a `finally`, with a channel
   close as a backstop — so nothing keeps transmitting after we unplug.
6. **A read-only service whitelist**, enforced by `guard_service()` on both CAN
   and K-line. This matters more here than anywhere else: **this ECU is flashed
   over its K-line**, so the physical layer being probed is the same one a
   programming tool uses, and a read request and an erase request travel
   identically — only the service id distinguishes them. Permitted: OBD modes
   01/02/03/06/07/09/0A, TesterPresent, ReadDataByIdentifier,
   ReadEcuIdentification, KWP Start/StopCommunication. Refused by name and with
   a reason: `0x04` clear-DTCs, `0x10` session control, `0x27` security access,
   `0x2E`/`0x3B`/`0x3D` writes, `0x31` routine control, `0x34`/`0x36` the flash
   download services, and every other non-read service. Anything not on the
   whitelist is refused by default.
7. **K-line is opt-in** (`--kline`), because it is the one section that drives a
   different physical layer. Four standard tester wake-ups are tried: the EOBD
   functional address 0x33 with five-baud (ISO9141-2) and fast init
   (ISO14230-4), and the VAG engine address 0x01 with both — the address
   VCDS and the Galletto use. Only StartCommunication and whitelisted reads
   follow; the init command form is the one Tactrix's own DLL uses
   (`PROTOCOL.md` §4). The previous form would have desynchronised the cable.
8. **Verbs of unknown meaning are bench-only.** Q5 (`atx`, and the bare
   forms of `atm` and `atw`) does not run on a vehicle unless
   `--unknown-verbs` is passed: "read-only" cannot be claimed for a command
   nobody understands.
9. **Ctrl-C leaves the cable silent.** Any exit path stops periodic messages,
   closes every channel and resets the cable.
10. **Every exchange is recorded raw.** Each command's bytes, payload, latency
    to the first reply byte and the complete reply hex go into the capture as
    `RAW` lines, so a wrong interpretation at the car costs nothing.
11. **The whole script has been rehearsed** end-to-end against the simulator,
    including the K-line variants and the live-mode requests, so the session
    is not also a debugging session.

## Before you go

- Ignition **on**, engine **off**. Engine running adds nothing.
- The cable's microSD card removed.
- Nothing else plugged into the OBD port.
- Expect roughly 10 minutes. Ignition-on for that long is not a battery concern
  on a healthy car, but do not leave it for an hour.

## At the car

Three steps. Everything below them is reference for the operator driving the
session, not a checklist for the owner.

1. **Cable in, microSD out, ignition on, engine off.** Nothing else on the
   OBD port.
2. **One command:** `tools/car/car-session.sh`. About fifteen minutes. It
   rebuilds, runs every capture section including the K-line wake-ups and the
   flash-counter read, the differential against the old driver, the
   driver-level K-line check, prints the analysis, and archives the session
   directory as a `.tgz`.
3. **The operator reads `9-analysis.txt` before the cable comes out** and runs
   at most a couple of single guarded requests to close anything inconclusive.

The one decision that may come up: if the ECU refuses the flash-counter read
outside a diagnostic session, the follow-up needs the diagnostic session VCDS
opens on every connection (`10 89`). It is not the programming session and
does not touch the counters, but it is a session change, so it is off unless
the owner says yes.

### Operator reference

Single requests, each running the pre-flight and the whitelist first:

```bash
python3 tools/car/car_capture.py --request 090A --out live-1.txt                       # one CAN request
python3 tools/car/car_capture.py --kline-init fast01 --kline-request 1A9C --out live-2.txt   # flash counters
python3 tools/car/car_capture.py --kline-init fast01 --kline-request 1A9C --allow-diag-session --out live-3.txt
python3 tools/car/car_capture.py --kline --kline-variants five01 --only q2 --no-preflight --out live-4.txt
python3 tools/car/analyse_capture.py live-*.txt
```

K-line init variants: `five33`, `fast33` (EOBD address 0x33), `five01`,
`fast01` (VAG engine address 0x01). Sections: q7 pins, q0 EOBD, q1 framing,
q2 K-line, q3 transmit arguments, q8 silent bus, q9 echo, q10 raw CAN listen,
q11 config read-back, q6 protocol numbers; q5 (unknown verbs) and
`--long-read` are bench-only and off.

Verdicts that decide driver code: K-line layout → `op_frame_has_timestamp()`;
long-message chunking → `absorb_frame()`; echo shape → the simulator default;
the init replies → `aty`/`ary` in PROTOCOL.md §4; a correct counter read →
the whole K-line path is confirmed on this ECU.

## Running it

```bash
cd ~/DEV/openport-j2534
tools/car/car-session.sh
```

Everything lands in `car-session-<timestamp>/`. To include the highest-value
section:

```bash
python3 tools/car/car_capture.py --kline --out kline.txt
```

If the pre-flight reports no response to TesterPresent, the addresses are wrong
for this vehicle — try the functional broadcast:

```bash
python3 tools/car/car_capture.py --tx 0x7DF --out capture.txt
```

To run only some K-line variants (for example just the VAG-address ones):

```bash
python3 tools/car/car_capture.py --kline --kline-variants five01,fast01 --out kline-vag.txt
```

## To stop early

Ctrl-C at any point. Every section is independent, output is flushed as it goes,
and the sections are ordered cheapest-and-safest first so an early abort still
leaves the most valuable data captured.

## What this closes

| Question | Section | Status |
|---|---|---|
| Received-message framing with real payload | `q0`, `q1` | closed on the Caddy (`PROTOCOL.md` §7) |
| **K-line frame layout** | `q2` (needs `--kline`; four init variants: EOBD 0x33 five-baud and fast, VAG 0x01 five-baud and fast; the init replies settle the `arw`/`ary` shapes too) | **open**: no K-line responder yet |
| Is `att`'s third argument TxFlags, and is the DLL's five-argument form accepted? | `q3` | closed (§4) |
| **Chunking of a reply longer than 250 bytes** | opt-in `--long-read ADDR`; not on this ECU (memory map unknown) | **open**; the vendor DLL's reading is implemented (§7.6) |
| **Transmit echo shape** with LOOPBACK=1 on ISO15765 | `q9` | **open** |
| **Raw CAN frame format** and what traffic reaches the port; does SNIFF_MODE open | `q10` (receive only) | closed (§7.8, §10) |
| Which configuration ids the firmware knows, and their defaults | `q11` | closed (§8) |
| The driver's own K-line path end to end (init ioctl, write, read) | `car-session.sh` step 7, `examples/op_kline` | **open**: no K-line responder yet |
| VW TP2.0 over raw CAN, and the EDC16 flash counters | `--tp20`; `--allow-diag-session` if the ECU wants `10 89` first | TP2.0 works (§11) |
| `atx` (`atm` and `atw` are known: periodic message and five-baud init) | `q5` (bench only, `--unknown-verbs`) | open, unimportant |
| Two-digit protocol numbers, and the DLL's five-argument `ato` | `q6` | closed (§5, §12) |
| Pin 16 reads battery voltage | `q7` | closed (§8) |
| How the firmware reports a silent bus | `q8` | closed (§6) |
| Differential vs a reference driver, live | `car-session.sh` step 5 (`OLD_DRIVER=...`, macOS) | not run live; the live comparisons were made against the vendor DLL (`AB-OFFICIAL.md`) |

## Reading the results

```bash
python3 tools/car/analyse_capture.py car-session-*/4-protocol-capture.txt
```

It answers the open questions directly rather than leaving hex to interpret.
The decisive one: from the raw K-line frame bodies it decides between the
uniform layout (a timestamp on every frame, the driver's original guess) and
the asymmetric one (timestamp only on start/end frames, what three independent
drivers describe and what the driver now implements). It names the layout,
says whether `op_frame_has_timestamp()` must change, and lists the init
replies, which settle the `aty`/`ary` command form.

Because the Audi's K-line is live, this should actually resolve — unlike a
vehicle where pin 7 has no responder and the capture records only silence.

## Reading the flash counters — a ground truth we can check

The EDC16 keeps its two OBD programming counters in EEPROM, and VCDS shows
them under Advanced ID as "Flash Status: Programming Attempts / Successful
Attempts". On VW KWP2000 modules that screen comes from ReadEcuIdentification
with identification option 0x9C, the "flash status" record — a read, and the
same request `pq-flasher` uses (`ECU_IDENTIFICATION_TYPE.STATUS_FLASH = 0x9C`).
The K-line VAG variants of `q2` now send `1A 9C` after `1A 9B`/`91`, and the
analyser prints the record's bytes as 8-bit and 16-bit numbers. The owner
knows the true values (more failed attempts than successes), so the numbers
identify themselves — and a correct read is proof the whole K-line path,
init, framing and reassembly, is right on this ECU.

If the ECU answers `7F 1A 80` (not allowed in this session), VCDS's own
diagnostic session is needed first. That is `10 89`, a session change and so
refused by default. `--allow-diag-session` unlocks exactly that one
subfunction for the run; the programming session `10 85`, the only thing that
moves the counters, stays refused whatever flags are given. The choice to use
it is made at the car, knowing that VCDS sends `10 89` on every connection.

## Session of 2026-09-13 — what the Audi actually gave us

**The car answered nothing on either bus, and that is a finding, not a
failure.** A VAG module is not addressed with ISO15765; it is reached with VW
TP2.0, which opens a channel on CAN id 0x200 and lets the module name the pair
of ids to use. Every request this session sent was ISO-TP addressed to 0x7E0 or
0x7DF, which no VAG module listens for. The bus was live throughout: our frames
were acknowledged, so the gateway was there, ignoring us. The empty raw-CAN
listen fits too, because VAG's diagnostic CAN stays quiet until a tester opens
a channel.

On K-line, every standard wake-up returned `are 7`. On a 2009 VAG the engine is
reached over CAN, so pin 7 most likely has no engine responder. Nothing here
indicts the driver's K-line code; it simply cannot be exercised on this car.

**Two real driver bugs fell out of it anyway.**

- Five-baud init used the wrong verb. `aty` is fast init; the five-baud verb is
  `atw`, and the difference is unmistakable on the wire — `atw` blocks about
  2450 ms, exactly one byte clocked out at 5 baud, while `aty` returns in about
  108 ms. Every `FIVE_BAUD_INIT` call had been silently performing a fast init.
- The `0x10` status is the normal transmit indication on *every* transmit, not
  the rarity the protocol notes described. A driver that reads it as data
  reports a phantom message on every write.

**What it could not settle.** The receive path. The driver's central correction,
the one this whole effort turned on, is still anchored to a recording made
through the previous driver in June. No vehicle has yet handed *this* driver a
received message with real payload. That is the single most valuable thing a
next car can provide, and almost any post-2008 non-VAG vehicle can.

**Left ready for next time.** `--tp20` opens a TP2.0 channel over the raw CAN
channel and runs the identification reads inside it, including the flash-status
record that carries the programming counters. It is covered by the `s_tp20`
scenarios against a simulated module, but it has never met a real one: the
ignition had timed out by the time it was built, and the pre-flight refused to
transmit onto a bus it could not confirm was powered. That refusal working on
real hardware is itself the first live test of the safety envelope.

To resume, with the ignition on:

```bash
python3 tools/car/car_capture.py --tp20 --out tp20.txt
python3 tools/car/car_capture.py --tp20 --allow-diag-session --out tp20-session.txt
```

## Afterwards

Send back the session directory. `PROTOCOL.md` §12 gets folded down, the
simulator's `MODELLED` behaviours get replaced with measured ones, and the
captured frames become golden-trace regression fixtures — so the session's value
persists rather than expiring with it.
