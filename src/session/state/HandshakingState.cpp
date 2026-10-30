#include "session/state/HandshakingState.h"

namespace packetmux {

HandshakingState::HandshakingState(SessionProcessor* const o_processor)
    : SessionStateBase(o_processor), m_deadlineMs(0U) {}

SessionState HandshakingState::id() const { return SESSION_STATE_HANDSHAKING; }

void HandshakingState::onEnter() {
  m_processor->queueHello();
  m_deadlineMs = m_processor->nowMs() + m_processor->config().helloTimeoutMs;
}

SessionStep HandshakingState::onAttach() {
  return stay(SESSION_STATUS_ALREADY_CONNECTED);
}

SessionStep HandshakingState::onMessage(
    const std::span<const uint8_t> i_message) {
  FrameView frame;
  const CodecStatus status = m_processor->decode(i_message, &frame);
  if (status != CODEC_STATUS_OK || frame.type != FRAME_TYPE_HELLO) {
    m_processor->setDisconnectReason(REASON_CODE_PROTOCOL_ERROR, status);
    return moveTo(SESSION_STATE_DRAINING);
  }
  m_processor->emitConnected(frame);
  return moveTo(SESSION_STATE_ESTABLISHED);
}

SessionStep HandshakingState::onConnectionLost() {
  m_processor->setDisconnectReason(REASON_CODE_TRANSPORT_LOST);
  return moveTo(SESSION_STATE_DISCONNECTED);
}

SessionStep HandshakingState::onTick() {
  if (m_processor->nowMs() < m_deadlineMs) {
    return stay();
  }
  m_processor->setDisconnectReason(REASON_CODE_TIMEOUT);
  return moveTo(SESSION_STATE_DISCONNECTED);
}

SessionStep HandshakingState::onClose(const ReasonCode i_code) {
  m_processor->setCloseCode(i_code);
  m_processor->queueGoaway(i_code);
  return moveTo(SESSION_STATE_CLOSING);
}

SessionStep HandshakingState::onOpenStream(const OpenRequest& i_request,
                                           uint32_t* const o_streamId) {
  return stay(m_processor->streams().open(i_request.kind, i_request.params,
                                          o_streamId));
}

SessionStep HandshakingState::onTransmitDrained() { return stay(); }

uint64_t HandshakingState::nextDeadlineMs() const { return m_deadlineMs; }

}  // namespace packetmux
