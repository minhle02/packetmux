#include "session/SessionStateMachine.h"

namespace packetmux {

SessionStateMachine::SessionStateMachine(const SessionConfig& i_config)
    : m_processor(i_config),
      m_disconnected(&m_processor),
      m_handshaking(&m_processor),
      m_established(&m_processor),
      m_draining(&m_processor),
      m_closing(&m_processor),
      m_closed(&m_processor),
      m_states{&m_disconnected, &m_handshaking, &m_established,
               &m_draining,     &m_closing,     &m_closed},
      m_current(&m_disconnected) {}

SessionStatus SessionStateMachine::attachConnection(const uint64_t i_nowMs) {
  m_processor.setNow(i_nowMs);
  return apply(m_current->onAttach());
}

SessionStatus SessionStateMachine::onMessage(
    const std::span<const uint8_t> i_message, const uint64_t i_nowMs) {
  // Events borrow the previous message, so it must be finished with first.
  if (m_processor.eventsPending()) {
    return SESSION_STATUS_EVENTS_PENDING;
  }
  m_processor.setNow(i_nowMs);
  return apply(m_current->onMessage(i_message));
}

SessionStatus SessionStateMachine::onConnectionLost() {
  return apply(m_current->onConnectionLost());
}

SessionStatus SessionStateMachine::onTick(const uint64_t i_nowMs) {
  m_processor.setNow(i_nowMs);
  return apply(m_current->onTick());
}

uint64_t SessionStateMachine::nextDeadlineMs() const {
  return m_current->nextDeadlineMs();
}

SessionStatus SessionStateMachine::openStream(
    const std::string_view i_kind, const std::span<const uint8_t> i_params,
    const uint64_t i_nowMs, uint32_t* const o_streamId) {
  m_processor.setNow(i_nowMs);
  return apply(m_current->onOpenStream({i_kind, i_params}, o_streamId));
}

SessionStatus SessionStateMachine::acceptStream(const uint32_t i_streamId) {
  return m_processor.streams().accept(i_streamId);
}

SessionStatus SessionStateMachine::rejectStream(
    const uint32_t i_streamId, const ReasonCode i_code,
    const std::span<const uint8_t> i_message) {
  if (!SessionProcessor::isSendable(i_code)) {
    return SESSION_STATUS_INVALID_ARGUMENT;
  }
  return m_processor.streams().reject(i_streamId, i_code, i_message);
}

SessionStatus SessionStateMachine::write(const uint32_t i_streamId,
                                         const std::span<const uint8_t> i_bytes,
                                         const uint64_t i_nowMs,
                                         size_t* const o_accepted) {
  m_processor.setNow(i_nowMs);
  return m_processor.streams().write(i_streamId, i_bytes, o_accepted);
}

SessionStatus SessionStateMachine::consume(const uint32_t i_streamId,
                                           const uint64_t i_bytes) {
  return m_processor.streams().consume(i_streamId, i_bytes);
}

SessionStatus SessionStateMachine::sendEof(const uint32_t i_streamId) {
  return m_processor.streams().sendEof(i_streamId);
}

SessionStatus SessionStateMachine::resetStream(const uint32_t i_streamId,
                                               const ReasonCode i_code) {
  if (!SessionProcessor::isSendable(i_code)) {
    return SESSION_STATUS_INVALID_ARGUMENT;
  }
  return m_processor.streams().reset(i_streamId, i_code);
}

SessionStatus SessionStateMachine::close(const ReasonCode i_code,
                                         const uint64_t i_nowMs) {
  if (!SessionProcessor::isSendable(i_code)) {
    return SESSION_STATUS_INVALID_ARGUMENT;
  }
  m_processor.setNow(i_nowMs);
  return apply(m_current->onClose(i_code));
}

bool SessionStateMachine::pollTransmit(
    std::span<const uint8_t>* const o_message) {
  if (!m_processor.popFrame(o_message)) {
    return false;
  }
  if (m_processor.transmitQueueEmpty()) {
    apply(m_current->onTransmitDrained());
  }
  return true;
}

bool SessionStateMachine::pollEvent(Event* const o_event) {
  return m_processor.pollEvent(o_event);
}

SessionState SessionStateMachine::state() const { return m_current->id(); }

void SessionStateMachine::listStreams(
    std::vector<StreamInfo>* const o_streams) const {
  m_processor.streams().list(o_streams);
}

bool SessionStateMachine::getRttMs(uint64_t* const o_rttMs) const {
  return m_processor.keepAlive().getRttMs(o_rttMs);
}

SessionStatus SessionStateMachine::apply(const SessionStep& i_step) {
  if (i_step.next != SESSION_STATE_MAX) {
    m_current = m_states[i_step.next];
    m_current->onEnter();
  }
  return i_step.status;
}

}  // namespace packetmux
