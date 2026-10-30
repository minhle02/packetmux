#include "stream/state/OpenState.h"

namespace packetmux {

StreamState OpenState::id() const { return STREAM_STATE_OPEN; }

StreamStep OpenState::onData(const StreamContext& i_context,
                             const std::span<const uint8_t> i_data) const {
  i_context.processor->deliverData(i_context.record, i_data);
  return stay();
}

StreamStep OpenState::onEof(const StreamContext& i_context) const {
  i_context.processor->emitEof(i_context.record->streamId);
  return moveTo(STREAM_STATE_HALF_CLOSED_REMOTE);
}

StreamStep OpenState::onWrite(const StreamContext& i_context,
                              const std::span<const uint8_t> i_bytes,
                              size_t* const o_accepted) const {
  i_context.processor->write(i_context.record, i_bytes, o_accepted);
  return stay();
}

StreamStep OpenState::onSendEof(const StreamContext& i_context) const {
  i_context.processor->queueEof(i_context.record->streamId);
  return moveTo(STREAM_STATE_HALF_CLOSED_LOCAL);
}

}  // namespace packetmux
