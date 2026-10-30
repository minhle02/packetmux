#include "stream/state/HalfClosedLocalState.h"

namespace packetmux {

StreamState HalfClosedLocalState::id() const {
  return STREAM_STATE_HALF_CLOSED_LOCAL;
}

StreamStep HalfClosedLocalState::onData(
    const StreamContext& i_context,
    const std::span<const uint8_t> i_data) const {
  i_context.processor->deliverData(i_context.record, i_data);
  return stay();
}

StreamStep HalfClosedLocalState::onEof(const StreamContext& i_context) const {
  i_context.processor->emitEof(i_context.record->streamId);
  return closeWith(REASON_CODE_OK);
}

}  // namespace packetmux
