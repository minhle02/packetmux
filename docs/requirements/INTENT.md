# PacketMux — Intent and Requirements

Status: core implemented (M1 to M4). SDK sections approved, not implemented. Version of the
wire protocol described here: **2**. It is not released yet, so `HELLO.resuming` and
`SESSION_NOT_FOUND` are added to v2 directly (D8, D22).

The keywords MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as in RFC 2119.
Each requirement has an ID so that design documents, code and tests can refer to it.

## 1. Background and intent

RemoteDevice runs a session layer that multiplexes many byte streams over one persistent connection
between a remote client and the backend (see
`RemoteDevice/backend/docs/transport_service_design.md`). This layer is implemented twice:

- Python: `backend/webserver/transport` (backend and remote client)
- TypeScript: `frontend/src/renderer/src/features/adb/core/v3` (Electron renderer)

The two implementations have already drifted. The TypeScript scheduler is plain round-robin and
client-only, while the Python one has priority classes and both roles. Every protocol change has to
be made twice and kept in step by golden-vector tests.

**Intent:** implement the session and multiplexing logic **once**, as a C++ library with no
dependencies beyond the standard library. The library compiles to native code and to WebAssembly and
is used from every host language through thin bindings. Each host keeps its own transport
(WebSocket, WebRTC, ...) and its own async runtime.

Integrating the core into RemoteDevice showed that this is not enough. The protocol now exists once,
but each host still had to write the same async layer around it (769 lines in Python, 938 in
TypeScript) and follow the same rules by hand. Those rules are easy to break: both hosts lost frames
on re-dial in the same way, because a send loop took a frame after its connection was replaced.

**Intent, SDK:** a consumer MUST be **one import and a few calls away** from working streams, and
MUST NOT need to understand the session logic. Each language ships an **async-first SDK** with the
same shape. The only thing a consumer implements is a **Transport**. The sans-I/O core stays
underneath, unchanged in purpose.

**Intent, other languages:** adding a language MUST take the published specs and a language-neutral
conformance suite, not a reading of the C++ source.

## 2. Scope

### 2.1 In scope
- Frame codec for protocol v2 (encode, decode, validation).
- Stream lifecycle: open, accept/reject, data, half-close (EOF), reset (RST).
- Session: stream registry, stream ID allocation, frame dispatch, limits, close/GOAWAY.
- Outbound scheduling: control frames first, round-robin between streams.
- Handshake (HELLO) and keepalive (PING/PONG, dead-connection detection, RTT).
- Bounded send queues (local backpressure).
- An optional length-prefix framer for byte-stream transports (§5.11).
- A stable C ABI over the C++ API.
- Thin bindings for Python and for JavaScript/TypeScript (WASM), in `bindings/<language>/`.
- An async-first SDK in Python and TypeScript, on top of those bindings: client, server, async
  streams, reconnect, and a Transport interface (§5.12 to §5.16).
- An in-memory transport with fault injection, for tests of the SDK and of consumers.
- A protocol spec, a binding spec and a language-neutral conformance suite (§5.17).

### 2.2 Out of scope (for this version)
- Wire transports other than the in-memory one (WebSocket, WebRTC, TCP, QUIC), TLS, authentication,
  device locking. The consumer implements the Transport and decides who is allowed in, through a hook
  (FR-SRV-3).
- Priority classes and weighted scheduling. Planned as a separate layer (ROADMAP.md §3).
- A connection pool with more than one live connection per session.
- Retransmission, acknowledgements, or resuming streams across a reconnect. Planned as a separate
  layer (ROADMAP.md §2).
- What a stream handler does (dialing TCP targets, pumping sockets). The SDK calls the handler; the
  work inside it is consumer code.
- How to reach the peer, including choosing between transports (for example, WebRTC with a WebSocket
  fallback). This lives in the consumer's dial function. The SDK decides when to call it.
- Management HTTP API.
- Compression and encryption of payloads.

Reconnect and the grace period are in scope for the SDK and stay out of the **core**, which only
keeps session state and never starts anything on its own (FR-API-1).

## 3. Users of the library

| User | Platform | How it uses the library |
|---|---|---|
| RemoteDevice backend | Python on Linux, macOS, Windows | Python SDK, server role |
| RemoteDevice remote client | Python, packaged with PyInstaller | Python SDK, client role |
| RemoteDevice frontend | Electron renderer (browser JS) | TypeScript SDK over WASM, client role |
| Future consumers | Python or TypeScript | SDK, with their own Transport |
| Authors of a new language binding | Any language with a C FFI | C ABI, the specs and the conformance suite (§5.17) |

## 4. Terminology

| Term | Meaning |
|---|---|
| Transport | One connection that delivers whole messages reliably and in order. Owned by the host. |
| Message | One unit handed to or received from the transport. One message carries exactly one frame. |
| Frame | Header plus payload, as defined in §5.1. |
| Session | Persistent state shared by two peers. It owns the streams and outlives individual transports. |
| Connection | The binding of a session to one live transport. |
| Stream | One bidirectional, ordered byte stream multiplexed inside a session. |
| Opener / acceptor | The peer that sent `OPEN` for a stream / the peer that received it. |
| Host | The program that embeds the library and drives it (Python, JS, ...). |
| Role | `client` or `server`. It decides stream ID parity only; otherwise the protocol is symmetric. |
| Core | The C++ library and its C ABI: sans-I/O, synchronous. |
| Binding | The thin synchronous wrapper over the core in one language (M4). |
| SDK | The async layer over a binding. This is what consumers use. |
| Consumer | An application that uses the SDK. |
| Dial function | A consumer function that takes the session ID and returns one newly opened Transport. The SDK calls it to connect and to reconnect. |
| Grace period | How long a server keeps a disconnected session for the client to come back. |

## 5. Functional requirements

### 5.1 Wire format

- **FR-WIRE-1** Each frame MUST have this layout:

  ```
  +------+-----------+---------+
  | type | stream_id | payload |
  | 1 B  | 4 B (BE)  | rest    |
  +------+-----------+---------+
  ```
  The header is 5 bytes. There is no length field and no flags byte.
- **FR-WIRE-2** All multi-byte integers on the wire, in the header and in payloads, MUST be unsigned
  and big-endian (network byte order).
- **FR-WIRE-3** Encoded bytes MUST be identical whatever the host's byte order, word size
  (wasm32, 64-bit native) or compiler.
- **FR-WIRE-4** Control payloads MUST use binary encoding. They MUST NOT use JSON or another
  text format.
- **FR-WIRE-4b** Identifiers (`kind` and `session_id`) MUST be at least 1 character long and MUST
  contain only the ASCII characters `[a-zA-Z0-9-]`, with an explicit length prefix. The encoder
  MUST refuse any other identifier, so a bad one fails locally and is never sent. The decoder MUST
  reject it as a malformed field (FR-FRAME-4).
- **FR-WIRE-4a** Control payloads MUST use a fixed layout: every field is always present, in a
  fixed order, with no tags. Variable-length fields carry an explicit length prefix. A payload
  that is truncated, has a length prefix running past its end, or has bytes left after its last
  field MUST be a protocol error. Example: `OPEN` = `kind_len (u16) | kind | params_len (u32) |
  params`. The exact layouts go in the design document.
- **FR-WIRE-5** Opaque host data (`params` in `OPEN`, `capabilities` in `HELLO`, the message in
  `OPEN_ERR`) MUST be carried as length-prefixed bytes. The library MUST NOT interpret them.
- **FR-WIRE-6** Numeric values of frame types and reason codes are part of the protocol. Once
  released, they MUST NOT be renumbered; new values are only appended.
- **FR-WIRE-7** The protocol version is **2**. Version 2 is not wire-compatible with version 1
  (JSON control payloads, 6-byte header). v2 replaces v1. The backend serves the web frontend, so
  both are deployed together and no peer needs to speak both versions.
- **FR-WIRE-8** Frame type values are renumbered for v2. `WINDOW` is removed and no value is
  reserved for it.

### 5.2 Frame types

| Type | stream_id | Payload (logical content) | Meaning |
|---|---|---|---|
| `HELLO` | 0 | version, resuming (flag), session_id, capabilities (opaque) | First frame in each direction on a new connection |
| `OPEN` | ≠ 0 | kind (identifier), params (opaque) | Open a stream |
| `OPEN_OK` | ≠ 0 | none | Acceptor accepted the stream |
| `OPEN_ERR` | ≠ 0 | reason code, message (opaque) | Acceptor rejected the stream |
| `DATA` | ≠ 0 | bytes | Stream data |
| `EOF` | ≠ 0 | none | Sender half-closes its direction |
| `RST` | ≠ 0 | reason code | Abort the stream |
| `PING` | 0 | 64-bit nonce | Keepalive and RTT probe |
| `PONG` | 0 | 64-bit nonce (echoed) | Answer to `PING` |
| `GOAWAY` | 0 | reason code | The session is ending |

- **FR-FRAME-1** Connection frames (`HELLO`, `PING`, `PONG`, `GOAWAY`) MUST use stream_id 0. All
  other frames MUST use a non-zero stream_id.
- **FR-FRAME-2** Frames with no payload (`OPEN_OK`, `EOF`) MUST have an empty payload. Fixed-size
  payloads (`RST`, `GOAWAY`, `PING`, `PONG`) MUST be exactly their size.
- **FR-FRAME-3** A `DATA` payload MUST NOT exceed `max_data_payload`. A control payload MUST NOT
  exceed `max_control_payload` (see §7).
- **FR-FRAME-4** Decoding MUST reject: messages shorter than the header, unknown frame types, a
  wrong stream_id for the type, payloads of the wrong size, and malformed or truncated payload
  fields. A rejected frame is a protocol error (FR-ERR-1).
- **FR-FRAME-5** Reason codes: `OK`, `PEER_CLOSED`, `CONNECT_FAILED`, `POLICY_DENIED`,
  `LIMIT_EXCEEDED`, `TRANSPORT_LOST`, `KILLED`, `AUTH_REVOKED`, `TIMEOUT`, `PROTOCOL_ERROR`,
  `UNSUPPORTED_KIND`, `SESSION_NOT_FOUND`. An unknown reason code received from a peer MUST be treated as
  `PROTOCOL_ERROR`. It MUST NOT be rejected as a malformed frame.
- **FR-FRAME-6** `TRANSPORT_LOST` MUST only be reported locally and MUST NOT be sent on the wire.

### 5.3 Handshake

- **FR-HS-1** On a new connection, each peer MUST send `HELLO` as its first frame.
- **FR-HS-2** Each peer MUST treat any frame before the peer's `HELLO`, or a second `HELLO`, as a
  protocol error.
- **FR-HS-3** A `HELLO` with a protocol version other than 2 MUST end the connection with
  `GOAWAY(PROTOCOL_ERROR)`.
- **FR-HS-4** If no `HELLO` arrives within `hello_timeout`, the connection MUST be closed.
- **FR-HS-5** The peer's session_id and capabilities MUST be handed to the host. The host decides
  whether the session_id is acceptable (for example, binding to an existing session). The library
  does not authenticate.
- **FR-HS-7** `HELLO` MUST carry a `resuming` flag. It is false until the session has finished a
  handshake once, and true on every connection after that. The core sets it, so a host cannot get
  it wrong. A retry after a handshake that never finished is still not resuming.

### 5.4 Streams

- **FR-STR-1** Stream IDs are 32-bit unsigned. The client role MUST allocate odd IDs and the
  server role even IDs. 0 is reserved for the connection.
- **FR-STR-2** Within a session, stream IDs MUST increase and MUST NOT be reused, including across
  reconnects. When the ID space is exhausted, opening a stream MUST fail locally. A `session_id`
  therefore belongs to one `Session` object: a peer that restarts needs a new one.
- **FR-STR-3** Either role MAY open streams (the protocol is symmetric).
- **FR-STR-4** An `OPEN` with the wrong parity, or with an ID not above the highest the peer has
  used so far, MUST be a protocol error. This also covers an ID already in use.
- **FR-STR-5** The opener MAY send `DATA` and `EOF` before it receives `OPEN_OK` (pipelining).
  `OPEN` MUST always reach the wire before any frame of that stream. The acceptor cannot send
  before it accepts, and `OPEN_OK` goes out first, so `DATA` or `EOF` received before `OPEN_OK` is
  invalid for that stream (FR-ERR-2).
- **FR-STR-6** On an inbound `OPEN`, the library MUST report it to the host. The host then accepts
  it (→ `OPEN_OK`) or rejects it with a reason code and message (→ `OPEN_ERR`). The library rejects
  it on its own with `LIMIT_EXCEEDED` if `max_streams` is reached. Once a `GOAWAY` is queued, nothing
  more is sent and inbound messages are ignored.
- **FR-STR-7** Inbound `DATA`/`EOF` that arrive before the host has accepted the stream MUST still
  be delivered to the host, in order, after the open event. If the host rejects the stream, later
  frames for it MUST be dropped silently.
- **FR-STR-8** An `OPEN_ERR` received by the opener MUST end the stream with the received reason
  code.
- **FR-STR-9** Stream states: `SENDER_OPENING`, `SENDER_OPENING_EOF_SENT`, `RECEIVER_OPENING`,
  `RECEIVER_OPENING_EOF_RECEIVED`, `OPEN`, `HALF_CLOSED_LOCAL`, `HALF_CLOSED_REMOTE`, `CLOSED`. The
  sender is the side that sent `OPEN`, and the receiver is the side that received it. Allowed
  transitions are the table in `docs/spec/PROTOCOL.md` §5.3.
- **FR-STR-10** A stream MUST close cleanly once both sides have sent `EOF`. A stream MUST close
  with a reason code on `RST` (either direction), `OPEN_ERR`, or connection loss. The host MUST see
  the reason `OK` only for a clean close: `OK` from any other cause is reported as `PEER_CLOSED`.
- **FR-STR-11** Writing after a local `EOF` or after close MUST fail locally and send nothing.
- **FR-STR-12** Inbound `DATA` after the peer's `EOF` MUST reset that stream with
  `RST(PROTOCOL_ERROR)`. It does not end the connection.
- **FR-STR-13** Frames for an unknown or already-closed stream ID MUST be ignored. Late frames are
  normal after a local reset.
- **FR-STR-14** Sending `RST` MUST discard every frame still queued for that stream.
- **FR-STR-15** A closed stream MUST be removed from the registry. After 10 000 open/close cycles
  the registry and queues MUST be empty.

### 5.5 Outbound scheduling

- **FR-SCH-1** Control frames (`HELLO`, `OPEN`, `OPEN_OK`, `OPEN_ERR`, `RST`, `PING`, `PONG`,
  `GOAWAY`) MUST be sent before queued stream data, in the order they were queued.
- **FR-SCH-2** `DATA` and `EOF` MUST go through one queue per stream, so `EOF` never overtakes that
  stream's data.
- **FR-SCH-3** Streams with queued data MUST be served round-robin, one frame per turn.
- **FR-SCH-4** The library MUST split writes larger than `max_data_payload` into several `DATA`
  frames.
- **FR-SCH-5** Frames MUST be pulled by the host one at a time ("give me the next frame") so that
  the host can stop pulling while its transport buffer is above its own limit. Queueing therefore
  stays in the library, where order and fairness are enforced.

### 5.6 Local backpressure (replaces flow control)

- **FR-BP-1** Queued `DATA` bytes MUST be bounded per stream by `max_queued_bytes_per_stream`, and
  per session by `max_queued_bytes`. A write that does not fit MUST be partly or wholly refused,
  and the refused amount reported to the host. The library MUST NOT grow memory without bound.
- **FR-BP-1a** The per-stream limit keeps admission fair. One bulk stream cannot fill the whole
  session queue and so cause writes on other streams to be refused.
- **FR-BP-1b** Control frames do not count towards these limits. They are bounded by
  `max_queued_control_frames` instead, because a peer can trigger replies (`PONG`, `OPEN_ERR`,
  `RST`) without limit while it stops reading. The cap is checked after each inbound message, and
  going past it MUST end the connection with `LIMIT_EXCEEDED`. A configuration with a cap not above
  3 × `max_streams` MUST be refused, so normal host traffic cannot reach it.
- **FR-BP-2** When a stream's queue has room again, the library MUST tell the host that this
  stream is writable again (a "writable" event).
- **FR-BP-3** Inbound `DATA` MUST be handed to the host as it arrives. The library MUST NOT keep
  its own receive buffer: events borrow the buffer given to `onMessage` (FR-API-5). What the host
  may hold is bounded by credit (§5.18).

### 5.7 Keepalive

- **FR-KA-1** The library MUST send `PING` every `ping_interval` while a connection is up.
- **FR-KA-2** On `PING`, the library MUST answer with `PONG` carrying the same nonce.
- **FR-KA-3** The library MUST record the RTT from matching `PONG` frames and expose the latest
  value.
- **FR-KA-4** If no frame of any type is received for `dead_after`, the connection MUST be
  declared lost.
- **FR-KA-5** Unanswered pings MUST be tracked in bounded memory.

### 5.8 Session and connection lifecycle

- **FR-SES-1** A session MUST have at most one live connection at a time.
- **FR-SES-2** A session MUST outlive its connection. When the connection is lost, every open
  stream MUST end with `TRANSPORT_LOST`, and the session MUST report "disconnected". The host may
  later attach a new connection to the same session.
- **FR-SES-3** Opening a stream while no connection is attached MUST fail locally.
- **FR-SES-4** Closing a session MUST queue `GOAWAY(code)`, end all streams with that code (`OK`
  is reported to streams as `PEER_CLOSED`), and refuse new streams. The session MUST finish closing
  when the host has taken the frame, or after `drain_timeout`.
- **FR-SES-5** Receiving `GOAWAY(code)` MUST end all streams with that code (`OK` is reported to
  streams as `PEER_CLOSED`) and close the session.
- **FR-SES-6** The host MUST be able to list streams and reset one by ID (`RST(KILLED)` by
  default).
- **FR-SES-7** Per stream, the library MUST expose: ID, kind, opener role, state, bytes sent, bytes
  received, created-at and last-activity time (in the host-supplied clock), and the close reason
  once closed.

### 5.9 Errors

- **FR-ERR-1** A connection-level protocol error (FR-FRAME-4, FR-HS-2/3, FR-STR-4) MUST queue
  `GOAWAY(PROTOCOL_ERROR)`, let the host flush it, and then declare the connection lost. If the
  host has not taken the frame within `drain_timeout`, the connection MUST be declared lost anyway.
- **FR-ERR-2** A stream-level protocol error MUST reset only that stream. This covers every frame
  that is invalid for the stream's state (`docs/spec/PROTOCOL.md` §5.3), including
  FR-STR-12.
- **FR-ERR-3** All errors MUST be reported through return values (status codes or result types).
  The public API MUST NOT throw exceptions.

### 5.10 Host interface

- **FR-API-1** The library MUST be **sans-I/O**. It performs no I/O, starts no threads, and never
  reads a clock. The host supplies:
  - received messages,
  - the current time (a monotonic timestamp, in milliseconds),
  - application calls: open, accept/reject, write, EOF, reset, close.
- **FR-API-2** The library MUST hand back outputs only when the host asks for them: the next frame
  to transmit, and the next event (stream opened, data, EOF, stream closed, open accepted, writable,
  disconnected, session closed, ...). The library MUST NOT call into host code during an API call.
  This rules out re-entrancy hazards in every binding.
- **FR-API-3** The library MUST expose a deadline (the next time it needs a tick) so the host can
  set one timer, not poll.
- **FR-API-4** The library MUST provide a C++ API and a C ABI (`extern "C"`) built on top of it.
  The C ABI uses opaque handles, plain structs and explicit buffer-ownership rules.
- **FR-API-5** Ownership and lifetime of every buffer crossing the API MUST be documented. A buffer
  returned by `pollTransmit` MUST stay valid until the next call on the same session. Views in an
  event borrow the `onMessage` buffer that produced them, so the host MUST keep that buffer alive
  until `pollEvent` is drained, and MUST drain `pollEvent` before the next `onMessage`. The library
  MUST enforce this: `onMessage` with undrained events fails and changes nothing.
- **FR-API-6** Several sessions MUST be able to exist in one process. They MUST share no hidden
  global mutable state.
- **FR-API-7** The session MUST be the only public entry point for the protocol. The scheduler is
  an internal part of the session (NFR-MNT-4). The host MUST NOT have to move frames between
  components itself, and the public API MUST NOT expose the scheduler.

#### How a host uses the library (informative)

Names are illustrative. The M2 design note fixes the real API.

- A **session** is created once per peer relationship (for example, one per remote client). It
  lives across reconnects.
- A **stream** is created per logical connection (for example, one per `adb shell`). There are
  many streams in one session.

```
session = Session(config)                       // once per peer
session.attachConnection(now)                   // after the host's transport connects; queues HELLO
                                                // (again after every reconnect)

id = session.openStream("tcp-forward", params, now)     // many per session
session.acceptStream(id) / rejectStream(id, code, message)   // for streams the peer opened
session.write(id, bytes, now) → accepted byte count     // queued inside the session (FR-BP)
session.sendEof(id) / session.resetStream(id, code)

// host I/O loop, single-threaded per session (NFR-CON-1)
on transport message:     session.onMessage(msg, now), keeping msg until events are drained
on transport closed:      session.onConnectionLost()
on timer:                 session.onTick(now)
after every call:         repeat pollEvent until empty and pollTransmit while the transport has
                          room, until neither returns anything; then set a timer for
                          session.nextDeadlineMs()

session.close(code, now)                        // queues GOAWAY, ends every stream
```

Time rule: only calls that record time or arm a timer take `now`.

Inside the session:

```
Session ─ SessionStateMachine ─┬─ one class per session state
                               └─ SessionProcessor ─┬─ Scheduler (internal): control frames first,
                                                    │  then round-robin across the stream queues
                                                    └─ StreamManager: one state machine per stream
```

The session enforces the queue limits, using the scheduler's counters, so a future priority scheduler
does not have to repeat that logic.

### 5.12 SDK

- **FR-SDK-1** The SDK MUST be available in Python (`asyncio`) and in TypeScript (Promises), with the
  same concepts, the same operations and the same behaviour. Only naming and language idioms differ.
- **FR-SDK-2** The SDK MUST be the default import of each package. The synchronous binding stays
  available under a separate import for advanced use.
- **FR-SDK-3** A consumer MUST NOT have to pump events or frames, run a timer, run send or read loops,
  wait for `WRITABLE`, retry a partial write, pass the time, load WASM, or free native memory.
- **FR-SDK-4** The SDK MUST keep every host rule itself, so that no consumer can break one:
  - drain events after every core call;
  - keep event handling synchronous;
  - pull frames only while the transport has room (FR-SCH-5);
  - close the transport only after the send path has finished, because `SESSION_CLOSED` fires
    before `GOAWAY` is on the wire;
  - ignore anything a replaced connection reports, and never let its send loop take a frame after
    it has been replaced;
  - never call the core for a stream that has closed;
  - pass whole milliseconds from a monotonic clock.
- **FR-SDK-5** Every operation that can block MUST be cancellable in the language's own way (task
  cancellation in Python, `AbortSignal` in TypeScript). Cancelling MUST leave the session usable.
- **FR-SDK-6** Errors MUST be typed, and the same across languages: stream reset (with its reason
  code), stream closed, no connection, session closed, session not found, limit exceeded. No
  consumer should have to read a status code from the core.
- **FR-SDK-7** All SDK objects of one session MUST be used from one event loop (NFR-CON-1).
- **FR-SDK-8** The SDK MUST NOT change the wire protocol. An SDK peer and a peer built directly on a
  binding MUST interoperate.
- **FR-SDK-9** The SDK MUST NOT expose priority or flow control. If priority is added (ROADMAP.md
  §3), the consumer only states which priority a stream needs, and all scheduling stays in the core.

### 5.13 Transport interface

- **FR-TP-1** Each SDK MUST define one Transport interface. It is what a consumer implements, and it
  MUST have the same operations in every language: send a message, receive the next message, report
  the bytes buffered, wait until the buffer is below a limit, and close.
- **FR-TP-2** A Transport MUST deliver whole messages, reliably and in order. It MUST NOT lose a
  message that arrives before the SDK starts receiving.
- **FR-TP-3** After a Transport closes, whether cleanly or not, send, receive and wait MUST fail with
  one Transport-closed error, in each language. Close MUST be idempotent.
- **FR-TP-4** A Transport MUST declare the largest message it can carry. The SDK MUST refuse a
  Transport that cannot carry the session's largest frame, rather than fail in the middle of a
  transfer.
- **FR-TP-5** Each SDK MUST ship an in-memory Transport pair as public API. Tests MUST be able to
  stall it, release it and break it.
- **FR-TP-6** The TypeScript SDK MUST provide a helper for push-based sources (WebSocket, data
  channel), so that a consumer does not write the queue behind receive by hand.

### 5.14 Client

- **FR-CLI-1** A client MUST be created from a dial function. The call returns once the handshake
  has finished.
- **FR-CLI-1a** The SDK MUST generate the session ID, once per client, from a secure random source.
  A consumer MUST NOT be able to supply one (D21). The client MUST expose its ID to read, and MUST
  pass it to the dial function, because a consumer may need it to reach the peer (for example, in a
  URL).
- **FR-CLI-2** After a connection is lost, the SDK MUST call the dial function again, with backoff,
  on the same session. The backoff MUST reset only after a handshake finishes.
- **FR-CLI-3** The dial function MUST be able to stop reconnecting for good (for example, when
  credentials are refused). The client then closes with that error. Reconnect MUST also stop on
  `GOAWAY` and on a local close.
- **FR-CLI-4** Streams MUST NOT survive a reconnect: they end with `TRANSPORT_LOST` (FR-SES-2). A
  stream opened while reconnecting MUST wait for the connection, for at most `connect_timeout`, and
  then fail with a no-connection error.
- **FR-CLI-4a** If the server no longer has the session, the client MUST close with a
  session-not-found error, and reconnect MUST stop. The SDK MUST NOT open a replacement session by
  itself: the consumer's streams and any state tied to the session are gone, so it decides what to
  do, and connects again to get a new session with a new ID (D22).
- **FR-CLI-5** A consumer MUST be able to observe the connection state (connecting, connected,
  reconnecting, closed) and the active Transport. "Connected" MUST mean the handshake has finished.
  A failed handshake and a replaced connection MUST NOT be reported as a lost connection.
- **FR-CLI-6** Closing a client MUST send `GOAWAY`, wait for the send path within `drain_timeout`,
  close the Transport and release native memory. It MUST be idempotent, and it MUST work as
  `async with` in Python and as an awaited close in TypeScript.
- **FR-CLI-7** Cancelling `connect` MUST close the client and stop its dial loop, whether or not the
  handshake has finished.

### 5.15 Server

- **FR-SRV-1** A consumer MUST be able to hand each accepted Transport to a server object. The SDK
  then reads the first message, finds or creates the session, and runs the connection.
- **FR-SRV-2** The first message MUST be validated before any session is touched. It MUST be a valid
  `HELLO`, and it MUST arrive within `hello_timeout`. Otherwise the Transport is closed.
- **FR-SRV-3** An authorization hook MUST run before an existing session is touched, so that a peer
  that only knows a session ID cannot take over a live connection. Device locks, tokens and policy
  stay in the consumer.
- **FR-SRV-4** A re-dial on a session that still looks connected MUST replace the old connection,
  and MUST NOT start the grace period.
- **FR-SRV-5** A disconnected session MUST be kept for `grace_period` and then closed. Attaching a
  connection MUST cancel the timer.
- **FR-SRV-6** The number of sessions MUST be bounded by `max_sessions`. A new session over the limit
  MUST be refused without affecting existing ones.
- **FR-SRV-7** A consumer MUST be able to list sessions, and to close one with a reason code.
- **FR-SRV-8** The server MUST be available in both languages. In TypeScript it is meant for Node.
- **FR-SRV-9** A `HELLO` with `resuming` set for a session the server does not have MUST NOT create
  a session. After the authorization hook, the server MUST answer `HELLO`, then
  `GOAWAY(SESSION_NOT_FOUND)`, and close the Transport.
- **FR-SRV-10** A consumer MUST be able to register one callback that runs exactly once for each
  session that ends, after the session has left the registry. It receives the session and the
  reason code.
- **FR-SRV-11** A server session MUST expose its active Transport, under the same rule as FR-CLI-5.

### 5.16 Async streams

- **FR-AST-1** Either role MUST be able to open a stream with a kind and params, and write to it at
  once, without waiting for `OPEN_OK` (FR-STR-5). A consumer MUST also be able to wait until the peer
  has accepted.
- **FR-AST-2** Inbound streams MUST go to handlers registered by kind. A kind with no handler MUST be
  rejected with `UNSUPPORTED_KIND`. A handler MUST be able to reject a stream with a reason code. If
  a handler fails, its stream MUST be reset rather than left open.
- **FR-AST-3** Writing MUST wait until every byte is accepted, across `WRITABLE` events and however
  large the write. Concurrent writes on one stream MUST NOT interleave.
- **FR-AST-4** Reading MUST return the next chunk in order, and a distinct end-of-stream value once
  the peer has half-closed and all data has been read. A reset MUST fail the next read with its
  reason code, even if data is still buffered (D18). Async iteration MUST be supported.
- **FR-AST-5** The SDK stream MUST buffer received data, because the core does not (FR-BP-3). Credit
  bounds the buffer to `receive_window` (§5.18). A slow reader slows its own stream, and is not reset.
- **FR-AST-6** A stream MUST expose half-close, reset with a reason code, a way to wait until it has
  closed, and the details in FR-SES-7, plus the high-water mark of its read buffer.
- **FR-AST-7** All reads MUST go through one internal path that knows how many bytes the consumer has
  taken, and that path MUST report those bytes to the core (FR-FC-9).
- **FR-AST-8** A consumer MUST be able to declare a stream type: a kind plus typed params fields. The
  SDK then encodes and decodes the params, so neither side handles bytes or repeats the kind. The
  encoding is JSON. On the wire, params stay opaque bytes (FR-WIRE-5), and the core does not change.
  Raw `bytes` params MUST stay available, and they skip this and FR-AST-9.
- **FR-AST-9** The SDK MUST validate typed params with its own validator, with no schema library
  (NFR-SDK-2):
  - Outgoing params that do not match their type MUST fail locally, and nothing is sent.
  - Incoming params are untrusted (NFR-SEC-1). Params that do not match MUST reject the stream with
    `PROTOCOL_ERROR` before the handler runs, and the connection stays up.
  - Field types are string, int, float and bool, each optionally nullable, in a flat object. An int
    is a whole number within ±(2^53 − 1), so every language reads it the same way.
  - A missing field is rejected unless it has a declared default. A field the type does not declare
    is ignored, so that adding a field does not break an older peer.
  - Invalid UTF-8, invalid JSON, a value that is not an object, and the non-standard values `NaN`
    and `Infinity` are rejected.

### 5.17 Specs and conformance

- **FR-CONF-1** A protocol spec MUST define the wire format, the session and stream state machines
  and the error behaviour, in a form that does not depend on any language. It is authoritative: if
  the core disagrees with it, one of them has a bug, and it MUST be settled and recorded.
- **FR-CONF-2** A binding spec MUST define what a language has to provide: wrapping the C ABI, buffer
  ownership, error mapping, the SDK surface, the Transport interface, the host rules (FR-SDK-4), and
  the typed params encoding and validation rules (FR-AST-8, FR-AST-9).
- **FR-CONF-3** A conformance suite MUST live in `conformance/` as language-neutral data, with one
  small runner per language. It MUST have five layers:
  - **vectors:** frame encoding and decoding, including errors;
  - **sync:** scripted inputs and time, with the exact expected frames and events;
  - **params:** typed params cases, as declared fields plus input bytes, with the decoded value or a
    rejection expected. Every language MUST accept and reject the same inputs;
  - **async:** SDK scenarios over the in-memory Transport, asserting outcomes and not interleaving;
  - **transport:** checks that any Transport implementation can run against itself.
- **FR-CONF-4** Python and TypeScript MUST pass the whole suite. The C++ tests MUST run the vectors
  and sync layers.
- **FR-CONF-5** The transport layer MUST be available to consumers, so they can check their own
  Transport.
- **FR-CONF-6** Every bug found in a host MUST become a scenario. The suite MUST start with: a stale
  sender after re-dial, a lost `WRITABLE` when a queue is dropped, `on_connection_lost` after the
  session has ended, `EVENTS_PENDING` after `poll_transmit`, a re-dial on a live session, and a
  re-dial for a session the server no longer has.
- **FR-CONF-7** The suite MUST be versioned with the protocol. A protocol version bump MUST update the
  specs and scenarios with it (D8).
- **FR-CONF-8** The suite MUST be shown to catch bugs: deliberate bugs injected into a binding or an
  SDK MUST make it fail.

### 5.18 Credit flow control

- **FR-FC-1** Each side MUST announce `receive_window` in `HELLO`. It applies to every stream, for
  that connection.
- **FR-FC-2** A sender MUST NOT send `DATA` past its credit. A stream starts with the peer's
  `receive_window`, and gains what `WINDOW` grants.
- **FR-FC-3** Credit MUST be per stream. A stream without credit MUST NOT delay another stream.
- **FR-FC-4** Only `DATA` payload uses credit. `EOF` MUST stay behind its stream's data.
- **FR-FC-5** Data queued before a local close MUST keep receiving credit, and MUST still be sent.
- **FR-FC-6** `DATA` past the credit given MUST be a connection error with `PROTOCOL_ERROR`.
- **FR-FC-7** `WINDOW` that takes credit above the peer's `receive_window` MUST be the same error.
- **FR-FC-8** A receiver MUST grant credit once at least half of `receive_window` has been consumed
  and not yet granted, and only for data its consumer has taken. It need not grant after the peer's
  `EOF`.
- **FR-FC-9** The host MUST report consumed bytes. Reporting for a stream that has closed is not an
  error. Reporting more than was delivered MUST be refused.
- **FR-FC-10** `WINDOW` for a stream with nothing left to send MUST be ignored.
- **FR-FC-11** A sender MUST cut frames to at most half the peer's `receive_window`, so that a stream
  waiting for credit always leaves enough with the receiver to trigger a grant.
- **FR-FC-12** A write MUST accept nothing until the peer's `HELLO` has arrived, and the stream MUST
  then be told it is writable.
- **FR-FC-13** Each connection MUST use the window from its own `HELLO`.
- **FR-FC-14** Closing MUST NOT wait for credit.
- **FR-FC-15** A `receive_window` below 2 × `max_data_payload` MUST be refused as configuration.
- **FR-FC-16** A stalled reader MUST leave other streams working, however many streams have stalled.
- **FR-FC-17** Browser transports cannot be paused, so a browser cannot slow a peer by reading its
  socket more slowly. Credit is the only receive limit there, and it MUST NOT rely on the transport.

## 6. Non-functional requirements

### 6.1 Portability and dependencies
- **NFR-PORT-1** The library MUST depend only on the C++ standard library. It MUST NOT include OS
  or platform headers (`<sys/*>`, `<arpa/inet.h>`, `<windows.h>`, ...), compiler intrinsics, or
  third-party code.
- **NFR-PORT-2** Language standard: **C++20**.
- **NFR-PORT-3** The library MUST build with CMake (≥ 3.23) for: macOS (Clang, arm64 and x86_64),
  Linux (GCC and Clang, x86_64 and arm64), Windows (MSVC, x86_64), and WebAssembly (Emscripten,
  wasm32).
- **NFR-PORT-4** The library MUST NOT use `<thread>`, `<mutex>`, `<atomic>` or clock reads
  (`std::chrono::*_clock::now`).
- **NFR-PORT-5** The library SHOULD build and pass its tests with exceptions and RTTI disabled
  (`-fno-exceptions -fno-rtti`). This keeps the WASM build small.
- **NFR-PORT-6** Binding code (Python, JS) MAY use other dependencies. These dependencies MUST NOT
  leak into the core library target.
- **NFR-PORT-7** Tests use GoogleTest and `nlohmann/json`, fetched with CMake `FetchContent`. Both are
  linked only into test executables, never into the library.

### 6.2 Concurrency and determinism
- **NFR-CON-1** A session is not thread-safe. The host MUST serialize calls on one session.
  Different sessions MAY be used from different threads.
- **NFR-CON-2** For the same sequence of inputs (messages, times, calls), the library MUST produce
  the same sequence of outputs.

### 6.3 Robustness and security
- **NFR-SEC-1** All input from the peer is untrusted. Any byte sequence fed to the decoder or the
  session MUST NOT cause undefined behaviour, crashes, out-of-bounds access or unbounded memory
  growth.
- **NFR-SEC-2** The memory a peer can make the library use MUST be bounded by configuration:
  `max_streams`, payload limits, bounded ping tracking, no receive buffering (FR-BP-3).
- **NFR-SEC-3** The decoder SHOULD be fuzz-tested.
- **NFR-SEC-4** Tests SHOULD run under AddressSanitizer and UndefinedBehaviorSanitizer.

### 6.4 Performance
- **NFR-PERF-1** In a native build, the codec and scheduler SHOULD add negligible overhead
  compared with the transport. Target: at least 1 GB/s of 16 KB `DATA` frames through encode +
  schedule + decode on one core of a current laptop.
- **NFR-PERF-2** A `DATA` payload SHOULD be copied at most once on the send path inside the library
  (from the host write into the queue). No allocation per frame on the steady-state path is a
  SHOULD.
- **NFR-PERF-3** The WASM build SHOULD stay under 150 KB (stripped, `-Os`), not counting bindings.

### 6.5 Maintainability
- **NFR-MNT-1** Code MUST follow `STYLE.md` and `.clang-format`.
- **NFR-MNT-2** All code MUST be in namespace `packetmux`. C ABI symbols MUST be prefixed
  `packetmux_`.
- **NFR-MNT-3** Public headers MUST live under `include/packetmux/`. Implementation code MUST live
  under `src/`, with session code in `src/session/` and stream code in `src/stream/`. Tests MUST live
  under `tests/`. Language bindings MUST live under `bindings/<language>/`.
- **NFR-MNT-4** Inside the library, the stage that picks the next frame to send (the scheduler)
  MUST sit behind an internal interface. The boundary between the session and the transport MUST
  be a message interface (push received message, pull next message to send). The layers in `ROADMAP.md`
  can then be inserted without changing the session code.

### 6.6 SDK
- **NFR-SDK-1** The core MUST NOT change to suit one language. Logic that every SDK would otherwise
  repeat SHOULD move into the core, as long as the core stays sans-I/O.
- **NFR-SDK-2** The SDK MUST have no runtime dependencies beyond the Python standard library or the
  JS runtime, plus the core itself. Python 3.12 is the minimum. TypeScript MUST run in Node and in
  browser-like renderers, including Electron.
- **NFR-SDK-3** All memory a peer can make the SDK use MUST be bounded by configuration, as in
  NFR-SEC-2.
- **NFR-SDK-4** An application MUST be able to open a stream to a peer in about 10 lines, not
  counting its Transport. The README MUST show this as the main path.
- **NFR-SDK-5** Throughput through the SDK SHOULD stay within 10% of the hand-written RemoteDevice
  hosts on the same benchmark. Latency for a small stream during a bulk transfer MUST be measured,
  since it has not been yet.

## 7. Configuration and defaults

All values MUST be configurable. The first table is per session. The second is per SDK client or
server.

| Parameter | Default | Used by |
|---|---|---|
| `max_data_payload` | 16 000 B | FR-FRAME-3, FR-SCH-4 |
| `max_control_payload` | 32 KiB | FR-FRAME-3 |
| `max_streams` | 64 | FR-STR-6 |
| `max_queued_bytes` | 16 MiB | FR-BP-1 |
| `max_queued_bytes_per_stream` | 256 KiB | FR-BP-1 |
| `receive_window` | 256 KiB (D25) | FR-FC-1 |
| `max_queued_control_frames` | 1024 | FR-BP-1b |
| `max_framed_message` | 5 B + `max(max_data_payload, max_control_payload)` | FR-LPF-3 |
| `hello_timeout` | 10 s | FR-HS-4 |
| `ping_interval` | 15 s | FR-KA-1 |
| `dead_after` | 45 s | FR-KA-4 |
| `drain_timeout` | 1 s | FR-ERR-1, FR-SES-4 |
| `max_outstanding_pings` | 8 | FR-KA-5 |

Note: WebRTC messages are limited to 64 KiB (SCTP), so a header plus the largest payload must stay
below that.

| SDK parameter | Default | Used by |
|---|---|---|
| `inflight_limit` | 64 KiB | FR-SDK-4 |
| `reconnect_backoff` | 0.5, 1, 2, 4, 8 s, then 8 s | FR-CLI-2 |
| `connect_timeout` | 10 s | FR-CLI-4 |
| `grace_period` | 120 s | FR-SRV-5 |
| `max_sessions` | 1024 (D19) | FR-SRV-6 |

## 8. Known limitations (accepted for this version)

- **No priority.** Round-robin gives equal turns per stream. A latency-sensitive stream gets no
  preference over a bulk transfer, beyond taking its turn. See ROADMAP.md §3.
- **Head-of-line blocking.** With one transport over TCP or SCTP, packet loss stalls all streams.
  See ROADMAP.md §2.
- **Unknown progress after connection loss.** As in v1, a `TRANSPORT_LOST` stream gives no
  indication of which data reached the peer. See ROADMAP.md §2.
- **A stream speed cap on long paths.** A stream cannot go faster than `receive_window` / round trip,
  which is 12.8 MB/s at 20 ms with the default. A consumer can raise the window.
- **A writer that does not read can wait forever on an echoing peer.** Once the peer's replies fill
  the window, the peer stops reading. A consumer MUST read while it writes on such a stream.
- **Reconnect does not hide a lost connection.** The session survives, but its streams end with
  `TRANSPORT_LOST`, and the consumer opens new ones.

## 9. Verification

| Area | How it is verified |
|---|---|
| All C++ tests | GoogleTest (NFR-PORT-7), run in CTest |
| Codec (FR-WIRE, FR-FRAME) | Unit tests; **language-neutral golden vectors** (hex + expected fields) kept in this repo and run by the C++ tests and by every binding |
| Decoder robustness (NFR-SEC-1) | Fuzz target over `decode` and over session input; sanitizer builds |
| Stream and session behaviour | Scripted scenarios: input sequence → expected frames and events, with time injected |
| Scheduling (FR-SCH) | Ordering tests: OPEN before DATA, EOF after DATA, round-robin turns, RST discards queue |
| Backpressure (FR-BP) | Per-stream and per-session limits refuse writes; one full stream does not block writes on another |
| Framer (FR-LPF) | Every split point of a multi-message buffer yields the same messages; oversized length rejected |
| Leaks (FR-STR-15) | 10 000 open/close cycles end with empty registry and queues |
| Interop | Two library sessions back to back in memory; Python binding ↔ JS binding over a real WebSocket |
| Portability (NFR-PORT) | CI builds on every target in NFR-PORT-3, including Emscripten |
| SDK behaviour (FR-SDK, FR-CLI, FR-SRV, FR-AST) | The same async scenarios, run in Python and TypeScript over the in-memory Transport (FR-CONF-3) |
| Transport contract (FR-TP) | The transport layer of the suite, run against the in-memory Transport and a real WebSocket |
| Cross-language | SDK client and server, Python ↔ TypeScript in both directions, over a real WebSocket |
| Suite quality (FR-CONF-8) | Deliberate bugs injected into the binding and the SDK are caught |
| Specs are enough (Intent, other languages) | A fresh agent writes a small binding in a third language from the docs and the suite alone. Each time it has to read the C++ source is a gap in the specs, and is fixed |

## 10. Decisions

| # | Question | Decision (2026-10-30) |
|---|---|---|
| D1 | Frame type numbering without `WINDOW` | Renumber; no value reserved (FR-WIRE-8) |
| D2 | Test framework | GoogleTest via `FetchContent`, tests only (NFR-PORT-7) |
| D3 | Where bindings live | This repository, `bindings/<language>/` (NFR-MNT-3) |
| D4 | Byte-stream transports | Optional length-prefix framer in the library (§5.11) |
| D5 | Reconnect grace period | The core only keeps session state. The SDK runs the timer, and the consumer sets its length (§2.2, D13) |
| D6 | v1/v2 side by side | Not needed; backend and web frontend deploy together (FR-WIRE-7) |
| D7 | Flow control and priority | Not in v2 initial: round-robin plus bounded queues only (§5.5, §5.6) |
| D8 | How the protocol evolves | Keep each version minimal. Any wire change either raises the protocol version (v2 → v3) or, before a version is released, changes this spec and the code together. No in-band feature negotiation |
| D9 | Control payload encoding | Fixed layouts, no TLV (FR-WIRE-4a) |
| D10 | Delivery | In milestones, one agent task each (§11). Future layers are in `ROADMAP.md` |
| D11 | `kind` and `session_id` format | ASCII `[a-zA-Z0-9-]` only, not free UTF-8 (FR-WIRE-4b) |
| D12 | State machine structure | State pattern for sessions and streams: one class per state, handlers return the next state, and only the state machine applies it (`docs/design/M2_SESSION_CORE.md` §3) |

Added for the SDK (2026-10-31):

| # | Question | Decision |
|---|---|---|
| D13 | Where the async layer lives | In PacketMux, one SDK per language. This reverses `M4_BINDINGS.md`, which left it to the consumer |
| D14 | Import path | Async at the top level of each package, sync binding under a separate import (FR-SDK-2) |
| D15 | Reconnect | In the SDK, on by default, through the dial function. There is no attempt limit: it retries until closed, until the dial function gives up, or until the peer sends `GOAWAY` (FR-CLI-2, FR-CLI-3) |
| D16 | Which roles each SDK has | Client and server in both languages (FR-SRV-8) |
| D17 | Retired by D24. A read buffer past its limit | Reset that stream and leave the others alone. The default is 16 MiB, to be checked against real transfers before release (FR-AST-5, §8) |
| D18 | Data buffered when a reset arrives | The read fails at once and the buffer is dropped. A reset means abort, and both RemoteDevice hosts behave this way today (FR-AST-4) |
| D19 | SDK defaults | `reconnect_backoff`, `inflight_limit`, `connect_timeout` and `grace_period` keep RemoteDevice's current values. `max_sessions` is new, because nothing bounds sessions today, and 1024 is a starting value |
| D20 | Packaging | Installing the SDK MUST NOT need `PYTHONPATH` or an environment variable, or "one import" is not true. Python ships as a distributable wheel with the native library inside it, installable with `pip` and usable as a dependency in any `pyproject.toml`. TypeScript ships as an npm package with the WASM inside it. A macOS wheel and the npm package come with the SDK. Linux and Windows wheels follow when those builds exist, because a wheel with native code is built per platform |
| D21 | Who generates the session ID | Always the SDK. An ID chosen by a consumer is fragile: reusing one after a restart restarts the stream IDs on a live server session, which is a protocol error (FR-STR-2, FR-STR-4), and a guessable ID is easier to take over. A restarted client is a new session (FR-CLI-1a) |
| D22 | A re-dial for a session the server no longer has | It fails, and the consumer connects again for a new session. The server never recreates a session under an ID a client is resuming, so the client's stream ID state always matches a real peer. The two `HELLO`s look the same today, so `HELLO` gains `resuming`, and `SESSION_NOT_FOUND` is added. v2 is unreleased, so this changes v2 itself (D8) (FR-HS-7, FR-CLI-4a, FR-SRV-9) |
| D23 | Stream params in the SDK | Typed, declared once per language, and encoded as JSON, with validators of our own and no schema library. Raw `bytes` stay available. The wire stays bytes (FR-AST-8, FR-AST-9) |
| D24 | A slow reader | Credit flow control goes into v2, which is unreleased (D8). It replaces the read buffer reset of D17, because a real `adb push` of 200 MB failed with the 16 MiB default (M10). `max_read_buffer` is removed |
| D25 | The window | One `receive_window` per side, in `HELLO`, fixed for the connection. The default is 256 KiB, RemoteDevice's value and the largest tested that keeps the round trip beside a transfer under 1 ms (M10) |

## 11. Delivery milestones

Each milestone is one unit of work: a short design note in `docs/design/`, then code and tests.
A milestone is done when its exit criteria pass. The next milestone builds only on finished ones.

| # | Deliverable | Requirements | Exit criteria |
|---|---|---|---|
| M1 | Build skeleton and frame codec: CMake library target, GoogleTest setup, frame types, reason codes, encode/decode of every frame type, golden-vector files | FR-WIRE-\*, FR-FRAME-\*, NFR-PORT-1/2/4/5/7, NFR-MNT-\* | Codec unit tests and golden vectors pass on the native build |
| M2 | Stream and session core (C++ API): handshake, stream lifecycle, round-robin scheduler, bounded queues, keepalive, errors, events, time injection | FR-HS-\*, FR-STR-\*, FR-SCH-\*, FR-BP-\*, FR-KA-\*, FR-SES-\*, FR-ERR-\*, FR-API-1/2/3/5/6, NFR-CON-\*, NFR-SEC-1/2 | Scripted scenario tests pass; two sessions back to back in memory carry data both ways; 10 000 open/close cycles leave nothing behind |
| M3 | C ABI over the C++ API | FR-API-4/5, NFR-MNT-2 | A test compiled as C drives the full session and stream lifecycle through the ABI alone. Protocol details stay covered by the M2 tests |
| M4 | Bindings: Python (`bindings/python/`) and JS/WASM (`bindings/js/`); Emscripten build | NFR-PORT-3/6 | Golden vectors pass in both bindings; Python ↔ JS interop over a real WebSocket |
| M5 | Length-prefix framer, fuzzing, sanitizers, performance and size targets | FR-LPF-\*, NFR-SEC-3/4, NFR-PERF-\* | Framer split-point tests pass; fuzzers run clean; NFR-PERF targets measured and met (or the targets revised) |
| M6 | `HELLO.resuming` and `SESSION_NOT_FOUND` in the core; protocol spec, binding spec, and the vectors and sync layers of the conformance suite | FR-HS-7, FR-FRAME-5, FR-CONF-1/2/3/4/7 | The core and both bindings pass both layers from `conformance/` |
| M7 | SDK in Python and TypeScript: Transport, client, server, async streams, typed params, packaging; params, async and transport layers of the suite | FR-SDK-\*, FR-TP-\*, FR-CLI-\*, FR-SRV-\*, FR-AST-\*, FR-CONF-\*, NFR-SDK-\* | Both SDKs pass the whole suite; injected bugs are caught; Python ↔ TypeScript over a real WebSocket in both directions; the README example runs from an installed package |
| M8 | Third-language check | Intent, other languages | A small binding passes the vectors and sync layers from the docs alone, and every gap found is fixed in the specs |
| M9 | SDK gaps from the integration report: cancellation in Python `connect()`, a public "session ended" hook, the active transport on `ServerSession` | IMPROVEMENTS.md §3 | New async scenarios pass in both SDKs and fail when the change is removed |
| M10 | Benchmark in this repo: latency beside a bulk transfer, upload cost, read buffer high-water marks | IMPROVEMENTS.md §3 | Results recorded, with a proposed default window and the cause of the latency stated or narrowed down |
| M11 | Credit flow control | IMPROVEMENTS.md §3, ROADMAP.md §4 | See IMPROVEMENTS.md §3 |
| M12 | Consumer conveniences: socket pump, retryable refusal | IMPROVEMENTS.md §3 | Set when picked up |

The order is M6, M7, M9, M10, M11, M5, then M8 and M12 as needed (IMPROVEMENTS.md). M5 is a tuning
phase and can wait until the SDK is stable. Consumers move to the SDK after M7, and that work
belongs to each consumer. M5 and the Linux
and Windows builds have to finish before PacketMux goes into RemoteDevice's `main`: a native crash
now takes down the backend process, where it used to be a Python exception.

