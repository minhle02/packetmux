# PacketMux conformance suite

Language-neutral tests of `docs/spec/PROTOCOL.md`. The behaviour lives in these files, and each
language writes one small runner. Passing them is what "conforms" means.

| Layer | Files | Tests | Precision |
|---|---|---|---|
| vectors | `vectors/frames.txt` | Bytes to frames, and back | Exact bytes |
| sync | `sync/*.json` | One session against a scripted peer | Exact frames, events and time |
| params | `params/*.json` (M7) | Typed params validation | Same accept or reject |
| async | `async/*.json` (M7) | The SDK over a memory transport | Outcomes only |
| transport | `transport/checks.json` (M7) | Any `Transport` | Named checks |

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
| `stream_id`, `version`, `nonce` | Decimal |
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
| `nonce` | Number. In `rx`, `"last"` means the nonce of the last `PING` the session sent |

In `expect_streams`, each entry needs `stream_id`, and may give `kind`, `state`, `opener` (0 for
client, 1 for server), `bytes_sent` and `bytes_received`.
