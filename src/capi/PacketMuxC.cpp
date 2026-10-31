#include "packetmux/PacketMuxC.h"

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "packetmux/Codec.h"
#include "packetmux/Session.h"

// Ties each C constant to its C++ value, so the two cannot drift. The *_MAX
// lines also catch a value added on the C++ side only.
#define PACKETMUX_SAME_VALUE(i_cValue, i_cppValue) \
  static_assert(static_cast<uint8_t>(i_cValue) ==  \
                static_cast<uint8_t>(packetmux::i_cppValue))

PACKETMUX_SAME_VALUE(PACKETMUX_STATUS_OK, SESSION_STATUS_OK);
PACKETMUX_SAME_VALUE(PACKETMUX_STATUS_CLOSED, SESSION_STATUS_CLOSED);
PACKETMUX_SAME_VALUE(PACKETMUX_STATUS_NOT_CONNECTED,
                     SESSION_STATUS_NOT_CONNECTED);
PACKETMUX_SAME_VALUE(PACKETMUX_STATUS_ALREADY_CONNECTED,
                     SESSION_STATUS_ALREADY_CONNECTED);
PACKETMUX_SAME_VALUE(PACKETMUX_STATUS_EVENTS_PENDING,
                     SESSION_STATUS_EVENTS_PENDING);
PACKETMUX_SAME_VALUE(PACKETMUX_STATUS_UNKNOWN_STREAM,
                     SESSION_STATUS_UNKNOWN_STREAM);
PACKETMUX_SAME_VALUE(PACKETMUX_STATUS_INVALID_STATE,
                     SESSION_STATUS_INVALID_STATE);
PACKETMUX_SAME_VALUE(PACKETMUX_STATUS_LIMIT_EXCEEDED,
                     SESSION_STATUS_LIMIT_EXCEEDED);
PACKETMUX_SAME_VALUE(PACKETMUX_STATUS_IDS_EXHAUSTED,
                     SESSION_STATUS_IDS_EXHAUSTED);
PACKETMUX_SAME_VALUE(PACKETMUX_STATUS_INVALID_ARGUMENT,
                     SESSION_STATUS_INVALID_ARGUMENT);
PACKETMUX_SAME_VALUE(PACKETMUX_STATUS_MAX, SESSION_STATUS_MAX);

PACKETMUX_SAME_VALUE(PACKETMUX_CODEC_STATUS_OK, CODEC_STATUS_OK);
PACKETMUX_SAME_VALUE(PACKETMUX_CODEC_STATUS_TRUNCATED, CODEC_STATUS_TRUNCATED);
PACKETMUX_SAME_VALUE(PACKETMUX_CODEC_STATUS_UNKNOWN_TYPE,
                     CODEC_STATUS_UNKNOWN_TYPE);
PACKETMUX_SAME_VALUE(PACKETMUX_CODEC_STATUS_BAD_STREAM_ID,
                     CODEC_STATUS_BAD_STREAM_ID);
PACKETMUX_SAME_VALUE(PACKETMUX_CODEC_STATUS_PAYLOAD_TOO_LARGE,
                     CODEC_STATUS_PAYLOAD_TOO_LARGE);
PACKETMUX_SAME_VALUE(PACKETMUX_CODEC_STATUS_BAD_PAYLOAD_SIZE,
                     CODEC_STATUS_BAD_PAYLOAD_SIZE);
PACKETMUX_SAME_VALUE(PACKETMUX_CODEC_STATUS_UNSUPPORTED_VERSION,
                     CODEC_STATUS_UNSUPPORTED_VERSION);
PACKETMUX_SAME_VALUE(PACKETMUX_CODEC_STATUS_BAD_IDENTIFIER,
                     CODEC_STATUS_BAD_IDENTIFIER);
PACKETMUX_SAME_VALUE(PACKETMUX_CODEC_STATUS_INVALID_ARGUMENT,
                     CODEC_STATUS_INVALID_ARGUMENT);
PACKETMUX_SAME_VALUE(PACKETMUX_CODEC_STATUS_BAD_FLAG, CODEC_STATUS_BAD_FLAG);
PACKETMUX_SAME_VALUE(PACKETMUX_CODEC_STATUS_BAD_WINDOW,
                     CODEC_STATUS_BAD_WINDOW);
PACKETMUX_SAME_VALUE(PACKETMUX_CODEC_STATUS_MAX, CODEC_STATUS_MAX);

PACKETMUX_SAME_VALUE(PACKETMUX_ROLE_CLIENT, ROLE_CLIENT);
PACKETMUX_SAME_VALUE(PACKETMUX_ROLE_SERVER, ROLE_SERVER);
PACKETMUX_SAME_VALUE(PACKETMUX_ROLE_MAX, ROLE_MAX);

PACKETMUX_SAME_VALUE(PACKETMUX_REASON_OK, REASON_CODE_OK);
PACKETMUX_SAME_VALUE(PACKETMUX_REASON_PEER_CLOSED, REASON_CODE_PEER_CLOSED);
PACKETMUX_SAME_VALUE(PACKETMUX_REASON_CONNECT_FAILED,
                     REASON_CODE_CONNECT_FAILED);
PACKETMUX_SAME_VALUE(PACKETMUX_REASON_POLICY_DENIED, REASON_CODE_POLICY_DENIED);
PACKETMUX_SAME_VALUE(PACKETMUX_REASON_LIMIT_EXCEEDED,
                     REASON_CODE_LIMIT_EXCEEDED);
PACKETMUX_SAME_VALUE(PACKETMUX_REASON_TRANSPORT_LOST,
                     REASON_CODE_TRANSPORT_LOST);
PACKETMUX_SAME_VALUE(PACKETMUX_REASON_KILLED, REASON_CODE_KILLED);
PACKETMUX_SAME_VALUE(PACKETMUX_REASON_AUTH_REVOKED, REASON_CODE_AUTH_REVOKED);
PACKETMUX_SAME_VALUE(PACKETMUX_REASON_TIMEOUT, REASON_CODE_TIMEOUT);
PACKETMUX_SAME_VALUE(PACKETMUX_REASON_PROTOCOL_ERROR,
                     REASON_CODE_PROTOCOL_ERROR);
PACKETMUX_SAME_VALUE(PACKETMUX_REASON_UNSUPPORTED_KIND,
                     REASON_CODE_UNSUPPORTED_KIND);
PACKETMUX_SAME_VALUE(PACKETMUX_REASON_SESSION_NOT_FOUND,
                     REASON_CODE_SESSION_NOT_FOUND);
PACKETMUX_SAME_VALUE(PACKETMUX_REASON_MAX, REASON_CODE_MAX);

PACKETMUX_SAME_VALUE(PACKETMUX_FRAME_TYPE_UNKNOWN, FRAME_TYPE_UNKNOWN);
PACKETMUX_SAME_VALUE(PACKETMUX_FRAME_TYPE_HELLO, FRAME_TYPE_HELLO);
PACKETMUX_SAME_VALUE(PACKETMUX_FRAME_TYPE_OPEN, FRAME_TYPE_OPEN);
PACKETMUX_SAME_VALUE(PACKETMUX_FRAME_TYPE_OPEN_OK, FRAME_TYPE_OPEN_OK);
PACKETMUX_SAME_VALUE(PACKETMUX_FRAME_TYPE_OPEN_ERR, FRAME_TYPE_OPEN_ERR);
PACKETMUX_SAME_VALUE(PACKETMUX_FRAME_TYPE_DATA, FRAME_TYPE_DATA);
PACKETMUX_SAME_VALUE(PACKETMUX_FRAME_TYPE_EOF, FRAME_TYPE_EOF);
PACKETMUX_SAME_VALUE(PACKETMUX_FRAME_TYPE_RST, FRAME_TYPE_RST);
PACKETMUX_SAME_VALUE(PACKETMUX_FRAME_TYPE_PING, FRAME_TYPE_PING);
PACKETMUX_SAME_VALUE(PACKETMUX_FRAME_TYPE_PONG, FRAME_TYPE_PONG);
PACKETMUX_SAME_VALUE(PACKETMUX_FRAME_TYPE_GOAWAY, FRAME_TYPE_GOAWAY);
PACKETMUX_SAME_VALUE(PACKETMUX_FRAME_TYPE_WINDOW, FRAME_TYPE_WINDOW);
PACKETMUX_SAME_VALUE(PACKETMUX_FRAME_TYPE_MAX, FRAME_TYPE_MAX);

PACKETMUX_SAME_VALUE(PACKETMUX_EVENT_TYPE_UNKNOWN, EVENT_TYPE_UNKNOWN);
PACKETMUX_SAME_VALUE(PACKETMUX_EVENT_TYPE_CONNECTED, EVENT_TYPE_CONNECTED);
PACKETMUX_SAME_VALUE(PACKETMUX_EVENT_TYPE_STREAM_OPENED,
                     EVENT_TYPE_STREAM_OPENED);
PACKETMUX_SAME_VALUE(PACKETMUX_EVENT_TYPE_STREAM_ACCEPTED,
                     EVENT_TYPE_STREAM_ACCEPTED);
PACKETMUX_SAME_VALUE(PACKETMUX_EVENT_TYPE_DATA, EVENT_TYPE_DATA);
PACKETMUX_SAME_VALUE(PACKETMUX_EVENT_TYPE_EOF, EVENT_TYPE_EOF);
PACKETMUX_SAME_VALUE(PACKETMUX_EVENT_TYPE_WRITABLE, EVENT_TYPE_WRITABLE);
PACKETMUX_SAME_VALUE(PACKETMUX_EVENT_TYPE_STREAM_CLOSED,
                     EVENT_TYPE_STREAM_CLOSED);
PACKETMUX_SAME_VALUE(PACKETMUX_EVENT_TYPE_DISCONNECTED,
                     EVENT_TYPE_DISCONNECTED);
PACKETMUX_SAME_VALUE(PACKETMUX_EVENT_TYPE_SESSION_CLOSED,
                     EVENT_TYPE_SESSION_CLOSED);
PACKETMUX_SAME_VALUE(PACKETMUX_EVENT_TYPE_MAX, EVENT_TYPE_MAX);

PACKETMUX_SAME_VALUE(PACKETMUX_SESSION_STATE_DISCONNECTED,
                     SESSION_STATE_DISCONNECTED);
PACKETMUX_SAME_VALUE(PACKETMUX_SESSION_STATE_HANDSHAKING,
                     SESSION_STATE_HANDSHAKING);
PACKETMUX_SAME_VALUE(PACKETMUX_SESSION_STATE_ESTABLISHED,
                     SESSION_STATE_ESTABLISHED);
PACKETMUX_SAME_VALUE(PACKETMUX_SESSION_STATE_DRAINING, SESSION_STATE_DRAINING);
PACKETMUX_SAME_VALUE(PACKETMUX_SESSION_STATE_CLOSING, SESSION_STATE_CLOSING);
PACKETMUX_SAME_VALUE(PACKETMUX_SESSION_STATE_CLOSED, SESSION_STATE_CLOSED);
PACKETMUX_SAME_VALUE(PACKETMUX_SESSION_STATE_MAX, SESSION_STATE_MAX);

PACKETMUX_SAME_VALUE(PACKETMUX_STREAM_STATE_SENDER_OPENING,
                     STREAM_STATE_SENDER_OPENING);
PACKETMUX_SAME_VALUE(PACKETMUX_STREAM_STATE_SENDER_OPENING_EOF_SENT,
                     STREAM_STATE_SENDER_OPENING_EOF_SENT);
PACKETMUX_SAME_VALUE(PACKETMUX_STREAM_STATE_RECEIVER_OPENING,
                     STREAM_STATE_RECEIVER_OPENING);
PACKETMUX_SAME_VALUE(PACKETMUX_STREAM_STATE_RECEIVER_OPENING_EOF_RECEIVED,
                     STREAM_STATE_RECEIVER_OPENING_EOF_RECEIVED);
PACKETMUX_SAME_VALUE(PACKETMUX_STREAM_STATE_OPEN, STREAM_STATE_OPEN);
PACKETMUX_SAME_VALUE(PACKETMUX_STREAM_STATE_HALF_CLOSED_LOCAL,
                     STREAM_STATE_HALF_CLOSED_LOCAL);
PACKETMUX_SAME_VALUE(PACKETMUX_STREAM_STATE_HALF_CLOSED_REMOTE,
                     STREAM_STATE_HALF_CLOSED_REMOTE);
PACKETMUX_SAME_VALUE(PACKETMUX_STREAM_STATE_CLOSED, STREAM_STATE_CLOSED);
PACKETMUX_SAME_VALUE(PACKETMUX_STREAM_STATE_MAX, STREAM_STATE_MAX);

static_assert(PACKETMUX_NO_DEADLINE == packetmux::NO_DEADLINE);

#undef PACKETMUX_SAME_VALUE

struct packetmux_session {
  packetmux::Session session;
  std::vector<packetmux::StreamInfo> listed;  // the last list_streams snapshot
};

namespace packetmux {
namespace {

std::span<const uint8_t> toSpan(const packetmux_bytes_t& i_bytes) {
  return {i_bytes.data, i_bytes.size};
}

std::span<const uint8_t> toSpan(const uint8_t* const i_data,
                                const size_t i_size) {
  return {i_data, i_size};
}

std::string_view toView(const uint8_t* const i_data, const size_t i_size) {
  return {reinterpret_cast<const char*>(i_data), i_size};
}

std::string_view toView(const packetmux_bytes_t& i_bytes) {
  return toView(i_bytes.data, i_bytes.size);
}

packetmux_bytes_t toBytes(const std::span<const uint8_t> i_bytes) {
  return {i_bytes.data(), i_bytes.size()};
}

packetmux_bytes_t toBytes(const std::string_view i_text) {
  return {reinterpret_cast<const uint8_t*>(i_text.data()), i_text.size()};
}

FrameLimits toLimits(const packetmux_limits_t& i_limits) {
  return {.maxDataPayload = i_limits.max_data_payload,
          .maxControlPayload = i_limits.max_control_payload};
}

SessionConfig toConfig(const packetmux_config_t& i_config) {
  const std::span<const uint8_t> capabilities = toSpan(i_config.capabilities);
  return {.role = static_cast<Role>(i_config.role),
          .sessionId = std::string(toView(i_config.session_id)),
          .capabilities =
              std::vector<uint8_t>(capabilities.begin(), capabilities.end()),
          .limits = toLimits(i_config.limits),
          .maxStreams = i_config.max_streams,
          .maxQueuedBytes = i_config.max_queued_bytes,
          .maxQueuedBytesPerStream = i_config.max_queued_bytes_per_stream,
          .maxQueuedControlFrames = i_config.max_queued_control_frames,
          .helloTimeoutMs = i_config.hello_timeout_ms,
          .pingIntervalMs = i_config.ping_interval_ms,
          .deadAfterMs = i_config.dead_after_ms,
          .drainTimeoutMs = i_config.drain_timeout_ms,
          .maxOutstandingPings = i_config.max_outstanding_pings,
          .receiveWindow = i_config.receive_window};
}

packetmux_event_t toEvent(const Event& i_event) {
  return {.type = i_event.type,
          .stream_id = i_event.streamId,
          .code = i_event.code,
          .codec_status = i_event.codecStatus,
          .peer_session_id = toBytes(i_event.peerSessionId),
          .capabilities = toBytes(i_event.capabilities),
          .kind = toBytes(i_event.kind),
          .params = toBytes(i_event.params),
          .data = toBytes(i_event.data)};
}

packetmux_stream_info_t toStreamInfo(const StreamInfo& i_info) {
  return {.stream_id = i_info.streamId,
          .kind = toBytes(std::string_view(i_info.kind)),
          .opener = i_info.opener,
          .state = i_info.state,
          .bytes_sent = i_info.bytesSent,
          .bytes_received = i_info.bytesReceived,
          .created_at_ms = i_info.createdAtMs,
          .last_activity_ms = i_info.lastActivityMs,
          .send_credit = i_info.sendCredit};
}

packetmux_frame_t toFrame(const FrameView& i_frame) {
  return {.type = i_frame.type,
          .stream_id = i_frame.streamId,
          .version = i_frame.version,
          .resuming = i_frame.resuming,
          .session_id = toBytes(i_frame.sessionId),
          .capabilities = toBytes(i_frame.capabilities),
          .kind = toBytes(i_frame.kind),
          .params = toBytes(i_frame.params),
          .code = i_frame.code,
          .message = toBytes(i_frame.message),
          .data = toBytes(i_frame.data),
          .nonce = i_frame.nonce,
          .receive_window = i_frame.receiveWindow,
          .credit = i_frame.credit};
}

FrameView toFrameView(const packetmux_frame_t& i_frame) {
  return {.type = static_cast<FrameType>(i_frame.type),
          .streamId = i_frame.stream_id,
          .version = i_frame.version,
          .resuming = i_frame.resuming,
          .sessionId = toView(i_frame.session_id),
          .capabilities = toSpan(i_frame.capabilities),
          .kind = toView(i_frame.kind),
          .params = toSpan(i_frame.params),
          .code = static_cast<ReasonCode>(i_frame.code),
          .message = toSpan(i_frame.message),
          .data = toSpan(i_frame.data),
          .nonce = i_frame.nonce,
          .receiveWindow = i_frame.receive_window,
          .credit = i_frame.credit};
}

}  // namespace
}  // namespace packetmux

using packetmux::Codec;
using packetmux::CodecStatus;
using packetmux::Event;
using packetmux::FrameView;
using packetmux::ReasonCode;
using packetmux::Session;
using packetmux::SessionConfig;
using packetmux::toSpan;
using packetmux::toView;

extern "C" {

void packetmux_config_init(packetmux_config_t* const o_config) noexcept {
  const SessionConfig defaults;
  *o_config = {
      .role = defaults.role,
      .session_id = {nullptr, 0U},
      .capabilities = {nullptr, 0U},
      .limits = {.max_data_payload = defaults.limits.maxDataPayload,
                 .max_control_payload = defaults.limits.maxControlPayload},
      .max_streams = defaults.maxStreams,
      .max_queued_bytes = defaults.maxQueuedBytes,
      .max_queued_bytes_per_stream = defaults.maxQueuedBytesPerStream,
      .max_queued_control_frames = defaults.maxQueuedControlFrames,
      .hello_timeout_ms = defaults.helloTimeoutMs,
      .ping_interval_ms = defaults.pingIntervalMs,
      .dead_after_ms = defaults.deadAfterMs,
      .drain_timeout_ms = defaults.drainTimeoutMs,
      .max_outstanding_pings = defaults.maxOutstandingPings,
      .receive_window = defaults.receiveWindow};
}

packetmux_session_t* packetmux_session_create(
    const packetmux_config_t* const i_config) noexcept {
  return new packetmux_session{Session(packetmux::toConfig(*i_config)), {}};
}

void packetmux_session_destroy(packetmux_session_t* const i_session) noexcept {
  delete i_session;
}

packetmux_status_t packetmux_session_attach_connection(
    packetmux_session_t* const i_session, const uint64_t i_now_ms) noexcept {
  return i_session->session.attachConnection(i_now_ms);
}

packetmux_status_t packetmux_session_on_message(
    packetmux_session_t* const i_session, const uint8_t* const i_message,
    const size_t i_size, const uint64_t i_now_ms) noexcept {
  return i_session->session.onMessage(toSpan(i_message, i_size), i_now_ms);
}

packetmux_status_t packetmux_session_on_connection_lost(
    packetmux_session_t* const i_session) noexcept {
  return i_session->session.onConnectionLost();
}

packetmux_status_t packetmux_session_on_tick(
    packetmux_session_t* const i_session, const uint64_t i_now_ms) noexcept {
  return i_session->session.onTick(i_now_ms);
}

uint64_t packetmux_session_next_deadline_ms(
    const packetmux_session_t* const i_session) noexcept {
  return i_session->session.nextDeadlineMs();
}

packetmux_status_t packetmux_session_close(packetmux_session_t* const i_session,
                                           const packetmux_reason_t i_code,
                                           const uint64_t i_now_ms) noexcept {
  return i_session->session.close(static_cast<ReasonCode>(i_code), i_now_ms);
}

packetmux_status_t packetmux_session_open_stream(
    packetmux_session_t* const i_session, const uint8_t* const i_kind,
    const size_t i_kind_size, const uint8_t* const i_params,
    const size_t i_params_size, const uint64_t i_now_ms,
    uint32_t* const o_stream_id) noexcept {
  return i_session->session.openStream(toView(i_kind, i_kind_size),
                                       toSpan(i_params, i_params_size),
                                       i_now_ms, o_stream_id);
}

packetmux_status_t packetmux_session_accept_stream(
    packetmux_session_t* const i_session, const uint32_t i_stream_id) noexcept {
  return i_session->session.acceptStream(i_stream_id);
}

packetmux_status_t packetmux_session_reject_stream(
    packetmux_session_t* const i_session, const uint32_t i_stream_id,
    const packetmux_reason_t i_code, const uint8_t* const i_message,
    const size_t i_size) noexcept {
  return i_session->session.rejectStream(
      i_stream_id, static_cast<ReasonCode>(i_code), toSpan(i_message, i_size));
}

packetmux_status_t packetmux_session_write(packetmux_session_t* const i_session,
                                           const uint32_t i_stream_id,
                                           const uint8_t* const i_bytes,
                                           const size_t i_size,
                                           const uint64_t i_now_ms,
                                           size_t* const o_accepted) noexcept {
  return i_session->session.write(i_stream_id, toSpan(i_bytes, i_size),
                                  i_now_ms, o_accepted);
}

packetmux_status_t packetmux_session_consume(
    packetmux_session_t* const i_session, const uint32_t i_stream_id,
    const uint64_t i_bytes) noexcept {
  return i_session->session.consume(i_stream_id, i_bytes);
}

packetmux_status_t packetmux_session_send_eof(
    packetmux_session_t* const i_session, const uint32_t i_stream_id) noexcept {
  return i_session->session.sendEof(i_stream_id);
}

packetmux_status_t packetmux_session_reset_stream(
    packetmux_session_t* const i_session, const uint32_t i_stream_id,
    const packetmux_reason_t i_code) noexcept {
  return i_session->session.resetStream(i_stream_id,
                                        static_cast<ReasonCode>(i_code));
}

bool packetmux_session_poll_transmit(
    packetmux_session_t* const i_session,
    packetmux_bytes_t* const o_message) noexcept {
  std::span<const uint8_t> message;
  if (!i_session->session.pollTransmit(&message)) {
    return false;
  }
  *o_message = packetmux::toBytes(message);
  return true;
}

bool packetmux_session_poll_event(packetmux_session_t* const i_session,
                                  packetmux_event_t* const o_event) noexcept {
  Event event;
  if (!i_session->session.pollEvent(&event)) {
    return false;
  }
  *o_event = packetmux::toEvent(event);
  return true;
}

packetmux_session_state_t packetmux_session_state(
    const packetmux_session_t* const i_session) noexcept {
  return i_session->session.state();
}

bool packetmux_session_get_rtt_ms(const packetmux_session_t* const i_session,
                                  uint64_t* const o_rtt_ms) noexcept {
  return i_session->session.getRttMs(o_rtt_ms);
}

size_t packetmux_session_list_streams(
    packetmux_session_t* const i_session) noexcept {
  i_session->session.listStreams(&i_session->listed);
  return i_session->listed.size();
}

void packetmux_session_stream_at(
    const packetmux_session_t* const i_session, const size_t i_index,
    packetmux_stream_info_t* const o_info) noexcept {
  *o_info = packetmux::toStreamInfo(i_session->listed[i_index]);
}

packetmux_codec_status_t packetmux_frame_decode(
    const uint8_t* const i_message, const size_t i_size,
    const packetmux_limits_t* const i_limits,
    packetmux_frame_t* const o_frame) noexcept {
  FrameView frame;
  const CodecStatus status = Codec::decode(
      toSpan(i_message, i_size), packetmux::toLimits(*i_limits), &frame);
  *o_frame = packetmux::toFrame(frame);
  return status;
}

packetmux_codec_status_t packetmux_frame_encode(
    const packetmux_frame_t* const i_frame,
    const packetmux_limits_t* const i_limits, uint8_t* const o_buffer,
    const size_t i_capacity, size_t* const o_size) noexcept {
  std::vector<uint8_t> message;
  const CodecStatus status =
      Codec::encode(packetmux::toFrameView(*i_frame),
                    packetmux::toLimits(*i_limits), &message);
  if (status != packetmux::CODEC_STATUS_OK) {
    return status;
  }
  if (message.size() > i_capacity) {
    return packetmux::CODEC_STATUS_INVALID_ARGUMENT;
  }
  std::copy(message.begin(), message.end(), o_buffer);
  *o_size = message.size();
  return packetmux::CODEC_STATUS_OK;
}

}  // extern "C"
