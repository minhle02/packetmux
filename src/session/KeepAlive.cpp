#include "session/KeepAlive.h"

#include <algorithm>

namespace packetmux {

KeepAlive::KeepAlive(const uint64_t i_pingIntervalMs,
                     const uint64_t i_deadAfterMs,
                     const size_t i_maxOutstandingPings)
    : m_pingIntervalMs(i_pingIntervalMs),
      m_deadAfterMs(i_deadAfterMs),
      m_maxOutstandingPings(i_maxOutstandingPings),
      m_nextPingAtMs(0U),
      m_deadAtMs(0U),
      m_nextNonce(1U),
      m_rttMs(0U),
      m_hasRtt(false) {}

void KeepAlive::reset(const uint64_t i_nowMs) {
  m_nextPingAtMs = i_nowMs + m_pingIntervalMs;
  m_deadAtMs = i_nowMs + m_deadAfterMs;
  m_outstanding.clear();
}

void KeepAlive::onFrameReceived(const uint64_t i_nowMs) {
  m_deadAtMs = i_nowMs + m_deadAfterMs;
}

void KeepAlive::onPong(const uint64_t i_nonce, const uint64_t i_nowMs) {
  const std::vector<OutstandingPing>::const_iterator found =
      std::find_if(m_outstanding.begin(), m_outstanding.end(),
                   [i_nonce](const OutstandingPing& i_ping) {
                     return i_ping.nonce == i_nonce;
                   });
  if (found == m_outstanding.end()) {
    return;
  }
  m_rttMs = i_nowMs >= found->sentAtMs ? i_nowMs - found->sentAtMs : 0U;
  m_hasRtt = true;
  m_outstanding.erase(found);
}

KeepAliveAction KeepAlive::poll(const uint64_t i_nowMs,
                                uint64_t* const o_nonce) {
  if (i_nowMs >= m_deadAtMs) {
    return KEEP_ALIVE_ACTION_DEAD;
  }
  if (i_nowMs < m_nextPingAtMs) {
    return KEEP_ALIVE_ACTION_NONE;
  }

  *o_nonce = m_nextNonce;
  ++m_nextNonce;
  m_nextPingAtMs = i_nowMs + m_pingIntervalMs;
  m_outstanding.push_back({*o_nonce, i_nowMs});
  if (m_outstanding.size() > m_maxOutstandingPings) {
    m_outstanding.erase(m_outstanding.begin());
  }
  return KEEP_ALIVE_ACTION_SEND_PING;
}

uint64_t KeepAlive::nextDeadlineMs() const {
  return std::min(m_nextPingAtMs, m_deadAtMs);
}

bool KeepAlive::getRttMs(uint64_t* const o_rttMs) const {
  if (!m_hasRtt) {
    return false;
  }
  *o_rttMs = m_rttMs;
  return true;
}

}  // namespace packetmux
