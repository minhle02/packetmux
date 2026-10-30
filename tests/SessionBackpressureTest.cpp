#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "packetmux/Session.h"
#include "support/SessionHarness.h"

namespace packetmux {
namespace {

using EventTypes = std::vector<EventType>;
using FrameTypes = std::vector<FrameType>;

constexpr SessionStatus OK = SESSION_STATUS_OK;
constexpr size_t FRAME_SIZE = 10U;

SessionConfig smallQueues(const size_t i_perStream, const size_t i_perSession) {
  SessionConfig config = SessionHarness::configFor(ROLE_CLIENT);
  config.limits.maxDataPayload = FRAME_SIZE;
  config.maxQueuedBytesPerStream = i_perStream;
  config.maxQueuedBytes = i_perSession;
  return config;
}

std::vector<uint32_t> writableStreams(const std::vector<OwnedEvent>& i_events) {
  std::vector<uint32_t> streams;
  for (const OwnedEvent& event : i_events) {
    if (event.type == EVENT_TYPE_WRITABLE) {
      streams.push_back(event.streamId);
    }
  }
  return streams;
}

TEST(SessionBackpressureTest, WriteIsSplitAtTheDataLimit) {
  SessionHarness harness(smallQueues(100U, 100U));
  harness.establish();
  const uint32_t streamId = harness.openAccepted();

  Bytes payload(25U);
  for (size_t index = 0U; index < payload.size(); ++index) {
    payload[index] = static_cast<uint8_t>(index);
  }
  EXPECT_EQ(harness.write(streamId, payload), payload.size());

  const std::vector<OwnedFrame> frames = harness.takeFrames();
  ASSERT_EQ(frames.size(), 3U);
  EXPECT_EQ(frames[0].data.size(), 10U);
  EXPECT_EQ(frames[1].data.size(), 10U);
  EXPECT_EQ(frames[2].data.size(), 5U);

  Bytes joined;
  for (const OwnedFrame& frame : frames) {
    joined.insert(joined.end(), frame.data.begin(), frame.data.end());
  }
  EXPECT_EQ(joined, payload);
}

TEST(SessionBackpressureTest, EmptyWriteDoesNothing) {
  SessionHarness harness;
  harness.establish();
  const uint32_t streamId = harness.openAccepted();
  EXPECT_EQ(harness.write(streamId, Bytes{}), 0U);
  EXPECT_TRUE(harness.takeFrames().empty());
}

TEST(SessionBackpressureTest, WriteExactlyAtTheStreamLimitIsAccepted) {
  SessionHarness harness(smallQueues(20U, 100U));
  harness.establish();
  const uint32_t streamId = harness.openAccepted();
  EXPECT_EQ(harness.write(streamId, Bytes(20U, 0U)), 20U);
  EXPECT_TRUE(writableStreams(harness.takeEvents()).empty());
}

TEST(SessionBackpressureTest, WriteOverTheStreamLimitIsPartlyRefused) {
  SessionHarness harness(smallQueues(20U, 100U));
  harness.establish();
  const uint32_t streamId = harness.openAccepted();
  EXPECT_EQ(harness.write(streamId, Bytes(25U, 0U)), 20U);
  EXPECT_EQ(harness.write(streamId, Bytes(1U, 0U)), 0U);  // a valid answer
  EXPECT_EQ(harness.takeFrames().size(), 2U);
}

TEST(SessionBackpressureTest, WriteOverTheSessionLimitIsPartlyRefused) {
  SessionHarness harness(smallQueues(100U, 30U));
  harness.establish();
  const uint32_t first = harness.openAccepted();
  const uint32_t second = harness.openAccepted();
  EXPECT_EQ(harness.write(first, Bytes(20U, 0U)), 20U);
  EXPECT_EQ(harness.write(second, Bytes(20U, 0U)), 10U);
}

TEST(SessionBackpressureTest, OneFullStreamDoesNotBlockAnother) {
  SessionHarness harness(smallQueues(20U, 100U));
  harness.establish();
  const uint32_t full = harness.openAccepted();
  const uint32_t other = harness.openAccepted();
  EXPECT_EQ(harness.write(full, Bytes(50U, 0U)), 20U);
  EXPECT_EQ(harness.write(other, Bytes(20U, 0U)), 20U);
}

TEST(SessionBackpressureTest, WritableFiresOnceWhenTheQueueHasRoom) {
  SessionHarness harness(smallQueues(20U, 100U));
  harness.establish();
  const uint32_t streamId = harness.openAccepted();
  EXPECT_EQ(harness.write(streamId, Bytes(50U, 0U)), 20U);
  EXPECT_TRUE(writableStreams(harness.takeEvents()).empty());

  harness.takeFrames(1U);
  EXPECT_EQ(writableStreams(harness.takeEvents()),
            std::vector<uint32_t>{streamId});

  harness.takeFrames();
  EXPECT_TRUE(writableStreams(harness.takeEvents()).empty());
}

TEST(SessionBackpressureTest, RepeatedRefusedWritesGiveOneWritable) {
  SessionHarness harness(smallQueues(20U, 100U));
  harness.establish();
  const uint32_t streamId = harness.openAccepted();
  EXPECT_EQ(harness.write(streamId, Bytes(50U, 0U)), 20U);
  EXPECT_EQ(harness.write(streamId, Bytes(50U, 0U)), 0U);
  EXPECT_EQ(harness.write(streamId, Bytes(50U, 0U)), 0U);
  harness.takeFrames();
  EXPECT_EQ(writableStreams(harness.takeEvents()),
            std::vector<uint32_t>{streamId});
}

TEST(SessionBackpressureTest, SessionLimitClearedByAnotherStreamGivesWritable) {
  SessionHarness harness(smallQueues(100U, 20U));
  harness.establish();
  const uint32_t filler = harness.openAccepted();
  const uint32_t blocked = harness.openAccepted();
  EXPECT_EQ(harness.write(filler, Bytes(20U, 0U)), 20U);
  EXPECT_EQ(harness.write(blocked, Bytes(5U, 0U)), 0U);

  // Only the filler's frames leave, yet the blocked stream gets room.
  harness.takeFrames(1U);
  EXPECT_EQ(writableStreams(harness.takeEvents()),
            std::vector<uint32_t>{blocked});
  EXPECT_EQ(harness.write(blocked, Bytes(5U, 0U)), 5U);
}

TEST(SessionBackpressureTest, NoWritableAfterSendEof) {
  SessionHarness harness(smallQueues(20U, 100U));
  harness.establish();
  const uint32_t streamId = harness.openAccepted();
  EXPECT_EQ(harness.write(streamId, Bytes(50U, 0U)), 20U);
  ASSERT_EQ(harness.session().sendEof(streamId), OK);
  harness.takeFrames();
  EXPECT_TRUE(writableStreams(harness.takeEvents()).empty());
}

TEST(SessionBackpressureTest, NoWritableAfterReset) {
  SessionHarness harness(smallQueues(100U, 20U));
  harness.establish();
  const uint32_t filler = harness.openAccepted();
  const uint32_t blocked = harness.openAccepted();
  EXPECT_EQ(harness.write(filler, Bytes(20U, 0U)), 20U);
  EXPECT_EQ(harness.write(blocked, Bytes(5U, 0U)), 0U);
  ASSERT_EQ(harness.session().resetStream(blocked), OK);
  harness.takeFrames();
  EXPECT_TRUE(writableStreams(harness.takeEvents()).empty());
}

TEST(SessionBackpressureTest, ResettingAFullStreamWakesABlockedOne) {
  SessionHarness harness(smallQueues(100U, 20U));
  harness.establish();
  const uint32_t filler = harness.openAccepted();
  const uint32_t blocked = harness.openAccepted();
  EXPECT_EQ(harness.write(filler, Bytes(20U, 0U)), 20U);
  EXPECT_EQ(harness.write(blocked, Bytes(5U, 0U)), 0U);

  // The queue empties without a single DATA frame leaving.
  ASSERT_EQ(harness.session().resetStream(filler), OK);
  EXPECT_EQ(writableStreams(harness.takeEvents()),
            std::vector<uint32_t>{blocked});
}

TEST(SessionBackpressureTest, PeerResetOfAFullStreamWakesABlockedOne) {
  SessionHarness harness(smallQueues(100U, 20U));
  harness.establish();
  const uint32_t filler = harness.openAccepted();
  const uint32_t blocked = harness.openAccepted();
  EXPECT_EQ(harness.write(filler, Bytes(20U, 0U)), 20U);
  EXPECT_EQ(harness.write(blocked, Bytes(5U, 0U)), 0U);

  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_RST,
                        .streamId = filler,
                        .code = REASON_CODE_KILLED}),
            OK);
  EXPECT_EQ(writableStreams(harness.takeEvents()),
            std::vector<uint32_t>{blocked});
}

TEST(SessionBackpressureTest, ResettingABlockedStreamGivesItNoWritable) {
  SessionHarness harness(smallQueues(20U, 100U));
  harness.establish();
  const uint32_t streamId = harness.openAccepted();
  EXPECT_EQ(harness.write(streamId, Bytes(50U, 0U)), 20U);
  ASSERT_EQ(harness.session().resetStream(streamId), OK);
  EXPECT_TRUE(writableStreams(harness.takeEvents()).empty());
}

TEST(SessionBackpressureTest, EofIsNotCountedAsQueuedData) {
  SessionHarness harness(smallQueues(10U, 100U));
  harness.establish();
  const uint32_t streamId = harness.openAccepted();
  EXPECT_EQ(harness.write(streamId, Bytes(10U, 0U)), 10U);
  ASSERT_EQ(harness.session().sendEof(streamId), OK);
  EXPECT_EQ(typesOf(harness.takeFrames()),
            (FrameTypes{FRAME_TYPE_DATA, FRAME_TYPE_EOF}));
}

TEST(SessionBackpressureTest, ClosedStreamsQueuedDataStillCountsUntilSent) {
  SessionHarness harness(smallQueues(100U, 20U));
  harness.establish();
  const uint32_t closing = harness.openAccepted();
  const uint32_t other = harness.openAccepted();
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_EOF, .streamId = closing}), OK);
  EXPECT_EQ(harness.write(closing, Bytes(20U, 0U)), 20U);
  ASSERT_EQ(harness.session().sendEof(closing), OK);  // clean close
  EXPECT_EQ(harness.write(other, Bytes(5U, 0U)), 0U);
  harness.takeFrames();
  EXPECT_EQ(harness.write(other, Bytes(5U, 0U)), 5U);
}

TEST(SessionBackpressureTest, StreamsTakeTurnsOnTheWire) {
  SessionHarness harness(smallQueues(100U, 100U));
  harness.establish();
  const uint32_t first = harness.openAccepted();
  const uint32_t second = harness.openAccepted();
  harness.write(first, Bytes(30U, 0U));
  harness.write(second, Bytes(30U, 0U));

  const std::vector<OwnedFrame> frames = harness.takeFrames();
  ASSERT_EQ(frames.size(), 6U);
  for (size_t index = 0U; index < frames.size(); ++index) {
    EXPECT_EQ(frames[index].streamId, index % 2U == 0U ? first : second);
  }
}

TEST(SessionBackpressureTest, InboundDataIsNotCopied) {
  SessionHarness harness;
  harness.establish();
  const uint32_t streamId = harness.openAccepted();

  Bytes message;
  const Bytes payload{1U, 2U, 3U};
  ASSERT_EQ(
      Codec::encode(
          {.type = FRAME_TYPE_DATA, .streamId = streamId, .data = payload},
          FrameLimits{}, &message),
      CODEC_STATUS_OK);
  ASSERT_EQ(harness.session().onMessage(message, harness.nowMs()), OK);

  Event event;
  ASSERT_TRUE(harness.session().pollEvent(&event));
  EXPECT_EQ(event.type, EVENT_TYPE_DATA);
  EXPECT_EQ(event.data.data(), message.data() + HEADER_SIZE);
  EXPECT_EQ(event.data.size(), payload.size());
}

TEST(SessionBackpressureTest, TransmittedMessageStaysValidUntilTheNextCall) {
  SessionHarness harness;
  harness.establish();
  const uint32_t streamId = harness.openAccepted();
  harness.write(streamId, Bytes{7U});

  std::span<const uint8_t> message;
  ASSERT_TRUE(harness.session().pollTransmit(&message));
  Event event;
  EXPECT_FALSE(harness.session().pollEvent(&event));
  EXPECT_EQ(message[0], FRAME_TYPE_DATA);
  EXPECT_EQ(message.back(), 7U);
}

}  // namespace
}  // namespace packetmux
