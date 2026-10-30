#pragma once

#include "session/state/SessionStateBase.h"

namespace packetmux {

/**
 * The connection failed. We wait for the host to take the GOAWAY, then return
 * to DISCONNECTED. The session itself survives.
 */
class DrainingState : public SessionStateBase {
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

  explicit DrainingState(SessionProcessor* const o_processor);

  uint64_t m_deadlineMs;
};

}  // namespace packetmux
