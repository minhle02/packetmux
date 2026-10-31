# PacketMux

Many byte streams over one connection, with a session that survives reconnects.

One C++ library holds the protocol, with no dependencies beyond the C++ standard library. Python and
TypeScript each have an async SDK on top of it. Your code implements one thing, a `Transport`, and
says how to dial it. The SDK does the rest: handshake, keepalive, reconnecting, backpressure and a
clean close.

```python
from packetmux import connect

async with await connect(dial) as client:
    stream = await client.open_stream("echo")
    await stream.write(b"hello")
    print(await stream.read())
```

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
uv run scripts/build.py wheel     # Python package, for this operating system
uv run scripts/build.py npm       # TypeScript package
```

| Target | Output |
|---|---|
| `wasm` | `bindings/js/dist/packetmux.mjs`, `packetmux.wasm` |
| `macos` | `build/macos/libpacketmux.dylib` |
| `linux` | `build/linux/libpacketmux.so` |
| `windows` | `build/windows/<build type>/packetmux.dll` |
| `wheel` | `build/packages/packetmux-<version>-py3-none-<platform>.whl` |
| `npm` | `build/packages/packetmux-<version>.tgz` |

Options: `--build-type Debug|Release|RelWithDebInfo|MinSizeRel` (default `Release`) and `--clean`.

`macos`, `linux` and `windows` must each be built on that operating system. So far only `macos` and
`wasm` have been tested; `linux` and `windows` have not been built yet.

To run the tests:

```sh
# C++, including the conformance suite
cmake -S . -B build/tests
cmake --build build/tests --parallel
ctest --test-dir build/tests

# Python, including real WebSocket tests against JS (build the wasm target first, and install JS)
(cd bindings/python/packetmux && uv run pytest)

# JS (build the wasm target first)
(cd bindings/js && npm install && npm test)
```

To add another language, start with `docs/spec/BINDING.md`. `docs/spec/PROTOCOL.md` defines the
protocol, and `conformance/` tests it.

## How to use it

Install the package for your language, from `uv run scripts/build.py wheel` or `npm`:

```sh
pip install build/packages/packetmux-0.2.0-py3-none-macosx_11_0_arm64.whl
npm install build/packages/packetmux-0.2.0.tgz
```

A wheel contains native code, so it is built per platform. In a `pyproject.toml`:

```toml
[project]
dependencies = ["packetmux @ file:///path/to/packetmux-0.2.0-py3-none-macosx_11_0_arm64.whl"]
```

What to know:

- **A session is one relationship with a peer.** It survives reconnects, and the SDK generates its ID.
- **A stream is one logical connection,** such as one `adb shell`. Streams end when a connection is
  lost, with `TRANSPORT_LOST`. Open new ones after the reconnect.
- **Reconnecting is automatic,** with no attempt limit. It stops when you close the client, when your
  dial function raises `GiveUp`, or when the server ends the session.
- **If the server no longer has the session,** the client closes with `SessionNotFound`. Connect again
  for a new session.
- **A reader that falls 16 MiB behind loses its stream,** with `LIMIT_EXCEEDED`. There is no flow
  control per stream yet, so keep reading. Change the limit with `max_read_buffer`.

### Python

You write a `Transport` and a dial function:

```python
import asyncio

from packetmux import Transport, TransportClosed, connect
from websockets.asyncio.client import connect as ws_connect
from websockets.exceptions import ConnectionClosed


class WebSocketTransport(Transport):
    def __init__(self, socket) -> None:
        self._socket = socket

    async def send(self, message: bytes) -> None:
        try:
            await self._socket.send(message)
        except ConnectionClosed as error:
            raise TransportClosed() from error

    async def recv(self) -> bytes:
        try:
            return await self._socket.recv()
        except ConnectionClosed as error:
            raise TransportClosed() from error

    @property
    def buffered_amount(self) -> int:
        return self._socket.transport.get_write_buffer_size()

    async def close(self) -> None:
        await self._socket.close()


async def dial(session_id: str) -> WebSocketTransport:
    return WebSocketTransport(await ws_connect(f"ws://localhost:8000/mux?session={session_id}"))


async def main() -> None:
    async with await connect(dial) as client:
        stream = await client.open_stream("echo")
        await stream.write(b"hello")
        await stream.send_eof()
        async for chunk in stream:
            print(chunk)


asyncio.run(main())
```

A server hands each accepted connection to `serve`, and registers a handler for each kind of stream:

```python
from packetmux import ConnectRequest, IncomingStream, Refuse, Server


async def echo(request: IncomingStream) -> None:
    stream = request.accept()
    async for chunk in stream:
        await stream.write(chunk)


async def authorize(request: ConnectRequest) -> str:
    """Runs before an existing session is touched. Return who the peer is, or raise Refuse."""
    user = check_token(request.context)
    if request.session is not None and request.session.data != user:
        raise Refuse()
    return user


server = Server(handlers=[("echo", echo)], authorize=authorize)


async def on_websocket(socket, token: str) -> None:  # however your web framework calls you
    await server.serve(WebSocketTransport(socket), context=token)
```

Declare a stream type to send typed params, and the SDK encodes and checks them on both sides:

```python
from dataclasses import dataclass

from packetmux import IncomingStream, Server, stream_type


@stream_type("tcp-forward")
@dataclass
class TcpForward:
    host: str
    port: int
    timeout: float | None = None


async def forward(request: IncomingStream[TcpForward]) -> None:
    target = request.params
    stream = request.accept()
    await stream.write(f"connected to {target.host}:{target.port}".encode())


server = Server(handlers=[(TcpForward, forward)])

# On the client:
#     stream = await client.open_stream(TcpForward(host="127.0.0.1", port=5037))
```

`packetmux.testing` has an in-memory transport for your tests, and `check_transport` runs every check
a `Transport` has to pass.

### TypeScript

The same concepts, in TypeScript's style. Durations are in milliseconds, and `read` returns `null` at
the end of a stream. `PushTransport` holds the queue behind `recv`, for sources that push:

```ts
import { PushTransport, connect } from 'packetmux'

class WebSocketTransport extends PushTransport {
  constructor(private readonly socket: WebSocket) {
    super()
    socket.binaryType = 'arraybuffer'
    socket.onmessage = (event: MessageEvent<ArrayBuffer>) => this.handleMessage(new Uint8Array(event.data))
    socket.onclose = () => this.handleClose()
  }

  get bufferedAmount(): number {
    return this.socket.bufferedAmount
  }

  protected sendMessage(message: Uint8Array): void {
    this.socket.send(message)
  }

  protected closeSource(): void {
    this.socket.close()
  }
}

function dial(sessionId: string): Promise<WebSocketTransport> {
  return new Promise((resolve, reject) => {
    const socket = new WebSocket(`ws://localhost:8000/mux?session=${sessionId}`)
    socket.onopen = () => resolve(new WebSocketTransport(socket))
    socket.onerror = () => reject(new Error('cannot connect'))
  })
}

const client = await connect(dial)
const stream = await client.openStream('echo')
await stream.write(new TextEncoder().encode('hello'))
await stream.sendEof()
for await (const chunk of stream) {
  console.log(chunk)
}
await client.close()
```

Typed params declare their fields as values, because TypeScript types do not exist at runtime:

```ts
import { IncomingStream, ParamsOf, Server, defineStream } from 'packetmux'

const fields = { host: 'string', port: 'int', timeout: { type: 'float?', default: null } } as const
const TcpForward = defineStream('tcp-forward', fields)

async function forward(request: IncomingStream<ParamsOf<typeof fields>>): Promise<void> {
  const { host, port } = request.params
  const stream = request.accept()
  await stream.write(new TextEncoder().encode(`connected to ${host}:${port}`))
}

const server = new Server({ handlers: [[TcpForward, forward]] })

// On the client:
//     const stream = await client.openStream(TcpForward({ host: '127.0.0.1', port: 5037 }))
```

If your bundler moves `packetmux.wasm`, call `loadPacketMux({ wasmUrl })` before anything else.

### Without the SDK

The sync binding is `packetmux.core` in Python and `packetmux/core` in TypeScript. It is sans-I/O: you
own the event loop and keep the host rules in `docs/spec/BINDING.md` yourself. Almost every consumer
should use the SDK.
