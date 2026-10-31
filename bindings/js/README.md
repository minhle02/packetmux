# PacketMux for TypeScript

Many byte streams over one connection, with a session that survives reconnects.

You bring the connection, such as a WebSocket or a WebRTC data channel. PacketMux handles the
handshake, keepalive, reconnecting, backpressure and a clean close. It runs in Node and in browser
renderers, is written in plain JS with full types, and has no dependencies. The core is WebAssembly,
and loads by itself.

```ts
import { connect } from 'packetmux'

const client = await connect(dial)
const stream = await client.openStream('echo')
await stream.write(new TextEncoder().encode('hello'))
console.log(await stream.read())
await client.close()
```

To integrate it:

1. [Implement a Transport](#1-implement-a-transport) over your connection.
2. [Connect a client](#2-connect-a-client) with a dial function.
3. [Use streams](#3-use-streams) to send and receive bytes.
4. [Run a server](#4-run-a-server) that accepts them, in Node.

A complete client and server over WebSocket are in the `example/` folder of the source repository.

## How it works

- **A session** is one relationship with a peer. It survives reconnects, and the SDK generates its ID.
- **A stream** is one logical connection, such as one command or one tunnel. A session carries many.
- **Streams do not survive a lost connection.** They end with `TRANSPORT_LOST`. The session reconnects,
  and you open new ones.

## 1. Implement a Transport

A `Transport` is one connection that delivers whole messages, reliably and in order. It is the only
thing you have to implement.

WebSockets and data channels push messages, so extend `PushTransport`, which keeps the queue that the SDK
reads from:

```ts
import { PushTransport } from 'packetmux'

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
```

| You implement | What it must do |
|---|---|
| `bufferedAmount` | Bytes accepted and not yet on the wire |
| `sendMessage(message)` | Hand over one message |
| `closeSource()` | Close the connection |

| You call | When |
|---|---|
| `handleMessage(message)` | For each whole message. Start before the first one can arrive, so that none is lost |
| `handleClose(error?)` | When the connection closes, whether cleanly or not |
| `handleBufferLow()` | Optional, when the send buffer drains. Waiting polls `bufferedAmount` either way |

Set `maxMessageSize` if your connection carries less than the default 1 MiB, as an RTC data channel does.
It must be at least 32 773 bytes with the default limits: a smaller transport is refused, not left to
fail in the middle of a transfer.

A pull-based source can implement the `Transport` interface directly, with `maxMessageSize`,
`bufferedAmount`, `send`, `recv`, `waitBufferBelow` and `close`. After it closes, `send`, `recv` and
`waitBufferBelow` must reject with `TransportClosed`.

Check your Transport with the same tests the SDK uses, in Node:

```ts
import { checkTransport } from 'packetmux/testing'

await checkTransport(makeConnectedPair) // returns two connected transports, or a Promise of them
```

## 2. Connect a client

`connect` takes a **dial function**: it gets the session ID, and returns one newly opened Transport. The
SDK calls it to connect, and again after every lost connection. It resolves once the handshake has
finished.

```ts
import { connect } from 'packetmux'

function dial(sessionId: string): Promise<WebSocketTransport> {
  return new Promise((resolve, reject) => {
    const socket = new WebSocket(`wss://example.com/mux?session=${sessionId}`)
    socket.onopen = () => resolve(new WebSocketTransport(socket))
    socket.onerror = () => reject(new Error('cannot connect'))
  })
}

const client = await connect(dial, { connectTimeoutMs: 5000 })
// ...
await client.close()
```

`await using client = await connect(dial)` closes it for you, where your target supports it.

| Option | Default | Meaning |
|---|---|---|
| `connectTimeoutMs` | 10 000 | Limit for one dial, for the first `connect`, and for `openStream` to wait for a connection |
| `reconnectBackoffMs` | 500, 1000, 2000, 4000, 8000 | Delay between attempts. The last value repeats |
| `inflightLimit` | 64 KiB | How much to keep in the Transport's buffer |
| `session` | `{}` | Core settings, such as `maxStreams` or `pingIntervalMs`. See [Core settings](#core-settings) |
| `handlers` | None | For streams the server opens. See [Handlers](#handlers) |
| `capabilities` | Empty | Opaque bytes for the peer's `authorize` |
| `signal` | None | An `AbortSignal` for this `connect` call |

### Reconnecting

Reconnecting is automatic. After `connect` has resolved, there is no attempt limit. It stops when:

- you close the client;
- the server ends the session, with `GOAWAY`;
- your dial function throws `GiveUp`, for a failure that will not fix itself.

```ts
import { GiveUp } from 'packetmux'

async function dial(sessionId: string): Promise<WebSocketTransport> {
  const response = await fetch('/ticket')
  if (response.status === 401) {
    throw new GiveUp('the token was refused')
  }
  return openSocket(sessionId, await response.text())
}
```

Any other error counts as a failed attempt, and the SDK tries again after the backoff.

Choosing between connections belongs in the dial function, because the SDK only sees a Transport. To
prefer WebRTC and fall back to WebSocket, try one and then the other, and return whichever opens.

### State

```ts
client.sessionId // generated by the SDK, and you cannot set it
client.state // 'connecting' | 'connected' | 'reconnecting' | 'closed'
client.transport // the active Transport, or null
client.rttMs // the last round trip, or null
client.closeError // why it closed, if not by close()
client.streams() // StreamInfo for every open stream

const remove = client.onStateChange((state) => console.log(state))
await client.waitClosed()
```

`connected` means the handshake has finished.

### When the server loses the session

If the server restarted, or its grace period ran out, the client closes with `SessionNotFound`. The SDK
does not open a replacement by itself, because your streams and anything tied to the session are gone.
Connect again for a new session, with a new ID:

```ts
await client.waitClosed()
if (client.closeError instanceof SessionNotFound) {
  client = await connect(dial)
}
```

A reloaded page or a restarted process is always a new session too.

## 3. Use streams

```ts
const stream = await client.openStream('echo') // does not wait for the peer to accept
await stream.write(data) // resolves once every byte is queued
await stream.sendEof() // we are done writing

for await (const chunk of stream) { // until the peer has finished too
  console.log(chunk)
}

const code = await stream.waitClosed() // Reason.OK means a clean close
```

`openStream` waits for a connection for at most `connectTimeoutMs`, then rejects with `NoConnection`. You
can write at once, and to wait for the peer, call `await stream.waitAccepted()`.

### Reading

`await stream.read()` resolves with the next chunk, or `null` once the peer has half-closed and everything
has been read. A reset rejects the next read with `StreamReset`, even with data still buffered.

A stream that is not read stalls on its own: the peer stops after one receive window, 256 KiB by default,
and other streams carry on. On a stream whose peer replies to what you write, such as an echo, read while
you write. If you only read after a large write has finished, both sides wait for each other.

### Writing

`await stream.write(data)` takes any size, and waits while queues are full. Concurrent writes on one
stream run in order. `sendEof` waits for writes in progress, and writing after it rejects with
`StreamClosed`.

### Closing

A stream closes cleanly once both sides have sent EOF. To abort, call `stream.reset(code)`, which defaults
to `Reason.KILLED`. Queued data is discarded, and the peer's next read rejects with `StreamReset`.

### Cancelling

`read`, `write`, `waitAccepted` and `openStream` take `{ signal }`, an `AbortSignal`. The session stays
usable. An aborted `write` leaves an unknown amount sent.

`connect` takes `signal` too. There is no client to go back to, so aborting it closes the session and
stops dialing.

## 4. Run a server

The server runs in Node. Hand each accepted connection to `serve`. It reads the first message, finds or
creates the session, and runs the connection until it ends.

```ts
import { IncomingStream, Server } from 'packetmux'

async function echo(request: IncomingStream): Promise<void> {
  const stream = request.accept()
  for await (const chunk of stream) {
    await stream.write(chunk)
  }
}

const server = new Server({ handlers: [['echo', echo]] })

sockets.on('connection', (socket) => { // however your server calls you
  void server.serve(new WebSocketTransport(socket))
})
```

| Option | Default | Meaning |
|---|---|---|
| `gracePeriodMs` | 120 000 | How long a disconnected session waits for its client |
| `maxSessions` | 1024 | New sessions over this are refused with `LIMIT_EXCEEDED` |
| `handlers`, `authorize`, `capabilities` | | See below |
| `inflightLimit`, `session` | As for the client | |

### Handlers

A handler runs for each stream the peer opens, and gets a request to accept or reject. Clients can have
handlers too, through `connect(dial, { handlers })`, for streams the server opens.

```ts
import { IncomingStream, Reason, Reject } from 'packetmux'

async function forward(request: IncomingStream): Promise<void> {
  const target = parseTarget(request.params) // raw bytes
  let connection
  try {
    connection = await openTarget(target) // async work comes before accepting
  } catch (error) {
    throw new Reject(Reason.CONNECT_FAILED, new TextEncoder().encode(String(error)))
  }

  const stream = request.accept()
  // ...
}
```

Data that arrives before you accept is kept, in order.

| The handler | What happens |
|---|---|
| Throws `Reject(code, message)` before accepting | The stream is rejected with that code |
| Calls `request.reject(code, message)` | The same |
| Throws anything else before accepting | Rejected with `CONNECT_FAILED` |
| Returns without accepting | Rejected with `POLICY_DENIED` |
| Throws `Reject` after accepting | The stream is reset with that code |
| Throws anything else after accepting | Logged, and reset with `KILLED` |
| Returns after accepting | EOF is sent for you. The stream stays until the peer's EOF, so it can still read the reply |

A kind with no handler is rejected with `UNSUPPORTED_KIND`. When the session ends, the streams end, so
reads in a handler reject.

### Authorization

PacketMux does not authenticate. `authorize` is your hook, and it runs before any existing session is
touched, so that a peer who only knows a session ID cannot take over a live connection.

```ts
import { ConnectRequest, Reason, Refuse, Server } from 'packetmux'

async function authorize(request: ConnectRequest): Promise<string> {
  const user = checkToken(request.context) // what you passed to serve
  if (user === null) {
    throw new Refuse(Reason.AUTH_REVOKED)
  }
  if (request.session !== null && request.session.data !== user) {
    throw new Refuse(Reason.AUTH_REVOKED) // someone else's session
  }
  return user // stored as session.data
}

const server = new Server({ handlers, authorize })
await server.serve(transport, token)
```

A refused client closes with `SessionClosed`, and does not retry.

### Sessions

```ts
for (const session of server.sessions()) {
  console.log(session.sessionId, session.connected, session.data, session.rttMs, session.streams())
}

session.transport // the active Transport, or null
const stream = await session.openStream('logs') // rejects with NoConnection while disconnected
await session.close(Reason.KILLED)
await server.close() // closes every session
```

A client that dials while its old connection still looks alive replaces it. A session that loses its
connection waits for `gracePeriodMs`, then closes with `TIMEOUT`.

To clean up after a session, however it ended, pass `onSessionClosed`:

```ts
const server = new Server({
  handlers,
  authorize,
  onSessionClosed: (session, code) => releaseDevice(session.data),
})
```

It runs once per session, after the session has left `server.sessions()`. It is synchronous, so start your
own async work for cleanup. An exception is logged and goes no further.

## Typed params

`openStream` can send params with a stream. Declare a stream type, and the SDK encodes them as JSON and
checks them on both sides. TypeScript types do not exist at runtime, so the fields are values, and the
static type comes from them:

```ts
import { IncomingStream, ParamsOf, Server, defineStream } from 'packetmux'

const fields = { host: 'string', port: 'int', timeout: { type: 'float?', default: null } } as const
const TcpForward = defineStream('tcp-forward', fields)

async function forward(request: IncomingStream<ParamsOf<typeof fields>>): Promise<void> {
  const { host, port } = request.params // already decoded and checked
  const stream = request.accept()
  await stream.write(new TextEncoder().encode(`connected to ${host}:${port}`))
}

const server = new Server({ handlers: [[TcpForward, forward]] })

// On the other side
const stream = await client.openStream(TcpForward({ host: '127.0.0.1', port: 5037 }))
```

- Field types are `'string'`, `'int'`, `'float'` and `'bool'`. Add `?` to allow `null`. Give
  `{ type, default }` for a default.
- An `'int'` must be a whole number within the safe integer range.
- Peers in other languages must use the same field names, exactly as declared.
- A missing field is refused unless it has a default. A field the type does not declare is ignored, so
  adding one does not break older peers.
- Bad outgoing params reject with `InvalidParams`, and nothing is sent. Bad incoming params reject the
  stream with `PROTOCOL_ERROR` before your handler runs.

For binary params or your own format, send raw bytes, and register the handler by its kind:

```ts
const stream = await client.openStream('tcp-forward', new Uint8Array([1, 2]))
const server = new Server({ handlers: [['tcp-forward', forward]] }) // request.params is a Uint8Array
```

## Errors

All extend `PacketMuxError`.

| Error | Thrown when |
|---|---|
| `StreamReset` | Reading, writing or waiting on a reset stream. `.code` has the reason |
| `StreamClosed` | Writing after `sendEof`, or after a clean close |
| `NoConnection` | No connection within `connectTimeoutMs`, or a server session is disconnected |
| `SessionClosed` | The session has ended. `.code` has the reason |
| `SessionNotFound` | A `SessionClosed`: the server no longer has the session |
| `LimitExceeded` | `maxStreams` is reached |
| `InvalidParams` | Typed params do not match, or a stream kind is invalid |
| `TransportClosed` | You throw this from a `Transport` |
| `GiveUp` | You throw this from a dial function, to stop reconnecting |
| `Reject` | You throw this from a handler, to reject the stream |
| `Refuse` | You throw this from `authorize`, to refuse the connection |

Stream kinds may only contain `a-z`, `A-Z`, `0-9` and `-`.

`Reason` holds the numeric codes. To show one, compare it with the names:

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

`session` in both option objects sets these:

| Setting | Default |
|---|---|
| `maxStreams` | 64 |
| `maxQueuedBytes` | 16 MiB |
| `maxQueuedBytesPerStream` | 256 KiB |
| `receiveWindow` | 256 KiB. What the peer may send per stream before you read it. A stream is capped at this ÷ round trip, so raise it for long paths |
| `helloTimeoutMs` | 10 000 |
| `pingIntervalMs` | 15 000 |
| `deadAfterMs` | 45 000 |
| `drainTimeoutMs` | 1 000 |

Both sides of a connection should use the same `maxDataPayload` and `maxControlPayload`, if you change
them at all.

## Loading the WebAssembly

The module loads on the first `connect` or `serve`. If your bundler moves `packetmux.wasm`, say where it
went before anything else:

```ts
import { loadPacketMux } from 'packetmux'

await loadPacketMux({ wasmUrl: '/assets/packetmux.wasm' })
```

## Testing your code

`packetmux/testing` runs your client and server code in memory, with faults you control. It runs in Node.

```ts
import { Server, connect } from 'packetmux'
import { MemoryNetwork } from 'packetmux/testing'

const server = new Server({ handlers })
const network = new MemoryNetwork(server)
const client = await connect(network.dial)

const [clientSide, serverSide] = network.links.at(-1)!
clientSide.stall() // the link is full
clientSide.release()
await clientSide.breakConnection() // the link dies, on both sides
clientSide.failLocally() // only this side sees it die

network.failNext(3) // the next dials fail
network.hang() // dials wait, until hang(false)
network.giveUp() // dials throw GiveUp
```

## The sync binding

`packetmux/core` is the synchronous layer below this SDK. You own the event loop, and have to keep every
host rule yourself, so almost every application should use the SDK.
