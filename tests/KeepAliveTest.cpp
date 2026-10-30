#include <gtest/gtest.h>

#include <cstdint>

#include "session/KeepAlive.h"

namespace packetmux {
namespace {

constexpr uint64_t PING_INTERVAL_MS = 100U;
constexpr uint64_t DEAD_AFTER_MS = 350U;
constexpr size_t MAX_OUTSTANDING = 8U;
constexpr uint64_t START_MS = 1000U;

KeepAlive makeKeepAlive() {
  KeepAlive keepAlive(PING_INTERVAL_MS, DEAD_AFTER_MS, MAX_OUTSTANDING);
  keepAlive.reset(START_MS);
  return keepAlive;
}

TEST(KeepAliveTest, NoPingJustBeforeTheInterval) {
  KeepAlive keepAlive = makeKeepAlive();
  uint64_t nonce = 0U;
  EXPECT_EQ(keepAlive.poll(START_MS + PING_INTERVAL_MS - 1U, &nonce),
            KEEP_ALIVE_ACTION_NONE);
}

TEST(KeepAliveTest, PingAtExactlyTheInterval) {
  KeepAlive keepAlive = makeKeepAlive();
  uint64_t nonce = 0U;
  EXPECT_EQ(keepAlive.poll(START_MS + PING_INTERVAL_MS, &nonce),
            KEEP_ALIVE_ACTION_SEND_PING);
  EXPECT_EQ(nonce, 1U);
}

TEST(KeepAliveTest, NoncesAreUniqueAndPingsRepeat) {
  KeepAlive keepAlive = makeKeepAlive();
  uint64_t first = 0U;
  uint64_t second = 0U;
  ASSERT_EQ(keepAlive.poll(START_MS + PING_INTERVAL_MS, &first),
            KEEP_ALIVE_ACTION_SEND_PING);
  EXPECT_EQ(keepAlive.poll(START_MS + PING_INTERVAL_MS, &second),
            KEEP_ALIVE_ACTION_NONE);
  ASSERT_EQ(keepAlive.poll(START_MS + 2U * PING_INTERVAL_MS, &second),
            KEEP_ALIVE_ACTION_SEND_PING);
  EXPECT_NE(first, second);
}

TEST(KeepAliveTest, NotDeadJustBeforeDeadAfter) {
  KeepAlive keepAlive = makeKeepAlive();
  uint64_t nonce = 0U;
  EXPECT_NE(keepAlive.poll(START_MS + DEAD_AFTER_MS - 1U, &nonce),
            KEEP_ALIVE_ACTION_DEAD);
}

TEST(KeepAliveTest, DeadAtExactlyDeadAfter) {
  KeepAlive keepAlive = makeKeepAlive();
  uint64_t nonce = 0U;
  EXPECT_EQ(keepAlive.poll(START_MS + DEAD_AFTER_MS, &nonce),
            KEEP_ALIVE_ACTION_DEAD);
}

TEST(KeepAliveTest, ReceivedFramePushesDeadBack) {
  KeepAlive keepAlive = makeKeepAlive();
  const uint64_t receivedAtMs = START_MS + DEAD_AFTER_MS - 1U;
  keepAlive.onFrameReceived(receivedAtMs);
  uint64_t nonce = 0U;
  EXPECT_NE(keepAlive.poll(START_MS + DEAD_AFTER_MS, &nonce),
            KEEP_ALIVE_ACTION_DEAD);
  EXPECT_EQ(keepAlive.poll(receivedAtMs + DEAD_AFTER_MS, &nonce),
            KEEP_ALIVE_ACTION_DEAD);
}

TEST(KeepAliveTest, DeadlineIsTheEarlierOfPingAndDead) {
  KeepAlive keepAlive = makeKeepAlive();
  EXPECT_EQ(keepAlive.nextDeadlineMs(), START_MS + PING_INTERVAL_MS);

  KeepAlive shortDead(PING_INTERVAL_MS, 50U, MAX_OUTSTANDING);
  shortDead.reset(START_MS);
  EXPECT_EQ(shortDead.nextDeadlineMs(), START_MS + 50U);
}

TEST(KeepAliveTest, RttComesFromTheMatchingPong) {
  KeepAlive keepAlive = makeKeepAlive();
  uint64_t rttMs = 0U;
  EXPECT_FALSE(keepAlive.getRttMs(&rttMs));

  uint64_t nonce = 0U;
  const uint64_t sentAtMs = START_MS + PING_INTERVAL_MS;
  ASSERT_EQ(keepAlive.poll(sentAtMs, &nonce), KEEP_ALIVE_ACTION_SEND_PING);
  keepAlive.onPong(nonce, sentAtMs + 25U);
  ASSERT_TRUE(keepAlive.getRttMs(&rttMs));
  EXPECT_EQ(rttMs, 25U);
}

TEST(KeepAliveTest, UnknownNonceIsIgnored) {
  KeepAlive keepAlive = makeKeepAlive();
  keepAlive.onPong(12345U, START_MS);
  uint64_t rttMs = 0U;
  EXPECT_FALSE(keepAlive.getRttMs(&rttMs));
}

TEST(KeepAliveTest, RepeatedPongCountsOnce) {
  KeepAlive keepAlive = makeKeepAlive();
  uint64_t nonce = 0U;
  const uint64_t sentAtMs = START_MS + PING_INTERVAL_MS;
  ASSERT_EQ(keepAlive.poll(sentAtMs, &nonce), KEEP_ALIVE_ACTION_SEND_PING);
  keepAlive.onPong(nonce, sentAtMs + 5U);
  keepAlive.onPong(nonce, sentAtMs + 90U);
  uint64_t rttMs = 0U;
  ASSERT_TRUE(keepAlive.getRttMs(&rttMs));
  EXPECT_EQ(rttMs, 5U);
}

TEST(KeepAliveTest, OutstandingPingsAreBounded) {
  KeepAlive keepAlive(PING_INTERVAL_MS, 1000000U, MAX_OUTSTANDING);
  keepAlive.reset(START_MS);

  uint64_t firstNonce = 0U;
  uint64_t lastNonce = 0U;
  uint64_t nowMs = START_MS;
  for (size_t index = 0U; index < MAX_OUTSTANDING + 1U; ++index) {
    nowMs += PING_INTERVAL_MS;
    uint64_t nonce = 0U;
    ASSERT_EQ(keepAlive.poll(nowMs, &nonce), KEEP_ALIVE_ACTION_SEND_PING);
    if (index == 0U) {
      firstNonce = nonce;
    }
    lastNonce = nonce;
  }

  // The ninth ping evicted the oldest, so its late PONG no longer counts.
  keepAlive.onPong(firstNonce, nowMs + 1U);
  uint64_t rttMs = 0U;
  EXPECT_FALSE(keepAlive.getRttMs(&rttMs));
  keepAlive.onPong(lastNonce, nowMs + 1U);
  EXPECT_TRUE(keepAlive.getRttMs(&rttMs));
}

TEST(KeepAliveTest, ResetForgetsOutstandingPingsButKeepsNoncesUnique) {
  KeepAlive keepAlive = makeKeepAlive();
  uint64_t oldNonce = 0U;
  ASSERT_EQ(keepAlive.poll(START_MS + PING_INTERVAL_MS, &oldNonce),
            KEEP_ALIVE_ACTION_SEND_PING);
  const uint64_t restartMs = START_MS + 200U;
  keepAlive.reset(restartMs);
  keepAlive.onPong(oldNonce, restartMs);
  uint64_t rttMs = 0U;
  EXPECT_FALSE(keepAlive.getRttMs(&rttMs));

  uint64_t newNonce = 0U;
  ASSERT_EQ(keepAlive.poll(restartMs + PING_INTERVAL_MS, &newNonce),
            KEEP_ALIVE_ACTION_SEND_PING);
  EXPECT_NE(newNonce, oldNonce);
}

}  // namespace
}  // namespace packetmux
