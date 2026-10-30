#include "session/state/ClosingState.h"

namespace packetmux {

ClosingState::ClosingState(SessionProcessor* const o_processor)
    : SessionStateBase(o_processor), m_deadlineMs(0U) {}

SessionState ClosingState::id() const { return SESSION_STATE_CLOSING; }

void ClosingState::onEnter() {
  m_processor->streams().endAll(
      StreamProcessor::reasonForStreams(m_processor->closeCode()));
  m_processor->dropStreamQueues();
  m_deadlineMs = m_processor->nowMs() + m_processor->config().drainTimeoutMs;
}

SessionStep ClosingState::onAttach() { return stay(SESSION_STATUS_CLOSED); }

SessionStep ClosingState::onMessage(const std::span<const uint8_t>) {
  return stay();
}

SessionStep ClosingState::onConnectionLost() {
  return moveTo(SESSION_STATE_CLOSED);
}

SessionStep ClosingState::onTick() {
  if (m_processor->nowMs() < m_deadlineMs) {
    return stay();
  }
  return moveTo(SESSION_STATE_CLOSED);
}

SessionStep ClosingState::onClose(const ReasonCode) {
  return stay(SESSION_STATUS_CLOSED);
}

SessionStep ClosingState::onOpenStream(const OpenRequest&, uint32_t* const) {
  return stay(SESSION_STATUS_CLOSED);
}

SessionStep ClosingState::onTransmitDrained() {
  return moveTo(SESSION_STATE_CLOSED);
}

uint64_t ClosingState::nextDeadlineMs() const { return m_deadlineMs; }

}  // namespace packetmux
