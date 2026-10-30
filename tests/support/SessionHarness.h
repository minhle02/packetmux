#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "packetmux/Codec.h"
#include "packetmux/Session.h"

namespace packetmux {

using Bytes = std::vector<uint8_t>;

constexpr uint64_t HARNESS_START_MS = 1000U;

// Copies of an Event and a frame, so tests can keep them.
struct OwnedEvent {
  EventType type = EVENT_TYPE_UNKNOWN;
  uint32_t streamId = 0U;
  ReasonCode code = REASON_CODE_OK;
  CodecStatus codecStatus = CODEC_STATUS_OK;
  std::string peerSessionId;
  Bytes capabilities;
  std::string kind;
  Bytes params;
  Bytes data;
};

struct OwnedFrame {
  FrameType type = FRAME_TYPE_UNKNOWN;
  uint32_t streamId = 0U;
  ReasonCode code = REASON_CODE_OK;
  uint64_t nonce = 0U;
  std::string sessionId;
  std::string kind;
  Bytes params;
  Bytes message;
  Bytes data;
};

/**
 * Drives one Session from a script. Inbound frames are built with Codec, which
 * the M1 golden vectors already cover, so the script does not depend on
 * Session.
 */
class SessionHarness {
 public:
  explicit SessionHarness(const Role i_role = ROLE_CLIENT)
      : SessionHarness(configFor(i_role)) {}
  explicit SessionHarness(const SessionConfig& i_config)
      : m_config(i_config), m_session(i_config), m_nowMs(HARNESS_START_MS) {}

  static SessionConfig configFor(const Role i_role) {
    SessionConfig config;
    config.role = i_role;
    config.sessionId = "session-1";
    return config;
  }

  Session& session() { return m_session; }
  SessionState state() const { return m_session.state(); }
  uint64_t nowMs() const { return m_nowMs; }
  void advance(const uint64_t i_ms) { m_nowMs += i_ms; }

  // Peer message that the session should receive. Events are copied straight
  // away, while the message is still alive.
  SessionStatus rxRaw(const Bytes& i_message) {
    m_lastMessage = i_message;
    const SessionStatus status = m_session.onMessage(m_lastMessage, m_nowMs);
    collectEvents();
    return status;
  }

  SessionStatus rx(const FrameView& i_frame) {
    Bytes message;
    EXPECT_EQ(Codec::encode(i_frame, m_config.limits, &message),
              CODEC_STATUS_OK);
    return rxRaw(message);
  }

  SessionStatus rxHello() {
    return rx({.type = FRAME_TYPE_HELLO, .sessionId = "peer-1"});
  }

  SessionStatus tick() {
    const SessionStatus status = m_session.onTick(m_nowMs);
    collectEvents();
    return status;
  }

  SessionStatus attach() {
    const SessionStatus status = m_session.attachConnection(m_nowMs);
    collectEvents();
    return status;
  }

  SessionStatus lost() {
    const SessionStatus status = m_session.onConnectionLost();
    collectEvents();
    return status;
  }

  SessionStatus close(const ReasonCode i_code) {
    const SessionStatus status = m_session.close(i_code, m_nowMs);
    collectEvents();
    return status;
  }

  // Attaches, completes the handshake, and forgets the frames and events.
  void establish() {
    ASSERT_EQ(attach(), SESSION_STATUS_OK);
    ASSERT_EQ(rxHello(), SESSION_STATUS_OK);
    ASSERT_EQ(state(), SESSION_STATE_ESTABLISHED);
    takeFrames();
    takeEvents();
  }

  uint32_t open(const std::string_view i_kind = "kind") {
    uint32_t streamId = 0U;
    EXPECT_EQ(m_session.openStream(i_kind, {}, m_nowMs, &streamId),
              SESSION_STATUS_OK);
    return streamId;
  }

  // A stream opened by us and accepted by the peer, with nothing left over.
  uint32_t openAccepted() {
    const uint32_t streamId = open();
    EXPECT_EQ(rx({.type = FRAME_TYPE_OPEN_OK, .streamId = streamId}),
              SESSION_STATUS_OK);
    takeFrames();
    takeEvents();
    return streamId;
  }

  // A stream opened by the peer and accepted by us, with nothing left over.
  uint32_t peerOpenAccepted(const uint32_t i_streamId) {
    EXPECT_EQ(
        rx({.type = FRAME_TYPE_OPEN, .streamId = i_streamId, .kind = "k"}),
        SESSION_STATUS_OK);
    EXPECT_EQ(m_session.acceptStream(i_streamId), SESSION_STATUS_OK);
    takeFrames();
    takeEvents();
    return i_streamId;
  }

  size_t write(const uint32_t i_streamId, const Bytes& i_bytes) {
    size_t accepted = 0U;
    EXPECT_EQ(m_session.write(i_streamId, i_bytes, m_nowMs, &accepted),
              SESSION_STATUS_OK);
    return accepted;
  }

  // Takes frames until there are none, like a host whose transport has room.
  std::vector<OwnedFrame> takeFrames(const size_t i_limit = SIZE_MAX) {
    std::vector<OwnedFrame> frames;
    std::span<const uint8_t> message;
    while (frames.size() < i_limit && m_session.pollTransmit(&message)) {
      FrameView view;
      EXPECT_EQ(Codec::decode(message, m_config.limits, &view),
                CODEC_STATUS_OK);
      frames.push_back(
          {.type = view.type,
           .streamId = view.streamId,
           .code = view.code,
           .nonce = view.nonce,
           .sessionId = std::string(view.sessionId),
           .kind = std::string(view.kind),
           .params = Bytes(view.params.begin(), view.params.end()),
           .message = Bytes(view.message.begin(), view.message.end()),
           .data = Bytes(view.data.begin(), view.data.end())});
    }
    collectEvents();
    return frames;
  }

  std::vector<OwnedEvent> takeEvents() {
    collectEvents();
    std::vector<OwnedEvent> events;
    events.swap(m_events);
    return events;
  }

 private:
  void collectEvents() {
    Event event;
    while (m_session.pollEvent(&event)) {
      m_events.push_back(
          {.type = event.type,
           .streamId = event.streamId,
           .code = event.code,
           .codecStatus = event.codecStatus,
           .peerSessionId = std::string(event.peerSessionId),
           .capabilities =
               Bytes(event.capabilities.begin(), event.capabilities.end()),
           .kind = std::string(event.kind),
           .params = Bytes(event.params.begin(), event.params.end()),
           .data = Bytes(event.data.begin(), event.data.end())});
    }
  }

  SessionConfig m_config;
  Session m_session;
  uint64_t m_nowMs;
  Bytes m_lastMessage;
  std::vector<OwnedEvent> m_events;
};

inline std::vector<EventType> typesOf(const std::vector<OwnedEvent>& i_events) {
  std::vector<EventType> types;
  for (const OwnedEvent& event : i_events) {
    types.push_back(event.type);
  }
  return types;
}

inline std::vector<FrameType> typesOf(const std::vector<OwnedFrame>& i_frames) {
  std::vector<FrameType> types;
  for (const OwnedFrame& frame : i_frames) {
    types.push_back(frame.type);
  }
  return types;
}

}  // namespace packetmux
