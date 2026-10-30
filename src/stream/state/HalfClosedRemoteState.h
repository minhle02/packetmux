#pragma once

#include "stream/state/StreamStateBase.h"

namespace packetmux {

/**
 * Accepted, and the peer sent EOF. Only we can still send.
 */
class HalfClosedRemoteState : public StreamStateBase {
 public:
  StreamState id() const override;
  StreamStep onWrite(const StreamContext& i_context,
                     const std::span<const uint8_t> i_bytes,
                     size_t* const o_accepted) const override;
  StreamStep onSendEof(const StreamContext& i_context) const override;

 private:
  friend class StreamStateMachine;

  HalfClosedRemoteState() = default;
};

}  // namespace packetmux
