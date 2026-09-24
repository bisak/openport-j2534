# Contributing

## Build and test

```bash
make            # libj2534 and op_probe
make test       # unit tests and golden traces; must pass with no cable attached
make check-all  # sanitizers, fuzzing and simulator scenarios as well
```

A change is ready when:

- `make test` passes with no hardware;
- `make CFLAGS="-O2 -g -Werror"` builds clean on macOS and on Linux (a container
  is enough);
- any behaviour measured on a cable or a vehicle is written down in
  `docs/PROTOCOL.md` with its date, method and confidence marker ([V], [P] or
  [U]);
- a user-visible change has a line in `NEWS.md` under an unreleased section at
  the top (start one if the newest section is a release).

`docs/TESTING.md` describes every test layer.

## What helps most

Live-bus data. Most open questions in `docs/PROTOCOL.md` §12 need a vehicle the
driver has not seen, particularly one that answers on K-line.
`tools/car/car-session.sh` runs a read-only session and archives everything;
`docs/CAR-SESSION.md` explains what it sends and why each request is safe. Open
an issue at https://github.com/bisak/openport-j2534/issues with the session
archive attached. The archive can contain the vehicle's VIN (the capture asks
for it with `$09 02`) and contains the cable's serial number
(`1-descriptors.txt`); remove both before posting it publicly.

Bug reports. Include the output of `OPENPORT_LOG=- OPENPORT_LOG_HEX=1 your-tool`
and the firmware version `examples/op_smoke` prints. A hang is always a bug:
every transfer in this driver is bounded.

A second device. The transport seam is `src/op_transport.h`; a new cable is a
new transport and a device profile, not a rewrite.

## Ground rules

- Match Tactrix's DLL on the wire and in behaviour unless there is a stated
  reason not to, such as a J2534-1 return code or a request the firmware would
  act on wrongly without the application being able to tell. List every
  departure, with its reason, in `README.md` and `docs/AB-OFFICIAL.md`.
- Do not add ISO-TP segmentation to the host. The cable's firmware does it.
- A golden trace in `tests/unit/test_golden.c` is a measurement. If a test
  disagrees with one, the code is wrong, not the fixture.
- Nothing in `tools/car/` may write to an ECU, open a programming session or
  switch programming voltage on. Keep it that way.
- Do not commit capture logs or anything that identifies a vehicle (VIN) or a
  cable (serial number).

## License

Contributions are accepted under GPL-3.0-or-later, the project's license. New
files carry `SPDX-License-Identifier: GPL-3.0-or-later` in their header comment.
