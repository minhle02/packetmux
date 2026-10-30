#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace packetmux {

enum KeepAliveAction : uint8_t {
  KEEP_ALIVE_ACTION_NONE = 0U,
  KEEP_ALIVE_ACTION_SEND_PING,
  KEEP_ALIVE_ACTION_DEAD,
  KEEP_ALIVE_ACTION_MAX,
};

/**
 * Ping timing, dead-peer detection and RTT. It has no running or stopped state:
 * only EstablishedState calls it, and reset() starts it over.
 */
class KeepAlive {
 public:
  KeepAlive(const uint64_t i_pingIntervalMs, const uint64_t i_deadAfterMs,
            const size_t i_maxOutstandingPings);

  void reset(const uint64_t i_nowMs);
  void onFrameReceived(const uint64_t i_nowMs);
  void onPong(const uint64_t i_nonce, const uint64_t i_nowMs);

  // Call when a deadline passes. o_nonce is set for SEND_PING.
  KeepAliveAction poll(const uint64_t i_nowMs, uint64_t* const o_nonce);

  uint64_t nextDeadlineMs() const;

  // Returns false until a first RTT is known.
  bool getRttMs(uint64_t* const o_rttMs) const;

 private:
  struct OutstandingPing {
    uint64_t nonce = 0U;
    uint64_t sentAtMs = 0U;
  };

  uint64_t m_pingIntervalMs;
  uint64_t m_deadAfterMs;
  size_t m_maxOutstandingPings;
  uint64_t m_nextPingAtMs;
  uint64_t m_deadAtMs;
  uint64_t m_nextNonce;
  uint64_t m_rttMs;
  bool m_hasRtt;
  std::vector<OutstandingPing> m_outstanding;  // oldest first
};

}  // namespace packetmux
