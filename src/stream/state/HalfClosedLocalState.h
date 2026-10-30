#pragma once

#include "stream/state/StreamStateBase.h"

namespace packetmux {

/**
 * Accepted, and we sent EOF. Only the peer can still send.
 */
class HalfClosedLocalState : public StreamStateBase {
 public:
  StreamState id() const override;
  StreamStep onData(const StreamContext& i_context,
                    const std::span<const uint8_t> i_data) const override;
  StreamStep onEof(const StreamContext& i_context) const override;

 private:
  friend class StreamStateMachine;

  HalfClosedLocalState() = default;
};

}  // namespace packetmux
