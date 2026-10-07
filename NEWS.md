# Release notes

## Unreleased

Fixes

- A cable that disappears mid-session now fails every later call at once with `ERR_DEVICE_NOT_CONNECTED` instead of running each one to its timeout. A transport that fails every read for half a second counts as a lost cable, and the reader thread no longer spins on it.
- `PassThruClose` on one thread while a command was queued behind it on another could free the transport under that command. The teardown now waits for the command, which returns a closed-device code.
- `CLEAR_TX_BUFFER` emptied the receive queue. There is no transmit queue to clear; it now leaves received messages alone.
- A five-baud init whose `arw` reply carried no sequence number timed out. Both forms are accepted, and an init reply is matched to the command by kind: a late `ary` or `arw` can no longer be taken as the answer to whatever command came next.
- With `LOOPBACK` on a K-line channel, the echo was preceded by an empty `TX_DONE` indication. It is now a `START_OF_MESSAGE` indication, as Tactrix's driver delivers it, followed by the echoed message marked `TX_MSG_TYPE`.
- Frames on the L-line and jack channels (7-9) were laid out as CAN frames. They follow the K-line layout, as channels 3 and 4 do.
- A reply with more than three trailing numbers read past the parser's token array.
- `PassThruGetLastError` had no text after a failed `READ_VBATT` or `READ_PROG_VOLTAGE`, and lost the specific reason (a size range, a dead K-line) behind the entry point's generic one. The cable's `ERR_OEM_VOLTAGE_*` codes are named; a code with no name is reported by number.
- A `PassThruConnect` that ran out of memory after the cable had opened the channel left it open on the cable.
- The serial transport could wait forever on a timeout above 2^31 ms, and a zero-length USB packet was reported as data.

Changes

- Return codes the standard names: a message whose `ProtocolID` is not the channel's protocol is refused with `ERR_MSG_PROTOCOL_ID`, for transmits, periodic messages and a filter's flow-control message, as the vendor driver checks them, while a mask or pattern's `ProtocolID` is ignored, as it ignores it (Tactrix's driver reroutes a mismatched transmit by protocol); connecting a protocol whose line another open channel holds (ISO 9141 and ISO 14230 on K, their twins on L) returns `ERR_CHANNEL_IN_USE` without asking the cable, which answers `are 3`; a periodic message with an interval of 0, which the cable accepts and never sends, returns `ERR_INVALID_TIME_INTERVAL`; `FIVE_BAUD_INIT` without an output array returns `ERR_NULL_PARAMETER` before anything reaches the bus.
- `PassThruClose` returns the reset's failure code when `atz` did not reach the cable, since a pin may still carry voltage. The session is torn down either way.
- `PassThruReadMsgs` waits once per message for the time that is left instead of in 20 ms slices.
- Read from Tactrix's driver code and applied here: ISO 15765 extended addressing is marked (`ISO15765_ADDR_TYPE`, status bit `0x04`, a 5-byte header on every chunk); on the K-line channels only a START or END frame of exactly four bytes carries a timestamp, and a `0x10` frame there is data; a `Timeout` of 0 gives a raw CAN frame a 50 ms bus budget and a jack transmit none; a stale numbered `ary` or `arw` is discarded. A frame with status bit `0x08`, which the vendor driver would drop and whose meaning is unmeasured, is delivered and logged.
- `PassThruOpen` after a lost cable closes the dead session itself, so an application need not call `PassThruClose` first; `PassThruReadVersion` reports the loss like every other call.
- Protocol notes: `atx` and `atp 5 2` are the vendor driver's firmware-update entry; neither is ever sent by this driver. A cable in its bootloader (`0403:cc4b`) or enumerated as storage with a microSD card in (`0403:cc4c`) is named as such by `PassThruGetLastError` instead of "no OpenPort found".

## 0.3.2 (2026-10-03)

The library is unchanged apart from its version number.

- `tools/car/car_capture.py` checks the cable's K-line before any wake-up by sending one request with `LOOPBACK` on and comparing the echo, adds a `FIVE_BAUD_MOD` 3 wake-up, a fast init by hand that listens a full second for a slow ECU, and reads at the EDC16's own address 0x10, and can record pin 7 passively with `--kline-listen`.
- Protocol notes: the K-line echo layout and the checksum on the wire, measured on the bench; the Audi K-line notes corrected.
- The test simulator's serve loop no longer spins at 100% CPU while idle (it listed the pty master as a write fd to `select()`); a scenario now guards it.

## 0.3.1 (2026-09-24)

Documentation only. The library is unchanged apart from its version number.

- Documentation rewritten for readability: a shorter README, a new guide to the J2534 API on this cable (`docs/API.md`), and the protocol, testing and comparison notes reorganised with a summary first.

## 0.3.0 (2026-09-24)

Fixes

- `CLEAR_MSG_FILTERS` could leave a filter active and still report success once more than 32 filters had been set in one session. It now clears every filter on the channel with one command, as Tactrix's driver does, and `CLEAR_PERIODIC_MSGS` does the same for periodic messages.
- With `LOOPBACK` on an ISO 15765 channel, the echoes of your own transmissions were dropped. They are now delivered, as Tactrix's driver delivers them.
- `PassThruGetLastError` names `ERR_INIT_FAILED` instead of calling it `ERR_UNKNOWN`.

Changes, to match Tactrix's driver

- `GET_CONFIG` and `SET_CONFIG` process the whole parameter list even when one parameter is unsupported, and return the status of the last one. Previously the list stopped at the first unsupported parameter and the rest were never set.
- Periodic messages accept any interval the cable can run, not only J2534's 5–65535 ms.
- A flow-control message passed with a PASS or BLOCK filter is ignored instead of rejected.

Other changes

- `PassThruStartPeriodicMsg` checks the message size the way `PassThruWriteMsgs` does. The cable would otherwise repeat a malformed frame.
- Objects are rebuilt when a header they include changes.
- Documentation reconciled with the code, and the release notes kept in the tree as `NEWS.md`. The protocol notes now describe how long replies are chunked and how ISO 15765 loopback echoes arrive, both measured against a live ECU.
- The A/B test harness can sweep every J2534 parameter through Tactrix's driver and this one and compare what each sends, command by command.

## 0.2.1 (2026-09-22)

Builds and runs on Linux.

- Closing the driver on Linux no longer leaves `/dev/ttyACM*` missing until the cable is re-plugged.
- The test suite builds with GCC. It used `usleep`, which the POSIX profile the build selects does not declare.
- The Makefile no longer passes the macOS-only `-arch` flag on other platforms.
- `tools/car/car-session.sh` works on Linux.

Verified on Ubuntu 24.04 with GCC 13.3. Reported by Michał Słomkowski.

## 0.2.0 (2026-09-22)

Measured against Tactrix's own driver: the commands on the wire now match, and every return code agrees except three where the J2534 standard is followed instead.

- Periodic messages are scheduled by the cable, as Tactrix's driver does, instead of by a thread in the driver. Like Tactrix's, they keep running if your process dies, until the next `PassThruOpen`.
- TxDone indications carry the CAN id of the message sent, as J2534-1 specifies and Tactrix's driver does.
- `SNIFF_MODE` is passed to the cable instead of refused, so Tactrix's own `canlogger` sample connects. The cable still acknowledges frames in that mode.
- Programming voltage works whenever a tool asks for it; the `OPENPORT_ENABLE_PROG_VOLTAGE` switch is gone. A second voltage pin returns `ERR_PIN_INVALID`.
- A call before `PassThruOpen` or after `PassThruClose` returns `ERR_INVALID_DEVICE_ID`.
- ISO 15765 message size limits follow J2534-1.
- Long replies are reassembled the way Tactrix's driver reassembles them.
- `PassThruOpen` recovers a cable left mid-command by a crashed session.
- Fixed: `PassThruClose` from one thread while another waits in `PassThruReadMsgs` now returns at once.
- Fixed: a `PassThruWriteMsgs` timeout above 71 minutes wrapped around.
- `docs/PROTOCOL.md` rewritten as a reference.

## 0.1.2 (2026-09-16)

- The receive queue holds about 29,000 raw CAN frames per channel instead of 64 messages, so a busy bus read slowly no longer loses data.
- SCI and J1850, which the cable does not have, are refused instead of opening the wrong channel.
- 10 periodic messages per channel; was 8 in total.
- `READ_PROG_VOLTAGE` reads the pin the caller names, as Tactrix's driver does.
- New J2534-2 channels: the L line (`ISO9141_L`, `ISO14230_L`) and the RS-232 input on the 2.5 mm jack (`ISO9141_INNO`).
- Programming voltage: 5–20 V, one pin at a time, and the K and L lines cannot be grounded while in use.
- `SNIFF_MODE` is refused, because the cable still acknowledges frames in that mode. Reversed in 0.2.0.

## 0.1.1 (2026-09-13)

- Fixed: a successful five-baud init would have been reported as a failure, because the cable's reply format was misread.
- Protocol notes corrected in several places after measuring the cable.

## 0.1.0 (2026-09-13)

First release: J2534 for the Tactrix OpenPort 2.0 on macOS and Linux, with ISO 15765, raw CAN and K-line, no kernel extension or `sudo`, and a test suite that needs no hardware.
