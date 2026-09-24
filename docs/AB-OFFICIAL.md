# Testing against Tactrix's own driver

Tactrix's Windows driver, `op20pt32.dll`, is the one program written against the
cable's firmware by the people who wrote the firmware. This harness runs it on
an Apple Silicon Mac, under Wine in a Docker container, against the same
simulator or cable as this library. It drives both through the same calls and
compares what each returned and what each sent on the wire.

## Results

Against the bench cable on 2026-09-24 (DLL 1.02.0.4868, firmware 1.17.4877),
after the fixes below:

| Run | Steps | Identical on the wire |
|---|---|---|
| `sweep`: every J2534 parameter, with values J2534 allows | 670 | 648 |
| `edge`: values J2534 does not allow | 35 | 23 |

Every remaining difference is listed under
[Where the two drivers differ](#where-the-two-drivers-differ), each with its
reason. Against a live ECU (below), both drivers sent the same commands and
delivered every reply byte for byte the same.

Bugs this harness found in this driver, all fixed in 0.3.0:

- **`CLEAR_MSG_FILTERS` could leave a filter installed.** The driver tracked
  filter ids in a 32-bit mask at bit `id & 31`, but the cable never reuses a
  filter id within a session: ids count up across all channels and restart only
  at `ata`/`atz`. Filter 32 shared filter 0's bit, so once filter 32 was stopped,
  `CLEAR_MSG_FILTERS` sent nothing, returned success, and left filter 0 in
  place. Reproduced on the cable with 33 filter starts. The driver now sends
  Tactrix's single `atk<ch> -1` and keeps no filter table; `CLEAR_PERIODIC_MSGS`
  likewise sends `atl<ch>`.
- **ISO 15765 loopback echoes were dropped.** The firmware reports them on raw
  CAN channel 5 ([PROTOCOL.md §7.7](PROTOCOL.md)), and the driver discarded them
  because channel 5 was not open.
- **Configuration lists stopped at the first unsupported parameter**, leaving
  the rest unset. Tactrix's DLL sends every parameter and returns the last one's
  status; so does this driver now.

Earlier runs found the missing sequence numbers, the transmit budget in
`att`, the five-baud command, and periodic messages in the firmware.

## Running it

You need an Apple Silicon Mac (the container is native arm64), Docker, the
mingw-w64 cross compiler, and a copy of `op20pt32.dll`.

```bash
brew install mingw-w64
# against the simulator
make ab-official AB_DLL=/path/to/op20pt32.dll
# against the cable
make ab-official-cable AB_DLL=/path/to/op20pt32.dll CABLE=/dev/cu.usbmodemXXXX
```

`AB_ARGS` is passed to `ab.sh`. Scenario names go after `--`:

| Scenario | Covers |
|---|---|
| `open`, `iso15765`, `multiframe`, `timeouts`, `errors`, `periodic`, `raw_can` | the default set |
| `kline` | K-line connect and init |
| `bench` | N write/read cycles, for timing (`--cycles 50`) |
| `sweep_connect`, `sweep_config`, `sweep_filter`, `sweep_tx`, or `sweep` for all four | every J2534 parameter, one at a time |
| `edge` | values J2534 does not allow; run it on its own, last |

```bash
make ab-official AB_DLL=/path/to/op20pt32.dll AB_ARGS="-- open bench --cycles 50"
make ab-official-cable AB_DLL=/path/to/op20pt32.dll CABLE=/dev/cu.usbmodemXXXX AB_ARGS="-- sweep"
make ab-official-cable AB_DLL=/path/to/op20pt32.dll CABLE=/dev/cu.usbmodemXXXX AB_ARGS="-- edge"
```

Output goes to `tools/ab-official/out/`:

| File | Contents |
|---|---|
| `report.md` | the comparison, from `ab_diff.py` |
| `vendor.jsonl`, `ours.jsonl` | one line per call: arguments, return code, error text, outputs, microseconds |
| `vendor.tap`, `ours.tap` | every byte in both directions, timestamped |
| simulator logs | what the simulator received and answered |

## Where the two drivers differ

The goal is to behave like Tactrix's driver. This one differs only where J2534
names a different return code, or where passing a request on would let the
cable do something wrong that the application could not detect.

### Return codes

| Case | Tactrix's DLL | This driver |
|---|---|---|
| `PassThruDisconnect(9)` before any open | `ERR_INVALID_CHANNEL_ID` (2) | `ERR_INVALID_DEVICE_ID` (26), J2534-1 §7.2.1 |
| `PassThruIoctl(deviceId, 0xDEAD)` | `ERR_INVALID_CHANNEL_ID` (2) | `ERR_INVALID_IOCTL_ID` (15) |
| `PassThruStartMsgFilter` with a NULL mask | `ERR_FAILED` (7) | `ERR_NULL_PARAMETER` (4) |
| Mask and pattern of different lengths | `ERR_FAILED` | `ERR_INVALID_MSG` |
| NULL pattern; flow-control filter with no flow-control message | `ERR_FAILED` | `ERR_NULL_PARAMETER` |
| `FIVE_BAUD_INIT` with zero bytes | `ERR_INVALID_MSG` | `ERR_NULL_PARAMETER` |
| Flow-control message with different TxFlags from mask and pattern | `ERR_FAILED`, nothing sent | installed with the flow-control message's flags |
| A transmit the cable times out (`are 9`) | `ERR_TIMEOUT`, `NumMsgs` left as the caller set it (3 for a three-message write that sent one) | `ERR_TIMEOUT`, `NumMsgs` 0: the number sent, per J2534-1 |
| `CLEAR_FUNCT_MSG_LOOKUP_TABLE` | 0, nothing sent | `ERR_NOT_SUPPORTED` |
| `FIVE_BAUD_INIT` answered with a bare `aro` (the simulator's modelled reply) | 0 | `ERR_INIT_FAILED`: no key bytes means no init |
| `READ_PROG_VOLTAGE` with `pInput` NULL, as J2534-1 passes it | -1, nothing sent | reads pin 12 |
| `PassThruReadMsgs(timeout=0)` on an idle channel | returns after ~6 ms | returns in microseconds |

### Requests refused before they reach the cable

| Case | Tactrix's DLL | This driver |
|---|---|---|
| CAN at rate 0; an ISO 15765 write of 4100 bytes; a 3-byte CAN periodic | sent; the cable accepts all three (the 4100-byte message is attempted and times out; the malformed periodic repeats) | `ERR_INVALID_BAUDRATE`, `ERR_INVALID_MSG` |
| CAN writes of 0, 3 or 13 bytes; ISO 15765 writes of 3 bytes, or 4 with extended addressing | sent; the cable answers `are 10` | refused, same return code |
| Filter types 0 and 4 | sent; the cable answers `are 22` | `ERR_INVALID_MSG` |
| J1850 connect; an eleventh periodic | sent; `are 3`, `are 12` | refused, same return code (an eleventh filter is sent, as by Tactrix) |
| SCI and other ids with no firmware channel | `ato0`, `are 3`, `ERR_INVALID_PROTOCOL_ID` | same code, nothing sent |
| A second programming-voltage pin | `atv`, 0 | `ERR_PIN_INVALID` |
| Grounding K (`atv 7 -2`) under an open ISO 9141 channel | sent, 0; the channel stops working | `ERR_CHANNEL_IN_USE` |
| Connecting ISO 14230 while K is grounded | 0 | `ERR_CHANNEL_IN_USE` |

### Crashes

| Case | Tactrix's DLL | This driver |
|---|---|---|
| `GET_CONFIG` or `SET_CONFIG` with `pInput` NULL | the process dies (read fault at address 0) | `ERR_NULL_PARAMETER` |
| `FAST_INIT` with `pInput` NULL (J2534: wake-up pattern only) | the process dies (write fault at 0x11) | sends `aty<ch> 0 0` |

The sweep skips these three so that the DLL's process survives.

### Routing

| Case | Tactrix's DLL | This driver |
|---|---|---|
| A message whose `ProtocolID` differs from its channel's | picks the firmware channel from the message: CAN on an ISO 15765 channel goes out as `att5`; ISO 15765 on a CAN channel returns `ERR_INVALID_PROTOCOL_ID` | sends on the channel it was written to |
| Raw CAN and ISO 15765 channels both open | every channel-5 frame goes to the ISO 15765 channel, none to the CAN channel | channel-5 frames stay on the CAN channel |
| `LOOPBACK` on ISO 15765: order of echo and TxDone | TxDone first | wire order (the timestamps show the true order) |

### Wire differences that follow from these

- The firmware numbers filters and periodic messages in sequence, so one extra
  on either side shifts the id every later `atk`/`atn` names. Periodic ids keep
  counting across sessions while the cable is powered.
- Multi-message transmit budgets differ by the time already spent.
- Open and close differ: this driver sends `\r\n\r\n`, `atz`, `ata`, `ati` at
  open, where Tactrix's sends `\r\n\r\n`, `ati`, `ata`.
- A `Timeout=0` write is numbered by this driver, so its late reply can be
  recognised and dropped; Tactrix's sends it without a number.

## Where they agree

- Every configuration parameter on ISO 15765, CAN, ISO 14230 and ISO 9141 goes
  out as the same `atg`/`ats` with the same value. Neither converts units:
  `P1_MAX` 40 is `ats4 7 40` from both.
- Every connect flag and rate both accept, every J2534-2 channel id,
  `TX_PARAM_STOP_BITS`, `FAST_INIT` on the L line, and `SNIFF_MODE`.
- Every transmit flag: 29-bit, extended addressing, `ISO15765_FRAME_PAD`,
  `SW_CAN_HV_TX`, `WAIT_P3_MIN_ONLY`.
- The 4099-byte ISO 15765 transmit; 29-bit and extended-addressing flow-control
  filters; K-line filters.
- `GET_CONFIG`/`SET_CONFIG` lists: every parameter sent, the last one's status
  returned (`[3, 127]` gives `ERR_NOT_SUPPORTED`, `[127, 3]` gives 0).
- A flow-control message given with a PASS or BLOCK filter is ignored. J2534-1
  §7.2.9.2 calls it an error, but refusing it would fail an application that
  passes a zeroed message rather than NULL.
- Periodic intervals outside J2534's 5–65535 ms are passed on; the cable accepts
  4 ms and 65,536 ms.
- `CLEAR_MSG_FILTERS`, `CLEAR_PERIODIC_MSGS`, and `READ_PROG_VOLTAGE` with a pin
  in `pInput` (`atr <pin>`).
- Raw CAN frames, their loopback echo, the TxDone indication, and multi-frame
  delivery (a 4-byte `ISO15765_FIRST_FRAME` indication, then the whole message,
  as J2534-1 Table 72 asks).
- Every timeout code and every `ERR_CHANNEL_IN_USE`, `ERR_INVALID_FILTER_ID`,
  `ERR_INVALID_MSG_ID` and `ERR_NOT_SUPPORTED` in the `errors` scenario.

## What Tactrix's DLL sends

Read off `vendor.tap` (DLL 1.02.0.4868, firmware simulated as 1.17.4877). This
is the DLL's behaviour under emulation. Where it implies something about the
firmware, the cable runs confirm it. [PROTOCOL.md §4](PROTOCOL.md) has the
command list.

**Sequence numbers.** Every command carries a number and the DLL waits for the
reply that echoes it: `ata 2`, `atr 16 3`, `ato6 0 500000 0 4`, `ats6 30 0 5`,
`atf6 3 64 4 6`, `att6 6 64 1000000 7`, `atk6 0 8`, `atc6 9`, `atz 10`.
Numbering starts at 2, after a bare `ati` preceded by `\r\n\r\n`. It accepts
`aro <n>`, `arr 16 12480 <n>`, `arf6 0 <n>` and so on, waiting 500 ms for most
replies and 5 s for `READ_VBATT` and `GET_CONFIG`, then returns `ERR_TIMEOUT`.
The trailing field earlier notes saw as a constant 0 is this echo; it read 0
because this driver sent no number then.

**The transmit budget.** `att`'s fourth argument is the caller's `WriteMsgs`
`Timeout` in microseconds, with 1,000,000 substituted for 0. It looked constant
in the scenarios, which pass 1000 ms or 0; a replayed reflash tool, passing
5000 ms for a driver-window transfer, sent `5000000`. It is the firmware's
budget for getting the message onto the bus. Without it, the firmware's ~1 s
default would cut short a 257-byte transfer paced by a slow ECU. This driver
sends the same.

**`Timeout=0` writes** go out without a number, and the DLL does not wait:
`att6 6 64 1000000` against `att6 6 64 1000000 7` for `Timeout=1000`. It returns
0 with one message sent, as J2534 asks. On the cable an unnumbered `att` gets no
reply at all, not even `are 9` on a dead bus, and while the firmware keeps
trying, the next command (`atc6`) goes unanswered for about a second, so the
DLL's `PassThruDisconnect` straight after returns `ERR_TIMEOUT`. Numbered, the
same transmit answers `are 9 <n>` after 1.0 s. This driver numbers it and does
not wait, so the late reply is recognised and dropped.

**Other commands.**

- `CLEAR_RX_BUFFER` sends nothing; it clears the DLL's own queue. Same here.
- `PassThruStartPeriodicMsg` at 100 ms sends `atm6 100000 0 64 6 <n>` plus the
  payload: interval in microseconds, 0, TxFlags, length. The cable answers
  `arm6 0 <n>`, then `arm6 1 <n>` for the next. `PassThruStopPeriodicMsg` sends
  `atn6 <id> <n>`. A running periodic message produces no transmit indication:
  none in 550 ms on a live bus. This driver sends the same commands.
- `SetProgrammingVoltage(pin, VOLTAGE_OFF)` is `atv 12 -1`, printed signed. Same
  here; the firmware accepts either form.
- Open is `ati`, `ata`; close is `atz`. `PassThruReadVersion` answers from the
  `ati` reply kept from open. `READ_VBATT` is `atr 16 <n>`.
- If `ati` gets no reply, the DLL sends `tbi\n` (LF only) three times, 300 ms
  apart, on a second handle, reading up to 256 bytes each time, then returns
  `ERR_DEVICE_NOT_CONNECTED`. The cable does not answer `tbi` with either line
  ending; it is probably a bootloader query.
- Five-baud init is `atw<ch> <address>`, the address in decimal and nothing
  after it (`atw3 51` for 0x33). An earlier reading, `atw3 1` plus the address
  as a raw byte, makes the firmware initialise address 1 and then take the byte
  for the next command, which wedges the following disconnect. This driver
  sends Tactrix's form.
- ISO 9141 opens with a trailing value that varies between runs (3 on the
  bench, 5 on the car): the DLL's handle of the ISO 14230 channel it had just
  closed. The firmware ignores it and this driver does not copy it.

**Long replies.** Fed a 600-byte ISO 15765 reply with the CAN id only in the
first chunk, the DLL delivered 598 bytes. With the id at the start of every
chunk, it delivered all 606 bytes intact. So Tactrix reads the firmware as
repeating the id in every chunk, which is what the cable does
([PROTOCOL.md §7.6](PROTOCOL.md)). Both drivers strip it.

## Against the cable

`ab.sh --cable /dev/cu.usbmodemXXXX` serves the cable's serial device on
`127.0.0.1:5455` with `ttybridge.py`. The container reaches it as
`host.docker.internal:5455` and turns it back into a pty, and the DLL drives the
real cable. This driver then runs natively on the same device. With no ECU,
transmits fail with `ERR_TIMEOUT` on both sides for want of a second node.

Measured on 2026-09-13: every reply echoes the sequence number
(`arr 16 108 3`, `arf6 0 8`, `arg6 30 0 10`, `are 1 9`); `atm` answers
`arm6 <id> <n>` and `atn` stops it; `tbi` gets no reply with either line ending;
`atv 12 -1` is accepted. All four are in PROTOCOL.md and modelled in the
simulator.

The `edge` scenario sends the cable input nothing had tried before: Tactrix's
DLL passes all of it on. None of it writes flash or switches a voltage, so the
realistic worst case is a cable that stops answering until it is unplugged.
**Run `edge` on its own and last, with nothing on the OBD connector.** If the
cable wedges, the tap's last command names the culprit, and no further
`PassThruOpen` follows, because the DLL's reopen of a silent cable sends `tbi`,
whose answered path has never been seen. On 2026-09-24 the cable answered every
`edge` command and kept working.

The image bakes `iface.reg` into its Wine registry. An image built from an older
`iface.reg` registers a device the container never links, and the DLL answers
"no devices available" without sending a byte. `ab.sh` labels the image with a
hash of `Dockerfile` and `iface.reg` and rebuilds when either changes.

## Against a live ECU

The bench ECU of [PROTOCOL.md §10](PROTOCOL.md) supplies what a lone cable
lacks: a second node, real replies, a real bus. On 2026-09-24, with 12.1 V on
pin 16 checked before and after, a replay drove both drivers through the same
read-only sequence:

- 300 ms of the module's own broadcast on raw CAN;
- four identification and status reads on ISO 15765, needing no session
  (12–22 bytes, all segmented);
- one read with `LOOPBACK` on;
- a firmware periodic TesterPresent for 3 s.

Both drivers sent identical commands and delivered every reply identically: the
TxDone indication with the CAN id, the first-frame indication, and the
reassembled message, with the same `RxStatus` and `ExtraDataIndex`. The one
difference, the dropped loopback echoes, is fixed (see Results).

**Multi-frame transmit**, the path a reflash depends on, which nothing had
exercised on real hardware before: requests of 10, 20 and 100 bytes (a first
frame and up to 14 consecutive frames). The module reads each in full and then
refuses it on length, as its binary says it does for any request of that service
longer than two bytes. Its flow control asked for no block limit and no
separation time. Both drivers put the same 190 payload bytes on the wire in the
same commands, every request got its negative response, and a status read before
and after returned the same bytes.

The probe files for this module live with that module's own tooling, not here.

## On the Audi

A 2009 Audi 1.9 TDI (EDC16), 2026-09-13.

**CAN.** Both drivers delivered identical raw frames: the `$01 00` request on
0x7DF and the engine's reply on 0x7E8 (`06 41 00 98 3b 80 19`), with the same
status bits and sizes, and TxDone indications of the same shape.

**K-line.** Both drivers failed every init identically, also after re-seating
the OBD plug: fast init to 0x33 and 0x01, and five-baud to 0x33, 0x01, 0x10,
0x17, 0x19, 0x25, 0x08 and 0x46, all `ERR_FAILED`. The line is electrically
present: a fast init takes 126–132 ms on the car for either driver (wake-up
pulse, request, wait) against 78 ms on the bench with nothing on pin 7.

The open-source EDC16 K-line flasher `fjvva/ecu-tool` does a five-baud init to
0x01, the keyword handshake, then StartCommunication `81 10 F1 81` to 0x10
expecting `83 F1 10 C1 EF 8F`, retrying the five-baud init up to six times
because the ECU often ignores the first. Replicating that from the cable (eight
five-baud attempts at 0x01 on both K-line channels, W1 raised to 1000 ms, 9600
and 10400 baud, six fast inits to 0x10) produced no sync byte.

So either OBD pin 7 on this car is not on a live K-line (the owner's Galletto
may have been used at the ECU connector or in boot mode), or the cable's
five-baud waveform is not what this ECU accepts. A multimeter on pin 7 (12 V at
idle means a pulled-up K-line) and a bit-banged init from an FTDI cable would
tell the two apart. The driver is not the cause: Tactrix's failed identically.

## Replaying an application through both drivers

The scenarios are this project's idea of what an application does. A recording
is the application itself.

- `OPENPORT_RECORD=<file>` makes this driver append one line per call, with
  every input and the ids each call returned.
- `j2534_trace --replay` runs that file against any J2534 library, mapping the
  recorded ids onto the ones the library under test hands back.
- `ab.sh --replay` runs one recording through both drivers against identical
  simulators and compares the wire command by command, payloads included. Only
  the open/close handshake, known to differ, is set aside.

A recording is plain text, one call per line, so a probe can be written by hand.
`ioctl <dev> 14 <pin>` passes a pin through `pInput` to `READ_PROG_VOLTAGE`,
which is how the DLL was shown to take one.

```bash
tools/ab-official/record.sh out/app.rec --sim-args "--ecu-model model.py:Ecu:image.bin" -- python3 my_tool.py --lib libj2534.dylib
AB_DLL=/path/to/op20pt32.dll tools/ab-official/ab.sh --replay out/app.rec --sim-args "--ecu-model model.py:Ecu:image.bin"
```

`--ecu-model FILE.py:CLASS[:IMAGE]` puts a scripted ECU behind the simulator.
`CLASS(image_bytes_or_None, log=...)` is constructed once, and the instance
answers each request (`request_bytes -> response_bytes | None`). This is how a
reflash tool's write path (programming session, seed/key, driver upload, erase,
page transfers, read-back) is rehearsed against a model of the target
bootloader, which no car allows. The model for an ECU lives with that ECU's
tooling.

Rehearsed against a 512 KB bootloader model, one reflash tool's recording was
21,497 calls: one session, 5,882 transmits (driver windows, 1,920 image pages,
and every `$23` read of the verify) and 15,605 reads. Replayed through
Tactrix's DLL, it showed that for the exact requests the tool sends, both
drivers put the same bytes on the wire.

## The parameter sweep

Every difference found by the scenarios came from one parameter translated
differently, and the scenarios pass only what a typical session uses. The
`sweep` scenarios pass each of the rest once:

- connect flags and rates, and every J2534 protocol id;
- every configuration parameter on four channels: read, set to its J2534-1
  default, read back;
- filter shapes and flags;
- transmit flags and sizes;
- periodic limits, list and K-line init variants.

The sweep passes only values J2534 allows. `edge` passes the rest: CAN at rate
0, filter types 0 and 4, an eleventh filter, CAN frames of 0, 3 and 13 bytes,
ISO 15765 frames of 3 and 4100 bytes (and 4 with extended addressing),
periodics every 4 ms and every 65,536 ms or 3 bytes long, and a 260-byte K-line
frame.

Each step starts with a marker call, `READ_PROG_VOLTAGE` with pin 17 in
`pInput`, which both drivers send as `atr 17`. `ab_diff.py` cuts each wire
capture at the markers and compares every step's commands, payloads included
(report section 2c). Transmits get a 100 ms budget so a lone cable fails them
quickly, and carry only read-only requests (`$22`, `$3E`, OBD mode 1), so a run
with the cable on a car changes nothing in the car.

The simulator's counts differ slightly from the cable's, because the simulator
acknowledges transmits and a lone cable cannot.

## Timing

The DLL runs interpreted, so its CPU-bound work is tens of times slower than
native, and per-call latencies cannot be compared. What can be compared are the
waits the DLL chooses, which are wall-clock: the 500 ms and 5 s reply timeouts,
the 300 ms `tbi` retry, the ~6 ms polling granularity of `ReadMsgs`, and
`ReadMsgs(100)` / `ReadMsgs(500)` returning at 107 / 508 ms, against this
driver's 99.5 / 501 ms.

This driver's own numbers are native: `PassThruOpen` about 2 ms,
`PassThruClose` about 11 ms, against the DLL's 24 and 6 ms for the same wire
work. An earlier open that drained the pipe of stale replies took 92 ms;
numbering commands made the drain unnecessary.

`bench` runs N write/read cycles; `ab_diff.py` prints medians. On the cable,
the DLL's emulation overhead sits on top of real USB latency, so treat its
timings as upper bounds.

## How the DLL runs without Windows

The DLL exists unpacked only inside a running process. On Windows it reaches the
cable through `openport.sys`, a 20 KB KMDF bulk read/write driver built from the
WDK sample, so everything it says to the cable is `ReadFile`/`WriteFile` on that
pipe. On macOS the cable is a CDC-ACM tty and the simulator is a pty. So the job
is to run the DLL under Wine and splice its device handle onto a pty. Four
things had to be established.

**1. An emulation stack that runs the packed DLL** (2026-09-13, Apple M4 Max).
Three failed:

| Stack | Failure |
|---|---|
| Wine for macOS | its WoW64 layer faults inside its own exception dispatch when the protector raises the privileged-instruction exceptions it uses for control flow |
| Docker amd64 under Rosetta | Rosetta aborts on Wine's signal-return path |
| Docker i386 under qemu-user | Wine's mmap-alignment assertion |

What works is a native arm64 container with box64 translating an x86-64 Wine
11.17 wow64 build, and only with `BOX64_DYNAREC=0`: the dynarec mis-executes the
protector's self-modifying code and the DLL exits silently during
`LoadLibrary`. The interpreter runs a whole session in about five seconds; the
DLL is I/O bound. The `Dockerfile` pins all of it.

**2. How the DLL finds the cable.** `WINEDEBUG=+relay` showed it:
`SetupDiGetClassDevsW` on interface class
`{6d1781b7-c987-4f6c-8d4f-1efc098bea67}` with `DIGCF_PRESENT`, then
`SetupDiEnumDeviceInterfaces`, then `CreateFileW` on the returned symbolic link.
Wine answers SetupDi from the registry, so `iface.reg` registers one present
interface. Wine's setupapi builds its link as
`\\?\usb#vid_0403&pid_cc4d#op20ab001#{guid}`, and Wine's ntdll resolves a plain
`\??\name` through `$WINEPREFIX/dosdevices/name`. A symlink there to the pty is
the whole splice.

**3. Read semantics.** The DLL issues one overlapped `ReadFile` of 8191 bytes
from a reader thread and expects it to complete when a USB packet arrives. Wine
treats the pty as a serial port with zero `COMMTIMEOUTS`, so that read waits for
all 8191 bytes and the version reply never arrives (the DLL then sends `tbi`
three times and gives up). Wine completes an overlapped serial read on the first
byte, and never with zero bytes, when only `ReadIntervalTimeout=MAXDWORD` is
set. `wineshim.c` patches the export-table entry of `kernel32!CreateFileW` in
the harness process before the DLL loads, and sets those timeouts on any handle
opened on the device path. No instruction is patched, and the shim does nothing
on real Windows.

**4. The simulator had to echo sequence numbers.** Without the echo, the DLL
reported "no devices available" after a correct `ari` reply.

`container-run.sh` is the half that runs in the container. `ab.sh` builds both
harness binaries (the same `j2534_trace.c`, compiled with `i686-w64-mingw32-gcc`
and with `cc`), runs the container, runs the native side, and compares. The
native side reaches the simulator or cable through its serial device
(`OPENPORT_DEVICE`), with `ptytap.py` recording the bytes.
