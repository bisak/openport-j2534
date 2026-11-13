# Testing against Tactrix's own driver

Tactrix's Windows driver, `op20pt32.dll`, is the one program written against the
cable's firmware by its authors. This harness runs it under Wine in a Docker
container on an Apple Silicon Mac, against the same simulator or cable as this
library. It makes the same calls through both drivers and compares their return
values and wire traffic.

## Results

Against the bench cable on 2026-09-24 (DLL 1.02.0.4868, firmware 1.17.4877):

| Run | Steps | Identical on the wire |
|---|---|---|
| `sweep`: every J2534 parameter, with values J2534 allows | 670 | 648 |
| `edge`: values J2534 does not allow | 35 | 23 |

[Where the two drivers differ](#where-the-two-drivers-differ) lists every
difference and its reason.

On 2026-09-24, a read-only replay on the bench ECU of
[PROTOCOL.md §10](PROTOCOL.md) covered raw CAN, segmented ISO 15765 reads,
`LOOPBACK`, a firmware periodic message, and multi-frame requests of 10, 20,
and 100 bytes. Both drivers sent identical commands and delivered every
reply and indication identically, with the same `RxStatus` and
`ExtraDataIndex`.

A reflash tool's recording, replayed against a 512 KB bootloader model, put the
same bytes on the wire through both drivers in 21,497 calls.

On a Windows PC on 2026-11-12, both drivers ran the default scenarios and
`sweep`. They gave the same results in over 1,450
steps, except the [return codes](#return-codes) below
([TESTING.md](TESTING.md#on-a-windows-pc)).

## Where the two drivers differ

This driver copies Tactrix's except where J2534 names a different return
code, or where passing a request on would let the cable do something wrong that
the application could not detect. `<ch>` is the firmware channel.

### Return codes

| Case | Tactrix's DLL | This driver |
|---|---|---|
| `PassThruDisconnect(9)` before any open | `ERR_INVALID_CHANNEL_ID` (2) | `ERR_INVALID_DEVICE_ID` (26), J2534-1 §7.2.1 |
| `PassThruIoctl(deviceId, 0xDEAD)` | `ERR_INVALID_CHANNEL_ID` (2) | `ERR_INVALID_IOCTL_ID` (15) |
| `PassThruStartMsgFilter` with a NULL mask | `ERR_FAILED` (7) | `ERR_NULL_PARAMETER` (4) |
| Mask and pattern of different lengths | `ERR_FAILED` | `ERR_INVALID_MSG` |
| NULL pattern; flow-control filter with no flow-control message | `ERR_FAILED` | `ERR_NULL_PARAMETER` |
| `FIVE_BAUD_INIT` with zero bytes | `ERR_INVALID_MSG` | `ERR_NULL_PARAMETER` |
| Flow-control message with different TxFlags from mask and pattern | `ERR_FAILED`, nothing sent | installed with the flow-control message's flags |
| A transmit the cable times out (`are 9`) | `ERR_TIMEOUT`, `NumMsgs` left as the caller set it (3 for a three-message write that sent one) | `ERR_TIMEOUT`, `NumMsgs` 0: the number sent, per J2534-1 |
| `CLEAR_FUNCT_MSG_LOOKUP_TABLE` | 0, nothing sent | `ERR_NOT_SUPPORTED` |
| `FIVE_BAUD_INIT` answered with a bare `aro` (the simulator's modelled reply) | 0 | `ERR_INIT_FAILED`: no key bytes means no init |
| `READ_PROG_VOLTAGE` with `pInput` NULL, as J2534-1 passes it | -1, nothing sent | reads pin 12 |
| `PassThruReadMsgs(timeout=0)` on an idle channel | returns after ~6 ms | returns in microseconds |

### Requests refused before they reach the cable

| Case | Tactrix's DLL | This driver |
|---|---|---|
| CAN at rate 0; an ISO 15765 write of 4100 bytes; a 3-byte CAN periodic | sent; the cable accepts all three (the 4100-byte message is attempted and times out; the malformed periodic repeats) | `ERR_INVALID_BAUDRATE`, `ERR_INVALID_MSG` |
| CAN writes of 0, 3, or 13 bytes; ISO 15765 writes of 3 bytes, or 4 with extended addressing | sent; the cable answers `are 10` | refused, same return code |
| Filter types 0 and 4 | sent; the cable answers `are 22` | `ERR_INVALID_MSG` |
| J1850 connect; an eleventh periodic | sent; `are 3`, `are 12` | refused, same return code (an eleventh filter is sent, as by Tactrix) |
| SCI and other ids with no firmware channel | `ato0`, `are 3`, `ERR_INVALID_PROTOCOL_ID` | same code, nothing sent |
| A second programming-voltage pin | `atv`, 0 | `ERR_PIN_INVALID` |
| Grounding K (`atv 7 -2`) under an open ISO 9141 channel | sent, 0; the channel stops working | `ERR_CHANNEL_IN_USE` |
| Connecting ISO 14230 while K is grounded | 0 | `ERR_CHANNEL_IN_USE` |

### Crashes

| Case | Tactrix's DLL | This driver |
|---|---|---|
| `GET_CONFIG` or `SET_CONFIG` with `pInput` NULL | the process dies (read fault at address 0) | `ERR_NULL_PARAMETER` |
| `FAST_INIT` with `pInput` NULL (J2534: wake-up pattern only) | the process dies (write fault at 0x11) | sends `aty<ch> 0 0` |

The sweep skips these three cases so the DLL's process survives.

### Routing

| Case | Tactrix's DLL | This driver |
|---|---|---|
| A message whose `ProtocolID` differs from its channel's | picks the firmware channel from the message: CAN on an ISO 15765 channel goes out as `att5`; ISO 15765 on a CAN channel returns `ERR_INVALID_PROTOCOL_ID` | refuses it with `ERR_MSG_PROTOCOL_ID`, the standard's code |
| A filter mask or pattern whose `ProtocolID` differs from its channel's | ignores the id: an ISO 15765 mask on a CAN channel goes out as `atf5`, a CAN mask on an ISO 15765 channel as `atf6`, both 0 | the same; only the flow-control message's `ProtocolID` is checked, as the DLL's code checks it |
| Raw CAN and ISO 15765 channels both open | every channel-5 frame goes to the ISO 15765 channel, none to the CAN channel | channel-5 frames stay on the CAN channel |
| `LOOPBACK` on ISO 15765: order of echo and TxDone | TxDone first | wire order (the timestamps show the true order) |

### Wire differences that follow from these

- The firmware numbers filters and periodic messages in sequence, so one extra
  on either side shifts the id that every later `atk` or `atn` names. Periodic
  ids keep counting across sessions while the cable is powered.
- Multi-message transmit budgets differ by the time already spent.
- At open, this driver sends `\r\n\r\n`, `atz`, `ata`, `ati`, and Tactrix's
  sends `\r\n\r\n`, `ati`, `ata`. Close differs too.
- If `ati` gets no reply, the DLL sends `tbi\n` (LF only) three times, 300 ms
  apart, then returns `ERR_DEVICE_NOT_CONNECTED`. The running firmware does not
  answer `tbi`, which is probably a bootloader query. This driver sends no
  `tbi`.
- The DLL's ISO 9141 open ends with a value that varies between runs, the
  handle of the ISO 14230 channel it had just closed. The firmware ignores it,
  and this driver does not send it.

### Timing

The DLL runs interpreted, so only the waits it chooses are comparable. It waits
500 ms for most replies and 5 s for `READ_VBATT` and `GET_CONFIG`, then returns
`ERR_TIMEOUT`. `ReadMsgs(100)` and `ReadMsgs(500)` return at 107 and 508 ms,
against this driver's 99.5 and 501 ms.

## What Tactrix's DLL sends

These commands come from `vendor.tap` and the cable runs. This driver sends the
same, except at open and close. `<n>` is the sequence number.
[PROTOCOL.md §4](PROTOCOL.md) has the full command list.

| Call | Command |
|---|---|
| Every command after the first `ati` | ends with a number, from 2, that the reply echoes: `aro <n>`, `arf6 0 <n>`, `are 1 9` |
| Connect | `ato6 0 500000 0 4` |
| `SET_CONFIG`, `GET_CONFIG` | one `ats` or `atg` per parameter, value unconverted (`P1_MAX` 40 is `ats4 7 40`). The call returns the last one's status. |
| Start filter | `atf6 3 64 4 6` |
| Write, `Timeout=1000` | `att6 6 64 1000000 7` plus the payload |
| Write, `Timeout=0` | `att6 6 64 1000000`, unnumbered, without waiting |
| Periodic message every 100 ms | `atm6 100000 0 64 6 <n>` plus the payload, answered `arm6 <id> <n>` |
| Stop filter, stop periodic | `atk6 0 8`, `atn6 <id> <n>` |
| `CLEAR_MSG_FILTERS`, `CLEAR_PERIODIC_MSGS` | `atk<ch> -1`, `atl<ch>` |
| Disconnect, close | `atc6 9`, `atz 10` |
| `READ_VBATT`, `READ_PROG_VOLTAGE` with a pin in `pInput` | `atr 16 3`, `atr <pin>` |
| `SetProgrammingVoltage(pin, VOLTAGE_OFF)` | `atv 12 -1` |
| `FIVE_BAUD_INIT` to 0x33 | `atw3 51` |
| `CLEAR_RX_BUFFER`, `PassThruReadVersion` | nothing. The version comes from the `ati` reply kept from open. |

`att`'s fourth argument is the firmware's budget for getting the message onto
the bus: the caller's `WriteMsgs` `Timeout` in microseconds, with 1,000,000 for
0. An unnumbered `att` gets no reply, not even `are 9` on a dead bus, and the
firmware answers nothing else for about a second. So the DLL's
`PassThruDisconnect` straight after one returns `ERR_TIMEOUT`.

Long ISO 15765 replies repeat the CAN id in every chunk
([PROTOCOL.md §7.6](PROTOCOL.md)), and both drivers strip it. Fed a 600-byte
reply with the id in the first chunk only, the DLL delivered 598 bytes.

Both drivers also agree on every connect flag and rate, every J2534-2 channel
id, every transmit flag, flow-control and K-line filters, and the return codes
of the `errors` scenario.

## Run the comparison

You need an Apple Silicon Mac, Docker, the
mingw-w64 cross compiler, and a copy of `op20pt32.dll`.

```bash
brew install mingw-w64
# against the simulator
make ab-official AB_DLL=/path/to/op20pt32.dll
# against the cable
make ab-official-cable AB_DLL=/path/to/op20pt32.dll CABLE=/dev/cu.usbmodemXXXX
```

`make` passes `AB_ARGS` to `ab.sh`. Put scenario names after `--`:

| Scenario | Covers |
|---|---|
| `open`, `iso15765`, `multiframe`, `timeouts`, `errors`, `periodic`, `raw_can` | the default set |
| `kline` | K-line connect and init |
| `bench` | N write/read cycles, for timing (`--cycles 50`) |
| `sweep_connect`, `sweep_config`, `sweep_filter`, `sweep_tx`, or `sweep` for all four | every J2534 parameter, one at a time |
| `edge` | values J2534 does not allow; run it on its own, last |

```bash
make ab-official AB_DLL=/path/to/op20pt32.dll AB_ARGS="-- open bench --cycles 50"
make ab-official-cable AB_DLL=/path/to/op20pt32.dll CABLE=/dev/cu.usbmodemXXXX AB_ARGS="-- sweep"
make ab-official-cable AB_DLL=/path/to/op20pt32.dll CABLE=/dev/cu.usbmodemXXXX AB_ARGS="-- edge"
```

On the cable, run `edge` last, with nothing on the OBD connector. It writes no
flash and switches no voltage, so the worst case is a cable that stops
answering until it is unplugged. Sweep transmits carry only read-only
requests.

Output goes to `tools/ab-official/out/`:

| File | Contents |
|---|---|
| `report.md` | the comparison, from `ab_diff.py` |
| `vendor.jsonl`, `ours.jsonl` | one line per call: arguments, return code, error text, outputs, microseconds |
| `vendor.tap`, `ours.tap` | every byte in both directions, timestamped |
| simulator logs | what the simulator received and answered |

Each sweep step starts with `READ_PROG_VOLTAGE` for pin 17 (`atr 17`).
`ab_diff.py` cuts the wire captures at these markers and compares each step's
commands and payloads (report section 2c).

### Replay an application through both drivers

`OPENPORT_RECORD=<file>` makes this driver append one line per call, with every
input and the ids the call returned. `ab.sh --replay` runs that recording
through both drivers against identical simulators and compares the wire command
by command, apart from the open and close handshake.

```bash
tools/ab-official/record.sh out/app.rec --sim-args "--ecu-model model.py:Ecu:image.bin" -- python3 my_tool.py --lib libj2534.dylib
AB_DLL=/path/to/op20pt32.dll tools/ab-official/ab.sh --replay out/app.rec --sim-args "--ecu-model model.py:Ecu:image.bin"
```

A recording is plain text, one call per line, so you can write one by hand.
`--ecu-model FILE.py:CLASS[:IMAGE]` puts a scripted ECU behind the simulator.
The simulator constructs `CLASS(image_bytes_or_None, log=...)` once, and the
instance answers each request (`request_bytes -> response_bytes | None`).

### If the DLL does not run

- If the DLL exits silently during `LoadLibrary`, box64 is running without
  `BOX64_DYNAREC=0`.
- If it reports "no devices available" without sending a byte, the image is
  stale. `ab.sh` rebuilds it when `Dockerfile` or `iface.reg` changes.
- If it reports "no devices available" after a correct `ari` reply, the
  simulator is not echoing sequence numbers.
- If it falls back to `tbi` at open, the `ati` reply did not reach it. Check
  `wineshim.c`, which sets `ReadIntervalTimeout=MAXDWORD` so the DLL's 8191-byte
  `ReadFile` completes on the first byte.

## On the Audi

On a 2009 Audi 1.9 TDI (EDC16), both drivers delivered identical raw CAN
frames. On the K-line, no init got an answer through either driver, at eight
addresses, on two visits. The cable's K-line echo came back intact at the car,
and Tactrix's DLL failed identically, so neither the cable nor this driver is
the cause. [PROTOCOL.md §11](PROTOCOL.md) has the details.
