# Testing

What each test layer checks and how to run it. Most layers need no hardware:
a simulator and recordings from vehicles stand in for the bus, and they can
also misbehave on request, which a working vehicle cannot.

## What a vehicle provides

A one-cable bench lacks one thing a vehicle has: a second CAN node that
acknowledges a frame. A CAN controller cannot acknowledge its own transmission
(the ACK slot must be driven by a different node), so it retransmits and then
goes error-passive. Every transmit on a one-cable bench therefore returns
`ERR_TIMEOUT`, and nothing that follows a successful transmit is reached: no
loopback frame, no ECU reply, no received message, no multi-frame reassembly.

The simulator (layer 4) supplies the acknowledging peer at the protocol level.
The bench ECU of `PROTOCOL.md` §10, a production chassis ECU on a bench
harness, supplies it on a real bus (layer 7).

## The layers

| Layer | Hardware | What only this layer catches |
|---|---|---|
| 1. Sanitizers | none | Data races, undefined behaviour, memory errors |
| 2. Fuzzing | none | Malformed, truncated and desynchronised streams |
| 3. Unit tests over a mock transport | none | Entry-point semantics, error mapping |
| 3a. Golden traces (`test_golden.c`) | none | Frame sequences a vehicle actually sent, judged by a real consumer's rule |
| 4. Protocol simulator | none | Successful transmit, receive, reassembly, fault injection |
| 4a. A real consumer's acceptance rule against the simulator (`s_consumer_rule`) | none | What a consuming application keeps from `PassThruReadMsgs`, end to end |
| 4b. A reflash tool against a scripted bootloader (`--ecu-model`, see `AB-OFFICIAL.md`) | none | The write path: session, unlock, driver upload, erase, page transfers, read-back verify |
| 5. Conformance against other implementations | none | Semantic divergence from the standard |
| 5b. A/B against the vendor's Windows DLL | Docker | What Tactrix's own consumer sends and accepts; the firmware dialect |
| 6. Differential against the prior driver | cable | Behavioural equivalence on real hardware |
| 7. Two-node bench | cable + peer | The ACK peer, real bus timing |
| 8. Vehicle | car | Confirmation on a real vehicle |

Layers 1 to 5 need no hardware and run on macOS and Linux; an Ubuntu container
is enough. `make check-all` runs layers 1 to 4a. Layer 5b needs Docker on an
Apple Silicon Mac; layer 6 needs macOS.

---

## 1. Sanitizers

ASAN and UBSAN:

```bash
make sanitize
```

TSAN:

```bash
make sanitize-thread
```

TSAN found three data races that the 438 unit checks passing at the time did
not, all from `volatile` used where an atomic was required: `volatile`
constrains the compiler but says nothing about memory ordering between threads.
The driver runs a reader thread beside the application's own, so this class of
bug is live and functional tests do not see it.

## 2. Fuzzing

300 000 iterations under ASAN and UBSAN:

```bash
make fuzz
```

1 500 000 iterations with each of four seeds:

```bash
make fuzz-deep
```

`op_proto.c` is pure (no I/O, no globals, no allocation), so it fuzzes at
millions of executions per minute. The target asserts the invariants a caller
depends on: never read past the buffer, always either consume bytes or report
INCOMPLETE (so a reader loop terminates), and keep every returned pointer
inside the input.

A vehicle produces only well-formed frames. A wedged cable, a desynchronised
stream and a partially read USB packet produce the malformed input this layer
explores, which makes it stronger evidence about the parser than driving.

Apple's clang ships no libFuzzer, so `tests/fuzz/fuzz_main.c` is a
deterministic mutation driver that calls the same `LLVMFuzzerTestOneInput`
entry point. The Makefile builds the target with it on every platform, so the
layer runs on any machine; the same target can be linked with libFuzzer where
that is available.

## 3. Unit tests and golden traces

```bash
make test
```

A mock transport is injected through `op_device_set_factory`, so everything
above the byte pipe is the real code. The run prints its number of checks and
failures and needs no cable.

`tests/unit/test_golden.c` transcribes vehicle sessions and cites them: the
2012 VW Caddy's VIN reply, its raw CAN exchange and its 29-bit marker. A fixture
taken from a recording cannot agree with the implementation merely because it
was derived from it.

## 4. The protocol simulator

All scenarios, against the library just built:

```bash
make sim
```

One scenario by name. The runner loads `libj2534.dylib` from the repository
root unless `OPENPORT_LIB` names another library; on Linux, prefix the command
with `OPENPORT_LIB=$PWD/libj2534.so`:

```bash
python3 tests/sim/run_scenarios.py s_multiframe
```

The simulator on its own, printing its pty:

```bash
python3 tests/sim/openport_sim.py --verbose
```

`tests/sim/openport_sim.py` is a simulated OpenPort 2.0 that speaks the wire
protocol behind a pty. `OPENPORT_DEVICE=<pty>` points the shipped library at
it, so the library under test is the real one, loaded as an application loads
it; only the transport differs. Reassembly, reply matching, filters, timeouts
and the entry points are all the real code on the real path.

Fidelity is anchored to measurements. Every command reply in the simulator was
measured on a cable (firmware 1.17.4877) and is recorded in `docs/PROTOCOL.md`,
and so are the receive framing, the 70-byte chunking of long replies and the
CAN/ISO15765 loopback echo (`PROTOCOL.md` §7.3, §7.6, §7.7). What the cable was
never observed doing is marked `MODELLED` in the source: the K-line frame
layout, the K-line init replies, the configuration sets of channels 3, 5 and
7–9, and the VW TP2.0 module.

What the simulator reaches that a one-cable bench cannot:

- a transmit that succeeds, because there is a peer to acknowledge it
- a received message with real payload
- multi-frame reassembly (a 600-byte response arriving as one message)
- `TX_MSG_TYPE` on a loopback echo

What it reaches that a vehicle cannot, because a working vehicle does not
misbehave on demand:

| Scenario | What it checks |
|---|---|
| `s_backlog` | Seven stale replies queued at open, the condition that once made this driver read a firmware version as the answer to a battery query |
| `s_dropped_reply` | The device swallows every fourth reply; no later result may be shifted by one |
| `s_no_ack` | Bench conditions: `ERR_TIMEOUT`, and no fabricated messages |
| `s_disconnect` | The cable vanishes mid-session: the driver fails promptly and never hangs |
| `s_truncated` | Frames cut in half: no crash, no message with an impossible length |
| `s_garbage` | Noise before every reply: reported as a protocol failure, not "device not connected" |

`s_garbage` found a bug: the driver reported `ERR_DEVICE_NOT_CONNECTED` for a
device that was connected but not answering coherently, which sent the user to
check cables instead of the link.

The other scenarios cover the normal path (`s_happy_path`, `s_multiframe`,
`s_loopback`, `s_periodic_keepalive`), the consumer's rule (`s_consumer_rule`),
VW TP2.0 in the capture tool (`s_tp20`, `s_tp20_silent_module`), padding
(`s_frame_pad`, `s_capture_tool_pads`) and the candidate chunk layouts
(`s_chunking_models`).

### Why none of this caught the receive-framing bug, and what changed

The driver shipped treating a START frame as the first chunk of a message's
data. On a vehicle the START frame carries only the CAN id; the data comes
later and repeats the id. Every segmented response was therefore delivered with
the id twice and `START_OF_MESSAGE` set, and the consumer discarded it.

Every layer above passed. Each one missed it for a stated reason:

| Layer | Why it missed it |
|---|---|
| Unit tests | The receive fixtures were written from the same model as the code. A test derived from the implementation's own assumptions confirms the assumptions |
| Simulator | `send_message` was that model in Python, marked `MODELLED` in the docstring but relied on as if measured. The claim that fidelity was anchored held for command replies and never for receive frames |
| Fuzzing, sanitizers | They find crashes and races, not a wrong but self-consistent interpretation |
| Differential | The bench has no bus, so neither driver ever received a frame. The drop-in comparison in `DIFFERENTIAL.md` compared two no-response outcomes |
| `PROTOCOL.md` | §7 stated the frame layout as verified, which it was, and the meaning of the bits in the same table without a separate confidence mark. The one receive-side capture with real data had been in the consumer's own log directory since June and was never consulted |

Three things now make that kind of failure harder:

1. Golden traces come from recordings, not from code. `test_golden.c`
   transcribes a vehicle session and cites it.
2. The real consumer's rule runs. `s_consumer_rule` replicates a consuming
   application's acceptance rule in the repository so that it always runs. The
   consumer's rule, not the driver's view of its own output, decides.
3. The simulator's receive behaviour is marked measured, and what is still
   modelled is named, so a reader can tell which behaviours a scenario actually
   checks. Chunking and the loopback echo, modelled when this was written, have
   since been measured (`PROTOCOL.md` §7.6, §7.7).

A claim about the wire is verified only by a capture from the wire. The project
applied that to command replies before it applied it to frames.

The wider search that followed found the same kind of error a second time: the
K-line frame layout had been assumed uniform with CAN, and the published
implementations (two independent lines of evidence, `PROTOCOL.md` §7.3) and the
prior driver say it is not (`PROTOCOL.md` §7.9). That layout is corrected from
sources and marked [P] until `car_capture.py --kline` records it;
`analyse_capture.py` is written to distinguish the two layouts rather than to
confirm the one implemented.

## 5. Conformance against other implementations

Semantics are cross-checked against open-source J2534 implementations and the
SAE J2534-1 definition, since bug-compatibility with one prior driver is not
the goal. Results and intentional divergences: `docs/DIFFERENTIAL.md`.

## 5b. The vendor's own driver

Both drivers against the simulator:

```bash
make ab-official AB_DLL=/path/to/op20pt32.dll
```

Both drivers against the cable:

```bash
make ab-official-cable AB_DLL=/path/to/op20pt32.dll CABLE=/dev/cu.usbmodemXXXX
```

`op20pt32.dll` is the only consumer of the firmware written by the people who
wrote the firmware. It runs inside a container on an Apple Silicon Mac (box64
and Wine, no Windows), with its device handle spliced onto the simulator's pty
or the cable, and the same scripted sequence drives it and `libj2534.dylib`.
The first run found four things the cable sweeps had not: the command
sequence-number protocol, the `atm` periodic command, the chunk layout the DLL
reassembles, and a `tbi` fallback. The harness can also replay a recording of a
real application (`OPENPORT_RECORD`) through both drivers and compare the wire
command by command, payloads included; that is how a reflash tool's traffic is
checked against the vendor without an ECU. `AB_ARGS="-- sweep"` passes every
connect, configuration, filter, transmit and periodic parameter once through
both drivers and compares each step's wire commands; `-- edge` does the same for
values J2534 forbids. See `docs/AB-OFFICIAL.md`.

## 6. Differential against the prior driver

macOS only, because the tap is a DYLD interposer:

```bash
make differential OLD_DRIVER=/path/to/libj2534.dylib DIFF_ARGS=--hardware
```

See `docs/DIFFERENTIAL.md`.

## 7. Two-node bench

Real bus timing and genuine ECU behaviour need a second CAN node: a vehicle, or
a bench ECU with its own CAN transceiver. The bench ECU of `PROTOCOL.md` §10 is
that node. Against it were measured the firmware periodic messages and
`SNIFF_MODE` (2026-09-16), and the 70-byte chunking of long replies, the
ISO15765 loopback echo and the A/B against a live ECU (2026-09-24,
`AB-OFFICIAL.md`).

## 8. Vehicle

`docs/CAR-SESSION.md` describes a read-only vehicle session. The vehicle
recordings made so far are replayed by the golden traces.

---

## What needs hardware and what does not

Nothing in `make check-all` needs a vehicle or a cable. Several `PROTOCOL.md` claims
were measured on vehicles (§7.3, §7.4, §7.8, §11) and on the bench ECU (§7.6,
§7.7, §10); the golden traces and the simulator carry those measurements, so
`make check-all` checks the driver against them without hardware. What only
hardware can still show is real bus timing and ECU behaviour that no recording
covers yet, K-line above all (`PROTOCOL.md` §12).

## Running everything

Unit, sanitizers, fuzzing and simulator; no cable required:

```bash
make check-all
```
