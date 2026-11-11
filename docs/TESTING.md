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

Tested two ways: on Windows 10 in a virtual machine with the cable passed
through, and under Wine, which runs the same DLL on the build machine and is
what `make` can repeat.

| Command | What it checks |
|---|---|
| `make CC=i686-w64-mingw32-gcc test` | The unit tests of layer 3, compiled for Windows and run under Wine. Everything above the byte pipe is the same code as elsewhere, but `unsigned long` is 4 bytes there. This found a transmit timeout above 2^31 ms read as negative, and the sequence-number seed overflowing. |
| `make wine-sim` | `tests/windows/run_wine.sh` for both widths: the DLL under Wine and the native library run `tools/ab-official/j2534_trace.c`'s scenarios against the simulator, and every step's result and every command on the wire must be the same. This runs `src/op_win.c`, which the unit tests replace with the mock. |
| `tests/windows/run_wine.sh 32 --cable /dev/ttyACM0` | The same on a cable. Three things differ from run to run there and are reported as differences: the battery voltage, the periodic message ids (the cable keeps counting them while powered), and which frames a live ECU happened to broadcast during a raw CAN read. |

`run_wine.sh` takes scenario names after `--`, as `make ab-official` does:
`-- sweep`, `-- kline edge`, `-- bench --cycles 200`.

### On Windows

Run on 2026-11-11 on Windows 10 Pro 22H2 (64-bit) in VirtualBox 7.0.16, the
cable (firmware 1.17.4877) passed through and wired to the bench ECU. The same
programs were run with each of the two USB drivers the cable can have, 32-bit
and 64-bit alike:

| Run | Tactrix's driver | Windows' `usbser.sys` |
|---|---|---|
| `op_probe.exe` | the device interface | "USB Serial Device (COM3)" |
| `run_tests.exe` (needs no cable) | 654 checks, 0 failures | the same |
| `op_smoke.exe` | all checks pass | all checks pass |
| `op_iso15765.exe` on the bench ECU | single-frame and multi-frame replies, raw CAN; 0 failures | the same |
| `j2534_trace.exe`, default scenarios | 67 steps | 67 steps, every return code as with Tactrix's driver |
| `j2534_trace.exe bench --cycles 100` | 100 of 100 answered; a write takes 1.3 ms (median) | 100 of 100; 1.3 ms |
| Cable detached from the machine mid-session | the waiting `PassThruReadMsgs` returns `ERR_DEVICE_NOT_CONNECTED` at once, and so does every later call; a new `PassThruOpen` reports no cable | the same, the waiting read after 0.27 s |
| A second program opening the cable | `ERR_DEVICE_IN_USE`; the first program's session is not disturbed | not run |
| `regsvr32`, `regsvr32 /u` | each DLL's key appears in, and leaves, its own registry view | |

Removing Tactrix's driver package (`pnputil /delete-driver … /uninstall`) was
enough for Windows to give the cable `usbser.sys` and a COM port, which the
library found by USB id.

The first run found one fault: a log or recording file was only written when
the program ended, because Windows' C library has no line buffering. Both are
now flushed after every line.

Two things from that session to know about:

- The cable stopped answering once, 11 seconds into a session through
  Tactrix's driver: writes were accepted and nothing came back, in that
  program, in the next, and from the Linux host after the cable was handed
  back. A USB port reset did not clear it. The cause is not established; the
  cable had been attached to and detached from the virtual machine several
  times, and test programs had been killed mid-session (next item). After a
  replug it answered again, and a further detach and reattach did not bring
  the fault back.
- Windows Defender ends a test program that is started by typing its command
  into the Run dialog (`Behavior:Win32/SuspClickFix`), about a second in and
  with exit code 0. Start them from a console or by double-clicking a script.

Still open on Windows:

- a J2534 application loading the registered DLL;
- any Windows version other than 10.

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
