#include "packetmux/Session.h"

#include "session/SessionStateMachine.h"

namespace packetmux {

Session::Session(const SessionConfig& i_config)
    : m_stateMachine(std::make_unique<SessionStateMachine>(i_config)) {}

Session::~Session() = default;

Session::Session(Session&&) noexcept = default;

Session& Session::operator=(Session&&) noexcept = default;

SessionStatus Session::attachConnection(const uint64_t i_nowMs) {
  return m_stateMachine->attachConnection(i_nowMs);
}

SessionStatus Session::onMessage(const std::span<const uint8_t> i_message,
                                 const uint64_t i_nowMs) {
  return m_stateMachine->onMessage(i_message, i_nowMs);
}

SessionStatus Session::onConnectionLost() {
  return m_stateMachine->onConnectionLost();
}

SessionStatus Session::onTick(const uint64_t i_nowMs) {
  return m_stateMachine->onTick(i_nowMs);
}

uint64_t Session::nextDeadlineMs() const {
  return m_stateMachine->nextDeadlineMs();
}

SessionStatus Session::openStream(const std::string_view i_kind,
                                  const std::span<const uint8_t> i_params,
                                  const uint64_t i_nowMs,
                                  uint32_t* const o_streamId) {
  return m_stateMachine->openStream(i_kind, i_params, i_nowMs, o_streamId);
}

SessionStatus Session::acceptStream(const uint32_t i_streamId) {
  return m_stateMachine->acceptStream(i_streamId);
}

SessionStatus Session::rejectStream(const uint32_t i_streamId,
                                    const ReasonCode i_code,
                                    const std::span<const uint8_t> i_message) {
  return m_stateMachine->rejectStream(i_streamId, i_code, i_message);
}

SessionStatus Session::write(const uint32_t i_streamId,
                             const std::span<const uint8_t> i_bytes,
                             const uint64_t i_nowMs, size_t* const o_accepted) {
  return m_stateMachine->write(i_streamId, i_bytes, i_nowMs, o_accepted);
}

SessionStatus Session::sendEof(const uint32_t i_streamId) {
  return m_stateMachine->sendEof(i_streamId);
}

SessionStatus Session::resetStream(const uint32_t i_streamId,
                                   const ReasonCode i_code) {
  return m_stateMachine->resetStream(i_streamId, i_code);
}

SessionStatus Session::close(const ReasonCode i_code, const uint64_t i_nowMs) {
  return m_stateMachine->close(i_code, i_nowMs);
}

bool Session::pollTransmit(std::span<const uint8_t>* const o_message) {
  return m_stateMachine->pollTransmit(o_message);
}

bool Session::pollEvent(Event* const o_event) {
  return m_stateMachine->pollEvent(o_event);
}

SessionState Session::state() const { return m_stateMachine->state(); }

void Session::listStreams(std::vector<StreamInfo>* const o_streams) const {
  m_stateMachine->listStreams(o_streams);
}

bool Session::getRttMs(uint64_t* const o_rttMs) const {
  return m_stateMachine->getRttMs(o_rttMs);
}

}  // namespace packetmux
