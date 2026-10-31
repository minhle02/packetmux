#pragma once

#include <cstddef>
#include <cstdint>

namespace packetmux {

constexpr uint16_t PROTOCOL_VERSION = 2U;
constexpr size_t HEADER_SIZE = 5U;
constexpr uint32_t CONNECTION_STREAM_ID = 0U;
constexpr size_t DEFAULT_MAX_DATA_PAYLOAD = 16000U;
constexpr uint32_t DEFAULT_RECEIVE_WINDOW = 256U * 1024U;
constexpr uint32_t MIN_RECEIVE_WINDOW = 2U;
constexpr size_t DEFAULT_MAX_CONTROL_PAYLOAD = 32U * 1024U;

}  // namespace packetmux
