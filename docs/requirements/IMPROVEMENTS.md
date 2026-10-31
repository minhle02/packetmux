# PacketMux — Improvement roadmap

Status: approved triage. M9 is done, apart from its removal check. Source: the RemoteDevice integration report of 2026-10-31,
on PacketMux `b8ce10a` and RemoteDevice `packetmux-integration` at `63c9be0`. Numbers in the first
column below are the report's request numbers.

This file is the plan. Requirements still go in `INTENT.md`, and each milestone gets its own design
note in `docs/design/`. `ROADMAP.md` stays the list of layers after v2.

## 1. Decisions on the requests

| # | Request | Decision | Where |
|---|---|---|---|
| 1 | Per-stream credit flow control | Do | M11 |
| 2 | Latency during bulk transfer | Measure, then fix with credit | M10, M11 |
| 3 | Browser upload cap | Deferred | §5 |
| 4 | Linux and Windows wheels | Deferred | §5 |
| 5 | Cancellation in Python `connect()` | Do | M9 |
| 6 | Upload cost of the SDK | Measure | M10, M11 |
| 7 | Public "session ended" hook on the server | Do | M9 |
| 8 | Active transport on `ServerSession` | Do | M9 |
| 9 | Socket-to-stream pump | Later | M12 |
| 10 | Option to start a new session after `SessionNotFound` | Declined | §5 |
| 11 | Retryable refusal | Later | M12 |
| 12 | Reset by stream ID on the server | Declined | §5 |
| 13 | Verify optimised output in `build.py` | Deferred | §5 |
| 14 | Note in `INTENT.md` that browser transports cannot be paused | Do | M11 |

## 2. Order

```
M9 SDK gaps ──► M10 measure ──► M11 credit flow control ──► M5 ──► M12, M8 as needed
```

- M9 comes first because it is small and does not depend on the rest.
- M10 comes before M11 because its results set the default window, and because credit changes the
  numbers. A baseline cannot be taken afterwards.
- M11 comes before M5 so that fuzzing covers the final wire format, including `WINDOW`.

## 3. Milestones

### M9: SDK gaps

Small API additions, in Python and TypeScript together, so the SDKs stay level.

- **Request 5:** cancelling Python `connect()` closes the client and stops the dial loop. Today only
  `TimeoutError` is handled, so a cancelled call leaves the loop retrying forever. The JS client
  already takes a `signal`.
- **Request 7:** a public way to learn that a server session ended, on `ServerSession` or `Server`.
  This replaces the sweep over `server.sessions()` in RemoteDevice's `TokenWatcher`.
- **Request 8:** the active Transport on `ServerSession`, matching `Client.transport`.

Exit criteria: new scenarios in the async conformance layer pass in both SDKs, and fail when the
change is removed. The binding spec and both binding READMEs describe the new API.

### M10: Measure

A benchmark in this repo, over loopback WebSocket, so results do not depend on another repo. It takes
the numbers below for M11 to use, and changes no library behaviour.

- **Request 2:** round-trip time on a second stream during a bulk transfer, in three setups: full
  speed, bulk rate capped at 360 MB/s, and small socket buffers. The extra 1.7 ms in the report equals
  about 1 MB of queue at that speed. The capped run shows whether the delay comes from queues or from
  a saturated event loop.
- **Request 6:** upload through the SDK against a thin adapter over the sync core, with the two
  versions run alternately. The report's 544 against 447 to 467 MB/s came from runs that were not
  alternated.
- **Window input:** `read_high_water` during those runs, which `streams()` already reports.

Exit criteria: the results are recorded in the M10 design note, with a proposed default window and a
stated cause for the latency, or a statement of which causes were ruled out.

### M11: Credit flow control

A slow consumer slows only its own stream. A `WINDOW` frame grants send credit per stream, and the
receiver grants it only after the consumer has taken the data (`ROADMAP.md` §4).

This is the one wire change here. v2 is not released, so `WINDOW` goes into v2 directly (D8), as
`HELLO.resuming` did. All three peers of the RemoteDevice branch must then move together.

To settle in the design note:

1. How a receiver announces its window. The two sides may be configured differently, so one side
   cannot assume the other's value.
2. The default window, from M10.
3. When to send a grant, so that small reads do not produce one frame each.
4. What counts against credit. The starting point is `DATA` payload only, so `EOF`, `RST` and control
   frames always pass.
5. A peer that sends past its credit: a connection error, like other protocol violations.
6. What replaces FR-BP-3. The core needs a call that reports consumed bytes, which FR-AST-7 already
   provides for in the SDK.
7. What happens to `max_read_buffer` and D17. A stalled reader now stalls its stream and is not reset.
   RemoteDevice can then remove its 1 GiB setting.
8. How a closing session behaves when queued data has no credit. `drain_timeout_ms` must still end it.
9. **Request 14:** browser transports cannot be paused, so credit is the only receive limit there.
   This goes in `INTENT.md`.

Work spans the spec, core, C ABI, both bindings, both SDKs and every conformance layer that sees
`DATA`.

Exit criteria:

- The conformance suite passes in C++, Python and TypeScript, including new vectors and scenarios for
  `WINDOW`, and injected bugs in the credit logic are caught.
- A stalled reader leaves other streams on the session running, and memory stays within the window.
- Python ↔ TypeScript passes over a real WebSocket in both directions.
- M10 is repeated and compared with its baseline.
- The RemoteDevice harness is run again from that repo, on its own branch, and meets the targets
  below.

Proposed targets on the report's harness, to confirm once M10 is done:

| Metric | Native | SDK today | Target |
|---|---|---|---|
| Round trip during download, p50 | 0.66 ms | 2.35 ms | 1.0 ms or less |
| Download throughput | 362 MB/s | 583 MB/s | Not below native |
| Upload throughput | 307 MB/s | 457 MB/s | Not below native |

If credit does not reach the latency target, the cause from M10 gets its own fix before M5.

### M12: Consumer conveniences

Picked up when wanted.

- **Request 9:** a socket-to-stream pump, with half-close and reset handling.
- **Request 11:** a refusal that lets the client retry. `Refuse` is final today, so a temporary
  device lock stays outside it. A `dial` that raises already retries.

## 4. After this roadmap

M5 keeps its scope: framer, fuzzing, sanitizers, performance and size targets.

## 5. Deferred and declined

| # | Request | State | Reason |
|---|---|---|---|
| 3 | Browser upload cap | Deferred | When picked up, a flat higher default is the wrong fix. 1 MiB on a 10 Mbit/s uplink can hold a keystroke behind about 0.8 s of queue, so the limit should grow only while the buffer drains between ticks |
| 4 | Linux and Windows wheels | Deferred | Needs a git remote with CI runners. `INTENT.md` §11 still requires these builds before RemoteDevice's `main` |
| 13 | Verify optimised output | Deferred | The current build is fine, and `a68193b` fixed one possible cause of the slow dylib, mixed Debug and Release objects |
| 10 | New session after `SessionNotFound` | Declined | D22: restarting silently would hide the loss of every open stream |
| 12 | Reset by stream ID | Declined | Nothing uses it. RemoteDevice removed its route, and the UI never called it |
