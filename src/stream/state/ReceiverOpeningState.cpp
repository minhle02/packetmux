#include "stream/state/ReceiverOpeningState.h"

namespace packetmux {

StreamState ReceiverOpeningState::id() const {
  return STREAM_STATE_RECEIVER_OPENING;
}

StreamStep ReceiverOpeningState::onData(
    const StreamContext& i_context,
    const std::span<const uint8_t> i_data) const {
  i_context.processor->deliverData(i_context.record, i_data);
  return stay();
}

StreamStep ReceiverOpeningState::onEof(const StreamContext& i_context) const {
  i_context.processor->emitEof(i_context.record->streamId);
  return moveTo(STREAM_STATE_RECEIVER_OPENING_EOF_RECEIVED);
}

StreamStep ReceiverOpeningState::onAccept(
    const StreamContext& i_context) const {
  i_context.processor->queueOpenOk(i_context.record->streamId);
  return moveTo(STREAM_STATE_OPEN);
}

StreamStep ReceiverOpeningState::onReject(
    const StreamContext& i_context, const ReasonCode i_code,
    const std::span<const uint8_t> i_message) const {
  i_context.processor->queueOpenErr(i_context.record->streamId, i_code,
                                    i_message);
  return closeWith(i_code);
}

}  // namespace packetmux
