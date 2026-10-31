# PacketMux conformance suite

Language-neutral tests of `docs/spec/PROTOCOL.md`. The behaviour lives in these files, and each
language writes one small runner. Passing them is what "conforms" means.

| Layer | Files | Tests | Precision |
|---|---|---|---|
| vectors | `vectors/frames.txt` | Bytes to frames, and back | Exact bytes |
| sync | `sync/*.json` | One session against a scripted peer | Exact frames, events and time |
| params | `params/*.json` | Typed params validation | Same accept or reject |
| async | `async/*.json` | The SDK over a memory transport | Outcomes only |
| transport | `transport/checks.json` | Any `Transport` | Named checks |

Runners exist in `tests/ConformanceTest.cpp`, `tests/GoldenVectorsTest.cpp`,
`bindings/python/packetmux/tests/` and `bindings/js/test/`. Copy the closest one.

## Rules for a runner

- Load files from `manifest.json`, not by listing a directory.
- Fail on any step you do not recognise. Do not skip anything.
- Check that the expected number of cases ran.

Every bug found in a host becomes a scenario (FR-CONF-6). A protocol change updates this suite with
it, and `protocol_version` in the manifest (FR-CONF-7).

## Vectors

`frames.txt` is INI-like. Lines starting with `#` are comments, and hex values may contain spaces.

```
[hello_basic]
wire = 01 00000000 0002 00 0002 7331 00000000
type = HELLO
stream_id = 0
version = 2
session_id = 7331

[rst_unknown_code]
wire = 07 00000003 ff
type = RST
stream_id = 3
code = PROTOCOL_ERROR
roundtrip = false

[open_kind_bad_char]
wire = 02 00000001 0003 615f62 00000000
status = BAD_IDENTIFIER
```

| Key | Meaning |
|---|---|
| `wire` | The full message, in hex |
| `status` | Expected decode status. Default `OK` |
| `type`, `code` | Names |
| `stream_id`, `version`, `nonce`, `receive_window`, `credit` | Decimal. `receive_window` defaults to 262144 |
| `resuming` | `1` or `0`. Default `0` |
| `session_id`, `kind`, `capabilities`, `params`, `message`, `data` | Hex, so invalid identifiers can be written. Omitted means empty |
| `max_data_payload`, `max_control_payload` | Decimal limits for this vector. Omitted means default |
| `roundtrip` | Default `true`: encoding the fields must reproduce `wire`. `false` when decoding remaps a value |

For `status = OK`, check decoding against the fields and, when `roundtrip` is true, encoding against
`wire`. Otherwise, check the decode status only.

## Sync

A scenario runs one real session, with session ID `session-1`. The steps play the peer. The clock
starts at 1000 and moves only on `advance`, so a run is deterministic.

```json
{
  "name": "reset-discards-queued-data",
  "requirement": "FR-STR-14",
  "role": "client",
  "setup": "established",
  "config": {"max_streams": 2},
  "steps": [
    {"op": "open", "kind": "k", "expect_id": 1},
    {"op": "write", "stream": 1, "data": "aabb", "expect_accepted": 2},
    {"op": "reset", "stream": 1, "code": "KILLED"},
    {"op": "expect_frames", "frames": [
      {"type": "OPEN", "stream_id": 1, "kind": "k"},
      {"type": "RST", "stream_id": 1, "code": "KILLED"}
    ]},
    {"op": "expect_events", "events": [
      {"type": "STREAM_CLOSED", "stream_id": 1, "code": "KILLED"}
    ]}
  ]
}
```

To run one: create the session from `role` and `config`, run the steps of `setup` from
`sync/setups.json`, run `steps`, then check that no frames and no events are left.

`config` keys are the fields of `packetmux_config_t`, plus `capabilities` in hex and
`max_data_payload`.

### Input steps

Each makes one session call. It must return OK, unless the step has
`"expect_status": "<name>"`.

| Op | Fields | Call |
|---|---|---|
| `attach` | | `attach_connection` |
| `rx` | `frame`, or `wire` in hex | `on_message` |
| `lost` | | `on_connection_lost` |
| `advance` | `ms` | None. Moves the clock |
| `tick` | | `on_tick` |
| `open` | `kind`, optional `params`, `expect_id` | `open_stream` |
| `accept` | `stream` | `accept_stream` |
| `reject` | `stream`, `code`, optional `message` | `reject_stream` |
| `write` | `stream`, `data`, optional `expect_accepted` | `write` |
| `eof` | `stream` | `send_eof` |
| `reset` | `stream`, `code` | `reset_stream` |
| `close` | `code` | `close` |
| `consume` | `stream`, `bytes` | `consume` |

Keep the message from `rx` alive until its events are taken (`docs/spec/BINDING.md` §1.4). Do not take
events automatically after `rx`: the scenario decides, and some test `EVENTS_PENDING`.

### Check steps

| Op | Fields | Checks |
|---|---|---|
| `expect_frames` | `frames` | Polls until empty, and compares in order |
| `expect_events` | `events` | Polls until empty, and compares in order |
| `expect_state` | `state` | The session state |
| `expect_deadline` | `ms`, or `null` | `next_deadline_ms`, relative to now |
| `expect_rtt` | `ms`, or `null` | The last RTT |
| `expect_streams` | `streams` | `list_streams`, by ascending stream ID |
| `drain` | | Polls frames, then events, without comparing |
| `drain_frames` | | Polls frames without comparing |
| `drain_events` | | Polls events without comparing |

### Fields

Frames and events compare `type` and `stream_id`, which is 0 if omitted, plus any other field the
scenario gives. Fields it leaves out are not compared.

| Field | Format |
|---|---|
| `capabilities`, `params`, `message`, `data` | Hex. `data` may also be `{"repeat": "ab", "count": 300000}` |
| `kind`, `session_id`, `peer_session_id` | Text |
| `code`, `codec_status`, `state` | Names |
| `resuming` | Boolean |
| `receive_window`, `credit` | Numbers. In `rx`, `receive_window` defaults to 262144 |
| `nonce` | Number. In `rx`, `"last"` means the nonce of the last `PING` the session sent |

In `expect_streams`, each entry needs `stream_id`, and may give `kind`, `state`, `opener` (0 for
client, 1 for server), `bytes_sent`, `bytes_received` and `send_credit`.

## Params

`params/typed.json` has one set of `fields`, in the form that `define_stream` takes, and two lists.

```json
{
  "fields": {"host": "string", "port": "int", "ratio": "float?",
             "enabled": {"type": "bool", "default": true}},
  "encode_rejected": [{"name": "wrong-type", "value": {"host": 1, "port": 1, "ratio": null}}],
  "cases": [
    {"name": "valid", "input": "7b22...", "result": {"host": "a", "port": 1, "ratio": null, "enabled": true}},
    {"name": "invalid-utf8", "input": "7b22ff...", "rejected": true}
  ]
}
```

`input` is hex, so that invalid UTF-8 can be written. For each case, decode the input and compare the
value with `result`, or check that it fails with `InvalidParams`. For `encode_rejected`, check that
encoding fails with `InvalidParams`.

## Transport

`transport/checks.json` lists named checks, and each language implements each one over a factory that
returns a connected pair. Expose them to consumers (`check_transport`).

| Check | Requires |
|---|---|
| `order` | Whole messages arrive in order, at 80 sizes |
| `both-directions` | Traffic flows both ways at once |
| `oversize` | A message over `max_message_size` is refused |
| `bulk` | 5 MB arrives intact, and the buffer drains to 0 |
| `buffer-wait` | `wait_buffer_below` returns, while data is buffered and after it drains |
| `early-messages` | A message that arrives before `recv` is called is not lost |
| `close-fails-operations` | After a close, `send` and `recv` fail with `TransportClosed` |
| `peer-close` | A close reaches the other side |
| `close-is-idempotent` | A second close does nothing |

## Async

A scenario runs real SDK objects, with a memory network between them, and asserts **outcomes**, never
interleaving: asyncio and Promises schedule differently.

```json
{
  "name": "resume-after-the-server-lost-the-session",
  "requirement": "FR-CLI-4a",
  "handlers": {"echo": "echo"},
  "options": {"reconnect_backoff_ms": [1], "connect_timeout_ms": 500, "grace_period_ms": 5},
  "steps": [
    {"op": "connect"},
    {"op": "dial_hangs"},
    {"op": "break"},
    {"op": "expect_sessions", "count": 0},
    {"op": "dial_works"},
    {"op": "expect_closed", "error": "SessionNotFound", "code": "SESSION_NOT_FOUND"}
  ]
}
```

To run one: create a `Server` and a memory network, and register `handlers` on both the server and each
client. Run the steps. Then close everything, and fail if that takes more than a few seconds.

`options` may hold `connect_timeout_ms`, `reconnect_backoff_ms`, `grace_period_ms`, `max_sessions`,
and `session` for core settings. A scenario may set `authorize` to `refuse:<code>`,
or to `owner`: the first context owns the session, and any other is refused with `AUTH_REVOKED`.
Every server has `on_session_closed` set. It checks that the session has left `sessions()` and records
the code. A scenario may set `session_hook` to `raise`, and the callback then raises after recording.

Handlers take these behaviours. Where a kind maps to an object, it has `behaviour` and `fields`, and the
handler is registered by stream type.

| Behaviour | The handler |
|---|---|
| `echo` | Accepts, writes back everything it reads, then sends `EOF` |
| `reject:<code>` | Raises `Reject` |
| `fail` | Raises an error |
| `accept-then-fail` | Accepts, then raises an error |
| `return` | Returns without accepting |
| `reply:<hex>` | Accepts, writes, and returns |
| `reply-then-reset:<hex>` | Accepts, writes, waits 20 ms, and resets with `KILLED` |
| `flood:<bytes>` | Accepts, writes that many bytes, and waits |
| `record-close` | Accepts, and records the reason the stream closed with |
| `reply-params` | Accepts, and writes `host:port` from typed params |
| `reply-raw-params` | Accepts, and writes its raw params back |

Streams and clients are named with `as`, and the default client is `client`. `on` picks who opens a
stream: a client's name, or `server` for its first session.

**Steps on clients and streams:**

| Op | Fields | Does |
|---|---|---|
| `connect` | `as`, optional `expect_error`, `expect_code` | `connect` through the network |
| `open` | `kind`, `as`, optional `on`, `params`, `background`, `expect_error` | `open_stream` |
| `open_typed` | `kind`, `as`, `fields`, `value`, optional `expect_error` | `open_stream` with typed params |
| `write` | `stream`, `data`, optional `background` | `write` |
| `write_concurrently` | `stream`, `writes` | Starts every write at once, in order |
| `eof` | `stream`, optional `background` | `send_eof` |
| `reset` | `stream`, `code` | `reset` |
| `close` | optional `client`, `code`, `background` | `close` |
| `wait_accepted` | `stream` | `wait_accepted` |
| `cancel_read` | `stream` | Starts a read, cancels it after 5 ms |
| `cancel_connect` | | Starts `connect`, cancels it after 5 ms, and checks it ended as cancelled |
| `await` | `task`, optional `error`, `code` | Waits for a `background` step |
| `settle` | optional `ms`, default 20 | Sleeps |

**Steps on the network and server:**

| Op | Fields | Does |
|---|---|---|
| `dial_fails` | `count` | The next dials fail |
| `dial_gives_up` | | Every dial raises `GiveUp` |
| `dial_hangs`, `dial_works` | | Dials wait, then go on |
| `stall`, `release` | `side`, optional `link` | Holds one side of a link, or lets it go |
| `break` | optional `link` | The link dies on both sides |
| `drop` | optional `side`, `link` | That side sees it die, and the other does not. Default `client` |
| `inject` | `side`, `link`, `wire` | A message arrives on that side, from nobody |
| `small_transport` | `max_message_size` | Later dials give transports that size |
| `set_context` | `value` | The context for later connections |
| `close_session` | `code` | Closes the first server session |
| `raw_connect` | optional `wire` | A transport with no client on it, sending those bytes |
| `raw_hello` | `session_of`, `resuming` | The same, sending a `HELLO` with a client's session ID |
| `record_states` | | Starts recording client states |
| `record_server_close` | | Starts recording the code the session closes with |
| `dial_count` | | Remembers how many dials there have been |

`side` is `client` or `server`. `link` is `current`, the default, or `previous`.

**Checks.** Those that wait fail after a few seconds:

| Op | Fields | Checks |
|---|---|---|
| `expect_state` | `state`, optional `client` | Waits for a client state |
| `expect_states` | `states` | The recorded states |
| `expect_connected` | | The client is connected now |
| `expect_closed` | optional `error`, `code` | Waits for the client to close, with that error, or none |
| `expect_read` | `stream`, `data` | Reads until that many bytes have arrived, and compares |
| `expect_bytes` | `stream`, `count` | Reads until that many bytes have arrived, ignoring their value |
| `expect_eof` | `stream` | The next read is the end of stream |
| `expect_error` | `stream`, `call`, `error`, optional `code` | `read`, `write` or `wait_accepted` fails that way |
| `expect_stream_closed` | `stream`, `code` | `wait_closed` gives that reason |
| `expect_pending` | `task` | A `background` step has not finished |
| `expect_high_water` | `stream`, `value` | The read buffer high-water mark |
| `expect_stream_info` | `stream`, `send_credit`, `read_high_water` | `Stream.info()` and the client's `streams()` both report these |
| `expect_handler_saw` | `code` | `record-close` saw that reason |
| `expect_sessions` | `count` | Waits for that many server sessions |
| `expect_session_disconnected` | | Waits for the first session to disconnect |
| `expect_server_close` | `code` | The recorded close code |
| `expect_session_closed` | `code` | Waits for `on_session_closed`. It ran once, with that code |
| `expect_no_session_closed` | | `on_session_closed` has not run |
| `expect_session_transport` | `is` | Waits for the first session's Transport: `current`, the server side of the latest link, or `none` |
| `expect_dials` | `min` | Waits for that many dials |
| `expect_no_new_dials` | | No dials since `dial_count` |
| `expect_session_ids_differ` | `a`, `b` | Two clients have different, valid IDs |
| `expect_dial_ids_match` | `client` | The dial function got that client's ID |
| `expect_raw_closed` | | The raw transport was closed |
