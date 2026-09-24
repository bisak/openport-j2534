# Vehicle session checklist

One read-only session on a vehicle, with everything captured. Nothing in the
test suite depends on it: vehicle recordings reach the suite as golden traces
(`tests/unit/test_golden.c`), and a session adds recordings.

Status. Run on 2026-09-13 on the 2009 Audi below (raw CAN worked; the K-line
answered no init; TP2.0 reached the engine module later that day, see the end
of this file) and on a 2012 VW Caddy (ISO15765 and raw CAN). What each settled
is in `PROTOCOL.md` §7 and
§11; the table under [What this closes](#what-this-closes) says which questions
are closed. The session that still matters is one on a vehicle that answers on
K-line, or on a bench OBD simulator that does (`PROTOCOL.md` §12).

## Which vehicle

Candidates considered before the first session:

| Candidate | Assessment |
|---|---|
| Audi 2009 1.9 TDI (EDC16C34) | Chosen for the first session. It predates gateway authentication and secure onboard communication, so nothing sent can trip a security counter or leave a logged event. The owner has read and written this ECU with a Galletto 1260, which flashes EDC16 over the OBD K-line at the VAG engine address, so its K-line was expected to answer; K-line framing is the one unresolved question in the parser. In the session it answered no init at any address tried (see the end of this file). Not the project car. |
| VW 2012 | Fine as a second opinion. Slightly more likely to have a gateway between the OBD port and the powertrain bus. |
| Toyota 2021 | Avoid. Modern gateway and message-authentication designs may log or reject unexpected diagnostic traffic. Nothing here would harm it, but there is nothing to gain and some chance of a stored event. |

## What is known about the Audi's ECU, and what it changes

Researched on 2026-09-11 so that the limits below follow from the hardware.
Sources at the end of this section.

- EDC16C34 / EDC16U34 is a Freescale MPC562 PowerPC with external flash (the
  MPC562 has no on-chip flash) and a serial EEPROM. Bosch used the MPC5xx
  across EDC16. The K-line is on ECU pin 72, wired to OBD pin 7.
- The Galletto 1260 is a K-line-only FTDI cable. If the owner's reads and
  writes went through the OBD port, pin 7 reaches the ECU, the ECU speaks
  KWP2000 there, and that K-line is its flash path. The session got no answer
  on pin 7, so this is unconfirmed: the Galletto may have been used at the ECU
  connector or in boot mode (`AB-OFFICIAL.md`).
- The ECU keeps two counters in EEPROM: OBD programming attempts and successful
  OBD programmings. They move only when a programming session is entered;
  bench and boot-mode reads do not touch them, and neither does diagnostics.
  Nothing the capture tool sends can start a programming session (`$10` is
  refused by name, apart from the one diagnostic session `10 89` that
  `--allow-diag-session` unlocks), so the counters stay where the Galletto left
  them.
- `$23 ReadMemoryByAddress` is not sent by default. It is a read, but its
  address is in the ECU's own address space, and the EDC16 memory map is not
  established here (external flash is commonly reported at 0x800000; address 0
  may be unmapped on a flashless MPC562). A read of an unmapped address can
  raise a bus fault inside the ECU. The long-read probe is therefore opt-in
  (`--long-read ADDR`, with `--long-read-kline fast01` for the K-line), needs an
  address the operator can vouch for, and is gated twice.
- What the requests mean to this ECU. Modes `$01`/`$09` at 0x7DF are the
  legislated EOBD it must answer. `$22`, `$3E` at 0x7E0 and the KWP
  `$1A`/`$3E`/`$81` on the K-line are what VCDS and any scan tool send; the
  worst case is a negative response, which is recorded as a result. None of
  them writes EEPROM, sets a DTC, or changes session state.
- Battery. Ignition on and engine off, a diesel draws a few amps (dash, pumps
  priming, glow control). Fifteen minutes is fine on a healthy battery; on a
  doubtful one, connect a charger rather than run the engine.

Sources: [Autotuner EDC16C34 (MPC562)](https://www.autotuner.com/pages/ecu/bosch-edc16c34-mpc562),
[MPC5xx family](https://en.wikipedia.org/wiki/MPC5xx),
[EDC16U34 pinout](https://ecu.design/ecu-pinout/pinout-bosch-edc16u34-xrom-mpc562-egpt-vag/),
[Flashtec: EDC16 VAG counters](https://www.flashtec.ch/Files/counters_eng.pdf),
[digital-kaos on the counters](https://www.digital-kaos.co.uk/forums/showthread.php/426090-vag-ecu-flash-counter),
[Galletto 1260, K-line only](https://www.astools.eu/products/galletto-1260-eobd-programmer-obd2-usb-interface-cable),
[MPC561/563 Reference Manual](https://www.nxp.com/docs/en/data-sheet/MPC561RM.pdf).

## Safety, as enforced by the capture tool

`tools/car/car_capture.py` talks to the cable's serial node directly, not
through `libj2534`. These are checks in it, not intentions:

1. A hard CAN-id envelope. Every transmit goes through `guard_tx()`, which
   permits only `0x7DF`–`0x7FF`, the ISO 15765-4 diagnostic range. Below
   `0x7DF` is where real powertrain and chassis broadcasts live, and a frame
   injected with one of those arbitration ids can be read by a listening module
   as genuine vehicle data. With `--tp20` it also permits `0x200` (TP2.0
   channel setup) and the one id the module returns.
2. A pre-flight abort. It reads pin 16 first and transmits nothing unless it
   reads at least 11 V. On the bench it aborts before putting a single frame on
   the wire, which was verified. It is skipped with `--no-preflight`, and when
   `--only` runs a single section. `car-session.sh` skips it only for its
   K-line step (4b), which follows the main capture's pre-flight but runs even
   if that pre-flight aborted.
3. Read-only services only, enforced by `guard_service()` on CAN and K-line
   alike. This matters most on the Audi: its ECU is flashed over its K-line,
   so the physical layer being probed is the one a programming tool uses, and a
   read request and an erase request travel identically; only the service id
   tells them apart. Permitted: OBD modes 01, 02, 03, 05, 06, 07, 09 and 0A;
   TesterPresent; ReadDataByIdentifier; KWP ReadEcuIdentification,
   ReadDataByLocalIdentifier and ReadDiagnosticTroubleCodesByStatus; KWP
   StartCommunication and StopCommunication; and ReadMemoryByAddress only
   through `--long-read`. Refused by name, with a reason: `0x04` clear DTCs,
   `0x08`, `0x10` session control, `0x11`, `0x14`, `0x27` security access,
   `0x28`, `0x2E`/`0x3B`/`0x3D` writes, `0x2F`, `0x31` routine control,
   `0x34`–`0x38` the transfer services, `0x85` and `0x87`. Anything not on the
   whitelist is refused by default.
4. No programming voltage. The tool never sends `atv`.
5. It stops what it started. On every exit path, Ctrl-C included, it stops
   periodic messages, closes every channel it uses and resets the cable, so
   nothing keeps transmitting after the cable comes out.
6. K-line is opt-in (`--kline`), because it is the one section that drives a
   different physical layer. Five tester wake-ups are tried: the EOBD
   functional address 0x33 with five-baud (ISO9141-2) and fast init
   (ISO14230-4), the VAG engine address 0x01 with both (the address VCDS and
   the Galletto use), and five-baud to 0x10, the Bosch default. Only
   StartCommunication and whitelisted reads follow. The init commands are the
   ones Tactrix's DLL sends (`PROTOCOL.md` §4); the form used before would have
   desynchronised the cable.
7. Verbs of unknown meaning are bench-only. Section q5 (`atx`, and the bare
   forms of `atm` and `atw`) does not run unless `--unknown-verbs` is passed:
   "read-only" cannot be claimed for a command nobody understands.
8. Every exchange is recorded raw. Each command's bytes, payload, latency to
   the first reply byte and the complete reply hex go into the capture as `RAW`
   lines, so a wrong interpretation at the car costs nothing.
9. The whole script has been rehearsed end to end against the simulator,
   including the K-line variants and the live-mode requests, so the session is
   not also a debugging session.

The examples `car-session.sh` runs are read-only too: `op_smoke --tx` sends one
TesterPresent, and `op_kline` sends StartCommunication, ReadEcuIdentification
and TesterPresent.

## Before you go

- Ignition on, engine off. A running engine adds nothing.
- The cable's microSD card removed.
- Nothing else plugged into the OBD port.
- Expect about fifteen minutes. Ignition-on for that long is not a battery
  concern on a healthy car, but do not leave it for an hour.

## At the car

Three steps. Everything after them is reference for the operator running the
session, not a checklist for the owner.

1. Cable in, microSD out, ignition on, engine off. Nothing else on the OBD
   port.
2. Run `tools/car/car-session.sh` (below). It takes about fifteen minutes. It
   rebuilds, reads the USB descriptors, runs `op_smoke` over USB and over the
   serial node, runs every capture section, then the K-line wake-ups including
   the flash-counter read, the differential against the old driver if
   `OLD_DRIVER` is set (macOS), a hex-logged `op_smoke --tx`, and the
   driver-level K-line check `op_kline`; it prints the analysis and archives
   the session directory as a `.tgz`.
3. The operator reads `8-analysis.txt` before the cable comes out and runs at
   most a couple of single guarded requests to close anything inconclusive.

One decision may come up. If the ECU refuses the flash-counter read outside a
diagnostic session, the follow-up needs the diagnostic session VCDS opens on
every connection (`10 89`). It is not the programming session and does not
touch the counters, but it is a session change, so it stays off unless the
owner agrees.

### Operator reference

Single requests, each checked against the whitelist and, unless
`--no-preflight` is given, preceded by the pre-flight. On Linux add
`--dev /dev/ttyACM0`; the default is the first `/dev/cu.usbmodem*`.

One CAN request:

```bash
python3 tools/car/car_capture.py --request 090A --out live-1.txt
```

The flash counters over the K-line:

```bash
python3 tools/car/car_capture.py --kline-init fast01 --kline-request 1A9C --out live-2.txt
```

The same, allowing the `10 89` diagnostic session:

```bash
python3 tools/car/car_capture.py --kline-init fast01 --kline-request 1A9C --allow-diag-session --out live-3.txt
```

One K-line wake-up variant, without the pre-flight:

```bash
python3 tools/car/car_capture.py --kline --kline-variants five01 --only q2 --no-preflight --out live-4.txt
```

Analyse them:

```bash
python3 tools/car/analyse_capture.py live-*.txt
```

K-line init variants: `five33`, `fast33` (EOBD address 0x33), `five01`,
`fast01` (VAG engine address 0x01), `five10` (0x10). Sections, in run order:
q7 pins, q0 EOBD, q1 framing, q2 K-line, q3 transmit arguments, q8 silent bus,
q9 echo, q10 raw CAN listen, q11 configuration read-back, q5 unknown verbs, q6
protocol numbers. q5 is bench-only and off by default; q2 needs `--kline`;
`--long-read` is off unless given an address.

Verdicts that decide driver code: the K-line layout decides
`op_frame_has_timestamp()`; the init replies decide the `arw`/`ary` shapes in
`PROTOCOL.md` §3; a correct flash-counter read confirms the whole K-line path
on this ECU. Long-message chunking (`absorb_frame()`) and the echo shape (the
simulator default) were decided on the bench ECU (`PROTOCOL.md` §7.6, §7.7).

## Running it

From the repository root:

```bash
tools/car/car-session.sh
```

Everything lands in `car-session-<timestamp>/` and `car-session-<timestamp>.tgz`
(an output directory can be given as the first argument). `OPENPORT_DEV` names
the cable's serial node if the first `/dev/cu.usbmodem*` or `/dev/ttyACM*` is
not it, and `OLD_DRIVER` enables the differential step on macOS.

The K-line section on its own:

```bash
python3 tools/car/car_capture.py --kline --out kline.txt
```

If the pre-flight reports no response to TesterPresent, the addresses are wrong
for this vehicle; try the functional broadcast:

```bash
python3 tools/car/car_capture.py --tx 0x7DF --out capture.txt
```

Only some K-line variants, for example the VAG-address ones:

```bash
python3 tools/car/car_capture.py --kline --kline-variants five01,fast01 --out kline-vag.txt
```

## To stop early

Press Ctrl-C at any point. Every section is independent, output is flushed as
it goes, and the sections are ordered cheapest and safest first, so an early
abort still leaves the most useful data captured.

## What this closes

| Question | Section | Status |
|---|---|---|
| Received-message framing with real payload | `q0`, `q1` | closed on the Caddy (`PROTOCOL.md` §7) |
| K-line frame layout | `q2` (needs `--kline`; five init variants: EOBD 0x33 five-baud and fast, VAG 0x01 five-baud and fast, 0x10 five-baud; the init replies settle the `arw`/`ary` shapes too) | open: no K-line responder yet |
| Is `att`'s third argument TxFlags, and is the DLL's five-argument form accepted? | `q3` | closed (§4) |
| Chunking of a long reply | opt-in `--long-read ADDR`; not on the Audi (memory map unknown) | closed on the bench ECU: 70-byte chunks, the id on each (§7.6) |
| Transmit echo shape with LOOPBACK=1 on ISO15765 | `q9` | closed on the bench ECU: echoes on channel 5 (§7.7) |
| Raw CAN frame format and what traffic reaches the port; does SNIFF_MODE open | `q10` (receive only) | closed (§7.8, §10) |
| Which configuration ids the firmware knows, and their defaults | `q11` | closed (§8) |
| The driver's own K-line path end to end (init ioctl, write, read) | `car-session.sh` step 7, `examples/op_kline` | open: no K-line responder yet |
| VW TP2.0 over raw CAN, and the EDC16 flash counters | `--tp20`; `--allow-diag-session` if the ECU wants `10 89` first | closed on the 2009 Audi's engine module (2026-09-13, below); also covered by the `s_tp20` scenarios |
| `atx` (`atm` and `atw` are known: periodic message and five-baud init) | `q5` (bench only, `--unknown-verbs`) | open, unimportant |
| Two-digit protocol numbers, and the DLL's five-argument `ato` | `q6` | closed (§5, §12) |
| Pin 16 reads battery voltage | `q7` | closed (§8) |
| How the firmware reports a silent bus | `q8` | closed (§6) |
| Differential against a reference driver, live | `car-session.sh` step 5 (`OLD_DRIVER=...`, macOS) | not run live; the live comparisons were made against the vendor DLL (`AB-OFFICIAL.md`) |

## Reading the results

```bash
python3 tools/car/analyse_capture.py car-session-*/4-protocol-capture.txt car-session-*/4b-kline-capture.txt
```

It answers the open questions directly rather than leaving hex to interpret.
The one that decides driver code: from the raw K-line frame bodies it decides
between the uniform layout (a timestamp on every frame, the driver's original
guess) and the asymmetric one (a timestamp only on start and end frames, which
the published implementations describe and the driver now implements). It
names the layout, says whether `op_frame_has_timestamp()` must change, and
lists the init replies, which settle the `arw`/`ary` reply shapes. This
resolves only on a vehicle whose pin 7 has a responder; on the Audi the capture
recorded silence.

## Reading the flash counters

The EDC16 keeps its two OBD programming counters in EEPROM, and VCDS shows them
under Advanced ID as "Flash Status: Programming Attempts / Successful Attempts".
On VW KWP2000 modules that screen comes from ReadEcuIdentification with
identification option 0x9C, the flash-status record: a read, and the same
request `pq-flasher` uses (`ECU_IDENTIFICATION_TYPE.STATUS_FLASH = 0x9C`). The
K-line VAG variants of `q2` send `1A 9C` after `1A 9B`/`91`, and the analyser
prints the record's bytes as 8-bit and 16-bit numbers. The owner knows the true
values (more failed attempts than successes), so the numbers identify
themselves, and a correct read shows that the whole K-line path (init, framing
and reassembly) is right on this ECU.

If the ECU answers `7F 1A 80` (not allowed in this session), VCDS's own
diagnostic session is needed first. That is `10 89`, a session change and so
refused by default. `--allow-diag-session` unlocks exactly that one
subfunction for the run; the programming session `10 85`, the only thing that
moves the counters, stays refused whatever flags are given. The choice to use
it is made at the car, knowing that VCDS sends `10 89` on every connection.

## Session of 2026-09-13 on the Audi

The capture session's requests got no answer on either bus. Every request it
sent was ISO-TP addressed to 0x7E0 or 0x7DF, unpadded at the time (`PROTOCOL.md`
§11, "Pad the request"). A VAG module of that era is reached for manufacturer
diagnostics with VW TP2.0, which opens a channel on CAN id 0x200 and lets the
module name the pair of ids to use. The bus was live throughout: the frames
were acknowledged. The raw-CAN listen was empty, which fits VAG's diagnostic
CAN staying quiet until a tester opens a channel. A later A/B replay on the
same car's raw CAN did get the engine's reply to a legislated `$01 00` on
0x7E8, identically through both drivers (`AB-OFFICIAL.md`).

On K-line, every standard wake-up returned `are 7`. Later attempts through
both drivers, including the retry sequence of an open-source EDC16 K-line
flasher, got no answer at any of eight VAG addresses. The line is electrically
present, so either pin 7 is not on a live K-line on this car or the cable's
five-baud waveform is not what this ECU accepts (`AB-OFFICIAL.md`). Nothing here
implicates the driver's K-line code; it could not be exercised on this car.

Two driver bugs came out of the session:

- Five-baud init used the wrong verb. `aty` is fast init; the five-baud verb
  is `atw`, and the difference is unmistakable on the wire: `atw` blocks about
  2450 ms, one byte clocked out at 5 baud, while `aty` returns in about 108 ms.
  Every `FIVE_BAUD_INIT` call had been performing a fast init.
- The `0x10` status is the normal transmit indication on every transmit, not
  the rarity the protocol notes described. A driver that reads it as data
  reports a phantom message on every write.

The receive path was not settled on the Audi. It was settled later the same day
on the 2012 VW Caddy, through this driver's own entry points (`PROTOCOL.md`
§7.3, §11), and the Caddy's VIN reply is now a golden trace.

`--tp20` opens a TP2.0 channel over the raw CAN channel and runs the
identification reads inside it, including the flash-status record that carries
the programming counters, and the `s_tp20` scenarios cover it against a
simulated module. When it was first built the ignition had timed out, and the
pre-flight refused to transmit onto a bus it could not confirm was powered;
that refusal was the first live test of the safety checks. Later the same day,
with the ignition on, it opened a channel to the engine module (0x01), agreed
the timing parameters, and got answers to five identification reads, the
flash-status record `1A 9C` among them, and to TesterPresent. That capture is
kept with the owner's vehicle records, not in this repository.

To repeat it, with the ignition on:

```bash
python3 tools/car/car_capture.py --tp20 --out tp20.txt
```

```bash
python3 tools/car/car_capture.py --tp20 --allow-diag-session --out tp20-session.txt
```

## Afterwards

Attach the session archive to an issue (see `CONTRIBUTING.md`; remove the VIN
and the cable serial first). `PROTOCOL.md` §12 then gets folded down, the
simulator's `MODELLED` behaviours get replaced with measured ones, and the
captured frames become golden-trace regression fixtures, so the session's value
persists beyond the session.
