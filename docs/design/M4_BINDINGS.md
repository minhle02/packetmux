# M4 — Python and JS/WASM bindings: design

Status: implemented. Exit criteria met on macOS arm64 (§7).
Requirements: `docs/requirements/INTENT.md` (NFR-PORT-3/6, D3).

Both bindings are thin: one call to one `packetmux_*` function, plus memory handling. They stay
synchronous, like the library. `asyncio` and Promise wrappers belong to the consumer (RemoteDevice).

## 1. API

Both bindings have the same shape, in each language's naming style. Reason codes and states are
exported as constants. Events are plain objects holding **their own copy** of the bytes, so nobody
using a binding has to think about buffer lifetimes.

### Python (`bindings/python/packetmux/`)

```python
session = Session(role=Role.CLIENT, session_id="s1", capabilities=b"")   # other limits as keywords
session.attach_connection(now_ms)
session.on_message(message: bytes, now_ms)
session.on_connection_lost()
session.on_tick(now_ms)
session.next_deadline_ms() -> int | None                     # None instead of NO_DEADLINE
session.open_stream(kind: str, params: bytes, now_ms) -> int
session.accept_stream(stream_id)
session.reject_stream(stream_id, code, message: bytes = b"")
session.write(stream_id, data: bytes, now_ms) -> int         # accepted byte count
session.send_eof(stream_id)
session.reset_stream(stream_id, code=Reason.KILLED)
session.close(code, now_ms)
session.poll_transmit() -> bytes | None
session.poll_event() -> Event | None
session.state, session.rtt_ms, session.list_streams()

decode_frame(message: bytes) -> Frame                        # for the server's first HELLO
encode_frame(frame: Frame) -> bytes
```

A non-OK status raises `SessionError(status)`. `Session` is a context manager, so the native handle
is released at the end.

### JS (`bindings/js/`)

```ts
const { Session, decodeFrame, encodeFrame, Role, Reason } = await loadPacketMux();

const session = new Session({ role: Role.CLIENT, sessionId: "s1" });
session.attachConnection(nowMs);
session.onMessage(message: Uint8Array, nowMs);
session.onConnectionLost();
session.onTick(nowMs);
session.nextDeadlineMs(): number | null;
session.openStream(kind: string, params: Uint8Array, nowMs): number;
session.acceptStream(id);  session.rejectStream(id, code, message?);
session.write(id, data: Uint8Array, nowMs): number;
session.sendEof(id);  session.resetStream(id, code?);  session.close(code, nowMs);
session.pollTransmit(): Uint8Array | null;
session.pollEvent(): SessionEvent | null;
session.state;  session.rttMs;  session.listStreams();
session.destroy();                       // WASM memory is not garbage-collected
```

Written as plain JS with a hand-written `index.d.ts`, so the package needs no TypeScript build.

## 2. Wiring

```python
# Python: ctypes over the shared library. No build step and no dependencies.
class Session:
    def on_message(self, message, now_ms):
        self._check(lib.packetmux_session_on_message(self._handle, message, len(message), now_ms))
        self._message = message             # keeps it alive while events borrow from it

    def poll_event(self):
        event = packetmux_event_t()
        if not lib.packetmux_session_poll_event(self._handle, byref(event)):
            return None
        return Event.from_native(event)     # copies every byte out
```

```js
// JS: WASM memory is separate, so each message is copied in.
onMessage(message, nowMs) {
  this.#freeMessage();                      // events are drained by now, or the call below refuses
  this.#message = copyIn(message);          // _malloc + HEAPU8.set
  check(wasm._packetmux_session_on_message(this.#handle, this.#message, message.length, BigInt(nowMs)));
}

pollEvent() {
  if (!wasm._packetmux_session_poll_event(this.#handle, this.#eventBuffer)) return null;
  return readEvent(this.#eventBuffer);      // reads fields by offset, slices bytes out
}
```

If `onMessage` is refused with `EVENTS_PENDING`, the previous message must survive, so the real code
frees it only after the call succeeds, and frees the new copy on failure.

### Build

- **Python:** `cmake -DBUILD_SHARED_LIBS=ON`. The binding finds the library next to the package, or
  through `PACKETMUX_LIBRARY`. The project is managed with `uv` in `bindings/python/packetmux/`, and
  has no runtime dependencies. `websockets` and `pytest` are in uv's dev group.
- **JS:** a `packetmux_wasm` CMake target, used when `EMSCRIPTEN` is set. Linked with `em++`, which
  pulls in the C++ runtime. Flags: `-Os -fno-exceptions -fno-rtti -sWASM_BIGINT -sMODULARIZE
  -sEXPORT_ES6 -sALLOW_MEMORY_GROWTH`, plus the exported functions and
  `-sEXPORTED_RUNTIME_METHODS=HEAPU8`. The output goes to `bindings/js/dist/`.

### Struct layout

Python gets field offsets from `ctypes.Structure`. JS has to read them by hand, and they differ from
native because pointers are 4 bytes on wasm32 (`packetmux_event_t` is 52 bytes there, 96 natively).

The offsets live in one table in `layout.js`. A wasm-only `static_assert(offsetof(...))` file repeats
them, so changing a C struct without updating JS fails the build, not a test at runtime.

## 3. Changes outside the bindings

| Change | Why |
|---|---|
| Add `-Wno-missing-designated-field-initializers` to the library, when the compiler knows it | Emscripten's newer Clang warns on our partial `{.type = ...}` initializers (21 times), and `PACKETMUX_WERROR` turns that into a build failure. Apple Clang 17 stays silent. The initializers are deliberate: unset fields use their defaults |

## 4. Verification

Kept to acceptance level, like M3. Everything below these layers is already covered.

| Test | Tool | Proves |
|---|---|---|
| Golden vectors, read from `tests/vectors/FramesV2.txt` | `pytest`, Node `node:test` | Exit criterion: `encode`, `decode` and their errors work through each binding |
| Lifecycle: handshake, request and reply, reject, reset, keepalive, reconnect, close | Same | The binding drives a session, and events own their bytes |
| Python ↔ JS over a real WebSocket: handshake, a transfer checked byte for byte in both directions, a clean close | Python server, Node client | Exit criterion: the same protocol across two languages and a real transport |
| WASM size | Build check | Under the 150 KB target (NFR-PERF-3) |

The Python server uses `websockets`, and the Node client uses Node's built-in `WebSocket`. Each
binding also checks its enum values against the C header.

File names in `bindings/` follow each language's convention, not the PascalCase rule for C++.

## 5. Facts checked before writing this

Measured with Emscripten 6.0.6, Python 3.15.0 and Node 24.18.1, in a throwaway build outside the repo:
- The library compiles to WASM with no source changes, apart from the warning in §3.
- A module with all 23 functions is **38 KB of WASM** (16 KB gzipped), plus a 10 KB JS loader.
- It loads in Node, and `packetmux_config_init` returns the right defaults.

## 6. How to run

```sh
# WASM module, needed by the JS tests and the interop test
emcmake cmake -S . -B build/wasm -DPACKETMUX_BUILD_TESTS=OFF
cmake --build build/wasm

(cd bindings/js && npm test)
(cd bindings/python/packetmux && uv run pytest)   # builds the shared library itself
```

## 7. Verification results (2026-10-30)

Platform: macOS arm64, Emscripten 6.0.6, Python 3.15.0, Node 24.18.1. All of this ran from a clean copy
holding only the files git would track.

| Check | Result |
|---|---|
| WASM build with `PACKETMUX_WERROR=ON` | Clean. 38 KB of WASM, against a 150 KB target |
| JS tests: golden vectors, enums, lifecycle | 74 of 74 pass |
| Python tests: golden vectors, enums, lifecycle, interop | 74 of 74 pass |
| Python server ↔ Node client over a real WebSocket | 3 MiB and 2 MiB, byte for byte, with backpressure and a clean close |
| WASM memory growing during a transfer (16 to 32 MiB) | Events stay intact |
| Deliberate bugs in the bindings, layout table and interop | 11 of 11 caught |
| C++ tests after the CMake change | 181 of 181 pass |

Not done:
- **A real browser or Electron renderer.** JS ran in Node only. The module is the same, but WASM
  loading in the renderer is untested. It belongs with the RemoteDevice migration.
- **Python 3.12.** The project allows it, but only 3.15 has been run.
- **Linux and Windows.** Still unbuilt, along with GCC and MSVC.
- **Packaging.** Left for later, by decision.
