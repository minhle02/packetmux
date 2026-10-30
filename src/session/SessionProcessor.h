#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "packetmux/Frames.h"
#include "packetmux/SessionTypes.h"
#include "session/EventQueue.h"
#include "session/KeepAlive.h"
#include "session/Scheduler.h"
#include "stream/StreamManager.h"

namespace packetmux {

/**
 * Connection-level actions, shared by all session states. It only carries
 * actions out: it never sees the current state and never decides what happens
 * next. Those decisions belong to the state classes.
 */
class SessionProcessor {
 public:
  explicit SessionProcessor(const SessionConfig& i_config);

  void setNow(const uint64_t i_nowMs);
  uint64_t nowMs() const;
  const SessionConfig& config() const;

  // Whether attachConnection can go ahead with this configuration.
  bool validateConfig() const;

  CodecStatus decode(const std::span<const uint8_t> i_message,
                     FrameView* const o_frame) const;

  void queueHello();
  void queueGoaway(const ReasonCode i_code);
  void queuePing(const uint64_t i_nonce);
  void queuePong(const uint64_t i_nonce);

  void setDisconnectReason(const ReasonCode i_code,
                           const CodecStatus i_codecStatus = CODEC_STATUS_OK);
  void setCloseCode(const ReasonCode i_code);
  ReasonCode closeCode() const;

  void emitConnected(const FrameView& i_frame);
  void emitDisconnected();
  void emitSessionClosed();

  void clearQueues();
  void dropStreamQueues();
  bool transmitQueueEmpty() const;
  bool controlQueueOverCap() const;

  bool popFrame(std::span<const uint8_t>* const o_message);
  bool pollEvent(Event* const o_event);
  bool eventsPending() const;

  KeepAlive& keepAlive();
  const KeepAlive& keepAlive() const;
  StreamManager& streams();
  const StreamManager& streams() const;

  static bool isSendable(const ReasonCode i_code);

 private:
  void queueControl(const FrameView& i_frame);

  const SessionConfig m_config;
  EventQueue m_events;
  std::unique_ptr<Scheduler> m_scheduler;
  KeepAlive m_keepAlive;
  StreamManager m_streams;
  std::vector<uint8_t> m_txMessage;  // what the last pollTransmit returned
  ReasonCode m_disconnectCode;
  CodecStatus m_disconnectCodecStatus;
  ReasonCode m_closeCode;
  uint64_t m_nowMs;
};

}  // namespace packetmux
