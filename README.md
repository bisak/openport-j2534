# openport-j2534

A J2534 PassThru driver for the Tactrix OpenPort 2.0 cable, for macOS and Linux.

Tactrix ships a driver for Windows only. This one lets J2534 diagnostic and
tuning tools, and your own code, use the same cable on a Mac or a Linux machine:
CAN, ISO 15765 (ISO-TP) and K-line.

It talks to the cable the way Tactrix's own driver does. That has been checked
command by command against Tactrix's DLL, on the cable and against a live ECU
([docs/AB-OFFICIAL.md](docs/AB-OFFICIAL.md)).

## Install

macOS, with Homebrew:

```bash
brew install bisak/tap/openport-j2534
```

From source, on macOS or Linux:

```bash
# macOS:          brew install libusb pkg-config
# Debian/Ubuntu:  sudo apt install libusb-1.0-0-dev pkg-config
git clone https://github.com/bisak/openport-j2534.git
cd openport-j2534
make
make test          # hardware-free test suite, no cable needed
make install       # into /opt/homebrew or /usr/local; set PREFIX=... to change
```

On Linux, allow your user to open the cable once, then re-plug it:

```bash
echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="0403", ATTR{idProduct}=="cc4d", MODE="0666"' \
  | sudo tee /etc/udev/rules.d/70-openport.rules
```

## Check that it works

Remove the cable's microSD card (with a card inserted the cable appears as a
USB disk), plug the cable in, and run:

```bash
op_probe           # should list the cable: USB 0403:cc4d
```

From a source checkout you can also open the cable and read its firmware
version and the battery voltage. This sends nothing on the bus:

```bash
make smoke && ./examples/op_smoke
```

## Use it

The library is `libj2534.dylib` (`libj2534.so` on Linux) and implements the
J2534-1 API, version 04.04: all fourteen `PassThru*` functions. Point your J2534
tool at it, or call it directly.

From C:

```c
#include <j2534/j2534.h>

unsigned long dev, ch;
PassThruOpen(NULL, &dev);
PassThruConnect(dev, ISO15765, 0, 500000, &ch);
/* filters, PassThruWriteMsgs, PassThruReadMsgs ... */
PassThruDisconnect(ch);
PassThruClose(dev);
```

```bash
cc app.c $(pkg-config --cflags --libs openport-j2534) -o app
```

From Python:

```python
import ctypes
j2534 = ctypes.CDLL("/opt/homebrew/lib/libj2534.dylib")  # /usr/local/lib/libj2534.so on Linux
dev = ctypes.c_ulong()
j2534.PassThruOpen(None, ctypes.byref(dev))
```

The header declares J2534's integers as `unsigned long`, as the standard does,
so on 64-bit macOS and Linux they are 8 bytes. Use `ctypes.c_ulong`. A tool
that assumes 32-bit integers (`c_uint32`, Rust `u32`) will not work with this
library.

What each call does on this cable, the IOCTLs, programming voltage, and every
difference from Tactrix's driver: [docs/API.md](docs/API.md).

## What works

| Protocol | Status |
|---|---|
| ISO 15765 (diagnostics over CAN) | Works. Tested on vehicles and against a live ECU. |
| Raw CAN | Works. |
| K-line: ISO 9141, ISO 14230 | Implemented but not yet confirmed: no K-line ECU has answered in testing so far. |
| L-line and 2.5 mm jack channels | Open and configure; no data seen yet. |
| J1850, SCI | Not supported by the cable. |

Tested with cable firmware 1.17.4877 on macOS (Apple Silicon) and on Linux
(Ubuntu 24.04).

## Troubleshooting

When a call fails, `PassThruGetLastError` says why. For more detail, run your
tool with `OPENPORT_LOG=- OPENPORT_LOG_HEX=1`, which prints every byte exchanged
with the cable.

| Symptom | Cause and fix |
|---|---|
| `PassThruOpen` returns 8, "USB mass storage" | A microSD card is in the cable. Remove it and re-plug. |
| `PassThruOpen` returns 8, no cable found | Check the connection with `op_probe`. |
| `PassThruOpen` returns 14 | Another program holds the cable, or on Linux the udev rule is missing. |
| Every transmit returns 9 (`ERR_TIMEOUT`) | Nothing on the bus acknowledged the frame: no ECU connected, or the ignition is off. |
| An ECU ignores ISO 15765 requests | Set the `ISO15765_FRAME_PAD` TxFlag. Many ECUs ignore unpadded frames. |
| A call hangs | That is a bug: every transfer has a time limit. Please report it with the log. |

Do not run your tool with `sudo` to get around an error.

## Environment variables

| Variable | Effect |
|---|---|
| `OPENPORT_LOG=<file>` | Write a diagnostic log; `-` for stderr. |
| `OPENPORT_LOG_HEX=1` | Add every byte sent and received to that log. |
| `OPENPORT_DEVICE=<path>` | Use the cable's serial device (`/dev/cu.usbmodem…`, `/dev/ttyACM0`), or the simulator's, instead of USB. |
| `OPENPORT_RECORD=<file>` | Record every J2534 call, for replay against another driver. |

## Safety

Reading diagnostics is safe. Writing to an ECU is not: this driver reports
every failed transmit, but it cannot protect you from a wrong image or a wrong
tool. Like Tactrix's driver, it applies programming voltage when a tool asks
for it. You are responsible for what you send to your vehicle.

## Documentation

- [docs/API.md](docs/API.md): the J2534 API on this cable, in detail
- [docs/PROTOCOL.md](docs/PROTOCOL.md): the cable's wire protocol, with the evidence for each claim
- [docs/TESTING.md](docs/TESTING.md): how the driver is tested, with and without hardware
- [docs/AB-OFFICIAL.md](docs/AB-OFFICIAL.md): comparison with Tactrix's Windows DLL
- [docs/DIFFERENTIAL.md](docs/DIFFERENTIAL.md): comparison with the driver this one replaced
- [docs/CAR-SESSION.md](docs/CAR-SESSION.md): running a read-only session on a vehicle
- [NEWS.md](NEWS.md): release notes
- [CONTRIBUTING.md](CONTRIBUTING.md): how to help, and the code layout

## License

GPL-3.0-or-later, copyright 2026 Biser Atanasov. Not affiliated with or
endorsed by Tactrix. See [NOTICE](NOTICE).
