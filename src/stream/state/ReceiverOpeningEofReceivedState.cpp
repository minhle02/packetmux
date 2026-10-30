#include "stream/state/ReceiverOpeningEofReceivedState.h"

namespace packetmux {

StreamState ReceiverOpeningEofReceivedState::id() const {
  return STREAM_STATE_RECEIVER_OPENING_EOF_RECEIVED;
}

StreamStep ReceiverOpeningEofReceivedState::onAccept(
    const StreamContext& i_context) const {
  i_context.processor->queueOpenOk(i_context.record->streamId);
  return moveTo(STREAM_STATE_HALF_CLOSED_REMOTE);
}

StreamStep ReceiverOpeningEofReceivedState::onReject(
    const StreamContext& i_context, const ReasonCode i_code,
    const std::span<const uint8_t> i_message) const {
  i_context.processor->queueOpenErr(i_context.record->streamId, i_code,
                                    i_message);
  return closeWith(i_code);
}

}  // namespace packetmux
