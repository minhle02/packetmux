#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "packetmux/Codec.h"

namespace packetmux {
namespace {

using Bytes = std::vector<uint8_t>;

constexpr uint32_t STREAM_ID = 7U;

const Bytes SAMPLE_BYTES{0x01, 0x02, 0x03};

Bytes roundTrip(const FrameView& i_frame, FrameView* const o_decoded) {
  Bytes wire;
  EXPECT_EQ(Codec::encode(i_frame, FrameLimits{}, &wire), CODEC_STATUS_OK);
  EXPECT_EQ(Codec::decode(wire, FrameLimits{}, o_decoded), CODEC_STATUS_OK);
  return wire;
}

void expectSameBytes(const std::span<const uint8_t> i_actual,
                     const std::span<const uint8_t> i_expected) {
  EXPECT_EQ(Bytes(i_actual.begin(), i_actual.end()),
            Bytes(i_expected.begin(), i_expected.end()));
}

TEST(CodecRoundTripTest, Hello) {
  FrameView decoded;
  const Bytes wire = roundTrip({.type = FRAME_TYPE_HELLO,
                                .sessionId = "session-1",
                                .capabilities = SAMPLE_BYTES},
                               &decoded);
  EXPECT_EQ(decoded.type, FRAME_TYPE_HELLO);
  EXPECT_EQ(decoded.streamId, CONNECTION_STREAM_ID);
  EXPECT_EQ(decoded.version, PROTOCOL_VERSION);
  EXPECT_EQ(decoded.sessionId, "session-1");
  expectSameBytes(decoded.capabilities, SAMPLE_BYTES);
  EXPECT_FALSE(decoded.resuming);
}

TEST(CodecRoundTripTest, HelloResuming) {
  FrameView decoded;
  roundTrip({.type = FRAME_TYPE_HELLO, .resuming = true, .sessionId = "s"},
            &decoded);
  EXPECT_TRUE(decoded.resuming);
}

TEST(CodecRoundTripTest, Open) {
  FrameView decoded;
  const Bytes wire = roundTrip({.type = FRAME_TYPE_OPEN,
                                .streamId = STREAM_ID,
                                .kind = "tcp-forward",
                                .params = SAMPLE_BYTES},
                               &decoded);
  EXPECT_EQ(decoded.type, FRAME_TYPE_OPEN);
  EXPECT_EQ(decoded.streamId, STREAM_ID);
  EXPECT_EQ(decoded.kind, "tcp-forward");
  expectSameBytes(decoded.params, SAMPLE_BYTES);
}

TEST(CodecRoundTripTest, OpenErr) {
  FrameView decoded;
  const Bytes wire = roundTrip({.type = FRAME_TYPE_OPEN_ERR,
                                .streamId = STREAM_ID,
                                .code = REASON_CODE_POLICY_DENIED,
                                .message = SAMPLE_BYTES},
                               &decoded);
  EXPECT_EQ(decoded.code, REASON_CODE_POLICY_DENIED);
  expectSameBytes(decoded.message, SAMPLE_BYTES);
}

TEST(CodecRoundTripTest, Data) {
  FrameView decoded;
  const Bytes wire = roundTrip(
      {.type = FRAME_TYPE_DATA, .streamId = STREAM_ID, .data = SAMPLE_BYTES},
      &decoded);
  EXPECT_EQ(decoded.streamId, STREAM_ID);
  expectSameBytes(decoded.data, SAMPLE_BYTES);
}

TEST(CodecRoundTripTest, EmptyPayloadFrames) {
  for (const FrameType type : {FRAME_TYPE_OPEN_OK, FRAME_TYPE_EOF}) {
    FrameView decoded;
    const Bytes wire =
        roundTrip({.type = type, .streamId = STREAM_ID}, &decoded);
    EXPECT_EQ(decoded.type, type);
    EXPECT_EQ(wire.size(), HEADER_SIZE);
  }
}

TEST(CodecRoundTripTest, ReasonFrames) {
  FrameView decoded;
  roundTrip({.type = FRAME_TYPE_RST,
             .streamId = STREAM_ID,
             .code = REASON_CODE_KILLED},
            &decoded);
  EXPECT_EQ(decoded.code, REASON_CODE_KILLED);
  roundTrip({.type = FRAME_TYPE_GOAWAY, .code = REASON_CODE_AUTH_REVOKED},
            &decoded);
  EXPECT_EQ(decoded.code, REASON_CODE_AUTH_REVOKED);
}

TEST(CodecRoundTripTest, NonceFrames) {
  constexpr uint64_t NONCE = 0x0102030405060708ULL;
  for (const FrameType type : {FRAME_TYPE_PING, FRAME_TYPE_PONG}) {
    FrameView decoded;
    roundTrip({.type = type, .nonce = NONCE}, &decoded);
    EXPECT_EQ(decoded.type, type);
    EXPECT_EQ(decoded.nonce, NONCE);
  }
}

TEST(CodecDecodeTest, ViewsBorrowFromTheMessage) {
  Bytes wire;
  ASSERT_EQ(Codec::encode({.type = FRAME_TYPE_DATA,
                           .streamId = STREAM_ID,
                           .data = SAMPLE_BYTES},
                          FrameLimits{}, &wire),
            CODEC_STATUS_OK);
  FrameView decoded;
  ASSERT_EQ(Codec::decode(wire, FrameLimits{}, &decoded), CODEC_STATUS_OK);
  EXPECT_EQ(decoded.data.data(), wire.data() + HEADER_SIZE);
}

TEST(CodecDecodeTest, ResetsTheOutputOnFailure) {
  const Bytes truncated{0x01};
  FrameView frame{.type = FRAME_TYPE_DATA, .streamId = STREAM_ID, .nonce = 9U};
  EXPECT_EQ(Codec::decode(truncated, FrameLimits{}, &frame),
            CODEC_STATUS_TRUNCATED);
  EXPECT_EQ(frame.type, FRAME_TYPE_UNKNOWN);
  EXPECT_EQ(frame.streamId, CONNECTION_STREAM_ID);
  EXPECT_EQ(frame.nonce, 0U);
}

TEST(CodecDecodeTest, ReportsThePeerVersion) {
  const Bytes wire{0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03};
  FrameView frame;
  EXPECT_EQ(Codec::decode(wire, FrameLimits{}, &frame),
            CODEC_STATUS_UNSUPPORTED_VERSION);
  EXPECT_EQ(frame.version, 3U);
}

TEST(CodecLimitsTest, DataAtTheLimit) {
  const FrameLimits limits{.maxDataPayload = 3U};
  const FrameView frame{
      .type = FRAME_TYPE_DATA, .streamId = STREAM_ID, .data = SAMPLE_BYTES};
  Bytes wire;
  ASSERT_EQ(Codec::encode(frame, limits, &wire), CODEC_STATUS_OK);
  FrameView decoded;
  EXPECT_EQ(Codec::decode(wire, limits, &decoded), CODEC_STATUS_OK);
}

TEST(CodecLimitsTest, DataOneByteOverTheLimit) {
  const FrameLimits limits{.maxDataPayload = 2U};
  const FrameView frame{
      .type = FRAME_TYPE_DATA, .streamId = STREAM_ID, .data = SAMPLE_BYTES};
  Bytes wire;
  ASSERT_EQ(Codec::encode(frame, FrameLimits{}, &wire), CODEC_STATUS_OK);
  FrameView decoded;
  EXPECT_EQ(Codec::decode(wire, limits, &decoded),
            CODEC_STATUS_PAYLOAD_TOO_LARGE);

  Bytes refused;
  EXPECT_EQ(Codec::encode(frame, limits, &refused),
            CODEC_STATUS_PAYLOAD_TOO_LARGE);
  EXPECT_TRUE(refused.empty());
}

TEST(CodecLimitsTest, ControlPayloadAtAndOverTheLimit) {
  // kind_len (2) + "ab" (2) + params_len (4) + params (3) = 11
  const FrameView frame{.type = FRAME_TYPE_OPEN,
                        .streamId = STREAM_ID,
                        .kind = "ab",
                        .params = SAMPLE_BYTES};
  Bytes wire;
  EXPECT_EQ(Codec::encode(frame, FrameLimits{.maxControlPayload = 11U}, &wire),
            CODEC_STATUS_OK);
  FrameView decoded;
  EXPECT_EQ(
      Codec::decode(wire, FrameLimits{.maxControlPayload = 11U}, &decoded),
      CODEC_STATUS_OK);
  EXPECT_EQ(
      Codec::decode(wire, FrameLimits{.maxControlPayload = 10U}, &decoded),
      CODEC_STATUS_PAYLOAD_TOO_LARGE);

  Bytes refused;
  EXPECT_EQ(
      Codec::encode(frame, FrameLimits{.maxControlPayload = 10U}, &refused),
      CODEC_STATUS_PAYLOAD_TOO_LARGE);
  EXPECT_EQ(
      Codec::encode(frame, FrameLimits{.maxControlPayload = 3U}, &refused),
      CODEC_STATUS_PAYLOAD_TOO_LARGE);
  EXPECT_TRUE(refused.empty());
}

TEST(CodecIdentifierTest, EncoderAcceptsEveryAllowedCharacter) {
  const std::string identifier =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-";
  FrameView decoded;
  const Bytes wire = roundTrip(
      {.type = FRAME_TYPE_OPEN, .streamId = STREAM_ID, .kind = identifier},
      &decoded);
  EXPECT_EQ(decoded.kind, identifier);
}

TEST(CodecIdentifierTest, EverySingleByteOutsideTheSetIsRefused) {
  for (uint32_t value = 0U; value <= 0xFFU; ++value) {
    const char character = static_cast<char>(value);
    const bool allowed = (character >= 'a' && character <= 'z') ||
                         (character >= 'A' && character <= 'Z') ||
                         (character >= '0' && character <= '9') ||
                         character == '-';
    const std::string_view identifier(&character, 1U);
    const CodecStatus expected =
        allowed ? CODEC_STATUS_OK : CODEC_STATUS_BAD_IDENTIFIER;

    Bytes wire;
    EXPECT_EQ(Codec::encode({.type = FRAME_TYPE_OPEN,
                             .streamId = STREAM_ID,
                             .kind = identifier},
                            FrameLimits{}, &wire),
              expected)
        << "byte " << value;
    EXPECT_EQ(Codec::encode({.type = FRAME_TYPE_HELLO, .sessionId = identifier},
                            FrameLimits{}, &wire),
              expected)
        << "byte " << value;

    // Same check on the decoder, with bytes the encoder would refuse to write.
    const Bytes hostile{0x02, 0x00, 0x00, 0x00,
                        0x01, 0x00, 0x01, static_cast<uint8_t>(value),
                        0x00, 0x00, 0x00, 0x00};
    FrameView decoded;
    EXPECT_EQ(Codec::decode(hostile, FrameLimits{}, &decoded), expected)
        << "byte " << value;
  }
}

TEST(CodecIdentifierTest, EncoderRefusesEmptyIdentifiers) {
  Bytes wire;
  EXPECT_EQ(Codec::encode({.type = FRAME_TYPE_OPEN, .streamId = STREAM_ID},
                          FrameLimits{}, &wire),
            CODEC_STATUS_BAD_IDENTIFIER);
  EXPECT_EQ(Codec::encode({.type = FRAME_TYPE_HELLO}, FrameLimits{}, &wire),
            CODEC_STATUS_BAD_IDENTIFIER);
  EXPECT_TRUE(wire.empty());
}

TEST(CodecIdentifierTest, EncoderRefusesIdentifierTooLongForItsPrefix) {
  const std::string identifier(70000U, 'a');
  const FrameLimits limits{.maxControlPayload = 100000U};
  Bytes wire;
  EXPECT_EQ(
      Codec::encode(
          {.type = FRAME_TYPE_OPEN, .streamId = STREAM_ID, .kind = identifier},
          limits, &wire),
      CODEC_STATUS_BAD_IDENTIFIER);
  EXPECT_TRUE(wire.empty());
}

TEST(CodecEncodeTest, RefusesReasonCodesThatMayNotBeSent) {
  const ReasonCode unknown = static_cast<ReasonCode>(200U);
  for (const ReasonCode code :
       {REASON_CODE_TRANSPORT_LOST, REASON_CODE_MAX, unknown}) {
    Bytes wire;
    EXPECT_EQ(Codec::encode(
                  {.type = FRAME_TYPE_RST, .streamId = STREAM_ID, .code = code},
                  FrameLimits{}, &wire),
              CODEC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(Codec::encode({.type = FRAME_TYPE_GOAWAY, .code = code},
                            FrameLimits{}, &wire),
              CODEC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(
        Codec::encode(
            {.type = FRAME_TYPE_OPEN_ERR, .streamId = STREAM_ID, .code = code},
            FrameLimits{}, &wire),
        CODEC_STATUS_INVALID_ARGUMENT);
    EXPECT_TRUE(wire.empty());
  }
}

TEST(CodecEncodeTest, RefusesAnotherVersion) {
  Bytes wire;
  EXPECT_EQ(
      Codec::encode({.type = FRAME_TYPE_HELLO, .version = 3U, .sessionId = "a"},
                    FrameLimits{}, &wire),
      CODEC_STATUS_INVALID_ARGUMENT);
  EXPECT_TRUE(wire.empty());
}

TEST(CodecEncodeTest, RefusesUnknownFrameTypes) {
  Bytes wire;
  EXPECT_EQ(Codec::encode({.type = FRAME_TYPE_UNKNOWN}, FrameLimits{}, &wire),
            CODEC_STATUS_UNKNOWN_TYPE);
  EXPECT_EQ(Codec::encode({.type = FRAME_TYPE_MAX}, FrameLimits{}, &wire),
            CODEC_STATUS_UNKNOWN_TYPE);
  EXPECT_TRUE(wire.empty());
}

TEST(CodecEncodeTest, RefusesTheWrongStreamId) {
  Bytes wire;
  EXPECT_EQ(Codec::encode({.type = FRAME_TYPE_DATA, .data = SAMPLE_BYTES},
                          FrameLimits{}, &wire),
            CODEC_STATUS_BAD_STREAM_ID);
  EXPECT_EQ(Codec::encode({.type = FRAME_TYPE_PING, .streamId = STREAM_ID},
                          FrameLimits{}, &wire),
            CODEC_STATUS_BAD_STREAM_ID);
  EXPECT_TRUE(wire.empty());
}

TEST(CodecEncodeTest, RefusesEmptyData) {
  Bytes wire;
  EXPECT_EQ(Codec::encode({.type = FRAME_TYPE_DATA, .streamId = STREAM_ID},
                          FrameLimits{}, &wire),
            CODEC_STATUS_BAD_PAYLOAD_SIZE);
  EXPECT_TRUE(wire.empty());
}

TEST(CodecEncodeTest, AppendsToExistingContent) {
  Bytes wire{0xAA};
  ASSERT_EQ(Codec::encode({.type = FRAME_TYPE_EOF, .streamId = 1U},
                          FrameLimits{}, &wire),
            CODEC_STATUS_OK);
  EXPECT_EQ(wire, (Bytes{0xAA, 0x06, 0x00, 0x00, 0x00, 0x01}));
}

TEST(CodecEncodeTest, FailureLeavesExistingContentUnchanged) {
  Bytes wire{0xAA, 0xBB};
  EXPECT_EQ(Codec::encode(
                {.type = FRAME_TYPE_OPEN, .streamId = STREAM_ID, .kind = "a_b"},
                FrameLimits{}, &wire),
            CODEC_STATUS_BAD_IDENTIFIER);
  EXPECT_EQ(wire, (Bytes{0xAA, 0xBB}));
}

}  // namespace
}  // namespace packetmux
