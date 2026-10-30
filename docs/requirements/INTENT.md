# PacketMux — Intent and Requirements

Status: draft, for review. Version of the wire protocol described here: **2**.

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

### 2.2 Out of scope (for this version)
- Wire transports (WebSocket, WebRTC, TCP, QUIC), TLS, authentication, device locking.
- Credit-based flow control (`WINDOW` frames).
- Priority classes and weighted scheduling. Planned as a separate layer (ROADMAP.md §3).
- A connection pool with more than one live connection per session.
- Retransmission, acknowledgements, or resuming streams across a reconnect. Planned as a separate
  layer (ROADMAP.md §2).
- Running stream handlers (dialing TCP targets, pumping sockets). This is host code.
- Management HTTP API, reconnect dialing, reconnect grace timers. These are host or service code.
  The library only keeps session state; the host decides how long a disconnected session lives.
- Compression and encryption of payloads.

## 3. Users of the library

| User | Platform | How it uses the library |
|---|---|---|
| RemoteDevice backend | Python on Linux, macOS, Windows | Native build, Python binding, server role |
| RemoteDevice remote client | Python, packaged with PyInstaller | Native build, Python binding, client role |
| RemoteDevice frontend | Electron renderer (browser JS) | WASM build, JS binding, client role |
| Future consumers | Any language with a C FFI | C ABI |

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
| `HELLO` | 0 | version, session_id, capabilities (opaque) | First frame in each direction on a new connection |
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
  `UNSUPPORTED_KIND`. An unknown reason code received from a peer MUST be treated as
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
  transitions are the table in `docs/design/M2_SESSION_CORE.md` §1.2.
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
  its own receive buffer: events borrow the buffer given to `onMessage` (FR-API-5).

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
  that is invalid for the stream's state (`docs/design/M2_SESSION_CORE.md` §1.2), including
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
- **NFR-PORT-7** Tests use GoogleTest, fetched with CMake `FetchContent`. GoogleTest is linked only
  into test executables, never into the library.

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

## 7. Configuration and defaults

All values MUST be configurable per session.

| Parameter | Default | Used by |
|---|---|---|
| `max_data_payload` | 16 000 B | FR-FRAME-3, FR-SCH-4 |
| `max_control_payload` | 32 KiB | FR-FRAME-3 |
| `max_streams` | 64 | FR-STR-6 |
| `max_queued_bytes` | 1 MiB | FR-BP-1 |
| `max_queued_bytes_per_stream` | 256 KiB | FR-BP-1 |
| `max_queued_control_frames` | 1024 | FR-BP-1b |
| `max_framed_message` | 5 B + `max(max_data_payload, max_control_payload)` | FR-LPF-3 |
| `hello_timeout` | 10 s | FR-HS-4 |
| `ping_interval` | 15 s | FR-KA-1 |
| `dead_after` | 45 s | FR-KA-4 |
| `drain_timeout` | 1 s | FR-ERR-1, FR-SES-4 |
| `max_outstanding_pings` | 8 | FR-KA-5 |

Note: WebRTC messages are limited to 64 KiB (SCTP), so a header plus the largest payload must stay
below that.

## 8. Known limitations (accepted for this version)

- **No per-stream receive backpressure.** Without credit flow control, a host can only slow a peer
  by pulling messages from its transport more slowly, and that stalls every stream on the
  connection. A slow consumer can delay other streams. Credit flow control is planned for a later
  protocol version (D8).
- **No priority.** Round-robin gives equal turns per stream. A latency-sensitive stream gets no
  preference over a bulk transfer, beyond taking its turn. See ROADMAP.md §3.
- **Head-of-line blocking.** With one transport over TCP or SCTP, packet loss stalls all streams.
  See ROADMAP.md §2.
- **Unknown progress after connection loss.** As in v1, a `TRANSPORT_LOST` stream gives no
  indication of which data reached the peer. See ROADMAP.md §2.

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

## 10. Decisions

| # | Question | Decision (2026-10-30) |
|---|---|---|
| D1 | Frame type numbering without `WINDOW` | Renumber; no value reserved (FR-WIRE-8) |
| D2 | Test framework | GoogleTest via `FetchContent`, tests only (NFR-PORT-7) |
| D3 | Where bindings live | This repository, `bindings/<language>/` (NFR-MNT-3) |
| D4 | Byte-stream transports | Optional length-prefix framer in the library (§5.11) |
| D5 | Reconnect grace period | Host decides; the library only keeps session state (§2.2) |
| D6 | v1/v2 side by side | Not needed; backend and web frontend deploy together (FR-WIRE-7) |
| D7 | Flow control and priority | Not in v2 initial: round-robin plus bounded queues only (§5.5, §5.6) |
| D8 | How the protocol evolves | Keep each version minimal. Any wire change either raises the protocol version (v2 → v3) or, before a version is released, changes this spec and the code together. No in-band feature negotiation |
| D9 | Control payload encoding | Fixed layouts, no TLV (FR-WIRE-4a) |
| D10 | Delivery | In milestones, one agent task each (§11). Future layers are in `ROADMAP.md` |
| D11 | `kind` and `session_id` format | ASCII `[a-zA-Z0-9-]` only, not free UTF-8 (FR-WIRE-4b) |
| D12 | State machine structure | State pattern for sessions and streams: one class per state, handlers return the next state, and only the state machine applies it (`docs/design/M2_SESSION_CORE.md` §3) |

## 11. Delivery milestones

Each milestone is one unit of work: a short design note in `docs/design/`, then code and tests.
A milestone is done when its exit criteria pass. The next milestone builds only on finished ones.

| # | Deliverable | Requirements | Exit criteria |
|---|---|---|---|
| M1 | Build skeleton and frame codec: CMake library target, GoogleTest setup, frame types, reason codes, encode/decode of every frame type, golden-vector files | FR-WIRE-\*, FR-FRAME-\*, NFR-PORT-1/2/4/5/7, NFR-MNT-\* | Codec unit tests and golden vectors pass on the native build |
| M2 | Stream and session core (C++ API): handshake, stream lifecycle, round-robin scheduler, bounded queues, keepalive, errors, events, time injection | FR-HS-\*, FR-STR-\*, FR-SCH-\*, FR-BP-\*, FR-KA-\*, FR-SES-\*, FR-ERR-\*, FR-API-1/2/3/5/6, NFR-CON-\*, NFR-SEC-1/2 | Scripted scenario tests pass; two sessions back to back in memory carry data both ways; 10 000 open/close cycles leave nothing behind |
| M3 | C ABI over the C++ API | FR-API-4/5, NFR-MNT-2 | The M2 scenarios pass again through a C-only test |
| M4 | Bindings: Python (`bindings/python/`) and JS/WASM (`bindings/js/`); Emscripten build | NFR-PORT-3/6 | Golden vectors pass in both bindings; Python ↔ JS interop over a real WebSocket |
| M5 | Length-prefix framer, fuzzing, sanitizers, performance and size targets | FR-LPF-\*, NFR-SEC-3/4, NFR-PERF-\* | Framer split-point tests pass; fuzzers run clean; NFR-PERF targets measured and met (or the targets revised) |

