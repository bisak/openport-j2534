# The J2534 API on the OpenPort 2.0

This page describes each J2534 call on this driver and the OpenPort 2.0
cable, for people who write or port a J2534 tool. The library implements SAE
J2534-1 version 04.04, the revision that Tactrix's own driver reports.

## Before you start

- **Pad ISO 15765 requests.** Set the `ISO15765_FRAME_PAD` TxFlag on every
  ISO 15765 message and on the flow-control message of its filter. The cable
  pads only when asked, and many ECUs ignore a frame shorter than 8 bytes.
- **Put flow control on the ECU's physical ids.** A request sent to the
  functional id `0x7DF` still needs its flow-control filter on the ECU's own
  ids, for example `0x7E8` and `0x7E0`. Otherwise every multi-frame reply
  stalls after the first frame.
- **Declare integers as `unsigned long` in any binding.** That is 8 bytes on
  64-bit macOS and Linux and 4 bytes on Windows. On Windows the functions are
  `__stdcall`, exported under their plain names and with Tactrix's ordinals
  (alphabetical, 1 to 14).
- **Every call has a time limit.** When a call fails, `PassThruGetLastError`
  gives the reason.
- **Periodic messages run in the cable.** If your process dies, a periodic
  message keeps transmitting until the next `PassThruOpen` resets the cable.
  Tactrix's driver behaves the same.

## Functions

| Function | Behavior |
|---|---|
| `PassThruOpen`, `PassThruClose` | Both reset the cable, so no channel, periodic message, or pin voltage carries over between sessions. `PassThruClose` closes the session even when the reset does not reach the cable, and then returns the reset's failure code. |
| `PassThruConnect`, `PassThruDisconnect` | Every J2534-1 protocol the cable has, plus the J2534-2 ids `ISO9141_L`, `ISO14230_L`, `ISO9141_INNO`, and the `_CH1` aliases. Connect flags go to the cable unchanged. |
| `PassThruReadMsgs` | Waits exactly the timeout given. Each channel queues up to 1 MiB, about 29,000 raw CAN frames. An overrun returns `ERR_BUFFER_OVERFLOW` and still delivers the surviving messages. |
| `PassThruWriteMsgs` | The timeout covers the whole call and is also the cable's time budget for putting each message on the bus. `Timeout=0` queues and returns, with Tactrix's bus budgets: 50 ms on raw CAN, none on the 2.5 mm jack, and 1 s otherwise. |
| `PassThruStartPeriodicMsg`, `PassThruStopPeriodicMsg` | The cable schedules them, 10 per channel, at any interval it can run: 1 to 4,294,967 ms, not only J2534's 5 to 65535 ms. |
| `PassThruStartMsgFilter`, `PassThruStopMsgFilter` | PASS, BLOCK, and FLOW_CONTROL, 10 per channel. The driver ignores a flow-control message passed with a PASS or BLOCK filter. |
| `PassThruSetProgrammingVoltage` | See [Programming voltage](#programming-voltage). |
| `PassThruReadVersion` | Firmware version from the cable, library version `openport-j2534 <version>`, and API `04.04`. |
| `PassThruGetLastError` | Describes the last failure on the calling thread, with the call and the cause. |
| `PassThruIoctl` | See [IOCTLs](#ioctls). |

## IOCTLs

| IOCTL | Behavior |
|---|---|
| `GET_CONFIG`, `SET_CONFIG` | Passed to the cable, which supports a different set per protocol. See [Configuration parameters](#configuration-parameters). The driver processes the whole list and returns the last parameter's status, as Tactrix's driver does. |
| `READ_VBATT` | Battery voltage on OBD pin 16, in millivolts. |
| `FIVE_BAUD_INIT`, `FAST_INIT` | Sent the way Tactrix's driver sends them. `FIVE_BAUD_INIT` needs an output array for the key bytes. |
| `CLEAR_TX_BUFFER` | Does nothing, as in Tactrix's driver. A transmit is already on the wire when `PassThruWriteMsgs` returns. |
| `CLEAR_RX_BUFFER` | Empties the channel's receive queue. |
| `CLEAR_PERIODIC_MSGS`, `CLEAR_MSG_FILTERS` | One command each to the cable, as Tactrix's driver sends. |
| `CLEAR_FUNCT_MSG_LOOKUP_TABLE`, `ADD_TO_FUNCT_MSG_LOOKUP_TABLE`, `DELETE_FROM_FUNCT_MSG_LOOKUP_TABLE` | `ERR_NOT_SUPPORTED`. These are for J1850, which the cable does not have. |
| `READ_PROG_VOLTAGE` | Reads the pin that `pInput` points to: 8, 12, 16, or 17 (the programming supply). Reads pin 12 when `pInput` is NULL. |

## Configuration parameters

The cable supports these, measured on firmware 1.17.4877. Any other parameter
returns `ERR_NOT_SUPPORTED`.

| Protocol | Parameters |
|---|---|
| ISO 15765 | `DATA_RATE`, `LOOPBACK`, `BIT_SAMPLE_POINT`, `SYNC_JUMP_WIDTH`, `ISO15765_BS`, `ISO15765_STMIN`, `BS_TX`, `STMIN_TX`, `ISO15765_WFT_MAX` |
| ISO 14230 | `DATA_RATE` (read only), `LOOPBACK`, `P1_MAX`, `P3_MIN`, `P4_MIN`, `W0` to `W5`, `TIDLE`, `TINIL`, `TWUP`, `PARITY`, `DATA_BITS`, `FIVE_BAUD_MOD` |

Tactrix's own `TX_PARAM_STOP_BITS` (0x9000) works on the ISO 9141 and jack
channels. The driver sends values in J2534 units, unconverted, exactly as
Tactrix's driver does.

## Programming voltage

The cable can put 5 to 20 V on a connector pin, or short a pin to ground, for
bench and bootloader reflash procedures. The driver passes these requests on
as Tactrix's driver does, but first makes two checks that the cable does not.

- **One pin at a time.** All voltage pins share one supply, so a second pin
  would silently change the first. A voltage on a second pin returns
  `ERR_PIN_INVALID` until the first is switched off, as J2534-1 requires.
  Grounding is exempt.
- **No grounding a line in use.** Grounding K (pin 7) under an open ISO 9141
  or ISO 14230 channel, or L (pin 15) under an L-line channel, returns
  `ERR_CHANNEL_IN_USE`, as does opening such a channel while its pin is
  grounded. The cable would allow either and silently cut the channel off.

For an out-of-range voltage, the cable returns `ERR_OEM_VOLTAGE_TOO_HIGH`
(0x77) or `ERR_OEM_VOLTAGE_TOO_LOW` (0x78), and both drivers pass it through.
Pin 12 is also the tip of the 2.5 mm jack. With a plug in the jack, pin 12 is
disconnected from the vehicle and the voltage goes to the jack. Opening or closing the device switches every output off.

## Differences from Tactrix's driver

This driver behaves like Tactrix's driver except where the J2534 standard
names a different return code, or where passing a request on would let the
cable do something wrong that the application could not detect.
[AB-OFFICIAL.md](AB-OFFICIAL.md) has the full list, with measurements.

### Return codes that follow the standard

| Case | This driver | Tactrix's driver |
|---|---|---|
| A call before `PassThruOpen` or after `PassThruClose` | `ERR_INVALID_DEVICE_ID` | `ERR_INVALID_CHANNEL_ID` |
| A NULL filter message | `ERR_NULL_PARAMETER` | `ERR_FAILED` |
| An unknown IOCTL | `ERR_INVALID_IOCTL_ID` | `ERR_INVALID_CHANNEL_ID` |
| `pNumMsgs` after a failed write | The number sent | The caller's value, unchanged |
| A write, a periodic message, or a filter's flow-control message whose `ProtocolID` is not the channel's protocol | `ERR_MSG_PROTOCOL_ID` | Reroutes a write by its protocol. See [Routing](#routing). |
| Connecting ISO 14230 while ISO 9141 holds K, or the reverse, and the same on L | `ERR_CHANNEL_IN_USE`, without asking the cable | `ERR_INVALID_PROTOCOL_ID`, the cable's own `are 3` |
| A periodic message with an interval of 0 | `ERR_INVALID_TIME_INTERVAL` | Passes it on. The cable accepts it and never sends it. |
| A K-line init that the ECU did not answer | `ERR_INIT_FAILED` (0x21), a code from a later revision of the standard | Success, measured against a bare `aro` |
| Any call once the cable is gone | `ERR_DEVICE_NOT_CONNECTED`, at once | `ERR_TIMEOUT` from every call, read from its code |

Neither driver checks the `ProtocolID` of a filter's mask or pattern.

Once the cable is gone, the next `PassThruOpen` closes the dead session
itself. In a test on 2026-10-07 that unplugged the USB lead mid-session, the
call in flight returned within a millisecond, and the reopen after replugging
needed no `PassThruClose`.

### Requests refused before they reach the cable

Tactrix's driver passes all of these on.

- A baud rate of 0. The cable would open CAN at 0 baud.
- An ISO 15765 message over 4099 bytes, or over 4100 bytes with
  `ISO15765_ADDR_TYPE`. The cable would try to send it.
- A periodic message outside its protocol's size limits. The cable accepted a
  3-byte CAN one and repeated it every interval.
- The two programming-voltage requests in
  [Programming voltage](#programming-voltage).

### Routing

- A message goes out on the channel it was written to, and must carry that
  channel's protocol. Tactrix's driver picks the channel from the message's
  `ProtocolID` instead.
- With both a raw CAN and an ISO 15765 channel open, frames that the cable
  reports for CAN stay on the CAN channel. Tactrix's driver moves them to the
  ISO 15765 channel.
- With `LOOPBACK` on ISO 15765, the driver delivers the echo and the TxDone
  indication in the order the cable reports them. The timestamps show this is
  the true order. Tactrix's driver delivers TxDone first, as J2534-1
  describes. Reordering would hide the measurement, so this exception to the
  rule above is deliberate.

### NULL inputs

Tactrix's driver crashes the calling process when `GET_CONFIG`, `SET_CONFIG`,
or `FAST_INIT` gets a NULL input. This driver returns `ERR_NULL_PARAMETER` for
`GET_CONFIG` and `SET_CONFIG`. For `FAST_INIT` it sends the wake-up pattern
alone (`aty<ch> 0 0`), which is what J2534 defines a NULL input to mean.
Firmware 1.17.4877 makes the pulse and then answers `are 7`, so the call
returns `ERR_FAILED` ([PROTOCOL.md §7.9](PROTOCOL.md)).

## Known gaps

- No K-line ECU has answered in tests so far, so K-line, including
  `FIVE_BAUD_INIT` and `FAST_INIT`, is unconfirmed on a vehicle. The frame
  layout comes from other implementations ([PROTOCOL.md §7.9](PROTOCOL.md)).
- Every known K-line use of this cable, Tactrix's own logger sample included,
  connects with `ISO9141_NO_CHECKSUM` and computes checksums in the
  application. Do the same, because the cable's own checksum handling is
  unobserved.
- The L-line and jack channels open as Tactrix's driver opens them, but no
  device on them has been seen to send data. The driver assumes their frames
  have the K-line layout.
- `ISO15765_ADDR_TYPE` on a received message follows status bit `0x04` and a
  5-byte header, as Tactrix's driver reads them. No ECU that uses extended
  addressing has been captured on this cable.
- The driver keeps the last-error text per thread. J2534-1 describes one for
  the process.
