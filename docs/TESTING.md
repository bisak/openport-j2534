# Testing

Most tests need no hardware. To run them on macOS or Linux:

```bash
make check-all
```

## Commands

| Command | What it runs | Needs |
|---|---|---|
| `make test` | Unit tests and vehicle recordings | nothing |
| `make sanitize`, `make sanitize-thread` | The unit tests under ASAN and UBSAN, and under TSAN | nothing |
| `make fuzz`, `make fuzz-deep` | The reply parser, 300,000 and 4 × 1,500,000 fuzz iterations | nothing |
| `make sim` | Scenarios against a simulated cable | Python 3 |
| `make check-all` | `make test`, both sanitizer runs, `make fuzz`, and `make sim` | Python 3 |
| `make ab-official AB_DLL=…` | Tactrix's DLL and this driver, side by side, on the simulator | Apple Silicon Mac, Docker, mingw-w64, the DLL |
| `make ab-official-cable AB_DLL=… CABLE=…` | The same on the cable | the above, and the cable |
| `make differential OLD_DRIVER=…` | This driver and the one it replaced | macOS. The full run adds `DIFF_ARGS=--hardware` and the cable. |
| `./examples/op_smoke` | Open, read versions and voltages, close (after `make smoke`) | the cable |
| `tools/car/car-session.sh` | A read-only vehicle session | a vehicle |

[The Windows build](#the-windows-build) lists the commands for the Windows DLLs.

To run one scenario, or the simulator alone:

```bash
# on Linux: OPENPORT_LIB=$PWD/libj2534.so first
python3 tests/sim/run_scenarios.py s_multiframe
# prints the pty to point OPENPORT_DEVICE at
python3 tests/sim/openport_sim.py --verbose
```

## The layers

| Layer | What only this layer catches |
|---|---|
| 1. Sanitizers | Data races, undefined behaviour, and memory errors |
| 2. Fuzzing | Malformed, truncated, and out-of-step input from the cable |
| 3. Unit tests and vehicle recordings | What each call returns, and the frame sequences real vehicles sent |
| 4. Simulator | Successful transmits, replies, reassembly, and faults on demand |
| 5. Comparison with other implementations ([DIFFERENTIAL.md](DIFFERENTIAL.md)) | Differences from the J2534-1 text |
| 6. Tactrix's DLL, side by side ([AB-OFFICIAL.md](AB-OFFICIAL.md)) | What Tactrix's own driver sends and accepts |
| 7. The replaced driver, side by side ([DIFFERENTIAL.md](DIFFERENTIAL.md)) | Behaviour on real hardware |
| 8. Bench ECU ([PROTOCOL.md §10](PROTOCOL.md)) | A real second node and real timing |
| 9. Vehicle ([CAR-SESSION.md](CAR-SESSION.md)) | Confirmation, and the recordings layer 3 replays |

The fuzz target checks that the reply parser, `src/op_proto.c`, never reads
past its buffer or loops without consuming input. Malformed input comes from a
hung cable or a partly read USB packet, never from a vehicle. Apple's clang has
no libFuzzer, so the Makefile uses `tests/fuzz/fuzz_main.c`, a small mutation
driver around the same `LLVMFuzzerTestOneInput` entry point.

### The simulator

CAN needs a second node to acknowledge every frame. With a single cable on a
desk, every transmit fails with `ERR_TIMEOUT`, and no test reaches a reply.
`tests/sim/openport_sim.py` is that second node. It is a simulated OpenPort
2.0 behind a pty, which the real library opens through `OPENPORT_DEVICE`.

Everything the simulator answers was measured on a cable (firmware 1.17.4877)
and is in [PROTOCOL.md](PROTOCOL.md). The source marks what was never observed
as `MODELLED`: the K-line frame layout and init replies, the VW TP2.0 module,
and the configuration sets of channels 3, 5, and 7–9.

These scenarios test faults a working car does not produce:

| Scenario | Checks |
|---|---|
| `s_backlog` | Seven stale replies waiting at open. None may be taken as the answer to a later command. |
| `s_dropped_reply` | The cable drops every fourth reply. No later result may shift by one. |
| `s_no_ack` | No second node. `ERR_TIMEOUT`, and no invented messages. |
| `s_disconnect` | The cable disappears mid-session. The driver fails promptly. |
| `s_truncated` | Frames cut in half. No crash, and no message with an impossible length. |
| `s_garbage` | Noise before every reply. A protocol error, not "device not connected". |

## Continuous integration

`.github/workflows/ci.yml` runs these jobs on every push and before every
release:

- Linux: `make test`, `make sanitize`, `make fuzz`, and `make sim`.
- macOS: `make check-all`. A release does not wait for this job.
- Windows, both widths: the unit tests, the simulator over a named pipe, and
  `registry_check.py`, with DLLs built on Linux as a release builds them.

## The Windows build

| Command | Where | What it checks |
|---|---|---|
| `make CC=i686-w64-mingw32-gcc test` | Wine, or MSYS2 on Windows | The unit tests, built for Windows (`x86_64-w64-mingw32-gcc` for 64-bit) |
| `OPENPORT_LIB=…\openport-j2534-64.dll python tests\sim\run_scenarios.py` | Windows | The simulator behind a named pipe. A 32-bit DLL needs a 32-bit Python. |
| `python tests\windows\registry_check.py <dll>` | Windows, as administrator | `regsvr32`, loading the DLL from the registry, an open, and `regsvr32 /u` leaving the registry as it was |
| `make wine-sim` | macOS or Linux | The DLL under Wine and the native library must give the same results and send the same commands, for both widths. |
| `j2534_trace <dll> out.jsonl stream --load 3 --seconds 30 --capture cap.txt` | a cable on a bus that acknowledges | The cable sends three frames every 1 ms. Compare the count received with another logger's. |
| `j2534_trace <dll> out.jsonl thread_writes --cycles 300` | a cable on a bus that acknowledges | 300 threads in turn each write 50 frames with a `Timeout` of 0 and exit at once. Windows cancels the pending I/O of a thread that exits. With `LOOPBACK` on, every frame must come back, in order. |

### On a Windows PC

This session ran on 2026-11-12 on Windows 10 21H2 (x64, a physical machine).
The cable (firmware 1.17.4877) used Tactrix's driver (`openport2.inf`
1.0.0.4227) on a 500 kbit/s bench CAN bus with an ECU and an independent CAN
logger. Tactrix's DLL (`op20pt32.dll` 1.02.0.4868) ran on the same machine and
bus.

| Run | Result |
|---|---|
| Unit tests, both widths | 655 checks, 0 failures |
| Simulator over a named pipe, 64-bit | 57 passed, 0 failed |
| `registry_check.py` (64-bit) and `regsvr32` by hand (32-bit) | Every check passes. Removal leaves no trace. |
| Against Tactrix's DLL, default scenarios and `sweep` without `sweep_connect` (32-bit) | The same results in 1,368 steps, except the [return codes](AB-OFFICIAL.md#return-codes) |
| 32-bit against 64-bit, default scenarios | the same results |
| `thread_writes --cycles 300`, both widths and Tactrix's DLL | 15,000 of 15,000 frames sent, in order |
| ECU reads, 200 rounds | The same replies and round trips as Tactrix's DLL |
| A second program opening the cable | `ERR_DEVICE_IN_USE`. The first session continues. |
| Soak: a CAN stream, a periodic message, ECU reads, filters, configuration, and a channel opened and closed every 30 s, 5 to 7 minutes per DLL (2026-11-13) | Both widths and Tactrix's DLL alike: no read error, overflow, or backwards timestamp, and memory and handles flat. Both DLLs hit the firmware's periodic-message limit after channel closes ([PROTOCOL.md §10](PROTOCOL.md)) |
| 1,225 read-only requests from recorded GTS+ sessions, replayed through both DLLs | The same results, except where Tactrix's DLL answers a second open with "no devices available" and this driver with `ERR_DEVICE_IN_USE` |
| Bench supply cut for 5 s mid-session | Both DLLs: transmits fail with `ERR_TIMEOUT` while the ECU is off, nothing hangs, and the session continues when power returns |

The next table counts frames lost from the cable's 3 × 1 ms load over 30 s,
on a bus carrying about 3,100 frames a second. "Frozen" suspends the reading
process for 300 ms of every 500 ms, as paging does. "One read waiting" is this
driver before it kept 256 USB reads waiting.

| Condition | Tactrix's DLL | one read waiting | this driver |
|---|---|---|---|
| idle | 0 | 0 | 0 |
| every core busy at normal priority | 0 | 0 | 0 |
| frozen 300 ms of every 500 | 2,538 | 2,517 | 0 |
| every core busy above normal priority, the reading program starved | 19,945 | 9,516 | 12,841, reported as an overflow |

In a second run, the final DLLs of both widths lost no frame frozen, where
Tactrix's DLL lost 2,516. A starved program cannot read, so the cable
discards frames whatever the driver. In about 650,000 frames, the cable's
timestamps never ran backwards.

| Work | Tactrix's DLL | this driver |
|---|---|---|
| 488 configuration commands (`sweep_config`): run, CPU | 1.19–1.31 s, 0.14–0.25 s | 1.04 s, 1.04 s |
| 800 ECU reads: run, CPU | 4.62 s, 0.19–0.22 s | 4.44 s, 0.13 s |
| A transmit with a `Timeout`: median | 1.13 ms | 1.34 ms |

The CPU time on configuration commands is the reader waiting for each
reply. Why the cable confirms a transmit sooner to Tactrix's DLL is not
known.

The session also found:

- The cable can hang, through either DLL, when a channel is opened at
  250 kbit/s on a live 500 kbit/s bus and closed again. Only removing all its
  power, from USB and the vehicle connector, brings it back. See
  [PROTOCOL.md](PROTOCOL.md#the-cable-hangs-after-a-wrong-rate-connect-v).
- `pnputil /restart-device` waits while the cable is open, so software cannot
  detach the cable.

A USB detach mid-session and the path through Windows' own USB serial driver,
`usbser.sys`, were last tested on 2026-11-11 in a virtual machine, with an
earlier version of the Windows transport. To test that path, remove Tactrix's
driver package (`pnputil /delete-driver … /uninstall`). Windows then gives the
cable `usbser.sys` and a COM port.

### Under Wine

Wine hands the DLL a serial port, so a Wine run tests `src/op_win.c` but
neither Windows driver. Wine 9.0 lacks the serial read timeouts the library
sets on Windows, so under Wine the library sets others. Every unit check
passes under Wine. On 2026-11-13, under box64 and Wine on macOS, the final DLLs
gave the same results and commands as the native library.

## On macOS

This session ran on 2026-11-13 on an Apple Silicon Mac, with the same cable and
bus. It compared release 0.4.2 with this release.

| Run | Result |
|---|---|
| `make check-all` | every hardware-free layer passes |
| Default scenarios and `sweep_config sweep_filter sweep_tx` | The same results in 1,370 steps but one. A 4,099-byte ISO 15765 transmit returns `ERR_TIMEOUT`, as with Tactrix's DLL, where 0.4.2 returned `ERR_DEVICE_NOT_CONNECTED`. |
| ECU reads, 200 rounds | the same replies and round trips |
| `thread_writes --cycles 300` | 15,000 of 15,000 frames, in order. 10.1 s against 16.9 s for 0.4.2. |
| Frames lost from the 3 × 1 ms load, idle and frozen | none through either |

Another program can hold the cable's serial device. While UltiMaker Cura ran,
`PassThruOpen` returned `ERR_DEVICE_IN_USE`.

## Not yet tested

- The `usbser.sys` path with the current Windows transport.
- An application other than the test programs, on Windows.
- Any Windows version but 10.
- K-line frames received from an ECU ([PROTOCOL.md §7.9](PROTOCOL.md)). The
  cable's own K-line echoes are tested.
- What the simulator marks as `MODELLED`.

## The bug every layer missed

The driver once treated the first frame of a segmented reply as data. On a
vehicle, that frame carries only the CAN id, so every segmented reply arrived
with the id twice and the application discarded it. Every layer passed,
because the unit fixtures and the simulator came from the same assumption as
the code. Fixtures now come from recordings, and a real application's rule for
accepting messages runs in the suite (`s_consumer_rule`).
