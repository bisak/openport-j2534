# A/B against the vendor's own driver

Tactrix ships one consumer of the cable's firmware: `op20pt32.dll`, a 32-bit
Windows library, packed with a commercial protector. It is the authority on
what the firmware accepts and how its replies are meant to be read, and until
now every claim about that in `PROTOCOL.md` came from sweeping the cable or
from third parties. This harness runs the DLL itself, on this Mac, against the
same simulator and the same cable as `libj2534.dylib`, drives both through one
scripted sequence, and diffs what they returned and what they put on the wire.

```bash
make ab-official                                   # both against the simulator
make ab-official-cable CABLE=/dev/cu.usbmodemXXXX  # both against the cable
make ab-official AB_ARGS="-- open bench --cycles 50"
```

Artifacts land in `tools/ab-official/out/`: `vendor.jsonl` / `ours.jsonl`
(one JSON line per call: arguments, return code, error text, outputs,
microseconds), `vendor.tap` / `ours.tap` (every byte both ways, timestamped),
the simulator logs, and `report.md` from `ab_diff.py`.

## How the DLL runs without Windows

The DLL only exists unpacked inside a running process, and on Windows it talks
to the cable through `openport.sys`, a 20 KB KMDF bulk read/write driver built
from the WDK sample. Everything it says to the cable is `ReadFile`/`WriteFile`
on that pipe. On macOS the same cable is a CDC-ACM tty, and the simulator is a
pty. So the job is to run the DLL under Wine and splice its device handle onto
a pty. Four things had to be true, and each was established by an experiment
rather than assumed:

1. **Which emulation stack executes the packed DLL.** Three failed, differently:
   Wine for macOS (its WoW64 layer faults inside its own exception dispatch
   when the protector raises the privileged-instruction exceptions it uses for
   control flow); Docker amd64 under Rosetta (Rosetta aborts on Wine's
   signal-return path); Docker i386 under qemu-user (Wine's mmap-alignment
   assertion). The stack that works is a native arm64 container with **box64**
   translating an x86-64 **Wine 11.17 wow64** build, and only with
   `BOX64_DYNAREC=0`: the dynarec mis-executes the protector's self-modifying
   code and the DLL exits silently during `LoadLibrary`. The interpreter runs
   it in about five seconds end to end; the DLL is I/O bound. `Dockerfile`
   pins all of it.
2. **How the DLL finds the cable.** `WINEDEBUG=+relay` showed it, no
   disassembly needed: `SetupDiGetClassDevsW` on interface class
   `{6d1781b7-c987-4f6c-8d4f-1efc098bea67}` with `DIGCF_PRESENT`, then
   `SetupDiEnumDeviceInterfaces`, then `CreateFileW` on the returned symbolic
   link. Wine answers SetupDi from the registry, so `iface.reg` fabricates one
   present interface; Wine's setupapi computes its link as
   `\\?\usb#vid_0403&pid_cc4d#op20ab001#{guid}`, and Wine's ntdll resolves a
   plain `\??\name` through `$WINEPREFIX/dosdevices/name`. A symlink there to
   the pty is the whole splice.
3. **Read semantics.** The DLL issues one overlapped `ReadFile` of 8191 bytes
   from a reader thread and expects it to complete when a USB packet arrives.
   Wine treats the pty as a serial port with zero `COMMTIMEOUTS`, so that read
   waits for all 8191 bytes and the version reply never arrives (the DLL then
   sends `tbi` three times and gives up, see below). Wine completes an
   overlapped serial read on the first byte, and never with zero bytes, when
   only `ReadIntervalTimeout=MAXDWORD` is set. `wineshim.c` patches the export
   table entry of `kernel32!CreateFileW` in the harness process before the DLL
   loads and sets those timeouts on any handle opened on the device path. No
   instruction patching; inert on real Windows.
4. **The simulator had to speak the DLL's dialect.** The DLL numbers its
   commands and waits for the number to come back (below). The simulator did
   not echo it, so the DLL reported "no devices available" after a perfectly
   good `ari` reply. It does now.

`container-run.sh` is the in-container half; `ab.sh` builds both harness
binaries (the same `j2534_trace.c` compiled with `i686-w64-mingw32-gcc` and
with `cc`), runs the container, runs the native side, and diffs.

## What the vendor DLL does on the wire

Everything below was read off `vendor.tap`, build 1.02.0.4868, firmware
simulated as 1.17.4877. It is the vendor's behaviour under emulation, not a
cable measurement; where it makes a claim about the *firmware*, the cable run
of this harness is what confirms it.

- **Every command carries a sequence number, and the DLL waits for its echo.**
  `ata 2`, `atr 16 3`, `ato6 0 500000 0 4`, `ats6 30 0 5`, `atf6 3 64 4 6`,
  `att6 6 64 1000000 7`, `atk6 0 8`, `atc6 9`, `atz 10`. It starts at 2 after
  `ati`, which is sent bare, preceded by `\r\n\r\n` to flush the line. The
  reply it accepts is `aro <seq>`, `arr 16 12480 <seq>`, `arf6 0 <seq>` and so
  on; the trailing field `PROTOCOL.md` §3 had seen as a constant 0 is this
  echo, 0 because this driver sends no number. The DLL waits 500 ms for most
  replies and 5 s for `READ_VBATT` and `GET_CONFIG`, then returns
  `ERR_TIMEOUT`.
- **A write with `Timeout=0` is sent without a sequence number** and the DLL
  does not wait for `aro`: `att6 6 64 1000000` versus `att6 6 64 1000000 7`
  for `Timeout=1000`. The fourth argument looked constant in the scenarios,
  which always pass 1000 ms or 0; the replay of the reflash tool, which passes
  5000 ms for its driver-window transfer, sent `5000000`. It is the caller's
  `WriteMsgs` Timeout in microseconds, with 1 000 000 substituted for 0: the
  firmware's budget for getting the message onto the bus. This driver had
  been sending none, leaving the firmware's ~1 s default in force even when
  the tool allowed 5 s for a 257-byte transfer paced by a slow ECU. It sends
  the argument now. On the cable an unnumbered `att` gets **no reply at all**, not
  even `are 9` when the bus never acknowledges, and while the firmware is
  still trying to send, the next command (`atc6`) is not answered for about
  a second, which is the DLL's `PassThruDisconnect` returning `ERR_TIMEOUT`
  right after. The numbered form on a dead bus answers `are 9 <seq>` after
  1.0 s. J2534 says `Timeout=0` means queue and return; the vendor does that
  (`rc=0`, one message sent). This driver did not, and now does: the write
  goes out numbered and unwaited, and its reply is dropped by number when it
  arrives.
- **`CLEAR_RX_BUFFER` touches the wire not at all** in the vendor DLL: it
  clears the DLL's own receive queue. This driver used to send `atl` as well,
  whose device-side effect is unmeasured; it no longer does.
- **`atm` is the firmware's periodic message.** `PassThruStartPeriodicMsg`
  (100 ms) sends `atm6 100000 0 64 6 <seq>` + payload: interval in
  microseconds, a 0, TxFlags, length, sequence. `PassThruStopPeriodicMsg` sends
  `atn6 <id> <seq>`. On the cable `atm` answers `arm6 0 <seq>`, then
  `arm6 1 <seq>`, and the facility was measured against a bench ECU
  (`PROTOCOL.md` §10). This driver sends the same commands.
- **`SetProgrammingVoltage(pin, VOLTAGE_OFF)` is sent as `atv 12 -1`**, the
  value printed signed. This driver sends it the same way since 2026-09-16;
  the firmware accepts both forms.
- **Open is `ati`, `ata`; close is `atz`.** `PassThruReadVersion` answers from
  the `ati` reply cached at open and touches the wire not at all.
  `READ_VBATT` is `atr 16 <seq>`.
- **If `ati` gets no reply the DLL sends `tbi\n`** (LF only, no CR) three
  times 300 ms apart on a second handle, reading up to 256 bytes each time,
  then reports `ERR_DEVICE_NOT_CONNECTED`. No reply shape this project can
  guess satisfies it; a bootloader query is the likely reading, and the cable
  run answers it.
- **Long replies repeat the CAN id in every chunk.** Fed a 600-byte ISO15765
  reply chunked with the id only in the first frame, the DLL delivered 598
  bytes; fed the same reply with the id at the start of every continuation
  chunk, it delivered all 606 bytes intact. Tactrix's own consumer reads the
  firmware as `id_every_chunk`, which is the simulator's default. This
  driver strips the id from every continuation chunk, as the DLL does.
- **Multi-frame delivery shape matches.** Both drivers hand the application a
  4-byte `ISO15765_FIRST_FRAME` indication and then one complete message,
  which is what J2534 Table 72 asks for.

- **Five-baud init is `atw<ch> <address>`**, the address in decimal on the
  command line and nothing after it (`atw3 51` for 0x33). This driver sent
  `atw3 1` plus the address as a raw byte, so the firmware initialised
  address 1 every time and then misread the byte, which on the bench wedges
  the following disconnect. That is the most likely reason the Audi's live
  K-line never answered in June. Fixed; fast init already matched. The vendor
  also opens ISO9141 with a trailing value that varies from run to run (3 on
  the bench, 5 on the car: the DLL's handle of the ISO14230 channel it had
  just closed), which is its own bookkeeping and not copied.
- **On the Audi (2009 1.9 TDI, EDC16), both drivers fail every K-line init
  identically**: fast init to 0x33 and 0x01, five-baud to 0x33, 0x01, 0x10,
  0x17, 0x19, 0x25, 0x08 and 0x46, all `ERR_FAILED` from both, and again
  after re-seating the OBD plug. The line is electrically present: a fast
  init takes 126–132 ms on the car for either driver (the wake-up pulse, the
  request and the wait) against 78 ms on the bench with nothing on pin 7.
  The driver is not the reason. The open-source EDC16 K-line flasher
  (`fjvva/ecu-tool`, which reads and writes EDC16U31/34 over pin 7) does a
  5-baud init to **0x01**, the keyword handshake, then StartCommunication
  `81 10 F1 81` to **0x10** expecting `83 F1 10 C1 EF 8F`, and retries the
  5-baud init up to six times because the ECU often ignores the first ones.
  Replicating that from the cable (eight 5-baud attempts at 0x01 on both
  K-line channels, W1 raised to 1000 ms, 9600 and 10400 baud, six fast inits
  to 0x10) produced no sync byte at all. No module answered a 5-baud init at
  any of eight VAG addresses either. Either OBD pin 7 on this car is not on
  a live K-line (the Galletto may have been used at the ECU connector or in
  boot mode), or the cable's 5-baud waveform is not what this ECU accepts;
  a multimeter on pin 7 (12 V idle means a pulled-up K-line is present) and
  a bit-banged init from an FTDI cable would tell the two apart.
- **On the Audi's CAN, both drivers delivered the identical raw frames**: the
  `$01 00` request echoed on 0x7DF and the engine's reply on 0x7E8
  (`06 41 00 98 3b 80 19`), same status bits, same sizes. The transmit-done
  indication has the same shape too: a 4-byte message carrying the CAN id,
  as J2534-1 §8.6 specifies.
- **A running periodic message is silent in the vendor DLL**: `atm` on the
  live bus produced no transmit-indication frames at all in 550 ms, and the
  application read nothing. This driver uses `atm` and sees the same.

## API semantics where the two disagree

From the `errors` scenario. This driver follows the standard where it can
justify it; the vendor is the de-facto reference an application was tested
against. Both are listed so a decision is a decision and not an accident.

| Call | Vendor | This driver |
|---|---|---|
| `PassThruDisconnect(9)` before any open | `ERR_INVALID_CHANNEL_ID` (2) | `ERR_INVALID_DEVICE_ID` (26), J2534-1 §7.2.1 |
| `PassThruIoctl(deviceId, 0xDEAD)` | `ERR_INVALID_CHANNEL_ID` (2) | `ERR_INVALID_IOCTL_ID` (15) |
| `PassThruStartMsgFilter` with a NULL mask | `ERR_FAILED` (7) | `ERR_NULL_PARAMETER` (4) |
| `PassThruReadMsgs(timeout=0)` on an idle channel | returns after ~6 ms | returns in microseconds |
| `PassThruIoctl(READ_PROG_VOLTAGE)` with `pInput` NULL, as J2534-1 passes it | -1, nothing sent | reads pin 12 |
| `SetProgrammingVoltage(7, SHORT_TO_GROUND)` under an open ISO9141 channel | `atv 7 -2`, 0 | `ERR_CHANNEL_IN_USE` (20), nothing sent |
| `PassThruConnect(ISO14230)` while K is grounded | 0 | `ERR_CHANNEL_IN_USE` (20) |
| `SetProgrammingVoltage` on a second pin while one holds a voltage | `atv`, 0 | `ERR_PIN_INVALID` (19), nothing sent |
| `PassThruConnect(SCI_A_ENGINE)` and other ids with no firmware channel | `ato0`, `are 3`, `ERR_INVALID_PROTOCOL_ID` | `ERR_INVALID_PROTOCOL_ID`, nothing sent |

The last five are from cable replays on 2026-09-16 (`PROTOCOL.md` §5, §8).
Every J2534-2 channel id, `TX_PARAM_STOP_BITS`, `FAST_INIT` on the L line and
`SNIFF_MODE` produce the same commands from both drivers. Of the rest:
grounding K under a K-line channel ends its communication, and neither the
firmware nor the vendor DLL objects. With a pin in `pInput`, both drivers send
`atr <pin>` and agree.

Everything else in the `errors` sequence, including every timeout code, every
`ERR_CHANNEL_IN_USE`, `ERR_INVALID_FILTER_ID`, `ERR_INVALID_MSG_ID` and
`ERR_NOT_SUPPORTED`, agrees.

## Timing, and what it can and cannot say

The vendor side runs interpreted. Its CPU-bound work is tens of times slower
than native, so a per-call latency comparison is meaningless in absolute terms.
What the vendor numbers *are* good for is the waits the DLL chooses, which are
wall-clock and unaffected: the 500 ms and 5 s reply timeouts, the 300 ms
`tbi` retry, the ~6 ms polling granularity of `ReadMsgs`, and its
`ReadMsgs(100)` / `ReadMsgs(500)` returning at 107 / 508 ms against this
driver's 99.5 / 501 ms.

Our own numbers are native and worth reading: before this harness,
`PassThruOpen` took ~92 ms and `PassThruClose` ~51 ms here against 24 and
6 ms for the vendor DLL doing the same wire work. The cause was the open
sequence draining and settling the pipe to guard against stale replies, and
the close waiting for a 50 ms reader poll. Numbering commands the way the
vendor does removes the need for the drain, and the poll is now 10 ms; the
cable run after the change is in `tools/ab-official/out/report.md`.

`bench` runs N write/read cycles and records each; `ab_diff.py` prints
medians. With the cable attached both sides see real USB latency, and the
vendor's emulation overhead sits on top of it, so treat vendor cable timings
as upper bounds.

## Running against the cable

The image bakes `iface.reg` into its Wine registry. An image built from an
older `iface.reg` (before the public release it carried the bench cable's own
serial) registers a device the container never links, and the vendor DLL
answers "no devices available" without sending a byte. `ab.sh` labels the
image with a hash of `Dockerfile` and `iface.reg` and rebuilds it when they
change.

`ab.sh --cable /dev/cu.usbmodemXXXX` serves the tty on `127.0.0.1:5455` with
`ttybridge.py`, the container reaches it as `host.docker.internal:5455` and
turns it back into a pty; the vendor DLL then drives the real cable. Our side
runs natively on the same device afterwards. No ECU is needed for the
firmware questions; transmits fail with `ERR_TIMEOUT` on both sides for want
of an ACK peer, as on any one-cable bench.

Measured on the bench, 2026-09-13, firmware 1.17.4877: the sequence number is
echoed on every reply (`arr 16 108 3`, `arf6 0 8`, `arg6 30 0 10`,
`are 1 9`); `atm` answers `arm6 <id> <seq>` and `atn` stops it; `tbi` gets
no reply with either line ending; `atv 12 -1` is accepted. All four are in
`PROTOCOL.md` and the simulator now answers `atm`. Still open: the chunk
layout of a >250-byte reply on the wire, which needs an ECU that sends one.

## Replaying a real application through both drivers

The scenarios above are this project's own idea of what an application does.
A recording is the application itself. `OPENPORT_RECORD=<file>` makes the
driver append one line per entry-point call with every input and the id each
call handed back; `j2534_trace --replay` executes that file against any
J2534 library, mapping the recorded ids onto the ones the library under test
returns. `ab.sh --replay` runs one recording through the vendor DLL and
through `libj2534.dylib` against identical simulators and compares the wire
byte for byte, command by command, payloads included, with only the
open/close handshake (`ati`/`ata`/`atz`, known to differ) set aside.

A recording is plain text, one call per line, so a probe can also be written
by hand. `ioctl <dev> 14 <pin>` passes the pin through `pInput` to
`READ_PROG_VOLTAGE`, which is how the vendor DLL was shown to take it
(`PROTOCOL.md` §8).

```bash
tools/ab-official/record.sh out/app.rec \
    --sim-args "--ecu-model model.py:Ecu:image.bin" -- \
    python3 my_tool.py --lib libj2534.dylib
tools/ab-official/ab.sh --replay out/app.rec --sim-args "--ecu-model model.py:Ecu:image.bin"
```

`--ecu-model FILE.py:CLASS[:IMAGE]` installs a scripted ECU behind the
simulator: `CLASS(image_bytes_or_None, log=...)` is called once and the
instance answers each request (`request_bytes -> response_bytes | None`).
That is how a reflash tool's write path — programming session, seed/key,
driver upload, erase, page transfers, read-back verify — is rehearsed end to
end against a model of the target bootloader, which no car allows. The model
for a given ECU lives with that ECU's tooling, not here.

Rehearsed this way against a 512 KB bootloader model, a real reflash tool's
recording was 21 497 calls: one session, 5 882 transmits
(driver windows, 1 920 image pages, and every `$23` read of the verify) and
15 605 reads. Replaying it through the vendor DLL is the strongest statement
this harness can make about a flash: for the exact requests the tool will
send, Tactrix's own driver and this one put the same bytes on the wire.
