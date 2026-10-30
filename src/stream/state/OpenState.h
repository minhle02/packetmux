#pragma once

#include "stream/state/StreamStateBase.h"

namespace packetmux {

/**
 * Accepted, and both directions are open.
 */
class OpenState : public StreamStateBase {
 public:
  StreamState id() const override;
  StreamStep onData(const StreamContext& i_context,
                    const std::span<const uint8_t> i_data) const override;
  StreamStep onEof(const StreamContext& i_context) const override;
  StreamStep onWrite(const StreamContext& i_context,
                     const std::span<const uint8_t> i_bytes,
                     size_t* const o_accepted) const override;
  StreamStep onSendEof(const StreamContext& i_context) const override;

 private:
  friend class StreamStateMachine;

  OpenState() = default;
};

}  // namespace packetmux
