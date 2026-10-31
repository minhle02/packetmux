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
  const bool wasEmpty = queue.frames.empty();
  queue.frames.push_back({std::move(i_message), i_dataBytes});
  queue.queuedBytes += i_dataBytes;
  m_totalQueuedBytes += i_dataBytes;
  if (wasEmpty) {
    schedule(i_streamId, queue);
  }
}

void RoundRobinScheduler::openCredit(const uint32_t i_streamId,
                                     const uint64_t i_credit) {
  StreamQueue& queue = m_streams[i_streamId];
  queue.credit = i_credit;
  if (m_parked.erase(i_streamId) > 0U) {
    schedule(i_streamId, queue);
  }
}

bool RoundRobinScheduler::addCredit(const uint32_t i_streamId,
                                    const uint64_t i_credit,
                                    const uint64_t i_limit) {
  const std::map<uint32_t, StreamQueue>::iterator found =
      m_streams.find(i_streamId);
  if (found == m_streams.end()) {
    return true;
  }
  StreamQueue& queue = found->second;
  if (i_credit > i_limit - std::min(queue.credit, i_limit)) {
    return false;
  }
  queue.credit += i_credit;
  if (m_parked.erase(i_streamId) > 0U) {
    schedule(i_streamId, queue);
  }
  return true;
}

void RoundRobinScheduler::retireCredit(const uint32_t i_streamId) {
  const std::map<uint32_t, StreamQueue>::iterator found =
      m_streams.find(i_streamId);
  if (found == m_streams.end()) {
    return;
  }
  if (found->second.frames.empty()) {
    m_streams.erase(found);
  } else {
    found->second.retired = true;
  }
}

uint64_t RoundRobinScheduler::credit(const uint32_t i_streamId) const {
  const std::map<uint32_t, StreamQueue>::const_iterator found =
      m_streams.find(i_streamId);
  return found == m_streams.end() ? 0U : found->second.credit;
}

void RoundRobinScheduler::dropStream(const uint32_t i_streamId) {
  const std::map<uint32_t, StreamQueue>::iterator found =
      m_streams.find(i_streamId);
  if (found == m_streams.end()) {
    return;
  }
  m_totalQueuedBytes -= found->second.queuedBytes;
  m_streams.erase(found);
  m_parked.erase(i_streamId);
  m_order.erase(std::remove(m_order.begin(), m_order.end(), i_streamId),
                m_order.end());
}

void RoundRobinScheduler::dropAllStreams() {
  m_streams.clear();
  m_order.clear();
  m_parked.clear();
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
  queue.credit -= o_frame->dataBytes;
  m_totalQueuedBytes -= o_frame->dataBytes;

  if (!queue.frames.empty()) {
    schedule(streamId, queue);
  } else if (queue.retired) {
    m_streams.erase(streamId);
  }
  return true;
}

void RoundRobinScheduler::schedule(const uint32_t i_streamId,
                                   const StreamQueue& i_queue) {
  if (i_queue.frames.front().dataBytes <= i_queue.credit) {
    m_order.push_back(i_streamId);
  } else {
    m_parked.insert(i_streamId);
  }
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
  return m_control.empty() && m_order.empty() && m_parked.empty();
}

}  // namespace packetmux
