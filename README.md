# openport-j2534: Tactrix OpenPort 2.0 driver for macOS and Linux

An open-source SAE J2534 PassThru driver for the Tactrix OpenPort 2.0 cable on
macOS (Apple Silicon and Intel) and Linux. It builds `libj2534`, a shared
library that a J2534 application, or a few lines of Python `ctypes`, loads to
talk CAN, ISO 15765 (ISO-TP) and K-line (ISO 9141, ISO 14230) to a vehicle
through the cable.

Tactrix ships a Windows driver only, with no SDK, source or protocol
specification. This driver is written against the J2534-1 standard and a wire
protocol established by observing the cable and by running Tactrix's own
Windows DLL against the same cable. The protocol is documented in
[`docs/PROTOCOL.md`](docs/PROTOCOL.md).

- Runs without `sudo`, kernel extensions or boot arguments. Linux needs a udev
  rule once (below).
- Every USB transfer is bounded: a wedged cable returns `ERR_TIMEOUT` instead of
  hanging the application.
- A transmit the cable rejected is reported as rejected.
- Sends the commands Tactrix's DLL sends. Compared step by step on the cable
  (2026-09-24), 648 of 670 parameter-sweep steps put identical commands on the
  wire; the other 22 follow from the differences listed under
  [Where this driver differs from Tactrix's DLL](#where-this-driver-differs-from-tactrixs-dll)
  ([`docs/AB-OFFICIAL.md`](docs/AB-OFFICIAL.md)).
- A hardware-free test suite (unit tests, golden vehicle traces, fuzzing,
  sanitizers, a protocol simulator) runs on macOS and Linux.

The project lives at https://github.com/bisak/openport-j2534. Release history,
and the changes on `main` since the last release, are in [`NEWS.md`](NEWS.md).

GPL-3.0-or-later. Not affiliated with or endorsed by Tactrix.

## Requirements

- macOS on Apple Silicon or Intel, or Linux. The protocol measurements were
  made on macOS 26.6 (Apple Silicon) with libusb 1.0.29. The Linux build and
  the hardware-free suite were verified for 0.2.1 in an Ubuntu 24.04 container
  (GCC 13.3, aarch64).
- A C11 compiler, `make`, `pkg-config` and libusb 1.0.
- Python 3 for the simulator scenarios and the tools under `tools/`.
- A Tactrix OpenPort 2.0 (USB `0403:cc4d`). Measured with firmware 1.17.4877.

## Install

### Homebrew (macOS)

```bash
brew install bisak/tap/openport-j2534
```

Homebrew asks you to trust the `bisak/tap` tap the first time
(`brew trust bisak/tap`). The formula lives in
[bisak/homebrew-tap](https://github.com/bisak/homebrew-tap) and builds the
latest release from source in about a second; `--HEAD` builds `main`.

### From source (macOS or Linux)

Install libusb and pkg-config. On macOS:

```bash
brew install libusb pkg-config
```

On Debian or Ubuntu:

```bash
sudo apt install libusb-1.0-0-dev pkg-config
```

Get the source:

```bash
git clone https://github.com/bisak/openport-j2534.git
```

```bash
cd openport-j2534
```

Build the library and `op_probe`:

```bash
make
```

Run the hardware-free suite; no cable is needed:

```bash
make test
```

Install:

```bash
make install
```

`make install` puts `libj2534.dylib` (`libj2534.so` on Linux), the
`j2534/j2534.h` header, the `openport-j2534` pkg-config file and the `op_probe`
tool under `/opt/homebrew` on Apple Silicon and `/usr/local` elsewhere. Set
`PREFIX=...` to change it, and use `sudo make install` if you cannot write to
the prefix (usually the case for `/usr/local` on Linux). `make uninstall`
removes the same files.

Plug in the cable and check that it is seen:

```bash
op_probe
```

It should print a device `0403:cc4d`. If the cable enumerates as USB mass
storage instead, remove its microSD card and re-plug it.

### Linux

Two things Linux needs that macOS does not:

- Permission to open the cable. Install a udev rule once, then re-plug:

  ```
  # /etc/udev/rules.d/70-openport.rules
  SUBSYSTEM=="usb", ATTR{idVendor}=="0403", ATTR{idProduct}=="cc4d", MODE="0666"
  ```

  The serial transport (`OPENPORT_DEVICE=/dev/ttyACM0`) instead needs
  membership of the group that owns the node, usually `dialout`.
- The cable is handed back to the kernel on close. Linux binds `cdc_acm` to the
  cable, and claiming the USB interface detaches it, which takes
  `/dev/ttyACM*` away. `PassThruClose` reattaches the kernel driver, so the
  serial node returns a moment after the library lets go. This was written from
  a report on Linux hardware; this project's own cable has not yet confirmed it
  (`docs/PROTOCOL.md` §1, §12).

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

Build against the installed library:

```bash
cc app.c $(pkg-config --cflags --libs openport-j2534) -o app
```

Python via `ctypes`, and existing J2534 applications, load the library
unchanged. The API is J2534-1 04.04, the revision Tactrix's DLL reports. Its
constants and the Tactrix-specific ones (J2534-2 channel ids, `SNIFF_MODE`,
`TX_PARAM_STOP_BITS`, pin numbers, the OEM voltage error codes) are in the
header.

### Examples

`examples/` holds three end-to-end checks against a real cable. Build the first
one:

```bash
make smoke
```

Run it:

```bash
./examples/op_smoke
```

`op_smoke` opens the cable, reads the versions and voltages, opens and closes
an ISO15765 channel and installs a filter; it transmits only with `--tx`.
`make iso15765` builds `op_iso15765` (OBD modes 01 and 09 on a vehicle, with and
without `ISO15765_FRAME_PAD`), and `make kline` builds `op_kline` (one K-line
init, one ReadEcuIdentification, one TesterPresent). All three are read-only.

On macOS an example loads the installed library, because the library's install
name is its installed path. To run one against the library in the build
directory, set `DYLD_LIBRARY_PATH` (macOS) or `LD_LIBRARY_PATH` (Linux) to that
directory.

### Hardware and protocols

OpenPort 2.0 (`0403:cc4d`), firmware 1.17.4877 measured.

| Protocol | Status |
|---|---|
| ISO15765 (CAN + ISO-TP) | complete; segmentation is done by the cable's firmware |
| CAN (raw) | complete |
| ISO9141, ISO14230 (K-line) | complete, but no K-line ECU has answered it yet; the frame layout comes from other implementations (two independent lines of evidence) and has not been captured on this project's cable |
| ISO9141, ISO14230 on the L line (`ISO9141_L`, `ISO14230_L`) | opens the channels Tactrix's DLL opens; no data measured |
| RS-232 receive on the 2.5 mm jack (`ISO9141_INNO`) | opens, `TX_PARAM_STOP_BITS` configurable; no data measured |
| SCI A/B, J1850 VPW/PWM | refused with `ERR_INVALID_PROTOCOL_ID`, as Tactrix's DLL does; the firmware has no channel for them |

## J2534 conformance

Everything below is exercised by the test suite and, where a cable was
involved, compared against Tactrix's DLL. "Untested" means implemented but never
exercised against a vehicle.

### Entry points

| Function | Status |
|---|---|
| `PassThruOpen` / `Close` | complete; open resets the cable, so a session never inherits a channel, a periodic message or a live pin, and close resets it too |
| `PassThruConnect` / `Disconnect` | complete, J2534-2 channel ids included; connect flags go to the firmware unchanged |
| `PassThruReadMsgs` | complete; the timeout is honoured exactly as given; 1 MiB queue per channel; an overrun is reported as `ERR_BUFFER_OVERFLOW` |
| `PassThruWriteMsgs` | complete; the caller's timeout covers the whole call and goes to the firmware as its transmit budget, as the vendor DLL sends it |
| `PassThruStartPeriodicMsg` / `StopPeriodicMsg` | complete, scheduled by the firmware (`atm`/`atn`), 10 per channel; any interval up to 4 294 967 ms, as the vendor DLL forwards it, not only J2534's 5–65535 ms |
| `PassThruStartMsgFilter` / `StopMsgFilter` | complete (PASS, BLOCK, FLOW_CONTROL); the firmware holds 10 per channel and answers an eleventh with `ERR_EXCEEDED_LIMIT`; a flow-control message given with a PASS or BLOCK filter is ignored, as in the vendor DLL |
| `PassThruSetProgrammingVoltage` | complete, see [Programming voltage](#programming-voltage) |
| `PassThruReadVersion` | complete |
| `PassThruGetLastError` | complete |
| `PassThruIoctl` | see below |

### IOCTLs

| ID | Name | Status |
|---|---|---|
| 1, 2 | `GET_CONFIG`, `SET_CONFIG` | complete; passed to the firmware, whose supported set differs per protocol (`PROTOCOL.md` §8). Every parameter in the list is sent and the call returns the last one's status, as the vendor DLL does; an unsupported parameter answers `ERR_NOT_SUPPORTED` |
| 3 | `READ_VBATT` | complete |
| 4, 5 | `FIVE_BAUD_INIT`, `FAST_INIT` | the commands Tactrix's DLL sends, with `SBYTE_ARRAY` in and out for five-baud as the standard specifies; no K-line ECU has answered one yet (on the one car tried, both drivers failed identically, `AB-OFFICIAL.md`) |
| 7, 8 | `CLEAR_TX_BUFFER`, `CLEAR_RX_BUFFER` | complete, host-side as in the vendor DLL |
| 9, 10 | `CLEAR_PERIODIC_MSGS`, `CLEAR_MSG_FILTERS` | complete: one `atl<ch>` or `atk<ch> -1`, as the vendor DLL sends them |
| 11–13 | functional message table | `ERR_NOT_SUPPORTED`: J1850 only, which this hardware rejects |
| 14 | `READ_PROG_VOLTAGE` | complete; reads the pin `pInput` points to, as Tactrix's DLL does (8, 12, 16, or 17 for the adjustable supply), and pin 12 when `pInput` is NULL as J2534-1 passes it (the vendor DLL returns -1 then) |

### Where this driver differs from Tactrix's DLL

Return codes follow the J2534-1 text where the vendor departs from it:

- any call before `PassThruOpen` or after `PassThruClose` returns
  `ERR_INVALID_DEVICE_ID`;
- a NULL message pointer returns `ERR_NULL_PARAMETER`;
- an unknown ioctl returns `ERR_INVALID_IOCTL_ID`;
- a failed `PassThruWriteMsgs` reports in `pNumMsgs` how many messages went out;
  the vendor leaves the caller's count as it was.

These are refused before the wire, although the vendor passes them to the
cable, because the firmware would act on them wrongly and the application could
not tell:

- programming voltage on a second pin, and grounding the K or L line under a
  channel that uses it ([Programming voltage](#programming-voltage));
- a connect at rate 0, which the firmware opens;
- an ISO15765 message longer than 4099 bytes (4100 with extended addressing),
  which the firmware tries to send;
- a periodic message outside its protocol's size limits (the firmware accepted a
  3-byte CAN one and repeated it every interval).

Messages are routed differently in two cases:

- A message goes out on the channel it was written to. The vendor picks the
  firmware channel by the message's `ProtocolID`.
- With a raw CAN channel open beside an ISO15765 channel, frames the firmware
  reports on CAN channel 5 stay on the CAN channel. The vendor delivers all of
  them to the ISO15765 channel. With channel 5 closed, both drivers deliver the
  ISO15765 loopback echoes to the ISO15765 channel (`PROTOCOL.md` §7.7).

On the wire, this driver opens with `atz`, `ata`, `ati` and the vendor with `ati`,
`ata`; and a `Timeout=0` write carries a sequence number here, so that its late
reply is recognised and dropped.

The remaining differences are return codes, some for requests this driver
refuses before the wire, and one case this driver accepts where the vendor
refuses (a flow-control filter whose messages carry different TxFlags). All are
listed in [`docs/AB-OFFICIAL.md`](docs/AB-OFFICIAL.md). The vendor DLL crashes
the application on a NULL `GET_CONFIG`, `SET_CONFIG` or `FAST_INIT` input; this
driver does not.

### Known gaps

- The K-line frame layout is implemented from other implementations but not yet
  measured on this project's hardware (`PROTOCOL.md` §7.9). One K-line session
  recorded with `tools/car/car_capture.py --kline`, or a bench OBD simulator
  that speaks ISO 9141-2 and KWP2000, would settle it (`PROTOCOL.md` §12).
- The L-line and jack channels are wired to what Tactrix's DLL sends, but no
  L-line ECU or Innovate device has been attached to confirm their data.
- A real ECU may ignore an unpadded ISO15765 request, so a caller must set the
  `ISO15765_FRAME_PAD` TxFlag, as the vendor's own samples do. The driver logs a
  transmit without it but does not change the caller's TxFlags.

## Programming voltage

The OpenPort 2.0 can put a voltage on a connector pin or short it to ground
(`atv`), which some bench and bootloader reflash procedures use. The driver
passes these requests to the cable as Tactrix's DLL does. The cable accepts
5000–20000 mV and answers anything outside that with `ERR_OEM_VOLTAGE_TOO_LOW`
or `ERR_OEM_VOLTAGE_TOO_HIGH`. The driver adds two refusals the cable does not
make:

- Every voltage pin is fed from one supply, so a second pin would silently move
  the first pin's voltage. A second pin is refused with `ERR_PIN_INVALID` until
  the first is switched off, as J2534-1 §7.2.11 requires. `READ_PROG_VOLTAGE`
  with pin 17 reads the supply.
- Grounding K (pin 7) while an ISO9141 or ISO14230 channel is open, or L (pin
  15) while an L-line channel is open, returns `ERR_CHANNEL_IN_USE`, and so does
  opening such a channel while this session holds its pin grounded. The cable
  allows it, and it silently ends communication on that line.

Pin 12 is also the tip of the 2.5 mm jack: with a plug inserted, the cable
disconnects pin 12 from the vehicle connector, and `atv 12` drives the jack
instead. Which pins accept what is in `docs/PROTOCOL.md` §8. `PassThruOpen` and
`PassThruClose` both reset the cable, which switches every output off.

## Environment variables

All are off unless set.

- `OPENPORT_LOG=<path>` writes a diagnostic log to the file, or to stderr for
  `-` or `stderr`.
- `OPENPORT_LOG_HEX=1` adds every byte transferred to that log.
- `OPENPORT_RECORD=<file>` appends one line per entry-point call, for replay
  through another J2534 library (`docs/AB-OFFICIAL.md`).
- `OPENPORT_DEVICE=<node>` uses a character device instead of USB: the cable's
  own CDC-ACM node (`/dev/cu.usbmodem...` on macOS, `/dev/ttyACM0` on Linux) or
  the simulator's pty.

```bash
OPENPORT_LOG=/tmp/op.log OPENPORT_LOG_HEX=1 ./your-tool
```

## Troubleshooting

After a failure, call `PassThruGetLastError`: its text names the cause.

- `PassThruOpen` returns 8 (`ERR_DEVICE_NOT_CONNECTED`):
  - "enumerated as USB mass storage": a microSD card is inserted, the cable
    presents as a disk, and the data interface is unavailable. Remove the card
    and re-plug.
  - No cable found: run `op_probe`, which should print `0403:cc4d`.
- `PassThruOpen` returns 14 (`ERR_DEVICE_IN_USE`):
  - "interface ... busy": another program holds the interface. Close other
    J2534 applications.
  - "libusb_open failed: LIBUSB_ERROR_ACCESS" on Linux: no permission to open
    the cable. Install the udev rule above and re-plug.
- `PassThruOpen` returns 7 (`ERR_FAILED`) with "not answering the protocol
  correctly": the cable was found and accepts writes, but did not answer the
  open sequence (`\r\n\r\n`, `atz`, `ata`; `PROTOCOL.md` §9.1) coherently.
  Report it with a log.
- A call hangs. It should not: every transfer is bounded and a wedged cable
  returns `ERR_TIMEOUT`. A hang is a bug; report it with the output of
  `OPENPORT_LOG=- OPENPORT_LOG_HEX=1`.
- A call returns the previous call's answer. Every command carries a sequence
  number and only the reply that echoes it is accepted (`PROTOCOL.md` §3), so
  this should not happen. If it does, it is a bug, and the log shows it.

Do not run applications with `sudo` to work around any of these. On Linux the
udev rule grants access, and if claiming the interface fails, the error says
why.

## Testing

- `make test`: unit tests and golden vehicle traces, no hardware.
- `make check-all`: the above plus sanitizers, fuzzing and the simulator
  scenarios.
- `make differential OLD_DRIVER=/path/to/libj2534.dylib`: A/B against another
  J2534 library (macOS).
- `make ab-official AB_DLL=/path/to/op20pt32.dll`: A/B against Tactrix's DLL in
  Docker (Apple Silicon Mac).
- `./examples/op_smoke`: end to end, cable required.

The first two run on macOS and Linux. [`docs/TESTING.md`](docs/TESTING.md) says
what each layer checks.

## Files

```
include/j2534/j2534.h   the standard API, plus Tactrix's own constants
src/op_proto.c          wire codec: pure, no I/O, fully unit-tested
src/op_transport.h      byte-pipe interface (a second device plugs in here)
src/op_usb.c            libusb bulk transport
src/op_serial.c         CDC-ACM transport (OPENPORT_DEVICE)
src/op_device.c         channels, reassembly, reader thread, reply matching
src/op_j2534.c          the fourteen entry points
src/op_log.c            OPENPORT_LOG
src/op_error.c          PassThruGetLastError text
examples/               op_smoke, op_iso15765, op_kline: end-to-end checks on a cable
tools/probe/            op_probe: USB descriptor dump
tools/usbtap/           libusb interposer that records wire traffic (macOS)
tools/car/              read-only vehicle capture and analysis
tools/ab-official/      A/B harness against Tactrix's Windows DLL (Docker + Wine)
tests/unit/             hardware-free tests over a mock transport, golden vehicle traces
tests/mock/             the mock transport
tests/sim/              protocol simulator and scenarios, fault injection
tests/fuzz/             fuzzer for the wire parser, and its corpus
tests/differential/     A/B harness against another J2534 library
docs/PROTOCOL.md        the wire protocol, with the evidence for each claim
docs/TESTING.md         the test layers and what each catches
docs/AB-OFFICIAL.md     the A/B harness against Tactrix's DLL, and its results
docs/DIFFERENTIAL.md    the comparison with the driver this one replaced
docs/CAR-SESSION.md     how to run a read-only vehicle session
NEWS.md                 release history
CONTRIBUTING.md         how to contribute
NOTICE                  provenance and trademark notes
LICENSE                 GPL-3.0
```

## Safety

Read-only diagnostics are safe. Reflashing is not. A driver that reports a
failed transmit as sent can brick an ECU; this driver reports every transmit
failure, but it cannot protect against a wrong image or a wrong tool.
Programming voltage works as with the vendor driver: an application that asks
for it gets it. Everything in `tools/car/` is read-only by construction. You are
responsible for what you send to your vehicle.

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md). The most useful contribution is
live-bus data from a vehicle this driver has not seen, especially one that
answers on K-line; `PROTOCOL.md` §12 lists what is still open. Report bugs at
https://github.com/bisak/openport-j2534/issues.

## Credits

Maintained by Biser Atanasov. Michał Słomkowski reported the Linux `cdc_acm`
problem fixed in 0.2.1. The third-party implementations whose readings of the
cable were tested against it are listed in `docs/PROTOCOL.md` §14.

## License

GPL-3.0-or-later. Copyright 2026 Biser Atanasov. See [`NOTICE`](NOTICE) for
provenance and trademark notes.
