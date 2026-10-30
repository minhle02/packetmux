#include "stream/state/StreamStateBase.h"

namespace packetmux {

StreamStep StreamStateBase::onOpenOk(const StreamContext&) const {
  return invalid();
}

StreamStep StreamStateBase::onOpenErr(const StreamContext&,
                                      const ReasonCode) const {
  return invalid();
}

StreamStep StreamStateBase::onData(const StreamContext&,
                                   const std::span<const uint8_t>) const {
  return invalid();
}

StreamStep StreamStateBase::onEof(const StreamContext&) const {
  return invalid();
}

StreamStep StreamStateBase::onRst(const StreamContext& i_context,
                                  const ReasonCode i_code) const {
  i_context.processor->dropQueue(i_context.record->streamId);
  return closeWith(StreamProcessor::reasonForStreams(i_code));
}

StreamStep StreamStateBase::onAccept(const StreamContext&) const {
  return invalid();
}

StreamStep StreamStateBase::onReject(const StreamContext&, const ReasonCode,
                                     const std::span<const uint8_t>) const {
  return invalid();
}

StreamStep StreamStateBase::onWrite(const StreamContext&,
                                    const std::span<const uint8_t>,
                                    size_t* const) const {
  return invalid();
}

StreamStep StreamStateBase::onSendEof(const StreamContext&) const {
  return invalid();
}

StreamStep StreamStateBase::onReset(const StreamContext& i_context,
                                    const ReasonCode i_code) const {
  const uint32_t streamId = i_context.record->streamId;
  i_context.processor->dropQueue(streamId);
  i_context.processor->queueRst(streamId, i_code);
  return closeWith(StreamProcessor::reasonForStreams(i_code));
}

StreamStep StreamStateBase::stay() { return {}; }

StreamStep StreamStateBase::moveTo(const StreamState i_next) {
  return {.next = i_next};
}

StreamStep StreamStateBase::closeWith(const ReasonCode i_code) {
  return {.next = STREAM_STATE_CLOSED, .closeCode = i_code};
}

StreamStep StreamStateBase::invalid() { return {.valid = false}; }

}  // namespace packetmux
