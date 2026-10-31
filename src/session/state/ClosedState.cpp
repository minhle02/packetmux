#include "session/state/ClosedState.h"

namespace packetmux {

ClosedState::ClosedState(SessionProcessor* const o_processor)
    : SessionStateBase(o_processor) {}

SessionState ClosedState::id() const { return SESSION_STATE_CLOSED; }

void ClosedState::onEnter() {
  m_processor->streams().endAll(
      StreamProcessor::reasonForStreams(m_processor->closeCode()));
  m_processor->clearQueues();
  m_processor->emitSessionClosed();
}

SessionStep ClosedState::onAttach() { return stay(SESSION_STATUS_CLOSED); }

SessionStep ClosedState::onMessage(const std::span<const uint8_t>) {
  return stay(SESSION_STATUS_CLOSED);
}

SessionStep ClosedState::onConnectionLost() { return stay(); }

SessionStep ClosedState::onTick() { return stay(); }

SessionStep ClosedState::onClose(const ReasonCode) {
  return stay(SESSION_STATUS_CLOSED);
}

SessionStep ClosedState::onOpenStream(const OpenRequest&, uint32_t* const) {
  return stay(SESSION_STATUS_CLOSED);
}

SessionStep ClosedState::onTransmitDrained() { return stay(); }

uint64_t ClosedState::nextDeadlineMs() const { return NO_DEADLINE; }

}  // namespace packetmux
