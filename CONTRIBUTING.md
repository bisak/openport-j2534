# Contributing

## What helps most

**Data from a vehicle this driver has not seen**, above all one that answers on
K-line: K-line is implemented but has never been confirmed on a car.
`tools/car/car-session.sh` runs a read-only session and archives everything;
[docs/CAR-SESSION.md](docs/CAR-SESSION.md) explains what it sends and why each
request is safe. Attach the archive to an
[issue](https://github.com/bisak/openport-j2534/issues). It can contain the
vehicle's VIN and contains the cable's serial number
(`1-descriptors.txt`); remove both before posting.

**Bug reports.** Include the output of your tool run with
`OPENPORT_LOG=- OPENPORT_LOG_HEX=1`, and the firmware version
`examples/op_smoke` prints. A hang is always a bug: every transfer has a time
limit.

**Support for another cable.** The transport interface is
`src/op_transport.h`; a new cable is a new transport, not a rewrite.

## Build and test

```bash
# the library and op_probe
make
# unit tests and vehicle recordings; no cable needed
make test
# plus sanitizers, fuzzing and the protocol simulator
make check-all
```

[docs/TESTING.md](docs/TESTING.md) describes each layer and the tests that need
hardware.

A change is ready when:

- `make test` passes with no cable attached;
- `make CFLAGS="-O2 -g -Werror"` builds cleanly on macOS and on Linux (a
  container is enough);
- anything measured on a cable or a vehicle is written down in
  [docs/PROTOCOL.md](docs/PROTOCOL.md), with the date, how it was measured and
  a confidence marker ([V], [P] or [U]);
- a user-visible change has a line in [NEWS.md](NEWS.md) under
  "Unreleased" (start that section if the newest one is a release).

## Ground rules

- **Behave like Tactrix's driver** unless there is a stated reason not to: a
  J2534 return code, or a request the cable would act on wrongly without the
  application being able to tell. Every difference is listed, with its reason,
  in [docs/API.md](docs/API.md) and [docs/AB-OFFICIAL.md](docs/AB-OFFICIAL.md).
- **Leave ISO-TP to the cable.** Its firmware does segmentation and flow
  control; the host does not.
- **Recordings win.** A golden trace in `tests/unit/test_golden.c` is a
  measurement. If a test disagrees with one, the code is wrong, not the trace.
- **Keep `tools/car/` read-only.** Nothing in it may write to an ECU, open a
  programming session or switch programming voltage on.
- **No identifying data in commits**: no capture logs, no VINs, no cable serial
  numbers.

## Code layout

| Path | What |
|---|---|
| `include/j2534/j2534.h` | The J2534 API, plus Tactrix's own constants |
| `src/op_j2534.c` | The fourteen `PassThru*` functions |
| `src/op_device.c` | Channels, the reader thread, reassembly, matching replies to commands |
| `src/op_proto.c` | Encoding commands and decoding replies; no I/O, fully unit-tested |
| `src/op_transport.h`, `op_usb.c`, `op_serial.c` | The byte pipe to the cable: libusb, or the serial device |
| `src/op_log.c`, `op_error.c` | `OPENPORT_LOG`, and the `PassThruGetLastError` text |
| `examples/` | `op_smoke`, `op_iso15765`, `op_kline`: end-to-end checks on a cable |
| `tests/unit/`, `tests/mock/` | Unit tests over a mock transport, and vehicle recordings |
| `tests/sim/` | A simulated cable behind a pty, with fault injection |
| `tests/fuzz/` | Fuzzer for the reply parser |
| `tests/differential/` | Comparison with another J2534 library |
| `tools/ab-official/` | Comparison with Tactrix's Windows DLL, run under Wine in Docker |
| `tools/car/` | Read-only vehicle capture and analysis |
| `tools/probe/`, `tools/usbtap/` | USB descriptor dump; USB traffic recorder (macOS) |

## License

Contributions are accepted under GPL-3.0-or-later. New files carry
`SPDX-License-Identifier: GPL-3.0-or-later` in their header comment.
