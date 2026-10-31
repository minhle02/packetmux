# PacketMux binding specification

Status: authoritative. Level 1 is the binding, and level 2 is the SDK.

This document says what a language has to provide to use PacketMux. It assumes
`docs/spec/PROTOCOL.md`. The C header, `include/packetmux/PacketMuxC.h`, is the reference for exact
signatures.

Most languages should wrap the C ABI, not reimplement the protocol. A binding is thin: one call to one
`packetmux_*` function, plus memory handling.

## Getting started

Work in this order, because each step proves the one before it:

1. Load the library and call `packetmux_config_init`.
2. Wrap the codec, and pass the **vectors** layer of `conformance/`.
3. Wrap the session, and pass the **sync** layer.
4. Write the SDK (level 2), and pass the **params**, **async** and **transport** layers.

`conformance/README.md` explains how to write the runner.

## Level 1: binding

### 1.1 Loading

| Target | Library |
|---|---|
| macOS | `libpacketmux.dylib` |
| Linux | `libpacketmux.so` |
| Windows | `packetmux.dll` |
| WebAssembly | `packetmux.mjs`, `packetmux.wasm` |

A binding SHOULD let the library path be overridden, and otherwise look next to its own package.

### 1.2 Functions

| Group | Functions |
|---|---|
| Config | `config_init` |
| Lifetime | `session_create`, `session_destroy` |
| Connection | `session_attach_connection`, `session_on_message`, `session_on_connection_lost`, `session_on_tick`, `session_next_deadline_ms`, `session_close` |
| Streams | `session_open_stream`, `session_accept_stream`, `session_reject_stream`, `session_write`, `session_send_eof`, `session_reset_stream` |
| Output | `session_poll_transmit`, `session_poll_event` |
| Inspection | `session_state`, `session_get_rtt_ms`, `session_list_streams`, `session_stream_at` |
| Codec | `frame_decode`, `frame_encode` |

All of them start with `packetmux_`.

- Call `config_init` first, then set only the fields you need, so that defaults have one source.
- `session_create` copies the config, so its buffers can be released afterwards.
- `session_destroy` MUST be reachable. Where memory is not garbage-collected, such as WebAssembly, the
  host has to call it.
- `frame_encode` writes nothing, and returns `INVALID_ARGUMENT`, if the buffer is too small. Size the
  buffer as the sum of the field sizes plus 32.

### 1.3 Struct layout

`packetmux_config_t`, `packetmux_event_t`, `packetmux_stream_info_t`, `packetmux_frame_t` and
`packetmux_limits_t` cross the ABI by value or pointer. Offsets differ between targets: pointers are 4
bytes on wasm32. A binding that reads them by hand MUST check its offsets at build time, as
`bindings/js/LayoutCheck.cpp` does, so that a changed struct fails the build.

### 1.4 Buffers

| Buffer | Lifetime |
|---|---|
| Message passed to `on_message` | Keep it alive until `poll_event` returns false. Events borrow from it |
| `poll_transmit` result | Valid until the next call on that session |
| Event fields | Borrow the message that produced them |
| `kind` in `session_stream_at` | Valid until the next `session_list_streams` |

A binding SHOULD copy every byte out of events, so its users never see a borrowed buffer. If
`on_message` is refused with `EVENTS_PENDING`, the previous message is still in use, so it can only
be released after a call succeeds.

### 1.5 Errors

Each function returns a status. `OK` is 0. A binding raises or returns anything else, in the usual way
for its language, and keeps the status name.

`SESSION_STATUS` and `CODEC_STATUS` values come from the C header. A binding MUST check its constants
against the header in a test, so that a new value cannot be missed.

Peer errors are never a status. They arrive as events. A non-OK status always means the host misused
the call, and nothing changed.

### 1.6 Time

Time is passed in as whole milliseconds from a monotonic clock. A wall clock must not be used. A
language with floating-point time MUST round down, as a fraction can fail at the ABI boundary. 64-bit
values need care on targets without a 64-bit integer type.

## Host rules

A host that uses the binding directly MUST keep these rules. The SDK keeps them on the consumer's
behalf, which is why almost every consumer should use it.

| Rule | Why |
|---|---|
| After every session call, take events and frames until neither has any left | `poll_transmit` can queue `WRITABLE`, and `on_message` then refuses with `EVENTS_PENDING` |
| Keep event handling synchronous | Draining cannot pause halfway |
| Report a closed transport, always | It does nothing if the session already ended the connection |
| Close the transport only after the send path has finished | `SESSION_CLOSED` fires when `GOAWAY` is taken, before it is sent |
| Ignore anything a replaced connection reports | An old transport can still deliver a message or its close |
| Stop a replaced connection's send loop before it takes another frame | Otherwise it takes the new connection's frames, and they are lost on the old transport |
| Do not call the session for a stream that has closed | It returns `UNKNOWN_STREAM` |
| Pull frames only while the transport has room | Queueing stays in the session, which keeps order and fairness |
| Set one timer for `next_deadline_ms` | It changes after every call |

```
on transport message:  session.on_message(message, now)
on transport closed:   session.on_connection_lost()
on timer:              session.on_tick(now)
after each of those:   repeat
                         take every event, and handle it
                         take frames while the transport has room, and send them
                       until neither gave anything
                       set a timer for session.next_deadline_ms()
```

A server reads the first message of a new connection before it has a session: decode it, check it is
a `HELLO`, find the session by its ID (`PROTOCOL.md` §6.3), and then pass the same message to
`on_message`.

## Level 2: SDK

The SDK is what consumers use. It runs the host rules above on the language's own event loop, so
that a consumer implements a Transport and nothing else. `docs/design/M7_SDK.md` has the reasoning and
the pseudocode, and `bindings/python/packetmux/packetmux/` is the shortest reference implementation.

Names follow each language. Durations follow its habits: seconds in Python, milliseconds in
TypeScript. Everything else below MUST be the same.

### 2.1 Layers

| Layer | One per | Job |
|---|---|---|
| `SessionHost` | Session | Owns the core. Every core call goes through one path, which then drains events, re-arms the timer and wakes the sender |
| `Connection` | Transport | Runs the reader and sender loops |
| `Stream` | Stream | Read buffer, write loop |
| `Client`, `Server`, `ServerSession` | | Lifecycle, reconnect, grace period, session registry |

Event handling is synchronous. If handling an event needs a core call, such as a reject, make the call
and let the same drain pick up its events.

### 2.2 Transport

| Member | Requirement |
|---|---|
| `max_message_size` | The largest message it carries. The SDK refuses a Transport that cannot carry the largest frame |
| `send(message)` | Hands over one message. Accepted does not mean delivered. May wait. Refuses a message over the limit |
| `recv()` | The next whole message. A message that arrived before this call is not lost |
| `buffered_amount` | Bytes accepted and not yet on the wire |
| `wait_buffer_below(limit)` | Waits until `buffered_amount` is below `limit` |
| `close()` | Idempotent |

After a Transport closes, whatever the cause, `send`, `recv` and `wait_buffer_below` fail with
`TransportClosed`. After a close from this side, `recv` fails at once. After a close by the peer, what
has already arrived is read first.

A language where sources push MUST provide a helper that keeps the queue behind `recv`, as
`PushTransport` does. It MUST NOT depend on a drained-buffer event, because a browser `WebSocket` has
none.

Ship an in-memory pair as public test API, with controls to stall a side, release it, break the link,
and fail one side without telling the other. Ship the transport checks too, so that consumers can run
them on their own Transport.

### 2.3 Client

`connect(dial, handlers, capabilities, options)` generates the session ID from a secure random source,
calls `dial(session_id)`, and returns once the handshake has finished. A consumer cannot supply an ID.

| State | handshake done | connection lost | session closed | dial gave up | `close` | `open_stream` |
|---|---|---|---|---|---|---|
| `CONNECTING` | → `CONNECTED` | Next attempt | → `CLOSED` | → `CLOSED` | → `CLOSED` | Waits |
| `CONNECTED` | - | → `RECONNECTING` | → `CLOSED` | - | `GOAWAY`, → `CLOSED` | Opens |
| `RECONNECTING` | → `CONNECTED` | Next attempt | → `CLOSED` | → `CLOSED` | → `CLOSED` | Waits |
| `CLOSED` | - | - | - | - | - | Fails |

- Backoff starts at `reconnect_backoff[0]`, and the last value repeats. It resets only after a handshake
  finishes. After `connect` has returned, there is no attempt limit.
- The first `connect` retries too, and fails with `NoConnection` after `connect_timeout`.
- Cancelling `connect` closes the client and stops dialing, in the language's usual way of cancelling.
  If the handshake has already finished, the session closes too, because the caller has no client to
  close.
- A dial function stops reconnecting by raising `GiveUp`. The client closes with that error.
- `GOAWAY` closes the client. `SESSION_NOT_FOUND` gives `SessionNotFound`, and any other code gives
  `SessionClosed(code)`. The SDK MUST NOT open a replacement session by itself.
- `open_stream` waits for a connection for at most `connect_timeout`, then fails with `NoConnection`.
  It does not wait for `OPEN_OK`.
- `close` sends `GOAWAY`, waits for the send path, closes the Transport and releases native memory. It
  is idempotent.

Before attaching a dialed Transport, check its `max_message_size` against
`5 + max(max_data_payload, max_control_payload)`. Failing that check is permanent, so treat it as `GiveUp`.

### 2.4 Server

`serve(transport, context)` runs one connection, and returns when it ends:

1. Read the first message, for at most `hello_timeout`. Close the Transport if it does not arrive, does
   not decode, or is not a `HELLO`. No session has been touched.
2. Run `authorize`, with the session ID, capabilities, `resuming`, the consumer's `context`, and the
   existing session. It returns data, stored as `session.data`, or raises `Refuse(code)`.
3. Look the session up again, because `authorize` may have waited.
4. With no session: if `resuming`, refuse with `SESSION_NOT_FOUND`. Over `max_sessions`, refuse with
   `LIMIT_EXCEEDED`. Otherwise create it.
5. Cancel the grace timer, and attach. A live connection is replaced, and the replacement MUST NOT start
   the grace period.

To refuse, send `HELLO`, then `GOAWAY(code)`, then wait for the buffer within `drain_timeout`, and close.

On a lost connection that was not replaced, start the grace timer. When it fires, close the session
with `TIMEOUT`.

`on_session_closed(session, code)` is an optional server callback. It runs once for each session that
ends, however it ended, after the session has left `sessions()`. It is synchronous, and an exception
from it is logged and goes no further. A refused connection never made a session, so it does not run.

`session.transport` is the active Transport. Like `client.transport`, it is empty until the handshake
finishes and while disconnected, and it is never a replaced Transport.

### 2.5 Streams

- **Reading** returns the next chunk, or a distinct end-of-stream value once the peer has half-closed
  and everything has been read. A reset fails the next read, even with data still buffered.
- **The read buffer** is bounded by `max_read_buffer`. Past it, reset that stream with `LIMIT_EXCEEDED`.
- **Writing** waits until every byte is queued, across `WRITABLE` events. Concurrent writes on one
  stream run in order, and do not interleave. Clear the writable signal **before** the core call, or
  its `WRITABLE` is lost. Hand the core at most `max_queued_bytes_per_stream` per call.
- **Inbound streams** go to handlers, registered by kind or by stream type. A kind with no handler is
  rejected with `UNSUPPORTED_KIND`.

| The handler | The SDK then |
|---|---|
| Raises `Reject(code, message)` before accepting | Rejects with that code |
| Raises anything else before accepting | Rejects with `CONNECT_FAILED` |
| Returns without accepting | Rejects with `POLICY_DENIED` |
| Raises `Reject` after accepting | Resets with that code |
| Raises anything else after accepting | Logs it, and resets with `KILLED` |
| Returns after accepting | Sends `EOF` if the handler did not. The stream stays until the peer's `EOF` |

Accepting is explicit, so a handler can do async work first. Do not accept on the first write: a
handler that reads first would deadlock against a peer waiting for `OPEN_OK`.

### 2.6 Errors

The same names in every language, all extending `PacketMuxError`: `TransportClosed`, `StreamReset(code)`,
`StreamClosed`, `NoConnection`, `SessionClosed(code)`, `SessionNotFound` (extends `SessionClosed`),
`LimitExceeded`, `InvalidParams`. Raised by consumer code: `GiveUp`, `Reject(code, message)`,
`Refuse(code)`.

### 2.7 Typed params

A stream type is a kind plus declared fields, and the wire still carries opaque bytes. The encoding is
UTF-8 JSON: a flat object, with field names exactly as declared.

| Field type | Accepts |
|---|---|
| `string` | A JSON string |
| `bool` | `true` or `false`. A number is not a bool |
| `float` | A finite JSON number. A bool is not a number |
| `int` | A whole number within ±(2^53 − 1). `5037.0` is read as `5037` |

A type with `?` also accepts `null`. A field may declare a default, used when it is missing. A missing
field with no default is rejected, even if nullable. A field the type does not declare is ignored.

Reject invalid UTF-8, a byte-order mark, invalid JSON, anything that is not an object, and `NaN` or
`Infinity`. Languages disagree on these by default, so check each against the **params** layer.

Outgoing params that do not match fail locally with `InvalidParams`, and nothing is sent. Incoming params
that do not match reject the stream with `PROTOCOL_ERROR` before the handler runs. Raw bytes stay
available, and skip all of this.

### 2.8 Packaging

Installing the SDK MUST NOT need a path setting or an environment variable. Put the native library or
WASM inside the package.

### 2.9 Conventions

Generate the session ID from a secure source that exists in every context the language runs in. In a
browser, `crypto.randomUUID` needs a secure context, and `crypto.getRandomValues` does not.

Every blocking call MUST be cancellable in the language's own way, and cancelling MUST leave the session
usable.
