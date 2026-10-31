#include <gtest/gtest.h>

#include <cctype>
#include <cstdint>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "packetmux/Codec.h"

namespace packetmux {
namespace {

using Bytes = std::vector<uint8_t>;

const std::map<std::string, CodecStatus> STATUS_NAMES{
    {"OK", CODEC_STATUS_OK},
    {"TRUNCATED", CODEC_STATUS_TRUNCATED},
    {"UNKNOWN_TYPE", CODEC_STATUS_UNKNOWN_TYPE},
    {"BAD_STREAM_ID", CODEC_STATUS_BAD_STREAM_ID},
    {"PAYLOAD_TOO_LARGE", CODEC_STATUS_PAYLOAD_TOO_LARGE},
    {"BAD_PAYLOAD_SIZE", CODEC_STATUS_BAD_PAYLOAD_SIZE},
    {"UNSUPPORTED_VERSION", CODEC_STATUS_UNSUPPORTED_VERSION},
    {"BAD_IDENTIFIER", CODEC_STATUS_BAD_IDENTIFIER},
    {"BAD_FLAG", CODEC_STATUS_BAD_FLAG},
};

const std::map<std::string, FrameType> TYPE_NAMES{
    {"HELLO", FRAME_TYPE_HELLO},     {"OPEN", FRAME_TYPE_OPEN},
    {"OPEN_OK", FRAME_TYPE_OPEN_OK}, {"OPEN_ERR", FRAME_TYPE_OPEN_ERR},
    {"DATA", FRAME_TYPE_DATA},       {"EOF", FRAME_TYPE_EOF},
    {"RST", FRAME_TYPE_RST},         {"PING", FRAME_TYPE_PING},
    {"PONG", FRAME_TYPE_PONG},       {"GOAWAY", FRAME_TYPE_GOAWAY},
};

const std::map<std::string, ReasonCode> CODE_NAMES{
    {"OK", REASON_CODE_OK},
    {"PEER_CLOSED", REASON_CODE_PEER_CLOSED},
    {"CONNECT_FAILED", REASON_CODE_CONNECT_FAILED},
    {"POLICY_DENIED", REASON_CODE_POLICY_DENIED},
    {"LIMIT_EXCEEDED", REASON_CODE_LIMIT_EXCEEDED},
    {"KILLED", REASON_CODE_KILLED},
    {"AUTH_REVOKED", REASON_CODE_AUTH_REVOKED},
    {"TIMEOUT", REASON_CODE_TIMEOUT},
    {"PROTOCOL_ERROR", REASON_CODE_PROTOCOL_ERROR},
    {"UNSUPPORTED_KIND", REASON_CODE_UNSUPPORTED_KIND},
    {"SESSION_NOT_FOUND", REASON_CODE_SESSION_NOT_FOUND},
};

struct Vector {
  std::string name;
  std::map<std::string, std::string> fields;
};

std::string trim(const std::string& i_text) {
  const size_t first = i_text.find_first_not_of(" \t\r");
  if (first == std::string::npos) {
    return "";
  }
  return i_text.substr(first, i_text.find_last_not_of(" \t\r") - first + 1U);
}

std::vector<Vector> loadVectors(const std::string& i_path) {
  std::ifstream file(i_path);
  EXPECT_TRUE(file.is_open()) << i_path;
  std::vector<Vector> vectors;
  std::string line;
  while (std::getline(file, line)) {
    const std::string text = trim(line);
    if (text.empty() || text.front() == '#') {
      continue;
    }
    if (text.front() == '[') {
      vectors.push_back({text.substr(1U, text.size() - 2U), {}});
      continue;
    }
    const size_t separator = text.find('=');
    EXPECT_NE(separator, std::string::npos) << text;
    EXPECT_FALSE(vectors.empty()) << text;
    vectors.back().fields[trim(text.substr(0U, separator))] =
        trim(text.substr(separator + 1U));
  }
  return vectors;
}

Bytes parseHex(const std::string& i_hex) {
  std::string digits;
  for (const char character : i_hex) {
    if (std::isspace(static_cast<unsigned char>(character)) == 0) {
      digits.push_back(character);
    }
  }
  EXPECT_EQ(digits.size() % 2U, 0U) << i_hex;
  Bytes bytes;
  for (size_t index = 0U; index + 1U < digits.size(); index += 2U) {
    bytes.push_back(static_cast<uint8_t>(
        std::stoul(digits.substr(index, 2U), nullptr, 16)));
  }
  return bytes;
}

std::string fieldOr(const Vector& i_vector, const std::string& i_key,
                    const std::string& i_fallback) {
  const auto found = i_vector.fields.find(i_key);
  return found == i_vector.fields.end() ? i_fallback : found->second;
}

Bytes hexField(const Vector& i_vector, const std::string& i_key) {
  return parseHex(fieldOr(i_vector, i_key, ""));
}

std::string_view asText(const Bytes& i_bytes) {
  return {reinterpret_cast<const char*>(i_bytes.data()), i_bytes.size()};
}

FrameLimits limitsOf(const Vector& i_vector) {
  FrameLimits limits;
  limits.maxDataPayload = std::stoull(fieldOr(
      i_vector, "max_data_payload", std::to_string(DEFAULT_MAX_DATA_PAYLOAD)));
  limits.maxControlPayload =
      std::stoull(fieldOr(i_vector, "max_control_payload",
                          std::to_string(DEFAULT_MAX_CONTROL_PAYLOAD)));
  return limits;
}

void checkValidVector(const Vector& i_vector, const Bytes& i_wire,
                      const FrameLimits& i_limits) {
  const Bytes sessionId = hexField(i_vector, "session_id");
  const Bytes capabilities = hexField(i_vector, "capabilities");
  const Bytes kind = hexField(i_vector, "kind");
  const Bytes params = hexField(i_vector, "params");
  const Bytes message = hexField(i_vector, "message");
  const Bytes data = hexField(i_vector, "data");

  // The expected frame comes from the vector, not from the decoder, so encode
  // is checked on its own.
  const FrameView expected{
      .type = TYPE_NAMES.at(i_vector.fields.at("type")),
      .streamId =
          static_cast<uint32_t>(std::stoul(i_vector.fields.at("stream_id"))),
      .version =
          static_cast<uint16_t>(std::stoul(fieldOr(i_vector, "version", "2"))),
      .resuming = fieldOr(i_vector, "resuming", "0") == "1",
      .sessionId = asText(sessionId),
      .capabilities = capabilities,
      .kind = asText(kind),
      .params = params,
      .code = CODE_NAMES.at(fieldOr(i_vector, "code", "OK")),
      .message = message,
      .data = data,
      .nonce = std::stoull(fieldOr(i_vector, "nonce", "0")),
  };

  FrameView decoded;
  ASSERT_EQ(Codec::decode(i_wire, i_limits, &decoded), CODEC_STATUS_OK);
  EXPECT_EQ(decoded.type, expected.type);
  EXPECT_EQ(decoded.streamId, expected.streamId);
  EXPECT_EQ(decoded.version, expected.version);
  EXPECT_EQ(decoded.resuming, expected.resuming);
  EXPECT_EQ(decoded.sessionId, expected.sessionId);
  EXPECT_EQ(Bytes(decoded.capabilities.begin(), decoded.capabilities.end()),
            capabilities);
  EXPECT_EQ(decoded.kind, expected.kind);
  EXPECT_EQ(Bytes(decoded.params.begin(), decoded.params.end()), params);
  EXPECT_EQ(decoded.code, expected.code);
  EXPECT_EQ(Bytes(decoded.message.begin(), decoded.message.end()), message);
  EXPECT_EQ(Bytes(decoded.data.begin(), decoded.data.end()), data);
  EXPECT_EQ(decoded.nonce, expected.nonce);

  if (fieldOr(i_vector, "roundtrip", "true") == "true") {
    Bytes encoded;
    ASSERT_EQ(Codec::encode(expected, i_limits, &encoded), CODEC_STATUS_OK);
    EXPECT_EQ(encoded, i_wire);
  }
}

TEST(GoldenVectorsTest, FramesV2) {
  const std::vector<Vector> vectors =
      loadVectors(std::string(PACKETMUX_CONFORMANCE_DIR) + "/vectors/frames.txt");
  ASSERT_GE(vectors.size(), 50U);

  std::map<std::string, size_t> names;
  for (const Vector& vector : vectors) {
    SCOPED_TRACE(vector.name);
    EXPECT_EQ(++names[vector.name], 1U) << "duplicate vector name";

    const Bytes wire = hexField(vector, "wire");
    const FrameLimits limits = limitsOf(vector);
    const CodecStatus expected =
        STATUS_NAMES.at(fieldOr(vector, "status", "OK"));
    if (expected == CODEC_STATUS_OK) {
      checkValidVector(vector, wire, limits);
    } else {
      FrameView decoded;
      EXPECT_EQ(Codec::decode(wire, limits, &decoded), expected);
    }
  }
}

}  // namespace
}  // namespace packetmux
