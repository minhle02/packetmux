#pragma once

#include "stream/state/StreamStateBase.h"

namespace packetmux {

/**
 * We sent OPEN and EOF, and still wait for the answer.
 */
class SenderOpeningEofSentState : public StreamStateBase {
 public:
  StreamState id() const override;
  StreamStep onOpenOk(const StreamContext& i_context) const override;
  StreamStep onOpenErr(const StreamContext& i_context,
                       const ReasonCode i_code) const override;

 private:
  friend class StreamStateMachine;

  SenderOpeningEofSentState() = default;
};

}  // namespace packetmux
