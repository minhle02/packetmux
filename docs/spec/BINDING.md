# PacketMux binding specification

Status: level 1 (binding) is authoritative. Level 2 (SDK) is written with M7.

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

Written with M7, from `docs/design/M7_SDK.md`.
