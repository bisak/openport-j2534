# Plan: the driver in a browser, as WebAssembly

Status: approved in outline by the owner, 2026-10-12. Not started.

**Goal.** Run the owner's GR Yaris bench tool `gry_write` (`rev-eng-gr-yaris/tools_local/c`) in
Chrome and Edge over Web Serial, on a WebAssembly build of this C driver, behaving as the native
build does. The phases end in a go/no-go report. Package API, licence and publishing come after, in
their own design.

The reasons for this architecture, the alternatives already rejected, and the decisions left for
after the PoC are in [WASM-DECISIONS.md](WASM-DECISIONS.md). Read it before proposing a different
approach.

## Before you start

**Read:** [PROTOCOL.md](PROTOCOL.md) §2, §3, §7 and §9; [TESTING.md](TESTING.md);
`src/op_device.h`, `src/op_device.c`, `src/op_j2534.c`; the test list in
`tests/unit/test_j2534.c`. Phase 4 adds `rev-eng-gr-yaris/CLAUDE.md`,
`rev-eng-gr-yaris/tools_local/README.md` and `tools_local/c/` (`Makefile`, `gry_cable.c`,
`gry_write_main.c`).

**Working rules:**
- Minimal change, matched to Tactrix's driver. The refactor moves code: wire bytes, return codes
  and timing stay as they are.
- Work on a branch named `wasm-poc` in each repository you touch, and commit when the owner asks;
  each repository's `CLAUDE.md` governs commits there. Your baseline is the commit you branch
  from; other sessions may edit these repositories meanwhile.
- At each **checkpoint**, stop, report to the owner, and continue on their go.
- Every measurement carries its date and conditions, as PROTOCOL.md does.

**Hardware:**
- The owner runs every step that talks to a real ECU. You prepare the command and the expected
  result.
- Bench work uses the donor AWD ECU and the bench chassis ECU only (PROTOCOL.md §10;
  `rev-eng-gr-yaris/notes/state/donor-first-powerup-sheet.md`).
- `gry_write dump` runs under the Yaris repository's own procedure: it changes the ECU's session
  and spends one security-access attempt from a budget of two. Flash writes to real ECUs are
  outside this plan.

## Design

Reference for every phase.

**Core and shells.** `src/op_device.c` mixes the device logic (sequence-number reply matching,
reassembly, channel queues, overflow counting) with threading and I/O. Phase 1 splits it into:
- the **core** (`op_core.c`): bytes in, commands started, events out. No locks, no I/O, no clock.
- the **native shell**: reader thread, mutex and condition variables, as today. The reader thread
  is a requirement: it drains frames while the application is not reading (Tactrix's DLL held
  1,460 frames over 29 s without a loss, 2026-09-16).
- the **browser shell** (`op_web.c`, WebAssembly only): no threads; every wait pumps.

`op_j2534.c` keeps its blocking J2534 calls; `op_proto.c` is unchanged.

**Pump:** read what has arrived, up to a deadline, and feed it to the core. A browser-shell wait
pumps until its condition holds or its deadline passes.

**Single-entry rule** (browser shell). Clang keeps arrays, structs and address-taken locals on one
stack in linear memory, and JSPI switches only the engine's own stack, so two suspended calls would
overwrite each other's frames. So:
- One call is inside the C code at a time. The JavaScript layer serialises entries; the C side
  counts them and aborts on a second, so a violation fails in tests.
- A JavaScript loop reads Web Serial continuously into a buffer. When bytes arrive with no call in
  flight, it enters C once to drain them into the core. This is event-driven, so a hidden tab's
  throttled timers cannot stall it, and the 1 MiB per-channel queue and its overflow reporting
  hold as natively.
- The package API waits for received messages in JavaScript, then calls ReadMsgs with Timeout 0,
  so a pending read leaves writes free. The J2534-shaped calls keep their exact semantics.

**How each environment reaches a cable.** Web Serial opens real serial devices only, so the
simulator's pty is reached another way, and each path proves something different:

| Environment | Transport | Proves |
|---|---|---|
| Native | `OPENPORT_DEVICE=<pty>` (existing) | the baseline |
| Node | TCP to `tools/ab-official/ttybridge.py`, which serves the pty (Node's `net`) | the WebAssembly build, the browser shell, parity |
| Chrome, simulator | WebSocket to a dev-only relay in front of `ttybridge.py` | the Worker, JSPI, OPFS, hidden-tab behaviour, the Yaris profiles |
| Chrome, cable | Web Serial | Web Serial itself: throughput, unplug, reopen |
| Node, cable | the `serialport` package on `/dev/cu.usbmodem*` | the transport Node users will run |

## Phase 0: toolchain and spike

1. Install Emscripten 6.0.10 (`brew install emscripten`). Record the versions of emcc, Node and
   Chrome.
2. Build a C program that awaits a JavaScript Promise through JSPI and keeps an address-taken local
   intact across the suspension. Run it in Node and in a Chrome dedicated Worker.
3. With the owner and the cable: a dedicated Worker opens the cable through Web Serial and `ati`
   answers.

**Done when** all three work. **Checkpoint.** If JSPI fails here, report it with the error; the
fallback is in WASM-DECISIONS.md.

## Phase 1: pin, then split the core

1. Inventory which of the native shell's timing paths `tests/unit/test_j2534.c` already pins
   (`late_reply_is_not_reused`, `device_loss_is_reported`, `close_wakes_a_blocked_read`,
   `queue_overrun_is_reported`, and others). Add a test for every path still unpinned: a late
   reply at the moment of a timeout, a close during a wait, the cable lost during a command, an
   overflow during a read. All green on the current code.
2. Record the baseline trace (recipe below).
3. Propose the core's interface. **Checkpoint:** the owner approves it before code moves. A
   starting sketch:

   ```c
   /* everything op_device holds today except the transport, threads and locks */
   typedef struct op_core op_core;

   void     op_core_reset(op_core *c, uint32_t seq_start);
   /* parse, reassemble, queue, match a waiting reply; returns OP_EV_* bits */
   unsigned op_core_feed(op_core *c, const uint8_t *buf, size_t n);
   /* number the line if it takes a number, claim the reply slot */
   size_t   op_core_begin_cmd(op_core *c, const char *line, size_t len,
                              char *out, size_t out_sz, int wait);
   int      op_core_take_reply(op_core *c, op_reply *out);   /* 1 when one is ready */
   void     op_core_end_cmd(op_core *c);                      /* timeout or done */
   int      op_core_pop(op_core *c, unsigned ch, PASSTHRU_MSG *out);
   unsigned op_core_take_dropped(op_core *c, unsigned ch);
   /* open_channel, close_channel, flush_channel as today */
   ```

4. Move the logic into `op_core.c`; `op_device.c` becomes the native shell.
5. Add core unit tests that use no threads and no sleeps, and a `make fuzz-core` target: arbitrary
   byte streams interleaved with arbitrary command sequences, checking that queues stay intact,
   every reply reaches its own command, and every dropped message is counted.

**Done when** `make check-all` is green; the characterization tests are green; the after-trace
shows no difference from the baseline in `ab_diff.py`; and the owner's short cable run
(`examples/op_smoke`, an ISO 15765 exchange with the bench ECU) behaves as before.
**Checkpoint**, with the diff.

Trace recipe, before versus after (`ab.sh` does vendor versus ours). Use a fresh simulator for
each run:

```bash
python3 tests/sim/openport_sim.py --pty-file /tmp/op.pty --log /tmp/op-sim.log &
cc -O2 -Wall -Iinclude -o tools/ab-official/j2534_trace tools/ab-official/j2534_trace.c
python3 tools/ab-official/ptytap.py "$(cat /tmp/op.pty)" --log /tmp/before.tap --pty-file /tmp/op.tapdev &
OPENPORT_DEVICE="$(cat /tmp/op.tapdev)" tools/ab-official/j2534_trace ./libj2534.dylib /tmp/before.jsonl
# after the refactor, the same three steps into /tmp/after.*, then:
python3 tools/ab-official/ab_diff.py /tmp/before.jsonl /tmp/after.jsonl --tap /tmp/before.tap /tmp/after.tap
```

`ab_diff.py` labels its inputs vendor and ours; any two runs compare.

## Phase 2: the driver in WebAssembly

1. Add `make wasm`: the driver plus `op_web.c`, `-sJSPI`, an ES module for the `web` and `node`
   environments, memory growth on. Add `make wasm-test`.
2. Build `tests/unit` with Emscripten and run it in Node.
   - `close_wakes_a_blocked_read` and `close_during_commands_is_safe` stay native-only. JavaScript
     tests cover the same guarantees: a close ends a pending read promptly, and overlapping calls
     reach the C code one at a time.
   - `stray_init_reply_is_not_a_filter_reply` and `stale_numbered_init_replies_are_dropped` switch
     from a delivering thread to scheduled pushes.

**Done when** every unit test and golden trace, less the two thread tests, is green in Node.

## Phase 3: the JavaScript layer and parity

1. Create `js/`: the loader, the Worker, the transports (Web Serial; TCP for tests; `serialport`
   for Node users), a thin J2534 wrapper, and a test page.
2. Add the WebSocket relay to `tools/` (dev only).
3. Add command-line flags to `tests/sim/openport_sim.py` for its desync, truncation and disconnect
   faults.
4. Build `j2534_trace` for WebAssembly, linked statically in place of `dlopen`.

**Done when:**
- native and WebAssembly `j2534_trace` runs against the simulator are identical in return codes,
  outputs and wire commands, with sequence numbers, timestamps and elapsed times excepted;
- every simulator fault (backlog, late reply, no ACK, desync, truncation, disconnect) gives the
  same return codes and the same recovery through both builds.

## Phase 4: `gry_write` in the browser

`gry_write` is single-threaded and calls the driver in sequence, so a whole command runs as one
suspended call under the single-entry rule. From the driver it uses (`gry_cable.c`):
- an ISO 15765 channel with `SET_CONFIG`, a flow-control filter and `CLEAR_RX_BUFFER`;
- raw CAN channels with pass-all filters;
- firmware periodic messages for the functional keepalive (`3E 80` to 0x777 and 0x77F every
  1000 ms, repeated by the cable, so throttled timers cannot touch it);
- `READ_VBATT`;
- batched ReadMsgs: 8 per 25 ms on the main channel, 128 per 50 ms on the capture channel.

The keepalive (`gry_cable.c:1108`) and the dump capture (`gry_cable.c:1268`) each connect raw CAN,
which is firmware channel 5 and opens once: run them one at a time, as natively.

1. Run the driver, the tool and the Web Serial read loop in one dedicated Worker, with the page as
   UI. Web Serial works in dedicated workers (Chrome 89+, per MDN), and OPFS synchronous access
   handles exist only in workers.
2. In the WebAssembly build, make `nanosleep` a suspending sleep that pumps, so frames keep moving
   while the tool waits.
3. Put the write journal (`gry_journal.c`: flushed, fsync'd) and the 0x27 attempt budget
   (`gry_cable.c`: tmp file, fsync, rename) on WasmFS's OPFS backend, which has a JSPI path that
   needs no pthreads. Test whether its flush and rename give the guarantees fsync and rename give
   on APFS. Inputs (package `.ini`, `secrets/keys.json`, kernel, image) come in through a file
   picker; dumps go out as downloads and to OPFS.
4. Map `argv`, the `GRY_*` environment switches and stdout/stderr to the page's UI.
5. Add a WebAssembly target to `tools_local/c/Makefile`, linking the driver's WebAssembly objects
   the way the native build links `libj2534.dylib`.
6. Add two simulator ECU models:
   - the dump kernel: frames on 0x7B9, one counter byte and seven data bytes, 8 KiB per trigger,
     56 triggers, about 3,400 frames/s;
   - the write sequence: `34`, 1024-byte `36` blocks, `37`, `31` routines with response-pending.
7. With the owner, the Web Serial throughput load. The cable's periodic messages run at 1 ms
   (PROTOCOL.md §10), and with `LOOPBACK` on and a pass-all filter each transmission returns as
   the frame plus its echo.
   - Setup: two periodic messages at 1 ms on raw CAN, eight zero bytes each, id 0x7FF (the bench
     ECU does not receive it), with the bench chassis ECU on the bus to acknowledge.
   - Expected: about 4,000 frames/s and 84 KB/s into the host, above the dump's 3,400 frames/s of
     the same size, at about half the bus's capacity.
   - One message at 1 ms is measured; two at once is not, so count what actually arrives per id.
8. With the owner, `gry_write dump` on the donor, natively and then in Chrome.

**Done when:**
- the simulated dump completes with zero lost frames natively, in Node, and in Chrome both in
  front and hidden for more than five minutes;
- the simulated write completes, with the journal and attempt budget intact after a forced reload
  mid-run;
- on the cable in Chrome:
  - open, firmware version, battery voltage, ISO 15765 and CAN, and close all work;
  - pulling the USB lead mid-call returns `ERR_DEVICE_NOT_CONNECTED` promptly, and a reopen after
    replugging works;
- the throughput load runs 60 s with no lost frame, in front and hidden;
- the donor dump from Chrome has the same SHA-256 as the native dump.

Report each run's time beside native's; the owner decides whether the difference is acceptable.

## Phase 5: go/no-go report

Write `docs/WASM-REPORT.md`:
- every phase's done-when result, with its measurements;
- answers to these questions:
  - Does Emscripten 6.0.10's JSPI do what this needs?
  - Does Web Serial in a Worker carry the dump's rate with no loss?
  - Do OPFS flush and rename match fsync and rename?
  - How does Web Serial report an unplugged cable, and how fast?
  - Are a Worker's timers throttled in a hidden tab?
  - Does Chrome freeze a hidden tab that has a serial port open?
- for each failure, whether a fix exists within this design.

Go requires every phase's done-when to be met.

## Scope

In: the driver and its tests and simulator, `js/` as far as the phases need it, and `gry_write`'s
WebAssembly build.

Out: the public JavaScript API and TypeScript types, the package's licence and publishing, flash
writes to real ECUs, the Evo X tools, the power supply, Pico and RH850 serial boot in the browser,
Windows, Firefox and Safari.
