# The OpenPort 2.0 wire protocol

How a host talks to a Tactrix OpenPort 2.0 over USB. Tactrix publishes no
specification. The facts here come from sweeps of the cable, vehicle
recordings and runs of Tactrix's Windows DLL against the same cable
([AB-OFFICIAL.md](AB-OFFICIAL.md)).

Measured on firmware 1.17.4877, on macOS 26.6 (Apple Silicon) with libusb
1.0.29, against Tactrix's `op20pt32.dll` 1.02.0.4868. Live-bus measurements
come from a 2012 VW Caddy, a 2009 Audi 1.9 TDI and a production chassis ECU on
a bench harness (§10).

| Marker | Meaning |
|---|---|
| **[V]** | Verified on the cable above |
| **[P]** | Partly verified: the shape is confirmed, a detail is not |
| **[U]** | Unverified: inferred, needs a bus to confirm |

[§12](#12-what-is-still-open) lists what is still open, and
[§13](#13-readings-the-cable-refuted) lists third-party claims that the cable
refuted.

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

A host has two ways in:

1. **The serial device node**, `/dev/cu.usbmodem<serial>1` on macOS or
   `/dev/ttyACM0` on Linux. The driver uses it on macOS, because the kernel's
   CDC-ACM driver keeps reading while the application is stalled (below).
2. **libusb on the bulk endpoints.** The driver uses it on Linux, and on macOS
   for a cable with no serial node (§12). `OPENPORT_DEVICE=usb` selects it.
   `OPENPORT_DEVICE=<path>` selects a serial node or a simulator's pty.

On Windows, Tactrix's USB driver offers the bulk pipes as a device interface of
class `{6d1781b7-c987-4f6c-8d4f-1efc098bea67}` (`tools/ab-official/iface.reg`).
Without it, Windows 10 binds `usbser.sys` ("USB Serial Device (COMn)").
`src/op_win.c` opens either, the interface first, and keeps 256 reads waiting.
With the process frozen 300 ms of every 500 ms at about 3,100 frames a second,
that lost no frames. One waiting read lost about 2,500 frames in 30 s through
either DLL (2026-11-12, [TESTING.md](TESTING.md#on-a-windows-pc)). A detached
cable fails the waiting read with error 995. Tactrix's driver refuses a second
open with error 5.

### Claiming the interface on macOS **[V]**

macOS's `AppleUSBCDCACM` binds both interfaces:

| Interface | `detach_kernel_driver` | `claim_interface` |
|---|---|---|
| 0 (comm) | `LIBUSB_ERROR_ACCESS` | `LIBUSB_ERROR_ACCESS` |
| 1 (data) | `LIBUSB_ERROR_ACCESS` | success |

A host claims the data interface, found by its bulk endpoints. The failed
detach is expected.

### Claiming the interface on Linux **[P]**

`cdc_acm` binds the comm interface and also claims the data interface.
Detaching it from either interface removes `/dev/ttyACM*` until the comm
interface is probed again. This driver reattaches at close, comm interface
first, the interfaces that had a kernel driver at open. That rests on one
report from Linux hardware (2026-09-21).

### The cable hangs after a wrong-rate connect **[V]**

Opening a CAN channel at 250 kbit/s on a live 500 kbit/s bus and closing it
again sometimes leaves the cable silent. The close and every later command go
unanswered, and after a few the cable stops taking USB data. This happened
once in six rounds through this driver and once in three through Tactrix's
DLL, both times at the close (2026-11-12, `sweep_connect` in `j2534_trace`).
Neither a USB replug nor a power cycle of the vehicle connector alone revived
the cable. Removing both at once did.

### The cable discards frames a host does not collect **[V]**

The cable sends each received frame record as its own bulk IN packet. With no
read posted, it holds about 1.1 KB, which is 50 to 60 raw CAN records or some
17 ms of a dense stream. When that is full, it discards new CAN frames without
a mark. Only the next record's timestamp, which jumps across the gap, shows
the loss.

On 2026-10-23, at about 3,000 frames a second with one synchronous read posted
at a time, 9 of 18 runs of about 85,000 frames lost 1 to 140 frames at a time.
Every missing frame was absent from the USB stream, and at every hole the cable
held 1,050 to 1,260 bytes of unread records.

On macOS, user-space reads are not enough. With 256 libusb reads posted by a
dedicated thread, a Mac short of memory lost frames in 12 of 13 reads over four
minutes. Freezing the reading process for 300 ms every 2 s failed 4 of 4
libusb reads. Through the serial node, the same freezes and 1 s freezes every
1.5 s lost nothing in 6 of 6 reads. Linux is unmeasured.

---

## 2. Framing **[V]**

Host to device: an ASCII line ending in `\r\n`, sometimes followed directly by
binary payload bytes.

```
at<verb>[<channel>] <arg> <arg> ...\r\n   [payload bytes]
```

Tactrix's DLL sends the line and its payload in one write. A payload that
follows its line about 15 ms later draws `are 10`. The cable then reads the
payload as the next command, which goes unanswered (2026-10-23).

A command stopped part way can leave the next command unanswered **[P]**. On
2026-11-12, with no ECU answering, a 4,099-byte ISO 15765 transmit with a
100 ms timeout was still being taken over USB at the timeout. Cutting the
transfer there left the next command unanswered in one of two runs. This
driver carries a started command on for up to 2 s past the timeout
(`OP_WRITE_GRACE_MS`). The same transmit then returned `ERR_TIMEOUT` in 105 ms
with the next command answered, in 5 of 5 runs.

Device to host: two kinds of data on the same pipe.

```
ar<verb> <args>\r\n                                    ASCII reply
'a' 'r' <channel-digit> <len:u8> <payload...>          binary message frame
```

- **They interleave.** A message frame can arrive between a command and its
  reply.
- **Byte 2 tells them apart.** A letter marks a reply and an ASCII digit a
  frame.
- **An unknown command gets no reply**, not an error.
- **Spacing matters.** Channel commands attach the digit to the verb
  (`ato6 …`). `atr`, `atv` and `atp` take a space before their first argument.

---

## 3. Replies and sequence numbers **[V]**

| Reply | Meaning |
|---|---|
| `aro` | success, no value |
| `are <code>` | failure. `<code>` is a J2534 return code (§6) |
| `are <code> <detail>` | failure, echoing the offending value |
| `ari <text>` | text, such as `ari main code version : 1.17.4877` |
| `arr <pin> <millivolts>` | a pin voltage |
| `arf<ch> <filter_id>` | filter installed |
| `arg<ch> <param> <value>` | a configuration value |
| `arm<ch> <id>` | periodic message started |
| `arw<ch> <b> <b> [<seq>]` | five-baud init result: the ECU's key bytes, in decimal **[P]** |
| `ary<ch> <n> [<seq>]` + `n` raw bytes | fast init result: the ECU's response **[P]** |
| `arp <service> <len> [<seq>]` + `len` bytes | application-service result (§4), read from Tactrix's DLL **[P]** |

**Every reply echoes the number its command carried.** When a decimal number
follows a command's arguments, the cable appends it to the reply. Measured
2026-09-13:

```
ata 2            -> aro 2
atr 16 3         -> arr 16 108 3
atf6 3 64 4 8    -> arf6 0 8
atg6 30 10       -> arg6 30 0 10
atg6 127 9       -> are 1 9
atn6 5 14        -> are 13 14
aty3 4 0 10      -> are 7 10
```

`ato6 0 500000 0 1` answers `aro 1`. Without a number, `arf` and `arg` end in
0, and other replies have no trailing field. `ati` ignores a number. An `att`
without a number gets no reply at all (§4).

Tactrix's DLL matches replies by the trailing number alone, `arw` and `ary`
included (read from its code) **[P]**. The one `arw` seen on a cable, with
Honda HDS (`Aiden-korbs/openport2-winarm-j2534`), answered an unnumbered
`atw`. With no K-line ECU on this bench, both inits answer `are 7`.

This driver numbers every command except `ati` and a transmit it does not wait
for. Its numbers start at 256, so a K-line key byte never looks like one. It
accepts a K-line init reply with or without a number and drops any other reply
that lacks the command's number (§9).

---

## 4. Commands **[V]**

Every `at<a..z>` was tried. `atb atd ate ath atj atq atu` get no reply.

| Command | Arguments | Does |
|---|---|---|
| `ati` | | reports the firmware version |
| `atz` | | resets: closes every channel, stops every periodic message and switches every output off |
| `ata` | | closes every channel and switches every output off |
| `ato<proto>` | `<flags> <baud> 0` | opens a channel (§5) |
| `atc<ch>` | | closes a channel and stops its periodic messages |
| `att<ch>` | `<len> <txflags> <budget_us>` + payload | transmits a message |
| `atf<ch>` | `<type> <txflags> <len>` + payload | installs a filter |
| `atk<ch>` | `<filter_id>` | removes a filter. `-1` removes all of them |
| `atg<ch>` | `<param>` | reads a configuration value (§8) |
| `ats<ch>` | `<param> <value>` | writes a configuration value (§8) |
| `atm<ch>` | `<interval_us> 0 <txflags> <len>` + payload | starts a periodic message and replies `arm<ch> <id>`. Tactrix's DLL fills the `0`, a second interval, from bits 16–30 of `TimeInterval` when bit 31 is set. Its effect is unmeasured |
| `atn<ch>` | `<msg_id>` | stops a periodic message |
| `atl<ch>` | | stops every periodic message on the channel |
| `atr` | ` <pin>` | reads a pin voltage in millivolts (§8) |
| `atv` | ` <pin> <millivolts>` | puts a voltage on a pin, or grounds it (§8) |
| `aty<ch>` | `<len> 0` + request bytes | K-line fast init |
| `atw<ch>` | `<address>`, in decimal, no payload | K-line five-baud init |
| `atp` | ` <service> <len>` + `len` bytes | application-service request, answered `arp` **[P]** |
| `atx` | ` <n>` | restarts the cable into its bootloader **[P]** |

- **`ata`** answers with the command's number. A filter installed before `ata`
  answers `are 2` afterwards.
- **`ato`'s fourth field is ignored.** Tactrix's DLL puts a bookkeeping value
  there for ISO 9141.
- **`atc`** answers `aro` even for a channel that was never open.
- **`att`'s budget** is how long, in microseconds, the cable may try to get the
  message onto the bus. Tactrix's DLL sends the caller's `WriteMsgs` timeout
  there. For a timeout of 0 it sends 50,000 (50 ms) on raw CAN, 0 on the
  2.5 mm jack (channel 9) and 1,000,000 (1 s) on every other channel (read from
  its code) **[P]**. This driver does the same (`write_one` in
  `src/op_j2534.c`). Without a budget, the cable gives up after about a second.
- **An `att` without a number** gets no reply, not even `are 9`, and the cable
  answers nothing else for about a second. With a number, the same transmit
  answers `are 9 <number>` after 1.0 s.
- **`atf`** takes type 1 (PASS), 2 (BLOCK) or 3 (FLOW_CONTROL), then two or
  three messages of `<len>` bytes each: mask, pattern and flow control. A wrong
  payload length knocks the parser out of step (§9.1).
- **`atk<ch> -1`** and **`atl`** serve `CLEAR_MSG_FILTERS` and
  `CLEAR_PERIODIC_MSGS`. Both answer `aro`, and the old ids then answer `are 22`
  and `are 13`. Filter ids count up across all channels and restart only at
  `ata` or `atz` (2026-09-24).
- **`aty`** returns in about 108 ms, after a wake-up pulse of 25 ms low and
  25 ms high. **`atw`** takes about 2,450 ms. `atw3 51` initialises address
  0x33.
- **`atp <service> <len>`** plus up to 128 bytes is an application-service
  request, `TX_IOCTL_APP_SERVICE` in Tactrix's header (§8.1). The DLL formats
  it from `atv`'s template, `at%c %d %d %u`, and sends it only in its firmware
  updater (§12) (read from its code) **[P]**. On the cable, `atp` with two
  arguments and no payload answered after 0.3–1.3 s **[V]**. A second argument
  of 0 drew `are 5`. Every other value tried drew `are 10`: 1, 5000, 8000,
  12000, 20000, `SHORT_TO_GROUND` and `VOLTAGE_OFF`, with first arguments 0, 1,
  2, 6, 12 and 15.
- **`atx <n>`** ends the DLL's firmware updater, and the cable restarts in its
  bootloader (§12) **[P]**. This driver never sends it. On the cable,
  `atx<ch> <n>` answered `are 7 <n>` **[V]**. Whether the two forms are the
  same command is unknown **[U]**.

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

- The DLL waits for `aro <number>`, 500 ms for most commands and 5 s for `atr`
  and `atg`.
- When `ati` gets no answer, the DLL sends the bootloader verb `tbi\n` three
  times, 300 ms apart, and gives up.

This driver sends the same commands with the same arguments. It opens with
`\r\n\r\n`, `atz`, `ata` and `ati`.

---

## 5. Channels and protocols **[V]**

The channel number is the firmware's protocol number. A second `ato` on an open
channel answers `are 20` (`ERR_CHANNEL_IN_USE`). Tactrix's DLL maps the J2534
ids onto these channels:

| Channel | J2534 ids | Line |
|---|---|---|
| 3 | `ISO9141`, `ISO9141_CH1` (`ISO9141_K`) | K, pin 7 |
| 4 | `ISO14230`, `ISO14230_CH1` (`ISO14230_K`) | K, pin 7 |
| 5 | `CAN`, `CAN_CH1` | CAN, pins 6 and 14 |
| 6 | `ISO15765`, `ISO15765_CH1` | CAN, pins 6 and 14 |
| 7 | `ISO9141_CH2` (`ISO9141_L`) | L, pin 15 |
| 8 | `ISO14230_CH2` (`ISO14230_L`) | L, pin 15 |
| 9 | `ISO9141_CH3` (`ISO9141_INNO`) | RS-232 receive on the 2.5 mm jack |

- **J1850 and SCI** are not supported. The DLL sends `ato0`, gets `are 3` and
  returns `ERR_INVALID_PROTOCOL_ID`. `ato1`, `ato10` and `ato11` also answer
  `are 3`. `atoC` answers `are 7` without a number.
- **Protocols sharing a line exclude each other**, with `are 3`. The cable
  refuses `ato4` while ISO 9141 holds K and `ato8` while ISO 9141 holds L.
  The reverse also holds. There is no other limit. Channels 5, 6, 7 and 9 open together, and
  so do 7, 9 and 3 with `ISO9141_K_LINE_ONLY` (`ato3 4096 …`).
- **The baud rate is not checked.** `ato5 0 123456 0` and `ato6 0 0 0` answer
  `aro`. A CAN channel opened at 250 kbit/s on a live 500 kbit/s bus can hang
  the cable when it is closed
  ([§1](#the-cable-hangs-after-a-wrong-rate-connect-v)). `ato3 512 5 0` reads
  back `DATA_RATE` 10400 and sends at that rate.

**The cable does ISO-TP.** The host sends and receives whole messages. The
cable segments, sends flow control and honours the ECU's block size and
separation time. `ISO15765_BS` (30) and `ISO15765_STMIN` (31) set the flow
control the cable sends. `BS_TX` (34) and `STMIN_TX` (35) set what it asks of
the ECU. `ISO15765_WFT_MAX` (37) limits the WAIT frames it accepts.

---

## 6. Error codes are J2534 codes **[V]**

`are <n>` carries a J2534-1 return code unchanged. Each was provoked:

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

A frame carries at most 250 payload bytes. A real one:

```
61 72 36 | 09 | 10 | 19 50 b8 0c | 00 00 07 e0
'a''r''6'  len  sts   timestamp     CAN id 0x7E0
```

The timestamp counts microseconds since power-on and wraps about every 71
minutes. For CAN and ISO 15765, the payload starts with the 4-byte big-endian
CAN id, as J2534's `Data[0..3]` does. K-line frames differ (§7.9).

### 7.2 The status byte **[V]**

| Bit | Meaning |
|---|---|
| `0x80` | START: a segmented message begins (§7.3) |
| `0x40` | END: this frame completes a message |
| `0x20` | LOOPBACK: an echo of our own transmit (§7.7) |
| `0x10` | transmit indication (§7.5) |
| `0x02` | the CAN id is 29-bit (§7.4) |
| `0x04` | ISO 15765 extended addressing. The header is 5 bytes, id and address, on every chunk. Read from Tactrix's DLL, not seen on a cable **[P]** |
| `0x08` | Tactrix's DLL delivers nothing for a CAN frame or a K-line END that carries it **[P]**. Its meaning is unknown, so this driver delivers such frames and logs the bit |

### 7.3 What the bits mean on a live bus **[V]**

Measured over ISO 15765 at 500 kbit on a 2012 VW Caddy (2026-09-13) and a
vehicle's chassis controller (2026-06-17):

| Frames on the wire | Meaning |
|---|---|
| one `0x40` frame: id + data | a message that fitted one CAN frame, with nothing before it |
| a `0x80` frame with the id only, then `0x40` frames with id + data | a segmented message. START announces it, and the data follows in frames that repeat the id |

A `0xC0` frame (START and END together) was never seen.

A START frame without END is an indication. J2534-1 §8.6 and A.4 prescribe it as
`ISO15765_FIRST_FRAME`, `DataSize` 4, `ExtraDataIndex` 0. Tactrix's own
`canlogger` and `klogger` samples skip every `START_OF_MESSAGE` message.
`emdzej/j2534`, written from a disassembly of the DLL, reads the bits the same
way. So does `MCU-Innovations/opta-j2534-rs`, verified on a Honda CBR1000RR ECM.

### 7.4 The low bits **[V]**

The same request sent four ways on the Caddy:

| TxFlags | Status of the transmit indication |
|---|---|
| `0x000` | `0x10` |
| `0x040` `ISO15765_FRAME_PAD` | `0x10` |
| `0x100` `CAN_29BIT_ID` | `0x12` |
| `0x140` both | `0x12` |

`0x02` follows the 29-bit id exactly and nothing else. The driver passes it on
as `CAN_29BIT_ID` on CAN channels, as Tactrix's changelog for firmware 1.44
describes.

### 7.5 The transmit indication **[V]**

Every successful transmit produces one `0x10` frame right after the `aro`,
carrying the 4-byte CAN id and no data. It is not received data. J2534-1 §8.6
defines it as `TX_MSG_TYPE | TX_DONE`, `DataSize` 4, `Data` the CAN id.
Tactrix's DLL (`RxStatus 9`, `DataSize 4`) and this driver both deliver it that
way. Periodic messages produce none (§10).

### 7.6 Long replies arrive in 70-byte chunks **[V]**

A segmented ISO 15765 reply arrives as a `0x80` announcement with the CAN id
only, then chunks of up to 70 data bytes (ten consecutive frames). Each chunk
repeats the CAN id, and the last is marked `0x40`. The first chunk holds 69
bytes: the first frame's six and nine consecutive frames' 63. All 6,162
segmented replies of a 512 KB `$23` read from the bench ECU had this shape
(2026-09-24):

| Message size (id + data) | Chunks after the announcement |
|---|---|
| up to 69 bytes | one: id + all the data |
| 133 bytes (4,097 replies) | 73 + 64 |
| 259 bytes | 73 + 74 + 74 + 50 |
| 260 bytes (2,057 replies) | 73 + 74 + 74 + 51 |

Tactrix's DLL strips the four id bytes from every chunk after the first, and so
does this driver.

### 7.7 Seeing your own transmit, and LOOPBACK **[V]**

**Without LOOPBACK.** On a raw CAN channel with a filter that passes
everything, the frame you just sent comes back as an ordinary `0x00` frame. A
filter that excludes its id stops it.

**LOOPBACK on raw CAN.** Setting `LOOPBACK` (`ats<ch> 3 1`) adds a separate
`0x20` frame whose payload is four zero bytes instead of the CAN id (bench ECU,
2026-09-16). Tactrix's DLL delivers it as a `TX_MSG_TYPE` message of four zero
bytes, and so does this driver. A transmit that fails with `are 9` has no echo.

**LOOPBACK on ISO 15765.** The echoes arrive on raw CAN channel 5 (bench ECU,
2026-09-24). A single-frame request gave, in order:

1. `ar5`, status `0x20`, four zero bytes: one per CAN frame the cable sent.
2. `ar6`, status `0x10`, the CAN id: the transmit indication.
3. While the ECU's segmented reply arrived: `ar5`, status `0x20`, the
   flow-control frame the cable sent, id and data intact
   (`00 00 07 b5 30 00 00 00 00 00 00 00`).

Tactrix's DLL delivers the `0x20` frames to the ISO 15765 channel. With
channel 5 also open, it delivers every channel-5 frame there and none to the
CAN channel. This driver delivers them to the ISO 15765 channel only while
channel 5 is closed.

### 7.8 Raw CAN frames carry no START or END **[V]**

On a raw CAN channel (5), every frame arrives with status `0x00`, our own and
the ECU's alike (Caddy, 2026-09-13). One frame is one whole message.

### 7.9 The K-line frame layout **[P]**: echoes measured, received frames not

On channels 3 and 4 (and probably 7–9), frames are not laid out like CAN frames:

| Frame | Body after the status byte |
|---|---|
| `0x00` data, `0x20` loopback data | the K-line bytes themselves, **no timestamp** |
| `0x80` start, `0xA0` loopback start | the 4-byte timestamp, no data |
| `0x40` end, `0x60` loopback end | the 4-byte timestamp, no data |
| `0x10` transmit indication | the 4-byte timestamp |

Two independent sources give this layout. The first is one code lineage, from
`NikolaKozina/j2534` (2018) to `opta-j2534-rs` and `emdzej/j2534`. Its users
got checksum-valid frames from Subaru, BMW and Honda ECUs by reading data from
the byte after the status. No repository holds a capture of a received K-line
frame.

The second source is Tactrix's DLL (read from its code) **[P]**. On channels 3,
4, 7, 8 and 9, only a START or END frame whose body is exactly four bytes
starts with a timestamp. Every other frame is data. A K-line START is delivered
as a `START_OF_MESSAGE` indication with no data. An END delivers the message.
This driver applies the same rule. It delivers the bench echo of
`68 6A F1 01 00` as an indication with `RxStatus` 2, then the five bytes with
`RxStatus` 1 **[V]**.

All those sessions connected with `ISO9141_NO_CHECKSUM`. How the cable handles
the checksum byte with the flag clear is unobserved.

The echo, measured on the bench with nothing on pin 7 (2026-09-24):

```
ato3 4608 10400 0 / ats3 3 1 / atf3 1 0 1 + 00 00
att3 5 0 1000000 + 68 6a f1 01 00
  ar3 05 a0 22 a6 cb 63            loopback start, timestamp
  ar3 06 20 68 6a f1 01 00         loopback data, no timestamp
  ar3 05 60 22 a7 28 b2            loopback end, timestamp 24 ms later
  aro
```

- `aro` arrives before or after the echo.
- No transmit indication follows a K-line transmit. With `LOOPBACK` off,
  nothing is echoed.
- The echo passes through the transceiver, so an echo shows that pin 7 is not
  held low.
- ISO 14230 without `ISO9141_NO_CHECKSUM` appends the checksum on the wire. The
  echo carries the message without it.
- An `aty` fast init produces no echo.
- The firmware's fast init waits about 50 ms for the answer. `aty4 0 0` makes
  the wake-up pulse alone and answers `are 7` after 52 ms. A raw `att4` can
  follow at once, as a fast init with as long a wait as the caller wants.
- A fast init waits for 300 ms of quiet on the line first (ISO 14230's W5)
  **[P]**. An `aty` sent about 250 ms after a transmit answered after 86–95 ms
  (the Audi, 2026-09-24).

---

## 8. Configuration and pins **[V]**

### 8.1 Configuration parameters

`atg<ch> <param>` reads and `ats<ch> <param> <value>` writes, with J2534
parameter numbers. Parameters 1–37 were tried:

| Protocol | Supported parameters |
|---|---|
| 6, ISO 15765 | 1 `DATA_RATE`, 3 `LOOPBACK`, 23 `BIT_SAMPLE_POINT`, 24 `SYNC_JUMP_WIDTH`, 30 `ISO15765_BS`, 31 `ISO15765_STMIN`, 34 `BS_TX`, 35 `STMIN_TX` and 37 `ISO15765_WFT_MAX` |
| 4, ISO 14230 | 1 `DATA_RATE` (read only), 3 `LOOPBACK`, 7 `P1_MAX`, 10 `P3_MIN`, 12 `P4_MIN`, 14–18 `W1`–`W5`, 19 `TIDLE`, 20 `TINIL`, 21 `TWUP`, 22 `PARITY`, 25 `W0`, 32 `DATA_BITS` and 33 `FIVE_BAUD_MOD` |

- Anything else answers `are 1`. The driver passes every parameter on.
- `W1` raised to 1000 ms makes `atw` take 3,501 ms instead of 2,451.
- Tactrix's `TX_PARAM_STOP_BITS` (0x9000) reads 1 on channels 9 and 3.
- `SNIFF_MODE` in `ato`'s flags is accepted, but the cable still acknowledges
  frames (§10).
- Tactrix's private IOCTLs (`TX_IOCTL_*`, 0x70000 and up) send nothing to the
  cable when called with a NULL input. `TX_IOCTL_APP_SERVICE` sends `atp`
  (§4). Its structures are read from the DLL **[P]**: input
  `{u32 len; u32 service; u8 data[]}`, output `{u32 len; u8 data[]}`. Tactrix's
  `klogger` sample calls it with service 5 and info 1 to read the serial
  number. The input structures of the other four are unknown. This driver
  answers all five `ERR_INVALID_IOCTL_ID`.
- `READ_PROG_VOLTAGE` takes a pin in Tactrix's DLL, although J2534-1 passes
  `pInput` as NULL. The DLL reads a pin number from `pInput`, and with NULL it
  returns -1 and sends nothing (2026-09-16). This driver reads the named pin,
  or pin 12 when `pInput` is NULL.

### 8.2 Readable pins

Of pins 0–20, four answer `atr <pin>`, and the rest answer `are 19`. Readings
are in millivolts.

| Pin | Reading | Meaning |
|---|---|---|
| 8 | 0 with nothing attached | OBD pin 8 (OEM8), an analogue input |
| 12 | 0 with nothing attached | OBD pin 12 (OEM12), an analogue input, also driven by `atv 12` and shared with the jack's tip (below) |
| 16 | 130–152 on USB power alone, 11,894 from an 11.9 V supply | OBD pin 16, the battery, read by `READ_VBATT` |
| 17 | 5,729–5,794, drifting, with no output on | `PIN_VADJ`, the adjustable output supply that `atv` switches onto a pin |

According to Tactrix, the 2.5 mm jack's tip is OEM12, and inserting a plug
disconnects it from OBD pin 12 **[U]**. The jack's ring and sleeve are an
RS-232 input for Innovate MTS devices, channel 9.

### 8.3 Programming voltage and ground

`atv <pin> <millivolts>` sets a pin. `VOLTAGE_OFF` (0xFFFFFFFF) and
`SHORT_TO_GROUND` (0xFFFFFFFE) are accepted signed or unsigned:

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

What each pin supports, from Tactrix's header:

| Pin | Supports |
|---|---|
| 0 (`AUX_PIN`, the 2.5 mm jack) | ground and voltage |
| 1, 3, 9, 11, 12 and 13 | ground and voltage |
| 2 (J1850+) | 5 V and 8 V according to the header. On this firmware, `atv 2` answers `are 19` |
| 7 (K), 10 (J1850−) and 15 (L) | ground |
| 8, 12, 16 and 17 | reading (`atr`) |

The cable leaves these to the driver:

- **The range is 5,000–20,000 mV**, inclusive.
- **All voltage pins share one supply.** Setting a second pin moves the first to
  the new voltage. This driver refuses a second pin with `ERR_PIN_INVALID`
  until the first is off, as J2534-1 §7.2.11 requires.
- **Grounding K cuts off the channel using it.** The cable accepts
  `SHORT_TO_GROUND` on pins 7 and 15 while a channel uses them. This driver
  refuses to ground K under an open K-line channel or L under an open L-line
  channel, and to open such a channel while its pin is grounded
  (`ERR_CHANNEL_IN_USE`).
- **This driver sends `ata` and `atz` at open and `atz` at close**, so a
  session never starts or ends with a live pin.

---

## 9. Two hazards a driver must handle

### 9.1 A bad payload knocks the parser out of step **[V]**

When `atf6 3 0 12` carries 12 bytes and the cable expects 4, the cable takes the
bytes meant as the next command for payload. Later commands get no reply.
Recovery is newlines to end the partial line, then `atz`, then a check that
`ata` answers `aro`. Tactrix's DLL and this driver open with `\r\n\r\n` for
this reason.

### 9.2 Replies carry no tag unless the command is numbered **[V]**

Without numbers, replies are matched by position, and one stale reply gives
every later command the previous command's answer. A stale reply comes from a
previous session's backlog or from a command that timed out. A CAN transmit
with nothing on the bus takes about 1.2 s to fail with `are 9`, so a caller
that gave up after 200 ms leaves that reply for the next command. Sequence
numbers (§3) fix both. The driver also drops any reply that arrives when no
command is waiting.

---

## 10. Against a bench ECU, 2026-09-16 **[V]**

A production chassis ECU on a bench harness, with 60 Ω termination and no
other CAN node. It broadcasts a 4-byte status frame every 20.0 ms, so it is the
acknowledging second node a single cable lacks.

Firmware periodic messages (`atm`):

| Question | Measured |
|---|---|
| Timing | every 100 ms gave frames 100.0 ms apart. 1 ms and 5 ms intervals also hold |
| Visibility | with `LOOPBACK` on, each transmission shows as the frame plus the `0x20` echo. No `0x10` transmit indication |
| `atn` | answers in about 60 ms. The id then answers `are 13` |
| `atc`, `ata` and `atz` | each stops every periodic message on the channel |
| Host process killed | the message keeps running until `atn` |
| How many | 10 per channel. The eleventh `atm` answers `are 12` |
| Interval 0 | accepted (`arm`), and sends nothing |
| Ids | never reused. They kept counting across `atc`, `ata` and `atz` |

`PassThruOpen` and `PassThruClose` reset the cable, so a message left by a
killed process runs until the next open.

`SNIFF_MODE` does not make the controller listen-only. On a `SNIFF_MODE`
channel (`ato5 268435456`), `att5 12 0 1000000` answers `aro` in 60 ms and the
frame appears on the bus. Firmware 1.17.4877 accepts the flag and ignores it.

With nothing on the bus, `att5 12 0 1000000` answers `are 9` after about
1.3 s.

Against Tactrix's DLL on live traffic ([AB-OFFICIAL.md](AB-OFFICIAL.md)), raw
CAN receive and the transmit indication match message for message. Left unread
for 30 s, both delivered all of about 1,500 frames. This driver queues up to
1 MiB per channel, about 29,000 raw CAN frames.

---

## 11. Legislated OBD-II services on a vehicle **[V]**

The SAE J1979 read services on a 2012 VW Caddy (2026-09-13):

| Service | Reads | On this vehicle |
|---|---|---|
| `$01` | current powertrain data | answered. PID bitmaps 00, 20 and 40 present, 60 absent |
| `$02` | freeze-frame data | answered |
| `$03` | stored emissions DTCs | answered, none stored |
| `$05` | oxygen sensor tests | not applicable, replaced by `$06` on CAN |
| `$06` | on-board monitoring results | answered |
| `$07` | pending DTCs | answered, none pending |
| `$09` | vehicle information | answered: 00, 02 VIN, 04 calibration id, 06 CVN and 0A ECU name |
| `$0A` | permanent DTCs | no reply: not supported by this vehicle |

An unsupported mode or PID gets no reply, not a negative response.

- **The request must be padded.** ISO 15765-4 clause 8.1 says a diagnostic CAN
  frame shorter than 8 data bytes shall be ignored. The cable pads only when the
  caller sets `ISO15765_FRAME_PAD`. The Caddy ignored the request unpadded.
- **Flow control goes to the physical id.** A request to `0x7DF` whose reply is
  segmented needs its flow-control filter on the ECU's own id. Otherwise the
  reply stalls after its first frame without an error.

On a 2009 Audi 1.9 TDI (2026-09-13), the engine answered `$01 00` on raw CAN,
and a VW TP2.0 channel to it opened (`tools/car/car_capture.py --tp20`). Its
K-line answered no five-baud or fast init at any of eight addresses, through
either driver, across two visits. The cause is unknown.

---

## 12. What is still open

| Question | What would settle it |
|---|---|
| The K-line frame layout (§7.9), the `arw` reply and a successful `ary` | one K-line session recorded with `car_capture.py --kline` |
| Whether the L line and the jack carry data on channels 7–9 | an L-line ECU or an Innovate device |
| Whether the firmware drives L on channels 3 and 4 | a scope on pin 15 during a five-baud init |
| How extended addressing is marked on receive | a capture from an ECU that uses it (§7.2) |
| The reattach at close on Linux (§1) | `examples/op_smoke` twice on Linux, then `ls /dev/ttyACM*` |
| What application services other than 5 do, and whether `atx<ch> <n>` is the updater's `atx <n>` (§4) | unknown. Tactrix's DLL sends both only in its firmware updater |

### The firmware updater and other USB ids **[P]**

Read from Tactrix's DLL (2026-10-07). Its firmware updater sends `atp 5 2` with
two raw bytes and reads a seed from `arp 5 4` plus four bytes. It then sends
`atx <n>`. The cable re-enumerates as USB `0403:cc4b` ("Openport 2.0
Bootloader") and speaks `tbi`, `tbp`, `tbw` and `tbx`. It answers `tro` or
`tre <code>`.

The MCU is an LPC2364 with NXP's CRP2 code protection, so its flash
cannot be read out over ISP **[U]** (from forum update logs).

With a microSD card inserted, the cable enumerates as `0403:cc4c`, a storage
composite with no serial link (one user report, 2009). The driver names that
case.

---

## 13. Readings the cable refuted

| Claim | Source | What the cable said |
|---|---|---|
| `0x02` is J2534's `START_OF_MESSAGE` | two third-party drivers | `0x02` marks a 29-bit id (§7.4) |
| Five-baud init is `aty<ch> 1 1` + the address byte | `emdzej/j2534` | `are 7`. `atw` is five-baud init (§4) |
| `C`, `D` and `S` are the channels for the L line and the jack | `emdzej/j2534` | `are 7`. The channels are 7, 8 and 9 (§5) |
| `ato`'s fourth argument is a channel slot | `Mackanized/op2j2534` | it is ignored (§4) |
| Three channels at most | same | 5, 6, 7 and 9 open together (§5) |
| ISO 14230 opens at 10400 baud only | same | no baud rate is checked (§5) |
| `atf`'s second argument is a mask length | same | it is TxFlags (§4) |

---

## 14. Sources

| Source | What it settles |
|---|---|
| SAE J2534-1 DEC2004 (04.04) | indications, segmented replies and `RxStatus` bits |
| Vehicle and bench sessions, 2026-06-17 to 2026-11-12 | everything measured on a bus |
| `op20pt32.dll` 1.02.0.4868 run against the cable ([AB-OFFICIAL.md](AB-OFFICIAL.md)) | the commands Tactrix sends |
| `op20pt32.dll` 1.02.0.4868 unpacked in memory (2026-10-07) | the firmware updater, the reply parser and the `att` budget for `Timeout` 0 (50 ms on raw CAN, none on channel 9, 1 s otherwise, §4) |
| Tactrix `openport2.inf` and forums.openecu.org threads 4978 and 5432 | the bootloader and storage USB ids, the MCU and its code protection |
| Tactrix's setup: `j2534_tactrix.h` and the `canlogger` and `klogger` samples | private IOCTLs, OEM error codes and pin numbers |
| Tactrix EcuFlash changelog (firmware 1.41 and 1.44) | K-line echoes follow `LOOPBACK`, and received messages carry `CAN_29BIT_ID` |
| github.com/emdzej/j2534 @ `35aae08` | status bytes from a disassembly of the DLL |
| github.com/MCU-Innovations/opta-j2534-rs @ `9774569` | status bits and chunks, verified on a Honda CBR1000RR ECM |
| github.com/Aiden-korbs/openport2-winarm-j2534 | the `arw` reply, with HDS on a 2005 Honda CR-V |
| github.com/Mackanized/op2j2534 | command templates from the unpacked DLL |

## Reproducing this

`tools/probe/op_probe.c` prints the USB descriptors. For protocol work, the
serial device is easier to use than libusb:

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
