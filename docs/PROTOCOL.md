# The OpenPort 2.0 wire protocol

How a host talks to a Tactrix OpenPort 2.0 over USB. Tactrix publishes no
protocol specification, SDK or source, so everything here was found by
observation: sweeping the cable, recording vehicles, and running Tactrix's own
Windows DLL against the same cable ([AB-OFFICIAL.md](AB-OFFICIAL.md)).

Measured on an OpenPort 2.0 with firmware 1.17.4877, on macOS 26.6 (Apple
Silicon) with libusb 1.0.29, against Tactrix's `op20pt32.dll` 1.02.0.4868.
Live-bus measurements come from a 2012 VW Caddy (ISO 15765, raw CAN), a 2009
Audi 1.9 TDI (raw CAN, TP2.0; its K-line never answered) and a production
chassis ECU on a bench harness (§10). Bench results did not change between USB
power and 11.9 V on pin 16.

Each section is marked with how sure the claim is:

| Marker | Meaning |
|---|---|
| **[V]** | Verified on the cable above |
| **[P]** | Partly verified: the shape is confirmed, a detail is not |
| **[U]** | Unverified: inferred, needs a bus to confirm |

[§12](#12-what-is-still-open) lists what is still open, and
[§13](#13-readings-the-cable-refuted) the earlier readings the cable refuted.

## Summary

For someone writing a driver, the essentials:

- The cable is a USB CDC-ACM serial device. Talk to it over the bulk endpoints
  with libusb, or through its serial device node (§1).
- The host sends ASCII command lines, `at<verb><channel> <args>\r\n`, some
  followed by binary payload. The cable sends ASCII replies (`ar<verb> …`) and
  binary message frames (`ar<digit><len>…`) on the same pipe, interleaved (§2).
- Append a sequence number to every command; the cable echoes it in the reply.
  That is how Tactrix's DLL, and this driver, match replies to commands (§3).
- Error replies carry J2534 return codes unchanged (§6).
- The cable does ISO-TP segmentation and flow control itself. The host sends and
  receives whole messages (§5).
- A segmented message arrives as a START frame with the CAN id only, then the
  data in chunks of up to 70 bytes, each repeating the CAN id (§7.3, §7.6).
- Every successful transmit produces a transmit-indication frame (§7.5). Do not
  mistake it for received data.
- A malformed command can knock the parser out of step, and a late reply can be
  mistaken for the next command's (§9).

---

## 1. USB layer **[V]**

`0403:cc4d`, manufacturer "Tactrix", product "OpenPort 2.0", USB 1.1,
bus-powered at 100 mA, one configuration:

```
bDeviceClass 0x02 (CDC communications)
  interface 0  class 0x02 sub 0x02 proto 0x01   (CDC ACM, "AT commands V.250")
    endpoint 0x81  interrupt IN   16 bytes      (notification element)
  interface 1  class 0x0a                       (CDC data)
    endpoint 0x02  bulk OUT       64 bytes
    endpoint 0x82  bulk IN        64 bytes
```

It is a standard CDC-ACM serial device; protocol `0x01` is "AT commands", which
is why every command starts with `at`. There are two ways in:

1. **libusb on the bulk endpoints.** This is what the driver does.
2. **The serial device node**, `/dev/cu.usbmodem<serial>1` on macOS or
   `/dev/ttyACM0` on Linux. No libusb and no claiming; convenient for scripts
   and protocol work. `OPENPORT_DEVICE` points the driver at it.

### Claiming the interface on macOS **[V]**

macOS's `AppleUSBCDCACM` binds both interfaces, and
`libusb_kernel_driver_active` reports 1 for each:

| Interface | `detach_kernel_driver` | `claim_interface` |
|---|---|---|
| 0 (comm) | `LIBUSB_ERROR_ACCESS` | `LIBUSB_ERROR_ACCESS` |
| 1 (data) | `LIBUSB_ERROR_ACCESS` | success |

So: claim the interface that has the bulk endpoints, never interface 0, and
treat a failed detach as expected. No `sudo`, kext removal or boot arguments are
needed. Find the interface by looking for one with a bulk IN and a bulk OUT
endpoint rather than assuming it is number 1.

### Claiming the interface on Linux **[P]**

Linux binds `cdc_acm` to the comm interface, and `cdc_acm` also claims the data
interface. Claiming the data interface with libusb means detaching the kernel
driver, and detaching either interface removes the whole serial device,
`/dev/ttyACM*` included; only re-probing the comm interface brings it back. A
driver that detaches must therefore reattach at close, comm interface first, or
the serial device stays missing until the cable is re-plugged.

This driver records which interfaces had a kernel driver at open and reattaches
them at close. That behaviour comes from a report on Linux hardware
(2026-09-21) and matches how `cdc_acm` works; this project's own cable has not
been tested on Linux hardware.

---

## 2. Framing **[V]**

Host to device: an ASCII line ending in `\r\n`, sometimes followed directly by
binary payload bytes.

```
at<verb>[<channel>] <arg> <arg> ...\r\n   [payload bytes]
```

Device to host: two kinds of data on the same pipe.

```
ar<verb> <args>\r\n                                    ASCII reply
'a' 'r' <channel-digit> <len:u8> <payload...>          binary message frame
```

- **They interleave.** A message frame can arrive between a command and its
  reply, so a driver must separate the two streams rather than expect the reply
  next.
- **Byte 2 tells them apart:** a letter for a reply, an ASCII digit for a frame.
  That is the only test needed, and it is unambiguous because channels are
  single digits. The length byte takes any value 0–255, so do not use it to
  decide.
- **An unknown command gets no reply at all**, not an error.
- **Spacing matters.** Channel commands attach the digit to the verb (`ato6 …`,
  `atc6`); `atr`, `atv` and `atp` take a space before their first argument.
  `ato 6 …` means something else and fails.

---

## 3. Replies and sequence numbers **[V]**

| Reply | Meaning |
|---|---|
| `aro` | success, no value |
| `are <code>` | failure; `<code>` is a J2534 return code (§6) |
| `are <code> <detail>` | failure, echoing the offending value |
| `ari <text>` | text, e.g. `ari main code version : 1.17.4877` |
| `arr <pin> <millivolts>` | a pin voltage |
| `arf<ch> <filter_id>` | filter installed |
| `arg<ch> <param> <value>` | a configuration value |
| `arm<ch> <id>` | periodic message started |
| `arw<ch> <b> <b> …` | five-baud init result: the ECU's key bytes, in decimal **[P]** |
| `ary<ch> <n>` + `n` raw bytes | fast init result: the ECU's response **[P]** |

The `arw` form was measured by a third party with Honda HDS on a 2005 Honda
(`Aiden-korbs/openport2-winarm-j2534`); the `ary` form is the reading of the
`dschultzca` driver family. On this bench, with no K-line ECU, both inits answer
`are 7`; a successful init has not been seen here.

**Every reply echoes the number its command carried.** Add a decimal number
after a command's arguments and the cable appends it to the reply. Tactrix's DLL
uses this to match replies to commands. Measured 2026-09-13:

```
ata 2            -> aro 2
atr 16 3         -> arr 16 108 3
atf6 3 64 4 8    -> arf6 0 8
atg6 30 10       -> arg6 30 0 10
atg6 127 9       -> are 1 9
atn6 5 14        -> are 13 14
aty3 4 0 10      -> are 7 10
```

Without a number, `arf` and `arg` end in 0 and other replies have no trailing
field. `ati` ignores a number (`ati 5` answers a plain `ari`). A successful
`ary` has not been seen with a number.

This driver numbers every command except `ati` and accepts a reply only if it
carries the command's number; a K-line init reply is accepted with or without
one. A reply with another number, or with none when one was expected, is stale
and is dropped (§9).

---

## 4. Commands **[V]**

Found by trying every `at<a..z>` and then each verb's arguments. These verbs get
no reply and do not exist: `atb atd ate ath atj atq atu`.

| Command | Arguments | Does |
|---|---|---|
| `ati` | | reports the firmware version |
| `atz` | | resets: closes every channel, stops every periodic message, switches every output off |
| `ata` | | closes every channel and switches every output off, like `atz` |
| `ato<proto>` | `<flags> <baud> 0` | opens a channel (§5) |
| `atc<ch>` | | closes a channel and stops its periodic messages |
| `att<ch>` | `<len> <txflags> <budget_us>` + payload | transmits a message |
| `atf<ch>` | `<type> <txflags> <len>` + payload | installs a filter |
| `atk<ch>` | `<filter_id>` | removes a filter; `-1` removes all of them |
| `atg<ch>` | `<param>` | reads a configuration value (§8) |
| `ats<ch>` | `<param> <value>` | writes a configuration value (§8) |
| `atm<ch>` | `<interval_us> 0 <txflags> <len>` + payload | starts a periodic message; replies `arm<ch> <id>` |
| `atn<ch>` | `<msg_id>` | stops a periodic message |
| `atl<ch>` | | stops every periodic message on the channel |
| `atr` | ` <pin>` | reads a pin voltage in millivolts (§8) |
| `atv` | ` <pin> <millivolts>` | puts a voltage on a pin, or grounds it (§8) |
| `aty<ch>` | `<len> 0` + request bytes | K-line fast init |
| `atw<ch>` | `<address>`, in decimal, no payload | K-line five-baud init |
| `atp` | ` <pin> <value>` | unknown; see below |
| `atx<ch>` | `<n>` | unknown; answers `are 7 <n>` |

Notes:

- **`ata`** answers with the command's number. A filter installed before `ata`
  answers `are 2` afterwards.
- **`ato`'s fourth field is ignored.** Channels 5, 6 and 3 opened together with
  0 there, `ato5 … 7` was accepted, and a second `ato5` answers `are 20`
  whatever the field holds. Tactrix's DLL puts its own bookkeeping value there
  when opening ISO 9141.
- **`atc`** answers `aro` even for a channel that was never open.
- **`att`'s budget** is how long the cable may try to get the message onto the
  bus, in microseconds. Tactrix's DLL sends the caller's `WriteMsgs` timeout
  there, and 1,000,000 for a timeout of 0. Without it, the cable gives up after
  about a second.
- **`atf`** takes type 1 (PASS), 2 (BLOCK) or 3 (FLOW_CONTROL). `<len>` is the
  length of one of the appended messages; the cable expects two (mask and
  pattern) or three (plus the flow-control message). `atf6 3 0 12` with 12 bytes
  answers `are 10` and knocks the parser out of step (§9.1).
- **`atk`** answers `are 22` for an unknown id. `atk<ch> -1` removes every filter
  on the channel and answers `aro` even when there are none; the old ids then
  answer `are 22`. Tactrix's DLL uses it for `CLEAR_MSG_FILTERS`, and so does
  this driver. Filter ids are never reused within a session: they count up
  across all channels and restart only at `ata` or `atz` (2026-09-24).
- **`atn`** answers `are 13` for an unknown id. **`atl`** answers `aro`, and the
  old ids then answer `are 13` (2026-09-24). Tactrix's DLL uses `atl` for
  `CLEAR_PERIODIC_MSGS`, and so does this driver.
- **`aty`** returns in about 108 ms, after a 25 ms low / 25 ms high wake-up
  pulse. **`atw`** takes about 2,450 ms, one byte sent at 5 baud; `atw3 51`
  initialises address 0x33, which is how Tactrix's DLL sends it.
- **`atp`** takes the same arguments as `atv` (the DLL's format table has one
  template, `at%c %d %d %u`, for both). It answers `are 10` for every pin and
  value tried (pins 0, 1, 2, 6, 12 and 15; values 1, 5000, 8000, 12000, 20000,
  `SHORT_TO_GROUND`, `VOLTAGE_OFF`) and `are 5` for value 0, after 0.3–1.3 s. Its
  purpose is unknown, and Tactrix's DLL does not send it.

### What Tactrix's DLL sends **[V]**

Measured by running `op20pt32.dll` 1.02.0.4868 against the simulator and the
cable ([AB-OFFICIAL.md](AB-OFFICIAL.md)):

```
\r\n\r\nati                       once at open, without a number
ata 2                              every later command carries a number
atr 16 3                           READ_VBATT
ato6 0 500000 0 4                  connect
ats6 30 0 5 / ats6 31 0 6          one ats per SET_CONFIG parameter
atf6 3 64 4 7 + 12 bytes           filter: type, txflags, length of one message
att6 6 64 1000000 8 + payload      transmit, Timeout=1000 (budget = Timeout in µs)
att6 153 64 5000000 9 + payload    transmit, Timeout=5000
att6 6 64 1000000   + payload      transmit, Timeout=0: 1 s budget, no number, no wait
atm6 100000 0 64 6 9 + payload     periodic message every 100 ms (interval in µs)
atn6 0 10                          stop periodic
atk6 0 11 / atc6 12 / atz 13       stop filter / disconnect / close
atv 12 -1 14                       VOLTAGE_OFF, printed signed
ato4 0 10400 0 3 / aty4 4 0 4 + 4  ISO 14230 connect, fast init
ato3 0 10400 3 7 / atw3 51 8       ISO 9141 connect, five-baud init to 0x33
```

- The DLL waits for `aro <number>`: 500 ms for most commands, 5 s for `atr` and
  `atg`. It ignores a reply without its number.
- An `att` without a number gets no reply at all, not even `are 9` when nothing
  acknowledges, and the cable answers nothing else for about a second while it
  keeps trying. With a number, the same transmit answers `are 9 <number>` after
  1.0 s.
- When `ati` gets no answer, the DLL sends `tbi\n` (LF only) three times, 300 ms
  apart, and gives up. The running firmware does not answer it with either line
  ending, so it is probably a bootloader query.
- `PassThruReadVersion` is answered from the `ati` reply kept from open.

This driver sends the same commands with the same arguments, except that it
opens with `\r\n\r\n`, `atz`, `ata`, `ati`, and numbers a `Timeout=0` transmit
so that its late reply can be recognised and dropped.

---

## 5. Channels and protocols **[V]**

The channel number is the firmware's protocol number. `ato6` opens ISO 15765; a
second `ato6` answers `are 20` (`ERR_CHANNEL_IN_USE`); `atc6` closes it and it
can be opened again. Tactrix's DLL maps the J2534 ids onto these channels:

| Channel | J2534 ids | Line |
|---|---|---|
| 3 | `ISO9141`, `ISO9141_CH1` (`ISO9141_K`) | K, pin 7 |
| 4 | `ISO14230`, `ISO14230_CH1` (`ISO14230_K`) | K, pin 7 |
| 5 | `CAN`, `CAN_CH1` | CAN, pins 6 and 14 |
| 6 | `ISO15765`, `ISO15765_CH1` | CAN, pins 6 and 14 |
| 7 | `ISO9141_CH2` (`ISO9141_L`) | L, pin 15 |
| 8 | `ISO14230_CH2` (`ISO14230_L`) | L, pin 15 |
| 9 | `ISO9141_CH3` (`ISO9141_INNO`) | RS-232 receive on the 2.5 mm jack |

- **J1850 and SCI** (J2534 ids 1, 2 and 7–10): the DLL sends `ato0`, gets
  `are 3` and returns `ERR_INVALID_PROTOCOL_ID`; Tactrix's header marks them as
  not supported. `ato0`, `ato1`, `ato10` and `ato11` all answer `are 3`. A letter
  as the channel (`atoC`) answers `are 7` without a number: it is not parsed.
- **Protocols sharing a line exclude each other**, with `are 3` rather than
  `are 20`: `ato4` is refused while ISO 9141 holds K, `ato8` while ISO 9141
  holds L, and the other way round. Otherwise there is no limit on open
  channels: 5, 6, 7 and 9 open together, and so do 7, 3 with
  `ISO9141_K_LINE_ONLY` (`ato3 4096 …`) and 9.
- **The baud rate is not checked.** `ato5 0 123456 0`, `ato3 0 4800 0`,
  `ato4 0 9600 0` and `ato6 0 0 0` all answer `aro`; `ERR_INVALID_BAUDRATE` never
  appeared. What a wrong rate does on the bus is not measured, except that
  `ato3 512 5 0` reads back `DATA_RATE` 10400 and sends at that rate: a
  five-baud address cannot be sent as data (2026-09-24).

**The cable does ISO-TP.** The host sends a whole service request and receives a
whole reply; the cable sends first and consecutive frames, answers with
flow-control frames when receiving, and honours the ECU's block size and
separation time when sending. The host only passes the parameters on:
`ISO15765_BS` (30) and `ISO15765_STMIN` (31) for the flow control the cable
sends, `BS_TX` (34) and `STMIN_TX` (35) for what it asks of the ECU, and
`ISO15765_WFT_MAX` (37) for how many WAIT frames it accepts.

What the host does manage is the USB link above that: one command at a time; a
timed-out command's late reply thrown away, not handed to the next command
(§9); a caller's `Timeout` spent across a whole `WriteMsgs` call; and a receive
queue whose overruns are reported, not hidden.

---

## 6. Error codes are J2534 codes **[V]**

`are <n>` carries a J2534-1 return code as it is. Each was provoked:

| Provocation | Reply | J2534 |
|---|---|---|
| `atg6 0` (unsupported parameter) | `are 1` | `ERR_NOT_SUPPORTED` |
| `ato1 …` (J1850) | `are 3` | `ERR_INVALID_PROTOCOL_ID` |
| `atp 12 0` | `are 5` | `ERR_INVALID_IOCTL_VALUE` |
| a malformed command | `are 7` | `ERR_FAILED` |
| a transmit with nothing on the bus | `are 9` | `ERR_TIMEOUT` |
| `atf6 3 0 12` (wrong payload length) | `are 10` | `ERR_INVALID_MSG` |
| an eleventh `atm` on a channel | `are 12` | `ERR_EXCEEDED_LIMIT` |
| an eleventh live `atf` on a channel | `are 12` | `ERR_EXCEEDED_LIMIT` |
| `atn5 0` (no such periodic message) | `are 13` | `ERR_INVALID_MSG_ID` |
| `atr 0` (not a readable pin) | `are 19` | `ERR_PIN_INVALID` |
| a second `ato` on one protocol | `are 20` | `ERR_CHANNEL_IN_USE` |
| `atk6 1` (no such filter) | `are 22` | `ERR_INVALID_FILTER_ID` |
| `atf5 0 …`, `atf5 4 …` (no such filter type) | `are 22` | `ERR_INVALID_FILTER_ID` |
| `atv 12 20001` | `are 119` | Tactrix `ERR_OEM_VOLTAGE_TOO_HIGH` (0x77) |
| `atv 12 4999` | `are 120` | Tactrix `ERR_OEM_VOLTAGE_TOO_LOW` (0x78) |

Pass them through. Translating them would lose information the cable already
gave correctly.

---

## 7. Binary message frames

### 7.1 Layout **[V]**

```
  offset  size  field
       0     2  'a' 'r'
       2     1  channel, an ASCII digit ('3'..'9')
       3     1  length of everything that follows
       4     1  status (bits below)
       5     4  timestamp, microseconds, big-endian
       9   n-5  payload
```

A frame occupies `4 + len` bytes, so it carries at most 250 payload bytes. A
real one:

```
61 72 36 | 09 | 10 | 19 50 b8 0c | 00 00 07 e0
'a''r''6'  len  sts   timestamp     CAN id 0x7E0
```

The timestamp counts microseconds since power-on (two frames 1.2 s apart
differed by 1,238,083) and wraps about every 71 minutes. For CAN and ISO 15765
the payload starts with the 4-byte big-endian CAN id, as J2534's `Data[0..3]`
does, followed by the data. K-line frames differ (§7.9).

### 7.2 The status byte **[V]**

| Bit | Meaning |
|---|---|
| `0x80` | START: a segmented message begins (§7.3) |
| `0x40` | END: this frame completes a message |
| `0x20` | LOOPBACK: an echo of our own transmit (§7.7) |
| `0x10` | transmit indication (§7.5) |
| `0x02` | the CAN id is 29-bit (§7.4) |

### 7.3 What the bits mean on a live bus **[V]**

Measured over ISO 15765 at 500 kbit on a 2012 VW Caddy (2026-09-13) and a
vehicle's chassis controller (2026-06-17):

| Frames on the wire | Meaning |
|---|---|
| one `0x40` frame: id + data | a message that fitted one CAN frame; nothing comes before it |
| a `0x80` frame with the id only, then `0x40` frames with id + data | a segmented message: START announces it, and the data follows in frames that repeat the id |

Seen for every segmented reply (`$1A 87`, `$1A 9A`, `$1A 9C`, `$21 E4`,
`$22 F1 90`, `$09 02`) and every single-frame one. A `0xC0` frame (START and END
together) was never seen on receive; the driver accepts one as a complete
message, since that is what the bits say.

A driver must treat a START frame without END as an indication: in J2534 terms
`ISO15765_FIRST_FRAME`, `DataSize` 4, `ExtraDataIndex` 0, as J2534-1 §8.6 and
A.4 prescribe. Folding it into the data produces a message with the CAN id
twice, marked `START_OF_MESSAGE`, which J2534 applications discard; Tactrix's
own `canlogger` and `klogger` samples begin `dump_msg` with
`if (msg->RxStatus & START_OF_MESSAGE) return;`. `tests/unit/test_golden.c`
replays the Caddy's VIN reply so this cannot come back.

Other implementations agree. `emdzej/j2534`, written from a Ghidra disassembly
of the DLL, maps `0x80` to a `START_OF_MESSAGE` message with the id and `0x40`
to the data. `MCU-Innovations/opta-j2534-rs`, verified on a Honda CBR1000RR ECM
including a 2 MB flash transfer, reads `0x80` as the start marker and strips the
id from every later frame. `opta-j2534-rs`, `firefighter-19/tuneforge` and the
`dschultzca`/`NikolaKozina` driver are one family; only `emdzej/j2534` is
independent of it, so this is two lines of evidence, not several.

### 7.4 The low bits **[V]**

The same request sent four ways on the Caddy:

| TxFlags | Status of the transmit indication |
|---|---|
| `0x000` | `0x10` |
| `0x040` `ISO15765_FRAME_PAD` | `0x10` |
| `0x100` `CAN_29BIT_ID` | `0x12` |
| `0x140` both | `0x12` |

`0x02` follows the 29-bit id exactly and nothing else. Two third-party drivers
read it as J2534's `START_OF_MESSAGE`, which would make `0x12` "transmit done
and start of a message". The driver passes it on as `CAN_29BIT_ID` on CAN
channels. Tactrix's changelog for firmware 1.44 ("return CAN_29BIT_ID flag on
appropriate read results") agrees. `0x44` (END with `RX_BREAK`) is a
third-party claim, not measured here.

### 7.5 The transmit indication **[V]**

Every successful transmit produces one `0x10` frame, carrying the 4-byte CAN id
and no data, right after the `aro`. A driver that takes it for received data
reports a phantom message on every write. J2534-1 §8.6 defines the indication as
`TX_MSG_TYPE | TX_DONE`, `DataSize` 4, `Data` the CAN id of the message sent,
`ExtraDataIndex` 0, and that is how both Tactrix's DLL (measured on the Audi:
`RxStatus 9`, `DataSize 4`) and this driver deliver it. Periodic messages
produce none (§10).

### 7.6 Long replies arrive in 70-byte chunks **[V]**

A segmented ISO 15765 reply is forwarded as it arrives: a `0x80` announcement
with the CAN id only, then chunks of up to 70 data bytes (ten consecutive
frames), each repeating the CAN id, the last one marked `0x40`. The first chunk
holds 69 bytes: the first frame's six and nine consecutive frames' 63.

Measured on the bench ECU of §10 on 2026-09-24, over all 6,162 segmented replies
of a 512 KB `$23` read, from a raw wire log (`OPENPORT_LOG_HEX=1`). Every reply
had exactly this shape:

| Message size (id + data) | Chunks after the announcement |
|---|---|
| up to 69 bytes | one: id + all the data |
| 133 bytes (4,097 replies) | 73 + 64 |
| 259 bytes | 73 + 74 + 74 + 50 |
| 260 bytes (2,057 replies) | 73 + 74 + 74 + 51 |

The 250-byte frame limit is never reached. Tactrix's DLL strips four bytes from
every chunk after the first (fed a reply with the id in the first chunk only, it
loses four bytes per chunk; [AB-OFFICIAL.md](AB-OFFICIAL.md)), and so does this
driver. The 512 KB image read this way matched two passes at different read
sizes and an earlier dump.

### 7.7 Seeing your own transmit, and LOOPBACK **[V]**

**Without LOOPBACK.** On a raw CAN channel with a filter that passes
everything, the frame you just sent comes back as an ordinary `0x00` frame.
That is not a loopback: with a filter that excludes its id, it does not come
back. A CAN node sees its own transmission on the bus, and an open filter shows
it to you like any ECU's frame. Filter by id.

**LOOPBACK on raw CAN.** Setting `LOOPBACK` (`ats<ch> 3 1`) adds a second,
separate `0x20` frame whose payload is four zero bytes instead of the CAN id.
Measured against the bench ECU (§10, 2026-09-16). Tactrix's DLL delivers it as a
`TX_MSG_TYPE` message of four zero bytes, and so does this driver. With nothing
on the bus the transmit fails with `are 9` before any echo: the echo follows a
successful transmit.

**LOOPBACK on ISO 15765.** The echoes arrive on raw CAN channel 5, not on the
ISO 15765 channel. Measured against the bench ECU on 2026-09-24. A single-frame
request with `LOOPBACK` on the ISO 15765 channel gave, in order:

1. `ar5`, status `0x20`, four zero bytes: the echo of the request. A 20-byte
   request gave three, one per CAN frame the cable sent.
2. `ar6`, status `0x10`, the CAN id: the transmit indication.
3. While the ECU's segmented reply arrived: `ar5`, status `0x20`, the
   flow-control frame the cable sent, id and data intact
   (`00 00 07 b5 30 00 00 00 00 00 00 00`).

Nothing marked as an echo arrives on channel 6. With channel 5 open and a
filter passing the request's id, channel 5 also gets the ordinary `0x00` copies
of both frames, as above.

Tactrix's DLL delivers the `0x20` frames to the ISO 15765 channel unchanged
(`ProtocolID` CAN, `TX_MSG_TYPE`). With channel 5 also open, it delivers every
channel-5 frame to the ISO 15765 channel and none to the CAN channel. This
driver delivers them to the ISO 15765 channel only while channel 5 is closed.

### 7.8 Raw CAN frames carry no START or END **[V]**

On a raw CAN channel (5) every frame arrives with status `0x00`, our own and the
ECU's alike (Caddy, 2026-09-13). Raw CAN has no transport layer: one frame is
one whole message. A driver that waits for an END bit swallows every raw CAN
frame; `caddy_raw_can_replay` in `tests/unit/test_golden.c` guards against that.
Compared with Tactrix's DLL, the delivered messages are identical: `RxStatus` 0,
`DataSize` 8, `ExtraDataIndex` 8, CAN id first.

### 7.9 The K-line frame layout **[P]**: echoes measured, received frames not

On channels 3 and 4 (and probably 7–9) frames are not laid out like CAN frames:

| Frame | Body after the status byte |
|---|---|
| `0x00` data, `0x20` loopback data | the K-line bytes themselves, **no timestamp** |
| `0x80` start, `0xA0` loopback start | the 4-byte timestamp, no data |
| `0x40` end, `0x60` loopback end | the 4-byte timestamp, no data |
| `0x10` transmit indication | the 4-byte timestamp |

Every implementation that has run K-line on this cable describes this:

- `emdzej/j2534` (firmware 1.17.4877, from a disassembly of the DLL): "K-line
  packets have no timestamp";
- `opta-j2534-rs` (verified on a Honda PGM-FI ECM): "the firmware does not insert
  a timestamp; payload begins immediately after kind", and "the RxEnd payload is
  a 4-byte free-running timestamp, not user data";
- `tuneforge` and the `dschultzca` family.

One of them has a test fixture with an END frame that has no body at all; the
parser accepts both. Treating K-line like CAN would eat the first four bytes of
every K-line message.

The echo rows are measured (bench, nothing on pin 7, 2026-09-24). A transmit
with `LOOPBACK` on, no init first:

```
ato3 4608 10400 0 / ats3 3 1 / atf3 1 0 1 + 00 00
att3 5 0 1000000 + 68 6a f1 01 00
  ar3 05 a0 22 a6 cb 63            loopback start, timestamp
  ar3 06 20 68 6a f1 01 00         loopback data, no timestamp
  ar3 05 60 22 a7 28 b2            loopback end, timestamp 24 ms later
  aro
```

- `aro` arrives before or after the echo; both orders were seen.
- No transmit indication (`0x10`) follows a K-line transmit, with or without
  `LOOPBACK`, and with `LOOPBACK` off nothing is echoed.
- The echo passes through the transceiver, which reads pin 7, so it doubles as
  a check that the line is not held low (`car_capture.py`'s K-line echo check).
- ISO 14230 without `ISO9141_NO_CHECKSUM` appends the checksum on the wire: the
  same four bytes take 6 ms longer (one byte and one `P4_MIN` gap), and a fast
  init's reply comes 6 ms later. The echo carries the message without it.
- An `aty` fast init produces no echo of its StartCommunication bytes.
- The firmware's fast init waits about 50 ms for the answer (126 ms in all
  for a four-byte request). `aty4 0 0` makes the wake-up pulse alone and answers
  `are 7` after 52 ms; a raw `att4` can follow at once on the same channel, which
  is a fast init with as long a wait as the caller wants.
- A fast init appears to wait for 300 ms of quiet on the line first (ISO
  14230's W5, default 300 ms): `aty` sent about 250 ms after a transmit
  answered after 86–95 ms instead of 52 (the Audi, 2026-09-24).

Frames received from an ECU are still unmeasured: how the checksum byte is
delivered and how a checksum error is marked. Tactrix's changelog for firmware
1.41 says K-line echoes follow `LOOPBACK` ("you will no longer get echoes of
your commands unless LOOPBACK=1").

`tools/car/car_capture.py --kline` records every K-line frame's raw body, and
`analyse_capture.py` tests both layouts against the recording: in this one the
timestamp moves only on start and end frames, and data frames begin with a
K-line header byte.

---

## 8. Configuration and pins **[V]**

### 8.1 Configuration parameters

`atg<ch> <param>` reads and `ats<ch> <param> <value>` writes, with J2534
parameter numbers. Each protocol supports its own set; parameters 1–37 were
tried with both:

| Protocol | Supported parameters |
|---|---|
| 6, ISO 15765 | 1 `DATA_RATE`, 3 `LOOPBACK`, 23 `BIT_SAMPLE_POINT`, 24 `SYNC_JUMP_WIDTH`, 30 `ISO15765_BS`, 31 `ISO15765_STMIN`, 34 `BS_TX`, 35 `STMIN_TX`, 37 `ISO15765_WFT_MAX` |
| 4, ISO 14230 | 1 `DATA_RATE` (read only), 3 `LOOPBACK`, 7 `P1_MAX`, 10 `P3_MIN`, 12 `P4_MIN`, 14–18 `W1`–`W5`, 19 `TIDLE`, 20 `TINIL`, 21 `TWUP`, 22 `PARITY`, 25 `W0`, 32 `DATA_BITS`, 33 `FIVE_BAUD_MOD` |

- Anything else answers `are 1` (`ERR_NOT_SUPPORTED`). The driver passes every
  parameter on and returns the cable's answer.
- `W1` raised to 1000 ms is honoured: `atw` then takes 3,501 ms instead of 2,451.
- `FIVE_BAUD_MOD` 0–3 are all accepted and read back (2026-09-24).
- Tactrix's own `TX_PARAM_STOP_BITS` (0x9000) exists: `atg9 36864` reads 1 on the
  jack channel, `atg3 36864` on ISO 9141, and `ats9 36864 2` is accepted and
  reads back 2 (through the DLL, 2026-09-16).
- `SNIFF_MODE` goes to the cable in `ato`'s flags (`ato5 268435456 500000 0`) and
  is accepted, but the cable still acknowledges frames (§10). Tactrix's
  `canlogger` sample connects with `SNIFF_MODE | CAN_ID_BOTH`; this driver passes
  the flag on, as the DLL does, and logs the caveat.
- Tactrix's private IOCTLs (`TX_IOCTL_*`, 0x70000 and up) send nothing to the
  cable when called with a NULL input. Their input structures are known only
  from the `klogger` sample (`TX_IOCTL_APP_SERVICE` with service 5 and info 1
  returns the serial number). This driver does not implement them.
- `READ_PROG_VOLTAGE` takes a pin in Tactrix's DLL. J2534-1 passes `pInput` as
  NULL; the DLL reads a pin number from `pInput` and sends `atr <pin>`, and with
  NULL returns -1 and sends nothing (under emulation, 2026-09-16). This driver
  reads the named pin, or pin 12 when `pInput` is NULL.

### 8.2 Readable pins

`atr <pin>` for pins 0–20: four answer, the rest answer `are 19`. Meanings are
from Tactrix's header (`j2534_tactrix.h`); readings are measured, in
millivolts.

| Pin | Reading | Meaning |
|---|---|---|
| 8 | 0 with nothing attached | OBD pin 8 (OEM8), an analogue input |
| 12 | 0 with nothing attached | OBD pin 12 (OEM12), an analogue input; also the pin `atv 12` drives, and the tip of the 2.5 mm jack |
| 16 | 130–152 on USB power alone; 11,894 from an 11.9 V supply; 12,156–12,199 on a vehicle with a ~12.2 V battery | OBD pin 16, the battery: `READ_VBATT` |
| 17 | 5,729–5,794, drifting, with no output on | `PIN_VADJ`, the adjustable output supply that `atv` switches onto a pin; not an OBD pin |

The 2.5 mm jack takes pin 12 away from the vehicle. According to Tactrix, the
jack's tip is OEM12, and inserting a plug disconnects it from OBD pin 12; `atr
12` then reads the jack and `atv 12` drives it. The jack's ring and sleeve are
an RS-232 input for Innovate MTS devices, channel 9. **[U]**: no plug was
inserted.

### 8.3 Programming voltage and ground

`atv <pin> <millivolts>`. `VOLTAGE_OFF` (0xFFFFFFFF) and `SHORT_TO_GROUND`
(0xFFFFFFFE) are accepted signed (`-1`, `-2`, as the DLL prints them) or
unsigned:

```
atv 12 -1          -> aro        VOLTAGE_OFF
atv 12 0           -> are 120    zero is rejected; use VOLTAGE_OFF
atv 7 -2           -> aro        SHORT_TO_GROUND
atv 2 5000         -> are 19     pin 2 takes no voltage, whatever the value
atv 12 5000        -> aro        atr 12 -> 5075
atv 12 12000       -> aro        atr 12 -> 12199, atr 17 -> 12330
atv 12 20000       -> aro        atr 12 -> 20238
atv 12 20001       -> are 119    ERR_OEM_VOLTAGE_TOO_HIGH; so does 25000
atv 12 4999        -> are 120    ERR_OEM_VOLTAGE_TOO_LOW
atv 13 5000 / atv 12 9000 -> aro aro, atr 17 -> 9302: one supply for all pins
atv 0 8000         -> aro        atr 12 -> 8104: the jack drives pin 12 with no plug in
atv 12 7000 / ata  -> atr 12 -> 0: ata switches outputs off, and so does atz
```

What each pin supports, from Tactrix's header and product description:

| Pin | Supports |
|---|---|
| 0 (`AUX_PIN`, the 2.5 mm jack) | ground, voltage; drives pin 12 while no plug is in |
| 1, 3, 9, 11, 12, 13 | ground, voltage |
| 2 (J1850+) | 5 V and 8 V according to the header, but `atv 2` answers `are 19` and `atp 2` `are 10` for every value on this firmware, as expected without J1850 |
| 7 (K), 10 (J1850−), 15 (L) | ground |
| 8, 12, 16, 17 | reading (`atr`) |

What a driver has to handle, because the cable does not:

- **The range is 5,000–20,000 mV**, inclusive. Tactrix's description says 5–25 V;
  the firmware refuses anything above 20 V.
- **All voltage pins share one supply.** Setting a second pin moves the first to
  the new voltage without switching it off. J2534-1 §7.2.11 requires one pin at
  a time; this driver refuses a second pin with `ERR_PIN_INVALID` until the first
  is off. Tactrix's DLL passes it on.
- **Pin 17 reads back the supply**, within about 3 %, and keeps its last value
  after the output is switched off.
- **Grounding K cuts off the channel using it.** `SHORT_TO_GROUND` on pins 7 and
  15 answers `aro` even while ISO 9141 is open on K, and Tactrix's DLL passes the
  call on. This driver refuses to ground K under an open ISO 9141 or ISO 14230
  channel, or L under an open L-line channel, and refuses to open such a channel
  while its pin is grounded (`ERR_CHANNEL_IN_USE` both ways). L is not guarded
  under K-line channels: J2534-1 lets them use L for initialisation unless
  opened with `ISO9141_K_LINE_ONLY`, and whether this firmware drives L on
  channels 3 and 4 needs a scope on pin 15 during a five-baud init.
- **`ata` and `atz` switch every output off.** This driver sends both at open
  and `atz` at close, so a session never starts or ends with a live pin.

---

## 9. Two hazards a driver must handle

### 9.1 A bad payload knocks the parser out of step **[V]**

Send `atf6 3 0 12` with 12 bytes when the cable expects 4, and it takes the
bytes meant as the next command for payload. Later commands disappear without a
reply. To recover, send newlines to end the partial line, then `atz`, then check
that `ata` answers `aro`. That is why Tactrix's DLL, and this driver, open with
`\r\n\r\n`.

### 9.2 Replies carry no tag unless the command is numbered **[V]**

Without numbers, replies are matched by position. If one stale reply sits in the
pipe, every later command receives the previous command's answer: a silently
wrong result. Both causes were seen:

- A previous session left replies queued. A backlog seven deep, after an
  interrupted session, made the driver read the firmware version as the answer
  to `atr 16`.
- A command timed out and the cable answered later. A CAN transmit with nothing
  on the bus takes about 1.2 s to fail with `are 9`; a caller that gave up after
  200 ms leaves that reply for the next command.

Sequence numbers (§3) are the fix, and they are Tactrix's own: number every
command, accept only the reply that carries the number back, and throw away any
reply that arrives when no command is waiting. Sending `ata` twice and hoping
only clears a backlog one reply deep.

---

## 10. Against a bench ECU, 2026-09-16 **[V]**

A production chassis ECU on a bench harness: OBD pins 4, 5, 6, 14 and 16 only,
60 Ω termination, 11.8 V supply, no other CAN node. The module broadcasts a
4-byte status frame every 20.0 ms, which makes it the acknowledging second node
a single cable lacks. One identification request, needing no session, answered
in two frames and was reassembled to 22 bytes by the driver. Periodic tests used
id 0x7FF, which the module does not receive, with a zero payload.

Firmware periodic messages (`atm`):

| Question | Measured |
|---|---|
| Timing | every 100 ms → 20 frames in 2 s, 100.0 ms apart; 1 ms and 5 ms intervals also hold |
| Visibility | with `LOOPBACK` on, each transmission shows as the frame plus the `0x20` echo; no `0x10` transmit indication |
| `atn` | answers in about 60 ms; no frame follows; the id then answers `are 13` |
| `atc`, `ata`, `atz` | each stops every periodic message on the channel |
| Host process killed (SIGKILL) | the message keeps running: another process that sent nothing saw 39 frames in the next ~4 s; it stopped only on `atn` |
| How many | 10 per channel; the eleventh `atm` answers `are 12` |
| Interval 0 | accepted (`arm`), and sends nothing |
| Ids | never reused: they kept counting (1…18) across `atc`, `ata` and `atz` |

This driver runs periodic messages with `atm`/`atn`, as Tactrix's DLL does. A
periodic message outlives a crashed application. `PassThruOpen` resets the
cable, so the next session starts clean, and `PassThruClose` does too; a process
killed in between leaves the message running until the next open, exactly as
with Tactrix's driver.

`SNIFF_MODE`: three windows on raw CAN with a filter passing everything. Normal
open, 2 s: 101 status frames. `ato5 268435456`, 3 s: 151 frames. Normal again,
2 s: 101 frames. 20.0 ms apart in all three. On a `SNIFF_MODE` channel,
`att5 12 0 1000000` answers `aro` in 60 ms and the frame appears on the bus,
which a listen-only controller could not do. The same holds on a first open
right after `atz`/`ata`, and with `SNIFF_MODE | CAN_ID_BOTH` (`ato5 268437504`).
Firmware 1.17.4877 accepts the flag and ignores it.

A transmit with the ECU present: `att5 12 0 1000000` answers `aro`. With nothing
on the bus it answers `are 9` after about 1.3 s, with or without battery voltage
on pin 16.

Against Tactrix's DLL on live traffic ([AB-OFFICIAL.md](AB-OFFICIAL.md)
replays):

- `ISO9141_CH1` opens as `ato3` and `ISO14230_CH1` as `ato4`.
- Raw CAN receive is message for message the same (§7.8), and so is the transmit
  indication (§7.5).
- With `LOOPBACK` on and a filter passing everything, one transmit gives the
  application two messages from both drivers: the frame itself (`RxStatus` 0,
  12 bytes) and a `TX_MSG_TYPE` message of four zero bytes.
- The receive queue. Left unread for 30 s, Tactrix's DLL delivered all ~1,500
  frames. A 64-message queue kept 64 and dropped 1,438. This driver stores each
  message at its own size in a 1 MiB queue per channel (about 29,000 raw CAN
  frames), and delivered 1,503 frames over the same 30 s with no gap.

---

## 11. Legislated OBD-II services on a vehicle **[V]**

The SAE J1979 / ISO 15031-5 read services, each exercised on a 2012 VW Caddy on
2026-09-13 through the driver's own functions:

| Service | Reads | On this vehicle |
|---|---|---|
| `$01` | current powertrain data | answered; PID bitmaps 00, 20 and 40 present, 60 absent |
| `$02` | freeze-frame data | answered |
| `$03` | stored emissions DTCs | answered, none stored |
| `$05` | oxygen sensor tests | not applicable; replaced by `$06` on CAN |
| `$06` | on-board monitoring results | answered |
| `$07` | pending DTCs | answered, none pending |
| `$09` | vehicle information | answered: 00, 02 VIN, 04 calibration id, 06 CVN, 0A ECU name |
| `$0A` | permanent DTCs | no reply: not supported by this vehicle |

`$04` (clear DTCs) and `$08` (on-board system control) write or actuate and were
never sent. An unsupported mode or PID gets no reply at all, not a negative
response, so silence is a valid answer.

Two things a caller must get right:

- **Pad the request.** ISO 15765-4 clause 8.1 requires every diagnostic CAN
  frame to have 8 data bytes and says a shorter one shall be ignored. The cable
  pads only when the caller sets `ISO15765_FRAME_PAD`; the Caddy ignored the
  same request unpadded. The simulator's ECU enforces this too.
- **Flow control goes to the physical id.** A request to the functional id
  `0x7DF` whose reply is segmented needs its flow-control filter on the ECU's
  own id, not on `0x7DF`. Sent to `0x7DF`, the flow control never arrives, and
  every multi-frame reply stalls after its first frame without an error.

**VAG uses a different transport.** VAG modules of that era are reached with VW
TP2.0 on raw CAN: the tester opens a channel on id 0x200, and the module names
the pair of ids to use.

On a 2009 Audi 1.9 TDI, on 2026-09-13:

- The capture tool's ISO 15765 requests got no answer, though every frame was
  acknowledged. The tool sent them unpadded at the time (above).
- On raw CAN, the engine answered a legislated `$01 00` sent to 0x7DF, on
  0x7E8, identically through both drivers ([AB-OFFICIAL.md](AB-OFFICIAL.md)).
- Over TP2.0 (`tools/car/car_capture.py --tp20`, on this driver's raw CAN
  channel; covered in the simulator by `s_tp20`), a channel to the engine module
  (0x01) opened, and five identification records were read through it, the
  flash-status record `1A 9C` among them.
- The K-line answered no five-baud or fast init at any of eight VAG addresses,
  through this driver or Tactrix's DLL. Every init timed out exactly as on an
  empty connector; an earlier reading of the fast-init timing as proof of a
  live line did not reproduce. Audi's wiring diagrams run the engine ECU's
  K-line to OBD pin 7, a K-line-only tool has flashed it there, and the cable's
  own K-line works (§7.9). A second visit sent the open-source EDC16 tool's
  exact fast init by hand, with a second's listening, several hundred times
  and across two ignition cycles, and got no answer either
  ([AB-OFFICIAL.md](AB-OFFICIAL.md)). It is not the driver: Tactrix's DLL
  failed identically.

---

## 12. What is still open

| Question | What would settle it |
|---|---|
| The K-line frame layout (§7.9), the five-baud `arw` reply, a successful fast-init `ary` | one K-line session recorded with `car_capture.py --kline`, or a bench OBD simulator that speaks ISO 9141-2 / KWP2000 |
| Whether the L line and the 2.5 mm jack carry data on channels 7–9 | an L-line ECU or an Innovate device |
| Whether the firmware drives L on channels 3 and 4 | a scope on pin 15 during a five-baud init |
| How extended addressing (`ISO15765_ADDR_TYPE`) is marked on receive | an ECU that uses it |
| The reattach at close on Linux (§1), as this driver does it | `examples/op_smoke` twice on a Linux machine, then `ls /dev/ttyACM*` |
| What `atx` and `atp` do | unknown; Tactrix's DLL sends neither |

Settled, for anyone checking: Tactrix's five-argument forms are accepted and the
trailing number echoed (`ato6 0 500000 0 1` → `aro 1`); two-digit protocol
numbers answer `are 3`; a letter as the channel answers `are 7`; `atv 12 -1` is
accepted; `tbi` gets no reply; filter ids are not reused within a session (33
start/stop cycles reached id 32, 2026-09-24).

---

## 13. Readings the cable refuted

Every claim here was tested on the cable rather than taken on trust. The ones
from this project are kept because each is a trap for the next implementer; the
rest are third-party claims.

| Claim | Source | What the cable said |
|---|---|---|
| A START frame carries the first chunk of data | this project's first model | START carries only the CAN id; the data follows with the id repeated (§7.3) |
| The `0x10` frame is rare, or is received data | this project | every successful transmit produces one (§7.5) |
| The low bits are J2534 `RxStatus`, `0x02` = `START_OF_MESSAGE` | two third-party drivers | `0x02` marks a 29-bit id (§7.4) |
| Every K-line frame has a timestamp, as on CAN | this project | data frames have none, per every K-line implementation (§7.9) |
| Five-baud init is `atw<ch> 1` + the address as a raw byte | this project | the cable took the 1 as the address and the byte as the next command; Tactrix's DLL sends `atw3 51` (§4) |
| Five-baud init is `aty<ch> 1 1` + the address byte | `emdzej/j2534` | answers `are 7`, like `aty<ch> 1 0`; `aty` is fast init |
| The five-baud reply is `ary` | this project | `arw<ch> <b> <b>` with HDS on a Honda (§3) |
| `atp` starts periodic messages | this project | it is a pin command shaped like `atv`; `atm` starts periodic messages (§4) |
| Channels 7 and 8 are SCI A engine and transmission | this project | they are the L line; Tactrix's DLL opens SCI as `ato0` (§5) |
| `ato`'s fourth argument is a channel slot, 0–2 | `Mackanized/op2j2534` | it is ignored (§4) |
| Three channels at most | same | 5, 6, 7 and 9 open together (§5) |
| ISO 14230 opens at 10400 baud only, `are 25` otherwise | same | no baud rate is checked (§5) |
| `atf`'s arguments are `<type> <mask_len> <pattern_len>` | same | the second is TxFlags: Tactrix's DLL sends `atf6 3 64 4` |
| Letters `C`, `D`, `S` as channels for the L line and the jack | `emdzej/j2534` | `are 7`; Tactrix's DLL sends `ato7`, `ato8`, `ato9` (§5) |
| Only four configuration parameters are supported | this project's first sweep | about two dozen, depending on the protocol (§8.1) |
| Pin 12 senses the programming voltage | this project | it is OEM12, the pin `atv 12` drives |
| `ata` means "attention" | this project | it closes every channel and switches every output off |
| A transmit indication carries no data | this project, citing J2534-1 JAN2022 | the 04.04 text and Tactrix's DLL both put the CAN id in it (§7.5) |

Third-party claims confirmed: `ata` closes all channels, `atp` shares `atv`'s
template, and the readable pins are 8, 12, 16 and 17 (all from
`Mackanized/op2j2534`).

---

## 14. Sources

| Source | What it settles |
|---|---|
| SAE J2534-1 DEC2004 (04.04) §7.2.5, §8.5–8.7, A.2–A.4 | what an application must be given for indications and segmented replies; the `RxStatus` bits; message size limits |
| Vehicle and bench sessions: 2026-06-17 (a chassis controller, ISO 15765), 2026-09-13 (2012 VW Caddy; 2009 Audi 1.9 TDI), 2026-09-16 and 2026-09-24 (bench chassis ECU) | frame sequences, status bits, the transmit indication, raw CAN, periodic timing, `SNIFF_MODE`, long-reply chunking, the ISO 15765 loopback echo |
| `op20pt32.dll` 1.02.0.4868 under emulation and on the cable ([AB-OFFICIAL.md](AB-OFFICIAL.md)) | every command Tactrix sends, sequence numbers, `atm`, the chunk layout, `tbi`, `READ_PROG_VOLTAGE`'s pin argument |
| Tactrix `openport2_setup_1024820.exe`: `samples/common/j2534_tactrix.h`, `samples/canlogger/canlogger.cpp`, `samples/klogger/klogger.cpp` | Tactrix's header (private IOCTLs, J2534-2 ids, `SNIFF_MODE`, OEM error codes, pin numbers), and Tactrix's own programs discarding `START_OF_MESSAGE` messages |
| Tactrix EcuFlash changelog (firmware 1.41, 1.42, 1.44) | K-line echoes follow `LOOPBACK`; transmit-done messages carry timestamps; `CAN_29BIT_ID` on received messages |
| github.com/emdzej/j2534 @ `35aae08` (`packages/core/src/parser.ts`, `commands.ts`, `AGENTS.md`) | an independent reading of every status byte, CAN and K-line, from a disassembly of the DLL |
| github.com/MCU-Innovations/opta-j2534-rs @ `9774569` (`j2534-core/src/devices/tactrix/`) | status bits and chunk handling, verified on a Honda CBR1000RR ECM |
| github.com/Aiden-korbs/openport2-winarm-j2534 | the `arw` five-baud reply, with HDS on a 2005 Honda CR-V |
| github.com/Mackanized/op2j2534 | command templates taken from the unpacked DLL; the claims above |
| github.com/firefighter-19/tuneforge, the `dschultzca`/`NikolaKozina` `j2534` driver and its forks, `sw7ft/BerryCore` | the same family's status table and K-line layout |

## Reproducing this

`tools/probe/op_probe.c` prints the USB descriptors. For protocol work the
serial device is easier than libusb:

```bash
python3 - <<'PY'
import os, termios, time
fd = os.open("/dev/cu.usbmodemXXXXXXXX1", os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
a = termios.tcgetattr(fd); a[0] = a[1] = a[3] = 0
a[2] = termios.CREAD | termios.CLOCAL | termios.CS8
termios.tcsetattr(fd, termios.TCSANOW, a)
os.write(fd, b"ati\r\n"); time.sleep(0.3)
print(os.read(fd, 4096))
PY
```
