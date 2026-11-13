# Testing against the driver this one replaced

This driver replaced the MQBau-Engineering fork, for macOS on arm64, of the
Linux `j2534` driver by `dschultzca` and `NikolaKozina`. The fork's upstream is
deleted. At the 0.1.0 release (2026-09-13), a harness ran both drivers through
the same calls on a bench with no bus. It compared what each returned and what
each sent to the cable. Live-bus comparisons since then use Tactrix's DLL, with
a separate harness: [AB-OFFICIAL.md](AB-OFFICIAL.md).

## Findings

The sequence was: open, read the version, read the battery, connect ISO 15765
at 500 kbit, set BS and STmin, install a flow-control filter, clear the receive
buffer, write a TesterPresent (`$3E`), read, start and stop a periodic message,
set programming voltage, then deliberate error paths. The cable was on USB with
no vehicle, so every transmit was expected to fail.

The old driver had five faults:

1. It reported success for a transmit the cable rejected.
2. It returned four messages from an empty bus.
3. It crashed on a NULL filter mask, because it dereferences `pMaskMsg` before
   it checks it.
4. It blocked forever on most USB transfers: 20 of 26 use timeout 0, and 19 of
   26 ignore the return code. A cable that stopped answering hung the
   application instead of returning `ERR_TIMEOUT`.
5. It stubbed out periodic messages and programming voltage, and reported
   success for both.

For fault 1, the cable answered `are 9`, but `WriteMsgs` returned 0 and
`sent=1`:

```
OUT ep=0x02 timeout=1000 len=17 |att6 6 64\r\n\x00\x00\x07\xe0>\x00|
IN  ep=0x82 timeout=0    len=7  |are 9\r\n|          <- ERR_TIMEOUT
```

For fault 2, the old driver writes back through `pMsg` and `pNumMsgs` only when
it has at least as many messages as requested:

```c
if (rcvBufIndex >= *pNumMsgs) { memcpy(pMsg, ...); /* pNumMsgs updated here */ }
return 0;
```

A short read returns `STATUS_NOERROR`, `*pNumMsgs` unchanged, and `pMsg`
untouched, so the caller reads stale buffer contents as bus traffic. The
overflow report never fires. `rcvBufIndex` is a `uint32_t` capped at 8, so
neither `rcvBufIndex < 0` nor `rcvBufIndex > 8` is ever true.

In the return codes, `rc=0` is `STATUS_NOERROR`. Both drivers agreed on
`PassThruOpen` (0), `ReadVersion` (`1.17.4877`), `Ioctl READ_VBATT` (130),
`Connect ISO15765` (0, channel 6), `Ioctl SET_CONFIG` (0), and `StartMsgFilter`
(0, filter 0). In every row below, this driver is right.

| Call | Old | New | Note |
|---|---|---|---|
| `GetLastError` before open | `len=0` | `len=8` | The old one always returns an empty string. |
| `WriteMsgs $3E` | 0, sent=1 | 9, sent=0 | Fault 1 |
| `ReadMsgs` (500 ms) | 0, received=4 | 16, received=0 | Fault 2 |
| `StartPeriodicMsg` | 0, msgid=0 | 0 | The old one transmits nothing. |
| `SetProgrammingVoltage` | 0 | the cable's answer | The old one is a stub. |
| `Connect` twice | 0 | 20 | The cable said `are 20`. |
| `Disconnect` channel 9 | 0 | 2 | Channel 9 was never opened. |
| `Ioctl` id `0xDEAD` | 1 (`ERR_NOT_SUPPORTED`) | 15 (`ERR_INVALID_IOCTL_ID`) | 15 is the specified code. |
| `StartMsgFilter(NULL mask)` | segfault | 4 | Fault 3 |
| `StopMsgFilter`, `Disconnect`, `Close` | not reached | 0 | |

On the wire, both drivers send the same commands: `ato6 0 500000 0`,
`ats6 30 0`, `atf6 3 0 4` plus 12 payload bytes, `att6 6 64` plus payload,
`atc6`, and `atz`. The old driver opens with `\r\n\r\nati\r\n`, then
`ata`, and reads on demand inside `ReadMsgs`. This driver opens with
`\r\n\r\n`, `atz`, `ata`, `ati`, and reads in a thread that polls every 10 ms.
It also numbers every command and adds a transmit budget to `att`, as Tactrix's
DLL does ([PROTOCOL.md §3, §4](PROTOCOL.md)).

This driver also differs on purpose in these ways:

- Nothing returns `STATUS_NOERROR` for work it did not do. The J1850
  functional-message table, the one unsupported facility, returns
  `ERR_NOT_SUPPORTED`.
- `ReadMsgs` does not double a timeout below 100 ms. The old driver did for
  K-line.
- IOCTLs 4, 9, 10, and 14 are implemented. The old driver lacked them.
- Errors from the cable pass through unchanged, because `are <n>` already
  carries the J2534 code.
- `WriteMsgs` treats `Timeout` as a budget for the whole call. With the full
  value per message, a five-message write overran a 300 ms budget by 635 ms
  and still returned `STATUS_NOERROR`.
- A full receive queue drops the arriving message, as J2534-1 requires.
  `ReadMsgs` then returns `ERR_BUFFER_OVERFLOW` and still delivers the
  surviving messages. The old driver never reported dropped messages.

The harness also found a bug in this driver. `PassThruStopMsgFilter` returned
`ERR_TIMEOUT` because it took the late `are 9` of a transmit that the driver
had abandoned after 200 ms and the cable rejected after 1.2 s
([PROTOCOL.md §9.2](PROTOCOL.md)). Replies are now matched by sequence number,
and `late_reply_is_not_reused()` in `tests/unit/test_j2534.c` guards it.

Python read and write tools that load the library with `ctypes.CDLL` got no
response on both drivers. The old driver's reason was
`no response to service 0x23 (last=None)`. This driver's was
`PassThruWriteMsgs failed: rc=9 ERR_TIMEOUT`, which is what happened.

With no bus, this comparison covers transmit failure, error paths, and the
command vocabulary, not received data. Received data is covered by the
live-bus comparisons in [AB-OFFICIAL.md](AB-OFFICIAL.md), the 2012 VW Caddy
recordings in `tests/unit/test_golden.c`, and `s_consumer_rule` in
`tests/sim/run_scenarios.py`.

## Run the harness

The harness runs on macOS only, because the USB traffic recorder is a
`DYLD_INSERT_LIBRARIES` interposer.

```bash
# the part that needs no cable
make differential OLD_DRIVER=/path/to/libj2534.dylib
# the full sequence, cable attached
make differential OLD_DRIVER=/path/to/libj2534.dylib DIFF_ARGS=--hardware
```

Output goes to `tests/differential/out/`. The run is safe on a bench, because it
switches programming voltage off and never on.

`tests/differential/diff_runner.c` loads each library with `dlopen` and runs
the sequence. `tools/usbtap/usbtap.c` interposes `libusb_bulk_transfer` and
logs the direction, endpoint, timeout, result, and bytes of each transfer.

Do not set `DYLD_LIBRARY_PATH`. Both libraries are named `libj2534.dylib`, and
the variable overrides even an absolute `dlopen` path by leaf name. The runner
checks the path that `dladdr` resolved, and an empty result or a missing trace
is a fatal error.
