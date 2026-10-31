#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "packetmux/Frames.h"
#include "packetmux/SessionTypes.h"
#include "stream/StreamProcessor.h"
#include "stream/StreamRecord.h"
#include "stream/state/StreamStateBase.h"

namespace packetmux {

/**
 * One stream: its current state and its data. Each input goes to the current
 * state, which returns a step. StreamManager applies the step through setState,
 * so nothing here changes state while a handler is running.
 *
 * Only this class creates states.
 */
class StreamStateMachine {
 public:
  StreamStateMachine(const StreamRecord& i_record,
                     const StreamState i_initialState,
                     StreamProcessor* const o_processor);

  StreamState state() const;
  void setState(const StreamState i_state);

  StreamStep onFrame(const FrameView& i_frame);
  bool takePeerCredit(const size_t i_bytes);
  bool consume(const uint64_t i_bytes);
  StreamStep onAccept();
  StreamStep onReject(const ReasonCode i_code,
                      const std::span<const uint8_t> i_message);
  StreamStep onWrite(const std::span<const uint8_t> i_bytes,
                     size_t* const o_accepted);
  StreamStep onSendEof();
  StreamStep onReset(const ReasonCode i_code);

  StreamInfo info() const;

 private:
  static const StreamStateBase* stateFor(const StreamState i_state);

  StreamContext context();

  const StreamStateBase* m_state;
  StreamRecord m_record;
  StreamProcessor* m_processor;
};

}  // namespace packetmux
