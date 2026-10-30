#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace packetmux {

/**
 * Bounds-checked big-endian cursor. Each read returns false and leaves the
 * cursor unchanged if not enough bytes remain.
 */
class ByteReader {
 public:
  explicit ByteReader(const std::span<const uint8_t> i_data);

  bool readU8(uint8_t* const o_value);
  bool readU16(uint16_t* const o_value);
  bool readU32(uint32_t* const o_value);
  bool readU64(uint64_t* const o_value);
  bool readBytes(const size_t i_length,
                 std::span<const uint8_t>* const o_bytes);
  size_t remaining() const;

 private:
  uint64_t readBigEndian(const size_t i_byteCount);

  std::span<const uint8_t> m_data;
  size_t m_offset;
};

}  // namespace packetmux
