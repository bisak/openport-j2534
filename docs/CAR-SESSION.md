# Record a vehicle session

`tools/car/car-session.sh` runs one read-only session on a vehicle and archives
everything the cable saw. Recordings like these become the test suite's golden
traces (`tests/unit/test_golden.c`).

**The project still needs a vehicle that answers on K-line**, or a bench OBD
simulator that speaks ISO 9141-2 or KWP2000. K-line is implemented but has never
been confirmed on a car ([PROTOCOL.md §12](PROTOCOL.md)). A session takes about
fifteen minutes. If you have such a vehicle, attach the archive to an
[issue](https://github.com/bisak/openport-j2534/issues).

## Run a session

Before you start, check these conditions:

- The ignition is on and the engine is off.
- The cable's microSD card is removed.
- Nothing else is plugged into the OBD port.
- The battery is healthy, or a charger is on a doubtful one. Fifteen minutes of
  ignition is fine. An hour is not. A diesel with the ignition on and the
  engine off draws a few amps (dash, pumps priming, glow control).

Then, from the repository root, run:

```bash
tools/car/car-session.sh
```

The script rebuilds, reads the USB descriptors, and runs `op_smoke` over USB and
over the serial device. It then runs every capture section, tries the K-line
wake-ups, runs a hex-logged `op_smoke --tx` and the driver-level K-line check
`op_kline`, and prints its analysis. With `OLD_DRIVER` set (macOS), it also runs
the [differential](DIFFERENTIAL.md) against the old driver.

Everything goes into `car-session-<timestamp>/` and a `.tgz` of it. To use
another output directory, give it as the first argument. If the cable's serial
device is not the first `/dev/cu.usbmodem*` or `/dev/ttyACM*`, name it in
`OPENPORT_DEV`.

To stop early, press Ctrl-C. Output is flushed as it goes, and the sections run
cheapest and safest first, so an early stop keeps the most useful data.

The archive can contain the vehicle's VIN, and it contains the cable's serial
number (`1-descriptors.txt`). Remove both, then attach the archive to an issue.

## Why the session is safe

The capture tool, `tools/car/car_capture.py`, talks to the cable's serial device
directly, not through `libj2534`. Each item below is a check in its code.

1. **CAN ids 0x7DF to 0x7FF only.** This is the ISO 15765-4 diagnostic range,
   and `guard_tx()` enforces it on every transmit. Real powertrain and chassis
   broadcasts use ids below 0x7DF, and a module could read a frame injected
   with one of those ids as genuine vehicle data. With `--tp20`, the tool also
   allows 0x200 (TP2.0 channel setup) and the one id the module returns.
2. **A battery check first.** The tool reads pin 16 and transmits nothing
   unless it reads at least 11 V. On a bench, it stops before it sends a single
   frame. `--no-preflight` skips the check, and so does `--only` when it runs
   one section. `car-session.sh` skips the check only for its K-line step (4b).
   That step follows the main capture's check, but runs even if that check
   stopped the capture.
3. **Read-only services only.** `guard_service()` enforces this on CAN and
   K-line alike. It matters most on K-line, because many ECUs are flashed over
   it, and a read request and an erase request travel the same way. Only the
   service id tells them apart.

   The tool allows these services:

   - OBD modes 01, 02, 03, 05, 06, 07, 09, and 0A
   - TesterPresent
   - ReadDataByIdentifier
   - KWP ReadEcuIdentification, ReadDataByLocalIdentifier, and
     ReadDiagnosticTroubleCodesByStatus
   - KWP StartCommunication and StopCommunication
   - ReadMemoryByAddress, only through `--long-read`

   The tool refuses these services by name, with a reason:

   - `0x04` clear DTCs
   - `0x08`
   - `0x10` session control
   - `0x11`
   - `0x14`
   - `0x27` security access
   - `0x28`
   - the writes `0x2E`, `0x3B`, and `0x3D`
   - `0x2F`
   - `0x31` routine control
   - the transfer services `0x34` to `0x38`
   - `0x85`
   - `0x87`

   The tool refuses any other service by default.
4. **No programming voltage.** The tool never sends `atv`.
5. **It stops what it started.** On every exit, Ctrl-C included, it stops
   periodic messages, closes every channel it opened, and resets the cable. No
   message keeps transmitting after the cable comes out.
6. **K-line is opt-in** (`--kline`), because it drives a different physical
   layer. The tool first sends one OBD mode 01 request with `LOOPBACK` on and
   checks that the echo comes back intact. A pin 7 held low would make every
   wake-up time out, as on an empty connector. Then it tries six wake-ups:
   five-baud and fast init to the EOBD address 0x33, five-baud and fast init to
   the VAG engine address 0x01, five-baud to 0x10 (the Bosch default), and
   five-baud to 0x01 again with `FIVE_BAUD_MOD` 3, which reports whatever key
   bytes arrive. Last, for an ECU slower than the firmware waits, it does a fast
   init by hand to 0x10 and to 0x01. That is the wake-up pulse alone
   (`aty4 0 0`), StartCommunication as a raw transmit, and a second's
   listening, up to 20 tries. It then does the same on ISO 9141 raw, where the
   tool frames the request itself. Only StartCommunication and allowed reads
   follow, after a VAG wake-up to 0x01 and then to 0x10, the EDC16's own
   KWP2000 address. The init commands are the ones Tactrix's DLL sends
   ([PROTOCOL.md §4](PROTOCOL.md)).
7. **Commands of unknown meaning are bench-only.** Section q5 (`atx`, and the
   bare forms of `atm` and `atw`) runs only with `--unknown-verbs`. A command
   that nobody understands cannot be called read-only.
8. **Everything is recorded raw.** The capture holds each command's bytes,
   payload, latency to the first reply byte, and full reply as `RAW` lines. A
   wrong interpretation at the car costs nothing.
9. **Rehearsed.** The whole script has run end to end against the simulator,
   including the K-line variants and single requests.

The examples that `car-session.sh` runs are read-only too. `op_smoke --tx` sends
one TesterPresent. `op_kline` sends StartCommunication, ReadEcuIdentification,
and TesterPresent.

On the Audi's ECU (EDC16C34, also listed as EDC16U34), modes `$01` and `$09`
at 0x7DF are the legislated EOBD that the ECU must answer. `$22` and `$3E` at 0x7E0, and KWP `$1A`, `$3E`, and `$81` on
K-line, are what VCDS and any scan tool send. The worst case is a negative
response. None of them writes EEPROM, sets a DTC, or changes session state.

The tool does not send `$23 ReadMemoryByAddress` by default. It is a read, but
the address is in the ECU's own address space, and a read of an unmapped
address can raise a bus fault inside the ECU. The memory map of the Audi's
EDC16 is not established here (external flash is commonly reported at 0x800000, and address
0 may be unmapped). The long-read probe (`--long-read ADDR`, with
`--long-read-kline fast01` for K-line) needs an address the operator can vouch
for.

## Send single requests

Use single requests to close anything a session left open. The tool checks each
request against the allow-list. Unless you give `--no-preflight`, the battery
check runs first. On Linux, add `--dev /dev/ttyACM0`. The default is the first
`/dev/cu.usbmodem*`.

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
# record pin 7 for 60 s while another tester talks, through an OBD Y-splitter;
# transmits nothing
python3 tools/car/car_capture.py --kline-listen 60 --out listen.txt
# analyse them
python3 tools/car/analyse_capture.py live-*.txt
```

The K-line init variants are:

- `five33` and `fast33`: EOBD address 0x33.
- `five01` and `fast01`: VAG engine address 0x01.
- `five10`: 0x10.
- `five01m3`: 0x01 with `FIVE_BAUD_MOD` 3.
- `manual10` and `manual01`: fast inits by hand (ISO 14230).
- `raw10` and `raw01`: fast inits by hand on ISO 9141 raw, with the framing and
  checksum built by the tool.

## Read the results

To analyse a session, run:

```bash
python3 tools/car/analyse_capture.py car-session-*/4-protocol-capture.txt car-session-*/4b-kline-capture.txt
```

The analyser answers the open questions directly. The one that decides driver
code is the K-line frame layout. From the raw frame bodies, the analyser
decides between a timestamp on every frame (the driver's first guess) and a
timestamp only on start and end frames (what published implementations
describe, and what the driver now does). It names the layout, says whether
`op_frame_has_timestamp()` must change, and lists the init replies. The init
replies settle the `arw` and `ary` reply shapes ([PROTOCOL.md §3](PROTOCOL.md)).

The sections, in run order, are below. Section numbers (§) refer to
[PROTOCOL.md](PROTOCOL.md).

| Section | Checks | Status |
|---|---|---|
| q7 | pin voltages, and that pin 16 reads the battery | settled (§8) |
| q0, q1 | received-message framing with real payload | settled on the Caddy (§7) |
| q2 | K-line echo, frame layout, and init replies (needs `--kline`) | echo settled on the bench (§7.9). Received frames **open**: no K-line responder yet |
| q3 | `att`'s arguments, and the DLL's five-argument form | settled (§4) |
| q8 | how the firmware reports a silent bus | settled (§6) |
| q9 | the transmit echo with `LOOPBACK` on ISO 15765 | settled on the bench ECU: echoes on channel 5 (§7.7) |
| q10 | raw CAN frames and what reaches the port, and `SNIFF_MODE` (receive only) | settled (§7.8, §10) |
| q11 | which configuration parameters the firmware knows, and their defaults | settled (§8) |
| q5 | `atx` (bench only, `--unknown-verbs`) | may restart the cable into its bootloader (PROTOCOL.md section 4) |
| q6 | two-digit protocol numbers, the five-argument `ato` | settled (§5, §12) |
| `--long-read ADDR` | chunking of a long reply | settled on the bench ECU: 70-byte chunks, the id on each (§7.6) |
| `--tp20` | VW TP2.0 on raw CAN, and the EDC16 flash counters | settled on the Audi's engine module (§11) |
| step 7, `op_kline` | the driver's own K-line path end to end | **open**: no K-line responder yet |

## Read the flash counters on the Audi's EDC16

This section comes from research on the Audi's ECU on 2026-09-11. The ECU keeps
two counters in EEPROM: OBD programming attempts and successful
OBD programmings. They move only when a programming session is entered. Bench
reads, boot-mode reads, and diagnostics do not touch them. Nothing the capture
tool sends can start a programming session.

VCDS shows the counters under Advanced ID as "Flash Status: Programming
Attempts / Successful Attempts". On VW KWP2000 modules, that screen comes from
ReadEcuIdentification with option 0x9C, the flash-status record. This is a
read, and the same request that `pq-flasher` uses
(`ECU_IDENTIFICATION_TYPE.STATUS_FLASH = 0x9C`). The K-line VAG variants of q2
send `1A 9C` after `1A 9B` and `1A 91`. The analyser prints the record's bytes as
8-bit and 16-bit numbers. If the owner knows the true values, a correct read
identifies itself.

The ECU can answer `7F 1A 80` (not allowed in this session). That means it
wants `10 89`, the diagnostic session that VCDS opens on every connection. That
session is not the programming session and does not touch the counters. It is
still a session change, so the tool refuses it unless you give
`--allow-diag-session` and the owner agrees. The tool always refuses the
programming session `10 85`, whatever flags you give. `10 85` is the only
request that moves the counters.

```bash
python3 tools/car/car_capture.py --kline-init fast01 --kline-request 1A9C --allow-diag-session --out live-3.txt
python3 tools/car/car_capture.py --tp20 --allow-diag-session --out tp20-session.txt
```

## Sessions so far

Two vehicles were recorded on 2026-09-13: a 2012 VW Caddy, which settled the
receive path, and a 2009 Audi 1.9 TDI (EDC16), whose K-line answered no init.
A second visit to the Audi on 2026-09-24 got no K-line answer either. The
results are in [PROTOCOL.md §11](PROTOCOL.md) and
[AB-OFFICIAL.md](AB-OFFICIAL.md#on-the-audi).
