# PacketMux — Roadmap (after v2)

Status: ideas, not requirements. Nothing here is part of protocol v2 (see `INTENT.md`).

Each item arrives with a new protocol version and its own requirements update (INTENT.md, D8).
INTENT.md NFR-MNT-4 keeps the internal boundaries open so these layers can be inserted.

## 1. Layering

```
  host application (streams)
            │
  ┌─────────▼─────────┐
  │ session           │  streams, IDs, OPEN/EOF/RST, keepalive          (v2)
  ├───────────────────┤
  │ scheduler         │  round-robin (v2) → priority (§3)
  ├───────────────────┤
  │ transmission ctrl │  sequence, ack, replay, reorder (§2)            (future)
  └─────────┬─────────┘
            │  messages
  host transport (WebSocket, WebRTC, framer + TCP)
```

## 2. Transmission control layer

Purpose: keep streams alive across a reconnect, and allow unordered transport channels.

- Per-frame sequence numbers, cumulative acknowledgements, and a bounded replay buffer of
  unacknowledged frames on each side. After a reconnect, each peer resends what the other has not
  acknowledged, and streams continue instead of failing with `TRANSPORT_LOST`.
- A reorder buffer, so the layer can run on an **unordered but reliable** channel (WebRTC data
  channel with `ordered: false`). A lost SCTP packet then delays only the frames behind it in
  sequence, not every message queued after it in the channel.
- Questions to settle when this is specified:
  - Reordering per stream (removes head-of-line blocking between streams) or per connection
    (simpler, but keeps it).
  - What happens when the replay buffer is full: block writers, or fail the stream.
  - Whether frames may be striped over several connections. This is where the pool (N > 1) gains
    capacity.
- Only **reliable** channels are planned. An unreliable channel would need this layer to provide
  retransmission and congestion control as well, which the transports already do.

## 3. Priority layer

Purpose: let latency-sensitive streams (scrcpy, live logcat) go ahead of bulk transfers.

- Replaces the round-robin scheduler (INTENT.md FR-SCH-3) behind the same internal interface
  (NFR-MNT-4). Candidate policy: the weighted deficit round-robin with aging from the RemoteDevice
  v1 design (§5.4 there).
- The stream's priority class is sent as a field in `OPEN`, so the acceptor schedules its replies
  with the same class.
- A consumer only states which priority a stream needs. All scheduling stays in the core
  (INTENT.md FR-SDK-9).
- Works only while the host keeps its transport buffer shallow, by pulling frames on demand
  (FR-SCH-5). Without credit flow control (§4), a slow consumer on the receiving side still delays
  all streams.

## 4. Credit-based flow control

Purpose: a slow consumer slows only its own stream, not the whole connection (INTENT.md §8).

- A `WINDOW` frame, which the receiver uses to grant send credit per stream, as in the RemoteDevice
  v1 protocol (§5.3 there).
- The receiver returns credit only after its consumer has taken the data. This brings back a
  bounded receive buffer per stream (it replaces FR-BP-3). The SDK already has that buffer, and
  already knows what the consumer took (FR-AST-5, FR-AST-7), so the credit logic can stay in the
  core and the consumer API does not change.
