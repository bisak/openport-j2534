# Differential testing against the prior driver

This driver replaces one already in service. The question that matters is not
"does it work" but "does it do the same thing, and where it differs, which one
is right".

Reference implementation: the MQBau-Engineering macOS/arm64 fork of the
`dschultzca`/`NikolaKozina` Linux `j2534` driver (`libj2534.dylib`, passed as
`OLD_DRIVER`). Its upstream is deleted.

## Method

Both libraries are driven through **one identical sequence** by
`tests/differential/diff_runner.c`, which `dlopen`s whichever library it is
pointed at. Two things are captured per run:

1. **What it returned** — every return code and output value.
2. **What it put on the wire** — `tools/usbtap/usbtap.c` interposes
   `libusb_bulk_transfer` via `DYLD_INSERT_LIBRARIES` and logs every transfer
   with direction, endpoint, timeout, result and bytes.

Both libraries link the same libusb, so one tap observes both and the traces
are directly comparable. The wire trace is the stronger evidence: it is what
the cable actually sees, independent of how either library is written.

```bash
make differential                      # no-hardware section
make differential DIFF_ARGS=--hardware # full sequence, cable required
```

Artifacts land in `tests/differential/out/`.

### Three traps this harness fell into, and how it is guarded

Worth recording, because each produced a **confident, wrong "identical"** —
the same failure mode the driver itself is built to avoid.

1. **Both runs loaded the same library.** `DYLD_LIBRARY_PATH` overrides even an
   absolute `dlopen` path by leaf name, and both libraries are called
   `libj2534.dylib`. The result was a flawless match that meant nothing.
   *Guard:* the runner reports the path `dladdr` actually resolved and aborts
   if it is not the one requested; the harness sets no `DYLD_LIBRARY_PATH`.
2. **Both runs crashed and produced nothing.** Two empty files diff clean.
   *Guard:* an empty result or a missing trace is a fatal harness error.
3. **The hung run's output was lost.** The old driver blocks forever on many
   transfers, so the watchdog `SIGKILL`s it and fully-buffered stdout dies with
   it — discarding the record of which call wedged. *Guard:* the runner
   line-buffers.

One more, specific to Apple Silicon: `bash`, `env`, `dirname` and `sleep` are
all **arm64e**, while libusb is arm64-only so the tap cannot be fat. Any arm64e
process inheriting `DYLD_INSERT_LIBRARIES` aborts before it starts. The
injection is therefore applied to the runner's own `exec` and to nothing else.

## Results

Sequence: open, read version, read battery, connect ISO15765 @ 500k, set
BS/STMIN, install a flow-control filter, clear the RX buffer, write a
TesterPresent `$3E`, read, start and stop a periodic message, request
programming voltage, then a set of deliberate error paths. Bench conditions:
**cable on USB, not connected to a vehicle**, so there is no CAN bus and every
transmit is expected to fail.

`rc=0` is `STATUS_NOERROR`.

| Call | Old | New | Which is right |
|---|---|---|---|
| `GetLastError` before open | `len=0` | `len=8` | **New.** Old returns an empty string always — a caller cannot report why anything failed |
| `PassThruOpen` | 0 | 0 | agree |
| `ReadVersion` | `1.17.4877` | `1.17.4877` | agree |
| `Ioctl READ_VBATT` | 130 | 130 | agree |
| `Connect ISO15765` | 0, channel 6 | 0, channel 6 | agree |
| `Ioctl SET_CONFIG` | 0 | 0 | agree |
| `StartMsgFilter` | 0, filter 0 | 0, filter 0 | agree |
| `WriteMsgs $3E` | **0, sent=1** | **9, sent=0** | **New.** See below |
| `ReadMsgs` (500 ms) | **0, received=4** | **16, received=0** | **New.** See below |
| `StartPeriodicMsg` | 0, msgid=0 | 0, msgid=1 | **New.** Old is a stub |
| `SetProgrammingVoltage` | **0** | **1** | **New.** Old is a stub reporting success |
| `Connect` twice | **0** | **20** | **New.** Device said `are 20` |
| `Disconnect` channel 9 | **0** | **2** | **New.** Channel 9 was never opened |
| `Ioctl` id `0xDEAD` | 1 | 15 | **New.** `ERR_INVALID_IOCTL_ID` is the specified code |
| `StartMsgFilter(NULL mask)` | **segfault** | **4** | **New.** See below |
| `StopMsgFilter` / `Disconnect` / `Close` | not reached | 0 | — |

### The three that matter

**`WriteMsgs` reported success for a transmit the device rejected.** The wire
trace settles it — the device's own answer was `are 9`:

```
OUT ep=0x02 timeout=1000 len=17 |att6 6 64\r\n\x00\x00\x07\xe0>\x00|
IN  ep=0x82 timeout=0    len=7  |are 9\r\n|          <- ERR_TIMEOUT
```

The old driver returned `0` and `sent=1`. A caller writing to an ECU cannot
distinguish a delivered command from one that never reached the bus. On a
reflash that is the difference between a completed erase and a silent stall.

**`ReadMsgs` returned four messages that do not exist.** There was no bus, no
node and no traffic; the trace contains no inbound message frame.

The mechanism is simpler than it first looked, and worth stating exactly. The
old driver only writes back through `pMsg` and `pNumMsgs` when it has at least
as many messages as were requested:

```c
if (rcvBufIndex >= *pNumMsgs) { memcpy(pMsg, ...); /* pNumMsgs updated here */ }
return 0;
```

A short read falls straight past that and returns `STATUS_NOERROR` with
`*pNumMsgs` **still holding the caller's requested count** and `pMsg`
untouched. It does not fabricate message content — it simply never reports that
it read nothing, so the caller consumes whatever was already in its own buffer
as though it were bus traffic. Asking for four and receiving none yields
"success, four messages".

The new driver sets `*pNumMsgs` on every return path and reports
`ERR_BUFFER_EMPTY`.

Its buffer handling is otherwise sound and was misread on first inspection: a
correct `if (rcvBufIndex >= 8) goto ARRAY_FULL;` guard prevents the fixed
eight-slot array from being overrun. What does not work is the overflow
*report* beside it — `rcvBufIndex` is `uint32_t`, so `rcvBufIndex < 0` is never
true, and the guard caps the index at 8 so `rcvBufIndex > 8` is never true
either. The `ERR_BUFFER_OVERFLOW` return is unreachable, and messages dropped
when the array fills are lost silently.

**`StartMsgFilter(NULL mask)` crashes the old driver.** It dereferences
`pMaskMsg` before checking it. Reproduced in isolation: the process dies at the
call with no return. The new driver returns `ERR_NULL_PARAMETER`.

### Wire-level differences

Both drivers emit the same command vocabulary — `ato6 0 500000 0`,
`ats6 30 0`, `atf6 3 0 4` + 12 payload bytes, `att6 6 64` + payload, `atc6`,
`atz` — byte for byte. The differences are structural:

| | Old | New |
|---|---|---|
| Opening handshake | `\r\n\r\nati\r\n`, then `ata` | drain, `atz`, drain, `ata`, `ati` |
| Transfer timeouts | `timeout=0` on most transfers — **blocks forever** | every transfer bounded |
| Command + payload | one transfer | two (line, then payload) — same bytes, same order |
| Reads | on demand, inside `ReadMsgs` | continuous 50 ms poll in a reader thread |

The old driver's `timeout=0` is visible throughout its trace and is exactly
what makes a wedged cable hang the calling application instead of returning
`ERR_TIMEOUT`.

The reader thread makes the new trace noisier — idle polls appear as
`rc=-7 len=0` lines — but it is what allows asynchronous message frames and
command replies to be demultiplexed rather than one being lost behind the
other.

## Intentional divergences

Each is a deliberate decision to be correct rather than bug-compatible.

| # | Divergence | Why |
|---|---|---|
| 1 | Stubs return `ERR_NOT_SUPPORTED`, never `STATUS_NOERROR` | A caller using `StartPeriodicMsg` for a TesterPresent keep-alive otherwise gets silence and believes it is transmitting |
| 2 | `StartPeriodicMsg`/`StopPeriodicMsg` are implemented | In the firmware (`atm`/`atn`), the commands the vendor DLL sends |
| 3 | `SetProgrammingVoltage` implemented | `atv`, as the vendor sends it; one pin at a time, as J2534-1 §7.2.11 requires |
| 4 | `ReadMsgs` does **not** double a timeout below 100 ms | The old driver silently doubled the caller's timeout for K-line. That breaks the API contract. A K-line caller should pass the timeout it needs |
| 5 | Every libusb call is checked and every timeout bounded | 19 of 26 transfers in the old driver ignore the return code; 20 of 26 use timeout 0 |
| 6 | `GetLastError` returns real text | It returned an empty string |
| 7 | IOCTLs 4, 9, 10 and 14 implemented; 11–13 return `ERR_NOT_SUPPORTED` | 11–13 are the J1850 functional-message table, and this device rejects J1850 outright, so claiming support would be undetectably false |
| 8 | `Ioctl` with an unknown id returns `ERR_INVALID_IOCTL_ID` (15) | Old returned `ERR_NOT_SUPPORTED` (1) |
| 9 | Errors from the device are passed through unchanged | `are <n>` already carries the J2534 code; remapping loses information |
| 10 | Synchronising handshake at open; orphan replies discarded | §9.2 of PROTOCOL.md — a stale reply silently corrupts every later result |
| 11 | Filter messages must all be the same length | The device takes one length for all of them; a mismatch would shift the pattern on the wire |
| 12 | Version parsed at the `": "` delimiter, not byte offset 24 | Offset 24 happens to equal `strlen("ari main code version : ")`. The delimiter survives a firmware that changes the prefix |
| 13 | A flow-control message on a PASS/BLOCK filter is rejected with `ERR_INVALID_MSG` | J2534-1 DEC2004 §7.2.9.2 states it verbatim. Accepting it silently would let a caller believe flow control had been configured on a filter type that has none. Found by a conformance review; we were silently ignoring it |
| 14 | `WriteMsgs` treats `Timeout` as a budget for the whole call | Handing each message the full value let a five-message write overrun a 300 ms budget by 635 ms **and still return `STATUS_NOERROR`**. Only slow-but-successful writes expose this: the call returns on the first failure, so a silent device never reaches message two |
| 15 | `ReadMsgs` returns `ERR_BUFFER_OVERFLOW` when the receive queue has overrun | Dropped messages were counted and never surfaced. A caller reading too slowly lost vehicle data with no way to know its view of the bus had a hole in it. Surviving messages are still delivered and counted in `pNumMsgs` |
| 16 | An invalid `FilterType` returns `ERR_INVALID_MSG` | The standard names no code for this. `ERR_INVALID_FILTER_ID` is arguable, but that code is about the id, not the type, and `ERR_INVALID_MSG` is what §7.2.9.2 uses for the adjacent malformed-argument case. Recorded as a deliberate choice rather than left accidental |

## Decisions taken where the standard and the device disagree

Three points where conformance and observed behaviour pulled apart, resolved
2026-09-13 after auditing against SAE J2534-1, ISO 15765 and the K-line
standards. The principle applied was: no surprising behaviour for an
application written against the standard, and never discard information a
caller cannot recover.

| Point | Decision | Why |
|---|---|---|
| A TxDone indication carries the CAN id on the wire. J2534-1 DEC2004 §8.6 says DataSize 4 (or 5) with the CAN id of the message just sent, and the vendor DLL reports exactly that (measured on a live bus 2026-09-13, `RxStatus 0x9, DataSize 4`) | **Report the id** | An earlier revision reported no data, citing the JAN2022 revision. The driver reports API 04.04, whose text and the vendor agree |
| A full receive queue: discard the oldest message or the newest? | **Conform** — discard the arriving one | Also the safer answer. A caller that ignores the overflow return then gets a truncated but contiguous sequence, instead of one with an unmarked hole in the middle. The previous reasoning, that a stale message is less useful than a fresh one, is true of a live gauge and wrong for a diagnostic exchange where order is the point |
| Per-protocol message size limits | **Enforce the certain ones only** | The minima, and the maxima that follow from the wire format. ISO14230's maximum depends on connect flags this driver does not track, so it is left to the device — wrongly rejecting a valid message would be worse |

### Two ECUs answering at once

ISO15765 reassembly keeps one context per channel. If two ECUs answer a
functional request and **both** replies exceed one wire frame, their
continuation frames interleave and would be concatenated into each other.

Every continuation chunk carries the CAN id (the vendor DLL reassembles only
that layout, `AB-OFFICIAL.md`), so routing chunks by id is possible. It is
not done: no legislated OBD reply approaches 250 bytes, only one ECU has ever
answered in a measured session, and the vendor DLL keeps one context too.

## A bug this harness found in the new driver

Worth recording, because it is the argument for doing differential testing at
all rather than trusting a unit suite.

The first hardware run showed `PassThruStopMsgFilter` returning `ERR_TIMEOUT`
against real hardware while every unit test passed. Cause: the host-scheduled
periodic message used a 200 ms deadline, but a CAN transmit with no bus takes
~1.2 s to be rejected. The write gave up; the device's late `are 9` was then
collected by the *next* command, which reported it as its own failure.

That is the §9.2 hazard occurring inside the new driver. Fixed by accepting a
reply only when a command is waiting for one, and since then by matching
every reply to its command by sequence number. The host scheduler itself is
gone: periodic messages run in the firmware.
`late_reply_is_not_reused()` in `tests/unit/test_j2534.c` is the regression.

## What has not been tested

The bench had no vehicle, so **no test here has exercised a live CAN bus**.
Specifically untested against both drivers: received messages carrying real
data, multi-frame ISO-TP reassembly, K-line initialisation, timing under load,
and cable-yanked-mid-read. These need the sequence re-run with the cable on a
vehicle; the harness supports it unchanged.

## Against the vendor's own DLL

`tools/ab-official/` runs Tactrix's `op20pt32.dll` on this machine (Docker,
box64 + Wine) through the same scripted sequence as this driver; the method
and the wire findings are in `docs/AB-OFFICIAL.md`. Where the two libraries
return different codes for the same call, 2026-09-13, build 1.02.0.4868:

| Call | Vendor | This driver | Standard (J2534-1 JAN2022) |
|---|---|---|---|
| `PassThruDisconnect` on a channel that was never opened, before `Open` | `ERR_INVALID_CHANNEL_ID` | `ERR_INVALID_DEVICE_ID` | `ERR_INVALID_DEVICE_ID`: §7.2.1, any call before a successful Open |
| `PassThruIoctl` given a device id where a channel id is expected | `ERR_INVALID_CHANNEL_ID` | `ERR_INVALID_IOCTL_ID` | `ERR_INVALID_CHANNEL_ID` reads closer to the text |
| `PassThruStartMsgFilter` with a NULL mask | `ERR_FAILED` | `ERR_NULL_PARAMETER` | `ERR_NULL_PARAMETER` |
| `PassThruWriteMsgs` with `Timeout=0` on a bus with no ACK peer (cable, bench) | `STATUS_NOERROR`, 1 sent; the frame is queued unnumbered and not waited for | `ERR_TIMEOUT`, 0 sent | queue and return immediately: the vendor |
| `PassThruIoctl(CLEAR_RX_BUFFER)` | clears the DLL's queue, nothing on the wire | used to also send `atl` to the device | either; now the same as the vendor |
| `PassThruWriteMsgs` transmit budget | sends the Timeout to the firmware (`att … <timeout_us>`) | used to send none, leaving the firmware's ~1 s default even for a 5 s write | the vendor; now the same |
| `PassThruIoctl(FIVE_BAUD_INIT, 0x33)` | `atw3 51` | used to send `atw3 1` + a raw 0x33 byte, initialising address 1 | the vendor; now the same |
| Periodic message running (live bus, 100 ms) | `atm`: scheduled by the firmware, no transmit-done indications | used to schedule on the host with `att`; now `atm` | the vendor; now the same |

Where the two still differ, the driver follows the standard's text: any call
before a successful `PassThruOpen` returns `ERR_INVALID_DEVICE_ID` (§7.2.1),
a NULL message pointer returns `ERR_NULL_PARAMETER`, and an unknown ioctl id
returns `ERR_INVALID_IOCTL_ID`. Every other return code in the sequence
agrees, including the timeout, filter, message-id and channel-in-use cases.

## Real-world validation: drop-in replacement

**Caveat, added after the fact:** the bench run below never received a
message, so it validated the transmit side of "drop-in" and nothing of the
receive side. The receive framing this driver shipped with would have made a
real consumer discard every segmented response on a car (`PROTOCOL.md` §7).
The old driver, for all its faults, surfaced the START announcement as a
separate message — accidentally the right shape. That is now covered without
a car by `s_consumer_rule` in `tests/sim/run_scenarios.py`, which applies the
consumer's acceptance rule against the simulator.

The consuming tools in question are Python read and write tools that load a
J2534 library via `ctypes.CDLL` and use ISO15765 with a flow-control filter,
`SET_CONFIG`, `READ_VBATT`, `CLEAR_RX_BUFFER`, `ISO15765_BS`/`STMIN`, and the
`ISO15765_FRAME_PAD` TxFlag. The write tool shares the read tool's J2534
wrapper, so exercising the read tool covers both — which is the right way
round, because the write tool can erase an irreplaceable ECU and is not
something to run as a driver test.

All fourteen entry points are exported and resolve under `ctypes`.

The read tool's probe, cable on the bench with no vehicle:

| | Old driver | New driver |
|---|---|---|
| Firmware | `1.17.4877` | `1.17.4877` |
| Battery | `0.13 V` | `0.13 V` |
| Outcome | no response | no response |
| Reason given | `no response to service 0x23 (last=None)` | `PassThruWriteMsgs failed: rc=9 ERR_TIMEOUT` |

Same behaviour, one meaningful difference. The old driver swallowed the
transmit failure and presented it as **a silent ECU**, sending the operator off
to check CAN ids, baud rate and session state. The new driver reports that the
message never reached a bus at all — which is the actual situation, and is one
line of output instead of an afternoon.
