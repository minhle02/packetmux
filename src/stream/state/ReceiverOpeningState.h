#pragma once

#include "stream/state/StreamStateBase.h"

namespace packetmux {

/**
 * The peer sent OPEN and the host has not answered. Inbound DATA is delivered.
 */
class ReceiverOpeningState : public StreamStateBase {
 public:
  StreamState id() const override;
  StreamStep onData(const StreamContext& i_context,
                    const std::span<const uint8_t> i_data) const override;
  StreamStep onEof(const StreamContext& i_context) const override;
  StreamStep onAccept(const StreamContext& i_context) const override;
  StreamStep onReject(const StreamContext& i_context, const ReasonCode i_code,
                      const std::span<const uint8_t> i_message) const override;

 private:
  friend class StreamStateMachine;

  ReceiverOpeningState() = default;
};

}  // namespace packetmux
