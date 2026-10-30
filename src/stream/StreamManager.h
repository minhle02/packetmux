#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string_view>
#include <vector>

#include "packetmux/Frames.h"
#include "packetmux/SessionTypes.h"
#include "session/EventQueue.h"
#include "session/Scheduler.h"
#include "stream/StreamProcessor.h"
#include "stream/StreamStateMachine.h"

namespace packetmux {

/**
 * Owns every stream of a session: allocates IDs, routes inputs to the right
 * stream, and applies the step each one returns. A stream is erased here, never
 * while one of its own handlers is running.
 */
class StreamManager {
 public:
  StreamManager(const SessionConfig& i_config, Scheduler* const o_scheduler,
                EventQueue* const o_events);

  void setNow(const uint64_t i_nowMs);

  SessionStatus open(const std::string_view i_kind,
                     const std::span<const uint8_t> i_params,
                     uint32_t* const o_streamId);

  // Returns false if the OPEN is a connection-level protocol error.
  bool onPeerOpen(const FrameView& i_frame);
  void onFrame(const FrameView& i_frame);

  SessionStatus accept(const uint32_t i_streamId);
  SessionStatus reject(const uint32_t i_streamId, const ReasonCode i_code,
                       const std::span<const uint8_t> i_message);
  SessionStatus write(const uint32_t i_streamId,
                      const std::span<const uint8_t> i_bytes,
                      size_t* const o_accepted);
  SessionStatus sendEof(const uint32_t i_streamId);
  SessionStatus reset(const uint32_t i_streamId, const ReasonCode i_code);

  void endAll(const ReasonCode i_code);
  void notifyWritable();

  void list(std::vector<StreamInfo>* const o_streams) const;
  size_t count() const;

  // Lets a test reach the end of the ID space without opening 2^31 streams.
  void setNextStreamId(const uint64_t i_nextStreamId);

 private:
  StreamStateMachine* find(const uint32_t i_streamId);
  void add(const uint32_t i_streamId, const std::string_view i_kind,
           const Role i_opener, const StreamState i_initialState);
  void applyStep(const uint32_t i_streamId, const StreamStep& i_step);
  void failStream(const uint32_t i_streamId);

  const SessionConfig& m_config;
  StreamProcessor m_processor;
  std::map<uint32_t, StreamStateMachine> m_streams;  // ordered: deterministic
  uint64_t m_nextStreamId;
  uint32_t m_highestPeerStreamId;
};

}  // namespace packetmux
