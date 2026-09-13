# The OpenPort wire protocol

How a host talks to a Tactrix OpenPort 2.0 over USB. Tactrix publishes no
protocol specification, no SDK and no source, so everything here was
established by observation.

**Device used:** OpenPort 2.0, firmware `1.17.4877`,
on macOS 26.6 (Apple Silicon), libusb 1.0.29.

Every claim carries a confidence marker:

| Marker | Meaning |
|---|---|
| **[V]** | Verified — observed directly on the device named above |
| **[P]** | Partial — the shape is confirmed, some field is not fully pinned down |
| **[U]** | Unverified — inferred, and flagged as needing a bus to confirm |

The device was on a bench throughout: powered by USB, **not connected to a
vehicle**. Everything requiring live bus traffic is therefore **[U]**, and
§10 lists exactly what remains open.

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

The device is a **standard CDC-ACM serial device**, not a vendor-specific one.
`bInterfaceProtocol 0x01` is literally "AT commands", which is why every
command begins `at`. This also means there are two ways to reach it:

1. **libusb on the bulk pair** — what this driver does.
2. **The CDC-ACM character device** — macOS enumerates `/dev/cu.usbmodem<serial>1`.
   No libusb, no claiming. Useful for scripting and for the protocol work that
   produced this document.

### Claiming the interface on macOS **[V]**

`AppleUSBCDCACM` binds **both** interfaces, and `libusb_kernel_driver_active`
reports 1 for each. Measured behaviour:

| Interface | `detach_kernel_driver` | `claim_interface` |
|---|---|---|
| 0 (comm) | `LIBUSB_ERROR_ACCESS` | `LIBUSB_ERROR_ACCESS` |
| 1 (data) | `LIBUSB_ERROR_ACCESS` | **success** |

So: **claim the interface carrying the bulk pair, never interface 0, and treat
a detach failure as expected rather than fatal.** No `sudo`, no kext removal,
no `nvram boot-args` surgery. A driver that claims interface 0, or that treats
the detach failure as an error, fails on macOS for no reason.

Find the interface by walking configurations, interfaces and altsettings for
one exposing both a bulk IN and a bulk OUT, rather than hardcoding 1 — that is
also what will let a future OpenPort 3.0 work unchanged.

---

## 2. Framing **[V]**

Host to device: an **ASCII line terminated `\r\n`**, optionally followed
immediately by raw binary payload bytes.

```
at<verb>[<channel>] <arg> <arg> ...\r\n   [payload bytes]
```

Device to host, two interleaved kinds on the same pipe:

```
ar<verb> <args>\r\n                                    ASCII reply
'a' 'r' <channel-digit> <len:u8> <payload...>          binary message frame
```

**They interleave.** A message frame can arrive between a command and its
reply. A driver that reads a reply inline, without demultiplexing, will lose
replies behind bus traffic.

**Telling them apart:** byte 2 is a *letter* for an ASCII reply
(`o e i r f g`) and an *ASCII digit* for a binary frame. That is the whole
discriminator, and it is unambiguous because the device accepts only
single-digit channels. The length byte spans the full 0–255 range and must
**not** be used to distinguish them.

An unrecognised command produces **no reply at all** — silence, not an error.

---

## 3. Reply verbs **[V]**

| Reply | Meaning |
|---|---|
| `aro` | success, no value |
| `are <code>` | failure; `<code>` is a **J2534 error number** (§6) |
| `are <code> <detail>` | failure, echoing the offending value |
| `ari <text>` | informational text |
| `arr <pin> <millivolts>` | pin voltage |
| `arf<ch> <filter_id> <n>` | filter installed |
| `arg<ch> <param> <value> <n>` | configuration value |
| `arw<ch> <b> <b> …` | five-baud init result: the ECU's keybytes **in decimal on the line**, then the echoed number **[P]** (§4) — third-party, measured with HDS on a 2005 Honda (`Aiden-korbs/openport2-winarm-j2534`, `j2534.c` and `extras/macos_openport_iso9141_mode09.c` both check for `arw`); on this bench with no ECU the answer is `are 7`. Until 2026-09-13 this driver expected `ary` here and would have reported a real five-baud success as `ERR_INIT_FAILED` |
| `ary<ch> <n>` + `n` raw bytes | fast init result: the StartCommunication response follows the line as raw bytes **[P]** (§4), the `dschultzca` lineage's reading — the failure reply `are 7` is measured, a success has not yet been captured |

The trailing `<n>` on `arf`/`arg` was 0 in every single-parameter observation
and echoed a following parameter id in a multi-parameter `atg` query. **[P]**
It is the echo of a **command sequence number** **[V]**: Tactrix's own DLL
appends a number to every command and accepts only a reply that carries it
back (`docs/AB-OFFICIAL.md`), and the cable echoes it on every text reply
(measured 2026-09-13): `ata 2` → `aro 2`, `atr 16 3` → `arr 16 108 3`,
`atf6 3 64 4 8` → `arf6 0 8`, `atg6 30 10` → `arg6 30 0 10`,
`atg6 127 9` → `are 1 9`, `atn6 5 14` → `are 13 14`. Without a number the
field reads 0 on `arf`/`arg` and is absent elsewhere.

| `arm<ch> <id> <n>` | periodic message installed (reply to `atm`, §4) |

---

## 4. Commands

Verified by sweeping `at<a..z>` and then each verb's argument shape. Silence
means the verb does not exist.

| Command | Args | Purpose | Conf. |
|---|---|---|---|
| `ata` | — | **close every open channel**; replies `aro`. Measured 2026-09-13: a filter installed on channel 6 answers `are 2` after `ata`, and `ato6` opens again. It differs from `atz` only in that it is answered by number (§3). Earlier revisions of this document called it "attention" | **[V]** |
| `ati` | — | version: `ari main code version : 1.17.4877` | **[V]** |
| `atz` | — | reset; replies `aro` | **[V]** |
| `ato<proto>` | `<flags> <baud> 0` | open a channel. The vendor DLL puts a varying value in the last field when opening ISO9141 (3 on the bench, 5 on a car, each time the handle of the ISO14230 channel it had just closed), which is its own bookkeeping; the firmware accepts 0. **The field is ignored** (measured 2026-09-13): channels 5, 6 and 3 opened with 0 in it simultaneously, `ato5 … 7` was accepted, and a second `ato5` gets `are 20` whatever value it carries — so it is not a channel slot, as one third-party driver reads it | **[V]** |
| `atc<ch>` | — | close a channel | **[V]** |
| `att<ch>` | `<len> <txflags> <timeout_us>` + payload | transmit; `<timeout_us>` is the firmware's budget for getting the message on the bus, the DLL sends the caller's `WriteMsgs` Timeout in microseconds (1 000 000 for a Timeout of 0); without it the firmware gives up after ~1 s | **[V]** |
| `atf<ch>` | `<type> <txflags> <len>` + payload | install filter | **[V]** |
| `atk<ch>` | `<filter_id>` | remove filter | **[V]** |
| `atg<ch>` | `<param>` | read configuration | **[V]** |
| `ats<ch>` | `<param> <value>` | write configuration | **[V]** |
| `atr` | ` <pin>` | read a pin voltage | **[V]** |
| `atv` | ` <pin> <millivolts>` | **programming voltage** | **[V]** |
| `atl<ch>` | — | answers `aro`; what it clears is unmeasured. The vendor DLL never sends it (`CLEAR_RX_BUFFER` is host-side there) and this driver no longer does | **[P]** |
| `atp` | ` <pin> <value>` | a **pin verb**, sharing `atv`'s argument shape (the vendor DLL's format-string table has one template, `at%c %d %d %u`, for both `p` and `v`). Takes ~0.5 s to answer; every `(pin, value)` tried on 2026-09-13 — pins 0, 1, 6, 12, 15; values 1, 5000, 20000, `SHORT_TO_GROUND`, `VOLTAGE_OFF` — answered `are 10`, value 0 `are 5`. Function unknown; the DLL is not seen sending it. Earlier revisions read it as "start periodic message" and swept an interval: that shape was simply wrong | **[V]** rejected |
| `atn<ch>` | `<msg_id>` | stop periodic message | **[P]** |
| `atm<ch>` | `<interval_us> 0 <txflags> <len> <seq>` + payload | **start periodic message**, what the vendor DLL sends; replies `arm<ch> <id> <seq>`, ids from 0; `atn<ch> <id>` stops it, `are 13` for an unknown id | **[V]** |
| `aty<ch>` | `<len> 0` + request bytes | K-line **fast** init | **[V]** — measured: returns in ~108 ms, a 25/25 ms wake pulse |
| `atw<ch>` | `<address>` (decimal, no payload) | K-line **five-baud** init; `atw3 51` for 0x33, as the vendor DLL sends it. The earlier reading `<len>` + address byte was wrong: the firmware took the 1 as the address and the byte as the start of the next command, which is why every five-baud init this project sent went to ECU 0x01 | **[V]** — blocks ~2450 ms, one byte clocked out at 5 baud |
| `atx` | ? | exists, consumes a payload, purpose unknown | **[U]** |

`atb atd ate ath atj atq atu` do not exist (silence).

**Note the spacing.** `atr` and `atv` take a *space* before their first
argument; the channel-scoped verbs fuse the digit to the verb. `ato6 …`
opens ISO15765; `ato 6 …` is parsed differently and fails.

---

### What Tactrix's own DLL sends **[V]** (under emulation)

Measured by running `op20pt32.dll` 1.02.0.4868 itself against the simulator
and a byte tap (`docs/AB-OFFICIAL.md`; `tools/ab-official/`), which replaces
the earlier second-hand account from a Ghidra disassembly:

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
ato4 0 10400 0 3 / aty4 4 0 4 + 4  ISO14230 connect, fast init (same as this driver)
ato3 0 10400 3 7 / atw3 51 8       ISO9141 connect (trailing value varies, see §4 table), five-baud init to 0x33
```

The number is how the DLL matches replies: it waits for `aro <seq>` (500 ms,
5 s for `atr`/`atg`) and a reply without it is ignored; the cable echoes it
on every reply (§3, measured). An `att` sent **without** a number gets no
reply at all, not even `are 9` on a bus with no ACK peer, and the firmware
answers nothing else for about a second while it keeps trying (measured on
the bench); the numbered form on the same bus answers `are 9 <seq>` after
1.0 s. The fourth `att` argument is the caller's `WriteMsgs` Timeout in
microseconds (a replay of a reflash tool, whose driver-window transfer
uses 5000 ms, showed `5000000`), with 1 000 000 substituted for a Timeout of
0; it is the firmware's own budget for getting the message onto the bus.
Since 2026-09-13 this driver sends it the same way. `ati` ignores a number (`ati 5` answers a bare `ari`); `atl`,
`aty` and `atw` echo it (`atl6 7` → `aro 7`, `aty3 4 0 10` → `are 7 10`,
`atw3 1 11` → `are 7 11`; the success reply `ary` was not observed with
one). This driver numbers every command but `ati` since 2026-09-13 and
accepts a reply only when it echoes the number, an init reply with or
without. `PassThruReadVersion` is answered from the cached `ati` reply. When
`ati` gets no answer the DLL sends `tbi\n` (LF only) three times at 300 ms
and gives up; the application firmware does not answer it with either
terminator (measured), so it is a bootloader query. `atv 12 -1` is accepted
(`aro`, measured).

## 5. Channels **[V]**

**The channel id is the protocol id.** Proven by:

```
ato6 0 500000 0   -> aro          open ISO15765
ato6 0 500000 0   -> are 20       ERR_CHANNEL_IN_USE
atc6              -> aro          close it
ato6 0 500000 0   -> aro          opens again
```

Closing a channel that was never open still replies `aro`; the device does not
validate the digit on `atc`.

**Protocols sharing a pin group are mutually exclusive, and the refusal is
`are 3`, not `are 20`** (measured 2026-09-13):

```
ato3 0 10400 0    -> aro          ISO9141 open
ato4 0 10400 0    -> are 3        ISO14230 refused while ISO9141 holds the K line
atc3 / ato4       -> aro          and the other way round: ato3 is then are 3
ato5, ato6, ato7  -> aro aro aro
ato8 0 7812 0     -> are 3        SCI A trans refused while SCI A engine is open
ato9 0 7812 0     -> aro          four channels open at once: 5, 6, 7, 9
```

So there is **no three-channel cap** — a third-party driver reports one, but
its fourth open was a second `ato5`, which is `are 20` for being a duplicate,
not for being the fourth. The pairs are ISO9141/ISO14230 (K line) and SCI A
engine/trans.

**Baud rate is not validated on open.** `ato5 0 123456 0`, `ato3 0 4800 0`,
`ato4 0 9600 0` and `ato6 0 0 0` all answer `aro`. No `ERR_INVALID_BAUDRATE`
(25) was ever produced, and ISO14230 is not "10400 only" as one third-party
document states; what a wrong rate does on the bus is unmeasured.

Protocols accepted, by sweeping `ato0..ato12`:

| ID | Protocol | Accepted |
|---|---|---|
| 1, 2 | J1850 VPW / PWM | **no** — `are 3` |
| 3 | ISO9141 | yes |
| 4 | ISO14230 (KWP2000) | yes |
| 5 | CAN | yes |
| 6 | ISO15765 | yes |
| 7, 8, 9 | SCI A engine / A trans / B engine | yes |
| 10+ | — | `are 3` **[P]** — the two-digit parse is unconfirmed |

**ISO-TP segmentation and flow control are performed by the device firmware.**
The host sends a whole service request and the firmware handles first frame,
consecutive frames, flow-control frames on receive, and the ECU's requested
block size and separation time on transmit. Do not implement ISO-TP in the
host.

The host's only role in flow control is to pass the parameters through, and
they are per-protocol (§8). For ISO15765 the relevant ones are `ISO15765_BS`
(30) and `ISO15765_STMIN` (31) for the flow control **we** send, `BS_TX` (34)
and `STMIN_TX` (35) for what we request of the ECU, and `ISO15765_WFT_MAX`
(37) for how many WAIT frames to tolerate. All five are accepted by the
firmware and forwarded unfiltered by this driver.

What the host **does** have to pace is the USB link above ISO-TP: one command
in flight at a time, a timed-out command's late reply discarded rather than
handed to the next command (§9.2), a caller's `Timeout` spent across a whole
`WriteMsgs` call rather than per message, and a receive queue whose overruns
are reported rather than silently dropped.

---

## 6. Error codes are J2534 codes **[V]**

This is the single most useful discovery for anyone writing a driver: `are <n>`
carries a **literal SAE J2534-1 return code**, so the mapping is the identity
function. Confirmed by provoking each one:

| Provocation | Reply | J2534 |
|---|---|---|
| `atg6 0` (unsupported parameter) | `are 1` | `ERR_NOT_SUPPORTED` |
| `ato1 …` (J1850) | `are 3` | `ERR_INVALID_PROTOCOL_ID` |
| `atp 12 0` (pin verb, zero value) | `are 5` | `ERR_INVALID_IOCTL_VALUE` |
| malformed command | `are 7` | `ERR_FAILED` |
| transmit with no bus | `are 9` | `ERR_TIMEOUT` |
| `atf6 3 0 12` (wrong payload length) | `are 10` | `ERR_INVALID_MSG` |
| `atn5 0` (no such periodic message) | `are 13` | `ERR_INVALID_MSG_ID` |
| `atr 0` (not a readable pin) | `are 19` | `ERR_PIN_INVALID` |
| second `ato` on one protocol | `are 20` | `ERR_CHANNEL_IN_USE` |
| `atk6 1` (no such filter) | `are 22` | `ERR_INVALID_FILTER_ID` |

Pass them through. Inventing a mapping loses information the device already
gave you correctly.

---

## 7. Binary message frames **[V]** (layout) / **[U]** (K-line variant)

```
  offset  size  field
       0     2  'a' 'r'
       2     1  channel, as an ASCII digit ('3'..'9')
       3     1  length of everything that follows
       4     1  status (bitfield, below)
       5     4  timestamp, microseconds, big-endian
       9   n-5  payload
```

Frame stride is `4 + len`. A real capture:

```
61 72 36 | 09 | 10 | 19 50 b8 0c | 00 00 07 e0
'a''r''6'  len  sts   timestamp     CAN id 0x7E0
```

The timestamp is **microseconds since power-on**: two frames captured 1.2 s
apart differed by 1 238 083. It wraps every ~71 minutes.

For CAN and ISO15765 the payload begins with the **4-byte big-endian CAN
identifier**, matching J2534's `Data[0..3]` convention, followed by the
service data.

### The status byte is a bitfield **[V]**

| Bit | Meaning |
|---|---|
| `0x80` | START — announces a message; see below for what the frame carries |
| `0x40` | END — the frame completes a message |
| `0x20` | LOOPBACK — this is our own transmit echoed back |
| `0x10` | transmit indication — see below |

### What the bits mean on a live bus **[V]** (receive) / **[P]** (echo)

The obvious reading — START carries the first chunk of data, END the last —
is **wrong**, and it was wrong in this driver until it was checked against a
vehicle recording. Measured on a vehicle's chassis controller over ISO15765
at 500 kbit on 2026-06-17 (captured through the prior driver, whose per-frame
handling preserved the shape):

| Sequence on the wire | What arrived | Meaning |
|---|---|---|
| one `0x40` frame: `id` + `7f 3e 12` | a 3-byte negative response | **a message that fit one CAN frame is a single END frame; nothing precedes it** |
| `0x80` frame: `id` only, then `0x40` frame: `id` + 22 bytes | a 22-byte `$1A 87` response | **START carries the CAN id and nothing else.** It announces that a segmented message has begun. The data follows later in END-terminated frames that carry the id again |

Observed for every segmented response in that session (`$1A 87`, `$1A 9A`,
`$1A 9C`, `$21 E4`) and for every single-frame one. No `0xC0` (START|END)
frame was seen on receive; the driver still accepts one as a complete message
because the bits imply it, but it is unobserved.

So a driver must treat a START-without-END frame as an **indication** — in
J2534 terms `ISO15765_FIRST_FRAME`, `DataSize` 4 — and must **not** fold it
into the data. Folding it in, as this driver first did, yields a message with
the CAN id twice and `START_OF_MESSAGE` set, which every J2534 consumer
discards as a first-frame marker. `tests/unit/test_golden.c` replays the
recorded session so this cannot silently regress.

**Independently confirmed.** Two other drivers reached the same reading from
their own hardware, and the standard says why the shape exists:

- SAE J2534-1 (JAN2022) §7.2.6 Table 13 and A.1.5.2: on an ISO 15765
  channel the interface *shall* generate a Start indication "after receiving
  the first frame of a segmented message", with `START_OF_MESSAGE` set,
  `<DataLength>` four and `<DataBuffer>` holding "the network address
  information"; the assembled message is queued separately when reception
  completes, and "indications … shall bypass the filtering mechanism". The
  device's `0x80` id-only frame is that indication on the wire, and the
  Tactrix DLL surfaces it exactly so — which is why consumers written against
  the DLL discard messages carrying that bit.
- `MCU-Innovations/opta-j2534-rs` (`j2534-core/src/devices/tactrix/`,
  commit `9774569`), verified on an Openport 2.0 against a Honda CBR1000RR
  ECM over ISO 15765 including a 2 MB flash write: `0x80` is
  "start-of-message marker (… multi-frame ISO-TP on CAN)", `0x40` "carries …
  payload on CAN", and on ISO 15765 its assembly strips the first four bytes
  of every frame after the first — "On CAN the payload is `<CAN_ID 4B><UDS
  bytes>` (single-frame) or the final ISO-TP fragment prefixed with CAN_ID
  (multi-frame)". Same bytes out as this driver; it folds the announcement
  into the message instead of surfacing it as an indication.
- `emdzej/j2534` (`packages/core/src/parser.ts`, firmware 1.17.4877) maps the
  bytes to J2534 exactly as this driver now does: `0x80` becomes a message
  with `START_OF_MESSAGE` set and the CAN id as its data, `0x40` becomes the
  data message with `RxStatus` 0, `0x10` a `TX_MSG_TYPE` indication, `0x44`
  an end frame for extended addressing.
- `firefighter-19/tuneforge` (`crates/tuneforge-io/src/tactrix/protocol.rs`)
  and the `dschultzca` C driver this project measured against: the same kind
  table (`0x00` data, `0x10` TxDone, `0x20`/`0xA0`/`0x60` loopback, `0x40`
  end, `0x80` start).

**How independent these sources actually are.** Less than this document
previously implied, and it matters for anything resting on them.
`opta-j2534-rs` says in its own header that it is ported from `tuneforge`,
which in turn derived from `dschultzca`; the MQBau driver measured against in
`DIFFERENTIAL.md` is a fork of `dschultzca`/`NikolaKozina`. So three of the
four are one lineage with a single root. Only `emdzej/j2534`, written from a
Ghidra reading of Tactrix's own DLL, is genuinely independent of it. Where this
document says several sources agree, read that as **two** lines of evidence,
not four — and the measurement above shows that lineage is capable of carrying
an error the whole way down.
- Tactrix's own changelog for firmware 1.41: "fix ISO K and L lines to do
  proper J2534 LOOPBACK functionality — you will no longer get echoes of your
  commands unless LOOPBACK=1", which is why the June log shows no transmit
  echo; and "fix bug where ISO15765 TX_FLAG_DONE messages had incorrect
  timestamp", confirming transmit-done indications exist as timestamped
  frames.

**Vendor code agrees.** Tactrix's own sample programs, shipped inside the
official driver installer (`openport2_setup_1024820.exe`, `samples/canlogger/
canlogger.cpp` and `samples/klogger/klogger.cpp`), read messages from
their DLL with:

```c
void dump_msg(PASSTHRU_MSG* msg)
{
	if (msg->RxStatus & START_OF_MESSAGE)
		return; // skip
	...
```

The vendor's consumers expect their DLL to hand back the announcement as a
separate message carrying `START_OF_MESSAGE`, on both CAN and K-line, and
throw it away. That is the behaviour this driver now has, and the strongest
authority available short of the firmware source. The official DLL itself is
packed with a commercial protector, so its decompilation is pending a dump
from a running process (`refs/tactrix-dll/README.md`).

### The low nibble of the status byte **[V]** — and a claim it refutes

Two third-party drivers document the low nibble as J2534 `RxStatus` bits, with
`0x02` meaning `START_OF_MESSAGE`. **That is wrong on this firmware.** Measured
on a 2012 VW Caddy on 2026-09-13, transmitting the
same request four ways:

| TxFlags | Status byte |
|---|---|
| `0x000` | `0x10` |
| `0x040` `ISO15765_FRAME_PAD` | `0x10` |
| `0x100` `CAN_29BIT_ID` | **`0x12`** |
| `0x140` both | **`0x12`** |

`0x02` tracks the 29-bit identifier exactly and nothing else. Reading it as
`START_OF_MESSAGE` would make `0x12` mean "transmit done, and also the start of
a message", which is not a thing. The driver now carries it through as J2534's
`CAN_29BIT_ID`, which it had never set before.

`0x44` (end with `RX_BREAK`) is still only a third-party claim, unmeasured here.

How a payload longer than one wire frame (250 bytes) is chunked is **[P]**,
now with the vendor on one side: Tactrix's own DLL, fed a 600-byte reply with
the CAN id only in the first chunk, delivers 598 bytes; fed the id at the
start of every continuation chunk it delivers all 606 intact
(`docs/AB-OFFICIAL.md`). `opta-j2534-rs`, which strips an id from every frame
after the first, agrees, and was verified on a 2 MB flash read. The simulator
now defaults to `id_every_chunk`; this driver reassembles correctly under
either. No capture of a receive longer than one wire frame exists yet; a
`$23` read of 0xFF bytes on an ECU that answers it would make this **[V]**.

**`0x10` is not rare, and misreading it produces phantom messages.** **[V]**
(An earlier revision said no prior driver handled it. That was wrong: the
`dschultzca`/`NikolaKozina` C driver has a `TX_DONE` case for `0x10` and
reports it as `RxStatus` 8, `TX_INDICATION`; what it lacks is the `0x12`
variant below.) On a
vehicle (2026-09-13) *every* successful transmit produced one: a frame carrying
the 4-byte CAN id and no data, immediately after the `aro`. It is the device's
transmit indication, and a driver that mistakes it for received data will report
a phantom message on every write. With `TxFlags` 0x100 the status came back as
`0x12` rather than `0x10`; the extra bit matches the report that this firmware
ORs J2534 `RxStatus` bits into the low nibble of the status byte, so mask the
high bits before classifying a frame. Read `0x10` as `TX_MSG_TYPE | TX_DONE`.

### Seeing your own transmit is not a loopback **[V]**

On a raw CAN channel with a pass-all filter, the frame you just sent comes back
to you. It is tempting to read that as the device echoing with `LOOPBACK` off,
which would be a conformance defect. It is not. Measured on the Caddy: with a
filter that excludes the transmitted id, the frame does **not** come back. It
obeys the receive filter, so it is ordinary bus traffic — a CAN node sees its
own transmission on the wire, and a promiscuous filter shows it to you.

Turning `LOOPBACK` on adds a *second*, separate frame marked `0x20`, whose
payload is four zero bytes rather than the CAN id. That also explains the
anomalous `0x20` frame recorded during the Audi session and left unexplained
there.

The consequence for a caller is real even though the device is behaving: on raw
CAN with a promiscuous filter, your own frames arrive indistinguishable from an
ECU's. Filter by identifier, as the consuming scripts do.

**Transmit echo** on ISO15765 has only been seen on the bench, never on a
bus. It is assumed to mirror receive: a `0xA0` announcement with the id, then
`0x60` frames with the data, all flagged `TX_MSG_TYPE`. **[P]**

### The K-line frame layout **[P]** — sourced, not yet measured here

For channels `'3'` and `'4'` (ISO9141, ISO14230) the layout is **not** the
uniform one above. Every independent implementation that has run K-line on
this cable describes the same asymmetry, and this driver now follows it:

| Frame | Body after the status byte |
|---|---|
| `0x00` data, `0x20` loopback data | the K-line bytes themselves — **no timestamp** |
| `0x80` start, `0xA0` loopback start | the 4-byte timestamp, no data |
| `0x40` end, `0x60` loopback end | the 4-byte timestamp, no data |
| `0x10` transmit indication | the 4-byte timestamp |

Sources, in order of authority:

- `emdzej/j2534` (`packages/core/src/parser.ts`, firmware `1.17.4877` — the
  same firmware as this project's cable), whose transmit encoder cites a
  Ghidra disassembly of Tactrix's own `op20pt32.dll`; its `AGENTS.md` states
  it as a rule: "K-line packets (`NORM_MSG`/`TX_LB_MSG`) have **no
  timestamp**".
- `MCU-Innovations/opta-j2534-rs`, K-line verified on a Honda PGM-FI ECM:
  "the firmware does not insert a timestamp — payload begins immediately after
  kind"; "On K-line the RxEnd payload is a 4-byte Tactrix free-running
  timestamp, not user data".
- `firefighter-19/tuneforge`, same parser lineage, same fixture.
- The prior driver (the MQBau macOS port of `dschultzca/j2534`) assumed the same
  for `'3'`/`'4'`.

One of those projects also has a test fixture with an END frame carrying no
body at all; the parser accepts both shapes. This driver's earlier assumption —
a timestamp on every frame because that is what CAN does and what the single
`len` field implies — was exactly the kind of reading §7 warns about, and it
would have eaten the first four bytes of every K-line message.

Still **[P]** because no capture from this project's own hardware exists yet.
`tools/car/car_capture.py --kline` records every K-line frame's raw body and
`analyse_capture.py` tests both layouts against the recording: under this one
the counter must advance only on start/end frames and the data frames must
begin with a K-line header byte.

---

## 8. Configuration and pins **[V]**

`atg<ch> <param>` / `ats<ch> <param> <value>`, using J2534 parameter numbers.

**Support is per-protocol**, and the sets differ substantially. Swept 1..37
with both `atg` and `ats` on each channel:

| Protocol | Supported parameters |
|---|---|
| **6 ISO15765** | 1 DATA_RATE, 3 LOOPBACK, 23 BIT_SAMPLE_POINT, 24 SYNC_JUMP_WIDTH, 30 ISO15765_BS, 31 ISO15765_STMIN, 34 BS_TX, 35 STMIN_TX, 37 ISO15765_WFT_MAX |
| **4 ISO14230** | 1 DATA_RATE*, 3 LOOPBACK, 7 P1_MAX, 10 P3_MIN, 12 P4_MIN, 14–18 W1–W5, 19 TIDLE, 20 TINIL, 21 TWUP, 22 PARITY, 25 W0, 32 DATA_BITS, 33 FIVE_BAUD_MOD |

\* readable with `atg` on ISO14230 but **not** settable with `ats`; the GET and
SET sets are not identical.

Anything outside a protocol's set answers `are 1` (ERR_NOT_SUPPORTED).

An earlier revision of this document claimed only four parameters were
supported in total. That was a sampling error — the original sweep tried seven
values on one protocol and generalised. The driver forwards every parameter
unfiltered, so its behaviour was always correct; only this table was wrong.

`LOOPBACK` is accepted and reads back, but on a bench with no bus a transmit
still fails with `are 9` before any echo is produced — the echo follows a
*successful* bus transmit. It cannot be used to generate traffic without a
second CAN node. **[V]**

`atr <pin>` — sweeping 0..20, **four** pins answer; all others give `are 19`:

| Pin | Reading (bench, no vehicle) | Interpretation |
|---|---|---|
| 8 | `0` | J1962 pin 8, an OEM-optional pin. Reads zero with nothing attached |
| 12 | `0` | programming-voltage sense, off |
| 16 | `130`–`152`, drifting | J1962 pin 16, battery. Floating with no vehicle; **12 156–12 199 mV on a vehicle (measured 2026-09-13), so the unit is millivolts** **[V]** |
| 17 | `5751`–`5794`, drifting | internal rail, ~5.8 V. Not a J1962 pin |

Units are millivolts, matching J2534's `READ_VBATT`. Pin 16 with a vehicle
attached should read ~12 000. **[U]** — not confirmed without a car.

### Programming voltage **[V]**

`atv <pin> <millivolts>`.

```
atv 12 4294967295  -> aro        0xFFFFFFFF is J2534's VOLTAGE_OFF
atv 12 0           -> are 120    zero is rejected; use VOLTAGE_OFF
```

Only the "off" path was exercised. **Applying voltage was deliberately not
tested** — it energises a pin on the vehicle connector. This driver gates it
behind `OPENPORT_ENABLE_PROG_VOLTAGE=1`; turning it off is always allowed.

---

## 9. Two hazards a driver must handle

### 9.1 A bad payload desynchronises the parser **[V]**

Send `atf6 3 0 12` with 12 bytes when the device expects `4`, and it consumes
bytes as payload that you meant as the next command. Subsequent commands vanish
into silence. Recovery: send newlines to terminate the partial payload, then
`atz`, then confirm `ata` answers `aro`.

### 9.2 Replies are positional unless numbered, and a stale one corrupts everything after **[V]**

*Update 2026-09-13:* the sequence-number echo (§3) is the device's own
answer to this hazard. A driver that numbers its commands and matches the
echo cannot take a stale reply for a current one, whatever is left in the
pipe; this driver does so now, and its open no longer needs to drain and
settle the link (≈90 ms), only to reset and be answered by number.

Replies carry no tag identifying the command they answer. If the pipe holds one
stale reply, **every subsequent command receives the previous command's
answer** — a silent wrong result, not a visible failure.

Two ways this happens, both observed:

- **A previous session left replies queued.** A backlog seven deep was observed
  after an interrupted session; the driver then read the firmware version as
  the answer to `atr 16`, and so on down the line.
- **A command timed out and the device answered later.** A CAN transmit with no
  bus takes ~1.2 s to be rejected with `are 9`. A caller that gave up after
  200 ms leaves that `are 9` to be collected by the *next* command.

The fixes are structural, not heuristic:

1. **Drain and synchronise at open**, before anything depends on ordering:
   read until idle, `atz`, read until idle, then require `ata` to answer
   exactly `aro`.
2. **Only a command that is still waiting may claim a reply.** A reply arriving
   with no waiter is discarded as an orphan.

A "send `ata` twice and hope" workaround only clears a backlog exactly one
reply deep, and silently fails at depth two or more.

---

### Raw CAN frames carry no START or END **[V]**

Measured on a 2012 VW Caddy on 2026-09-13: on a raw CAN
channel every frame arrives with a status byte of `0x00`, our own echo and the
ECU's reply alike. Raw CAN has no transport layer, so one wire frame is one
whole message and the firmware never marks START or END.

A driver that waits for the END bit before delivering — as this one did — will
swallow every raw CAN frame and return `ERR_BUFFER_EMPTY` on a bus that is
answering. `caddy_raw_can_replay` in `tests/unit/test_golden.c` is the
regression. `opta-j2534-rs` reaches the same conclusion independently, handing
each raw CAN frame to the application as its own message.

## 10. What is still open

Updated after the vehicle session of 2026-09-13 (a 2009 Audi 1.9 TDI, EDC16).
What that session could and could not settle is itself informative: the car
answered no diagnostic request on either bus, so everything about *receiving*
remains as it was.

**Settled by measurement.**

| Question | Answer |
|---|---|
| Pin 16 with a vehicle attached | 12 156–12 199 mV against a ~12.2 V battery: `READ_VBATT` is millivolts (§8 drops its **[U]**) |
| Which pins answer `atr` | 8, 12, 16 and 17 (§8); every other pin 0–20 gives `are 19`, the same as on the bench. An earlier revision of this row omitted pin 8; re-measured 2026-09-13 |
| The `0x10` status | the normal transmit indication on every transmit (§7) |
| `atp` interval encoding | there is none: `atp` is a pin verb with `atv`'s shape (§4), and every interval this project swept was being parsed as a value. The vendor DLL's periodic command is `atm` (§4), which works |
| `atm`, `atw`, `atx`, `aty` | `atw` is five-baud init and `aty` is fast init (§4). `atx<ch> <n>` echoes its argument back in the error (`are 7 1`); `atm<ch>` fails immediately. Both remain unidentified |
| The DLL's five-argument forms | accepted. `ato6 0 500000 0 1` answered **`aro 1`** — the device echoes the trailing sequence number back in its acknowledgement, which is what that argument is for. The five-argument `att` was accepted too |
| Two-digit protocol numbers | `ato10`, `ato11`, `ato1`, `ato0` all give `are 3` |
| Letter channel bytes (`atoC`, `atoD`, `atoS` for the J2534-2 L-line and AUX protocols that `emdzej/j2534` reads out of the DLL) | `are 7` on firmware 1.17.4877 for all three, and `atcC` too. Not this firmware's dialect, or not at this firmware level |

**Still open, and why this car could not close them.**

1. ~~Received-message framing with real data~~ — **CONFIRMED on hardware
   2026-09-13**, on a 2012 VW Caddy. Legislated OBD (`$01`), UDS `$22 F1 90`
   (VIN) and `$09 02` all answered on ISO15765, and every multi-frame reply
   arrived in exactly the shape the September correction implemented: a `0x80`
   frame carrying the CAN id alone, then a `0x40` frame with the id repeated and
   the data. Through the driver's own entry points the VIN came out as a
   `ISO15765_FIRST_FRAME` indication (4 bytes) followed by one clean 24-byte
   message carrying the VIN. `tests/unit/test_golden.c` carries this trace
   (`caddy_vin_replay`, with a documentation VIN in place of the vehicle's). The prior
   driver would have merged the two frames and a consumer would have dropped
   the reply.

   The catch that hid it until now is not an ECU quirk but a standard this
   project was violating. **ISO 15765-4:2005 clause 8.1** requires the DLC of
   every diagnostic CAN frame to be eight, states that the unused bytes are
   undefined, and says a diagnostic CAN frame with a DLC below eight *shall be
   ignored by the receiving entity*. The OpenPort pads only when the caller
   sets `ISO15765_FRAME_PAD`; without it the frame goes out short and a
   conforming ECU is required to drop it. J2534 (JAN2022) pairs that flag with
   `ISO15765_PAD_VALUE` (config id 0x2B, default 0x00) for the byte to pad
   with. The Caddy was conforming; we were not.

   This is now enforced rather than remembered: the simulator's ISO15765 ECU
   ignores an unpadded request exactly as the standard requires, `s_frame_pad`
   proves it does, and `s_capture_tool_pads` is the regression for the tool
   that had it wrong. The driver logs the non-conformance when it sees one but
   does not rewrite `TxFlags`, which J2534 reserves to the application.

   Historical note: this ECU **ignores an unpadded request**.
   The reply only appears when the request and the flow-control filter carry
   the `ISO15765_FRAME_PAD` TxFlag (0x40). The capture tool had been
   transmitting unpadded, which is why the Audi and the first Caddy attempt saw
   silence. A driver leaves padding to the caller (via TxFlags), but a tool or
   consumer that wants an answer from a real ECU must set it.
2. **The K-line frame layout** (§7) — no K-line frames were captured. Every
   standard wake-up, five-baud and fast, at the EOBD address 0x33 and the VAG
   addresses 0x01 and 0x10, returned `are 7`. On a 2009 VAG the engine ECU is
   reached over CAN, so pin 7 most likely has no engine responder to find; this
   needs an older or non-VAG K-line car.
3. **Chunking of a reply longer than one wire frame** — still unmeasured on
   the cable, but no longer able to corrupt data, and the vendor's own DLL
   has now voted: it reassembles a 600-byte reply intact only when every
   chunk repeats the 4-byte CAN id (`docs/AB-OFFICIAL.md`). The measured
   Caddy reply cannot discriminate, because it had a single data frame and
   both readings predict exactly what was seen. `opta-j2534-rs`, which strips
   four bytes from every frame after the first and was verified on hardware
   doing a 2 MB flash read, agrees with the DLL.

   Rather than bet on either, `absorb_frame()` uses what the message already
   carries: the id is the first four bytes of the accumulated message, so a
   continuation frame beginning with those same four bytes is repeating it and
   they are dropped. Correct under the repeating model; a no-op under the
   other unless the payload coincidentally equals the id exactly on a frame
   boundary, which is logged. `s_chunking_models` asserts both models now
   reassemble identically. A real long-reply capture is still worth having,
   but it no longer decides whether the data is right.

   Practically, a read tool using 128-byte chunks produces replies of about
   133 bytes, which never reach one wire frame; only a 0xFF-byte chunk would
   cross 250.
5. ~~**What the vendor DLL's dialect gets back from the firmware**~~ —
   **settled on the bench 2026-09-13**: the sequence number is echoed on every
   reply (§3); `atm` is the working periodic command and answers
   `arm<ch> <id> <seq>` (§4), so the firmware's periodic facility is usable
   after all and host-side scheduling is a choice, not a necessity; `tbi` is
   silent; `atv 12 -1` is accepted.
4. **The transmit echo shape** with `LOOPBACK=1` — the one frame that arrived
   was anomalous: status `0x20` on channel digit `5` while the transmit was on
   channel 6, carrying four zero bytes where a CAN id was expected. Recorded
   and deliberately not built on. **[U]**

**Not a question about this cable at all.** A VAG module is not addressed with
ISO15765. It is reached with VW TP2.0, a transport of its own on raw CAN: a
channel is opened on id 0x200 and the module names the pair of ids to use.
That is why every ISO15765 request in this session was ignored while the bus
itself was live and acknowledging our frames. `tools/car/car_capture.py --tp20`
implements it over this driver's raw CAN channel; the protocol is documented at
`jazdw.net/tp20` and the implementation is covered by `s_tp20` in the simulator
scenarios.

## Legislated OBD-II services, measured on a vehicle **[V]**

The emissions-related services are defined by SAE J1979 / ISO 15031-5. Every
read among them was exercised on a 2012 VW Caddy on 2026-09-13, through the
driver's own entry points.

| Service | What it reads | On this vehicle |
|---|---|---|
| `$01` | current powertrain data | answered; PID bitmaps 00, 20, 40 present, 60 absent (end of chain) |
| `$02` | freeze-frame data | answered |
| `$03` | stored emissions DTCs | answered, none stored |
| `$05` | oxygen sensor test results | not applicable — superseded by `$06` on CAN |
| `$06` | on-board monitoring test results | answered |
| `$07` | pending DTCs | answered, none pending |
| `$09` | vehicle information | answered: 00, 02 VIN, 04 calibration id, 06 CVN, 0A ECU name |
| `$0A` | permanent DTCs | silent — not supported by this vehicle |

`$04` (clear DTCs) and `$08` (on-board system control) are the two that write
or actuate. They are refused by name by the capture tool and were never sent.

An unsupported mode or PID draws **no reply at all** rather than a negative
response, so silence is a conformant answer and not evidence of a fault. Both
silences above are that.

**A trap worth recording.** A functional request (`0x7DF`) whose reply is
segmented needs its flow control addressed to the ECU's *physical* id, not to
`0x7DF`. Flow control is a point-to-point answer to one ECU; broadcast to
`0x7DF` it never arrives, and every multi-frame reply stalls after the first
frame with no error. Half this sweep failed that way before the flow-control id
was separated from the request id in `tools/car/driver_multiframe.py`. The
single-frame services were unaffected, which is exactly what makes it
misleading — it looks like the vehicle supports some services and not others.

## Sources consulted for §7

| Source | What it settles | Authority |
|---|---|---|
| SAE J2534-1_0500 JAN2022, §7.2.6 Table 13, §9.1 Table 72, A.1.5.2 | what an application must be shown for a segmented ISO 15765 receive; the `RxStatus` bit table | the standard |
| Vehicle session of 2026-06-17 (ISO15765, 500 kbit, through the prior driver) | the device's actual frame sequence on a vehicle | measurement |
| github.com/MCU-Innovations/opta-j2534-rs @ `9774569`, `j2534-core/src/devices/tactrix/{README.md,protocol.rs,mod.rs}` | independent reading of the status byte, verified on different hardware | third party, hardware-verified |
| github.com/firefighter-19/tuneforge, `crates/tuneforge-io/src/tactrix/protocol.rs` | same kind table, derived from the `dschultzca` C driver | third party |
| Tactrix `openport2_setup_1024820.exe` → `samples/common/j2534_tactrix.h`, `samples/canlogger/canlogger.cpp`, `samples/klogger/klogger.cpp` | the vendor's own header (private IOCTLs at 0x70000, J2534-2 channel ids for the L-line and the 2.5 mm jack, `SNIFF_MODE`, OEM error codes 0x77/0x78 for programming voltage, `CAN_MIXED_FORMAT`) and the vendor's own consumers discarding `START_OF_MESSAGE` messages | **vendor** |
| github.com/emdzej/j2534 @ `35aae08`, `packages/core/src/parser.ts`, `commands.ts`, `AGENTS.md` | independent reading of every status byte on CAN and K-line, on firmware 1.17.4877; the K-line no-timestamp rule; the DLL's extra `att`/`ato` arguments (from a Ghidra disassembly of `op20pt32.dll`) | third party, DLL-informed |
| Tactrix EcuFlash changelog (openecu.org `EcuFlash`, tactrix.com "EcuFlash 1.44") | firmware history: 1.41 "fix ISO K and L lines to do proper J2534 LOOPBACK functionality — no echoes unless LOOPBACK=1" and "fix bug where ISO15765 TX_FLAG_DONE messages had incorrect timestamp"; 1.42 "fix bug which can cause CAN receive buffer overruns during large ISO15765 transfers"; 1.44 "return CAN_29BIT_ID flag on appropriate read results" | vendor |
| github.com/sw7ft/BerryCore `ports/openport/src/openport.c` | a CAN logger that accepts only `0x00`/`0x20`/`0x40` frames and reads id+data from them, working on a vehicle | third party |
| The prior macOS driver (MQBau fork of `dschultzca`/`NikolaKozina`, `j2534.c`) | the prior driver's per-frame handling, which preserved the shape in the June log; same K-line asymmetry | prior implementation |

## Third-party claims tested on the bench, 2026-09-13

A sweep of GitHub for other OpenPort 2.0 wire-protocol implementations turned
up several the sources table above does not list. Each claim that differed
from this document was put to the cable rather than adopted or dismissed.

| Source | Claim | Bench result |
|---|---|---|
| `Mackanized/op2j2534` (Windows COM-port DLL, live-bus verified; command templates dumped from the unpacked vendor DLL) | `ato`'s fourth argument is a channel slot 0–2 and a fourth open fails | **refuted** — the field is ignored (§4), and the "fourth open" in their log was a duplicate `ato5` (§5) |
| same | three channels maximum | **refuted** — 5, 6, 7 and 9 open together; the real rule is pin-group exclusivity (§5) |
| same | ISO14230 opens at 10400 only; `are 25` for a bad rate | **refuted** — no baud rate is validated (§5) |
| same | `atf` args are `<type> <mask_len> <pattern_len>` | not adopted — the vendor DLL sends `atf6 3 64 4` for a flow-control filter (`docs/AB-OFFICIAL.md`), where 64 is `ISO15765_FRAME_PAD`, so the third field is TxFlags; their `4 4` is accepted because 4 is a harmless TxFlags value and 4 is the right length |
| same | `ata` closes all channels | **confirmed** (§4) |
| same | `atp` shares `atv`'s template | **confirmed** (§4) |
| same | readable pins 8, 12, 16, 17 | **confirmed** (§8, §10) |
| `Aiden-korbs/openport2-winarm-j2534` (`dschultzca` fork; HDS and EvoScan on a 2005 Honda CR-V over ISO9141) | five-baud answers `arw<ch> <b> <b>` in decimal | **not testable without an ECU** (`are 7` here); adopted as **[P]** because a working HDS session depends on those key bytes, and the driver accepts both shapes (§3) |
| `emdzej/j2534` (TypeScript, from a Ghidra reading of the vendor DLL) | channel bytes `C`, `D`, `S` for ISO9141_L, ISO14230_L and AUX | **refuted on this firmware** — `are 7` (§10) |
| same | five-baud is `aty<ch> 1 1` + address byte | not adopted — `aty<ch> 1 1` + byte answers `are 7` here exactly like `aty<ch> 1 0`, and the vendor DLL was captured sending `atw3 51` (§4) |
| `NikolaKozina/j2534`, `Natzirt-BK/subaru-ecu-tools-linux`, `jakka351/j2534` | — | the same `dschultzca` source; the second adds libusb auto-detach for Wine, the third is a Drewtech Mongoose project carrying a stale copy. Nothing new |
| `kylehulscher/atlas`, `colecrouter/ecu-explorer`, `sw7ft/BerryCore`, `Nedkelly80/tactrix-mac-driver` | — | same command set and frame layout as §2–§7; no claim this document lacked |

## Reproducing this

`tools/probe/op_probe.c` dumps the descriptor tree. For protocol work, the CDC
node is easier than libusb:

```bash
python3 - <<'PY'
import os, termios, time, select
fd = os.open("/dev/cu.usbmodemXXXXXXXX1", os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
a = termios.tcgetattr(fd); a[0] = a[1] = a[3] = 0
a[2] = termios.CREAD | termios.CLOCAL | termios.CS8
termios.tcsetattr(fd, termios.TCSANOW, a)
os.write(fd, b"ati\r\n"); time.sleep(0.3)
print(os.read(fd, 4096))
PY
```
