#include "ByteReader.h"

namespace packetmux {

ByteReader::ByteReader(const std::span<const uint8_t> i_data)
    : m_data(i_data), m_offset(0U) {}

bool ByteReader::readU8(uint8_t* const o_value) {
  if (remaining() < sizeof(uint8_t)) {
    return false;
  }
  *o_value = static_cast<uint8_t>(readBigEndian(sizeof(uint8_t)));
  return true;
}

bool ByteReader::readU16(uint16_t* const o_value) {
  if (remaining() < sizeof(uint16_t)) {
    return false;
  }
  *o_value = static_cast<uint16_t>(readBigEndian(sizeof(uint16_t)));
  return true;
}

bool ByteReader::readU32(uint32_t* const o_value) {
  if (remaining() < sizeof(uint32_t)) {
    return false;
  }
  *o_value = static_cast<uint32_t>(readBigEndian(sizeof(uint32_t)));
  return true;
}

bool ByteReader::readU64(uint64_t* const o_value) {
  if (remaining() < sizeof(uint64_t)) {
    return false;
  }
  *o_value = readBigEndian(sizeof(uint64_t));
  return true;
}

bool ByteReader::readBytes(const size_t i_length,
                           std::span<const uint8_t>* const o_bytes) {
  // Compare against what remains: adding a hostile length to the offset could
  // overflow.
  if (i_length > remaining()) {
    return false;
  }
  *o_bytes = m_data.subspan(m_offset, i_length);
  m_offset += i_length;
  return true;
}

size_t ByteReader::remaining() const { return m_data.size() - m_offset; }

uint64_t ByteReader::readBigEndian(const size_t i_byteCount) {
  uint64_t value = 0U;
  for (size_t index = 0U; index < i_byteCount; ++index) {
    value = (value << 8U) | m_data[m_offset + index];
  }
  m_offset += i_byteCount;
  return value;
}

}  // namespace packetmux
