#include "stream/state/SenderOpeningEofSentState.h"

namespace packetmux {

StreamState SenderOpeningEofSentState::id() const {
  return STREAM_STATE_SENDER_OPENING_EOF_SENT;
}

StreamStep SenderOpeningEofSentState::onOpenOk(
    const StreamContext& i_context) const {
  i_context.processor->emitAccepted(i_context.record->streamId);
  return moveTo(STREAM_STATE_HALF_CLOSED_LOCAL);
}

StreamStep SenderOpeningEofSentState::onOpenErr(const StreamContext& i_context,
                                                const ReasonCode i_code) const {
  i_context.processor->dropQueue(i_context.record->streamId);
  return closeWith(i_code);
}

}  // namespace packetmux
