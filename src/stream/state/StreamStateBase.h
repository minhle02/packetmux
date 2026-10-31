#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "packetmux/Frames.h"
#include "packetmux/SessionTypes.h"
#include "stream/StreamProcessor.h"
#include "stream/StreamRecord.h"

namespace packetmux {

/**
 * What a handler returns. StreamManager applies it after the handler returned,
 * because reaching STREAM_STATE_CLOSED erases the stream. A handler never
 * changes state itself.
 */
struct StreamStep {
  bool valid = true;
  StreamState next = STREAM_STATE_MAX;  // STREAM_STATE_MAX: stay
  ReasonCode closeCode = REASON_CODE_OK;
};

struct StreamContext {
  StreamRecord* record = nullptr;
  StreamProcessor* processor = nullptr;
};

/**
 * One stream state. The defaults answer the 50 table cells that are the same
 * everywhere: an input a state does not override is invalid, and RST and reset
 * behave the same in all states. See docs/spec/PROTOCOL.md
 * section 5.3.
 *
 * States hold no data and are const: all stream data is in StreamContext.
 */
class StreamStateBase {
 public:
  virtual ~StreamStateBase() = default;

  virtual StreamState id() const = 0;

  virtual StreamStep onOpenOk(const StreamContext& i_context) const;
  virtual StreamStep onOpenErr(const StreamContext& i_context,
                               const ReasonCode i_code) const;
  virtual StreamStep onData(const StreamContext& i_context,
                            const std::span<const uint8_t> i_data) const;
  virtual StreamStep onEof(const StreamContext& i_context) const;
  virtual StreamStep onRst(const StreamContext& i_context,
                           const ReasonCode i_code) const;
  virtual StreamStep onAccept(const StreamContext& i_context) const;
  virtual StreamStep onReject(const StreamContext& i_context,
                              const ReasonCode i_code,
                              const std::span<const uint8_t> i_message) const;
  virtual StreamStep onWrite(const StreamContext& i_context,
                             const std::span<const uint8_t> i_bytes,
                             size_t* const o_accepted) const;
  virtual StreamStep onSendEof(const StreamContext& i_context) const;
  virtual StreamStep onReset(const StreamContext& i_context,
                             const ReasonCode i_code) const;

 protected:
  StreamStateBase() = default;

  static StreamStep stay();
  static StreamStep moveTo(const StreamState i_next);
  static StreamStep closeWith(const ReasonCode i_code);
  static StreamStep invalid();
};

}  // namespace packetmux
