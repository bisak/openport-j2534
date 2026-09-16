# openport-j2534 — Tactrix OpenPort 2.0 driver for macOS and Linux

An open-source **SAE J2534 PassThru driver for the Tactrix OpenPort 2.0** cable
on **macOS Apple Silicon (M1, M2, M3, M4) and Intel Macs**, and on Linux.
It is a drop-in `libj2534` that any J2534 application, or a few lines of
Python `ctypes`, can load to talk CAN, ISO 15765 (ISO-TP) and K-line
(ISO 9141 / ISO 14230) to a vehicle through the OpenPort 2.0.

Tactrix ships a Windows driver only: no macOS or Linux driver, no SDK, no
source, and no published protocol specification. This is an independent
implementation written against the J2534-1 standard and a wire protocol
established by observing the cable. That protocol is written down in
[`docs/PROTOCOL.md`](docs/PROTOCOL.md).

- No `sudo`, no kernel extensions, no boot arguments.
- Every USB transfer is bounded; a wedged cable returns `ERR_TIMEOUT`, never a hang.
- Every result is honest: a transmit the cable rejected is reported as rejected.
- Verified against real vehicles and, byte for byte on the wire, against
  Tactrix's own Windows driver ([`docs/AB-OFFICIAL.md`](docs/AB-OFFICIAL.md)).
- Hardware-free test suite: unit tests, fuzzing, sanitizers, a protocol simulator.

GPL-3.0-or-later. Not affiliated with or endorsed by Tactrix.

## Install

No kernel extensions, no `sudo`, no SIP or recovery-mode changes: the driver
is an ordinary user-space library on libusb.

### Homebrew (macOS)

```bash
brew install bisak/tap/openport-j2534
```

Homebrew asks you to trust the `bisak/tap` tap the first time (or run
`brew trust bisak/tap` beforehand). The formula lives in
[bisak/homebrew-tap](https://github.com/bisak/homebrew-tap) and builds the
latest release from source in about a second; `--HEAD` builds `main`.

### From source (macOS or Linux)

Requirements: a C11 compiler, `pkg-config`, and libusb 1.0
(`brew install libusb pkg-config` on macOS, `apt install libusb-1.0-0-dev pkg-config` on Debian/Ubuntu).

```bash
git clone https://github.com/bisak/openport-j2534.git
cd openport-j2534
make
make test          # hardware-free suite, no cable needed
sudo make install
```

`make install` puts `libj2534.dylib` (or `.so`), the `j2534/j2534.h` header,
a pkg-config file and the `op_probe` tool under `/opt/homebrew` on Apple
Silicon or `/usr/local` elsewhere (`PREFIX=...` to change). Plug in the cable
and check it is seen:

```bash
op_probe
```

It should report `0403:cc4d`. If the cable enumerates as a disk instead,
remove the microSD card and re-plug.

## Why another driver

A community macOS driver exists. It **returns success for operations it did
not perform**: a transmit the device rejected, messages that were never
received. That is dangerous when the caller is erasing an ECU. See
[`docs/DIFFERENTIAL.md`](docs/DIFFERENTIAL.md) for the measured comparison.

That lineage (`NikolaKozina/j2534`, BSD 3-Clause) could have been built on;
this driver is not justified by necessity but by what it does differently: an
architecture with a testable seam, every libusb call checked, every timeout
bounded, and the correctness differences documented and measured. Nothing here
is derived from that codebase.

## Use

```c
#include <j2534/j2534.h>

unsigned long dev, ch;
PassThruOpen(NULL, &dev);
PassThruConnect(dev, ISO15765, 0, 500000, &ch);
/* ... */
PassThruDisconnect(ch);
PassThruClose(dev);
```

From Python via `ctypes`, or from any existing J2534 application, unchanged.

```bash
./examples/op_smoke        # end-to-end check against a real cable
./tools/op_probe           # dump the cable's USB descriptors
```

### Supported hardware and protocols

OpenPort 2.0 (`0403:cc4d`). Device detection and protocol handling are factored
so a second device can be added without restructuring.

| Protocol | Supported |
|---|---|
| ISO15765 (CAN + ISO-TP) | yes — segmentation is done by the cable's firmware |
| CAN (raw) | yes |
| ISO9141, ISO14230 (K-line) | yes, but see the conformance matrix |
| ISO9141, ISO14230 on the L line (`ISO9141_L`, `ISO14230_L`) | opens the channels Tactrix's DLL opens; **no data measured** |
| RS-232 receive on the 2.5 mm jack (`ISO9141_INNO`, for Innovate MTS) | opens, `TX_PARAM_STOP_BITS` configurable; **no data measured** |
| SCI A/B | **no** — refused with `ERR_INVALID_PROTOCOL_ID`, as Tactrix's DLL does; the firmware's channels 7–9 are the L line and the jack, not SCI |
| J1850 VPW/PWM | **no** — the firmware rejects it; Tactrix lists it as "support pending" |

## J2534 conformance

Honest, not aspirational. "Untested" means implemented but never exercised
against a vehicle.

### Entry points

| Function | Status |
|---|---|
| `PassThruOpen` / `Close` | complete |
| `PassThruConnect` / `Disconnect` | complete |
| `PassThruReadMsgs` | complete; timeout honoured exactly as given |
| `PassThruWriteMsgs` | complete |
| `PassThruStartPeriodicMsg` / `StopPeriodicMsg` | complete, host-scheduled, 10 per channel |
| `PassThruStartMsgFilter` / `StopMsgFilter` | complete (PASS, BLOCK, FLOW_CONTROL) |
| `PassThruSetProgrammingVoltage` | complete, **gated** — see below |
| `PassThruReadVersion` | complete |
| `PassThruGetLastError` | complete |
| `PassThruIoctl` | see below |

### IOCTLs

| ID | Name | Status |
|---|---|---|
| 1, 2 | `GET_CONFIG`, `SET_CONFIG` | complete (DATA_RATE, LOOPBACK, ISO15765_BS, ISO15765_STMIN; others `ERR_NOT_SUPPORTED`, as the device reports) |
| 3 | `READ_VBATT` | complete |
| 4, 5 | `FIVE_BAUD_INIT`, `FAST_INIT` | implemented from the DLL-derived command form (`PROTOCOL.md` §4), five-baud with `SBYTE_ARRAY` in/out as the standard specifies, **untested on hardware** — needs a K-line vehicle |
| 7, 8 | `CLEAR_TX_BUFFER`, `CLEAR_RX_BUFFER` | complete |
| 9, 10 | `CLEAR_PERIODIC_MSGS`, `CLEAR_MSG_FILTERS` | complete |
| 11–13 | functional message table | `ERR_NOT_SUPPORTED` — J1850 only, which this hardware rejects |
| 14 | `READ_PROG_VOLTAGE` | complete; reads pin 12, or the pin `pInput` points to as in Tactrix's DLL (8, 12, 16, or 17 for the adjustable supply) |

### Known gaps

- **The receive path is validated on live hardware** (2026-09-13, a 2012 VW
  Caddy): legislated OBD and UDS `$22`/`$09` replies, including multi-frame
  reassembly of the VIN, came through the driver's own entry points correctly,
  and `tests/unit/test_golden.c` carries the trace. One caveat learned there: a
  real ECU may ignore an **unpadded** request, so a caller must set the
  `ISO15765_FRAME_PAD` TxFlag. `docs/PROTOCOL.md` §10 lists the remaining open
  items (K-line framing and >250-byte chunking, both needing a car that uses
  them).
- **K-line frame layout** is implemented from three independent sources but
  not yet measured on this project's hardware (`PROTOCOL.md` §7).
- The L-line and jack channels are wired to what Tactrix's DLL sends, but no
  L-line ECU or Innovate device has been attached to confirm their data.

## Programming voltage

The OpenPort 2.0 can put programming voltage on a connector pin — used for some
bench and bootloader reflash procedures. It is implemented (`atv`), and it is
**off by default**:

```bash
OPENPORT_ENABLE_PROG_VOLTAGE=1 ./your-tool
```

Without that, applying a voltage or `SHORT_TO_GROUND` returns
`ERR_NOT_SUPPORTED` and nothing reaches the cable. Switching it **off**
(`VOLTAGE_OFF`) is always permitted, so a caller can always make the pin safe.

The cable accepts 5000–20000 mV (outside that, `ERR_OEM_VOLTAGE_TOO_LOW` or
`_TOO_HIGH`), and every voltage pin is fed from one supply: a second pin would
silently move the first one's voltage, so it returns `ERR_EXCEEDED_LIMIT` until
the first is switched off. `READ_PROG_VOLTAGE` with pin 17 reads the supply.

Grounding K (pin 7) while an ISO9141 or ISO14230 channel is open, or L
(pin 15) while an L-line channel is open, returns `ERR_CHANNEL_IN_USE`, and so
does opening such a channel while this session holds its pin grounded. The firmware and Tactrix's DLL both allow it, and it
silently ends K-line communication.

Pin 12 is also the tip of the 2.5 mm jack: with a plug inserted, the cable
disconnects it from the vehicle connector, and `atv 12` drives the jack
instead. Which pins accept what is in `docs/PROTOCOL.md` §8.

## Logging

```bash
OPENPORT_LOG=/tmp/op.log OPENPORT_LOG_HEX=1 ./your-tool
```

`OPENPORT_LOG` takes a path, or `-`/`stderr`. `OPENPORT_LOG_HEX=1` adds every
byte transferred. Off unless asked for.

## Troubleshooting

**`PassThruOpen` returns 8 (`ERR_DEVICE_NOT_CONNECTED`)** — call
`PassThruGetLastError`, which names the actual cause. Then:

- **"enumerated as USB mass storage"** — a microSD card is inserted. The cable
  presents as a disk and the data interface is unavailable. Remove the card and
  re-plug.
- **No cable found** — check `./tools/op_probe`. It should report `0403:cc4d`.
- **Returns 14 (`ERR_DEVICE_IN_USE`)** — another program holds the interface.
  Close other J2534 applications.

**It hangs.** It should not: every transfer is bounded and a wedged cable
returns `ERR_TIMEOUT`. If you see a genuine hang, that is a bug — please report
it with `OPENPORT_LOG_HEX=1` output.

**Results look shifted — a call returns the previous call's answer.** This
driver synchronises at open and discards orphan replies specifically to prevent
that (`PROTOCOL.md` §9.2). If you still see it, it is a bug; the log will show
it.

**`sudo` is not the fix** for anything here. If claiming fails, the error says
why.

## Layout

```
include/j2534/j2534.h   the standard API
src/op_proto.c          wire codec: pure, no I/O, fully unit-tested
src/op_transport.h      byte-pipe interface (a second device plugs in here)
src/op_usb.c            libusb bulk transport
src/op_serial.c         CDC-ACM transport (OPENPORT_DEVICE=/dev/cu.usbmodem...)
src/op_device.c         channels, reassembly, reader thread, reply matching
src/op_j2534.c          the fourteen entry points
examples/               op_smoke, op_iso15765, op_kline: end-to-end checks on a cable
tools/probe/            op_probe: USB descriptor dump
tools/usbtap/           libusb interposer that records wire traffic
tools/car/              read-only vehicle capture and analysis
tools/ab-official/      A/B harness against Tactrix's Windows DLL (Docker + Wine)
tests/unit/             hardware-free tests over a mock transport, golden vehicle traces
tests/sim/              protocol simulator and scenarios, fault injection
tests/fuzz/             fuzzer for the wire parser
tests/differential/     A/B harness against another J2534 library
docs/                   PROTOCOL, TESTING, DIFFERENTIAL, AB-OFFICIAL, CAR-SESSION
```

## Testing

```bash
make test                                      # unit tests and golden traces, no hardware
make check-all                                 # + sanitizers, fuzzing, simulator scenarios
make differential OLD_DRIVER=/path/to/libj2534.dylib   # A/B vs another driver
make ab-official AB_DLL=/path/to/op20pt32.dll  # A/B vs the vendor DLL (Docker)
./examples/op_smoke                            # end to end, cable required
```

See [`docs/TESTING.md`](docs/TESTING.md) for what each layer proves.

## Safety

Read-only diagnostics are safe. Reflashing is not: a driver that lies about a
failed transmit can brick an ECU, which is the reason this driver exists, but
it cannot protect against a wrong image or a wrong tool. Programming voltage
is off unless `OPENPORT_ENABLE_PROG_VOLTAGE=1` is set. Everything in
`tools/car/` is read-only by construction. You are responsible for what you
send to your vehicle.

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md). The most valuable contribution is
**live-bus data** from a vehicle this driver has not seen, especially one that
speaks K-line: `PROTOCOL.md` §10 lists what is still open.

## License

GPL-3.0-or-later. Copyright 2026 Biser Atanasov. See [`NOTICE`](NOTICE) for
provenance and trademark notes.
