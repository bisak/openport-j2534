# Recording a vehicle session

`tools/car/car-session.sh` runs one read-only session on a vehicle and archives
everything the cable saw. Recordings like these become the test suite's golden
traces (`tests/unit/test_golden.c`), which is how the driver's receive path was
settled.

**What is still wanted is a vehicle that answers on K-line**, or a bench OBD
simulator that speaks ISO 9141-2 or KWP2000. K-line is implemented but has never
been confirmed on a car ([PROTOCOL.md §12](PROTOCOL.md)). If you have one, a
session takes about fifteen minutes, and the archive attached to an
[issue](https://github.com/bisak/openport-j2534/issues) settles it.

## Running a session

Before you start:

- ignition on, engine off;
- the cable's microSD card removed;
- nothing else plugged into the OBD port;
- a healthy battery, or a charger on a doubtful one. Fifteen minutes of ignition
  is fine; an hour is not.

Then, from the repository root:

```bash
tools/car/car-session.sh
```

It rebuilds, reads the USB descriptors, runs `op_smoke` over USB and over the
serial device, runs every capture section, tries the K-line wake-ups, runs a
hex-logged `op_smoke --tx` and the driver-level K-line check `op_kline`, and
prints its analysis. With `OLD_DRIVER` set (macOS), it also runs the
[differential](DIFFERENTIAL.md) against the old driver.

Everything lands in `car-session-<timestamp>/` and a `.tgz` of it. An output
directory can be given as the first argument. `OPENPORT_DEV` names the cable's
serial device if it is not the first `/dev/cu.usbmodem*` or `/dev/ttyACM*`.

To stop early, press Ctrl-C. Output is flushed as it goes and the sections run
cheapest and safest first, so an early stop still keeps the most useful data.

**Afterwards**, attach the archive to an issue. It can contain the vehicle's
VIN and contains the cable's serial number (`1-descriptors.txt`); remove both
first.

## What it sends, and why it is safe

The capture tool, `tools/car/car_capture.py`, talks to the cable's serial device
directly, not through `libj2534`. These are checks in its code, not intentions.

1. **CAN ids 0x7DF–0x7FF only**, the ISO 15765-4 diagnostic range, enforced by
   `guard_tx()` on every transmit. Real powertrain and chassis broadcasts live
   below 0x7DF, and a frame injected with one of those ids could be read by a
   module as genuine vehicle data. With `--tp20` it also allows 0x200 (TP2.0
   channel setup) and the one id the module returns.
2. **A battery check first.** It reads pin 16 and transmits nothing unless it
   reads at least 11 V; on a bench it stops before sending a single frame. The
   check is skipped with `--no-preflight` and when `--only` runs one section.
   `car-session.sh` skips it only for its K-line step (4b), which follows the
   main capture's check but runs even if that check stopped the capture.
3. **Read-only services only**, enforced by `guard_service()` on CAN and K-line
   alike. This matters on K-line in particular: many ECUs are flashed over it,
   and a read request and an erase request travel the same way; only the
   service id tells them apart.
   - Allowed: OBD modes 01, 02, 03, 05, 06, 07, 09 and 0A; TesterPresent;
     ReadDataByIdentifier; KWP ReadEcuIdentification, ReadDataByLocalIdentifier
     and ReadDiagnosticTroubleCodesByStatus; KWP StartCommunication and
     StopCommunication; ReadMemoryByAddress only through `--long-read`.
   - Refused by name, with a reason: `0x04` clear DTCs, `0x08`, `0x10` session
     control, `0x11`, `0x14`, `0x27` security access, `0x28`, the writes
     `0x2E`/`0x3B`/`0x3D`, `0x2F`, `0x31` routine control, the transfer services
     `0x34`–`0x38`, `0x85` and `0x87`.
   - Anything else is refused by default.
4. **No programming voltage.** The tool never sends `atv`.
5. **It stops what it started.** On every exit, Ctrl-C included, it stops
   periodic messages, closes every channel it opened and resets the cable, so
   nothing keeps transmitting after the cable comes out.
6. **K-line is opt-in** (`--kline`), because it drives a different physical
   layer. It tries five wake-ups: five-baud and fast init to the EOBD address
   0x33, five-baud and fast init to the VAG engine address 0x01, and five-baud
   to 0x10, the Bosch default. Only StartCommunication and allowed reads follow.
   The init commands are the ones Tactrix's DLL sends
   ([PROTOCOL.md §4](PROTOCOL.md)).
7. **Commands of unknown meaning are bench-only.** Section q5 (`atx`, and the
   bare forms of `atm` and `atw`) runs only with `--unknown-verbs`: "read-only"
   cannot be claimed for a command nobody understands.
8. **Everything is recorded raw.** Each command's bytes, payload, latency to the
   first reply byte and the full reply go into the capture as `RAW` lines, so a
   wrong interpretation at the car costs nothing.
9. **Rehearsed.** The whole script has run end to end against the simulator,
   including the K-line variants and single requests, so a session at the car
   is not also a debugging session.

The examples `car-session.sh` runs are read-only too: `op_smoke --tx` sends one
TesterPresent, and `op_kline` sends StartCommunication, ReadEcuIdentification
and TesterPresent.

`$23 ReadMemoryByAddress` is not sent by default. It is a read, but the address
is in the ECU's own address space, and reading an unmapped address can raise a
bus fault inside the ECU. The long-read probe (`--long-read ADDR`, with
`--long-read-kline fast01` for K-line) needs an address the operator can vouch
for.

## Single requests and reading the results

For the operator: single requests to close anything a session left open. Each
is checked against the allow-list and, unless `--no-preflight` is given,
preceded by the battery check. On Linux add `--dev /dev/ttyACM0`; the default is
the first `/dev/cu.usbmodem*`.

```bash
# one CAN request
python3 tools/car/car_capture.py --request 090A --out live-1.txt
# functional address, if TesterPresent got no answer
python3 tools/car/car_capture.py --tx 0x7DF --out capture.txt
# the K-line section
python3 tools/car/car_capture.py --kline --out kline.txt
# some K-line variants
python3 tools/car/car_capture.py --kline --kline-variants five01,fast01 --out kline-vag.txt
# one K-line request
python3 tools/car/car_capture.py --kline-init fast01 --kline-request 1A9C --out live-2.txt
# one K-line wake-up, without the battery check
python3 tools/car/car_capture.py --kline --kline-variants five01 --only q2 --no-preflight --out live-4.txt
# VW TP2.0
python3 tools/car/car_capture.py --tp20 --out tp20.txt
# analyse them
python3 tools/car/analyse_capture.py live-*.txt
```

K-line init variants: `five33`, `fast33` (EOBD address 0x33), `five01`, `fast01`
(VAG engine address 0x01), `five10` (0x10).

Sections, in run order:

| Section | Checks | Status |
|---|---|---|
| q7 | pin voltages; pin 16 reads the battery | settled (§8) |
| q0, q1 | received-message framing with real payload | settled on the Caddy (§7) |
| q2 | K-line frame layout and init replies (needs `--kline`) | **open**: no K-line responder yet |
| q3 | `att`'s arguments, and the DLL's five-argument form | settled (§4) |
| q8 | how the firmware reports a silent bus | settled (§6) |
| q9 | the transmit echo with `LOOPBACK` on ISO 15765 | settled on the bench ECU: echoes on channel 5 (§7.7) |
| q10 | raw CAN frames and what reaches the port; `SNIFF_MODE` (receive only) | settled (§7.8, §10) |
| q11 | which configuration parameters the firmware knows, and their defaults | settled (§8) |
| q5 | `atx` (bench only, `--unknown-verbs`) | open, unimportant |
| q6 | two-digit protocol numbers, the five-argument `ato` | settled (§5, §12) |
| `--long-read ADDR` | chunking of a long reply | settled on the bench ECU: 70-byte chunks, the id on each (§7.6) |
| `--tp20` | VW TP2.0 on raw CAN, and the EDC16 flash counters | settled on the Audi's engine module (below) |
| step 7, `op_kline` | the driver's own K-line path end to end | **open**: no K-line responder yet |

Section numbers (§) refer to [PROTOCOL.md](PROTOCOL.md).

To read a session:

```bash
python3 tools/car/analyse_capture.py car-session-*/4-protocol-capture.txt car-session-*/4b-kline-capture.txt
```

It answers the open questions directly rather than leaving hex to interpret. The
one that decides driver code is the K-line frame layout: from the raw frame
bodies it decides between a timestamp on every frame (the driver's first guess)
and a timestamp only on start and end frames (what published implementations
describe, and what the driver now does). It names the layout, says whether
`op_frame_has_timestamp()` must change, and lists the init replies, which settle
the `arw`/`ary` reply shapes ([PROTOCOL.md §3](PROTOCOL.md)).

## The sessions so far

Two vehicles on 2026-09-13: a 2009 Audi 1.9 TDI and a 2012 VW Caddy.

**The Caddy** (ISO 15765 and raw CAN) settled the receive path, through this
driver's own functions ([PROTOCOL.md §7.3, §11](PROTOCOL.md)). Its VIN reply is
now a golden trace.

**The Audi** was chosen first because it predates gateway authentication and
secure onboard communication, so nothing sent could trip a security counter or
leave a logged event. Its owner had read and written the ECU with a Galletto
1260, a K-line-only cable, so its K-line was expected to answer. A 2012 VW was
the second choice. A 2021 Toyota was ruled out: modern gateways may log or
reject unexpected diagnostic traffic, and there was nothing to gain.

What happened on the Audi:

- **ISO 15765 requests got no answer**, though the bus was live and every frame
  was acknowledged. The requests were unpadded at the time
  ([PROTOCOL.md §11](PROTOCOL.md), "Pad the request"), and VAG modules of that
  era are reached for manufacturer diagnostics with VW TP2.0 instead. The raw
  CAN listen was empty, which fits VAG's diagnostic CAN staying quiet until a
  tester opens a channel. A later A/B replay on raw CAN did get the engine's
  reply to a legislated `$01 00` on 0x7E8, identically through both drivers
  ([AB-OFFICIAL.md](AB-OFFICIAL.md#on-the-audi)).
- **The K-line answered no init.** Every standard wake-up returned `are 7`, and
  later attempts through both drivers, including an open-source EDC16 flasher's
  retry sequence, got no answer at any of eight VAG addresses. The line is
  electrically present, so either pin 7 is not on a live K-line on this car or
  the cable's five-baud waveform is not what this ECU accepts. The driver's
  K-line code could not be exercised.
- **TP2.0 worked.** `--tp20` opens a TP2.0 channel on the raw CAN channel and
  runs identification reads inside it. The first attempt came after the ignition
  had timed out, and the battery check refused to transmit onto a bus it could
  not confirm was powered: the first live test of the safety checks. With the
  ignition on, it opened a channel to the engine module (0x01), agreed the
  timing parameters, and got answers to TesterPresent and five identification
  reads, the flash-status record `1A 9C` among them. That capture stays with the
  owner's vehicle records. The `s_tp20` simulator scenarios cover the same path.

Two driver bugs came out of the Audi session:

- **Five-baud init used the wrong command.** `aty` is fast init; five-baud is
  `atw`. The difference is plain on the wire: `atw` blocks about 2,450 ms,
  clocking one byte out at 5 baud, while `aty` returns in about 108 ms. Every
  `FIVE_BAUD_INIT` had been performing a fast init.
- **The `0x10` frame is the transmit indication** that follows every transmit,
  not a rarity. A driver that reads it as data reports a phantom message on
  every write.

### The Audi's ECU

Researched on 2026-09-11, so that the session's limits followed from the
hardware.

- The EDC16C34 / EDC16U34 is a Freescale MPC562 PowerPC with external flash (the
  MPC562 has none on chip) and a serial EEPROM. The K-line is on ECU pin 72,
  wired to OBD pin 7.
- The ECU keeps two counters in EEPROM: OBD programming attempts and successful
  OBD programmings. They move only when a programming session is entered; bench
  and boot-mode reads do not touch them, and neither does diagnostics. Nothing
  the capture tool sends can start a programming session, so the counters stay
  where the Galletto left them.
- The EDC16 memory map is not established here (external flash is commonly
  reported at 0x800000; address 0 may be unmapped), which is why `$23` was not
  sent.
- The requests: modes `$01`/`$09` at 0x7DF are the legislated EOBD it must
  answer. `$22` and `$3E` at 0x7E0, and KWP `$1A`/`$3E`/`$81` on K-line, are
  what VCDS and any scan tool send. The worst case is a negative response. None
  writes EEPROM, sets a DTC or changes session state.
- A diesel with the ignition on and engine off draws a few amps (dash, pumps
  priming, glow control).

**Reading the flash counters.** VCDS shows them under Advanced ID as "Flash
Status: Programming Attempts / Successful Attempts". On VW KWP2000 modules that
screen comes from ReadEcuIdentification with option 0x9C, the flash-status
record: a read, and the same request `pq-flasher` uses
(`ECU_IDENTIFICATION_TYPE.STATUS_FLASH = 0x9C`). The K-line VAG variants of q2
send `1A 9C` after `1A 9B`/`91`, and the analyser prints the record's bytes as
8-bit and 16-bit numbers. The owner knows the true values, so a correct read
identifies itself.

If the ECU answers `7F 1A 80` (not allowed in this session), it wants the
diagnostic session VCDS opens on every connection, `10 89`. That is not the
programming session and does not touch the counters, but it is a session change,
so it stays refused unless `--allow-diag-session` is given and the owner agrees.
The programming session `10 85`, the only thing that moves the counters, stays
refused whatever flags are given.

```bash
python3 tools/car/car_capture.py --kline-init fast01 --kline-request 1A9C --allow-diag-session --out live-3.txt
python3 tools/car/car_capture.py --tp20 --allow-diag-session --out tp20-session.txt
```

Sources:
[Autotuner EDC16C34 (MPC562)](https://www.autotuner.com/pages/ecu/bosch-edc16c34-mpc562),
[MPC5xx family](https://en.wikipedia.org/wiki/MPC5xx),
[EDC16U34 pinout](https://ecu.design/ecu-pinout/pinout-bosch-edc16u34-xrom-mpc562-egpt-vag/),
[Flashtec: EDC16 VAG counters](https://www.flashtec.ch/Files/counters_eng.pdf),
[digital-kaos on the counters](https://www.digital-kaos.co.uk/forums/showthread.php/426090-vag-ecu-flash-counter),
[Galletto 1260, K-line only](https://www.astools.eu/products/galletto-1260-eobd-programmer-obd2-usb-interface-cable),
[MPC561/563 Reference Manual](https://www.nxp.com/docs/en/data-sheet/MPC561RM.pdf).
