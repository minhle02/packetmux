#include "session/SessionProcessor.h"

#include <utility>

#include "packetmux/Codec.h"
#include "session/RoundRobinScheduler.h"

namespace packetmux {
namespace {

// Each stream can queue at most OPEN or OPEN_OK or OPEN_ERR, plus RST.
constexpr size_t MIN_CONTROL_FRAMES_PER_STREAM = 3U;

}  // namespace

SessionProcessor::SessionProcessor(const SessionConfig& i_config)
    : m_config(i_config),
      m_scheduler(std::make_unique<RoundRobinScheduler>()),
      m_keepAlive(i_config.pingIntervalMs, i_config.deadAfterMs,
                  i_config.maxOutstandingPings),
      m_streams(m_config, m_scheduler.get(), &m_events),
      m_disconnectCode(REASON_CODE_TRANSPORT_LOST),
      m_disconnectCodecStatus(CODEC_STATUS_OK),
      m_closeCode(REASON_CODE_OK),
      m_nowMs(0U) {}

void SessionProcessor::setNow(const uint64_t i_nowMs) {
  m_nowMs = i_nowMs;
  m_streams.setNow(i_nowMs);
}

uint64_t SessionProcessor::nowMs() const { return m_nowMs; }

const SessionConfig& SessionProcessor::config() const { return m_config; }

bool SessionProcessor::validateConfig() const {
  const bool limitsAreSet =
      m_config.limits.maxDataPayload > 0U &&
      m_config.limits.maxControlPayload > 0U && m_config.maxStreams > 0U &&
      m_config.maxQueuedBytes > 0U && m_config.maxQueuedBytesPerStream > 0U &&
      m_config.maxOutstandingPings > 0U;
  const bool timesAreSet = m_config.helloTimeoutMs > 0U &&
                           m_config.pingIntervalMs > 0U &&
                           m_config.drainTimeoutMs > 0U &&
                           m_config.deadAfterMs > m_config.pingIntervalMs;
  const bool controlCapFits =
      m_config.maxQueuedControlFrames >
      MIN_CONTROL_FRAMES_PER_STREAM * m_config.maxStreams;

  std::vector<uint8_t> hello;
  const bool helloEncodes =
      Codec::encode({.type = FRAME_TYPE_HELLO,
                     .sessionId = m_config.sessionId,
                     .capabilities = m_config.capabilities},
                    m_config.limits, &hello) == CODEC_STATUS_OK;
  return m_config.role < ROLE_MAX && limitsAreSet && timesAreSet &&
         controlCapFits && helloEncodes;
}

CodecStatus SessionProcessor::decode(const std::span<const uint8_t> i_message,
                                     FrameView* const o_frame) const {
  return Codec::decode(i_message, m_config.limits, o_frame);
}

void SessionProcessor::queueHello() {
  queueControl({.type = FRAME_TYPE_HELLO,
                .sessionId = m_config.sessionId,
                .capabilities = m_config.capabilities});
}

void SessionProcessor::queueGoaway(const ReasonCode i_code) {
  queueControl({.type = FRAME_TYPE_GOAWAY, .code = i_code});
}

void SessionProcessor::queuePing(const uint64_t i_nonce) {
  queueControl({.type = FRAME_TYPE_PING, .nonce = i_nonce});
}

void SessionProcessor::queuePong(const uint64_t i_nonce) {
  queueControl({.type = FRAME_TYPE_PONG, .nonce = i_nonce});
}

void SessionProcessor::setDisconnectReason(const ReasonCode i_code,
                                           const CodecStatus i_codecStatus) {
  m_disconnectCode = i_code;
  m_disconnectCodecStatus = i_codecStatus;
}

void SessionProcessor::setCloseCode(const ReasonCode i_code) {
  m_closeCode = i_code;
}

ReasonCode SessionProcessor::closeCode() const { return m_closeCode; }

void SessionProcessor::emitConnected(const FrameView& i_frame) {
  m_events.push({.type = EVENT_TYPE_CONNECTED,
                 .peerSessionId = i_frame.sessionId,
                 .capabilities = i_frame.capabilities});
}

void SessionProcessor::emitDisconnected() {
  m_events.push({.type = EVENT_TYPE_DISCONNECTED,
                 .code = m_disconnectCode,
                 .codecStatus = m_disconnectCodecStatus});
}

void SessionProcessor::emitSessionClosed() {
  m_events.push({.type = EVENT_TYPE_SESSION_CLOSED, .code = m_closeCode});
}

void SessionProcessor::clearQueues() { m_scheduler->clear(); }

void SessionProcessor::dropStreamQueues() { m_scheduler->dropAllStreams(); }

bool SessionProcessor::transmitQueueEmpty() const {
  return m_scheduler->empty();
}

bool SessionProcessor::controlQueueOverCap() const {
  return m_scheduler->controlCount() > m_config.maxQueuedControlFrames;
}

bool SessionProcessor::popFrame(std::span<const uint8_t>* const o_message) {
  QueuedFrame frame;
  if (!m_scheduler->pop(&frame)) {
    return false;
  }
  m_txMessage = std::move(frame.message);
  *o_message = m_txMessage;
  if (frame.dataBytes > 0U) {
    m_streams.notifyWritable();
  }
  return true;
}

bool SessionProcessor::pollEvent(Event* const o_event) {
  return m_events.pop(o_event);
}

bool SessionProcessor::eventsPending() const { return !m_events.empty(); }

KeepAlive& SessionProcessor::keepAlive() { return m_keepAlive; }

const KeepAlive& SessionProcessor::keepAlive() const { return m_keepAlive; }

StreamManager& SessionProcessor::streams() { return m_streams; }

const StreamManager& SessionProcessor::streams() const { return m_streams; }

bool SessionProcessor::isSendable(const ReasonCode i_code) {
  std::vector<uint8_t> message;
  return Codec::encode({.type = FRAME_TYPE_GOAWAY, .code = i_code},
                       FrameLimits{}, &message) == CODEC_STATUS_OK;
}

void SessionProcessor::queueControl(const FrameView& i_frame) {
  std::vector<uint8_t> message;
  Codec::encode(i_frame, m_config.limits, &message);
  m_scheduler->pushControl(std::move(message));
}

}  // namespace packetmux
