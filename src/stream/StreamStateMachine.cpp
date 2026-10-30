#include "stream/StreamStateMachine.h"

#include "stream/state/HalfClosedLocalState.h"
#include "stream/state/HalfClosedRemoteState.h"
#include "stream/state/OpenState.h"
#include "stream/state/ReceiverOpeningEofReceivedState.h"
#include "stream/state/ReceiverOpeningState.h"
#include "stream/state/SenderOpeningEofSentState.h"
#include "stream/state/SenderOpeningState.h"

namespace packetmux {

StreamStateMachine::StreamStateMachine(const StreamRecord& i_record,
                                       const StreamState i_initialState,
                                       StreamProcessor* const o_processor)
    : m_state(stateFor(i_initialState)),
      m_record(i_record),
      m_processor(o_processor) {}

StreamState StreamStateMachine::state() const { return m_state->id(); }

void StreamStateMachine::setState(const StreamState i_state) {
  m_state = stateFor(i_state);
}

StreamStep StreamStateMachine::onFrame(const FrameView& i_frame) {
  switch (i_frame.type) {
    case FRAME_TYPE_OPEN_OK:
      return m_state->onOpenOk(context());
    case FRAME_TYPE_OPEN_ERR:
      return m_state->onOpenErr(context(), i_frame.code);
    case FRAME_TYPE_DATA:
      return m_state->onData(context(), i_frame.data);
    case FRAME_TYPE_EOF:
      return m_state->onEof(context());
    case FRAME_TYPE_RST:
      return m_state->onRst(context(), i_frame.code);
    default:
      return {.valid = false};  // connection frames never reach a stream
  }
}

StreamStep StreamStateMachine::onAccept() {
  return m_state->onAccept(context());
}

StreamStep StreamStateMachine::onReject(
    const ReasonCode i_code, const std::span<const uint8_t> i_message) {
  return m_state->onReject(context(), i_code, i_message);
}

StreamStep StreamStateMachine::onWrite(const std::span<const uint8_t> i_bytes,
                                       size_t* const o_accepted) {
  return m_state->onWrite(context(), i_bytes, o_accepted);
}

StreamStep StreamStateMachine::onSendEof() {
  return m_state->onSendEof(context());
}

StreamStep StreamStateMachine::onReset(const ReasonCode i_code) {
  return m_state->onReset(context(), i_code);
}

StreamInfo StreamStateMachine::info() const {
  return {.streamId = m_record.streamId,
          .kind = m_record.kind,
          .opener = m_record.opener,
          .state = state(),
          .bytesSent = m_record.bytesSent,
          .bytesReceived = m_record.bytesReceived,
          .createdAtMs = m_record.createdAtMs,
          .lastActivityMs = m_record.lastActivityMs};
}

const StreamStateBase* StreamStateMachine::stateFor(const StreamState i_state) {
  // States hold no data, so one const instance of each serves every stream.
  static const SenderOpeningState SENDER_OPENING{};
  static const SenderOpeningEofSentState SENDER_OPENING_EOF_SENT{};
  static const ReceiverOpeningState RECEIVER_OPENING{};
  static const ReceiverOpeningEofReceivedState RECEIVER_OPENING_EOF_RECEIVED{};
  static const OpenState OPEN{};
  static const HalfClosedLocalState HALF_CLOSED_LOCAL{};
  static const HalfClosedRemoteState HALF_CLOSED_REMOTE{};
  // In StreamState order. CLOSED has no class: reaching it erases the stream.
  static const StreamStateBase* const STATES[STREAM_STATE_CLOSED] = {
      &SENDER_OPENING,
      &SENDER_OPENING_EOF_SENT,
      &RECEIVER_OPENING,
      &RECEIVER_OPENING_EOF_RECEIVED,
      &OPEN,
      &HALF_CLOSED_LOCAL,
      &HALF_CLOSED_REMOTE};
  return STATES[i_state];
}

StreamContext StreamStateMachine::context() {
  return {.record = &m_record, .processor = m_processor};
}

}  // namespace packetmux
