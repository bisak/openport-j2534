# The J2534 API on the OpenPort 2.0

What each J2534 call does with this driver and this cable, for people writing
or porting a J2534 tool. The library implements SAE J2534-1 version 04.04, the
revision Tactrix's own driver reports. Where the two drivers behave differently,
this page says so.

## Before you start

- **Pad ISO 15765 requests.** Set the `ISO15765_FRAME_PAD` TxFlag on every
  ISO 15765 message and on the flow-control message of its filter. The cable
  pads only when asked, and many ECUs ignore a frame shorter than 8 bytes.
- **Address flow control to the ECU's physical id.** A request sent to the
  functional id `0x7DF` still needs its flow-control filter on the ECU's own
  ids (for example `0x7E8`/`0x7E0`), or every multi-frame reply stalls after the
  first frame.
- **Integers are `unsigned long`**, 8 bytes on 64-bit macOS and Linux. Declare
  them that way in any binding.
- **Nothing blocks forever.** Every call has a time limit. When one fails,
  `PassThruGetLastError` gives the reason.
- **Periodic messages run in the cable.** If your process dies, a periodic
  message keeps transmitting until the next `PassThruOpen`, which resets the
  cable. The same is true of Tactrix's driver.

## Functions

| Function | Notes |
|---|---|
| `PassThruOpen`, `PassThruClose` | Both reset the cable: no channel, periodic message or pin voltage carries over between sessions. |
| `PassThruConnect`, `PassThruDisconnect` | All J2534-1 protocols the cable has, plus the J2534-2 channel ids (`ISO9141_L`, `ISO14230_L`, `ISO9141_INNO` and the `_CH1` aliases). Connect flags go to the cable unchanged. |
| `PassThruReadMsgs` | The timeout is honoured exactly. Each channel queues up to 1 MiB (about 29,000 raw CAN frames); an overrun returns `ERR_BUFFER_OVERFLOW` with the surviving messages still delivered. |
| `PassThruWriteMsgs` | The timeout covers the whole call, and goes to the cable as its time budget for getting each message onto the bus. `Timeout=0` queues and returns. On failure, `pNumMsgs` holds the number actually sent. |
| `PassThruStartPeriodicMsg`, `PassThruStopPeriodicMsg` | Scheduled by the cable, 10 per channel. Any interval the cable can run (up to 4,294,967 ms), not only J2534's 5–65535 ms. |
| `PassThruStartMsgFilter`, `PassThruStopMsgFilter` | PASS, BLOCK and FLOW_CONTROL, 10 per channel. A flow-control message passed with a PASS or BLOCK filter is ignored. |
| `PassThruSetProgrammingVoltage` | See [Programming voltage](#programming-voltage). |
| `PassThruReadVersion` | Firmware version from the cable; library version `openport-j2534 <version>`; API `04.04`. |
| `PassThruGetLastError` | A description of the last failure, naming the call and the cause. |
| `PassThruIoctl` | See below. |

## IOCTLs

| IOCTL | Notes |
|---|---|
| `GET_CONFIG`, `SET_CONFIG` | Passed to the cable, which supports a different set per protocol ([below](#configuration-parameters)). The whole list is processed and the call returns the last parameter's status, as Tactrix's driver does; an unsupported parameter answers `ERR_NOT_SUPPORTED`. |
| `READ_VBATT` | Battery voltage on OBD pin 16, in millivolts. |
| `FIVE_BAUD_INIT`, `FAST_INIT` | Sent the way Tactrix's driver sends them. Not yet confirmed against a K-line ECU. |
| `CLEAR_TX_BUFFER`, `CLEAR_RX_BUFFER` | Clear the driver's queues. |
| `CLEAR_PERIODIC_MSGS`, `CLEAR_MSG_FILTERS` | One command to the cable each, as Tactrix's driver sends. |
| `CLEAR_FUNCT_MSG_LOOKUP_TABLE` and the other two table IOCTLs | `ERR_NOT_SUPPORTED`: they are for J1850, which the cable does not have. |
| `READ_PROG_VOLTAGE` | Reads the pin `pInput` points to (8, 12, 16, or 17 for the programming supply), or pin 12 when `pInput` is NULL. |

## Configuration parameters

Measured on the cable (firmware 1.17.4877). Anything else answers
`ERR_NOT_SUPPORTED`.

| Protocol | Parameters |
|---|---|
| ISO 15765 | `DATA_RATE`, `LOOPBACK`, `BIT_SAMPLE_POINT`, `SYNC_JUMP_WIDTH`, `ISO15765_BS`, `ISO15765_STMIN`, `BS_TX`, `STMIN_TX`, `ISO15765_WFT_MAX` |
| ISO 14230 | `DATA_RATE` (read only), `LOOPBACK`, `P1_MAX`, `P3_MIN`, `P4_MIN`, `W0`–`W5`, `TIDLE`, `TINIL`, `TWUP`, `PARITY`, `DATA_BITS`, `FIVE_BAUD_MOD` |

Tactrix's own `TX_PARAM_STOP_BITS` (0x9000) works on the ISO 9141 and jack
channels. Values go to the cable in J2534 units, unconverted, exactly as
Tactrix's driver sends them.

## Programming voltage

The cable can put 5–20 V on a connector pin, or short it to ground, for bench
and bootloader reflash procedures. The driver passes these requests on as
Tactrix's driver does, with two exceptions the cable itself does not enforce:

- **One pin at a time.** All voltage pins share one supply, so setting a second
  pin would silently change the first. A second pin returns `ERR_PIN_INVALID`
  until the first is switched off, as J2534-1 requires.
- **No grounding a line in use.** Grounding K (pin 7) under an open ISO 9141 or
  ISO 14230 channel, or L (pin 15) under an L-line channel, returns
  `ERR_CHANNEL_IN_USE`, and so does opening such a channel while its pin is
  grounded. The cable would allow it and silently cut the channel off.

Out-of-range voltages return `ERR_OEM_VOLTAGE_TOO_LOW` or
`ERR_OEM_VOLTAGE_TOO_HIGH` from the cable. Pin 12 is also the tip of the
2.5 mm jack: with a plug in the jack, pin 12 is disconnected from the vehicle
and the voltage goes to the jack. Opening or closing the device switches every
output off.

## Differences from Tactrix's driver

The goal is to behave like Tactrix's driver. It differs only where the J2534
standard names a different return code, or where passing a request on would let
the cable do something wrong that the application could not detect.

Return codes that follow the standard:

- A call before `PassThruOpen` or after `PassThruClose` returns
  `ERR_INVALID_DEVICE_ID` (Tactrix: `ERR_INVALID_CHANNEL_ID`).
- A NULL filter message returns `ERR_NULL_PARAMETER` (Tactrix: `ERR_FAILED`).
- An unknown IOCTL returns `ERR_INVALID_IOCTL_ID` (Tactrix:
  `ERR_INVALID_CHANNEL_ID`).
- After a failed write, `pNumMsgs` holds the number sent (Tactrix leaves the
  caller's value).

Requests refused before they reach the cable (Tactrix passes them on):

- CAN at 0 baud, which the cable would open.
- An ISO 15765 message over 4099 bytes, which the cable would try to send.
- A periodic message outside its protocol's size limits. The cable accepted a
  3-byte CAN one and repeated it every interval.
- A second programming-voltage pin, or grounding a line in use.

Routing:

- A message goes out on the channel it was written to. Tactrix's driver picks
  the channel from the message's `ProtocolID`.
- With both a raw CAN and an ISO 15765 channel open, frames the cable reports
  for CAN stay on the CAN channel. Tactrix's driver moves them to the ISO 15765
  channel.

Tactrix's driver crashes the calling process when `GET_CONFIG`, `SET_CONFIG`
or `FAST_INIT` gets a NULL input. This one returns `ERR_NULL_PARAMETER` for the
first two, and for `FAST_INIT` sends the wake-up pattern alone, which is what
J2534 defines a NULL input to mean.

The full list, with measurements: [AB-OFFICIAL.md](AB-OFFICIAL.md).

## Known gaps

- K-line has not been confirmed on a vehicle: no K-line ECU answered in the
  tests so far. The frame layout comes from other implementations
  ([PROTOCOL.md §7.9](PROTOCOL.md)).
- The L-line and jack channels open as Tactrix's driver opens them, but no
  device on them has been seen to send data.
