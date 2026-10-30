#pragma once

#include "stream/state/StreamStateBase.h"

namespace packetmux {

/**
 * The peer sent OPEN and EOF, and the host has not answered.
 */
class ReceiverOpeningEofReceivedState : public StreamStateBase {
 public:
  StreamState id() const override;
  StreamStep onAccept(const StreamContext& i_context) const override;
  StreamStep onReject(const StreamContext& i_context, const ReasonCode i_code,
                      const std::span<const uint8_t> i_message) const override;

 private:
  friend class StreamStateMachine;

  ReceiverOpeningEofReceivedState() = default;
};

}  // namespace packetmux
