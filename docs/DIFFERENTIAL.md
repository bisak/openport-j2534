# Testing against the driver this one replaced

This driver replaced one already in use: the MQBau-Engineering macOS/arm64 fork
of the `dschultzca`/`NikolaKozina` Linux `j2534` driver (its upstream is
deleted). This harness runs both through the same calls and compares what they
returned and what they sent to the cable. Where the two differ, the question is
which one is right.

The comparison with Tactrix's own driver is a separate harness:
[AB-OFFICIAL.md](AB-OFFICIAL.md).

## Summary

The comparison was made at the 0.1.0 release (2026-09-13), on a bench with no
bus. The old driver:

- reported success for a transmit the cable rejected;
- returned four messages from an empty bus;
- crashed on a NULL filter mask;
- blocked forever on most USB transfers, so a wedged cable hung the
  application;
- stubbed out periodic messages and programming voltage, reporting success.

This driver does none of those. The live-bus comparisons since then were made
against Tactrix's DLL instead.

## Running it

macOS only: the USB traffic recorder is a `DYLD_INSERT_LIBRARIES` interposer.

```bash
make differential OLD_DRIVER=/path/to/libj2534.dylib                          # the part that needs no cable
make differential OLD_DRIVER=/path/to/libj2534.dylib DIFF_ARGS=--hardware     # the full sequence, cable attached
```

Output goes to `tests/differential/out/`. The run is safe on a bench: it
switches programming voltage off, never on.

## How it works

`tests/differential/diff_runner.c` `dlopen`s whichever library it is given and
runs the same sequence through it. Two things are recorded per run:

1. **What it returned:** every return code and output value.
2. **What it sent:** `tools/usbtap/usbtap.c` interposes `libusb_bulk_transfer`
   and logs every transfer with direction, endpoint, timeout, result and bytes.

Both libraries link the same libusb, so one tap observes both and the traces
compare directly. The wire trace is the stronger evidence: it is what the cable
saw, whatever either library's code says.

### Traps, and the guards against them

Each of these made two runs look identical when they were not.

| Trap | Guard |
|---|---|
| Both runs loaded the same library. `DYLD_LIBRARY_PATH` overrides even an absolute `dlopen` path by leaf name, and both are called `libj2534.dylib`. | The runner reports the path `dladdr` resolved and aborts if it is not the one requested. The harness sets no `DYLD_LIBRARY_PATH`. |
| Both runs crashed and produced nothing; two empty files compare equal. | An empty result or a missing trace is a fatal harness error. |
| The hung run's output was lost. The old driver blocks forever on many transfers, the watchdog `SIGKILL`s it, and buffered stdout dies with it. | The runner line-buffers. |
| On Apple Silicon, `bash`, `env`, `dirname` and `sleep` are arm64e while libusb is arm64 only, so the tap cannot be fat, and any arm64e process inheriting `DYLD_INSERT_LIBRARIES` aborts. | The injection applies only to the runner's own `exec`. |

## Results

The sequence: open, read version, read battery, connect ISO 15765 at 500 kbit,
set BS and STmin, install a flow-control filter, clear the receive buffer, write
a TesterPresent (`$3E`), read, start and stop a periodic message, programming
voltage, then deliberate error paths. The cable was on USB with no vehicle, so
every transmit was expected to fail. `rc=0` is `STATUS_NOERROR`.

| Call | Old | New | Which is right |
|---|---|---|---|
| `GetLastError` before open | `len=0` | `len=8` | New. The old one always returns an empty string, so a caller cannot report why anything failed. |
| `PassThruOpen` | 0 | 0 | agree |
| `ReadVersion` | `1.17.4877` | `1.17.4877` | agree |
| `Ioctl READ_VBATT` | 130 | 130 | agree |
| `Connect ISO15765` | 0, channel 6 | 0, channel 6 | agree |
| `Ioctl SET_CONFIG` | 0 | 0 | agree |
| `StartMsgFilter` | 0, filter 0 | 0, filter 0 | agree |
| `WriteMsgs $3E` | 0, sent=1 | 9, sent=0 | New; below |
| `ReadMsgs` (500 ms) | 0, received=4 | 16, received=0 | New; below |
| `StartPeriodicMsg` | 0, msgid=0 | 0 | New. The old one is a stub that transmits nothing. |
| `SetProgrammingVoltage` | 0 | the cable's answer | New. The old one is a stub that reports success. |
| `Connect` twice | 0 | 20 | New. The cable said `are 20`. |
| `Disconnect` channel 9 | 0 | 2 | New. Channel 9 was never opened. |
| `Ioctl` id `0xDEAD` | 1 | 15 | New. `ERR_INVALID_IOCTL_ID` is the specified code. |
| `StartMsgFilter(NULL mask)` | segfault | 4 | New; below |
| `StopMsgFilter`, `Disconnect`, `Close` | not reached | 0 | |

### The three that matter

**`WriteMsgs` reported success for a transmit the cable rejected.** The wire
trace shows the cable's answer was `are 9`:

```
OUT ep=0x02 timeout=1000 len=17 |att6 6 64\r\n\x00\x00\x07\xe0>\x00|
IN  ep=0x82 timeout=0    len=7  |are 9\r\n|          <- ERR_TIMEOUT
```

The old driver returned 0 and `sent=1`. A caller writing to an ECU cannot tell a
delivered command from one that never reached the bus. During a reflash, that is
the difference between a completed erase and a silent stall.

**`ReadMsgs` returned four messages that do not exist.** There was no bus and
the trace has no inbound message frame. The old driver writes back through
`pMsg` and `pNumMsgs` only when it has at least as many messages as requested:

```c
if (rcvBufIndex >= *pNumMsgs) { memcpy(pMsg, ...); /* pNumMsgs updated here */ }
return 0;
```

A short read skips that and returns `STATUS_NOERROR` with `*pNumMsgs` still
holding the requested count and `pMsg` untouched, so the caller consumes
whatever was already in its buffer as bus traffic. Its overflow report can never
fire either: `rcvBufIndex` is `uint32_t`, so `rcvBufIndex < 0` is never true, and
the index is capped at 8, so `rcvBufIndex > 8` is never true. Messages dropped
when the eight-slot array fills are lost without a report.

**`StartMsgFilter(NULL mask)` crashes the old driver.** It dereferences
`pMaskMsg` before checking it.

### On the wire

Both drivers use the same commands: `ato6 0 500000 0`, `ats6 30 0`, `atf6 3 0 4`
plus 12 payload bytes, `att6 6 64` plus payload, `atc6`, `atz`. This driver also
sends the arguments Tactrix's DLL sends: a transmit budget on `att` and a
sequence number on every command ([PROTOCOL.md §3, §4](PROTOCOL.md)). Beyond
that:

| | Old | New |
|---|---|---|
| Opening | `\r\n\r\nati\r\n`, then `ata` | `\r\n\r\n`, `atz`, `ata`, `ati`; every command numbered and its reply matched by number |
| Transfer timeouts | 0 on most transfers: blocks forever | every transfer bounded |
| Command and payload | one transfer | two (line, then payload); same bytes, same order |
| Reads | on demand, inside `ReadMsgs` | a reader thread polling every 10 ms, so message frames and replies are separated instead of one being lost behind the other |

The old driver's zero timeouts are why a wedged cable hangs the application
instead of returning `ERR_TIMEOUT`.

## Deliberate differences

Each is a choice to be correct rather than compatible with the old driver.

| Difference | Why |
|---|---|
| Nothing returns `STATUS_NOERROR` for work it did not do. The one unsupported facility, the J1850 functional-message table, returns `ERR_NOT_SUPPORTED`. | A caller using `StartPeriodicMsg` for a TesterPresent keep-alive on the old driver got silence and believed it was transmitting. |
| Periodic messages are implemented, in the firmware (`atm`/`atn`), as Tactrix's DLL does. | The old driver's are stubs. |
| `SetProgrammingVoltage` is implemented (`atv`), one pin at a time as J2534-1 §7.2.11 requires. | The old driver's is a stub. |
| `ReadMsgs` does not double a timeout below 100 ms. | The old driver silently doubled the caller's timeout for K-line. |
| Every libusb call is checked and every timeout bounded. | 19 of 26 transfers in the old driver ignore the return code; 20 of 26 use timeout 0. |
| `GetLastError` returns real text. | It returned an empty string. |
| IOCTLs 4, 9, 10 and 14 are implemented. | They were missing. |
| An unknown IOCTL returns `ERR_INVALID_IOCTL_ID` (15). | The old one returned `ERR_NOT_SUPPORTED` (1). |
| Errors from the cable are passed through unchanged. | `are <n>` already carries the J2534 code; remapping loses information. |
| Commands are numbered and replies matched by number; a reply nobody is waiting for is discarded. | A stale reply otherwise corrupts every later result ([PROTOCOL.md §9](PROTOCOL.md)). |
| Filter messages must all be the same length. | The cable takes one length for all of them; a mismatch would shift the pattern on the wire. |
| The firmware version is parsed at the `": "` delimiter, not at byte offset 24. | Offset 24 happens to equal `strlen("ari main code version : ")`; the delimiter survives a firmware that changes the prefix. |
| `WriteMsgs` treats `Timeout` as a budget for the whole call. | Giving each message the full value let a five-message write overrun a 300 ms budget by 635 ms and still return `STATUS_NOERROR`. |
| `ReadMsgs` returns `ERR_BUFFER_OVERFLOW` after the receive queue overran, still delivering and counting the surviving messages. | Dropped messages were counted and never reported. |
| An invalid `FilterType` returns `ERR_INVALID_MSG`. | The standard names no code for this; `ERR_INVALID_MSG` is what §7.2.9.2 uses for the neighbouring malformed-argument case. |

Before 0.3.0 the driver also refused a flow-control message given with a PASS or
BLOCK filter (`ERR_INVALID_MSG`, as J2534-1 §7.2.9.2 states). It now ignores it,
as Tactrix's DLL does, because refusing it would fail an application that
passes a zeroed message instead of NULL.

## Where the standard and the cable pulled apart

| Point | Decision | Why |
|---|---|---|
| The transmit indication carries the CAN id on the wire | Report it: `DataSize` 4, `ExtraDataIndex` 0 | J2534-1 DEC2004 §8.6 says exactly that, and so does Tactrix's DLL (measured on a live bus 2026-09-13: `RxStatus 0x9`, `DataSize 4`) |
| A full receive queue: drop the oldest message or the newest? | Drop the arriving one | J2534-1 requires it, and a caller that ignores the overflow then sees a truncated but contiguous sequence, not one with an unmarked hole |
| Per-protocol message size limits | Enforce J2534-1 Figure 42 for CAN and ISO 15765; leave ISO 14230's to the cable | ISO 14230's maximum depends on the checksum flag, and rejecting a valid message would be worse than letting the cable reject it |
| Two ECUs answering a functional request with segmented replies at once | One reassembly per channel, as Tactrix's DLL keeps | Every chunk carries the CAN id ([PROTOCOL.md §7.6](PROTOCOL.md)), so routing by id would be possible, but only one ECU has ever answered in a measured session |

## A bug this harness found in the new driver

The first hardware run had `PassThruStopMsgFilter` returning `ERR_TIMEOUT` while
every unit test passed. A transmit the cable took 1.2 s to reject had been
abandoned after 200 ms, and its late `are 9` was taken by the next command,
which reported the transmit's failure as its own: [PROTOCOL.md §9.2](PROTOCOL.md)
inside the new driver. The fix was first to accept a reply only while a command
is waiting, and later to match every reply to its command by sequence number.
`late_reply_is_not_reused()` in `tests/unit/test_j2534.c` guards it.

## As a real tool sees it

Python read and write tools that load a J2534 library with `ctypes.CDLL` and use
ISO 15765 with a flow-control filter, `SET_CONFIG`, `READ_VBATT`,
`CLEAR_RX_BUFFER` and `ISO15765_FRAME_PAD` were pointed at both drivers, with
the cable on the bench and no vehicle. All fourteen entry points resolve under
`ctypes`.

| | Old driver | New driver |
|---|---|---|
| Firmware | `1.17.4877` | `1.17.4877` |
| Battery | 0.13 V | 0.13 V |
| Outcome | no response | no response |
| Reason given | `no response to service 0x23 (last=None)` | `PassThruWriteMsgs failed: rc=9 ERR_TIMEOUT` |

Same outcome, one difference that matters. The old driver swallowed the transmit
failure and presented it as a silent ECU, which sends the operator off checking
CAN ids, baud rate and session state. The new driver says the message never
reached a bus, which is what happened.

## What this does not cover

With no bus, this comparison covers transmit failure, error paths and the
command vocabulary, not received data. Received data is covered elsewhere:

- live-bus comparisons with Tactrix's DLL ([AB-OFFICIAL.md](AB-OFFICIAL.md)):
  identical raw CAN frames on a 2009 Audi, identical periodic commands,
  identical multi-frame delivery, and a live ECU on a bench;
- recordings from a 2012 VW Caddy in `tests/unit/test_golden.c`;
- the consuming tool's own acceptance rule, replayed over the Caddy exchange
  (`s_consumer_rule` in `tests/sim/run_scenarios.py`).

Running this harness on a vehicle would add nothing those have not settled.
