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
| `./examples/op_smoke` | Open, read versions and voltages, close (after `make smoke`) | the cable |
| `tools/car/car-session.sh` | A read-only vehicle session | a vehicle |

To run one simulator scenario, or the simulator on its own:

```bash
python3 tests/sim/run_scenarios.py s_multiframe   # on Linux: OPENPORT_LIB=$PWD/libj2534.so first
python3 tests/sim/openport_sim.py --verbose       # prints the pty to point OPENPORT_DEVICE at
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
layout was corrected from them and stays marked unconfirmed until a K-line
recording exists.
