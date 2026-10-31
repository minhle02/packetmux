#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "packetmux/SessionTypes.h"

namespace packetmux {

class SessionStateMachine;

/**
 * One session with one peer. It lives across reconnects, and has many streams.
 *
 * The session does no I/O, starts no threads and reads no clock. The host feeds
 * it messages and the time, and takes frames and events out with the poll
 * calls. After each call, repeat pollEvent until empty and pollTransmit while
 * the transport has room, until neither returns anything, then set one timer
 * for nextDeadlineMs(). Calls on one session must be serialized by the host.
 *
 * nowMs is a monotonic clock that must not go backwards.
 *
 * Buffers:
 *  - The span from pollTransmit stays valid until the next call on this
 * session.
 *  - Views in an Event borrow the onMessage buffer that produced the event.
 * Keep that buffer alive until pollEvent returns false.
 */
class Session {
 public:
  explicit Session(const SessionConfig& i_config);
  ~Session();

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  Session(Session&&) noexcept;
  Session& operator=(Session&&) noexcept;

  // Call after the host's transport connects, and after every reconnect.
  SessionStatus attachConnection(const uint64_t i_nowMs);

  // i_message is one transport message. Peer errors are reported as events; a
  // status other than OK means the host misused the call and nothing changed.
  SessionStatus onMessage(const std::span<const uint8_t> i_message,
                          const uint64_t i_nowMs);
  // Call when the host's transport closes. Does nothing if the session already
  // ended the connection, so the host need not track that.
  SessionStatus onConnectionLost();
  SessionStatus onTick(const uint64_t i_nowMs);

  // The next time onTick is needed, or NO_DEADLINE. Re-read it after each call.
  uint64_t nextDeadlineMs() const;

  SessionStatus openStream(const std::string_view i_kind,
                           const std::span<const uint8_t> i_params,
                           const uint64_t i_nowMs, uint32_t* const o_streamId);
  SessionStatus acceptStream(const uint32_t i_streamId);
  SessionStatus rejectStream(const uint32_t i_streamId, const ReasonCode i_code,
                             const std::span<const uint8_t> i_message);

  // o_accepted may be less than the size given, even 0: wait for
  // EVENT_TYPE_WRITABLE.
  SessionStatus write(const uint32_t i_streamId,
                      const std::span<const uint8_t> i_bytes,
                      const uint64_t i_nowMs, size_t* const o_accepted);
  // Says the host's consumer has taken this many DATA bytes, which gives the
  // peer its credit back. A stream that is never consumed stalls after one
  // receive window. OK if the stream has already closed.
  SessionStatus consume(const uint32_t i_streamId, const uint64_t i_bytes);
  SessionStatus sendEof(const uint32_t i_streamId);
  SessionStatus resetStream(const uint32_t i_streamId,
                            const ReasonCode i_code = REASON_CODE_KILLED);

  SessionStatus close(const ReasonCode i_code, const uint64_t i_nowMs);

  // Takes the next frame to send. Returns false if nothing is ready.
  bool pollTransmit(std::span<const uint8_t>* const o_message);
  bool pollEvent(Event* const o_event);

  SessionState state() const;
  void listStreams(std::vector<StreamInfo>* const o_streams) const;

  // Returns false until a first RTT is known.
  bool getRttMs(uint64_t* const o_rttMs) const;

 private:
  std::unique_ptr<SessionStateMachine> m_stateMachine;
};

}  // namespace packetmux
