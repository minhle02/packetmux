# PacketMux

One C++ library for multiplexing many byte streams over one connection, and for managing the
session around them. It has no dependencies beyond the C++ standard library, and it does no I/O: your
code owns the transport (WebSocket, WebRTC, ...), and the library handles the protocol logic.

Python and TypeScript use the same core through thin bindings.

## How to build

You need:
- CMake 3.23 or newer, and a C++20 compiler
- [uv](https://docs.astral.sh/uv/) to run the build script
- [Emscripten](https://emscripten.org/) on your `PATH`, for the `wasm` target

Build for one target:

```sh
uv run scripts/build.py wasm      # module for TypeScript / JS
uv run scripts/build.py macos     # shared library for Python
uv run scripts/build.py linux
uv run scripts/build.py windows
```

| Target | Output |
|---|---|
| `wasm` | `bindings/js/dist/packetmux.mjs`, `packetmux.wasm` |
| `macos` | `build/macos/libpacketmux.dylib` |
| `linux` | `build/linux/libpacketmux.so` |
| `windows` | `build/windows/<build type>/packetmux.dll` |

Options: `--build-type Debug|Release|RelWithDebInfo|MinSizeRel` (default `Release`) and `--clean`.

`macos`, `linux` and `windows` must each be built on that operating system. So far only `macos` and
`wasm` have been tested; `linux` and `windows` have not been built yet.

To run the tests:

```sh
# C++
cmake -S . -B build/tests
cmake --build build/tests --parallel
ctest --test-dir build/tests

# Python, including a real WebSocket test against JS (build the wasm target first)
(cd bindings/python/packetmux && uv run pytest)

# JS (build the wasm target first)
(cd bindings/js && npm test)
```

## How to integrate the library

The same rules apply in both languages:

- **Create one session per peer**, not one per stream. A session survives reconnects. Open a stream
  for each logical connection, such as one `adb shell`.
- **The session never touches the network or the clock.** You pass it each received message and the
  current time, and you take out frames to send and events to handle.
- **After every session call, run the pump:** take events and frames until neither has any left. The
  session refuses the next message if events are still waiting.
- **Set one timer** for `next_deadline_ms()`. It changes after every call, and `null`/`None` means no
  timer is needed.
- **`write` can accept less than you gave it**, even 0. Keep the rest, and continue on a `WRITABLE`
  event.
- **Close your transport** when you get `DISCONNECTED` or `SESSION_CLOSED`. After `DISCONNECTED`, the
  session stays usable: reconnect and call `attach` again. Its streams have ended, so open new ones.
- **Report a closed transport with `on_connection_lost`**, always. It does nothing if the session
  already ended that connection.
- **`session_id` and stream kinds** may only contain `a-z`, `A-Z`, `0-9` and `-`.
- **Time** is in milliseconds, from a monotonic clock, as a whole number.

The bindings are synchronous. Calling them from `asyncio` or Promises is up to your code.

### Python

Build the shared library, then make both the package and the library findable:

```sh
uv run scripts/build.py macos
export PYTHONPATH=$PWD/bindings/python/packetmux
export PACKETMUX_LIBRARY=$PWD/build/macos/libpacketmux.dylib
```

Or, to ship it with your application, copy the `bindings/python/packetmux/packetmux/` folder into
your project and put the library file inside it. `PACKETMUX_LIBRARY` is then not needed.

The binding has no runtime dependencies. A client looks like this, where `transport` is your own
connection object:

```python
import time

from packetmux import EventType, Reason, Role, Session

session = Session(Role.CLIENT, "my-session-1")
stream_id = 0


def now_ms() -> int:
    return time.monotonic_ns() // 1_000_000


def on_event(event) -> None:
    if event.type == EventType.DATA:
        print("received", event.data)
    elif event.type == EventType.STREAM_CLOSED:
        print("stream ended", event.code)  # Reason.OK means a clean close
    elif event.type in (EventType.DISCONNECTED, EventType.SESSION_CLOSED):
        transport.close()


def pump() -> None:
    """Run this after every session call."""
    progress = True
    while progress:
        progress = False
        while (event := session.poll_event()) is not None:
            on_event(event)
            progress = True
        while (message := session.poll_transmit()) is not None:
            transport.send(message)
            progress = True


# Call these from your transport and your timer.
def on_transport_connected() -> None:
    global stream_id
    session.attach_connection(now_ms())
    stream_id = session.open_stream("tcp-forward", b"", now_ms())
    session.write(stream_id, b"hello", now_ms())
    session.send_eof(stream_id)
    pump()


def on_transport_message(message: bytes) -> None:
    session.on_message(message, now_ms())
    pump()


def on_transport_closed() -> None:
    session.on_connection_lost()
    pump()


def on_timer() -> None:
    session.on_tick(now_ms())
    pump()
```

A status other than OK raises `SessionError`. Use `session.destroy()`, or `with Session(...)`, to
free the native session.

A server does not know which session a new connection belongs to until the first message arrives,
so it reads that message first. It also has to answer streams the peer opens:

```python
from packetmux import Reason, Role, Session, decode_frame

sessions: dict[str, Session] = {}


def on_new_connection(first_message: bytes, now_ms: int) -> Session:
    hello = decode_frame(first_message)
    # Check the client is allowed in here: the library does not authenticate.
    session = sessions.get(hello.session_id)
    if session is None:
        session = sessions[hello.session_id] = Session(Role.SERVER, hello.session_id)
    session.on_connection_lost()  # the client may have re-dialed before the old connection died
    while session.poll_event() is not None:
        pass  # in your host, handle these: the old streams have ended
    session.attach_connection(now_ms)
    session.on_message(first_message, now_ms)
    return session


def on_stream_opened(session: Session, event) -> None:
    if event.kind == "tcp-forward":
        session.accept_stream(event.stream_id)
    else:
        session.reject_stream(event.stream_id, Reason.UNSUPPORTED_KIND)
```

### TypeScript

Build the WASM module, then add the binding to your project:

```sh
uv run scripts/build.py wasm
npm install /path/to/PacketMux/bindings/js
```

The package includes its types. Load the module once, then create sessions from it:

```ts
import { EventType, Reason, Role, loadPacketMux } from 'packetmux'
import type { SessionEvent } from 'packetmux'

const { Session } = await loadPacketMux()
const session = new Session({ role: Role.CLIENT, sessionId: 'my-session-1' })

// Math.floor matters: the time must be a whole number.
const nowMs = (): number => Math.floor(performance.now())

const socket = new WebSocket('ws://localhost:8000/transport')
socket.binaryType = 'arraybuffer'

function onEvent(event: SessionEvent): void {
  if (event.type === EventType.DATA) {
    console.log('received', event.data)
  } else if (event.type === EventType.STREAM_CLOSED) {
    console.log('stream ended', event.code) // Reason.OK means a clean close
  } else if (event.type === EventType.DISCONNECTED || event.type === EventType.SESSION_CLOSED) {
    socket.close()
  }
}

// Run this after every session call.
function pump(): void {
  let progress = true
  while (progress) {
    progress = false
    for (let event = session.pollEvent(); event !== null; event = session.pollEvent()) {
      onEvent(event)
      progress = true
    }
    for (let message = session.pollTransmit(); message !== null; message = session.pollTransmit()) {
      socket.send(message)
      progress = true
    }
  }
}

socket.onopen = () => {
  session.attachConnection(nowMs())
  const streamId = session.openStream('tcp-forward', null, nowMs())
  session.write(streamId, new TextEncoder().encode('hello'), nowMs())
  session.sendEof(streamId)
  pump()
}

socket.onmessage = (message: MessageEvent<ArrayBuffer>) => {
  session.onMessage(new Uint8Array(message.data), nowMs())
  pump()
}

socket.onclose = () => {
  session.onConnectionLost()
  pump()
}

// When the session is finished. WASM memory is not garbage-collected.
export function shutdown(): void {
  session.close(Reason.OK, nowMs())
  pump()
  session.destroy()
}
```

A status other than OK throws `SessionError`. Timers work as in Python: set one for
`session.nextDeadlineMs()`, and call `session.onTick(nowMs())` then `pump()` when it fires.

So far the binding has only been run in Node. Loading the WASM file in a browser or an Electron
renderer has not been tested yet, and your bundler may need to be told where to find
`packetmux.wasm`.
