#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "packetmux/SessionTypes.h"
#include "session/SessionProcessor.h"

namespace packetmux {

/**
 * What a handler returns. SessionStateMachine applies it after the handler
 * returned, so a state never changes state itself.
 */
struct SessionStep {
  SessionStatus status = SESSION_STATUS_OK;
  SessionState next = SESSION_STATE_MAX;  // SESSION_STATE_MAX: stay
};

struct OpenRequest {
  std::string_view kind;
  std::span<const uint8_t> params;
};

/**
 * One session state. Every state must answer every event, so the compiler flags
 * any case left out. See docs/spec/PROTOCOL.md section 4.2.
 */
class SessionStateBase {
 public:
  virtual ~SessionStateBase() = default;

  virtual SessionState id() const = 0;

  // Runs after the state is entered. It may not start another transition.
  virtual void onEnter() = 0;

  virtual SessionStep onAttach() = 0;
  virtual SessionStep onMessage(const std::span<const uint8_t> i_message) = 0;
  virtual SessionStep onConnectionLost() = 0;
  virtual SessionStep onTick() = 0;
  virtual SessionStep onClose(const ReasonCode i_code) = 0;
  virtual SessionStep onOpenStream(const OpenRequest& i_request,
                                   uint32_t* const o_streamId) = 0;
  virtual SessionStep onTransmitDrained() = 0;

  virtual uint64_t nextDeadlineMs() const = 0;

 protected:
  explicit SessionStateBase(SessionProcessor* const o_processor)
      : m_processor(o_processor) {}

  static SessionStep stay(const SessionStatus i_status = SESSION_STATUS_OK) {
    return {.status = i_status};
  }

  static SessionStep moveTo(const SessionState i_next) {
    return {.next = i_next};
  }

  SessionProcessor* m_processor;
};

}  // namespace packetmux
