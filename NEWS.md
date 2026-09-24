# Release notes

## Unreleased

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
