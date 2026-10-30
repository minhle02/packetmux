#include "session/RoundRobinScheduler.h"

#include <algorithm>
#include <utility>

namespace packetmux {

RoundRobinScheduler::RoundRobinScheduler() : m_totalQueuedBytes(0U) {}

void RoundRobinScheduler::pushControl(std::vector<uint8_t>&& i_message) {
  m_control.push_back(std::move(i_message));
}

void RoundRobinScheduler::pushStreamFrame(const uint32_t i_streamId,
                                          std::vector<uint8_t>&& i_message,
                                          const size_t i_dataBytes) {
  StreamQueue& queue = m_streams[i_streamId];
  if (queue.frames.empty()) {
    m_order.push_back(i_streamId);
  }
  queue.frames.push_back({std::move(i_message), i_dataBytes});
  queue.queuedBytes += i_dataBytes;
  m_totalQueuedBytes += i_dataBytes;
}

void RoundRobinScheduler::dropStream(const uint32_t i_streamId) {
  const std::map<uint32_t, StreamQueue>::iterator found =
      m_streams.find(i_streamId);
  if (found == m_streams.end()) {
    return;
  }
  m_totalQueuedBytes -= found->second.queuedBytes;
  m_streams.erase(found);
  m_order.erase(std::remove(m_order.begin(), m_order.end(), i_streamId),
                m_order.end());
}

void RoundRobinScheduler::dropAllStreams() {
  m_streams.clear();
  m_order.clear();
  m_totalQueuedBytes = 0U;
}

void RoundRobinScheduler::clear() {
  m_control.clear();
  dropAllStreams();
}

bool RoundRobinScheduler::pop(QueuedFrame* const o_frame) {
  if (!m_control.empty()) {
    o_frame->message = std::move(m_control.front());
    o_frame->dataBytes = 0U;
    m_control.pop_front();
    return true;
  }
  if (m_order.empty()) {
    return false;
  }

  const uint32_t streamId = m_order.front();
  m_order.pop_front();
  StreamQueue& queue = m_streams[streamId];
  *o_frame = std::move(queue.frames.front());
  queue.frames.pop_front();
  queue.queuedBytes -= o_frame->dataBytes;
  m_totalQueuedBytes -= o_frame->dataBytes;

  if (queue.frames.empty()) {
    m_streams.erase(streamId);
  } else {
    m_order.push_back(streamId);
  }
  return true;
}

size_t RoundRobinScheduler::queuedBytes(const uint32_t i_streamId) const {
  const std::map<uint32_t, StreamQueue>::const_iterator found =
      m_streams.find(i_streamId);
  return found == m_streams.end() ? 0U : found->second.queuedBytes;
}

size_t RoundRobinScheduler::totalQueuedBytes() const {
  return m_totalQueuedBytes;
}

size_t RoundRobinScheduler::controlCount() const { return m_control.size(); }

bool RoundRobinScheduler::empty() const {
  return m_control.empty() && m_order.empty();
}

}  // namespace packetmux
