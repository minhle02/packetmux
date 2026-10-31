#include "stream/StreamManager.h"

#include <limits>
#include <string>

namespace packetmux {
namespace {

constexpr uint64_t FIRST_CLIENT_STREAM_ID = 1U;
constexpr uint64_t FIRST_SERVER_STREAM_ID = 2U;
constexpr uint64_t STREAM_ID_STEP = 2U;
constexpr size_t OPEN_ERR_FIXED_SIZE = 5U;  // code (1) + message length (4)

Role peerOf(const Role i_role) {
  return i_role == ROLE_CLIENT ? ROLE_SERVER : ROLE_CLIENT;
}

// The client opens odd IDs and the server opens even IDs.
bool hasParityOf(const uint32_t i_streamId, const Role i_opener) {
  return (i_streamId % STREAM_ID_STEP == FIRST_CLIENT_STREAM_ID) ==
         (i_opener == ROLE_CLIENT);
}

}  // namespace

StreamManager::StreamManager(const SessionConfig& i_config,
                             Scheduler* const o_scheduler,
                             EventQueue* const o_events)
    : m_config(i_config),
      m_processor(i_config, o_scheduler, o_events),
      m_nextStreamId(i_config.role == ROLE_CLIENT ? FIRST_CLIENT_STREAM_ID
                                                  : FIRST_SERVER_STREAM_ID),
      m_highestPeerStreamId(0U) {}

void StreamManager::setNow(const uint64_t i_nowMs) {
  m_processor.setNow(i_nowMs);
}

SessionStatus StreamManager::open(const std::string_view i_kind,
                                  const std::span<const uint8_t> i_params,
                                  uint32_t* const o_streamId) {
  if (m_streams.size() >= m_config.maxStreams) {
    return SESSION_STATUS_LIMIT_EXCEEDED;
  }
  if (m_nextStreamId > std::numeric_limits<uint32_t>::max()) {
    return SESSION_STATUS_IDS_EXHAUSTED;
  }
  const uint32_t streamId = static_cast<uint32_t>(m_nextStreamId);
  if (!m_processor.queueOpen(streamId, i_kind, i_params)) {
    return SESSION_STATUS_INVALID_ARGUMENT;  // no ID is used
  }
  m_nextStreamId += STREAM_ID_STEP;
  add(streamId, i_kind, m_config.role, STREAM_STATE_SENDER_OPENING);
  *o_streamId = streamId;
  return SESSION_STATUS_OK;
}

bool StreamManager::onPeerOpen(const FrameView& i_frame) {
  const uint32_t streamId = i_frame.streamId;
  const Role peer = peerOf(m_config.role);
  if (!hasParityOf(streamId, peer) || streamId <= m_highestPeerStreamId) {
    return false;
  }
  m_highestPeerStreamId = streamId;

  if (m_streams.size() >= m_config.maxStreams) {
    m_processor.queueOpenErr(streamId, REASON_CODE_LIMIT_EXCEEDED, {});
    return true;
  }
  add(streamId, i_frame.kind, peer, STREAM_STATE_RECEIVER_OPENING);
  m_processor.emitOpened(i_frame);
  return true;
}

void StreamManager::onPeerHello(const uint32_t i_receiveWindow) {
  m_processor.setPeerReceiveWindow(i_receiveWindow);
  for (const std::pair<const uint32_t, StreamStateMachine>& entry : m_streams) {
    m_processor.openCredit(entry.first);  // opened during the handshake
  }
  m_processor.notifyWritable();
}

bool StreamManager::onFrame(const FrameView& i_frame) {
  if (i_frame.type == FRAME_TYPE_WINDOW) {
    // Not the stream's state: data queued before a local close still needs it.
    return m_processor.onWindow(i_frame.streamId, i_frame.credit);
  }
  StreamStateMachine* const stream = find(i_frame.streamId);
  if (stream == nullptr) {
    return true;  // late frame, or a stream the host rejected
  }
  if (i_frame.type == FRAME_TYPE_DATA &&
      !stream->takePeerCredit(i_frame.data.size())) {
    return false;
  }
  const StreamStep step = stream->onFrame(i_frame);
  if (!step.valid) {
    failStream(i_frame.streamId);
    return true;
  }
  applyStep(i_frame.streamId, step);
  return true;
}

SessionStatus StreamManager::accept(const uint32_t i_streamId) {
  StreamStateMachine* const stream = find(i_streamId);
  if (stream == nullptr) {
    return SESSION_STATUS_UNKNOWN_STREAM;
  }
  const StreamStep step = stream->onAccept();
  if (!step.valid) {
    return SESSION_STATUS_INVALID_STATE;
  }
  applyStep(i_streamId, step);
  return SESSION_STATUS_OK;
}

SessionStatus StreamManager::reject(const uint32_t i_streamId,
                                    const ReasonCode i_code,
                                    const std::span<const uint8_t> i_message) {
  StreamStateMachine* const stream = find(i_streamId);
  if (stream == nullptr) {
    return SESSION_STATUS_UNKNOWN_STREAM;
  }
  if (i_message.size() + OPEN_ERR_FIXED_SIZE >
      m_config.limits.maxControlPayload) {
    return SESSION_STATUS_INVALID_ARGUMENT;
  }
  const StreamStep step = stream->onReject(i_code, i_message);
  if (!step.valid) {
    return SESSION_STATUS_INVALID_STATE;
  }
  applyStep(i_streamId, step);
  return SESSION_STATUS_OK;
}

SessionStatus StreamManager::write(const uint32_t i_streamId,
                                   const std::span<const uint8_t> i_bytes,
                                   size_t* const o_accepted) {
  *o_accepted = 0U;
  StreamStateMachine* const stream = find(i_streamId);
  if (stream == nullptr) {
    return SESSION_STATUS_UNKNOWN_STREAM;
  }
  const StreamStep step = stream->onWrite(i_bytes, o_accepted);
  if (!step.valid) {
    return SESSION_STATUS_INVALID_STATE;
  }
  applyStep(i_streamId, step);
  return SESSION_STATUS_OK;
}

SessionStatus StreamManager::consume(const uint32_t i_streamId,
                                     const uint64_t i_bytes) {
  StreamStateMachine* const stream = find(i_streamId);
  if (stream == nullptr) {
    return SESSION_STATUS_OK;  // it closed first, which is normal
  }
  return stream->consume(i_bytes) ? SESSION_STATUS_OK
                                  : SESSION_STATUS_INVALID_ARGUMENT;
}

SessionStatus StreamManager::sendEof(const uint32_t i_streamId) {
  StreamStateMachine* const stream = find(i_streamId);
  if (stream == nullptr) {
    return SESSION_STATUS_UNKNOWN_STREAM;
  }
  const StreamStep step = stream->onSendEof();
  if (!step.valid) {
    return SESSION_STATUS_INVALID_STATE;
  }
  applyStep(i_streamId, step);
  return SESSION_STATUS_OK;
}

SessionStatus StreamManager::reset(const uint32_t i_streamId,
                                   const ReasonCode i_code) {
  StreamStateMachine* const stream = find(i_streamId);
  if (stream == nullptr) {
    return SESSION_STATUS_UNKNOWN_STREAM;
  }
  applyStep(i_streamId, stream->onReset(i_code));
  return SESSION_STATUS_OK;
}

void StreamManager::endAll(const ReasonCode i_code) {
  while (!m_streams.empty()) {
    applyStep(m_streams.begin()->first,
              {.next = STREAM_STATE_CLOSED, .closeCode = i_code});
  }
  m_processor.setPeerReceiveWindow(0U);  // the next HELLO sets it again
}

void StreamManager::notifyWritable() { m_processor.notifyWritable(); }

void StreamManager::list(std::vector<StreamInfo>* const o_streams) const {
  o_streams->clear();
  for (const std::pair<const uint32_t, StreamStateMachine>& entry : m_streams) {
    o_streams->push_back(entry.second.info());
  }
}

size_t StreamManager::count() const { return m_streams.size(); }

void StreamManager::setNextStreamId(const uint64_t i_nextStreamId) {
  m_nextStreamId = i_nextStreamId;
}

StreamStateMachine* StreamManager::find(const uint32_t i_streamId) {
  const std::map<uint32_t, StreamStateMachine>::iterator found =
      m_streams.find(i_streamId);
  return found == m_streams.end() ? nullptr : &found->second;
}

void StreamManager::add(const uint32_t i_streamId,
                        const std::string_view i_kind, const Role i_opener,
                        const StreamState i_initialState) {
  const StreamRecord record{.streamId = i_streamId,
                            .kind = std::string(i_kind),
                            .opener = i_opener,
                            .createdAtMs = m_processor.nowMs(),
                            .lastActivityMs = m_processor.nowMs(),
                            .peerCredit = m_config.receiveWindow};
  m_streams.try_emplace(i_streamId, record, i_initialState, &m_processor);
  m_processor.openCredit(i_streamId);
}

void StreamManager::applyStep(const uint32_t i_streamId,
                              const StreamStep& i_step) {
  if (i_step.next == STREAM_STATE_MAX) {
    return;
  }
  if (i_step.next == STREAM_STATE_CLOSED) {
    m_streams.erase(i_streamId);
    m_processor.forgetBlocked(i_streamId);
    m_processor.retireCredit(i_streamId);
    m_processor.emitClosed(i_streamId, i_step.closeCode);
    return;
  }
  m_streams.at(i_streamId).setState(i_step.next);
}

void StreamManager::failStream(const uint32_t i_streamId) {
  m_processor.dropQueue(i_streamId);
  m_processor.queueRst(i_streamId, REASON_CODE_PROTOCOL_ERROR);
  applyStep(i_streamId, {.next = STREAM_STATE_CLOSED,
                         .closeCode = REASON_CODE_PROTOCOL_ERROR});
}

}  // namespace packetmux
