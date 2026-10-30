#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace packetmux {

/**
 * Big-endian writers. They use shifts only, so the output does not depend on
 * the host's byte order.
 */
class ByteWriter {
 public:
  ByteWriter() = delete;

  static void appendU8(const uint8_t i_value,
                       std::vector<uint8_t>* const o_out);
  static void appendU16(const uint16_t i_value,
                        std::vector<uint8_t>* const o_out);
  static void appendU32(const uint32_t i_value,
                        std::vector<uint8_t>* const o_out);
  static void appendU64(const uint64_t i_value,
                        std::vector<uint8_t>* const o_out);
  static void appendBytes(const std::span<const uint8_t> i_bytes,
                          std::vector<uint8_t>* const o_out);
};

}  // namespace packetmux
