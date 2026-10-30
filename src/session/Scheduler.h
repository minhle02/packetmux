#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace packetmux {

struct QueuedFrame {
  std::vector<uint8_t> message;
  size_t dataBytes = 0U;  // DATA payload bytes; counts towards the queue limits
};

/**
 * Decides which encoded frame goes out next. It is an interface so that a
 * priority scheduler can replace round-robin without changing the session.
 */
class Scheduler {
 public:
  virtual ~Scheduler() = default;

  virtual void pushControl(std::vector<uint8_t>&& i_message) = 0;
  virtual void pushStreamFrame(const uint32_t i_streamId,
                               std::vector<uint8_t>&& i_message,
                               const size_t i_dataBytes) = 0;
  virtual void dropStream(const uint32_t i_streamId) = 0;
  virtual void dropAllStreams() = 0;
  virtual void clear() = 0;

  // Takes the next frame. Returns false if nothing is queued.
  virtual bool pop(QueuedFrame* const o_frame) = 0;

  virtual size_t queuedBytes(const uint32_t i_streamId) const = 0;
  virtual size_t totalQueuedBytes() const = 0;
  virtual size_t controlCount() const = 0;
  virtual bool empty() const = 0;
};

}  // namespace packetmux
