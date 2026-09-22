# Contributing

## Build and test

```bash
make            # libj2534 and op_probe
make test       # unit tests and golden traces; must pass with no cable attached
make check-all  # sanitizers, fuzzing and simulator scenarios as well
```

A change is ready when `make test` passes with no hardware, `make CFLAGS="-O2 -g -Werror"` builds
clean on macOS and on Linux (a container is enough), and any behaviour that was measured on a
cable or a vehicle is written down in `docs/PROTOCOL.md` with its evidence level.

## What helps most

**Live-bus data.** Most open questions in `docs/PROTOCOL.md` §12 need a vehicle the driver has not
seen, particularly one that answers on K-line. `tools/car/car-session.sh` runs a read-only session
and archives everything; `docs/CAR-SESSION.md` explains what it asks and why each request is safe.
Open an issue with the session directory attached.

**Bug reports.** Include the output of `OPENPORT_LOG=- OPENPORT_LOG_HEX=1 your-tool` and the
firmware version `op_probe` prints. A hang is always a bug: every transfer in this driver is
bounded.

**A second device.** The transport seam is `src/op_transport.h`; a new cable is a new transport
and a device profile, not a rewrite.

## Ground rules

- Do not add ISO-TP segmentation to the host. The cable's firmware does it.
- A golden trace in `tests/unit/test_golden.c` is a measurement. If a test disagrees with one, the
  code is wrong, not the fixture.
- Nothing in `tools/car/` may write to an ECU, open a programming session or switch programming
  voltage on. Keep it that way.
- Do not commit capture logs or anything that identifies a vehicle (VIN) or a cable (serial).

## License

Contributions are accepted under GPL-3.0-or-later, the project's license. New files carry
`SPDX-License-Identifier: GPL-3.0-or-later` in their header comment.
