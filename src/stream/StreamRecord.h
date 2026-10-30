#pragma once

#include <cstdint>
#include <string>

#include "packetmux/SessionTypes.h"

namespace packetmux {

/**
 * The per-stream data. State classes hold no data of their own, so one instance
 * of each serves every stream.
 */
struct StreamRecord {
  uint32_t streamId = 0U;
  std::string kind;
  Role opener = ROLE_CLIENT;
  uint64_t bytesSent = 0U;
  uint64_t bytesReceived = 0U;
  uint64_t createdAtMs = 0U;
  uint64_t lastActivityMs = 0U;
};

}  // namespace packetmux
