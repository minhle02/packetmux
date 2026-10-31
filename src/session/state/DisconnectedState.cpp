#include "session/state/DisconnectedState.h"

namespace packetmux {

DisconnectedState::DisconnectedState(SessionProcessor* const o_processor)
    : SessionStateBase(o_processor) {}

SessionState DisconnectedState::id() const {
  return SESSION_STATE_DISCONNECTED;
}

void DisconnectedState::onEnter() {
  m_processor->streams().endAll(REASON_CODE_TRANSPORT_LOST);
  m_processor->clearQueues();
  m_processor->emitDisconnected();
}

SessionStep DisconnectedState::onAttach() {
  if (!m_processor->validateConfig()) {
    return stay(SESSION_STATUS_INVALID_ARGUMENT);
  }
  return moveTo(SESSION_STATE_HANDSHAKING);
}

SessionStep DisconnectedState::onMessage(const std::span<const uint8_t>) {
  return stay(SESSION_STATUS_NOT_CONNECTED);
}

SessionStep DisconnectedState::onConnectionLost() { return stay(); }

SessionStep DisconnectedState::onTick() { return stay(); }

SessionStep DisconnectedState::onClose(const ReasonCode i_code) {
  m_processor->setCloseCode(i_code);
  return moveTo(SESSION_STATE_CLOSED);
}

SessionStep DisconnectedState::onOpenStream(const OpenRequest&,
                                            uint32_t* const) {
  return stay(SESSION_STATUS_NOT_CONNECTED);
}

SessionStep DisconnectedState::onTransmitDrained() { return stay(); }

uint64_t DisconnectedState::nextDeadlineMs() const { return NO_DEADLINE; }

}  // namespace packetmux
