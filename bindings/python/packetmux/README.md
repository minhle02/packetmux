# PacketMux for Python

Many byte streams over one connection, with a session that survives reconnects.

You bring the connection, such as a WebSocket or a WebRTC data channel. PacketMux handles the
handshake, keepalive, reconnecting, backpressure and a clean close. It runs on `asyncio`, needs Python
3.12 or newer, and has no dependencies.

```python
from packetmux import connect

async with await connect(dial) as client:
    stream = await client.open_stream("echo")
    await stream.write(b"hello")
    print(await stream.read())
```

To integrate it:

1. [Implement a Transport](#1-implement-a-transport) over your connection.
2. [Connect a client](#2-connect-a-client) with a dial function.
3. [Use streams](#3-use-streams) to send and receive bytes.
4. [Run a server](#4-run-a-server) that accepts them.

A complete client and server over WebSocket are in the `example/` folder of the source repository.

## How it works

- **A session** is one relationship with a peer. It survives reconnects, and the SDK generates its ID.
- **A stream** is one logical connection, such as one command or one tunnel. A session carries many.
- **Streams do not survive a lost connection.** They end with `TRANSPORT_LOST`. The session reconnects,
  and you open new ones.

## 1. Implement a Transport

A `Transport` is one connection that delivers whole messages, reliably and in order. It is the only
thing you have to implement.

```python
from packetmux import Transport, TransportClosed
from websockets.exceptions import ConnectionClosed


class WebSocketTransport(Transport):
    def __init__(self, socket) -> None:
        self._socket = socket

    async def send(self, message: bytes) -> None:
        if len(message) > self.max_message_size:
            raise ValueError("message too large")
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
```

| Member | What it must do |
|---|---|
| `send(message)` | Hand over one message. Accepted does not mean delivered. It may wait. Raise `ValueError` for a message over `max_message_size` |
| `recv()` | Return the next whole message. A message that arrives before you call it must not be lost |
| `buffered_amount` | Bytes accepted by `send` and not yet on the wire |
| `close()` | Close the connection. Calling it twice must be safe |
| `wait_buffer_below(limit)` | Optional. Wait until `buffered_amount` is below `limit`. The default polls, so override it if your connection has a drain event |
| `max_message_size` | Optional. The largest message it carries. The default is 1 MiB |

After the connection closes, for any reason, `send` and `recv` must raise `TransportClosed`, and so must
`wait_buffer_below` if you override it. `max_message_size` must be at least 32 773 bytes with the default
limits: a smaller transport is refused, not left to fail in the middle of a transfer.

Check your Transport with the same tests the SDK uses:

```python
from packetmux.testing import check_transport


async def test_my_transport() -> None:
    await check_transport(make_connected_pair)  # returns two connected transports, or awaits to
```

## 2. Connect a client

`connect` takes a **dial function**: it gets the session ID, and returns one newly opened Transport. The
SDK calls it to connect, and again after every lost connection. It returns once the handshake has
finished.

```python
from packetmux import ClientOptions, connect
from websockets.asyncio.client import connect as ws_connect


async def dial(session_id: str) -> WebSocketTransport:
    socket = await ws_connect(f"wss://example.com/mux?session={session_id}")
    return WebSocketTransport(socket)


client = await connect(dial, options=ClientOptions(connect_timeout=5))
...
await client.close()
```

`async with await connect(dial) as client:` closes it for you. `connect` also takes `handlers`, for
streams the server opens, and `capabilities`, opaque bytes for the peer's `authorize`.

| `ClientOptions` | Default | Meaning |
|---|---|---|
| `connect_timeout` | 10 s | Limit for one dial, for the first `connect`, and for `open_stream` to wait for a connection |
| `reconnect_backoff` | 0.5, 1, 2, 4, 8 s | Delay between attempts. The last value repeats |
| `max_read_buffer` | 16 MiB | See [Reading](#reading) |
| `inflight_limit` | 64 KiB | How much to keep in the Transport's buffer |
| `session` | `{}` | Core settings, such as `max_streams` or `ping_interval_ms`. See [Core settings](#core-settings) |

### Reconnecting

Reconnecting is automatic. After `connect` has returned, there is no attempt limit. It stops when:

- you close the client;
- the server ends the session, with `GOAWAY`;
- your dial function raises `GiveUp`, for a failure that will not fix itself.

```python
from packetmux import GiveUp


async def dial(session_id: str) -> WebSocketTransport:
    try:
        return WebSocketTransport(await open_socket(session_id))
    except PermissionError as error:
        raise GiveUp("the token was refused") from error
```

Any other exception counts as a failed attempt, and the SDK tries again after the backoff.

Choosing between connections belongs in the dial function, because the SDK only sees a Transport. To
prefer WebRTC and fall back to WebSocket, try one and then the other, and return whichever opens.

### State

```python
client.session_id                  # generated by the SDK, and you cannot set it
client.state                       # ClientState: CONNECTING, CONNECTED, RECONNECTING, CLOSED
client.transport                   # the active Transport, or None
client.rtt_ms                      # the last round trip, or None
client.close_error                 # why it closed, if not by close()
client.streams()                   # StreamInfo for every open stream

remove = client.on_state_change(lambda state: print(state))
await client.wait_closed()
```

`CONNECTED` means the handshake has finished. Listeners run on the event loop, and must not block.

### When the server loses the session

If the server restarted, or its grace period ran out, the client closes with `SessionNotFound`. The SDK
does not open a replacement by itself, because your streams and anything tied to the session are gone.
Connect again for a new session, with a new ID:

```python
await client.wait_closed()
if isinstance(client.close_error, SessionNotFound):
    client = await connect(dial)
```

A restarted client process is always a new session too.

## 3. Use streams

```python
stream = await client.open_stream("echo")          # does not wait for the peer to accept
await stream.write(b"hello")                       # returns once every byte is queued
await stream.send_eof()                            # we are done writing

async for chunk in stream:                         # until the peer has finished too
    print(chunk)

code = await stream.wait_closed()                  # Reason.OK means a clean close
```

`open_stream` waits for a connection for at most `connect_timeout`, then raises `NoConnection`. You can
write at once, and to wait for the peer, call `await stream.wait_accepted()`.

### Reading

`await stream.read()` returns the next chunk, or `b""` once the peer has half-closed and everything has
been read. A reset raises `StreamReset` on the next read, even with data still buffered.

Keep reading. There is no flow control per stream yet, so if a stream falls `max_read_buffer` behind, it
is reset with `LIMIT_EXCEEDED`. Other streams are not affected.

### Writing

`await stream.write(data)` takes any size, and waits while queues are full. Concurrent writes on one
stream run in order. `send_eof` waits for writes in progress, and writing after it raises `StreamClosed`.

### Closing

A stream closes cleanly once both sides have sent EOF. To abort, call `stream.reset(code)`, which defaults
to `Reason.KILLED`. Queued data is discarded, and the peer's next read raises `StreamReset`.

### Cancelling

Every blocking call can be cancelled in the usual way, for example with `asyncio.wait_for`. The session
stays usable. A cancelled `write` leaves an unknown amount sent.

Cancelling `connect` is different: there is no client to go back to, so it closes the session and stops
dialing.

## 4. Run a server

Hand each accepted connection to `serve`. It reads the first message, finds or creates the session, and
runs the connection until it ends.

```python
from packetmux import IncomingStream, Server


async def echo(request: IncomingStream) -> None:
    stream = request.accept()
    async for chunk in stream:
        await stream.write(chunk)


server = Server(handlers=[("echo", echo)])


async def on_websocket(socket) -> None:            # however your web framework calls you
    await server.serve(WebSocketTransport(socket))
```

| `ServerOptions` | Default | Meaning |
|---|---|---|
| `grace_period` | 120 s | How long a disconnected session waits for its client |
| `max_sessions` | 1024 | New sessions over this are refused with `LIMIT_EXCEEDED` |
| `max_read_buffer`, `inflight_limit`, `session` | As for the client | |

### Handlers

A handler runs for each stream the peer opens, and gets a request to accept or reject. Clients can have
handlers too, through `connect(dial, handlers=...)`, for streams the server opens.

```python
from packetmux import IncomingStream, Reason, Reject


async def forward(request: IncomingStream) -> None:
    target = parse_target(request.params)          # raw bytes
    try:
        connection = await open_target(target)     # async work comes before accepting
    except OSError as error:
        raise Reject(Reason.CONNECT_FAILED, str(error).encode()) from error

    stream = request.accept()
    ...
```

Data that arrives before you accept is kept, in order.

| The handler | What happens |
|---|---|
| Raises `Reject(code, message)` before accepting | The stream is rejected with that code |
| Calls `request.reject(code, message)` | The same |
| Raises anything else before accepting | Rejected with `CONNECT_FAILED` |
| Returns without accepting | Rejected with `POLICY_DENIED` |
| Raises `Reject` after accepting | The stream is reset with that code |
| Raises anything else after accepting | Logged, and reset with `KILLED` |
| Returns after accepting | EOF is sent for you. The stream stays until the peer's EOF, so it can still read the reply |

A kind with no handler is rejected with `UNSUPPORTED_KIND`. Handlers are cancelled when the session ends.

### Authorization

PacketMux does not authenticate. `authorize` is your hook, and it runs before any existing session is
touched, so that a peer who only knows a session ID cannot take over a live connection.

```python
from packetmux import ConnectRequest, Refuse, Reason, Server


async def authorize(request: ConnectRequest) -> str:
    user = check_token(request.context)            # what you passed to serve
    if user is None:
        raise Refuse(Reason.AUTH_REVOKED)
    if request.session is not None and request.session.data != user:
        raise Refuse(Reason.AUTH_REVOKED)          # someone else's session
    return user                                    # stored as session.data


server = Server(handlers=handlers, authorize=authorize)
await server.serve(transport, context=token)
```

A refused client closes with `SessionClosed(code)`, and does not retry.

### Sessions

```python
for session in server.sessions():
    print(session.session_id, session.connected, session.data, session.rtt_ms, session.streams())

session.transport                                  # the active Transport, or None
stream = await session.open_stream("logs")         # raises NoConnection while disconnected
await session.close(Reason.KILLED)
await server.close()                               # closes every session
```

A client that dials while its old connection still looks alive replaces it. A session that loses its
connection waits for `grace_period`, then closes with `TIMEOUT`.

To clean up after a session, however it ended, pass `on_session_closed`:

```python
def forget(session: ServerSession, code: Reason) -> None:
    release_device(session.data)


server = Server(handlers=handlers, authorize=authorize, on_session_closed=forget)
```

It runs once per session, after the session has left `server.sessions()`. It is synchronous, so start a
task for async cleanup. An exception is logged and goes no further.

## Typed params

`open_stream` can send params with a stream. Declare a stream type, and the SDK encodes them as JSON and
checks them on both sides:

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
    target = request.params                        # already decoded and checked
    stream = request.accept()
    await stream.write(f"connected to {target.host}:{target.port}".encode())


server = Server(handlers=[(TcpForward, forward)])

# On the other side
stream = await client.open_stream(TcpForward(host="127.0.0.1", port=5037))
```

- Field types are `str`, `int`, `float`, `bool`, and any of those with `| None`, in a flat class.
- Peers in other languages must use the same field names, exactly as declared.
- A missing field is refused unless it has a default. A field the type does not declare is ignored, so
  adding one does not break older peers.
- Bad outgoing params raise `InvalidParams`, and nothing is sent. Bad incoming params reject the stream
  with `PROTOCOL_ERROR` before your handler runs.

For binary params or your own format, send raw bytes, and register the handler by its kind:

```python
stream = await client.open_stream("tcp-forward", b"my own format")
server = Server(handlers=[("tcp-forward", forward)])    # request.params is bytes
```

## Errors

All extend `PacketMuxError`.

| Error | Raised when |
|---|---|
| `StreamReset` | Reading, writing or waiting on a reset stream. `.code` has the reason |
| `StreamClosed` | Writing after `send_eof`, or after a clean close |
| `NoConnection` | No connection within `connect_timeout`, or a server session is disconnected |
| `SessionClosed` | The session has ended. `.code` has the reason |
| `SessionNotFound` | A `SessionClosed`: the server no longer has the session |
| `LimitExceeded` | `max_streams` is reached |
| `InvalidParams` | Typed params do not match, or a stream kind is invalid |
| `TransportClosed` | You raise this from a `Transport` |
| `GiveUp` | You raise this from a dial function, to stop reconnecting |
| `Reject` | You raise this from a handler, to reject the stream |
| `Refuse` | You raise this from `authorize`, to refuse the connection |

Stream kinds may only contain `a-z`, `A-Z`, `0-9` and `-`.

| Reason | Meaning |
|---|---|
| `OK` | Clean close |
| `PEER_CLOSED` | The other side closed, or the session ended |
| `CONNECT_FAILED`, `POLICY_DENIED`, `UNSUPPORTED_KIND` | The stream was refused |
| `LIMIT_EXCEEDED` | A limit was reached |
| `TRANSPORT_LOST` | The connection was lost |
| `KILLED` | Reset on purpose |
| `AUTH_REVOKED` | Authorization failed or was withdrawn |
| `TIMEOUT` | A peer went quiet, or a grace period ran out |
| `PROTOCOL_ERROR` | A peer broke the protocol |
| `SESSION_NOT_FOUND` | The server no longer has the session |

## Core settings

`session` in both option classes sets these:

| Setting | Default |
|---|---|
| `max_streams` | 64 |
| `max_queued_bytes` | 1 MiB |
| `max_queued_bytes_per_stream` | 256 KiB |
| `hello_timeout_ms` | 10 000 |
| `ping_interval_ms` | 15 000 |
| `dead_after_ms` | 45 000 |
| `drain_timeout_ms` | 1 000 |

Both sides of a connection should use the same `max_data_payload` and `max_control_payload`, if you
change them at all.

## Testing your code

`packetmux.testing` runs your client and server code in memory, with faults you control.

```python
from packetmux import Server, connect
from packetmux.testing import MemoryNetwork

server = Server(handlers=handlers)
network = MemoryNetwork(server)
client = await connect(network.dial)

client_side, server_side = network.links[-1]
client_side.stall()                                # the link is full
client_side.release()
await client_side.break_connection()               # the link dies, on both sides
client_side.fail_locally()                         # only this side sees it die

network.fail_next(3)                               # the next dials fail
network.hang()                                     # dials wait, until hang(False)
network.give_up()                                  # dials raise GiveUp
```

## Threads

All objects of one session must be used from one event loop.

## The sync binding

`packetmux.core` is the synchronous layer below this SDK. You own the event loop, and have to keep every
host rule yourself, so almost every application should use the SDK.
