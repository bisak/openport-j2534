# Differential testing against the prior driver

This driver replaced one already in service: the MQBau-Engineering macOS/arm64
fork of the `dschultzca`/`NikolaKozina` Linux `j2534` driver (`libj2534.dylib`,
passed as `OLD_DRIVER`; its upstream is deleted). The comparison asks whether
the new driver does the same thing as the old one, and where it differs, which
one is right. Where this driver differs from Tactrix's own driver is a separate
question, answered in [`AB-OFFICIAL.md`](AB-OFFICIAL.md).

## Method

Both libraries are driven through one identical sequence by
`tests/differential/diff_runner.c`, which `dlopen`s whichever library it is
pointed at. Two things are captured per run:

1. What it returned: every return code and output value.
2. What it put on the wire: `tools/usbtap/usbtap.c` interposes
   `libusb_bulk_transfer` via `DYLD_INSERT_LIBRARIES` and logs every transfer
   with direction, endpoint, timeout, result and bytes.

Both libraries link the same libusb, so one tap observes both and the traces
are directly comparable. The wire trace is the stronger evidence: it is what
the cable actually sees, independent of how either library is written. The
harness runs on macOS only, because the tap is a DYLD interposer.

The section that needs no hardware:

```bash
make differential OLD_DRIVER=/path/to/libj2534.dylib
```

The full sequence, cable required:

```bash
make differential OLD_DRIVER=/path/to/libj2534.dylib DIFF_ARGS=--hardware
```

Artifacts land in `tests/differential/out/`. The runner is bench-safe: it
switches programming voltage off rather than on.

### Three traps this harness fell into, and how it is guarded

Each made two runs look identical when they were not.

1. Both runs loaded the same library. `DYLD_LIBRARY_PATH` overrides even an
   absolute `dlopen` path by leaf name, and both libraries are called
   `libj2534.dylib`. Guard: the runner reports the path `dladdr` actually
   resolved and aborts if it is not the one requested; the harness sets no
   `DYLD_LIBRARY_PATH`.
2. Both runs crashed and produced nothing. Two empty files compare equal.
   Guard: an empty result or a missing trace is a fatal harness error.
3. The hung run's output was lost. The old driver blocks forever on many
   transfers, so the watchdog `SIGKILL`s it, and fully buffered stdout dies with
   it. Guard: the runner line-buffers.

One more, specific to Apple Silicon: `bash`, `env`, `dirname` and `sleep` are
all arm64e, while libusb is arm64 only, so the tap cannot be fat. Any arm64e
process inheriting `DYLD_INSERT_LIBRARIES` aborts before it starts. The
injection is therefore applied to the runner's own `exec` and to nothing else.

## Results

This comparison dates from the 0.1.0 release (2026-09-13). Sequence: open, read
version, read battery, connect ISO15765 at 500 kbit, set BS/STMIN, install a
flow-control filter, clear the RX buffer, write a TesterPresent `$3E`, read,
start and stop a periodic message, programming voltage, then a set of
deliberate error paths. Bench conditions: the cable on USB and not connected to
a vehicle, so there is no CAN bus and every transmit is expected to fail.
`rc=0` is `STATUS_NOERROR`.

| Call | Old | New | Which is right |
|---|---|---|---|
| `GetLastError` before open | `len=0` | `len=8` | New. Old always returns an empty string, so a caller cannot report why anything failed |
| `PassThruOpen` | 0 | 0 | agree |
| `ReadVersion` | `1.17.4877` | `1.17.4877` | agree |
| `Ioctl READ_VBATT` | 130 | 130 | agree |
| `Connect ISO15765` | 0, channel 6 | 0, channel 6 | agree |
| `Ioctl SET_CONFIG` | 0 | 0 | agree |
| `StartMsgFilter` | 0, filter 0 | 0, filter 0 | agree |
| `WriteMsgs $3E` | 0, sent=1 | 9, sent=0 | New. See below |
| `ReadMsgs` (500 ms) | 0, received=4 | 16, received=0 | New. See below |
| `StartPeriodicMsg` | 0, msgid=0 | 0 | New. Old is a stub that transmits nothing |
| `SetProgrammingVoltage` | 0 | device's answer | New. Old is a stub reporting success |
| `Connect` twice | 0 | 20 | New. The device said `are 20` |
| `Disconnect` channel 9 | 0 | 2 | New. Channel 9 was never opened |
| `Ioctl` id `0xDEAD` | 1 | 15 | New. `ERR_INVALID_IOCTL_ID` is the specified code |
| `StartMsgFilter(NULL mask)` | segfault | 4 | New. See below |
| `StopMsgFilter` / `Disconnect` / `Close` | not reached | 0 | |

### The three that matter

`WriteMsgs` reported success for a transmit the device rejected. The wire trace
settles it; the device's own answer was `are 9`:

```
OUT ep=0x02 timeout=1000 len=17 |att6 6 64\r\n\x00\x00\x07\xe0>\x00|
IN  ep=0x82 timeout=0    len=7  |are 9\r\n|          <- ERR_TIMEOUT
```

The old driver returned `0` and `sent=1`. A caller writing to an ECU cannot tell
a delivered command from one that never reached the bus. On a reflash that is
the difference between a completed erase and a silent stall.

`ReadMsgs` returned four messages that do not exist. There was no bus, no node
and no traffic; the trace contains no inbound message frame. The old driver
writes back through `pMsg` and `pNumMsgs` only when it has at least as many
messages as were requested:

```c
if (rcvBufIndex >= *pNumMsgs) { memcpy(pMsg, ...); /* pNumMsgs updated here */ }
return 0;
```

A short read falls past that and returns `STATUS_NOERROR` with `*pNumMsgs`
still holding the caller's requested count and `pMsg` untouched, so the caller
consumes whatever was already in its own buffer as though it were bus traffic.
Asking for four and receiving none yields "success, four messages". Its
overflow report is unreachable too: `rcvBufIndex` is `uint32_t`, so
`rcvBufIndex < 0` is never true, and the guard caps the index at 8, so
`rcvBufIndex > 8` is never true either. Messages dropped when the eight-slot
array fills are lost without a report.

`StartMsgFilter(NULL mask)` crashes the old driver. It dereferences `pMaskMsg`
before checking it.

### Wire-level differences

Both drivers use the same command vocabulary: `ato6 0 500000 0`, `ats6 30 0`,
`atf6 3 0 4` plus 12 payload bytes, `att6 6 64` plus payload, `atc6`, `atz`.
This driver also appends the arguments the vendor DLL sends: a transmit budget
on `att`, and a sequence number on every command (`PROTOCOL.md` §3, §4). The
structural differences:

| | Old | New |
|---|---|---|
| Opening handshake | `\r\n\r\nati\r\n`, then `ata` | `\r\n\r\n`, `atz`, `ata`, `ati`; every command numbered and its reply matched by number |
| Transfer timeouts | `timeout=0` on most transfers: blocks forever | every transfer bounded |
| Command + payload | one transfer | two (line, then payload); same bytes, same order |
| Reads | on demand, inside `ReadMsgs` | continuous 10 ms poll in a reader thread, so message frames and command replies are demultiplexed instead of one being lost behind the other |

The old driver's `timeout=0` is what makes a wedged cable hang the calling
application instead of returning `ERR_TIMEOUT`.

## Intentional divergences

Each is a decision to be correct rather than bug-compatible with the old
driver.

| # | Divergence | Why |
|---|---|---|
| 1 | Nothing returns `STATUS_NOERROR` for work it did not do; the one unsupported facility, the J1850 functional-message table, returns `ERR_NOT_SUPPORTED` | A caller using `StartPeriodicMsg` for a TesterPresent keep-alive on the old driver got silence and believed it was transmitting |
| 2 | `StartPeriodicMsg`/`StopPeriodicMsg` are implemented, in the firmware (`atm`/`atn`) as the vendor DLL does | The old driver's are stubs |
| 3 | `SetProgrammingVoltage` is implemented (`atv`); one pin at a time, as J2534-1 §7.2.11 requires | The old driver's is a stub |
| 4 | `ReadMsgs` does not double a timeout below 100 ms | The old driver silently doubled the caller's timeout for K-line, which breaks the API contract |
| 5 | Every libusb call is checked and every timeout bounded | 19 of 26 transfers in the old driver ignore the return code; 20 of 26 use timeout 0 |
| 6 | `GetLastError` returns real text | It returned an empty string |
| 7 | IOCTLs 4, 9, 10 and 14 implemented | They were missing |
| 8 | An unknown ioctl id returns `ERR_INVALID_IOCTL_ID` (15) | Old returned `ERR_NOT_SUPPORTED` (1) |
| 9 | Errors from the device are passed through unchanged | `are <n>` already carries the J2534 code; remapping loses information |
| 10 | Commands are numbered and replies matched by number; a reply nobody waits for is discarded | `PROTOCOL.md` §9: a stale reply otherwise corrupts every later result |
| 11 | Filter messages must all be the same length | The device takes one length for all of them; a mismatch would shift the pattern on the wire |
| 12 | Version parsed at the `": "` delimiter, not byte offset 24 | Offset 24 happens to equal `strlen("ari main code version : ")`; the delimiter survives a firmware that changes the prefix |
| 13 | `WriteMsgs` treats `Timeout` as a budget for the whole call | Handing each message the full value let a five-message write overrun a 300 ms budget by 635 ms and still return `STATUS_NOERROR` |
| 14 | `ReadMsgs` returns `ERR_BUFFER_OVERFLOW` when the receive queue has overrun; the surviving messages are still delivered and counted | Dropped messages were counted and never surfaced |
| 15 | An invalid `FilterType` returns `ERR_INVALID_MSG` | The standard names no code for this; `ERR_INVALID_MSG` is what §7.2.9.2 uses for the adjacent malformed-argument case |

Earlier revisions also refused a flow-control message given with a PASS or
BLOCK filter (`ERR_INVALID_MSG`, as J2534-1 §7.2.9.2 states). Since 0.3.0 the
driver ignores it, as the vendor DLL does, because refusing it would fail an
application that passes a zeroed message instead of NULL (`AB-OFFICIAL.md`).

## Decisions where the standard and the device pulled apart

| Point | Decision | Why |
|---|---|---|
| The transmit indication carries the CAN id on the wire | Report it: `DataSize` 4, `ExtraDataIndex` 0 | J2534-1 DEC2004 §8.6 says exactly that, and so does the vendor DLL (measured on a live bus 2026-09-13, `RxStatus 0x9, DataSize 4`) |
| A full receive queue: discard the oldest message or the newest? | Discard the arriving one | J2534-1 requires it, and a caller that ignores the overflow return then sees a truncated but contiguous sequence instead of one with an unmarked hole |
| Per-protocol message size limits | Enforce J2534-1 Figure 42 for CAN and ISO15765; leave ISO14230's to the device | ISO14230's maximum depends on the checksum flag, and wrongly rejecting a valid message would be worse than letting the device reject it |
| Two ECUs answering a functional request with segmented replies at once | One reassembly context per channel, as the vendor DLL keeps | Every chunk carries the CAN id (`PROTOCOL.md` §7.6), so routing by id is possible; only one ECU has ever answered in a measured session |

## A bug this harness found in the new driver

The first hardware run showed `PassThruStopMsgFilter` returning `ERR_TIMEOUT`
while every unit test passed, which is the case for testing against hardware
rather than trusting a unit suite. A transmit the cable took 1.2 s to reject had
been abandoned after 200 ms, and its late `are 9` was collected by the next
command, which reported the transmit's failure as its own. That is
`PROTOCOL.md` §9.2 occurring inside the new driver. It was fixed by accepting a
reply only when a command is waiting for one, and later by matching every reply
to its command by sequence number; `late_reply_is_not_reused()` in
`tests/unit/test_j2534.c` is the regression.

## What this comparison covers, and what it does not

The old-driver comparison was made on a bench with no bus, so it covers
transmit failure, error paths and the wire vocabulary, not received data. The
live-bus comparisons since then were made against Tactrix's own DLL rather than
the old driver (`AB-OFFICIAL.md`: identical raw CAN frames on the Audi,
identical periodic commands, identical multi-frame delivery, and a live ECU on
a bench), and the received-message framing is pinned by recordings from a 2012
VW Caddy in `tests/unit/test_golden.c`. Re-running this harness on a vehicle
would add nothing the vendor comparison has not already settled.

## Against the vendor's own DLL

Where this driver and Tactrix's `op20pt32.dll` 1.02.0.4868 return different
codes for the same call because the J2534-1 text says otherwise, the driver
follows the standard:

| Call | Vendor | This driver | J2534-1 DEC2004 |
|---|---|---|---|
| Any call before a successful `PassThruOpen`, or after `PassThruClose` | `ERR_INVALID_CHANNEL_ID` | `ERR_INVALID_DEVICE_ID` | §7.2.1 |
| `PassThruIoctl` with an unknown id, given a device id | `ERR_INVALID_CHANNEL_ID` | `ERR_INVALID_IOCTL_ID` | either reads |
| `PassThruStartMsgFilter` with a NULL mask | `ERR_FAILED` | `ERR_NULL_PARAMETER` | `ERR_NULL_PARAMETER` |
| A second programming-voltage pin, or grounding K or L under a channel that uses it | passed to the cable | refused (`ERR_PIN_INVALID`, `ERR_CHANNEL_IN_USE`) | §7.2.11: one pin at a time |

Every difference from the vendor DLL, including these, the requests this driver
refuses before the wire, and the wire differences at open and for a
`Timeout=0` write, is listed in [`AB-OFFICIAL.md`](AB-OFFICIAL.md) and
summarised in the [README](../README.md#where-this-driver-differs-from-tactrixs-dll).

## Drop-in replacement, as a consuming tool sees it

Python read and write tools that load a J2534 library via `ctypes.CDLL` and use
ISO15765 with a flow-control filter, `SET_CONFIG`, `READ_VBATT`,
`CLEAR_RX_BUFFER` and the `ISO15765_FRAME_PAD` TxFlag were pointed at both
drivers, with the cable on the bench and no vehicle. All fourteen entry points
resolve under `ctypes`.

| | Old driver | New driver |
|---|---|---|
| Firmware | `1.17.4877` | `1.17.4877` |
| Battery | `0.13 V` | `0.13 V` |
| Outcome | no response | no response |
| Reason given | `no response to service 0x23 (last=None)` | `PassThruWriteMsgs failed: rc=9 ERR_TIMEOUT` |

The behaviour is the same, with one difference that matters. The old driver
swallowed the transmit failure and presented it as a silent ECU, which sends the
operator off to check CAN ids, baud rate and session state. The new driver
reports that the message never reached a bus, which is the actual situation.
The receive side of "drop-in", which a bench cannot show, is covered by the
consumer's own acceptance rule replayed over the Caddy exchange
(`s_consumer_rule` in `tests/sim/run_scenarios.py`).
