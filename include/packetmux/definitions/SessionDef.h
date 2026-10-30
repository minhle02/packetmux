#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace packetmux {

// Returned by Session::nextDeadlineMs when no timer is needed.
constexpr uint64_t NO_DEADLINE = std::numeric_limits<uint64_t>::max();

constexpr uint32_t DEFAULT_MAX_STREAMS = 64U;
constexpr size_t DEFAULT_MAX_QUEUED_BYTES = 1024U * 1024U;
constexpr size_t DEFAULT_MAX_QUEUED_BYTES_PER_STREAM = 256U * 1024U;
constexpr size_t DEFAULT_MAX_QUEUED_CONTROL_FRAMES = 1024U;
constexpr size_t DEFAULT_MAX_OUTSTANDING_PINGS = 8U;
constexpr uint64_t DEFAULT_HELLO_TIMEOUT_MS = 10000U;
constexpr uint64_t DEFAULT_PING_INTERVAL_MS = 15000U;
constexpr uint64_t DEFAULT_DEAD_AFTER_MS = 45000U;
constexpr uint64_t DEFAULT_DRAIN_TIMEOUT_MS = 1000U;

}  // namespace packetmux
