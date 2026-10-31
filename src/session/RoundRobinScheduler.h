#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <vector>

#include "session/Scheduler.h"

namespace packetmux {

/**
 * Control frames first, in order. Then streams with queued frames take turns,
 * one frame per turn.
 */
class RoundRobinScheduler : public Scheduler {
 public:
  RoundRobinScheduler();

  void pushControl(std::vector<uint8_t>&& i_message) override;
  void pushStreamFrame(const uint32_t i_streamId,
                       std::vector<uint8_t>&& i_message,
                       const size_t i_dataBytes) override;
  void openCredit(const uint32_t i_streamId, const uint64_t i_credit) override;
  bool addCredit(const uint32_t i_streamId, const uint64_t i_credit,
                 const uint64_t i_limit) override;
  void retireCredit(const uint32_t i_streamId) override;
  uint64_t credit(const uint32_t i_streamId) const override;
  void dropStream(const uint32_t i_streamId) override;
  void dropAllStreams() override;
  void clear() override;
  bool pop(QueuedFrame* const o_frame) override;
  size_t queuedBytes(const uint32_t i_streamId) const override;
  size_t totalQueuedBytes() const override;
  size_t controlCount() const override;
  bool empty() const override;

 private:
  struct StreamQueue {
    std::deque<QueuedFrame> frames;
    size_t queuedBytes = 0U;
    uint64_t credit = 0U;
    bool retired = false;  // closed: goes once its frames have gone
  };

  void schedule(const uint32_t i_streamId, const StreamQueue& i_queue);

  std::deque<std::vector<uint8_t>> m_control;
  std::map<uint32_t, StreamQueue> m_streams;
  std::deque<uint32_t> m_order;  // stream IDs that can send, in turn order
  std::set<uint32_t> m_parked;   // stream IDs waiting for credit
  size_t m_totalQueuedBytes;
};

}  // namespace packetmux
