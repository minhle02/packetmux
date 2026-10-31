#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "packetmux/definitions/ProtocolDef.h"

namespace packetmux {

/**
 * Numeric values of FrameType and ReasonCode are part of the wire protocol:
 * never renumber. FRAME_TYPE_UNKNOWN and the *_MAX values never appear on the
 * wire.
 */
enum FrameType : uint8_t {
  FRAME_TYPE_UNKNOWN = 0U,
  FRAME_TYPE_HELLO,
  FRAME_TYPE_OPEN,
  FRAME_TYPE_OPEN_OK,
  FRAME_TYPE_OPEN_ERR,
  FRAME_TYPE_DATA,
  FRAME_TYPE_EOF,
  FRAME_TYPE_RST,
  FRAME_TYPE_PING,
  FRAME_TYPE_PONG,
  FRAME_TYPE_GOAWAY,
  FRAME_TYPE_WINDOW,
  FRAME_TYPE_MAX,
};

enum ReasonCode : uint8_t {
  REASON_CODE_OK = 0U,
  REASON_CODE_PEER_CLOSED,
  REASON_CODE_CONNECT_FAILED,
  REASON_CODE_POLICY_DENIED,
  REASON_CODE_LIMIT_EXCEEDED,
  REASON_CODE_TRANSPORT_LOST,  // local only, never on the wire
  REASON_CODE_KILLED,
  REASON_CODE_AUTH_REVOKED,
  REASON_CODE_TIMEOUT,
  REASON_CODE_PROTOCOL_ERROR,
  REASON_CODE_UNSUPPORTED_KIND,
  REASON_CODE_SESSION_NOT_FOUND,
  REASON_CODE_MAX,
};

enum CodecStatus : uint8_t {
  CODEC_STATUS_OK = 0U,
  CODEC_STATUS_TRUNCATED,
  CODEC_STATUS_UNKNOWN_TYPE,
  CODEC_STATUS_BAD_STREAM_ID,
  CODEC_STATUS_PAYLOAD_TOO_LARGE,
  CODEC_STATUS_BAD_PAYLOAD_SIZE,
  CODEC_STATUS_UNSUPPORTED_VERSION,
  CODEC_STATUS_BAD_IDENTIFIER,
  CODEC_STATUS_INVALID_ARGUMENT,  // encode only: e.g. a reason code that may
                                  // not be sent
  CODEC_STATUS_BAD_FLAG,
  CODEC_STATUS_BAD_WINDOW,
  CODEC_STATUS_MAX,
};

struct FrameLimits {
  size_t maxDataPayload = DEFAULT_MAX_DATA_PAYLOAD;
  size_t maxControlPayload = DEFAULT_MAX_CONTROL_PAYLOAD;
};

/**
 * One frame, as plain fields. Spans and string views BORROW memory: after
 * Codec::decode they point into the decoded message; for Codec::encode they
 * point to the caller's data. Only the fields of the frame's type are used; the
 * encoder ignores the others.
 */
struct FrameView {
  FrameType type = FRAME_TYPE_UNKNOWN;
  uint32_t streamId = CONNECTION_STREAM_ID;
  uint16_t version = PROTOCOL_VERSION;              // HELLO
  bool resuming = false;                            // HELLO
  std::string_view sessionId;                       // HELLO
  std::span<const uint8_t> capabilities;            // HELLO
  std::string_view kind;                            // OPEN
  std::span<const uint8_t> params;                  // OPEN
  ReasonCode code = REASON_CODE_OK;                 // OPEN_ERR, RST, GOAWAY
  std::span<const uint8_t> message;                 // OPEN_ERR
  std::span<const uint8_t> data;                    // DATA
  uint64_t nonce = 0U;                              // PING, PONG
  uint32_t receiveWindow = DEFAULT_RECEIVE_WINDOW;  // HELLO
  uint32_t credit = 0U;                             // WINDOW
};

}  // namespace packetmux
