# openport-j2534

A J2534 PassThru driver for the Tactrix OpenPort 2.0 cable, for macOS, Linux
and Windows.

Tactrix ships a driver for Windows only. This one lets J2534 diagnostic and
tuning tools, and your own code, use the same cable on a Mac or a Linux machine:
CAN, ISO 15765 (ISO-TP) and K-line. On Windows it is an open-source DLL that
can be installed beside Tactrix's.

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
# hardware-free test suite, no cable needed
make test
# into /opt/homebrew or /usr/local; set PREFIX=... to change
make install
```

On Linux, allow your user to open the cable once, then re-plug it:

```bash
echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="0403", ATTR{idProduct}=="cc4d", MODE="0666"' \
  | sudo tee /etc/udev/rules.d/70-openport.rules
```

### Windows

Download `openport-j2534-<version>-windows.zip` from the
[releases](https://github.com/bisak/openport-j2534/releases), unpack it where
it will stay, and register each DLL from an administrator Command Prompt in
that folder:

```bat
C:\Windows\SysWOW64\regsvr32 win32\openport-j2534-32.dll
C:\Windows\System32\regsvr32 win64\openport-j2534-64.dll
```

J2534 applications then list "OpenPort 2.0 (openport-j2534)", beside Tactrix's
own entry if that is installed: 32-bit applications (most of them) the 32-bit
DLL, 64-bit ones the 64-bit DLL. `regsvr32 /u` removes the entry again. A DLL
needs no other file.

The cable needs a USB driver: Tactrix's, installed with Tactrix's software,
or, with none installed, the USB serial driver Windows 10 and later give the
cable by themselves. `op_probe.exe` shows which one the cable has. Tactrix's
driver is the one tested with this release; the USB serial driver was last
tested with an earlier version of the Windows code.

To build the DLLs yourself, use MinGW-w64, on Windows under MSYS2 or
cross-compiled on macOS or Linux; no libusb is needed:

```bash
# 32-bit, into build/win32
make CC=i686-w64-mingw32-gcc
# 64-bit, into build/win64
make CC=x86_64-w64-mingw32-gcc
```

What was tested on Windows, and how:
[docs/TESTING.md](docs/TESTING.md#the-windows-build).

## Check that it works

Remove the cable's microSD card (with a card inserted the cable appears as a
USB disk), plug the cable in, and run:

```bash
# should list the cable: USB 0403:cc4d
op_probe
```

From a source checkout you can also open the cable and read its firmware
version and the battery voltage. This sends nothing on the bus:

```bash
make smoke && ./examples/op_smoke
```

## Use it

The library is `libj2534.dylib` (`libj2534.so` on Linux,
`openport-j2534-32.dll` and `openport-j2534-64.dll` on Windows) and implements
the J2534-1 API, version 04.04: all fourteen `PassThru*` functions. Point your
J2534 tool at it, or call it directly.

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
# /usr/local/lib/libj2534.so on Linux
j2534 = ctypes.CDLL("/opt/homebrew/lib/libj2534.dylib")
dev = ctypes.c_ulong()
j2534.PassThruOpen(None, ctypes.byref(dev))
```

The header declares J2534's integers as `unsigned long`, as the standard does,
so on 64-bit macOS and Linux they are 8 bytes. Use `ctypes.c_ulong`. A tool
that assumes 32-bit integers (`c_uint32`, Rust `u32`) will not work with this
library there. On Windows they are 4 bytes, as in every Windows J2534 DLL, and
the functions are `__stdcall`: load the 32-bit DLL with `ctypes.WinDLL`.

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
(Ubuntu 24.04), and on Windows 10 (64-bit) with Tactrix's USB driver.

## Troubleshooting

When a call fails, `PassThruGetLastError` says why. For more detail, run your
tool with `OPENPORT_LOG=- OPENPORT_LOG_HEX=1`, which prints every byte exchanged
with the cable.

| Symptom | Cause and fix |
|---|---|
| `PassThruOpen` returns 8, "USB mass storage" | A microSD card is in the cable. Remove it and re-plug. |
| `PassThruOpen` returns 8, no cable found | Check the connection with `op_probe`. |
| `PassThruOpen` returns 14 | Another program holds the cable, or on Linux the udev rule is missing. 3D-printer software that looks for printers on serial ports (UltiMaker Cura, for one) opens the cable's serial device on macOS as soon as it is plugged in, and keeps it; quit it first. On macOS, `lsof /dev/cu.usbmodem*` shows which program has it. |
| On Windows, `PassThruOpen` returns 8, "its driver gives no access" | The cable has a driver that is neither Tactrix's nor the Windows USB serial driver (one installed with Zadig, for example). Run `op_probe.exe`; remove that driver in Device Manager. |
| Every transmit returns 9 (`ERR_TIMEOUT`) | Nothing on the bus acknowledged the frame: no ECU connected, or the ignition is off. |
| An ECU ignores ISO 15765 requests | Set the `ISO15765_FRAME_PAD` TxFlag. Many ECUs ignore unpadded frames. |
| A call hangs | That is a bug: every transfer has a time limit. Please report it with the log. |

Do not run your tool with `sudo` to get around an error.

## Environment variables

| Variable | Effect |
|---|---|
| `OPENPORT_LOG=<file>` | Write a diagnostic log; `-` for stderr. |
| `OPENPORT_LOG_HEX=1` | Add every byte sent and received to that log. |
| `OPENPORT_DEVICE=<path>` | Use this serial device: the cable's (`/dev/cu.usbmodem…`, `/dev/ttyACM0`, `COM7` on Windows) or the simulator's. `usb` uses libusb. Unset, macOS uses the cable's serial device, Linux uses libusb, and Windows uses Tactrix's driver or else the cable's COM port. |
| `OPENPORT_RECORD=<file>` | Record every J2534 call, for replay against another driver. |

On Windows these are read from the process's environment, so a program may set
them itself before `PassThruOpen`.

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
