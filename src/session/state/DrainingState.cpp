#include "session/state/DrainingState.h"

namespace packetmux {

DrainingState::DrainingState(SessionProcessor* const o_processor)
    : SessionStateBase(o_processor), m_deadlineMs(0U) {}

SessionState DrainingState::id() const { return SESSION_STATE_DRAINING; }

void DrainingState::onEnter() {
  m_processor->streams().endAll(REASON_CODE_TRANSPORT_LOST);
  m_processor->dropStreamQueues();
  m_processor->queueGoaway(REASON_CODE_PROTOCOL_ERROR);
  m_deadlineMs = m_processor->nowMs() + m_processor->config().drainTimeoutMs;
}

SessionStep DrainingState::onAttach() {
  return stay(SESSION_STATUS_ALREADY_CONNECTED);
}

SessionStep DrainingState::onMessage(const std::span<const uint8_t>) {
  return stay();
}

SessionStep DrainingState::onConnectionLost() {
  return moveTo(SESSION_STATE_DISCONNECTED);
}

SessionStep DrainingState::onTick() {
  if (m_processor->nowMs() < m_deadlineMs) {
    return stay();
  }
  return moveTo(SESSION_STATE_DISCONNECTED);
}

SessionStep DrainingState::onClose(const ReasonCode i_code) {
  m_processor->setCloseCode(i_code);  // GOAWAY is already queued
  return moveTo(SESSION_STATE_CLOSING);
}

SessionStep DrainingState::onOpenStream(const OpenRequest&, uint32_t* const) {
  return stay(SESSION_STATUS_NOT_CONNECTED);
}

SessionStep DrainingState::onTransmitDrained() {
  return moveTo(SESSION_STATE_DISCONNECTED);
}

uint64_t DrainingState::nextDeadlineMs() const { return m_deadlineMs; }

}  // namespace packetmux
