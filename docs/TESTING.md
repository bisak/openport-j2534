# Testing

Most of the test suite needs no hardware. This runs all of it, on macOS or
Linux:

```bash
make check-all
```

## Commands

| Command | What it runs | Needs |
|---|---|---|
| `make test` | Unit tests and vehicle recordings | nothing |
| `make sanitize` / `make sanitize-thread` | The unit tests under ASAN and UBSAN / under TSAN | nothing |
| `make fuzz` / `make fuzz-deep` | The reply parser, 300,000 / 4 × 1,500,000 fuzz iterations | nothing |
| `make sim` | Scenarios against a simulated cable | Python 3 |
| `make check-all` | `make test`, both sanitizer runs, `make fuzz` and `make sim` | nothing |
| `make ab-official AB_DLL=…` | Tactrix's DLL and this driver, side by side, on the simulator | Apple Silicon Mac, Docker, mingw-w64, the DLL |
| `make ab-official-cable AB_DLL=… CABLE=…` | The same on the cable | the above, and the cable |
| `make differential OLD_DRIVER=…` | This driver and the one it replaced | macOS; add `DIFF_ARGS=--hardware` and the cable for the full run |
| `make CC=i686-w64-mingw32-gcc test` | The unit tests, built for Windows and run under Wine (`x86_64-w64-mingw32-gcc` for 64-bit) | MinGW-w64, Wine |
| `make wine-sim` | The Windows DLLs and the native library, side by side, on the simulator | MinGW-w64, Wine, Python 3 |
| `./examples/op_smoke` | Open, read versions and voltages, close (after `make smoke`) | the cable |
| `tools/car/car-session.sh` | A read-only vehicle session | a vehicle |

To run one simulator scenario, or the simulator on its own:

```bash
# on Linux: OPENPORT_LIB=$PWD/libj2534.so first
python3 tests/sim/run_scenarios.py s_multiframe
# prints the pty to point OPENPORT_DEVICE at
python3 tests/sim/openport_sim.py --verbose
```

## Why a simulator

A single cable on a desk cannot test much. CAN needs a second node to
acknowledge every frame; without one, every transmit fails with
`ERR_TIMEOUT`, and nothing that follows a successful transmit can be reached:
no reply, no received message, no multi-frame reassembly.

The simulator supplies that second node at the protocol level, and more: it can
also misbehave on request, which a working car cannot. For real bus timing,
the bench ECU of [PROTOCOL.md §10](PROTOCOL.md) plays the second node.

## The layers

| Layer | Needs | What only this layer catches |
|---|---|---|
| 1. Sanitizers | nothing | Data races, undefined behaviour, memory errors |
| 2. Fuzzing | nothing | Malformed, truncated and out-of-step input from the cable |
| 3. Unit tests and vehicle recordings | nothing | What each call returns; frame sequences real vehicles sent |
| 4. Simulator | nothing | Successful transmits, replies, reassembly, faults on demand |
| 5. Comparison with other implementations | nothing | Differences from the standard |
| 6. Tactrix's DLL, side by side | Docker | What Tactrix's own driver sends and accepts |
| 7. The replaced driver, side by side | the cable | Behaviour on real hardware |
| 8. Bench ECU | cable and ECU | A real second node and real timing |
| 9. Vehicle | a vehicle | Confirmation |

### 1. Sanitizers

TSAN found three data races that the 438 unit checks passing at the time did
not. All three came from `volatile` used where an atomic was needed: `volatile`
restrains the compiler but says nothing about memory ordering between threads.
The driver runs a reader thread beside the application's own threads, so this
kind of bug is real, and functional tests do not see it.

### 2. Fuzzing

The reply parser, `src/op_proto.c`, has no I/O, globals or allocation, so it
runs millions of inputs a minute. The fuzz target checks that it never reads
past its buffer, that every call either consumes bytes or reports that it needs
more (so a reader loop cannot spin), and that every pointer it returns stays
inside the input.

A vehicle only sends well-formed frames. A wedged cable, a stream that has lost
its place, or a partly read USB packet produce the malformed input this layer
explores.

Apple's clang ships without libFuzzer, so `tests/fuzz/fuzz_main.c` is a small
deterministic mutation driver around the same `LLVMFuzzerTestOneInput` entry
point; the Makefile uses it everywhere. The target can also be linked with
libFuzzer where that is available.

### 3. Unit tests and vehicle recordings

A mock transport replaces the cable (`op_device_set_factory`), so everything
above the byte pipe is the real code. `tests/unit/test_golden.c` replays
exchanges recorded on a 2012 VW Caddy: a VIN reply (with the VIN replaced), a
raw CAN exchange and a 29-bit frame. A fixture taken from a recording cannot
agree with the code merely because it was written from the code.

### 4. The simulator

`tests/sim/openport_sim.py` is a simulated OpenPort 2.0 behind a pty. Setting
`OPENPORT_DEVICE` to that pty points the real library at it, loaded as an
application loads it; only the transport differs.

Everything it answers was measured on a cable (firmware 1.17.4877) and is
written down in [PROTOCOL.md](PROTOCOL.md): command replies, receive framing,
the 70-byte chunking of long replies, the loopback echo. What was never
observed is marked `MODELLED` in the source: the K-line frame layout and init
replies, the configuration sets of channels 3, 5 and 7–9, and the VW TP2.0
module.

Scenarios a car cannot provide:

| Scenario | Checks |
|---|---|
| `s_backlog` | Seven stale replies waiting at open. This once made the driver report a firmware version as a battery voltage. |
| `s_dropped_reply` | The cable drops every fourth reply; no later result may shift by one. |
| `s_no_ack` | No second node: `ERR_TIMEOUT`, and no invented messages. |
| `s_disconnect` | The cable disappears mid-session: the driver fails promptly and does not hang. |
| `s_truncated` | Frames cut in half: no crash, no message with an impossible length. |
| `s_garbage` | Noise before every reply: reported as a protocol error, not as "device not connected". |

The rest cover the normal path (`s_happy_path`, `s_multiframe`, `s_loopback`,
`s_periodic_keepalive`), a real application's rule for accepting messages
(`s_consumer_rule`), TP2.0 in the capture tool (`s_tp20`,
`s_tp20_silent_module`), padding (`s_frame_pad`, `s_capture_tool_pads`) and
the candidate chunk layouts (`s_chunking_models`).

### 5. Comparison with other implementations

Behaviour is checked against the J2534-1 text and against open-source J2534
implementations, since matching one earlier driver bug for bug is not the goal.
Results: [DIFFERENTIAL.md](DIFFERENTIAL.md).

### 6. Tactrix's DLL, side by side

`op20pt32.dll` is the one program written against the firmware by the people
who wrote the firmware. The harness runs it under Wine in a container on an
Apple Silicon Mac, drives it and this library through the same sequence, and
compares what each returned and sent. It can also replay a recording of a real
application through both, and sweep every J2534 parameter. See
[AB-OFFICIAL.md](AB-OFFICIAL.md).

### 7. The replaced driver, side by side

`make differential` runs the same sequence through this driver and the one it
replaced, and records the USB traffic of both. macOS only: the traffic recorder
is a `DYLD_INSERT_LIBRARIES` interposer. See [DIFFERENTIAL.md](DIFFERENTIAL.md).

### 8. Bench ECU

A production ECU on a bench harness acknowledges frames and answers requests,
with real timing. Measured against it: firmware periodic messages and
`SNIFF_MODE` (2026-09-16); long-reply chunking, the ISO 15765 loopback echo,
multi-frame transmits and a side-by-side run of both drivers (2026-09-24).

### 9. Vehicle

[CAR-SESSION.md](CAR-SESSION.md) describes a read-only session. Its recordings
become layer 3's fixtures.

## The Windows build

Tested four ways: on a Windows PC with the cable through Tactrix's driver, in
CI on every push, in a virtual machine with each of the cable's two USB
drivers, and under Wine.

| Command | Where | What it checks |
|---|---|---|
| `make CC=i686-w64-mingw32-gcc test` | Wine, or MSYS2 on Windows | The unit tests of layer 3, compiled for Windows (`x86_64-w64-mingw32-gcc` for 64-bit). Everything above the byte pipe is the code that runs elsewhere, but `unsigned long` is 4 bytes there. |
| `OPENPORT_LIB=…\openport-j2534-64.dll python tests\sim\run_scenarios.py` | Windows | Layer 4 with the simulator behind a named pipe, which the DLL opens as it opens Tactrix's driver: a handle that is not a serial port, read with 256 overlapped reads. A 32-bit DLL needs a 32-bit Python. The scenarios that drive `tools/car` need a tty and are skipped. |
| `python tests\windows\registry_check.py <dll>` | Windows, as administrator | `regsvr32`, the key read back from the registry view an application of the DLL's width reads, the DLL loaded from its `FunctionLibrary`, an open (or `ERR_DEVICE_NOT_CONNECTED` with no cable), and `regsvr32 /u` leaving the registry as it was. |
| `make wine-sim` | macOS or Linux | `tests/windows/run_wine.sh` for both widths: the DLL under Wine and the native library run `j2534_trace.c`'s scenarios against the simulator, and every step's result and every command on the wire must be the same. Wine hands the DLL the pty as a COM port. |
| `j2534_trace <dll> out.jsonl stream --load 3 --seconds 30 --capture cap.txt` | a cable on a bus that acknowledges | The cable sends three frames every 1 ms and reports each one as received; the counts are every frame the library delivered. Compare them with another logger on the same bus. |
| `j2534_trace <dll> out.jsonl thread_writes --cycles 300` | a cable on a bus that acknowledges | 300 threads in turn each write 50 frames with a `Timeout` of 0 and exit at once; with `LOOPBACK` on, every frame must come back, in order. Windows cancels a thread's pending I/O when the thread exits. |

CI (`.github/workflows/ci.yml`) runs the first three on a Windows machine for
both widths, from DLLs built on Linux exactly as a release builds them.

### On a Windows PC

Run on 2026-11-12 on Windows 10 21H2 (x64, a physical machine), the cable
(firmware 1.17.4877) through Tactrix's driver (`openport2.inf` 1.0.0.4227),
wired to a bench CAN bus at 500 kbit/s with an ECU and a second, independent
CAN logger on it. Tactrix's DLL (`op20pt32.dll` 1.02.0.4868) ran on the same
machine and bus for comparison. The DLLs were built on Ubuntu 24.04, as CI
builds them.

| Run | Result |
|---|---|
| Unit tests, both widths | 655 checks, 0 failures |
| Simulator over a named pipe, 64-bit | 57 passed, 0 failed, a 20,000-frame burst included |
| Both DLLs against the simulator, `j2534_trace` default scenarios | 67 steps, the same as the native library's on macOS |
| `registry_check.py`, 64-bit; `regsvr32` by hand, 32-bit | every check passes; each key only in its own view, with Tactrix's protocol entries; removed without a trace |
| Against Tactrix's DLL, default scenarios and `sweep` (32-bit) | the same results in over 1,450 steps but for the return codes listed in [AB-OFFICIAL.md](AB-OFFICIAL.md#return-codes); the final DLL again in 1,368 steps (`sweep_connect` left out) |
| 32-bit against 64-bit, default scenarios | the same results |
| `thread_writes --cycles 300`, both widths and Tactrix's DLL | 15,000 of 15,000 frames sent, in order, no write failed |
| ECU reads (`22` of four identifiers, one a multi-frame reply), 200 rounds | the same replies as Tactrix's DLL; a round trip of 3.99 ms against 3.98, 10.02 ms against 10.03 |
| A second program opening the cable | `ERR_DEVICE_IN_USE`; the first program's session is undisturbed; a held cable gives Tactrix's DLL "no devices available" |

Frames lost from the cable's own 3 × 1 ms load, about 3,100 frames a second
with the ECU's, over 30 s: the second logger's count of the load frames on the
bus, less the count each DLL delivered. "Frozen" suspends every thread of the
reading process for 300 ms of every 500 ms, as paging does.

| Condition | Tactrix's DLL | one read waiting | this driver |
|---|---|---|---|
| idle | 0 | 0 | 0 |
| every core busy at normal priority | 0 | 0 | 0 |
| frozen 300 ms of every 500 | 2,538 | 2,517 | 0 |
| every core busy above normal priority, the reading program starved | 19,945 | 9,516 | 12,841, reported to the program as an overflow |

The final DLLs, 32-bit and 64-bit, lost no frame frozen; Tactrix's DLL lost
2,516 in the same run. Held to one core, the final 32-bit DLL lost none frozen
or with every core busy; Tactrix's DLL lost 2,534 frozen. A starved program cannot read, so the cable discards
frames whatever the driver; how many varies from run to run. The cable's
timestamps never ran backwards in about 650,000 frames read through 256
waiting reads, so the driver completes them in order.

Speed and CPU, the same work through both DLLs:

| Work | Tactrix's DLL | this driver |
|---|---|---|
| 488 configuration commands (`sweep_config`): median, run, CPU | 0.79–0.93 ms; 1.19–1.31 s; 0.14–0.25 s | 0.85 ms; 1.04 s; 1.04 s |
| The same, the program held to one core | 0.84 ms; 1.27 s; 0.16 s | 0.79 ms; 0.98 s; 0.97 s |
| 800 ECU reads: run, CPU | 4.62 s; 0.19–0.22 s | 4.44 s; 0.13 s |
| A transmit with a `Timeout` (`bench`), until `PassThruWriteMsgs` returns: median | 1.13 ms | 1.34 ms |
| 5,000 frames as 100 writes of 50 with a `Timeout` of 0: time in `PassThruWriteMsgs` | 3.40 s | 3.45 s (5.07 s while those transmits were numbered) |

The CPU on configuration commands is the reader watching for each reply for up
to 1.5 ms (`src/op_win.c`); between commands it costs nothing.

A transmit returns when the cable answers `aro`, with both DLLs: in the
emulator, delaying that answer by 100 ms delays Tactrix's DLL by as much. Here
the cable reports the frame sent 0.87 ms after the command and answers `aro`
0.4 ms later. Neither the number of reads waiting (1 to 256) nor their size
(256 bytes to 8 KB) changes that, and why the answer comes sooner to Tactrix's
DLL is not known. An ECU's reply is read as soon as it arrives, so the round
trips above are the same.

What the session found:

- A command cut off at the caller's timeout left the cable reading the next
  command as the rest of it ([PROTOCOL.md](PROTOCOL.md#2-framing-v)). Commands
  are now sent whole.
- An application may call `PassThruWriteMsgs` with a `Timeout` of 0 from a
  thread that then exits, and Windows cancels a thread's pending I/O when it
  exits. A version that returned with the USB write still pending sent 7,349
  of 15,000 frames from 300 such threads, out of order, and failed 150 writes
  with `ERR_DEVICE_NOT_CONNECTED`. A write now returns only once the cable has
  the whole command; all 15,000 arrive in order, as with Tactrix's DLL.
- A transmit sent with a number makes the cable answer it, which costs about
  0.4 ms before it takes the next command. Tactrix's DLL sends a `Timeout=0`
  transmit unnumbered, and so does this driver now.
- DLLs built on Ubuntu link `msvcrt.dll`, whose copy of the environment is made
  when the DLL loads, so a variable the host program set afterwards (Python's
  `os.environ`) went unseen. `OPENPORT_*` are now read from the process.
- The cable can hang, through either DLL, when a channel is opened at
  250 kbit/s on a live 500 kbit/s bus and closed again: once in six rounds
  through this driver and once in three through Tactrix's DLL; see
  [PROTOCOL.md](PROTOCOL.md#the-cable-hangs-after-a-wrong-rate-connect-v).
  Only removing all its power, USB and the vehicle connector's, brings it back.
- `pnputil /restart-device` waits while the cable is open, so a detach cannot
  be made in software; the virtual machine's detach below stands.

### The same changes on macOS

Run on 2026-11-13 on macOS (Apple Silicon), the same cable and bus, the cable
through its serial device, comparing release 0.4.2 with these changes:

| Run | Result |
|---|---|
| `make check-all` | every hardware-free layer passes |
| Default scenarios and `sweep_config sweep_filter sweep_tx` | the same results in 1,370 steps but one: the 4,099-byte ISO 15765 transmit returns `ERR_TIMEOUT`, as with Tactrix's DLL, where 0.4.2 returned `ERR_DEVICE_NOT_CONNECTED` |
| ECU reads, 200 rounds | the same replies and round trips (3.8 ms, 9.9 ms) |
| `thread_writes --cycles 300` | 15,000 of 15,000 frames, in order, through both; 10.1 s against 16.9 s, the transmits now unnumbered |
| A transmit with a `Timeout` (`bench`): median | 1.29 ms against 1.33 ms |
| Frames lost from the 3 × 1 ms load over 30 s, by the second logger, idle and frozen 300 ms of every 500 | none through either, and no timestamp ran backwards |

UltiMaker Cura, left running, opened the cable's serial device the moment the
cable was plugged in and kept it, so `PassThruOpen` returned `ERR_DEVICE_IN_USE`
until Cura quit.

### In a virtual machine

Run on 2026-11-11 on Windows 10 Pro 22H2 (64-bit) in VirtualBox 7.0.16, the
cable passed through and wired to a bench ECU, with the first version of the
Windows transport (one read waiting). The same programs ran with each of the
two USB drivers the cable can have, 32-bit and 64-bit alike:

| Run | Tactrix's driver | Windows' `usbser.sys` |
|---|---|---|
| `op_probe.exe` | the device interface | "USB Serial Device (COM3)" |
| `op_smoke.exe`, `op_iso15765.exe` on the bench ECU | all checks pass | all checks pass |
| `j2534_trace.exe`, default scenarios | 67 steps | 67 steps, every return code as with Tactrix's driver |
| `j2534_trace.exe bench --cycles 100` | 100 of 100 answered; a write takes 1.3 ms (median) | 100 of 100; 1.3 ms |
| Cable detached mid-session | the waiting `PassThruReadMsgs` returns `ERR_DEVICE_NOT_CONNECTED` at once, and so does every later call; a new `PassThruOpen` reports no cable | the same, the waiting read after 0.27 s |

Removing Tactrix's driver package (`pnputil /delete-driver … /uninstall`) was
enough for Windows to give the cable `usbser.sys` and a COM port, which the
library found by USB id. Windows Defender ends a test program started by typing
its command into the Run dialog (`Behavior:Win32/SuspClickFix`); start them
from a console.

Still open on Windows: the COM-port path since the transport changed, an
application other than the test programs, and any Windows version but 10.

### Under Wine

Run on 2026-11-11 (Wine 9.0, Ubuntu 24.04, MinGW-w64 GCC), 32-bit and 64-bit
alike unless noted. Wine hands the DLL the simulator's pty or `/dev/ttyACM0` as
a serial port, so this exercises `src/op_win.c` but neither Windows driver.

| Run | Result |
|---|---|
| Unit tests | 654 checks, 0 failures |
| Simulator, default scenarios | 67 steps, 0 differ, wire identical |
| Simulator, `sweep` | 679 steps, 0 differ, wire identical |
| Simulator, `kline edge` (32-bit) | 53 steps, 0 differ, wire identical |
| Cable and bench ECU, default scenarios | 67 steps; only the battery voltage, the periodic ids and the ECU's broadcast frames differ |
| Cable and bench ECU, `bench --cycles 200` (32-bit) | 407 steps, 0 differ, wire identical |
| Simulator killed mid-session (32-bit) | every later call fails with `ERR_DEVICE_NOT_CONNECTED` within 0.6 s |
| Opening through Tactrix's interface class | found and opened with `tools/ab-official/iface.reg` imported and the interface linked to the cable |

Wine 9.0 does not implement the serial read timeouts the library sets on
Windows, so under Wine it sets another combination (`src/op_win.c`).

The final DLLs ran again on 2026-11-13, under box64 and Wine on macOS against
the simulator as a COM port: 67 steps each width, the same results and the same
commands on the wire as the native library.

## The bug every layer missed

The driver once shipped treating the first frame of a segmented reply as data.
On a vehicle that frame carries only the CAN id; the data follows in later
frames that repeat the id. Every segmented reply was delivered with the id
twice, marked as a first frame, and the application discarded it. Every layer
passed:

- the unit tests' receive fixtures were written from the same assumption as the
  code, so they confirmed it;
- the simulator implemented that same assumption, marked as modelled but relied
  on as if measured;
- sanitizers and fuzzing find crashes, not a consistent misreading;
- the side-by-side run had no bus, so neither driver received anything;
- the protocol notes marked the frame layout as verified, which it was, but not
  the meaning of its bits, which was a guess. A recording that would have shown
  it had existed since June.

Three changes followed. Fixtures now come from recordings, not from the code. A
real application's acceptance rule runs in the suite (`s_consumer_rule`). And
the simulator names what it models, so a reader can tell which behaviour a
scenario really checks.

The same review found the K-line frame layout had been assumed to match CAN.
Two independent sources ([PROTOCOL.md §7.9](PROTOCOL.md)) say it does not; the
layout was corrected from them. The cable's own K-line echoes have since been
recorded on the bench and follow it; frames received from an ECU stay
unconfirmed until a K-line ECU answers.
