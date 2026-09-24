# News

Release history, newest first. Each release is tagged `v<version>` at
https://github.com/bisak/openport-j2534/releases.

## 0.3.0 (unreleased)

### Fixed

- `CLEAR_MSG_FILTERS` could leave a filter installed and return success once a
  session had started more than 32 filters. The driver recorded filters in a
  32-bit mask at bit `id & 31`, but the firmware never reuses a filter id within
  a session: ids count up across all channels and restart only at `ata`/`atz`.
  Filter 32 shared filter 0's bit, so once filter 32 was stopped the clear sent
  nothing. Reproduced on the cable with 33 filter starts. `CLEAR_MSG_FILTERS`
  now sends `atk<ch> -1` and `CLEAR_PERIODIC_MSGS` sends `atl<ch>`, the single
  commands Tactrix's DLL sends (previously one `atk` per recorded filter and
  one `atn` per periodic message). On the cable both answer `aro`, after which
  the old ids answer `are 22` and `are 13`. The driver keeps no filter table and
  returns the device's answer.
- ISO15765 loopback echoes were dropped. With `LOOPBACK` on an ISO15765
  channel, the firmware reports the echoes on raw CAN channel 5: four zero bytes
  per CAN frame the cable sent, and each flow-control frame it sent with id and
  data intact. The driver discarded them because channel 5 was not open, so a
  tool that turns loopback on to watch its own requests saw nothing. While
  channel 5 is closed they are now delivered to the ISO15765 channel unchanged
  (`ProtocolID` CAN, `TX_MSG_TYPE`), as the vendor DLL delivers them. While
  channel 5 is open they stay on it; the DLL then moves every channel-5 frame to
  the ISO15765 channel, which this driver does not copy. Measured against the
  bench ECU of `docs/PROTOCOL.md` §10, 2026-09-24.
- `PassThruGetLastError` names `ERR_INIT_FAILED`; it read `ERR_UNKNOWN`.

### Changed: parity with Tactrix's DLL

Found with the A/B parameter sweep, 2026-09-24. In each case the previous
behaviour could fail an application written against the vendor DLL.

- `GET_CONFIG` and `SET_CONFIG` process every parameter in the list and return
  the last one's status, as the DLL does (`[3, 127]` gives
  `ERR_NOT_SUPPORTED`, `[127, 3]` gives 0). The driver used to stop at the first
  unsupported parameter, leave the rest unset and fail the list. Earlier
  failures go to the log.
- Periodic intervals outside J2534's 5–65535 ms are forwarded; the firmware
  runs them (1 ms held). Only an interval the firmware's microsecond field
  cannot hold (above 4 294 967 ms) is refused.
- A flow-control message given with a PASS or BLOCK filter is ignored instead
  of refused with `ERR_INVALID_MSG`; the `atf` flags come from the mask.

### Changed

- `PassThruStartPeriodicMsg` applies the message-size check `PassThruWriteMsgs`
  applies. The firmware refuses a short `att` itself but repeats a 3-byte `atm`,
  a CAN id it cannot form, every interval.

### Build

- An object rebuilds when a header it includes changes (`-MMD -MP` dependency
  files; `make clean` removes them). Editing a struct in `op_device.h` could
  otherwise link old and new layouts into one library.

### Tooling

- A/B harness: `AB_ARGS="-- sweep"` passes every connect flag and rate, every
  configuration parameter on four channels, filter shapes and flags, transmit
  flags and sizes, and periodic limits once through both drivers.
  `AB_ARGS="-- edge"` does the same for values J2534 forbids and runs only on
  request. A marker call before each step lets `ab_diff.py` compare every
  step's wire commands (report section 2c); it also compares channel digits,
  reports `GET_CONFIG` values, and counts channel, filter and periodic ids as
  handles rather than divergences.
- `ptytap.py` no longer dies on a transmit larger than a pty buffer: it queues
  bytes per direction and writes them as each side accepts them. Short writes
  used to drop bytes.
- The simulator reproduces the measured filter and periodic id numbering, the
  ten-filter limit, filter types 0 and 4 (`are 22`), 70-byte chunking of long
  replies, and the ISO15765 loopback echo.

### Measured and documented

- Long ISO15765 replies arrive in 70-byte chunks with the CAN id on each
  (`docs/PROTOCOL.md` §7.6): 6 162 segmented replies of a 512 KB read against
  the bench ECU, 2026-09-24.
- The ISO15765 loopback echo arrives on channel 5 (§7.7).
- Filter ids count up across channels and restart at `ata`/`atz`; periodic ids
  count up across channels and sessions while the cable is powered; the
  eleventh live filter on a channel answers `are 12`; filter types 0 and 4
  answer `are 22`. Bench cable, 2026-09-24.
- The vendor DLL crashes the calling process on a NULL input to `GET_CONFIG`,
  `SET_CONFIG` (read fault at address 0) and `FAST_INIT` (write fault at
  0x11). This driver returns `ERR_NULL_PARAMETER` for the first two and sends
  the wake-up-only fast init for the third.
- Sweep results against the bench cable, 2026-09-24: 648 of 670 steps identical
  on the wire, 23 of 35 `edge` steps; every other step follows from a
  difference listed in `docs/AB-OFFICIAL.md`.
- Documentation reconciled with the code; this file added.

## 0.2.1 (2026-09-22)

Builds and runs on Linux. The README had claimed Linux support since 0.1.0;
this is the first release where it was exercised end to end.

### Fixed

- The tests and the mock transport slept with `usleep`, which the POSIX.1-2008
  profile the build selects does not declare, so GCC refused them. They use
  `nanosleep` now. The library itself was unaffected.
- The Makefile passed the Darwin-only `-arch` flag to the sanitizer and fuzz
  targets on every platform. `diff-tools`, whose `usbtap` is a
  `DYLD_INSERT_LIBRARIES` interposer, now refuses to build off macOS with a
  message instead of a compiler error.
- Closing the USB transport on Linux left the cable without its `cdc_acm`
  driver until it was replugged: detaching the data interface to claim it tears
  the whole ACM device down, `/dev/ttyACM*` included. Close now reattaches every
  interface the kernel had at open, comm interface first, which brings the tty
  back. No change on macOS, where nothing is detached.
- `tools/car/car-session.sh` finds `/dev/ttyACM*`, exports `LD_LIBRARY_PATH`,
  builds `diff-tools` on macOS only, and waits for the ACM node to come back
  after the USB step.

Verified in an Ubuntu 24.04 container (GCC 13.3, aarch64): `-Werror` build, 517
unit checks plain and under ASAN/UBSAN and TSAN, 67 simulator checks, 300 000
fuzz iterations, examples and install. The macOS suite is unchanged.

Reported by Michał Słomkowski.

## 0.2.0 (2026-09-22)

Measured against Tactrix's `op20pt32.dll` 1.02.0.4868 driven through the same
scripted sequence as this driver: the periodic commands are wire-identical, and
every return code agrees except three deliberate cases where the standard's text
is followed instead. `docs/PROTOCOL.md` is rewritten as a current-truth
reference; its §13 lists every reading the cable refuted.

### Changed

- Periodic messages are scheduled by the firmware (`atm`/`atn`), the commands
  Tactrix's DLL sends; the host scheduler thread is gone. Timing is the
  firmware's (100.0 ms at 100 ms), message ids are the firmware's, and no
  transmit-done indication is produced for them. As with the vendor driver, a
  periodic outlives a crashed application until the next `PassThruOpen`, which
  resets the cable.
- TxDone indications carry the CAN id of the message just sent: `DataSize` 4,
  `ExtraDataIndex` 0 (J2534-1 DEC2004 §8.6), as the vendor DLL reports them.
  0.1.x reported `DataSize` 0.
- `SNIFF_MODE` is passed through to the firmware instead of refused, so
  Tactrix's own `canlogger` sample connects. The firmware still acknowledges
  frames (`docs/PROTOCOL.md` §10).
- The `OPENPORT_ENABLE_PROG_VOLTAGE` gate is removed.
  `PassThruSetProgrammingVoltage` applies what it is asked, as with the vendor
  driver. A voltage on a second pin while one holds a voltage is refused with
  `ERR_PIN_INVALID` (was `ERR_EXCEEDED_LIMIT`), the code J2534-1 Figure 22
  names; grounding K or L under a channel that uses it is still refused.
- Any call before `PassThruOpen` or after `PassThruClose` returns
  `ERR_INVALID_DEVICE_ID` (J2534-1 §7.2.1); 0.1.x returned
  `ERR_DEVICE_NOT_CONNECTED`.
- ISO15765 transmit size limits follow J2534-1 Figure 42 (minimum 4 bytes, 5
  with extended addressing).

### Fixed

- `PassThruClose` on one thread while `PassThruReadMsgs` waits on another: the
  read returns `ERR_INVALID_DEVICE_ID` at once instead of spinning out its
  timeout over destroyed synchronisation objects.
- A `WriteMsgs` Timeout above 71 minutes wrapped the firmware's microsecond
  transmit budget; it saturates now.
- `GET_CONFIG`/`SET_CONFIG` errors from the device set the
  `PassThruGetLastError` text.
- Open sends the vendor's `\r\n\r\n` preamble, which terminates a partial line
  left by a crashed session, and requires `ata` to answer `aro`.
- `tests/differential/diff_runner.c` and `examples/op_smoke` send `VOLTAGE_OFF`
  rather than 17 V, which the gate had been refusing on their behalf.

### Protocol

- Continuation chunks of a reply longer than one wire frame have the repeated
  CAN id stripped unconditionally, as the vendor DLL does; it reassembles only
  that layout intact. No change under the measured layout; the heuristic that
  decided per chunk is gone.
- `docs/PROTOCOL.md` rewritten: current truth per section with confidence
  markers, the vendor DLL's command set, §12 open questions and what closes
  each, §13 refuted readings.
- The simulator models firmware periodics; the capture tool's `atp` sweep is
  removed (`atp` is a pin verb, not the periodic command).

Verified: 517 unit checks under plain, ASAN/UBSAN and TSAN builds, 300 000 fuzz
iterations, 67 simulator scenario checks, and the vendor-DLL A/B against the
simulator.

## 0.1.2 (2026-09-16)

Measured on firmware 1.17.4877 on a bench supply, through Tactrix's DLL replayed
against the cable, and against a production CAN ECU on a bench harness.
`docs/PROTOCOL.md` §5, §8 and §11 carry the evidence.

### Fixed

- The receive queue lost data under sustained traffic. A raw CAN channel left
  unread for 30 s at 50 frames/s kept 64 messages and dropped 1 438 (reported
  as `ERR_BUFFER_OVERFLOW`); Tactrix's DLL kept them all. The queue is now a
  1 MiB ring per channel that stores each message at its own size (about 29 000
  raw CAN frames). Re-measured: 1 503 frames over 30 s, no gap.
  Request/response diagnostics were never affected.
- `PassThruConnect(SCI_A_ENGINE)` opened ISO9141 on the L line and reported
  success. Firmware channels 7–9 are not SCI. SCI, J1850 and unknown protocol
  ids are now refused with `ERR_INVALID_PROTOCOL_ID` and nothing sent, as
  Tactrix's DLL refuses them.
- Periodic messages: 10 per channel (J2534-1's minimum and the firmware's
  limit); was 8 in total.
- `READ_PROG_VOLTAGE` reads the pin `pInput` points to, as Tactrix's DLL does
  (8, 12, 16, or 17 for the adjustable supply); pin 12 when `pInput` is NULL.

### Added

- J2534-2 channels: `ISO9141_L` / `ISO14230_L` (L line, firmware channels 7
  and 8) and `ISO9141_INNO` (RS-232 receive on the 2.5 mm jack, channel 9),
  plus the `_CH1` aliases of the J2534-1 protocols. Opened exactly as Tactrix's
  DLL opens them; no L-line or Innovate data measured yet.
- `TX_PARAM_STOP_BITS` (0x9000) defined; the firmware honours it on channels 3
  and 9.

### Changed

- `SNIFF_MODE` is refused with `ERR_NOT_SUPPORTED`. The firmware accepts it
  and still acknowledges: a sniffing channel alone with an ECU transmitted and
  was acknowledged, including with Tactrix's own `SNIFF_MODE | CAN_ID_BOTH`.
  (Reversed in 0.2.0.)
- Programming voltage rules, from measurement:
  - the cable accepts 5000–20000 mV, not 5–25 V;
  - all voltage pins share one supply, so a voltage on a second pin while
    another holds one returns `ERR_EXCEEDED_LIMIT` (`ERR_PIN_INVALID` since
    0.2.0);
  - `SHORT_TO_GROUND` is refused on K (pin 7) under an open K-line channel and
    on L (pin 15) under an L-line channel, and such a channel will not open on
    a pin this session grounded;
  - `VOLTAGE_OFF` and `SHORT_TO_GROUND` go on the wire signed (`-1`/`-2`), as
    Tactrix sends them.
- Periodic messages stay host-scheduled: a firmware periodic kept transmitting
  after the application was killed. (Reversed in 0.2.0.)

### Protocol documentation

- Pin table corrected: pin 12 is an ADC input and the 2.5 mm jack's tip (a plug
  disconnects it from the vehicle), pin 17 is the adjustable supply. Pin 0
  drives pin 12 while no plug is inserted.
- `ata` and `atz` switch every voltage output off and stop firmware periodics.
- The A/B harness rebuilds a stale Docker image and no longer hangs after a
  run.

## 0.1.1 (2026-09-13)

### Fixed

- Five-baud init (`PassThruIoctl FIVE_BAUD_INIT`): the firmware answers `atw`
  with `arw<ch> <b> <b>`, the key bytes in decimal on the line. The driver
  expected `ary` plus raw bytes and would have reported a real success as
  `ERR_INIT_FAILED`. Both shapes are now accepted. Third-party evidence (HDS on
  a 2005 Honda over ISO9141); not yet confirmed on this project's own K-line
  vehicle.

### Protocol corrections, measured on the cable (firmware 1.17.4877)

- `ata` closes every open channel; it was documented as "attention".
- `atp` is a pin verb sharing `atv`'s shape, not a periodic message.
- The fourth `ato` argument is ignored. There is no three-channel cap;
  ISO9141/ISO14230, and the channels then read as SCI A engine/trans (7 and 8,
  identified as the L line in 0.1.2), are mutually exclusive (`are 3`).
- Baud rate is not validated on open.
- Pin 8 answers `atr`.
- Retracted: "no prior driver handled 0x10". The `dschultzca` lineage does.

`docs/PROTOCOL.md` gains a table of third-party claims tested on the bench,
with each refuted claim and its source. The simulator, mock transport, fuzz
corpus and car capture tool follow the same corrections.

## 0.1.0 (2026-09-13)

First public release: an SAE J2534-1 PassThru driver for the Tactrix OpenPort
2.0 on macOS (Apple Silicon and Intel) and Linux.

- ISO 15765, raw CAN, ISO 9141 / ISO 14230 K-line.
- User-space libusb: no kernel extensions, no `sudo`, no SIP changes.
- Hardware-free test suite: unit tests, golden vehicle traces, fuzzing,
  sanitizers, protocol simulator.
- Compared byte for byte on the wire against Tactrix's Windows driver.

Install: `brew install bisak/tap/openport-j2534`.
