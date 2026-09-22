# Testing without a vehicle

The goal is not to approximate on-car testing. It is to be **better** than it,
because a working vehicle is the weakest test target available: unrepeatable,
untimed, and incapable of misbehaving on request.

## What a car actually provides

Exactly one thing a bench does not: **a second CAN node that acknowledges a
frame**. A lone CAN controller cannot acknowledge its own transmission — the
ACK slot must be driven by a different node — so it retransmits, then goes
error-passive. That is why every transmit on a one-cable bench returns
`ERR_TIMEOUT`, and it cascades: no successful transmit means no loopback
frame, no ECU reply, no received message, no multi-frame reassembly.

Everything else a car offers, a bench does better. So the strategy is: replace
the ACK peer, and beat the car everywhere else.

## The layers

| Layer | Hardware | What only this layer catches |
|---|---|---|
| 1. Sanitizers | none | Data races, UB, memory errors |
| 2. Fuzzing | none | Malformed, truncated and desynchronised streams |
| 3. Unit tests over a mock transport | none | Entry-point semantics, error mapping |
| 3a. Golden traces (`test_golden.c`) | none | Frame sequences a vehicle actually sent, judged by a real consumer's rule |
| 4. Protocol simulator | none | Successful transmit, receive, reassembly, fault injection |
| 4a. A real consumer's acceptance rule against the simulator (`s_consumer_rule`) | none | What a consuming application keeps from `PassThruReadMsgs`, end to end |
| 4b. A reflash tool against a scripted bootloader (`--ecu-model`, see `AB-OFFICIAL.md`) | none | The write path: session, unlock, driver upload, erase, page transfers, read-back verify |
| 5. Conformance vs known-good implementations | none | Semantic divergence from the standard |
| 5b. A/B against the vendor's Windows DLL | Docker | What Tactrix's own consumer sends and accepts; the firmware dialect |
| 6. Differential vs the prior driver | cable | Behavioural equivalence on real hardware |
| 7. Two-node bench | cable + peer | The ACK peer, real bus timing |
| 8. Vehicle | car | Confirmation only |

Layers 1–5 need no hardware whatsoever, and run on macOS and Linux alike; an
Ubuntu container is enough (`make check-all`).

---

## 1. Sanitizers

```bash
make sanitize        # ASAN + UBSAN
make sanitize-thread # TSAN
```

This is not box-ticking. TSAN found **three data races that 438 passing unit
tests did not**, all from using `volatile` where an atomic was required:
`volatile` constrains the compiler but says nothing about inter-thread memory
ordering. The driver has a reader thread beside the application's own, so
this class of bug is live and invisible to functional testing.

## 2. Fuzzing

```bash
make fuzz            # 300k iterations, ASAN + UBSAN
make fuzz-deep       # 1.5M iterations across four seeds
```

`op_proto.c` is pure — no I/O, no globals, no allocation — so it fuzzes at
millions of executions per minute. The target asserts the invariants a caller
depends on: never read past the buffer, always either consume bytes or report
INCOMPLETE (so a reader loop terminates), and keep every returned pointer
inside the input.

A vehicle only ever produces well-formed frames. A wedged cable, a
desynchronised stream and a partially-read USB packet produce exactly what this
explores, which is why it is stronger evidence about the parser than driving.

Apple's clang ships no libFuzzer, so `tests/fuzz/fuzz_main.c` provides a
deterministic mutation driver calling the same `LLVMFuzzerTestOneInput` entry
point. libFuzzer is used where available; the target is identical either way. A
fuzzing layer that only runs on machines with Homebrew LLVM is a fuzzing layer
that does not run.

## 3. Unit tests

```bash
make test            # unit tests and golden traces, no cable
```

A mock transport is injected through `op_device_set_factory`, so everything
above the byte pipe is the real code.

## 4. The protocol simulator — the centrepiece

```bash
make sim             # scenario checks, no cable
python3 tests/sim/run_scenarios.py s_multiframe   # one scenario
python3 tests/sim/openport_sim.py --verbose       # standalone, prints its pty
```

`tests/sim/openport_sim.py` is a simulated OpenPort 2.0 speaking the wire
protocol behind a pty. `OPENPORT_DEVICE=<pty>` points the **shipped dylib** at
it, so the library under test is the real one, loaded as an application loads
it; only the transport differs. Reassembly, reply matching, filters, timeouts
and the entry points are all the real code on the real path.

**Fidelity is anchored, not assumed.** Every command response in the simulator
was measured against a real cable (firmware 1.17.4877) and is recorded in
`docs/PROTOCOL.md`. Anything the simulator does that the cable was never
observed doing is marked `MODELLED` in its source.

What the simulator reaches that a one-cable bench cannot:

- a transmit that succeeds, because there is a peer to acknowledge it
- a received message with real payload
- multi-frame reassembly (a 600-byte response arriving as one message)
- `TX_MSG_TYPE` on a loopback echo

What it reaches that a **vehicle** cannot, because a working car never
misbehaves on demand:

| Scenario | What it proves |
|---|---|
| `s_backlog` | Seven stale replies queued at open — the real condition that once made this driver read a firmware version as the answer to a battery query |
| `s_dropped_reply` | The device swallows every 4th reply; no later result may be shifted by one |
| `s_no_ack` | Bench conditions: `ERR_TIMEOUT`, and **no fabricated messages** |
| `s_disconnect` | Cable vanishes mid-session: fails promptly, never hangs |
| `s_truncated` | Frames cut in half: no crash, no message with an impossible length |
| `s_garbage` | Noise before every reply: reports a *protocol* failure, not "device not connected" |

The last one is a bug this layer found: the driver reported
`ERR_DEVICE_NOT_CONNECTED` for a device that was plainly connected but not
answering coherently — sending the user to check cables instead of the link.

### Why none of this caught the receive-framing bug, and what changed

The driver shipped treating a START frame as the first chunk of a message's
data. On a vehicle the START frame carries only the CAN id; the data comes
later and repeats the id. Every segmented response was therefore delivered
with the id twice and `START_OF_MESSAGE` set, and the consumer discarded it.

Every layer above passed. Each one failed to see it for a stated reason:

| Layer | Why it was blind |
|---|---|
| Unit tests | The receive fixtures were written from the same model as the code. A test derived from the implementation's own assumptions confirms the assumptions |
| Simulator | `send_message` was that model in Python, marked `MODELLED` in the docstring but relied on as if measured. The check "fidelity is anchored" was true of command replies and never of receive frames |
| Fuzzing, sanitizers | They find crashes and races, not a wrong but self-consistent interpretation |
| Differential | The bench has no bus, so neither driver ever received a frame. The "drop-in replacement" run in `DIFFERENTIAL.md` compared two no-response outcomes |
| `PROTOCOL.md` | §7 stated the frame layout as verified, which it was, and the *meaning* of the bits in the same table without a separate confidence mark. The one receive-side capture with real data existed in the consumer's own log directory since June and was never consulted |

Three things now make that failure class structurally harder:

1. **Golden traces come from recordings, not from code.** `test_golden.c`
   transcribes a vehicle session and cites the file. A fixture that cannot be
   derived from the implementation cannot agree with it by construction.
2. **The real consumer's rule runs.** `s_consumer_rule` replicates a
   consuming application's acceptance rule in-repo so it always runs. The
   consumer's rule, not the driver's view of its own output, is what decides.
3. **The simulator's receive framing is now marked measured, with the
   unmeasured parts named** (chunking above one wire frame, echo shape), so a
   reader can tell which behaviours a scenario actually proves.

The general lesson is the one this project already knew for command replies
and had not applied to frames: a claim about the wire is verified only by a
capture from the wire.

The wider search that followed found the same class of error a second time:
the K-line frame layout had been assumed uniform with CAN, and three
independent implementations plus the prior driver say it is not
(`PROTOCOL.md` §7). That one is corrected from sources and marked [P] until
`car_capture.py --kline` records it; `analyse_capture.py` is written to
distinguish the two layouts rather than to confirm the one implemented.

## 5. Conformance against known-good implementations

Semantics are cross-checked against open-source J2534 implementations and the
SAE J2534-1 definition, since bug-compatibility with one prior driver is not
the goal. Results and intentional divergences: `docs/DIFFERENTIAL.md`.

## 5b. The vendor's own driver, run here

```bash
make ab-official                                   # both against the simulator
make ab-official-cable CABLE=/dev/cu.usbmodemXXXX  # both against the cable
```

`op20pt32.dll` is the only consumer of the firmware written by the people who
wrote the firmware. It runs on this Mac inside a container (box64 + Wine, no
Windows), with its device handle spliced onto the simulator's pty or the
cable, and the same scripted sequence drives it and `libj2534.dylib`. The
first run found four things in one afternoon that months of cable sweeps had
not: the command sequence-number protocol, the `atm` periodic command, the
chunk layout of long replies, and a `tbi` fallback nobody knew existed. It can
also replay a recording of a real application (`OPENPORT_RECORD`) through both
drivers and compare the wire command by command, payloads included, which is
how the reflash tool's exact traffic is checked against the vendor without an
ECU. See `docs/AB-OFFICIAL.md`.

## 6. Differential against the prior driver

```bash
make differential DIFF_ARGS=--hardware      # macOS: the tap is a DYLD interposer
```

See `docs/DIFFERENTIAL.md`.

## 7. Two-node bench — the only thing still worth hardware

The remaining gap is real bus timing and genuine ECU behaviour. It needs a
second CAN node: a vehicle, or a bench ECU with its own CAN transceiver.

---

## What is deliberately NOT relied on

**On-car testing is confirmation, not qualification.** No claim in
`docs/PROTOCOL.md` rests on it, and nothing in CI needs it. If the car is
never connected, the only thing lost is confirmation that a real vehicle's
timing matches the simulator's.

## Running everything

```bash
make check-all       # unit + sanitizers + fuzz + simulator; no cable required
```
