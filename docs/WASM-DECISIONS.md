# Why the browser build looks the way it does

The reasoning behind [WASM-PLAN.md](WASM-PLAN.md): what was chosen, what was rejected and why,
and what is left to decide after the proof of concept. Facts carry the date they were checked.

## WebAssembly with JSPI

The driver and `gry_write` block: a command waits for its reply, ReadMsgs waits for messages, the
tool sleeps between polls. Web Serial is asynchronous. JSPI (JavaScript Promise Integration) lets a
WebAssembly call suspend at an import that returns a Promise and resume when it settles, so the
blocking C runs unchanged.

- JSPI is in Chromium only. So is Web Serial, so it costs no browser reach.
- Node 26 has it on by default: `WebAssembly.Suspending` and `WebAssembly.promising` exist
  (checked 2026-10-11).
- Emscripten 6.0.10 is the current release (Homebrew, 2026-10-11).
- It needs no threads in the browser, so pages that use it need no cross-origin-isolation headers.
- Fallback if JSPI fails: Emscripten's Asyncify gives the same design in every browser, with a
  larger and slower build.

## Alternatives rejected

| Alternative | Why not |
|---|---|
| A TypeScript port | A second implementation to keep in parity with every measurement. The existing one, `@emdzej/j2534-*` at `35aae08`, discards bytes split across reads, takes stale replies as answers, routes frames to whichever channel is reading, drops 29-bit CAN, does not reassemble long ISO-TP replies, and sends commands the cable refuses (five-baud as `aty<ch> 1 1`, letters as channels, `VOLTAGE_OFF` as `atv <pin> 0`) |
| Emscripten pthreads | Requires `SharedArrayBuffer`, so every page that uses the package must send COOP and COEP headers |
| Cooperative threads on JSPI, keeping the C threading model | Needs a separate C stack per suspended call. Emscripten's `-sREENTRANT_JSPI` (pull requests #27698 and #27699) is open, not released; doing it by hand, as Pyodide describes, is fragile glue that only the browser build would exercise |
| Making the J2534 layer non-blocking | Rewrites the most-tested file into callback-style C for no gain over JSPI |
| A local helper daemon as the only route | Needs an install, which removes the point of a web app. Kept in view for writes (below) |
| WebUSB | macOS's CDC-ACM driver holds the cable's interfaces; Web Serial is the path |
| WebAssembly stack switching, shared-everything threads | Not shipped in V8 as of 2026-10-11 (stack switching's stage is disputed between 2 and 3; shared-everything threads is an early draft) |

## Why the core refactor

The device logic in `op_device.c` is the driver's most important code and is reachable only
through threads, real timeouts and the mock's background thread. Separating it serves both
platforms: the browser shell needs a core it can feed from one stack, and native gains fuzzing and
deterministic tests of reassembly and reply matching, which the current structure prevents. TSAN
once found three races that 438 passing checks missed (TESTING.md); a small locked shell is easier
to review than one interleaved with the logic.

## After the PoC

- **Licence.** The library is GPL-3.0-or-later. A GPL browser library effectively requires apps
  that ship it to be GPL-compatible. The author holds the copyright to every commit, so the npm
  package can carry a different licence: MIT, or MPL-2.0 to keep changes to the library itself
  open.
- **API shape.** J2534-faithful, idiomatic JavaScript, or both layers.
- **Package name.** `openport-j2534` is free on npm (checked 2026-10-11).
- **Writes from a browser.** A tab can be closed mid-erase. Whether writes run in the page, or are
  handed to a local companion app when one is installed, is decided with phase 4's results in
  hand. A companion runs the job independently of the tab, at the cost of an install and of
  securing its local connection to one page.

## Sources

- Pyodide, "Integrating JSPI with the WebAssembly C Runtime", 2025-07-03:
  https://blog.pyodide.org/posts/jspi-with-c-runtime/
- Emscripten issue #20058 (stack corruption across suspensions):
  https://github.com/emscripten-core/emscripten/issues/20058
- Emscripten pull requests #27698 (JSPI hooks) and #27699 (`REENTRANT_JSPI`):
  https://github.com/emscripten-core/emscripten/pull/27698
- MDN, `WorkerNavigator.serial`: https://developer.mozilla.org/docs/Web/API/WorkerNavigator/serial
- Emscripten's WasmFS OPFS backend (JSPI path):
  https://chromium.googlesource.com/external/github.com/kripken/emscripten/+/HEAD/src/lib/libwasmfs_opfs.js
- The emdzej comparison: `github.com/emdzej/j2534` at `35aae08`, read 2026-10-11 against
  PROTOCOL.md.
