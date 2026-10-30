#include "ByteWriter.h"

namespace packetmux {

void ByteWriter::appendU8(const uint8_t i_value,
                          std::vector<uint8_t>* const o_out) {
  o_out->push_back(i_value);
}

void ByteWriter::appendU16(const uint16_t i_value,
                           std::vector<uint8_t>* const o_out) {
  o_out->push_back(static_cast<uint8_t>(i_value >> 8U));
  o_out->push_back(static_cast<uint8_t>(i_value));
}

void ByteWriter::appendU32(const uint32_t i_value,
                           std::vector<uint8_t>* const o_out) {
  o_out->push_back(static_cast<uint8_t>(i_value >> 24U));
  o_out->push_back(static_cast<uint8_t>(i_value >> 16U));
  o_out->push_back(static_cast<uint8_t>(i_value >> 8U));
  o_out->push_back(static_cast<uint8_t>(i_value));
}

void ByteWriter::appendU64(const uint64_t i_value,
                           std::vector<uint8_t>* const o_out) {
  appendU32(static_cast<uint32_t>(i_value >> 32U), o_out);
  appendU32(static_cast<uint32_t>(i_value), o_out);
}

void ByteWriter::appendBytes(const std::span<const uint8_t> i_bytes,
                             std::vector<uint8_t>* const o_out) {
  o_out->insert(o_out->end(), i_bytes.begin(), i_bytes.end());
}

}  // namespace packetmux
