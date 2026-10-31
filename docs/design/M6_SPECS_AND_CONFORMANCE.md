# M6 — Specs and conformance suite: design

Status: implemented. Exit criterion met on macOS arm64 (§7).
Requirements: `docs/requirements/INTENT.md` (FR-HS-7, FR-FRAME-5, FR-CONF-1/2/3/4/7, D8, D22).

M6 has three parts: a small wire change that M7 needs, two specs, and the first two layers of the
conformance suite. The other layers (params, async, transport) are built in M7, but their formats are
defined here, so the suite has one design.

## 1. Core change: `HELLO.resuming` and `SESSION_NOT_FOUND`

The server has to tell a new client from one coming back (D22). v2 is unreleased, so this changes v2
directly, with no version bump (D8).

### 1.1 Wire

```
HELLO payload:  version u16 | resuming u8 | session_id id16 | capabilities bytes32
```

```
wire = 01 00000000 0002 00 0002 7331 00000000
```

`version` stays at offset 0 forever. `resuming` is 0 or 1. Any other value is a malformed field, and
decoding fails with a new status, `BAD_FLAG`, because the existing statuses all mean something else.

`SESSION_NOT_FOUND` is reason code 11, appended (FR-WIRE-6). It is sent on the wire, in `GOAWAY`.

### 1.2 API

```cpp
struct FrameView {
  ...
  uint16_t version = PROTOCOL_VERSION;    // HELLO
  bool resuming = false;                  // HELLO
  std::string_view sessionId;             // HELLO
```

```c
typedef struct {
  packetmux_frame_type_t type;
  uint32_t stream_id;
  uint16_t version;
  bool resuming;                          /* new */
  packetmux_bytes_t session_id;
```

On wasm32, `resuming` lands at offset 10, in existing padding, so `packetmux_frame_t` keeps its size
and no other offset moves. `layout.js` and `LayoutCheck.cpp` each gain one entry.

| Binding | Change |
|---|---|
| Python | `Frame.resuming`, `Reason.SESSION_NOT_FOUND`, `CodecStatus.BAD_FLAG`, one `c_bool` field |
| JS | The same three, and `index.d.ts` |

`decode_frame` and `encode_frame` carry the flag, because a server reads it from the first message,
and because the refusal path in M7 encodes a `HELLO` by hand.

### 1.3 Wiring

The core sets the flag, so a host cannot get it wrong (FR-HS-7):

```cpp
void SessionProcessor::queueHello() {
  queueControl({.type = FRAME_TYPE_HELLO,
                .resuming = m_hasEstablished,
                .sessionId = m_config.sessionId,
                .capabilities = m_config.capabilities});
}

void EstablishedState::onEnter() {
  m_processor->markEstablished();         // m_hasEstablished = true, and it is never cleared
  m_processor->keepAlive().reset(now);
}
```

A retry after a handshake that never finished sends false. Both roles send the flag, and only a server
reads it. No session state changes, so the §1.1 transition table in `M2_SESSION_CORE.md` stays as it
is.

`validateConfig` measures a trial `HELLO`, and it has to count the extra byte, as does the fits check
in `Codec.cpp`.

### 1.4 What else this touches

| File | Change |
|---|---|
| Vectors | Every `HELLO` gains the byte. New: `hello_resuming`, `hello_bad_flag` |
| `rst_first_unknown_code` | **Uses `0x0b`, which becomes a real code.** Moves to `0x0c`, and a new vector covers `SESSION_NOT_FOUND` |
| `GoldenVectorsTest.cpp`, both binding vector tests | Read `resuming` |
| `M1_FRAME_CODEC.md`, `M3_C_ABI.md`, `M4_BINDINGS.md` | Layout, struct and status updates |

This breaks wire compatibility with anything built before it, and it fails loudly: an old `HELLO` is
read with a wrong identifier length, which is a protocol error. RemoteDevice's backend and frontend
have to be rebuilt together, as they do already (FR-WIRE-7).

## 2. Specs

Two new documents, in `docs/spec/`. `INTENT.md` keeps saying **what is required and why**. The specs
say **exactly how it behaves**, for someone implementing it.

### 2.1 `PROTOCOL.md`

Everything a reimplementation needs, with no reference to C++ (FR-CONF-1).

| Section | Source |
|---|---|
| Conventions, integers, identifiers, opaque fields | `INTENT.md` §5.1 |
| Frame layouts, byte by byte, with one example each | `M1_FRAME_CODEC.md` §2 |
| Decoding order and which error each check gives | `M1_FRAME_CODEC.md` §3 |
| Reason codes and frame types, with values | `M1_FRAME_CODEC.md` §1 |
| Session states and transition table | `M2_SESSION_CORE.md` §1.1 |
| Stream states and transition table | `M2_SESSION_CORE.md` §1.2 |
| Handshake, keepalive, `GOAWAY`, reconnect, `resuming` | `INTENT.md` §5.3, §5.7, §5.8 |
| Scheduling and queue limits that a peer can observe | `INTENT.md` §5.5, §5.6 |
| Limits and defaults | `INTENT.md` §7 |
| Versioning rule | D8 |
| Resolved disagreements | New: the record FR-CONF-1 asks for |

The tables **move** here. `M1` and `M2` keep their reasoning and point to the spec, so there is one
copy. `SessionLifecycleTest` then points to the spec as its oracle. Requirement IDs stay in `INTENT.md`,
and the spec cites them.

The framer (§5.11 in `INTENT.md`) has no section yet: its requirements were never written, and it
belongs to M5.

### 2.2 `BINDING.md`

What a new language has to provide (FR-CONF-2). It has two levels, and M6 writes only the first.

| Level | Contents | Written in |
|---|---|---|
| **1. Binding** | Loading the library; every C ABI function; config defaults; buffer ownership and lifetimes; copying event bytes; status mapping; time as whole milliseconds; the host loop and its rules | M6 |
| **2. SDK** | Transport interface; client, server, stream surface; error names; host rules enforced by the SDK; typed params encoding and validation | M7, from `M7_SDK.md` |

Level 1 also says how to get a runner passing, as a checklist: vectors, then sync, then the rest.

## 3. Conformance suite

### 3.1 Layout

```
conformance/
  README.md            # how to write a runner, in about a page
  manifest.json        # protocol version, and every file a runner must load
  vectors/frames.txt   # moved from tests/vectors/FramesV2.txt
  sync/*.json
  params/*.json        # M7
  async/*.json         # M7
  transport/checks.json# M7
```

```json
{
  "protocol_version": 2,
  "vectors": ["vectors/frames.txt"],
  "sync": ["sync/handshake.json", "sync/session_table.json", "..."]
}
```

Runners load files from the manifest, not by listing a directory, and each fails on a step it does
not know. A language cannot skip a case by accident. A check in CTest confirms that every file on
disk is in the manifest. `protocol_version` changes with the protocol (FR-CONF-7).

| Layer | Who runs it | Built in |
|---|---|---|
| vectors, sync | C++, Python, JS | M6 |
| params, async, transport | Python, JS | M7 |

### 3.2 Vectors layer

The format stays as it is (`M1_FRAME_CODEC.md` §7), plus a `resuming` field. Only the path changes.

### 3.3 Sync layer

One session under test, against a peer written in the script. Time is injected, so every run gives
the same result, and a scenario states the **exact** frames and events.

```json
{
  "scenarios": [
    {
      "name": "reset-discards-queued-data",
      "requirement": "FR-STR-14",
      "role": "client",
      "setup": "established",
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
  ]
}
```

**Input steps.** Each one maps to one session call:

| Op | Fields | Call |
|---|---|---|
| `attach` | | `attachConnection` |
| `rx` | `frame`, or `wire` (hex, for malformed input) | `onMessage` |
| `lost` | | `onConnectionLost` |
| `advance` | `ms` | Moves the clock. Calls nothing |
| `tick` | | `onTick` |
| `open` | `kind`, `params`, `expect_id` | `openStream` |
| `accept` | `stream` | `acceptStream` |
| `reject` | `stream`, `code`, `message` | `rejectStream` |
| `write` | `stream`, `data`, `expect_accepted` | `write` |
| `eof` | `stream` | `sendEof` |
| `reset` | `stream`, `code` | `resetStream` |
| `close` | `code` | `close` |

Any input step can add `"expect_status": "EVENTS_PENDING"`. Without it, the step must return OK.

**Check steps:**

| Op | Fields | Checks |
|---|---|---|
| `expect_frames` | `frames` | Polls until empty, and compares in order |
| `expect_events` | `events` | Polls until empty, and compares in order |
| `expect_state` | `state` | The session state |
| `expect_deadline` | `ms`, or `null` | `nextDeadlineMs`, relative to now |
| `expect_rtt` | `ms`, or `null` | The last RTT |
| `expect_streams` | `streams` | `listStreams`: id, kind, state, counters |
| `drain`, `drain_frames`, `drain_events` | | Polls without comparing, for what a scenario does not test |

**Rules that keep it strict:**

- A scenario must end with no frames and no events left to poll, so nothing can go unnoticed.
- Frames compare on the fields the script gives, plus `type` and `stream_id`. Events do the same.
- `data` is hex. `{"repeat": "ab", "count": 300000}` gives large writes without large files.
- The clock starts at 1000. A `nonce` in a `PING` from the session is compared if given.
- Config is optional, in snake_case: `"config": {"max_streams": 2}`.

**Setup** names a shared starting point, so that a scenario shows only what it tests. Setups live in
`sync/setups.json`, so runners do not repeat them:

| Setup | Steps |
|---|---|
| `disconnected` | None |
| `handshaking` | `attach`, then drain |
| `established` | `attach`, `rx` the peer's `HELLO`, then drain |
| `draining` | `established`, `rx` a malformed message, then drain the events only |
| `closing` | `established`, `close(OK)`, then drain the events only |
| `closed` | `established`, `lost`, `close(OK)`, then drain |

### 3.4 What goes in the sync layer

The aim is that a correct binding is proven by this layer alone. The C++ tests stay, because they
also cover internals such as the scheduler's counters.

| File | Covers | Source |
|---|---|---|
| `session_table.json` | Every cell of the session table: 42 scenarios | `SessionLifecycleTest` |
| `stream_table.json` | Every cell of the stream table | `StreamStateMachineTest` |
| `handshake.json` | `HELLO` rules, timeouts, `resuming` over several reconnects | `SessionLifecycleTest` |
| `streams.json` | Pipelining, half-close, reject, late frames, ID rules | `SessionStreamTest` |
| `scheduling.json` | Control first, round-robin, `EOF` after data | `SessionPairTest` |
| `backpressure.json` | Limits, partial writes, `WRITABLE` | `SessionBackpressureTest` |
| `keepalive.json` | Ping, dead peer, RTT | `SessionLifecycleTest` |
| `errors.json` | Protocol errors, `GOAWAY`, control flood | `SessionRobustnessTest` |
| `regressions.json` | Every bug found so far that a sync scenario can show (FR-CONF-6) | Below |

| Regression (FR-CONF-6) | Layer |
|---|---|
| Lost `WRITABLE` when a queue is dropped | sync |
| `on_connection_lost` after the session has ended | sync |
| `EVENTS_PENDING` after `poll_transmit` | sync |
| `resuming` false after a handshake that never finished | sync |
| Stale sender after re-dial | async (M7) |
| Re-dial on a live session | async (M7) |
| Re-dial for a session the server no longer has | async (M7) |

### 3.5 Runners

```python
# Python: tests/test_conformance.py. JS and C++ have the same shape.
def run(scenario):
    harness = Harness(role=scenario["role"], config=scenario.get("config", {}))
    for step in SETUPS[scenario.get("setup", "disconnected")] + scenario["steps"]:
        STEPS[step["op"]](harness, step)         # an unknown op raises KeyError, and fails
    assert harness.session.poll_transmit() is None
    assert harness.session.poll_event() is None
```

Target: under 200 lines each, since that is what the next language has to write. The harness keeps
the clock and the expected-status check. C++ runs through `Session`, and Python and JS through their
binding, which also exercises the C ABI.

The C++ runner parses JSON with `nlohmann/json`, fetched like GoogleTest and linked only into the test
executable (NFR-PORT-7).

### 3.6 Formats for the M7 layers

**Params:** defined in `M7_SDK.md` §10.

**Async.** Named actors over memory transports. Each runner sets up a client and a server, with a
handler for each `kind` in the scenario's `handlers`. Timeouts in `options` are kept short.

```json
{
  "name": "resume-after-the-server-lost-the-session",
  "requirement": "FR-CLI-4a",
  "handlers": {"echo": "echo"},
  "options": {"reconnect_backoff_ms": [10]},
  "steps": [
    {"op": "connect", "as": "client"},
    {"op": "forget_session", "client": "client"},
    {"op": "break", "client": "client"},
    {"op": "expect_closed", "client": "client", "error": "SessionNotFound"}
  ]
}
```

| Op | Meaning |
|---|---|
| `connect` | Connect a client through the memory dial function |
| `dial_fails` | The next `count` dials fail, or `give_up` raises `GiveUp` |
| `open`, `write`, `eof`, `reset`, `close` | The SDK call of the same name, on a stream or client named by `as` |
| `stall`, `release`, `break` | Transport controls: on `current` or `previous`, for a client |
| `redial` | Open a new transport while the old one still looks alive |
| `forget_session` | The server drops its session, as after a grace period or a restart |
| `expect_read` | The next read gives this data, or end of stream |
| `expect_error` | The next call on a stream fails with this error and code |
| `expect_state` | The client's state, after waiting for it, with a timeout |
| `expect_closed` | The client closed with this error |
| `expect_no_session` | The server has no session with this ID |
| `wait_grace` | Waits out the server's grace period |

Async scenarios assert outcomes and never interleaving, because asyncio and Promises schedule
differently. The memory transport's controls set up each race deterministically.

**Transport.** `transport/checks.json` lists named checks with their parameters. Each language
implements each check once as a function over a factory that returns a connected pair, and exposes
it to consumers in `packetmux.testing` (FR-CONF-5):

| Check | Requires |
|---|---|
| `order` | Whole messages arrive in order, at 80 sizes up to `max_message_size` |
| `both-directions` | Traffic flows both ways at once |
| `oversize` | A message over `max_message_size` is refused |
| `bulk` | 5 MB arrives intact, and the buffer drains to 0 |
| `backpressure` | `wait_buffer_below` waits while the buffer is full, and returns once it drains |
| `early-messages` | A message sent before `recv` is called is not lost |
| `close-fails-operations` | After a close, `send`, `recv` and the wait fail with the closed error |
| `peer-close` | A close on one side reaches the other |
| `close-is-idempotent` | A second close does nothing |

This list starts from RemoteDevice's `test_transport_conformance.py`.

## 4. Build and test changes

| Change | Detail |
|---|---|
| `tests/CMakeLists.txt` | `PACKETMUX_VECTORS_DIR` becomes `PACKETMUX_CONFORMANCE_DIR`; fetch `nlohmann/json`; add `ConformanceTest.cpp` |
| Python and JS tests | Vector path, new `test_conformance.py` and `conformance.test.js` |
| Manifest check | A CTest that fails if a file in `conformance/` is not in the manifest |
| `README.md` | Test paths, and one line pointing to `docs/spec/` |

## 5. Verification

| Test | Tool | Proves |
|---|---|---|
| `HELLO` layout, `resuming` over reconnects, `BAD_FLAG` | Vectors and sync | §1 |
| All sync and vector files | CTest, pytest, `node:test` | Exit criterion: the core and both bindings agree |
| Deliberate bugs in the core and the bindings | By hand, as in M2 to M4, with at least 10 | FR-CONF-8: the suite catches what the C++ tests catch |
| A spec read-through against the suite | Each scenario cites a requirement that the spec covers | No behaviour is tested but unspecified |
| Full existing suites | C++, Python, JS, plus UBSan | §1 broke nothing else |

ASan stays out of M6: it hangs before `main` on this Mac, and it belongs to M5.

## 6. Decisions made here

| # | Decision | Why |
|---|---|---|
| 1 | `resuming` is a whole byte, and values other than 0 and 1 are rejected | Fixed layouts have no tolerance for junk (FR-WIRE-4a). It also leaves room if a real flag is ever needed |
| 2 | New status `BAD_FLAG` | `BAD_PAYLOAD_SIZE` and `BAD_IDENTIFIER` would mislead whoever reads the log |
| 3 | The transition tables move to `PROTOCOL.md`, and `M1` and `M2` point to it | One copy cannot drift |
| 4 | Sync scenarios are JSON | Every language has a parser, and that matters most for the next language. C++ pays with one test-only dependency |
| 5 | Scenarios state exact frames and events, and nothing may be left over | The suite exists to catch small differences |
| 6 | The C++ tests stay, and are not replaced | They cover internals that the suite cannot see |
| 7 | `BINDING.md` level 2 waits for M7 | Writing it now would document an API that does not exist |

## 7. Verification results (2026-10-31)

Platform: macOS arm64, Apple Clang, Emscripten 6.0.6, Python 3.15.0, Node 24.18.1. The C++, UBSan, WASM,
Python and JS runs used a clean copy holding only the files git would track.

| Check | Result |
|---|---|
| Sync scenarios, in C++, Python and JS | 200 of 200 in each |
| C++ tests, Release with `PACKETMUX_WERROR=ON` | 186 of 186 |
| C++ tests, UBSan with libc++ hardening | 186 of 186 |
| Python tests | 280 of 280 |
| JS tests | 280 of 280 |
| Deliberate bugs in the core, against vectors and sync only | 19 of 19 caught |
| Deliberate bugs in the bindings, against vectors and sync only | 8 of 8 caught |
| WASM size | 41 KB, against a 150 KB target |

The first run of the sync layer failed 3 scenarios. All three were mistakes in the scenarios, not in
the core. Expected values were written from the tables and requirements, not from running the code.

The sync layer has no opinion on event order where the spec gives none, apart from what `core` does.
One scenario, `lost-writable-when-a-full-queue-is-dropped`, pins `WRITABLE` before `STREAM_CLOSED`.
Nothing requires that order, so a reimplementation of the protocol could fairly differ there. Every
current binding wraps the same core.

The Python and JS runners met the 200-line target, at 189 and 183 lines. The C++ runner is 489 lines,
because C++ has no reflection, so it spells out every enum name.

Not done:
- **The params, async and transport layers.** Their formats are in §3.6, and they are built in M7.
- **FR-CONF-5,** which makes the transport checks available to consumers. It comes with those layers.
- **ASan, Linux, Windows.** Unchanged, and they belong to M5.
