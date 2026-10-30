#include "stream/state/HalfClosedRemoteState.h"

namespace packetmux {

StreamState HalfClosedRemoteState::id() const {
  return STREAM_STATE_HALF_CLOSED_REMOTE;
}

StreamStep HalfClosedRemoteState::onWrite(
    const StreamContext& i_context, const std::span<const uint8_t> i_bytes,
    size_t* const o_accepted) const {
  i_context.processor->write(i_context.record, i_bytes, o_accepted);
  return stay();
}

StreamStep HalfClosedRemoteState::onSendEof(
    const StreamContext& i_context) const {
  i_context.processor->queueEof(i_context.record->streamId);
  return closeWith(REASON_CODE_OK);
}

}  // namespace packetmux
