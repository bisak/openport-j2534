# The OpenPort 2.0 wire protocol

How a host talks to a Tactrix OpenPort 2.0 over USB. Tactrix publishes no
protocol specification, no SDK and no source, so everything here was
established by observation: sweeping the cable, recording vehicles, and
running Tactrix's own Windows DLL against the same cable
([`AB-OFFICIAL.md`](AB-OFFICIAL.md)).

Device: OpenPort 2.0, firmware `1.17.4877`, on macOS 26.6 (Apple Silicon),
libusb 1.0.29. Vendor DLL: `op20pt32.dll` 1.02.0.4868.

Every claim carries a confidence marker:

| Marker | Meaning |
|---|---|
| **[V]** | Verified: observed directly on the device above |
| **[P]** | Partial: the shape is confirmed, some field is not fully pinned down |
| **[U]** | Unverified: inferred, and flagged as needing a bus to confirm |

Bench measurements were made with the cable on USB power and repeated with
11.9 V on pin 16; none changed. Anything that needs a live bus was measured on
a 2012 VW Caddy (ISO15765 and raw CAN), a 2009 Audi 1.9 TDI (raw CAN; no
K-line responder) or a production chassis ECU on a bench harness (§10). Section
12 lists what is still open, and §13 the readings the cable refuted.

---

## 1. USB layer **[V]**

`0403:cc4d`, `iManufacturer` "Tactrix", `iProduct` "OpenPort 2.0", USB 1.1,
bus-powered at 100 mA, one configuration.

```
bDeviceClass 0x02 (CDC communications)
  interface 0  class 0x02 sub 0x02 proto 0x01   (CDC ACM, "AT commands V.250")
    endpoint 0x81  interrupt IN   16 bytes      (notification element)
  interface 1  class 0x0a                       (CDC data)
    endpoint 0x02  bulk OUT       64 bytes
    endpoint 0x82  bulk IN        64 bytes
```

The device is a standard CDC-ACM serial device. `bInterfaceProtocol 0x01` is
"AT commands", which is why every command begins `at`. There are
two ways to reach it:

1. **libusb on the bulk pair**, what this driver does.
2. **The CDC-ACM character device**: macOS enumerates
   `/dev/cu.usbmodem<serial>1`. No libusb, no claiming. Useful for scripting
   and for the protocol work that produced this document
   (`OPENPORT_DEVICE=/dev/cu.usbmodem...` points the driver at it).

### Claiming the interface on macOS **[V]**

`AppleUSBCDCACM` binds both interfaces, and `libusb_kernel_driver_active`
reports 1 for each:

| Interface | `detach_kernel_driver` | `claim_interface` |
|---|---|---|
| 0 (comm) | `LIBUSB_ERROR_ACCESS` | `LIBUSB_ERROR_ACCESS` |
| 1 (data) | `LIBUSB_ERROR_ACCESS` | success |

So: claim the interface carrying the bulk pair, never interface 0, and treat a
detach failure as expected rather than fatal. No `sudo`, no kext removal, no
boot arguments. Find the interface by walking configurations, interfaces and
altsettings for one exposing both a bulk IN and a bulk OUT rather than
hardcoding 1.

### Claiming the interface on Linux **[P]**

Linux binds `cdc_acm` to the comm interface, and `cdc_acm` claims the data
interface itself. Claiming the data interface from libusb needs its kernel
driver detached, and detaching either interface tears the whole ACM device
down, `/dev/ttyACM*` included; only re-probing the comm interface brings it
back. A driver that detaches must therefore reattach at close, comm interface
first, or the cable is unusable as a serial port until it is replugged. This
driver records every interface with a kernel driver at open and reattaches
them at close. Reported on Linux hardware (2026-09-21) and consistent with
`cdc_acm`'s design; this project's own cable has only been on macOS.

---

## 2. Framing **[V]**

Host to device: an ASCII line terminated `\r\n`, optionally followed
immediately by raw binary payload bytes.

```
at<verb>[<channel>] <arg> <arg> ...\r\n   [payload bytes]
```

Device to host, two kinds interleaved on the same pipe:

```
ar<verb> <args>\r\n                                    ASCII reply
'a' 'r' <channel-digit> <len:u8> <payload...>          binary message frame
```

They interleave: a message frame can arrive between a command and its
reply, so a driver must demultiplex the stream rather than read a reply
inline.

Byte 2 tells them apart: a letter for an ASCII reply, an ASCII digit for a
binary frame. That is the whole discriminator, and it is unambiguous because
the device accepts only single-digit channels. The length byte spans the full
0–255 range and must not be used to distinguish them.

An unrecognised command produces no reply at all: silence, not an error.

Spacing matters. Channel-scoped verbs fuse the digit to the verb (`ato6
…`, `atc6`); `atr`, `atv` and `atp` take a space before their first argument.
`ato 6 …` is parsed differently and fails.

---

## 3. Replies and sequence numbers **[V]**

| Reply | Meaning |
|---|---|
| `aro` | success, no value |
| `are <code>` | failure; `<code>` is a J2534 error number (§6) |
| `are <code> <detail>` | failure, echoing the offending value |
| `ari <text>` | informational text (`ari main code version : 1.17.4877`) |
| `arr <pin> <millivolts>` | pin voltage |
| `arf<ch> <filter_id>` | filter installed |
| `arg<ch> <param> <value>` | configuration value |
| `arm<ch> <id>` | periodic message installed |
| `arw<ch> <b> <b> …` | five-baud init result: the ECU's key bytes in decimal on the line **[P]**: third-party, measured with HDS on a 2005 Honda (`Aiden-korbs/openport2-winarm-j2534`); on this bench with no ECU the answer is `are 7` |
| `ary<ch> <n>` + `n` raw bytes | fast init result: the StartCommunication response follows the line as raw bytes **[P]**: the `dschultzca` lineage's reading; the failure reply `are 7` is measured, a success has not been captured |

Every text reply echoes the number its command carried. A command may end
with an extra decimal argument; the device appends it to the reply, and that
is how Tactrix's DLL ties a reply to its command (`AB-OFFICIAL.md`). Measured
2026-09-13: `ata 2` → `aro 2`, `atr 16 3` → `arr 16 108 3`, `atf6 3 64 4 8`
→ `arf6 0 8`, `atg6 30 10` → `arg6 30 0 10`, `atg6 127 9` → `are 1 9`,
`atn6 5 14` → `are 13 14`. Without a number the trailing field reads 0 on
`arf`/`arg` and is absent elsewhere. `ati` ignores a number (`ati 5` answers a
bare `ari`); `atl`, `aty` and `atw` echo it on failure (`aty3 4 0 10` →
`are 7 10`), and a success `ary` has not been observed with one.

This driver numbers every command but `ati` and accepts a reply only when it
echoes the number; an init reply is accepted with or without one. A reply
carrying another number, or none when one was expected, is stale or belongs
to an unwaited command and is dropped (§9).

---

## 4. Commands **[V]**

Verified by sweeping `at<a..z>` and then each verb's argument shape. Silence
means the verb does not exist: `atb atd ate ath atj atq atu`.

| Command | Args | Purpose |
|---|---|---|
| `ati` | | version: `ari main code version : 1.17.4877` |
| `atz` | | reset: closes every channel, stops every periodic message, switches every output off; replies `aro` |
| `ata` | | close every channel and switch every output off, like `atz`; answered by number. A filter installed before `ata` answers `are 2` after it |
| `ato<proto>` | `<flags> <baud> 0` | open a channel. The fourth field is ignored (channels 5, 6 and 3 opened with 0 simultaneously, `ato5 … 7` was accepted, a second `ato5` gets `are 20` whatever it carries); the vendor DLL puts its own handle bookkeeping there when opening ISO9141 |
| `atc<ch>` | | close a channel; stops its periodic messages; replies `aro` even for a channel that was never open |
| `att<ch>` | `<len> <txflags> <timeout_us>` + payload | transmit. `<timeout_us>` is the firmware's budget for getting the message onto the bus; the vendor DLL sends the caller's `WriteMsgs` Timeout in microseconds (1 000 000 for a Timeout of 0). Without it the firmware gives up after ~1 s |
| `atf<ch>` | `<type> <txflags> <len>` + payload | install a filter: type 1 PASS, 2 BLOCK, 3 FLOW_CONTROL; `<len>` is the length of **one** appended message and the device expects 2 or 3 of them; `atf6 3 0 12` with 12 bytes answers `are 10` and desynchronises the parser (§9.1) |
| `atk<ch>` | `<filter_id>` | remove a filter; `are 22` for an unknown id. `atk<ch> -1` removes every filter on the channel (`aro`, also with none installed; the old ids then answer `are 22`); it is the vendor DLL's `CLEAR_MSG_FILTERS` and this driver's. Filter ids are never reused within a session: they count up across all channels and restart at `ata`/`atz` (measured 2026-09-24) |
| `atg<ch>` | `<param>` | read a configuration value (§8) |
| `ats<ch>` | `<param> <value>` | write a configuration value |
| `atm<ch>` | `<interval_us> 0 <txflags> <len>` + payload | start a periodic message; replies `arm<ch> <id>`; measured against a bench ECU (§10) |
| `atn<ch>` | `<msg_id>` | stop a periodic message; `are 13` for an unknown id |
| `atr` | ` <pin>` | read a pin voltage in millivolts (§8) |
| `atv` | ` <pin> <millivolts>` | programming voltage or ground on a pin (§8) |
| `aty<ch>` | `<len> 0` + request bytes | K-line fast init; returns in ~108 ms with a 25/25 ms wake pulse |
| `atw<ch>` | `<address>` (decimal, no payload) | K-line five-baud init, `atw3 51` for 0x33, as the vendor DLL sends it; blocks ~2450 ms, one byte clocked out at 5 baud |
| `atl<ch>` | | stop every periodic message on the channel: answers `aro`, and the old ids then answer `are 13` (measured 2026-09-24). The vendor DLL's `CLEAR_PERIODIC_MSGS` and this driver's |
| `atp` | ` <pin> <value>` | a pin verb sharing `atv`'s argument shape (the vendor DLL's format-string table has one template, `at%c %d %d %u`, for `p` and `v`). Answers `are 10` for every pin and value tried (pins 0, 1, 2, 6, 12, 15; values 1, 5000, 8000, 12000, 20000, `SHORT_TO_GROUND`, `VOLTAGE_OFF`), `are 5` for value 0, after 0.3–1.3 s. Function unknown; the DLL is not seen sending it |
| `atx<ch>` | `<n>` | exists, echoes its argument in the error (`are 7 1`); purpose unknown |

### What Tactrix's DLL sends **[V]**

Measured by running `op20pt32.dll` 1.02.0.4868 against the simulator and
against the cable (`AB-OFFICIAL.md`):

```
\r\n\r\nati                       bare, once, at open
ata 2                              then every command carries a sequence number
atr 16 3                           READ_VBATT
ato6 0 500000 0 4                  connect
ats6 30 0 5 / ats6 31 0 6          one ats per SET_CONFIG parameter
atf6 3 64 4 7 + 12 bytes           filter: type, txflags, per-message length
att6 6 64 1000000 8 + payload      transmit with Timeout=1000 (budget = Timeout in µs)
att6 153 64 5000000 9 + payload    transmit with Timeout=5000
att6 6 64 1000000   + payload      transmit with Timeout=0: 1 s budget, no number, no wait
atm6 100000 0 64 6 9 + payload     periodic, 100 ms, interval in microseconds
atn6 0 10                          stop periodic
atk6 0 11 / atc6 12 / atz 13       stop filter / disconnect / close
atv 12 -1 14                       VOLTAGE_OFF, printed signed
ato4 0 10400 0 3 / aty4 4 0 4 + 4  ISO14230 connect, fast init
ato3 0 10400 3 7 / atw3 51 8       ISO9141 connect, five-baud init to 0x33
```

The DLL waits for `aro <seq>` (500 ms for most commands, 5 s for `atr` and
`atg`) and ignores a reply without its number. An `att` sent without a
number gets no reply at all, not even `are 9` on a bus with no ACK peer, and
the firmware answers nothing else for about a second while it keeps trying;
the numbered form on the same bus answers `are 9 <seq>` after 1.0 s. When
`ati` gets no answer the DLL sends `tbi\n` (LF only) three times at 300 ms
and gives up; the application firmware does not answer it with either
terminator, so it is a bootloader query. `PassThruReadVersion` is answered
from the cached `ati` reply. This driver sends the same commands with the
same arguments except in two places: at open it sends `\r\n\r\n` as the DLL
does, then `atz` and `ata`, and `ati` after them; and it numbers a `Timeout=0`
write so that its late reply can be recognised and dropped
(`AB-OFFICIAL.md`).

---

## 5. Channels and protocols **[V]**

The channel id is the firmware protocol number. `ato6` opens ISO15765, a
second `ato6` answers `are 20` (`ERR_CHANNEL_IN_USE`), `atc6` closes it and
`ato6` opens again. Tactrix's DLL opens every J2534 id on these channels:

| Channel | J2534 ids | Line |
|---|---|---|
| 3 | `ISO9141`, `ISO9141_CH1` (`ISO9141_K`) | K, pin 7 |
| 4 | `ISO14230`, `ISO14230_CH1` (`ISO14230_K`) | K, pin 7 |
| 5 | `CAN`, `CAN_CH1` | CAN, pins 6 and 14 |
| 6 | `ISO15765`, `ISO15765_CH1` | CAN, pins 6 and 14 |
| 7 | `ISO9141_CH2` (`ISO9141_L`) | L, pin 15 |
| 8 | `ISO14230_CH2` (`ISO14230_L`) | L, pin 15 |
| 9 | `ISO9141_CH3` (`ISO9141_INNO`) | RS-232 receive on the 2.5 mm jack |

For J1850 and SCI (ids 1, 2, 7–10) the DLL sends `ato0`, gets `are 3`, and
returns `ERR_INVALID_PROTOCOL_ID`; Tactrix's header marks them "not
supported". `ato0`, `ato1`, `ato10` and `ato11` all answer `are 3`, and a
letter in the channel position (`atoC`) answers `are 7` with no sequence
number: the letter is not parsed.

Protocols sharing a line are mutually exclusive, and the refusal is `are 3`,
not `are 20`: `ato4` is refused while ISO9141 holds K, `ato8` while ISO9141
holds L, and the other way round. Otherwise there is no cap on open
channels: 5, 6, 7 and 9 open together, and so do `ato7`, `ato3 4096 …`
(`ISO9141_K_LINE_ONLY`) and `ato9`.

Baud rate is not validated on open. `ato5 0 123456 0`, `ato3 0 4800 0`,
`ato4 0 9600 0` and `ato6 0 0 0` all answer `aro`. No `ERR_INVALID_BAUDRATE`
(25) was ever produced; what a wrong rate does on the bus is unmeasured.

ISO-TP segmentation and flow control are performed by the firmware. The
host sends a whole service request and the firmware handles first frame,
consecutive frames, flow-control frames on receive, and the ECU's block size
and separation time on transmit. The host only passes the parameters through:
`ISO15765_BS` (30) and `ISO15765_STMIN` (31) for the flow control we send,
`BS_TX` (34) and `STMIN_TX` (35) for what we request of the ECU, and
`ISO15765_WFT_MAX` (37) for how many WAIT frames to tolerate.

What the host does have to manage is the USB link above ISO-TP: one
command in flight at a time, a timed-out command's late reply discarded rather
than handed to the next command (§9), a caller's `Timeout` spent across a
whole `WriteMsgs` call rather than per message, and a receive queue whose
overruns are reported rather than silently dropped.

---

## 6. Error codes are J2534 codes **[V]**

`are <n>` carries a literal SAE J2534-1 return code, so the mapping is the
identity function. Confirmed by provoking each one:

| Provocation | Reply | J2534 |
|---|---|---|
| `atg6 0` (unsupported parameter) | `are 1` | `ERR_NOT_SUPPORTED` |
| `ato1 …` (J1850) | `are 3` | `ERR_INVALID_PROTOCOL_ID` |
| `atp 12 0` (pin verb, zero value) | `are 5` | `ERR_INVALID_IOCTL_VALUE` |
| malformed command | `are 7` | `ERR_FAILED` |
| transmit with no bus | `are 9` | `ERR_TIMEOUT` |
| `atf6 3 0 12` (wrong payload length) | `are 10` | `ERR_INVALID_MSG` |
| eleventh `atm` on a channel | `are 12` | `ERR_EXCEEDED_LIMIT` |
| eleventh live `atf` on a channel | `are 12` | `ERR_EXCEEDED_LIMIT` |
| `atn5 0` (no such periodic message) | `are 13` | `ERR_INVALID_MSG_ID` |
| `atr 0` (not a readable pin) | `are 19` | `ERR_PIN_INVALID` |
| second `ato` on one protocol | `are 20` | `ERR_CHANNEL_IN_USE` |
| `atk6 1` (no such filter) | `are 22` | `ERR_INVALID_FILTER_ID` |
| `atf5 0 …`, `atf5 4 …` (no such filter type) | `are 22` | `ERR_INVALID_FILTER_ID` |
| `atv 12 20001` | `are 119` | Tactrix `ERR_OEM_VOLTAGE_TOO_HIGH` (0x77) |
| `atv 12 4999` | `are 120` | Tactrix `ERR_OEM_VOLTAGE_TOO_LOW` (0x78) |

Pass them through. Inventing a mapping loses information the device already
gave correctly.

---

## 7. Binary message frames

### 7.1 Layout **[V]**

```
  offset  size  field
       0     2  'a' 'r'
       2     1  channel, as an ASCII digit ('3'..'9')
       3     1  length of everything that follows
       4     1  status (bitfield, below)
       5     4  timestamp, microseconds, big-endian
       9   n-5  payload
```

Frame stride is `4 + len`, so a frame carries at most 250 payload bytes. A
real capture:

```
61 72 36 | 09 | 10 | 19 50 b8 0c | 00 00 07 e0
'a''r''6'  len  sts   timestamp     CAN id 0x7E0
```

The timestamp is microseconds since power-on (two frames captured 1.2 s
apart differed by 1 238 083) and wraps every ~71 minutes.

For CAN and ISO15765 the payload begins with the 4-byte big-endian CAN
identifier, matching J2534's `Data[0..3]` convention, followed by the
service data. K-line frames differ (§7.9).

### 7.2 The status byte **[V]**

| Bit | Meaning |
|---|---|
| `0x80` | START: announces a segmented message (§7.3) |
| `0x40` | END: the frame completes a message |
| `0x20` | LOOPBACK: our own transmit echoed back (§7.7) |
| `0x10` | transmit indication (§7.5) |
| `0x02` | the frame's CAN id is 29-bit (§7.4) |

### 7.3 What the bits mean on a live bus **[V]**

Measured on a 2012 VW Caddy over ISO15765 at 500 kbit (2026-09-13) and on a
vehicle's chassis controller (2026-06-17):

| Sequence on the wire | Meaning |
|---|---|
| one `0x40` frame: `id` + data | a message that fit one CAN frame is a single END frame; nothing precedes it |
| `0x80` frame: `id` only, then `0x40` frame: `id` + data | START carries the CAN id and nothing else. It announces that a segmented message has begun; the data follows in END-terminated frames that carry the id again |

Observed for every segmented response (`$1A 87`, `$1A 9A`, `$1A 9C`,
`$21 E4`, `$22 F1 90`, `$09 02`) and every single-frame one. No `0xC0`
(START|END) frame was seen on receive; the driver accepts one as a complete
message because the bits imply it.

A driver must treat a START-without-END frame as an indication: in J2534
terms `ISO15765_FIRST_FRAME`, `DataSize` 4, `ExtraDataIndex` 0, exactly what
J2534-1 §8.6 and A.4 prescribe for the first frame of a segmented receive.
Folding it into the data yields a message with the CAN id twice and
`START_OF_MESSAGE` set, which every J2534 consumer discards as a first-frame
marker; Tactrix's own `canlogger` and `klogger` samples both begin
`dump_msg` with `if (msg->RxStatus & START_OF_MESSAGE) return;`.
`tests/unit/test_golden.c` replays the Caddy's VIN reply so this cannot
regress.

Independent readings agree: `emdzej/j2534` (from a Ghidra disassembly of the
vendor DLL, firmware 1.17.4877) maps `0x80` to a `START_OF_MESSAGE` message
carrying the id and `0x40` to the data message; `MCU-Innovations/opta-j2534-rs`
(verified on a Honda CBR1000RR ECM including a 2 MB flash transfer) reads
`0x80` as the start-of-message marker and strips the id from every frame after
the first. Note that `opta-j2534-rs`, `firefighter-19/tuneforge` and the
`dschultzca`/`NikolaKozina` C driver are one lineage; only `emdzej/j2534` is
independent of it, so read "several sources agree" as two lines of evidence.

### 7.4 The low nibble **[V]**

Transmitting the same request four ways on the Caddy:

| TxFlags | Status byte of the transmit indication |
|---|---|
| `0x000` | `0x10` |
| `0x040` `ISO15765_FRAME_PAD` | `0x10` |
| `0x100` `CAN_29BIT_ID` | `0x12` |
| `0x140` both | `0x12` |

`0x02` tracks the 29-bit identifier exactly and nothing else; two third-party
drivers read it as J2534's `START_OF_MESSAGE`, which would make `0x12`
"transmit done and also the start of a message". The driver carries it
through as `CAN_29BIT_ID` on CAN channels. `0x44` (end with `RX_BREAK`) is a
third-party claim, unmeasured here. Tactrix's changelog for firmware 1.44,
"return CAN_29BIT_ID flag on appropriate read results", is consistent.

### 7.5 The transmit indication **[V]**

Every successful transmit produces one `0x10` frame carrying the 4-byte CAN
id and no data, immediately after the `aro`. It is the device's transmit
indication; a driver that mistakes it for received data reports a phantom
message on every write. J2534-1 §8.6 defines it: `TX_MSG_TYPE | TX_DONE`,
`DataSize` 4, `Data` the CAN id of the message just sent, `ExtraDataIndex`
0, and that is how both the vendor DLL (measured on the Audi: `RxStatus 9,
DataSize 4`) and this driver deliver it. Periodic messages produce none
(§10).

### 7.6 Long replies arrive in 70-byte chunks **[V]**

A segmented ISO15765 reply is forwarded as it arrives, 70 data bytes (ten
consecutive frames) at a time, every chunk repeating the 4-byte CAN id:
a `0x80` announcement with the id only, `0x00` middle chunks, a `0x40` last
chunk. Measured on the bench ECU of §10, 2026-09-24, over all 6 162
segmented replies of a 512 KB `$23` read (a raw wire log, `OPENPORT_LOG_HEX=1`),
every one in exactly this shape:

| Message (id + data) | Chunks after the announcement |
|---|---|
| up to 69 B | one: id + all data |
| 133 B (4 097 replies) | 73 + 64 |
| 259 B | 73 + 74 + 74 + 50 |
| 260 B (2 057 replies) | 73 + 74 + 74 + 51 |

The first chunk carries the first frame's six data bytes and nine
consecutive frames (id + 69), each later one ten (id + 70). The 250-byte limit
of a wire frame is never reached. Tactrix's DLL strips four bytes from every
continuation chunk (fed the id only in the first chunk it drops four bytes
per chunk, `AB-OFFICIAL.md`), and so does this driver; the 512 KB image read
this way matched two passes at different read sizes and an earlier dump.

### 7.7 Seeing your own transmit, and LOOPBACK **[V]**

On a raw CAN channel with a pass-all filter, the frame you just sent comes
back to you as an ordinary `0x00` frame. It is not a loopback: with a filter
that excludes the transmitted id it does not come back. A CAN node sees
its own transmission on the wire, and a promiscuous filter shows it to you,
indistinguishable from an ECU's frame. Filter by identifier.

`LOOPBACK` (`ats<ch> 3 1`) adds a *second*, separate frame marked `0x20`
whose payload is four zero bytes rather than the CAN id. Measured on raw CAN
against the bench ECU (§10, 2026-09-16); the vendor DLL delivers it as a
`TX_MSG_TYPE` message of four zero bytes, and so does this driver. On a bench
with no bus a transmit still fails with `are 9` before any echo is produced:
the echo follows a *successful* bus transmit.

The transmit echo on ISO15765 is reported on raw CAN channel 5, not on the
ISO15765 channel, measured against the bench ECU of §10 on 2026-09-24 **[V]**. A
single-frame request with `LOOPBACK` on the ISO15765 channel gave, in order:
`ar5` `0x20` with four zero bytes (the echo of the request; a 20-byte request
gave three, one per CAN frame the cable sent), `ar6` `0x10` with
the CAN id (TxDone), and, while the ECU's segmented reply arrived, `ar5` `0x20`
carrying the flow-control frame the cable sent, id and data intact
(`00 00 07 b5 30 00 00 00 00 00 00 00`). Nothing is flagged on channel 6. With
channel 5 open and a filter passing the request id, channel 5 also gets the
ordinary `0x00` copies of both frames (as above). Tactrix's DLL delivers the
`0x20` frames to the ISO15765 channel unchanged, `ProtocolID` CAN,
`TX_MSG_TYPE`; with channel 5 also open it delivers every channel-5 frame to
the ISO15765 channel and none to the CAN channel. This driver delivers them to
the ISO15765 channel only while channel 5 is closed.

### 7.8 Raw CAN frames carry no START or END **[V]**

On a raw CAN channel (5) every frame arrives with a status byte of `0x00`,
our own echo and the ECU's reply alike (Caddy, 2026-09-13). Raw CAN has no
transport layer, so one wire frame is one whole message. A driver that waits
for an END bit swallows every raw CAN frame; `caddy_raw_can_replay` in
`tests/unit/test_golden.c` is the regression. Against the vendor DLL the
frames are message-for-message identical: `RxStatus` 0, `DataSize` 8,
`ExtraDataIndex` 8, CAN id first.

### 7.9 The K-line frame layout **[P]**: sourced, not yet measured here

For channels `'3'` and `'4'` (and presumably 7–9) the layout is not the
uniform one above:

| Frame | Body after the status byte |
|---|---|
| `0x00` data, `0x20` loopback data | the K-line bytes themselves, **no timestamp** |
| `0x80` start, `0xA0` loopback start | the 4-byte timestamp, no data |
| `0x40` end, `0x60` loopback end | the 4-byte timestamp, no data |
| `0x10` transmit indication | the 4-byte timestamp |

Every implementation that has run K-line on this cable describes this
asymmetry: `emdzej/j2534` (firmware `1.17.4877`, transmit encoder from a
Ghidra disassembly of the vendor DLL: "K-line packets have no
timestamp"), `opta-j2534-rs` (K-line verified on a Honda PGM-FI ECM: "the
firmware does not insert a timestamp; payload begins immediately after kind";
"the RxEnd payload is a 4-byte free-running timestamp, not user data"),
`tuneforge`, and the `dschultzca` lineage. One of them has a fixture with an
END frame carrying no body at all; the parser accepts both shapes. A uniform
rule would eat the first four bytes of every K-line message.

Whether the checksum byte is delivered in the data, and how the firmware
marks a checksum error, is unmeasured. Tactrix's changelog for firmware 1.41
("fix ISO K and L lines to do proper J2534 LOOPBACK functionality: you will
no longer get echoes of your commands unless LOOPBACK=1") says K-line echoes
follow the flag.

`tools/car/car_capture.py --kline` records every K-line frame's raw body and
`analyse_capture.py` tests both layouts against the recording: under this one
the counter advances only on start/end frames and the data frames begin with a
K-line header byte.

---

## 8. Configuration and pins **[V]**

### 8.1 Configuration parameters

`atg<ch> <param>` / `ats<ch> <param> <value>`, with J2534 parameter numbers.
Support is per protocol. Swept 1–37 with both `atg` and `ats`:

| Protocol | Supported parameters |
|---|---|
| 6 ISO15765 | 1 DATA_RATE, 3 LOOPBACK, 23 BIT_SAMPLE_POINT, 24 SYNC_JUMP_WIDTH, 30 ISO15765_BS, 31 ISO15765_STMIN, 34 BS_TX, 35 STMIN_TX, 37 ISO15765_WFT_MAX |
| 4 ISO14230 | 1 DATA_RATE (readable, not settable), 3 LOOPBACK, 7 P1_MAX, 10 P3_MIN, 12 P4_MIN, 14–18 W1–W5, 19 TIDLE, 20 TINIL, 21 TWUP, 22 PARITY, 25 W0, 32 DATA_BITS, 33 FIVE_BAUD_MOD |

Anything outside a protocol's set answers `are 1` (`ERR_NOT_SUPPORTED`). The
driver forwards every parameter unfiltered and returns the device's answer.
`W1` raised to 1000 ms is honoured: `atw` then takes 3501 ms instead of 2451.

Tactrix's own parameter `TX_PARAM_STOP_BITS` (0x9000) exists: `atg9
36864` reads 1 on the jack channel and `atg3 36864` on ISO9141, and `ats9
36864 2` is accepted and reads back 2 (through the vendor DLL, 2026-09-16).

`SNIFF_MODE` goes to the firmware in `ato`'s flags (`ato5 268435456
500000 0`) and is accepted, but the cable still acknowledges frames (§10).
Tactrix's `canlogger` sample connects with `SNIFF_MODE | CAN_ID_BOTH`; this
driver passes the flag through as the vendor DLL does and logs the caveat.

Tactrix's private IOCTLs (`TX_IOCTL_*`, 0x70000+) reach nothing on the
wire when called with a NULL input; their input structures are known only
from the `klogger` sample (`TX_IOCTL_APP_SERVICE` with service 5, info 1
returns the serial number). This driver does not implement them.

`READ_PROG_VOLTAGE` takes a pin in Tactrix's DLL. J2534-1 passes `pInput`
NULL; the vendor DLL reads a pin number through `pInput` and sends `atr
<pin>`, and with NULL returns -1 and sends nothing (measured under emulation,
2026-09-16). This driver accepts both: the named pin when `pInput` is given,
pin 12 when it is NULL.

### 8.2 Readable pins

`atr <pin>`, sweeping 0–20: four pins answer, all others `are 19`. The
meanings are from Tactrix's header (`j2534_tactrix.h`); the readings are
measured, in millivolts:

| Pin | Reading | Meaning |
|---|---|---|
| 8 | 0 with nothing attached | J1962 pin 8 (OEM8), an ADC input |
| 12 | 0 with nothing attached | J1962 pin 12 (OEM12), an ADC input; also the pin `atv 12` drives and the tip of the 2.5 mm jack |
| 16 | 130–152 floating on USB power; 11 894 from an 11.9 V bench supply; 12 156–12 199 on a vehicle with a ~12.2 V battery | J1962 pin 16, battery: `READ_VBATT` |
| 17 | 5729–5794, drifting, with no output enabled | `PIN_VADJ`, the adjustable output supply that `atv` switches onto a pin; not a J1962 pin |

The 2.5 mm jack takes pin 12 away from the vehicle. Per Tactrix, the
jack's tip is OEM12 and inserting a plug disconnects OEM12 from J1962 pin 12;
`atr 12` then reads the jack tip and `atv 12` drives it. Its ring and sleeve
are an RS-232 receive input for Innovate MTS devices, channel 9. **[U]**: no
plug was inserted.

### 8.3 Programming voltage and ground

`atv <pin> <millivolts>`, with `VOLTAGE_OFF` (0xFFFFFFFF) and
`SHORT_TO_GROUND` (0xFFFFFFFE) accepted signed (`-1`, `-2`, as the vendor DLL
prints them) or unsigned:

```
atv 12 -1          -> aro        VOLTAGE_OFF
atv 12 0           -> are 120    zero is rejected; use VOLTAGE_OFF
atv 7 -2           -> aro        SHORT_TO_GROUND
atv 2 5000         -> are 19     pin 2 is not an atv pin, whatever the value
atv 12 5000        -> aro        atr 12 -> 5075
atv 12 12000       -> aro        atr 12 -> 12199, atr 17 -> 12330
atv 12 20000       -> aro        atr 12 -> 20238
atv 12 20001       -> are 119    ERR_OEM_VOLTAGE_TOO_HIGH, so is 25000
atv 12 4999        -> are 120    ERR_OEM_VOLTAGE_TOO_LOW
atv 13 5000 / atv 12 9000 -> aro aro, atr 17 -> 9302: one supply for all pins
atv 0 8000         -> aro        atr 12 -> 8104: the jack drives pin 12, no plug in
atv 12 7000 / ata  -> atr 12 -> 0: ata switches outputs off, so does atz
```

What each pin supports, from Tactrix's header and product description:

| Pin | Supports |
|---|---|
| 0 (`AUX_PIN`, the 2.5 mm jack) | ground, voltage; drives pin 12 while no plug is inserted |
| 1, 3, 9, 11, 12, 13 | ground, voltage |
| 2 (J1850+) | 5 V and 8 V per the header, but `atv 2` answers `are 19` and `atp 2` `are 10` for every value on this firmware, consistent with J1850 being unsupported |
| 7 (K), 10 (J1850−), 15 (L) | ground |
| 8, 12, 16, 17 | reading (`atr`) |

Facts a driver has to act on, because the cable does not:

- The range is 5000–20000 mV, inclusive. Tactrix's description says
  5–25 V; the firmware refuses anything above 20 V.
- All voltage pins share one supply. Setting a second pin moves the first
  to the new voltage without switching it off. J2534-1 §7.2.11 requires one
  pin at a time; this driver refuses a second pin with `ERR_PIN_INVALID`
  until the first is off. The vendor DLL passes it through.
- Pin 17 reads back the supply within about 3 %, and keeps its last
  setpoint after the pin is switched off.
- Grounding K kills the channel it carries. `SHORT_TO_GROUND` on pins 7
  and 15 answers `aro` including while ISO9141 is open on K, and the
  vendor DLL passes the call through. This driver refuses to ground K under
  an open ISO9141 or ISO14230 channel, or L under an open L-line channel, and
  refuses to open such a channel on a pin it has grounded (`ERR_CHANNEL_IN_USE`
  both ways). L is not guarded under K-line channels: J2534-1 lets them use L
  for initialisation unless opened `ISO9141_K_LINE_ONLY`, and whether this
  firmware drives L on channels 3 and 4 needs a scope on pin 15 during a
  five-baud init.
- `ata` and `atz` release every output. This driver sends both at open
  and `atz` at close, so a session never inherits a live pin and leaves none
  when it closes.

---

## 9. Two hazards a driver must handle

### 9.1 A bad payload desynchronises the parser **[V]**

Send `atf6 3 0 12` with 12 bytes when the device expects 4, and it consumes
bytes you meant as the next command as payload. Subsequent commands vanish
into silence. Recovery: newlines to terminate the partial line, then `atz`,
then confirm `ata` answers `aro`. This is why the vendor DLL, and this driver,
open with `\r\n\r\n`.

### 9.2 Replies carry no tag unless numbered **[V]**

Replies are positional. If the pipe holds one stale reply, every subsequent
command receives the previous command's answer: a silent wrong result. Both
ways this happens were observed: a previous session left replies queued (a
backlog seven deep after an interrupted session made the driver read the
firmware version as the answer to `atr 16`), and a command timed out and the
device answered later (a CAN transmit with no bus takes ~1.2 s to be rejected
with `are 9`, which a caller that gave up after 200 ms leaves for the next
command).

The sequence-number echo (§3) is the structural fix, and it is the vendor's
own: number every command, accept only the reply that carries the number
back, and discard a reply arriving when no command is waiting. A "send `ata`
twice and hope" workaround clears a backlog exactly one reply deep.

---

## 10. Against a bench ECU, 2026-09-16 **[V]**

A production chassis ECU on a bench harness: OBD 4/5, 6, 14 and 16 only,
60 Ω termination, 11.8 V supply, no other CAN node. The module broadcasts a
4-byte status frame every 20.0 ms, which makes it the acknowledging peer a
one-cable bench lacks. One session-less identification request answered in
two wire frames and reassembled to 22 bytes through the driver. Periodic tests
used 0x7FF, an id absent from the module's receive table, with a zero payload.

Firmware periodic messages (`atm`):

| Question | Measured |
|---|---|
| Timing | 100 ms → 20 frames in 2 s, 100.0 ms apart; 1 ms and 5 ms intervals also hold |
| Visibility | with `LOOPBACK` on, each transmission shows as the frame plus the `0x20` echo; no `0x10` transmit indication is produced |
| `atn` | answers in ~60 ms; no frame follows; the id then answers `are 13` |
| `atc`, `ata`, `atz` | each stops every periodic on the channel |
| Host process killed (SIGKILL) | the periodic keeps running: a second process that sent nothing saw 39 frames in the next ~4 s; it stopped only on `atn` |
| Concurrency | 10 per channel; the eleventh `atm` answers `are 12` |
| Interval 0 | accepted (`arm`) and sends nothing |
| Ids | never reused: they kept counting (1…18) across `atc`, `ata` and `atz` |

This driver schedules periodic messages with `atm`/`atn`, as the vendor DLL
does. A firmware periodic outlives a crashed application; `PassThruOpen`
resets the cable, so the next session starts clean, and `PassThruClose`
resets it too. A process killed between the two leaves the message running
until the next open, exactly as with the vendor driver.

`SNIFF_MODE`. Three windows on raw CAN with a pass-all filter: normal
open, 2 s → 101 status frames; `ato5 268435456`, 3 s → 151 frames; normal
again, 2 s → 101 frames; 20.0 ms apart in all three. On a `SNIFF_MODE`
channel `att5 12 0 1000000` answers `aro` in 60 ms and the frame appears on
the bus, which a listen-only controller cannot do; the same on a first open
straight after `atz`/`ata`, and with `SNIFF_MODE | CAN_ID_BOTH`
(`ato5 268437504`). Firmware 1.17.4877 accepts the flag and ignores it.

Transmit with a peer. `att5 12 0 1000000` answers `aro`; on an empty bus
it is `are 9` after ~1.3 s, with or without battery voltage on pin 16.

Against Tactrix's DLL on live traffic (`AB-OFFICIAL.md` replays):

- `ISO9141_CH1` opens as `ato3` and `ISO14230_CH1` as `ato4`.
- Raw CAN receive is message-for-message the same (§7.8), and so is the
  transmit-done indication (§7.5).
- With `LOOPBACK` on and a pass-all filter, one transmit gives the application
  two messages from both drivers: the frame itself (`RxStatus` 0, 12 bytes)
  and a `TX_MSG_TYPE` message of four zero bytes.
- Receive queue. Left unread for 30 s, the vendor DLL delivered all ~1,500
  frames. A 64-message queue kept 64 and dropped 1,438; this driver stores
  each message at its own size in a 1 MiB ring per channel (about 29,000 raw
  CAN frames) and delivered 1,503 frames over the same 30 s with no gap.

---

## 11. Legislated OBD-II services on a vehicle **[V]**

SAE J1979 / ISO 15031-5, every read among them exercised on a 2012 VW Caddy on
2026-09-13 through the driver's own entry points:

| Service | What it reads | On this vehicle |
|---|---|---|
| `$01` | current powertrain data | answered; PID bitmaps 00, 20, 40 present, 60 absent (end of chain) |
| `$02` | freeze-frame data | answered |
| `$03` | stored emissions DTCs | answered, none stored |
| `$05` | oxygen sensor test results | not applicable, superseded by `$06` on CAN |
| `$06` | on-board monitoring test results | answered |
| `$07` | pending DTCs | answered, none pending |
| `$09` | vehicle information | answered: 00, 02 VIN, 04 calibration id, 06 CVN, 0A ECU name |
| `$0A` | permanent DTCs | silent: not supported by this vehicle |

`$04` (clear DTCs) and `$08` (on-board system control) write or actuate and
were never sent. An unsupported mode or PID draws no reply at all rather
than a negative response, so silence is a conformant answer.

Two things a caller must get right:

- Pad the request. ISO 15765-4 clause 8.1 requires a DLC of eight on every
  diagnostic CAN frame and says a shorter one shall be ignored. The OpenPort
  pads only when the caller sets `ISO15765_FRAME_PAD`; the Caddy ignored the
  same request unpadded. The simulator's ECU enforces this too.
- Flow control goes to the physical id. A functional request (`0x7DF`)
  whose reply is segmented needs its flow-control filter addressed to the
  ECU's *physical* id, not to `0x7DF`; broadcast to `0x7DF` it never arrives
  and every multi-frame reply stalls after the first frame with no error.

VAG is a different transport. VAG modules of that era are reached with VW
TP2.0 on raw CAN (a channel opened on id 0x200, the module naming the pair of
ids to use). On a 2009 Audi 1.9 TDI, on 2026-09-13, the capture tool's
ISO15765 requests got no answer while every frame was acknowledged; the tool
sent them unpadded at the time (above). On the same car's raw CAN channel the
engine answered a legislated `$01 00` sent to 0x7DF, on 0x7E8, identically
through both drivers (`AB-OFFICIAL.md`). `tools/car/car_capture.py --tp20`
implements TP2.0 over this driver's raw CAN channel and `s_tp20` in the
simulator scenarios covers it. On the same car, the same day, it opened a
channel to the engine module (0x01) and read five identification records
through it, the flash-status record `1A 9C` among them.
The same car's K-line answered no five-baud or fast init at any of eight VAG
addresses through either this driver or the vendor DLL, although the line is
electrically present (a fast init takes 126–132 ms on the car against 78 ms on
the bench). The failure is not the driver's: either OBD pin 7 is not on a live
K-line on this car, or the cable's five-baud waveform is not what this ECU
accepts (`AB-OFFICIAL.md`).

---

## 12. What is still open

| Question | What would close it |
|---|---|
| K-line frame layout (§7.9), the five-baud `arw` reply and a fast-init `ary` success | one K-line session recorded with `car_capture.py --kline`, or a bench OBD simulator that speaks ISO 9141-2 / KWP2000 |
| Whether the L line and the 2.5 mm jack carry data on channels 7–9 | an L-line ECU or an Innovate device |
| Whether the firmware drives L on channels 3 and 4 | a scope on pin 15 during a five-baud init |
| How extended addressing (`ISO15765_ADDR_TYPE`) is marked on receive | an ECU that uses it |
| The reattach at close on Linux (§1), as this driver does it | `examples/op_smoke` twice on a Linux machine, then `ls /dev/ttyACM*` |
| `atx` and `atp` | unknown; neither is sent by the vendor DLL |

Settled by measurement, for anyone checking a claim: the DLL's five-argument
forms are accepted and the trailing number echoed (`ato6 0 500000 0 1` →
`aro 1`); two-digit protocol numbers answer `are 3`; letter channel bytes
answer `are 7`; `atv 12 -1` is accepted; `tbi` is silent; filter ids are not
reused within a session (33 start/stop cycles reached id 32, 2026-09-24).

---

## 13. Readings the cable refuted

Every claim here was put to the cable rather than adopted or dismissed. The
first group are this project's own earlier readings, kept because each is a
trap for the next implementer; the rest are third-party claims.

| Claim | Source | What the cable said |
|---|---|---|
| A START frame carries the first chunk of data | this project's first model | START carries the CAN id only; the data follows with the id repeated (§7.3) |
| The `0x10` frame is rare, or is received data | this project | every successful transmit produces one (§7.5) |
| The low nibble is J2534 `RxStatus`, `0x02` = `START_OF_MESSAGE` | two third-party drivers | `0x02` tracks the 29-bit id (§7.4) |
| Every K-line frame carries a timestamp, as on CAN | this project | data frames carry none, per every K-line implementation (§7.9) |
| Five-baud init is `atw<ch> 1` + the address as a raw byte | this project | the firmware took the 1 as the address and the byte as the next command; the DLL sends `atw3 51` (§4) |
| Five-baud init is `aty<ch> 1 1` + address byte | `emdzej/j2534` | answers `are 7` exactly like `aty<ch> 1 0`; `aty` is fast init |
| The five-baud reply is `ary` | this project | `arw<ch> <b> <b>` with HDS on a Honda (§3) |
| `atp` is the periodic-message command | this project | it is a pin verb with `atv`'s shape; `atm` is the periodic command (§4) |
| Channels 7 and 8 are SCI A engine/trans | this project | the L line and the jack; SCI opens `ato0` in the vendor DLL (§5) |
| `ato`'s fourth argument is a channel slot 0–2 | `Mackanized/op2j2534` | ignored (§4) |
| Three channels maximum | same | 5, 6, 7 and 9 open together (§5) |
| ISO14230 opens at 10400 only, `are 25` otherwise | same | no baud rate is validated (§5) |
| `atf` args are `<type> <mask_len> <pattern_len>` | same | the third field is TxFlags: the vendor sends `atf6 3 64 4` |
| Letter channel bytes `C`, `D`, `S` for the L line and the jack | `emdzej/j2534` | `are 7`; the DLL sends `ato7`, `ato8`, `ato9` (§5) |
| Only four configuration parameters are supported | this project's first sweep | two dozen, per protocol (§8.1) |
| Pin 12 is the programming-voltage sense | this project | it is OEM12, the pin `atv 12` drives |
| `ata` is "attention" | this project | it closes every channel and releases every output |
| A TxDone indication carries no data | this project, citing J2534-1 JAN2022 | the 04.04 text and the vendor DLL both carry the CAN id (§7.5) |

Confirmed third-party claims: `ata` closes all channels, `atp` shares `atv`'s
template and readable pins are 8, 12, 16, 17 (all `Mackanized/op2j2534`).

---

## 14. Sources

| Source | What it settles |
|---|---|
| SAE J2534-1 DEC2004 (04.04) §7.2.5, §8.5–8.7, A.2–A.4 | what an application must be shown for indications and segmented receives; the `RxStatus` bit table; message size limits |
| Vehicle sessions: 2026-06-17 (chassis controller, ISO15765), 2026-09-13 (2012 VW Caddy; 2009 Audi 1.9 TDI), 2026-09-16 and 2026-09-24 (bench chassis ECU) | the frame sequences, status bits, transmit indication, raw CAN, periodic timing, `SNIFF_MODE`, long-reply chunking, the ISO15765 loopback echo |
| `op20pt32.dll` 1.02.0.4868 under emulation and against the cable (`AB-OFFICIAL.md`) | every command the vendor sends, sequence numbers, `atm`, chunk layout, `tbi`, `READ_PROG_VOLTAGE`'s pin argument |
| Tactrix `openport2_setup_1024820.exe` → `samples/common/j2534_tactrix.h`, `samples/canlogger/canlogger.cpp`, `samples/klogger/klogger.cpp` | the vendor's header (private IOCTLs, J2534-2 ids, `SNIFF_MODE`, OEM error codes, pin numbering) and the vendor's own consumers discarding `START_OF_MESSAGE` messages |
| Tactrix EcuFlash changelog (firmware 1.41, 1.42, 1.44) | K-line loopback follows the flag; TX_FLAG_DONE messages are timestamped; CAN_29BIT_ID on read results |
| github.com/emdzej/j2534 @ `35aae08` (`packages/core/src/parser.ts`, `commands.ts`, `AGENTS.md`) | an independent reading of every status byte on CAN and K-line, from a Ghidra disassembly of the DLL |
| github.com/MCU-Innovations/opta-j2534-rs @ `9774569` (`j2534-core/src/devices/tactrix/`) | status byte and chunk stripping, verified on a Honda CBR1000RR ECM |
| github.com/Aiden-korbs/openport2-winarm-j2534 | the `arw` five-baud reply, HDS on a 2005 Honda CR-V |
| github.com/Mackanized/op2j2534 | command templates dumped from the unpacked vendor DLL; the claims above |
| github.com/firefighter-19/tuneforge, `dschultzca`/`NikolaKozina` `j2534` and its forks, `sw7ft/BerryCore` | the same lineage's kind table and K-line asymmetry |

## Reproducing this

`tools/probe/op_probe.c` dumps the descriptor tree. For protocol work, the CDC
node is easier than libusb:

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
