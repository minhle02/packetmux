#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "packetmux/SessionTypes.h"
#include "session/SessionProcessor.h"
#include "session/state/ClosedState.h"
#include "session/state/ClosingState.h"
#include "session/state/DisconnectedState.h"
#include "session/state/DrainingState.h"
#include "session/state/EstablishedState.h"
#include "session/state/HandshakingState.h"

namespace packetmux {

/**
 * Owns the session states and the shared processor. Each event goes to the
 * current state, which returns a step, and apply() is the only place the state
 * changes. Only this class creates session states, and it creates each once.
 *
 * Calls on existing streams do not pass through the states: streams only exist
 * while connected, so in any other state the ID is simply unknown.
 */
class SessionStateMachine {
 public:
  explicit SessionStateMachine(const SessionConfig& i_config);

  SessionStatus attachConnection(const uint64_t i_nowMs);
  SessionStatus onMessage(const std::span<const uint8_t> i_message,
                          const uint64_t i_nowMs);
  SessionStatus onConnectionLost();
  SessionStatus onTick(const uint64_t i_nowMs);
  uint64_t nextDeadlineMs() const;

  SessionStatus openStream(const std::string_view i_kind,
                           const std::span<const uint8_t> i_params,
                           const uint64_t i_nowMs, uint32_t* const o_streamId);
  SessionStatus acceptStream(const uint32_t i_streamId);
  SessionStatus rejectStream(const uint32_t i_streamId, const ReasonCode i_code,
                             const std::span<const uint8_t> i_message);
  SessionStatus write(const uint32_t i_streamId,
                      const std::span<const uint8_t> i_bytes,
                      const uint64_t i_nowMs, size_t* const o_accepted);
  SessionStatus consume(const uint32_t i_streamId, const uint64_t i_bytes);
  SessionStatus sendEof(const uint32_t i_streamId);
  SessionStatus resetStream(const uint32_t i_streamId, const ReasonCode i_code);
  SessionStatus close(const ReasonCode i_code, const uint64_t i_nowMs);

  bool pollTransmit(std::span<const uint8_t>* const o_message);
  bool pollEvent(Event* const o_event);

  SessionState state() const;
  void listStreams(std::vector<StreamInfo>* const o_streams) const;
  bool getRttMs(uint64_t* const o_rttMs) const;

 private:
  SessionStatus apply(const SessionStep& i_step);

  SessionProcessor m_processor;
  DisconnectedState m_disconnected;
  HandshakingState m_handshaking;
  EstablishedState m_established;
  DrainingState m_draining;
  ClosingState m_closing;
  ClosedState m_closed;
  std::array<SessionStateBase*, SESSION_STATE_MAX> m_states;  // by SessionState
  SessionStateBase* m_current;
};

}  // namespace packetmux
