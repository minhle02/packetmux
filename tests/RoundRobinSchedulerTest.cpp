#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "session/RoundRobinScheduler.h"

namespace packetmux {
namespace {

using Bytes = std::vector<uint8_t>;

Bytes tag(const uint8_t i_value) { return Bytes{i_value}; }

// Pops everything and returns the first byte of each message.
Bytes drain(RoundRobinScheduler* const o_scheduler) {
  Bytes order;
  QueuedFrame frame;
  while (o_scheduler->pop(&frame)) {
    order.push_back(frame.message.at(0U));
  }
  return order;
}

void push(RoundRobinScheduler* const o_scheduler, const uint32_t i_streamId,
          std::vector<uint8_t>&& i_message, const size_t i_dataBytes) {
  constexpr uint64_t PLENTY = 1024U * 1024U;
  if (o_scheduler->credit(i_streamId) == 0U) {
    o_scheduler->openCredit(i_streamId, PLENTY);
  }
  o_scheduler->pushStreamFrame(i_streamId, std::move(i_message), i_dataBytes);
}

TEST(RoundRobinSchedulerTest, EmptyPopsNothing) {
  RoundRobinScheduler scheduler;
  QueuedFrame frame;
  EXPECT_FALSE(scheduler.pop(&frame));
  EXPECT_TRUE(scheduler.empty());
}

TEST(RoundRobinSchedulerTest, ControlGoesFirstInOrder) {
  RoundRobinScheduler scheduler;
  push(&scheduler, 1U, tag(10U), 1U);
  scheduler.pushControl(tag(1U));
  scheduler.pushControl(tag(2U));
  EXPECT_EQ(scheduler.controlCount(), 2U);
  EXPECT_EQ(drain(&scheduler), (Bytes{1U, 2U, 10U}));
}

TEST(RoundRobinSchedulerTest, ControlPushedLaterStillOvertakesData) {
  RoundRobinScheduler scheduler;
  push(&scheduler, 1U, tag(10U), 1U);
  push(&scheduler, 1U, tag(11U), 1U);
  QueuedFrame frame;
  ASSERT_TRUE(scheduler.pop(&frame));
  EXPECT_EQ(frame.message, tag(10U));
  scheduler.pushControl(tag(1U));
  EXPECT_EQ(drain(&scheduler), (Bytes{1U, 11U}));
}

TEST(RoundRobinSchedulerTest, OneFramePerStreamPerTurn) {
  RoundRobinScheduler scheduler;
  push(&scheduler, 1U, tag(10U), 1U);
  push(&scheduler, 1U, tag(11U), 1U);
  push(&scheduler, 1U, tag(12U), 1U);
  push(&scheduler, 3U, tag(30U), 1U);
  push(&scheduler, 5U, tag(50U), 1U);
  push(&scheduler, 5U, tag(51U), 1U);
  EXPECT_EQ(drain(&scheduler), (Bytes{10U, 30U, 50U, 11U, 51U, 12U}));
}

TEST(RoundRobinSchedulerTest, StreamOrderIsKept) {
  RoundRobinScheduler scheduler;
  push(&scheduler, 1U, tag(1U), 1U);
  push(&scheduler, 1U, tag(2U), 1U);
  push(&scheduler, 1U, tag(3U), 0U);  // EOF, after the stream's DATA
  EXPECT_EQ(drain(&scheduler), (Bytes{1U, 2U, 3U}));
}

TEST(RoundRobinSchedulerTest, StreamJoiningMidRoundWaitsItsTurn) {
  RoundRobinScheduler scheduler;
  push(&scheduler, 1U, tag(10U), 1U);
  push(&scheduler, 1U, tag(11U), 1U);
  push(&scheduler, 3U, tag(30U), 1U);
  QueuedFrame frame;
  ASSERT_TRUE(scheduler.pop(&frame));
  EXPECT_EQ(frame.message, tag(10U));
  push(&scheduler, 5U, tag(50U), 1U);
  EXPECT_EQ(drain(&scheduler), (Bytes{30U, 11U, 50U}));
}

TEST(RoundRobinSchedulerTest, DropStreamRemovesOnlyThatStream) {
  RoundRobinScheduler scheduler;
  scheduler.pushControl(tag(1U));
  push(&scheduler, 1U, tag(10U), 4U);
  push(&scheduler, 3U, tag(30U), 8U);
  push(&scheduler, 5U, tag(50U), 16U);
  scheduler.dropStream(3U);
  scheduler.dropStream(99U);
  EXPECT_EQ(scheduler.queuedBytes(3U), 0U);
  EXPECT_EQ(scheduler.totalQueuedBytes(), 20U);
  EXPECT_EQ(drain(&scheduler), (Bytes{1U, 10U, 50U}));
}

TEST(RoundRobinSchedulerTest, ByteCountersFollowEveryOperation) {
  RoundRobinScheduler scheduler;
  push(&scheduler, 1U, tag(10U), 100U);
  push(&scheduler, 1U, tag(11U), 50U);
  push(&scheduler, 3U, tag(30U), 7U);
  EXPECT_EQ(scheduler.queuedBytes(1U), 150U);
  EXPECT_EQ(scheduler.queuedBytes(3U), 7U);
  EXPECT_EQ(scheduler.totalQueuedBytes(), 157U);

  QueuedFrame frame;
  ASSERT_TRUE(scheduler.pop(&frame));
  EXPECT_EQ(frame.dataBytes, 100U);
  EXPECT_EQ(scheduler.queuedBytes(1U), 50U);
  EXPECT_EQ(scheduler.totalQueuedBytes(), 57U);

  drain(&scheduler);
  EXPECT_EQ(scheduler.totalQueuedBytes(), 0U);
  EXPECT_TRUE(scheduler.empty());
}

TEST(RoundRobinSchedulerTest, ControlFramesDoNotCountAsDataBytes) {
  RoundRobinScheduler scheduler;
  scheduler.pushControl(Bytes(100U, 0U));
  EXPECT_EQ(scheduler.totalQueuedBytes(), 0U);
  QueuedFrame frame;
  ASSERT_TRUE(scheduler.pop(&frame));
  EXPECT_EQ(frame.dataBytes, 0U);
}

TEST(RoundRobinSchedulerTest, DropAllStreamsKeepsControl) {
  RoundRobinScheduler scheduler;
  scheduler.pushControl(tag(1U));
  push(&scheduler, 1U, tag(10U), 1U);
  scheduler.dropAllStreams();
  EXPECT_EQ(scheduler.totalQueuedBytes(), 0U);
  EXPECT_EQ(drain(&scheduler), (Bytes{1U}));
}

TEST(RoundRobinSchedulerTest, ClearRemovesEverything) {
  RoundRobinScheduler scheduler;
  scheduler.pushControl(tag(1U));
  push(&scheduler, 1U, tag(10U), 1U);
  scheduler.clear();
  EXPECT_TRUE(scheduler.empty());
  EXPECT_EQ(scheduler.controlCount(), 0U);
}

TEST(RoundRobinSchedulerTest, StreamCanRequeueAfterDraining) {
  RoundRobinScheduler scheduler;
  push(&scheduler, 1U, tag(10U), 1U);
  drain(&scheduler);
  push(&scheduler, 1U, tag(11U), 1U);
  EXPECT_EQ(drain(&scheduler), (Bytes{11U}));
}

TEST(RoundRobinSchedulerTest, AStreamWithoutCreditIsParked) {
  RoundRobinScheduler scheduler;
  scheduler.openCredit(1U, 2U);
  scheduler.pushStreamFrame(1U, tag(10U), 2U);
  scheduler.pushStreamFrame(1U, tag(11U), 2U);
  QueuedFrame frame;
  ASSERT_TRUE(scheduler.pop(&frame));
  EXPECT_EQ(scheduler.credit(1U), 0U);
  EXPECT_FALSE(scheduler.pop(&frame));
  EXPECT_FALSE(scheduler.empty());

  EXPECT_TRUE(scheduler.addCredit(1U, 2U, 4U));
  EXPECT_TRUE(scheduler.pop(&frame));
  EXPECT_TRUE(scheduler.empty());
}

TEST(RoundRobinSchedulerTest, AParkedStreamDoesNotHoldUpOthers) {
  RoundRobinScheduler scheduler;
  scheduler.openCredit(1U, 0U);
  scheduler.pushStreamFrame(1U, tag(10U), 1U);
  push(&scheduler, 3U, tag(30U), 1U);
  QueuedFrame frame;
  ASSERT_TRUE(scheduler.pop(&frame));
  EXPECT_EQ(frame.message, tag(30U));
  EXPECT_FALSE(scheduler.pop(&frame));
}

TEST(RoundRobinSchedulerTest, CreditAboveTheLimitIsRefused) {
  RoundRobinScheduler scheduler;
  scheduler.openCredit(1U, 4U);
  EXPECT_TRUE(scheduler.addCredit(1U, 1U, 5U));
  EXPECT_FALSE(scheduler.addCredit(1U, 1U, 5U));
  EXPECT_EQ(scheduler.credit(1U), 5U);
}

TEST(RoundRobinSchedulerTest, CreditForAnUnknownStreamIsIgnored) {
  RoundRobinScheduler scheduler;
  EXPECT_TRUE(scheduler.addCredit(1U, 1U, 5U));
}

TEST(RoundRobinSchedulerTest, ARetiredStreamKeepsItsCreditUntilItsFramesGo) {
  RoundRobinScheduler scheduler;
  scheduler.openCredit(1U, 2U);
  scheduler.pushStreamFrame(1U, tag(10U), 2U);
  scheduler.pushStreamFrame(1U, tag(11U), 2U);
  QueuedFrame frame;
  ASSERT_TRUE(scheduler.pop(&frame));
  scheduler.retireCredit(1U);
  EXPECT_FALSE(scheduler.pop(&frame));

  EXPECT_TRUE(scheduler.addCredit(1U, 2U, 4U));
  EXPECT_TRUE(scheduler.pop(&frame));
  EXPECT_TRUE(scheduler.empty());
  EXPECT_EQ(scheduler.credit(1U), 0U);
}

TEST(RoundRobinSchedulerTest, DroppingAParkedStreamEmptiesTheScheduler) {
  RoundRobinScheduler scheduler;
  scheduler.openCredit(1U, 0U);
  scheduler.pushStreamFrame(1U, tag(10U), 1U);
  scheduler.dropStream(1U);
  EXPECT_TRUE(scheduler.empty());
}

}  // namespace
}  // namespace packetmux
