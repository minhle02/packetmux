#pragma once

#include "stream/state/StreamStateBase.h"

namespace packetmux {

/**
 * We sent OPEN and wait for the answer. DATA may be pipelined.
 */
class SenderOpeningState : public StreamStateBase {
 public:
  StreamState id() const override;
  StreamStep onOpenOk(const StreamContext& i_context) const override;
  StreamStep onOpenErr(const StreamContext& i_context,
                       const ReasonCode i_code) const override;
  StreamStep onWrite(const StreamContext& i_context,
                     const std::span<const uint8_t> i_bytes,
                     size_t* const o_accepted) const override;
  StreamStep onSendEof(const StreamContext& i_context) const override;

 private:
  friend class StreamStateMachine;

  SenderOpeningState() = default;
};

}  // namespace packetmux
