#include "packetmux/Codec.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <string_view>

#include "ByteReader.h"
#include "ByteWriter.h"

namespace packetmux {
namespace {

constexpr size_t REASON_PAYLOAD_SIZE = 1U;
constexpr size_t NONCE_PAYLOAD_SIZE = 8U;
constexpr size_t VERSION_FIELD_SIZE = 2U;
constexpr size_t ID_LENGTH_FIELD_SIZE = 2U;
constexpr size_t OPAQUE_LENGTH_FIELD_SIZE = 4U;
constexpr size_t REASON_FIELD_SIZE = 1U;

bool isConnectionFrame(const FrameType i_type) {
  return i_type == FRAME_TYPE_HELLO || i_type == FRAME_TYPE_PING ||
         i_type == FRAME_TYPE_PONG || i_type == FRAME_TYPE_GOAWAY;
}

bool isControlPayloadFrame(const FrameType i_type) {
  return i_type == FRAME_TYPE_HELLO || i_type == FRAME_TYPE_OPEN ||
         i_type == FRAME_TYPE_OPEN_ERR;
}

bool isKnownFrameType(const uint8_t i_rawType) {
  return i_rawType != FRAME_TYPE_UNKNOWN && i_rawType < FRAME_TYPE_MAX;
}

bool isValidStreamId(const FrameType i_type, const uint32_t i_streamId) {
  return isConnectionFrame(i_type) == (i_streamId == CONNECTION_STREAM_ID);
}

bool isIdentifierChar(const char i_char) {
  return (i_char >= 'a' && i_char <= 'z') || (i_char >= 'A' && i_char <= 'Z') ||
         (i_char >= '0' && i_char <= '9') || i_char == '-';
}

bool isValidIdentifier(const std::string_view i_identifier) {
  if (i_identifier.empty() ||
      i_identifier.size() > std::numeric_limits<uint16_t>::max()) {
    return false;
  }
  for (const char character : i_identifier) {
    if (!isIdentifierChar(character)) {
      return false;
    }
  }
  return true;
}

// An unknown code from a newer peer, or a local-only code, is an error but not
// a malformed frame.
ReasonCode reasonFromWire(const uint8_t i_rawCode) {
  if (i_rawCode >= REASON_CODE_MAX || i_rawCode == REASON_CODE_TRANSPORT_LOST) {
    return REASON_CODE_PROTOCOL_ERROR;
  }
  return static_cast<ReasonCode>(i_rawCode);
}

bool isSendableReason(const ReasonCode i_code) {
  return i_code < REASON_CODE_MAX && i_code != REASON_CODE_TRANSPORT_LOST;
}

CodecStatus readIdentifier(ByteReader* const o_reader,
                           std::string_view* const o_identifier) {
  uint16_t length = 0U;
  std::span<const uint8_t> bytes;
  if (!o_reader->readU16(&length) || !o_reader->readBytes(length, &bytes)) {
    return CODEC_STATUS_TRUNCATED;
  }
  const std::string_view identifier(reinterpret_cast<const char*>(bytes.data()),
                                    bytes.size());
  if (!isValidIdentifier(identifier)) {
    return CODEC_STATUS_BAD_IDENTIFIER;
  }
  *o_identifier = identifier;
  return CODEC_STATUS_OK;
}

CodecStatus readOpaque(ByteReader* const o_reader,
                       std::span<const uint8_t>* const o_bytes) {
  uint32_t length = 0U;
  if (!o_reader->readU32(&length) || !o_reader->readBytes(length, o_bytes)) {
    return CODEC_STATUS_TRUNCATED;
  }
  return CODEC_STATUS_OK;
}

CodecStatus readHello(ByteReader* const o_reader, FrameView* const o_frame) {
  if (!o_reader->readU16(&o_frame->version)) {
    return CODEC_STATUS_TRUNCATED;
  }
  if (o_frame->version != PROTOCOL_VERSION) {
    return CODEC_STATUS_UNSUPPORTED_VERSION;
  }
  const CodecStatus status = readIdentifier(o_reader, &o_frame->sessionId);
  if (status != CODEC_STATUS_OK) {
    return status;
  }
  return readOpaque(o_reader, &o_frame->capabilities);
}

CodecStatus readOpen(ByteReader* const o_reader, FrameView* const o_frame) {
  const CodecStatus status = readIdentifier(o_reader, &o_frame->kind);
  if (status != CODEC_STATUS_OK) {
    return status;
  }
  return readOpaque(o_reader, &o_frame->params);
}

CodecStatus readReason(ByteReader* const o_reader, FrameView* const o_frame) {
  uint8_t rawCode = 0U;
  if (!o_reader->readU8(&rawCode)) {
    return CODEC_STATUS_TRUNCATED;
  }
  o_frame->code = reasonFromWire(rawCode);
  return CODEC_STATUS_OK;
}

CodecStatus readOpenErr(ByteReader* const o_reader, FrameView* const o_frame) {
  const CodecStatus status = readReason(o_reader, o_frame);
  if (status != CODEC_STATUS_OK) {
    return status;
  }
  return readOpaque(o_reader, &o_frame->message);
}

CodecStatus readNonce(ByteReader* const o_reader, FrameView* const o_frame) {
  return o_reader->readU64(&o_frame->nonce) ? CODEC_STATUS_OK
                                            : CODEC_STATUS_TRUNCATED;
}

CodecStatus readPayload(ByteReader* const o_reader, FrameView* const o_frame) {
  switch (o_frame->type) {
    case FRAME_TYPE_HELLO:
      return readHello(o_reader, o_frame);
    case FRAME_TYPE_OPEN:
      return readOpen(o_reader, o_frame);
    case FRAME_TYPE_OPEN_ERR:
      return readOpenErr(o_reader, o_frame);
    case FRAME_TYPE_DATA:
      return o_reader->readBytes(o_reader->remaining(), &o_frame->data)
                 ? CODEC_STATUS_OK
                 : CODEC_STATUS_TRUNCATED;
    case FRAME_TYPE_RST:
    case FRAME_TYPE_GOAWAY:
      return readReason(o_reader, o_frame);
    case FRAME_TYPE_PING:
    case FRAME_TYPE_PONG:
      return readNonce(o_reader, o_frame);
    default:
      return CODEC_STATUS_OK;  // OPEN_OK, EOF: empty payload
  }
}

CodecStatus checkPayloadSize(const FrameType i_type, const size_t i_size,
                             const FrameLimits& i_limits) {
  if (i_type == FRAME_TYPE_DATA && i_size > i_limits.maxDataPayload) {
    return CODEC_STATUS_PAYLOAD_TOO_LARGE;
  }
  if (isControlPayloadFrame(i_type) && i_size > i_limits.maxControlPayload) {
    return CODEC_STATUS_PAYLOAD_TOO_LARGE;
  }
  bool validSize = true;
  switch (i_type) {
    case FRAME_TYPE_OPEN_OK:
    case FRAME_TYPE_EOF:
      validSize = (i_size == 0U);
      break;
    case FRAME_TYPE_RST:
    case FRAME_TYPE_GOAWAY:
      validSize = (i_size == REASON_PAYLOAD_SIZE);
      break;
    case FRAME_TYPE_PING:
    case FRAME_TYPE_PONG:
      validSize = (i_size == NONCE_PAYLOAD_SIZE);
      break;
    case FRAME_TYPE_DATA:
      validSize = (i_size != 0U);
      break;
    default:
      break;  // HELLO, OPEN, OPEN_ERR: checked field by field
  }
  return validSize ? CODEC_STATUS_OK : CODEC_STATUS_BAD_PAYLOAD_SIZE;
}

// Each size is checked against what is left of the limit, so the sum cannot
// overflow size_t.
bool fitsControlPayload(const size_t i_fixedSize,
                        const std::initializer_list<size_t> i_fieldSizes,
                        const FrameLimits& i_limits) {
  if (i_fixedSize > i_limits.maxControlPayload) {
    return false;
  }
  size_t left = i_limits.maxControlPayload - i_fixedSize;
  for (const size_t size : i_fieldSizes) {
    if (size > left) {
      return false;
    }
    left -= size;
  }
  return true;
}

bool fitsOpaqueLength(const std::span<const uint8_t> i_bytes) {
  return static_cast<uint64_t>(i_bytes.size()) <=
         std::numeric_limits<uint32_t>::max();
}

CodecStatus validateForEncode(const FrameView& i_frame,
                              const FrameLimits& i_limits) {
  if (!isKnownFrameType(i_frame.type)) {
    return CODEC_STATUS_UNKNOWN_TYPE;
  }
  if (!isValidStreamId(i_frame.type, i_frame.streamId)) {
    return CODEC_STATUS_BAD_STREAM_ID;
  }
  switch (i_frame.type) {
    case FRAME_TYPE_HELLO:
      if (i_frame.version != PROTOCOL_VERSION) {
        return CODEC_STATUS_INVALID_ARGUMENT;
      }
      if (!isValidIdentifier(i_frame.sessionId)) {
        return CODEC_STATUS_BAD_IDENTIFIER;
      }
      if (!fitsOpaqueLength(i_frame.capabilities) ||
          !fitsControlPayload(
              VERSION_FIELD_SIZE + ID_LENGTH_FIELD_SIZE +
                  OPAQUE_LENGTH_FIELD_SIZE,
              {i_frame.sessionId.size(), i_frame.capabilities.size()},
              i_limits)) {
        return CODEC_STATUS_PAYLOAD_TOO_LARGE;
      }
      return CODEC_STATUS_OK;
    case FRAME_TYPE_OPEN:
      if (!isValidIdentifier(i_frame.kind)) {
        return CODEC_STATUS_BAD_IDENTIFIER;
      }
      if (!fitsOpaqueLength(i_frame.params) ||
          !fitsControlPayload(ID_LENGTH_FIELD_SIZE + OPAQUE_LENGTH_FIELD_SIZE,
                              {i_frame.kind.size(), i_frame.params.size()},
                              i_limits)) {
        return CODEC_STATUS_PAYLOAD_TOO_LARGE;
      }
      return CODEC_STATUS_OK;
    case FRAME_TYPE_OPEN_ERR:
      if (!isSendableReason(i_frame.code)) {
        return CODEC_STATUS_INVALID_ARGUMENT;
      }
      if (!fitsOpaqueLength(i_frame.message) ||
          !fitsControlPayload(REASON_FIELD_SIZE + OPAQUE_LENGTH_FIELD_SIZE,
                              {i_frame.message.size()}, i_limits)) {
        return CODEC_STATUS_PAYLOAD_TOO_LARGE;
      }
      return CODEC_STATUS_OK;
    case FRAME_TYPE_DATA:
      return checkPayloadSize(i_frame.type, i_frame.data.size(), i_limits);
    case FRAME_TYPE_RST:
    case FRAME_TYPE_GOAWAY:
      return isSendableReason(i_frame.code) ? CODEC_STATUS_OK
                                            : CODEC_STATUS_INVALID_ARGUMENT;
    default:
      return CODEC_STATUS_OK;  // OPEN_OK, EOF, PING, PONG: nothing that can be
                               // invalid
  }
}

void writeIdentifier(const std::string_view i_identifier,
                     std::vector<uint8_t>* const o_out) {
  ByteWriter::appendU16(static_cast<uint16_t>(i_identifier.size()), o_out);
  ByteWriter::appendBytes(
      std::span<const uint8_t>(
          reinterpret_cast<const uint8_t*>(i_identifier.data()),
          i_identifier.size()),
      o_out);
}

void writeOpaque(const std::span<const uint8_t> i_bytes,
                 std::vector<uint8_t>* const o_out) {
  ByteWriter::appendU32(static_cast<uint32_t>(i_bytes.size()), o_out);
  ByteWriter::appendBytes(i_bytes, o_out);
}

void writePayload(const FrameView& i_frame, std::vector<uint8_t>* const o_out) {
  switch (i_frame.type) {
    case FRAME_TYPE_HELLO:
      ByteWriter::appendU16(i_frame.version, o_out);
      writeIdentifier(i_frame.sessionId, o_out);
      writeOpaque(i_frame.capabilities, o_out);
      break;
    case FRAME_TYPE_OPEN:
      writeIdentifier(i_frame.kind, o_out);
      writeOpaque(i_frame.params, o_out);
      break;
    case FRAME_TYPE_OPEN_ERR:
      ByteWriter::appendU8(i_frame.code, o_out);
      writeOpaque(i_frame.message, o_out);
      break;
    case FRAME_TYPE_DATA:
      ByteWriter::appendBytes(i_frame.data, o_out);
      break;
    case FRAME_TYPE_RST:
    case FRAME_TYPE_GOAWAY:
      ByteWriter::appendU8(i_frame.code, o_out);
      break;
    case FRAME_TYPE_PING:
    case FRAME_TYPE_PONG:
      ByteWriter::appendU64(i_frame.nonce, o_out);
      break;
    default:
      break;  // OPEN_OK, EOF: empty payload
  }
}

}  // namespace

CodecStatus Codec::decode(const std::span<const uint8_t> i_message,
                          const FrameLimits& i_limits,
                          FrameView* const o_frame) {
  *o_frame = FrameView{};

  ByteReader reader(i_message);
  uint8_t rawType = 0U;
  if (!reader.readU8(&rawType) || !reader.readU32(&o_frame->streamId)) {
    return CODEC_STATUS_TRUNCATED;
  }
  if (!isKnownFrameType(rawType)) {
    return CODEC_STATUS_UNKNOWN_TYPE;
  }
  o_frame->type = static_cast<FrameType>(rawType);
  if (!isValidStreamId(o_frame->type, o_frame->streamId)) {
    return CODEC_STATUS_BAD_STREAM_ID;
  }

  const CodecStatus sizeStatus =
      checkPayloadSize(o_frame->type, reader.remaining(), i_limits);
  if (sizeStatus != CODEC_STATUS_OK) {
    return sizeStatus;
  }

  const CodecStatus payloadStatus = readPayload(&reader, o_frame);
  if (payloadStatus != CODEC_STATUS_OK) {
    return payloadStatus;
  }
  return reader.remaining() == 0U ? CODEC_STATUS_OK
                                  : CODEC_STATUS_BAD_PAYLOAD_SIZE;
}

CodecStatus Codec::encode(const FrameView& i_frame, const FrameLimits& i_limits,
                          std::vector<uint8_t>* const o_message) {
  const CodecStatus status = validateForEncode(i_frame, i_limits);
  if (status != CODEC_STATUS_OK) {
    return status;
  }
  ByteWriter::appendU8(i_frame.type, o_message);
  ByteWriter::appendU32(i_frame.streamId, o_message);
  writePayload(i_frame, o_message);
  return CODEC_STATUS_OK;
}

}  // namespace packetmux
