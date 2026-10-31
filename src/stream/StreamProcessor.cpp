#include "stream/StreamProcessor.h"

#include <algorithm>
#include <utility>

#include "packetmux/Codec.h"

namespace packetmux {
namespace {

size_t spaceLeft(const size_t i_limit, const size_t i_used) {
  return i_used < i_limit ? i_limit - i_used : 0U;
}

}  // namespace

StreamProcessor::StreamProcessor(const SessionConfig& i_config,
                                 Scheduler* const o_scheduler,
                                 EventQueue* const o_events)
    : m_config(i_config),
      m_scheduler(o_scheduler),
      m_events(o_events),
      m_peerReceiveWindow(0U),
      m_nowMs(0U) {}

void StreamProcessor::setNow(const uint64_t i_nowMs) { m_nowMs = i_nowMs; }

uint64_t StreamProcessor::nowMs() const { return m_nowMs; }

void StreamProcessor::write(StreamRecord* const o_record,
                            const std::span<const uint8_t> i_bytes,
                            size_t* const o_accepted) {
  const uint32_t streamId = o_record->streamId;
  const size_t room = std::min(i_bytes.size(), roomFor(streamId));

  size_t offset = 0U;
  // At most half the peer's window: a parked stream then always has at least
  // that much with the receiver, which is enough to trigger a grant.
  const size_t maxChunk = std::min<size_t>(m_config.limits.maxDataPayload,
                                           m_peerReceiveWindow / 2U);
  while (offset < room) {
    const size_t chunkSize = std::min(room - offset, maxChunk);
    std::vector<uint8_t> message;
    Codec::encode({.type = FRAME_TYPE_DATA,
                   .streamId = streamId,
                   .data = i_bytes.subspan(offset, chunkSize)},
                  m_config.limits, &message);
    m_scheduler->pushStreamFrame(streamId, std::move(message), chunkSize);
    offset += chunkSize;
  }

  if (room > 0U) {
    o_record->bytesSent += room;
    o_record->lastActivityMs = m_nowMs;
  }
  if (room < i_bytes.size() &&
      std::find(m_blockedStreams.begin(), m_blockedStreams.end(), streamId) ==
          m_blockedStreams.end()) {
    m_blockedStreams.push_back(streamId);
  }
  *o_accepted = room;
}

void StreamProcessor::deliverData(StreamRecord* const o_record,
                                  const std::span<const uint8_t> i_data) {
  o_record->bytesReceived += i_data.size();
  o_record->lastActivityMs = m_nowMs;
  m_events->push({.type = EVENT_TYPE_DATA,
                  .streamId = o_record->streamId,
                  .data = i_data});
}

void StreamProcessor::emitOpened(const FrameView& i_frame) {
  m_events->push({.type = EVENT_TYPE_STREAM_OPENED,
                  .streamId = i_frame.streamId,
                  .kind = i_frame.kind,
                  .params = i_frame.params});
}

void StreamProcessor::emitAccepted(const uint32_t i_streamId) {
  m_events->push({.type = EVENT_TYPE_STREAM_ACCEPTED, .streamId = i_streamId});
}

void StreamProcessor::setPeerReceiveWindow(const uint32_t i_window) {
  m_peerReceiveWindow = i_window;
}

void StreamProcessor::openCredit(const uint32_t i_streamId) {
  m_scheduler->openCredit(i_streamId, m_peerReceiveWindow);
}

void StreamProcessor::retireCredit(const uint32_t i_streamId) {
  m_scheduler->retireCredit(i_streamId);
}

bool StreamProcessor::takePeerCredit(StreamRecord* const o_record,
                                     const size_t i_bytes) {
  if (i_bytes > o_record->peerCredit) {
    return false;
  }
  o_record->peerCredit -= i_bytes;
  return true;
}

bool StreamProcessor::onWindow(const uint32_t i_streamId,
                               const uint32_t i_credit) {
  return m_scheduler->addCredit(i_streamId, i_credit, m_peerReceiveWindow);
}

bool StreamProcessor::consume(StreamRecord* const o_record,
                              const uint64_t i_bytes) {
  const uint64_t held =
      m_config.receiveWindow - o_record->peerCredit - o_record->consumed;
  if (i_bytes > held) {
    return false;
  }
  o_record->consumed += i_bytes;
  if (o_record->consumed >= m_config.receiveWindow / 2U &&
      !o_record->remoteEof) {
    queueControl({.type = FRAME_TYPE_WINDOW,
                  .streamId = o_record->streamId,
                  .credit = static_cast<uint32_t>(o_record->consumed)});
    o_record->peerCredit += o_record->consumed;
    o_record->consumed = 0U;
  }
  return true;
}

uint64_t StreamProcessor::sendCredit(const uint32_t i_streamId) const {
  return m_scheduler->credit(i_streamId);
}

void StreamProcessor::emitEof(const uint32_t i_streamId) {
  m_events->push({.type = EVENT_TYPE_EOF, .streamId = i_streamId});
}

void StreamProcessor::emitClosed(const uint32_t i_streamId,
                                 const ReasonCode i_code) {
  m_events->push({.type = EVENT_TYPE_STREAM_CLOSED,
                  .streamId = i_streamId,
                  .code = i_code});
}

bool StreamProcessor::queueOpen(const uint32_t i_streamId,
                                const std::string_view i_kind,
                                const std::span<const uint8_t> i_params) {
  return queueControl({.type = FRAME_TYPE_OPEN,
                       .streamId = i_streamId,
                       .kind = i_kind,
                       .params = i_params});
}

bool StreamProcessor::queueOpenErr(const uint32_t i_streamId,
                                   const ReasonCode i_code,
                                   const std::span<const uint8_t> i_message) {
  return queueControl({.type = FRAME_TYPE_OPEN_ERR,
                       .streamId = i_streamId,
                       .code = i_code,
                       .message = i_message});
}

void StreamProcessor::queueOpenOk(const uint32_t i_streamId) {
  queueControl({.type = FRAME_TYPE_OPEN_OK, .streamId = i_streamId});
}

void StreamProcessor::queueEof(const uint32_t i_streamId) {
  forgetBlocked(i_streamId);  // it can no longer be written to

  std::vector<uint8_t> message;
  Codec::encode({.type = FRAME_TYPE_EOF, .streamId = i_streamId},
                m_config.limits, &message);
  // Through the stream's queue, so EOF cannot overtake the stream's DATA.
  m_scheduler->pushStreamFrame(i_streamId, std::move(message), 0U);
}

void StreamProcessor::queueRst(const uint32_t i_streamId,
                               const ReasonCode i_code) {
  queueControl(
      {.type = FRAME_TYPE_RST, .streamId = i_streamId, .code = i_code});
}

void StreamProcessor::dropQueue(const uint32_t i_streamId) {
  forgetBlocked(i_streamId);  // this stream is ending
  m_scheduler->dropStream(i_streamId);
  notifyWritable();  // room returned without a frame being sent
}

void StreamProcessor::forgetBlocked(const uint32_t i_streamId) {
  m_blockedStreams.erase(
      std::remove(m_blockedStreams.begin(), m_blockedStreams.end(), i_streamId),
      m_blockedStreams.end());
}

void StreamProcessor::notifyWritable() {
  std::vector<uint32_t> stillBlocked;
  for (const uint32_t streamId : m_blockedStreams) {
    if (roomFor(streamId) == 0U) {
      stillBlocked.push_back(streamId);
    } else {
      m_events->push({.type = EVENT_TYPE_WRITABLE, .streamId = streamId});
    }
  }
  m_blockedStreams = std::move(stillBlocked);
}

ReasonCode StreamProcessor::reasonForStreams(const ReasonCode i_code) {
  return i_code == REASON_CODE_OK ? REASON_CODE_PEER_CLOSED : i_code;
}

size_t StreamProcessor::roomFor(const uint32_t i_streamId) const {
  if (m_peerReceiveWindow == 0U) {
    return 0U;  // frames are cut to fit the peer's window, not known yet
  }
  return std::min(
      spaceLeft(m_config.maxQueuedBytesPerStream,
                m_scheduler->queuedBytes(i_streamId)),
      spaceLeft(m_config.maxQueuedBytes, m_scheduler->totalQueuedBytes()));
}

bool StreamProcessor::queueControl(const FrameView& i_frame) {
  std::vector<uint8_t> message;
  if (Codec::encode(i_frame, m_config.limits, &message) != CODEC_STATUS_OK) {
    return false;
  }
  m_scheduler->pushControl(std::move(message));
  return true;
}

}  // namespace packetmux
