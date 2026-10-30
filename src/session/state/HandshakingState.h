#pragma once

#include "session/state/SessionStateBase.h"

namespace packetmux {

/**
 * Our HELLO is queued, and we wait for the peer's. Streams may already be
 * opened: their frames go out after HELLO.
 */
class HandshakingState : public SessionStateBase {
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

  explicit HandshakingState(SessionProcessor* const o_processor);

  uint64_t m_deadlineMs;
};

}  // namespace packetmux
