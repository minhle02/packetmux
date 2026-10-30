#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "packetmux/Frames.h"
#include "packetmux/SessionTypes.h"
#include "session/EventQueue.h"
#include "session/Scheduler.h"
#include "stream/StreamRecord.h"

namespace packetmux {

/**
 * Stream-level actions, shared by all stream states. It only carries actions
 * out: it never sees a stream's state and never decides what happens next.
 */
class StreamProcessor {
 public:
  StreamProcessor(const SessionConfig& i_config, Scheduler* const o_scheduler,
                  EventQueue* const o_events);

  void setNow(const uint64_t i_nowMs);
  uint64_t nowMs() const;

  // Queues as much of i_bytes as the limits allow. 0 is a valid result.
  void write(StreamRecord* const o_record,
             const std::span<const uint8_t> i_bytes, size_t* const o_accepted);
  void deliverData(StreamRecord* const o_record,
                   const std::span<const uint8_t> i_data);

  void emitOpened(const FrameView& i_frame);
  void emitAccepted(const uint32_t i_streamId);
  void emitEof(const uint32_t i_streamId);
  void emitClosed(const uint32_t i_streamId, const ReasonCode i_code);

  // Both return false, queueing nothing, if the frame cannot be encoded.
  bool queueOpen(const uint32_t i_streamId, const std::string_view i_kind,
                 const std::span<const uint8_t> i_params);
  bool queueOpenErr(const uint32_t i_streamId, const ReasonCode i_code,
                    const std::span<const uint8_t> i_message);

  void queueOpenOk(const uint32_t i_streamId);
  void queueEof(const uint32_t i_streamId);
  void queueRst(const uint32_t i_streamId, const ReasonCode i_code);
  void dropQueue(const uint32_t i_streamId);

  void forgetBlocked(const uint32_t i_streamId);

  // Room returned: tells each blocked stream that has room again.
  void notifyWritable();

  // OK is kept for a clean close, so any other end reports PEER_CLOSED.
  static ReasonCode reasonForStreams(const ReasonCode i_code);

 private:
  size_t roomFor(const uint32_t i_streamId) const;
  bool queueControl(const FrameView& i_frame);

  const SessionConfig& m_config;
  Scheduler* m_scheduler;
  EventQueue* m_events;
  std::vector<uint32_t> m_blockedStreams;
  uint64_t m_nowMs;
};

}  // namespace packetmux
