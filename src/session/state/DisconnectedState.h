#pragma once

#include "session/state/SessionStateBase.h"

namespace packetmux {

/**
 * No connection. Session data and the stream ID counter are kept, so this is a
 * normal resting state. It is the initial state, entered without onEnter.
 */
class DisconnectedState : public SessionStateBase {
 public:
  SessionState id() const override;
  void onEnter() override;
  SessionStep onAttach() override;
  SessionStep onMessage(const std::span<const uint8_t> i_message) override;
  SessionStep onConnectionLost() override;
  SessionStep onTick() override;
  SessionStep onClose(const ReasonCode i_code) override;
  SessionStep onOpenStream(const OpenRequest& i_request,
                           uint32_t* const o_streamId) override;
  SessionStep onTransmitDrained() override;
  uint64_t nextDeadlineMs() const override;

 private:
  friend class SessionStateMachine;

  explicit DisconnectedState(SessionProcessor* const o_processor);
};

}  // namespace packetmux
