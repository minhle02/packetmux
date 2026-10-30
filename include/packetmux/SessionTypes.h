#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "packetmux/Frames.h"
#include "packetmux/definitions/SessionDef.h"

namespace packetmux {

enum SessionStatus : uint8_t {
  SESSION_STATUS_OK = 0U,
  SESSION_STATUS_CLOSED,             // the session has ended
  SESSION_STATUS_NOT_CONNECTED,      // no connection attached
  SESSION_STATUS_ALREADY_CONNECTED,  // attachConnection while one is attached
  SESSION_STATUS_EVENTS_PENDING,     // onMessage before pollEvent was drained
  SESSION_STATUS_UNKNOWN_STREAM,
  SESSION_STATUS_INVALID_STATE,   // e.g. write after sendEof
  SESSION_STATUS_LIMIT_EXCEEDED,  // max streams reached
  SESSION_STATUS_IDS_EXHAUSTED,
  SESSION_STATUS_INVALID_ARGUMENT,  // bad kind, oversized field, bad config
  SESSION_STATUS_MAX,
};

enum Role : uint8_t {
  ROLE_CLIENT = 0U,
  ROLE_SERVER,
  ROLE_MAX,
};

/**
 * Labels for reporting only. Transition logic lives in the state classes.
 */
enum SessionState : uint8_t {
  SESSION_STATE_DISCONNECTED = 0U,
  SESSION_STATE_HANDSHAKING,
  SESSION_STATE_ESTABLISHED,
  SESSION_STATE_DRAINING,
  SESSION_STATE_CLOSING,
  SESSION_STATE_CLOSED,
  SESSION_STATE_MAX,
};

/**
 * Labels for reporting only. The sender is the side that sent OPEN, and the
 * receiver is the side that received it.
 */
enum StreamState : uint8_t {
  STREAM_STATE_SENDER_OPENING = 0U,
  STREAM_STATE_SENDER_OPENING_EOF_SENT,
  STREAM_STATE_RECEIVER_OPENING,
  STREAM_STATE_RECEIVER_OPENING_EOF_RECEIVED,
  STREAM_STATE_OPEN,
  STREAM_STATE_HALF_CLOSED_LOCAL,
  STREAM_STATE_HALF_CLOSED_REMOTE,
  STREAM_STATE_CLOSED,
  STREAM_STATE_MAX,
};

enum EventType : uint8_t {
  EVENT_TYPE_UNKNOWN = 0U,
  EVENT_TYPE_CONNECTED,        // peerSessionId, capabilities
  EVENT_TYPE_STREAM_OPENED,    // streamId, kind, params (the peer opened it)
  EVENT_TYPE_STREAM_ACCEPTED,  // streamId (the peer accepted ours)
  EVENT_TYPE_DATA,             // streamId, data
  EVENT_TYPE_EOF,              // streamId
  EVENT_TYPE_WRITABLE,         // streamId
  EVENT_TYPE_STREAM_CLOSED,    // streamId, code (OK only for a clean close)
  EVENT_TYPE_DISCONNECTED,     // code, codecStatus
  EVENT_TYPE_SESSION_CLOSED,   // code
  EVENT_TYPE_MAX,
};

struct SessionConfig {
  Role role = ROLE_CLIENT;
  std::string sessionId;
  std::vector<uint8_t> capabilities;
  FrameLimits limits;
  uint32_t maxStreams = DEFAULT_MAX_STREAMS;
  size_t maxQueuedBytes = DEFAULT_MAX_QUEUED_BYTES;
  size_t maxQueuedBytesPerStream = DEFAULT_MAX_QUEUED_BYTES_PER_STREAM;
  size_t maxQueuedControlFrames = DEFAULT_MAX_QUEUED_CONTROL_FRAMES;
  uint64_t helloTimeoutMs = DEFAULT_HELLO_TIMEOUT_MS;
  uint64_t pingIntervalMs = DEFAULT_PING_INTERVAL_MS;
  uint64_t deadAfterMs = DEFAULT_DEAD_AFTER_MS;
  uint64_t drainTimeoutMs = DEFAULT_DRAIN_TIMEOUT_MS;
  size_t maxOutstandingPings = DEFAULT_MAX_OUTSTANDING_PINGS;
};

/**
 * One event, as plain fields. Only the fields of the event's type are set.
 * Views borrow the onMessage buffer that produced the event, so the host keeps
 * that buffer alive until pollEvent returns false.
 */
struct Event {
  EventType type = EVENT_TYPE_UNKNOWN;
  uint32_t streamId = CONNECTION_STREAM_ID;
  ReasonCode code = REASON_CODE_OK;
  CodecStatus codecStatus = CODEC_STATUS_OK;  // set when a bad frame caused it
  std::string_view peerSessionId;
  std::span<const uint8_t> capabilities;
  std::string_view kind;
  std::span<const uint8_t> params;
  std::span<const uint8_t> data;
};

struct StreamInfo {
  uint32_t streamId = 0U;
  std::string kind;
  Role opener = ROLE_CLIENT;
  StreamState state = STREAM_STATE_SENDER_OPENING;
  uint64_t bytesSent = 0U;  // accepted by write, not confirmed delivered
  uint64_t bytesReceived = 0U;
  uint64_t createdAtMs = 0U;
  uint64_t lastActivityMs = 0U;  // last DATA accepted or received
};

}  // namespace packetmux
