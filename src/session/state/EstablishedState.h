#pragma once

#include "session/state/SessionStateBase.h"

namespace packetmux {

/**
 * Normal operation: frames are dispatched and keepalive runs.
 */
class EstablishedState : public SessionStateBase {
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

  explicit EstablishedState(SessionProcessor* const o_processor);

  SessionStep failConnection(const CodecStatus i_status);
};

}  // namespace packetmux
