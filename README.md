# openport-j2534 — Tactrix OpenPort 2.0 driver for macOS and Linux

An open-source **SAE J2534 PassThru driver for the Tactrix OpenPort 2.0**
cable on **macOS (Apple Silicon and Intel) and Linux**. It is a drop-in
`libj2534` that any J2534 application, or a few lines of Python `ctypes`, can
load to talk CAN, ISO 15765 (ISO-TP) and K-line (ISO 9141 / ISO 14230) to a
vehicle through the OpenPort 2.0.

Tactrix ships a Windows driver only: no macOS or Linux driver, no SDK, no
source, no protocol specification. This driver is written against the J2534-1
standard and a wire protocol established by observing the cable and by running
Tactrix's own Windows DLL against the same cable. The protocol is written down
in [`docs/PROTOCOL.md`](docs/PROTOCOL.md).

- No `sudo` to run, no kernel extensions, no boot arguments.
- Every USB transfer is bounded; a wedged cable returns `ERR_TIMEOUT`, never a hang.
- Every result is honest: a transmit the cable rejected is reported as rejected.
- Byte-for-byte parity with Tactrix's own driver on the wire, verified with
  the vendor DLL running against the same cable ([`docs/AB-OFFICIAL.md`](docs/AB-OFFICIAL.md)).
- Hardware-free test suite: unit tests, fuzzing, sanitizers, a protocol
  simulator. Runs on macOS and Linux.

GPL-3.0-or-later. Not affiliated with or endorsed by Tactrix.

## Install

### Homebrew (macOS)

```bash
brew install bisak/tap/openport-j2534
```

Homebrew asks you to trust the `bisak/tap` tap the first time (`brew trust
bisak/tap`). The formula lives in
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

### Linux

The build and the whole hardware-free suite are verified on Ubuntu 24.04;
`make install` puts `libj2534.so` under `/usr/local`. Two things Linux needs
that macOS does not:

- **Permission to open the cable.** Install a udev rule once, then re-plug:

  ```
  # /etc/udev/rules.d/70-openport.rules
  SUBSYSTEM=="usb", ATTR{idVendor}=="0403", ATTR{idProduct}=="cc4d", MODE="0666"
  ```

  The serial transport (`OPENPORT_DEVICE=/dev/ttyACM0`) needs membership of
  the group that owns the node instead, usually `dialout`.
- **The cable is handed back to the kernel on close.** Linux binds `cdc_acm`
  to the cable and claiming the USB interface detaches it, which takes
  `/dev/ttyACM*` away. `PassThruClose` reattaches the driver, so the serial
  node returns a moment after the library lets go.

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
The API is J2534-1 04.04, the revision Tactrix's DLL reports; its constants
and the Tactrix-specific ones (J2534-2 channel ids, `SNIFF_MODE`,
`TX_PARAM_STOP_BITS`, pin numbers) are in the header.

```bash
./examples/op_smoke        # end-to-end check against a real cable
./tools/op_probe           # dump the cable's USB descriptors
```

### Hardware and protocols

OpenPort 2.0 (`0403:cc4d`), firmware 1.17.4877 measured.

| Protocol | Status |
|---|---|
| ISO15765 (CAN + ISO-TP) | complete; segmentation is done by the cable's firmware |
| CAN (raw) | complete |
| ISO9141, ISO14230 (K-line) | complete; frame layout from three implementations, not yet captured on this project's cable |
| ISO9141, ISO14230 on the L line (`ISO9141_L`, `ISO14230_L`) | opens the channels Tactrix's DLL opens; no data measured |
| RS-232 receive on the 2.5 mm jack (`ISO9141_INNO`) | opens, `TX_PARAM_STOP_BITS` configurable; no data measured |
| SCI A/B, J1850 VPW/PWM | refused with `ERR_INVALID_PROTOCOL_ID`, as Tactrix's DLL does; the firmware has no channel for them |

## J2534 conformance

Everything below is exercised by the test suite and, where a cable was
involved, compared against Tactrix's DLL. "Untested" means implemented but
never exercised against a vehicle.

### Entry points

| Function | Status |
|---|---|
| `PassThruOpen` / `Close` | complete; open resets the cable so a session never inherits a channel, a periodic message or a live pin |
| `PassThruConnect` / `Disconnect` | complete, J2534-2 channel ids included; connect flags go to the firmware unchanged |
| `PassThruReadMsgs` | complete; timeout honoured exactly as given; 1 MiB queue per channel, an overrun is reported as `ERR_BUFFER_OVERFLOW` |
| `PassThruWriteMsgs` | complete; the caller's timeout is the firmware's transmit budget, as the vendor sends it |
| `PassThruStartPeriodicMsg` / `StopPeriodicMsg` | complete, scheduled by the firmware (`atm`/`atn`), 10 per channel |
| `PassThruStartMsgFilter` / `StopMsgFilter` | complete (PASS, BLOCK, FLOW_CONTROL) |
| `PassThruSetProgrammingVoltage` | complete, see below |
| `PassThruReadVersion` | complete |
| `PassThruGetLastError` | complete |
| `PassThruIoctl` | see below |

### IOCTLs

| ID | Name | Status |
|---|---|---|
| 1, 2 | `GET_CONFIG`, `SET_CONFIG` | complete; passed to the firmware, whose supported set differs per protocol (`PROTOCOL.md` §8); an unsupported parameter returns `ERR_NOT_SUPPORTED`, as the device reports |
| 3 | `READ_VBATT` | complete |
| 4, 5 | `FIVE_BAUD_INIT`, `FAST_INIT` | the commands Tactrix's DLL sends, with `SBYTE_ARRAY` in/out for five-baud as the standard specifies; **untested on hardware**, needs a K-line vehicle |
| 7, 8 | `CLEAR_TX_BUFFER`, `CLEAR_RX_BUFFER` | complete, host-side as in the vendor DLL |
| 9, 10 | `CLEAR_PERIODIC_MSGS`, `CLEAR_MSG_FILTERS` | complete |
| 11–13 | functional message table | `ERR_NOT_SUPPORTED`: J1850 only, which this hardware rejects |
| 14 | `READ_PROG_VOLTAGE` | complete; reads pin 12, or the pin `pInput` points to as in Tactrix's DLL (8, 12, 16, or 17 for the adjustable supply) |

### Where this driver deliberately differs from Tactrix's DLL

Return codes follow the J2534-1 text where the vendor departs from it: any
call before `PassThruOpen` or after `PassThruClose` returns
`ERR_INVALID_DEVICE_ID`, a NULL message pointer returns `ERR_NULL_PARAMETER`,
an unknown ioctl returns `ERR_INVALID_IOCTL_ID`. Programming voltage on a
second pin, and grounding the K or L line under a channel that uses it, are
refused (below); the vendor passes them to the cable. Everything else the two
drivers put on the wire is the same, command for command
([`docs/AB-OFFICIAL.md`](docs/AB-OFFICIAL.md)).

### Known gaps

- **K-line frame layout** is implemented from three independent sources but
  not yet measured on this project's hardware (`PROTOCOL.md` §7). A bench
  ECU simulator with ISO 9141-2 and KWP2000 would settle it in an afternoon.
- **Replies longer than one wire frame (250 bytes)** are reassembled the way
  Tactrix's DLL reassembles them (the CAN id repeated in every chunk); no
  capture of one from the cable exists yet.
- The L-line and jack channels are wired to what Tactrix's DLL sends, but no
  L-line ECU or Innovate device has been attached to confirm their data.
- A real ECU may ignore an **unpadded** ISO15765 request, so a caller must set
  the `ISO15765_FRAME_PAD` TxFlag, as the vendor's own samples do.

## Programming voltage

The OpenPort 2.0 can put a voltage on a connector pin or short it to ground
(`atv`), used by some bench and bootloader reflash procedures. This works as
it does with Tactrix's DLL, with three rules the cable itself does not
enforce:

- The cable accepts 5000–20000 mV; outside that it answers
  `ERR_OEM_VOLTAGE_TOO_LOW` or `_TOO_HIGH`.
- Every voltage pin is fed from one supply, so a second pin would silently
  move the first pin's voltage. A second pin is refused with
  `ERR_PIN_INVALID` until the first is switched off, as J2534-1 §7.2.11
  requires. `READ_PROG_VOLTAGE` with pin 17 reads the supply.
- Grounding K (pin 7) while an ISO9141 or ISO14230 channel is open, or L
  (pin 15) while an L-line channel is open, returns `ERR_CHANNEL_IN_USE`, and
  so does opening such a channel while this session holds its pin grounded.
  The cable allows it and it silently ends communication on that line.

Pin 12 is also the tip of the 2.5 mm jack: with a plug inserted, the cable
disconnects it from the vehicle connector, and `atv 12` drives the jack
instead. Which pins accept what is in `docs/PROTOCOL.md` §8. `PassThruOpen`
and `PassThruClose` both reset the cable, which switches every output off.

## Logging

```bash
OPENPORT_LOG=/tmp/op.log OPENPORT_LOG_HEX=1 ./your-tool
```

`OPENPORT_LOG` takes a path, or `-`/`stderr`. `OPENPORT_LOG_HEX=1` adds every
byte transferred. `OPENPORT_RECORD=<file>` records every entry-point call for
replay through another J2534 library (`docs/AB-OFFICIAL.md`). All off unless
asked for.

## Troubleshooting

**`PassThruOpen` returns 8 (`ERR_DEVICE_NOT_CONNECTED`)**: call
`PassThruGetLastError`, which names the actual cause. Then:

- **"enumerated as USB mass storage"**: a microSD card is inserted. The cable
  presents as a disk and the data interface is unavailable. Remove the card and
  re-plug.
- **No cable found**: check `./tools/op_probe`. It should report `0403:cc4d`.
- **Returns 14 (`ERR_DEVICE_IN_USE`)**: another program holds the interface.
  Close other J2534 applications.

**It hangs.** It should not: every transfer is bounded and a wedged cable
returns `ERR_TIMEOUT`. A genuine hang is a bug; please report it with
`OPENPORT_LOG_HEX=1` output.

**A call returns the previous call's answer.** Every command carries a
sequence number and only the reply that echoes it is accepted
(`PROTOCOL.md` §3), so this cannot happen by design. If it does, it is a bug;
the log will show it.

**`sudo` is not the fix** for anything here. If claiming fails, the error says
why.

## Layout

```
include/j2534/j2534.h   the standard API, plus Tactrix's own constants
src/op_proto.c          wire codec: pure, no I/O, fully unit-tested
src/op_transport.h      byte-pipe interface (a second device plugs in here)
src/op_usb.c            libusb bulk transport
src/op_serial.c         CDC-ACM transport (OPENPORT_DEVICE=/dev/cu.usbmodem... or /dev/ttyACM0)
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
make differential OLD_DRIVER=/path/to/libj2534.dylib   # A/B vs another driver (macOS)
make ab-official AB_DLL=/path/to/op20pt32.dll  # A/B vs the vendor DLL (Docker)
./examples/op_smoke                            # end to end, cable required
```

The first two run on macOS and Linux. See [`docs/TESTING.md`](docs/TESTING.md)
for what each layer proves.

## Safety

Read-only diagnostics are safe. Reflashing is not: a driver that lies about a
failed transmit can brick an ECU, which is the reason this driver exists, but
it cannot protect against a wrong image or a wrong tool. Programming voltage
works exactly as with the vendor driver: an application that asks for it gets
it. Everything in `tools/car/` is read-only by construction. You are
responsible for what you send to your vehicle.

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md). The most valuable contribution is
**live-bus data** from a vehicle this driver has not seen, especially one that
speaks K-line: `PROTOCOL.md` §12 lists what is still open.

## License

GPL-3.0-or-later. Copyright 2026 Biser Atanasov. See [`NOTICE`](NOTICE) for
provenance and trademark notes.
