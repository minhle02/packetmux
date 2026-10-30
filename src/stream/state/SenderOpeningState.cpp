#include "stream/state/SenderOpeningState.h"

namespace packetmux {

StreamState SenderOpeningState::id() const {
  return STREAM_STATE_SENDER_OPENING;
}

StreamStep SenderOpeningState::onOpenOk(const StreamContext& i_context) const {
  i_context.processor->emitAccepted(i_context.record->streamId);
  return moveTo(STREAM_STATE_OPEN);
}

StreamStep SenderOpeningState::onOpenErr(const StreamContext& i_context,
                                         const ReasonCode i_code) const {
  i_context.processor->dropQueue(i_context.record->streamId);
  return closeWith(i_code);
}

StreamStep SenderOpeningState::onWrite(const StreamContext& i_context,
                                       const std::span<const uint8_t> i_bytes,
                                       size_t* const o_accepted) const {
  i_context.processor->write(i_context.record, i_bytes, o_accepted);
  return stay();
}

StreamStep SenderOpeningState::onSendEof(const StreamContext& i_context) const {
  i_context.processor->queueEof(i_context.record->streamId);
  return moveTo(STREAM_STATE_SENDER_OPENING_EOF_SENT);
}

}  // namespace packetmux
