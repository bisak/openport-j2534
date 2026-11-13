# openport-j2534

An open-source SAE J2534 PassThru driver for the Tactrix OpenPort 2.0 cable, for
macOS, Linux, and Windows.

Tactrix ships a driver for Windows only. This driver lets J2534 diagnostic and
tuning tools, and your own code, use the cable on a Mac or a Linux machine for
CAN, ISO 15765 (ISO-TP), and K-line. On Windows it installs beside Tactrix's
DLL, in 32-bit and 64-bit builds.

It sends the cable the same commands as Tactrix's driver. Side by side with
Tactrix's DLL, on the cable and against a live ECU, it returns the same results
in over 1,400 calls, except for the return codes listed in
[docs/AB-OFFICIAL.md](docs/AB-OFFICIAL.md).

## Install

On macOS, with Homebrew:

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
# the tests that need no cable
make test
# installs into /opt/homebrew or /usr/local; set PREFIX to change it
make install
```

On Linux, give your user access to the cable, then re-plug it:

```bash
echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="0403", ATTR{idProduct}=="cc4d", MODE="0666"' \
  | sudo tee /etc/udev/rules.d/70-openport.rules
```

### Windows

1. Download `openport-j2534-<version>-windows.zip` from the
   [releases](https://github.com/bisak/openport-j2534/releases) and unpack it
   in a folder that will stay.
2. Open a Command Prompt as administrator in that folder and register both DLLs:

   ```bat
   C:\Windows\SysWOW64\regsvr32 win32\openport-j2534-32.dll
   C:\Windows\System32\regsvr32 win64\openport-j2534-64.dll
   ```

J2534 applications then list "OpenPort 2.0 (openport-j2534)" beside Tactrix's
entry. A 32-bit application, which most are, uses the 32-bit DLL. To remove the
entry, run the same command with `/u`.

The cable needs a USB driver. Tactrix's driver, installed with Tactrix's
software, is the one tested with this release. Without it, Windows 10 and later
give the cable a USB serial driver of their own, which this DLL also opens.
`op_probe.exe` shows which driver the cable has.

To build the DLLs, use MinGW-w64 on Windows under MSYS2, or cross-compile on
macOS or Linux. They need no libusb:

```bash
make CC=i686-w64-mingw32-gcc     # 32-bit, into build/win32
make CC=x86_64-w64-mingw32-gcc   # 64-bit, into build/win64
```

## Check the cable

Remove the cable's microSD card first. With a card in it, the cable appears as
a USB disk. Then plug the cable in and run:

```bash
op_probe
```

It lists the cable as USB `0403:cc4d`. From a source checkout, `op_smoke` also
opens the cable and reads its firmware version and the battery voltage. It
sends nothing on the bus:

```bash
make smoke && ./examples/op_smoke
```

## Use the library

The library is `libj2534.dylib` on macOS, `libj2534.so` on Linux, and
`openport-j2534-32.dll` and `openport-j2534-64.dll` on Windows. It implements
all 14 `PassThru*` functions of J2534-1 version 04.04. Point your J2534 tool at
it, or call it directly.

From C:

```c
#include <j2534/j2534.h>

unsigned long dev, ch;
PassThruOpen(NULL, &dev);
PassThruConnect(dev, ISO15765, 0, 500000, &ch);
/* filters, PassThruWriteMsgs, PassThruReadMsgs */
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

J2534 integers are `unsigned long`, as the standard declares them. On 64-bit
macOS and Linux they are 8 bytes, so use `ctypes.c_ulong`. A tool that assumes
32-bit integers, such as one using `c_uint32` or Rust's `u32`, fails there. On
Windows they are 4 bytes and the functions are `__stdcall`, so load the DLL
with `ctypes.WinDLL`.

[docs/API.md](docs/API.md) describes each call on this cable, the IOCTLs,
programming voltage, and every difference from Tactrix's driver.

## Protocol support

| Protocol | Status |
|---|---|
| ISO 15765 (diagnostics over CAN) | Works. Tested on vehicles and against a live ECU. |
| Raw CAN | Works. |
| ISO 9141 and ISO 14230 (K-line) | Implemented. No K-line ECU has answered in testing yet. |
| L-line and the 2.5 mm jack | Open and configure. No data seen yet. |
| J1850 and SCI | The cable does not support them. |

Tested with cable firmware 1.17.4877 on macOS (Apple Silicon), Linux (Ubuntu
24.04), and Windows 10 (64-bit) with Tactrix's USB driver.

## Troubleshooting

When a call fails, `PassThruGetLastError` says why. For every byte sent to and
from the cable, run your tool with `OPENPORT_LOG=- OPENPORT_LOG_HEX=1`.

| Symptom | Cause and fix |
|---|---|
| `PassThruOpen` returns 8, "USB mass storage" | A microSD card is in the cable. Remove it and re-plug the cable. |
| `PassThruOpen` returns 8, no cable found | Run `op_probe` to check the connection. |
| `PassThruOpen` returns 14 | Another program holds the cable. On Linux, the udev rule may be missing. On macOS, `lsof /dev/cu.usbmodem*` names the program. 3D-printer software that scans serial ports, such as UltiMaker Cura, takes the cable as soon as it is plugged in. |
| On Windows, `PassThruOpen` returns 8, "its driver gives no access" | The cable has a driver that is neither Tactrix's nor the Windows USB serial driver, such as one installed with Zadig. Remove that driver in Device Manager. |
| Every transmit returns 9 (`ERR_TIMEOUT`) | No node on the bus acknowledged the frame. The ECU is not connected or the ignition is off. |
| An ECU ignores ISO 15765 requests | Set the `ISO15765_FRAME_PAD` TxFlag. Many ECUs ignore unpadded frames. |
| A call hangs | Every transfer has a time limit, so a hang is a bug. Report it with the log. |

Do not run your tool with `sudo` to get around an error.

## Environment variables

| Variable | Effect |
|---|---|
| `OPENPORT_LOG=<file>` | Writes a diagnostic log. `-` writes to stderr. |
| `OPENPORT_LOG_HEX=1` | Adds every byte sent and received to the log. |
| `OPENPORT_DEVICE=<path>` | Opens this serial device: the cable's (`/dev/cu.usbmodem…`, `/dev/ttyACM0`, or `COM7`) or the simulator's. `usb` selects libusb. When unset, macOS uses the cable's serial device, Linux uses libusb, and Windows uses Tactrix's driver or else the cable's COM port. |
| `OPENPORT_RECORD=<file>` | Records every J2534 call, for replay against another driver. |

On Windows the DLL reads these from the process environment, so a program can
set them before it calls `PassThruOpen`.

## Safety

Reading diagnostics is safe. Writing to an ECU is not. This driver reports
every failed transmit, but it cannot protect you from a wrong image or a wrong
tool. Like Tactrix's driver, it applies programming voltage when a tool asks for
it. You are responsible for what you send to your vehicle.

## Documentation

- [docs/API.md](docs/API.md): the J2534 API on this cable
- [docs/PROTOCOL.md](docs/PROTOCOL.md): the cable's USB protocol, with the evidence for each claim
- [docs/TESTING.md](docs/TESTING.md): how the driver is tested, and the results
- [docs/AB-OFFICIAL.md](docs/AB-OFFICIAL.md): the comparison with Tactrix's DLL
- [docs/CAR-SESSION.md](docs/CAR-SESSION.md): how to run a read-only session on a vehicle
- [docs/DIFFERENTIAL.md](docs/DIFFERENTIAL.md): the comparison with the macOS driver this one replaced
- [NEWS.md](NEWS.md): release notes
- [CONTRIBUTING.md](CONTRIBUTING.md): how to contribute, and the code layout

## License

GPL-3.0-or-later, copyright 2026 Biser Atanasov. Not affiliated with or
endorsed by Tactrix. See [NOTICE](NOTICE).
