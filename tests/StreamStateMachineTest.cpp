#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "packetmux/Codec.h"
#include "session/EventQueue.h"
#include "session/RoundRobinScheduler.h"
#include "stream/StreamManager.h"
#include "stream/StreamProcessor.h"
#include "stream/StreamStateMachine.h"

namespace packetmux {
namespace {

using Bytes = std::vector<uint8_t>;

constexpr uint32_t STREAM_ID = 5U;
constexpr uint64_t NOW_MS = 1000U;
constexpr size_t STATE_COUNT = STREAM_STATE_CLOSED;

enum Input : uint8_t {
  INPUT_OPEN_OK = 0U,
  INPUT_OPEN_ERR,
  INPUT_DATA,
  INPUT_EOF,
  INPUT_RST,
  INPUT_ACCEPT,
  INPUT_REJECT,
  INPUT_WRITE,
  INPUT_SEND_EOF,
  INPUT_RESET,
  INPUT_MAX,
};

// Short names for the table below.
constexpr StreamState SO = STREAM_STATE_SENDER_OPENING;
constexpr StreamState SE = STREAM_STATE_SENDER_OPENING_EOF_SENT;
constexpr StreamState RO = STREAM_STATE_RECEIVER_OPENING;
constexpr StreamState RE = STREAM_STATE_RECEIVER_OPENING_EOF_RECEIVED;
constexpr StreamState OP = STREAM_STATE_OPEN;
constexpr StreamState HL = STREAM_STATE_HALF_CLOSED_LOCAL;
constexpr StreamState HR = STREAM_STATE_HALF_CLOSED_REMOTE;
constexpr StreamState CL = STREAM_STATE_CLOSED;
constexpr StreamState STAY = STREAM_STATE_MAX;
constexpr uint8_t INVALID = 0xFFU;  // the cell is "not allowed"

// Written out here on purpose: this is the table from section 5.3 of
// docs/spec/PROTOCOL.md, kept independent of the state classes.
constexpr uint8_t TABLE[STATE_COUNT][INPUT_MAX] = {
    // OPEN_OK  OPEN_ERR  DATA     EOF      RST  ACCEPT   REJECT   WRITE
    // SEND_EOF RESET
    {OP, CL, INVALID, INVALID, CL, INVALID, INVALID, STAY, SE, CL},  // SO
    {HL, CL, INVALID, INVALID, CL, INVALID, INVALID, INVALID, INVALID,
     CL},                                                            // SE
    {INVALID, INVALID, STAY, RE, CL, OP, CL, INVALID, INVALID, CL},  // RO
    {INVALID, INVALID, INVALID, INVALID, CL, HR, CL, INVALID, INVALID,
     CL},                                                              // RE
    {INVALID, INVALID, STAY, HR, CL, INVALID, INVALID, STAY, HL, CL},  // OP
    {INVALID, INVALID, STAY, CL, CL, INVALID, INVALID, INVALID, INVALID,
     CL},  // HL
    {INVALID, INVALID, INVALID, INVALID, CL, INVALID, INVALID, STAY, CL,
     CL},  // HR
};

class StreamStateMachineTest : public ::testing::Test {
 protected:
  StreamStateMachineTest()
      : m_processor(m_config, &m_scheduler, &m_events),
        m_stream({.streamId = STREAM_ID, .kind = "tcp-forward"},
                 STREAM_STATE_SENDER_OPENING, &m_processor) {
    m_processor.setNow(NOW_MS);
    m_processor.setPeerReceiveWindow(DEFAULT_RECEIVE_WINDOW);
    m_scheduler.openCredit(STREAM_ID, DEFAULT_RECEIVE_WINDOW);
  }

  StreamStep fire(const Input i_input) {
    const Bytes payload{1U, 2U, 3U};
    size_t accepted = 0U;
    switch (i_input) {
      case INPUT_OPEN_OK:
        return m_stream.onFrame(
            {.type = FRAME_TYPE_OPEN_OK, .streamId = STREAM_ID});
      case INPUT_OPEN_ERR:
        return m_stream.onFrame({.type = FRAME_TYPE_OPEN_ERR,
                                 .streamId = STREAM_ID,
                                 .code = REASON_CODE_POLICY_DENIED});
      case INPUT_DATA:
        return m_stream.onFrame(
            {.type = FRAME_TYPE_DATA, .streamId = STREAM_ID, .data = payload});
      case INPUT_EOF:
        return m_stream.onFrame(
            {.type = FRAME_TYPE_EOF, .streamId = STREAM_ID});
      case INPUT_RST:
        return m_stream.onFrame({.type = FRAME_TYPE_RST,
                                 .streamId = STREAM_ID,
                                 .code = REASON_CODE_KILLED});
      case INPUT_ACCEPT:
        return m_stream.onAccept();
      case INPUT_REJECT:
        return m_stream.onReject(REASON_CODE_POLICY_DENIED, payload);
      case INPUT_WRITE:
        return m_stream.onWrite(payload, &accepted);
      case INPUT_SEND_EOF:
        return m_stream.onSendEof();
      default:
        return m_stream.onReset(REASON_CODE_KILLED);
    }
  }

  std::vector<FrameType> popFrameTypes() {
    std::vector<FrameType> types;
    QueuedFrame frame;
    while (m_scheduler.pop(&frame)) {
      FrameView decoded;
      EXPECT_EQ(Codec::decode(frame.message, FrameLimits{}, &decoded),
                CODEC_STATUS_OK);
      types.push_back(decoded.type);
    }
    return types;
  }

  std::vector<EventType> popEventTypes() {
    std::vector<EventType> types;
    Event event;
    while (m_events.pop(&event)) {
      types.push_back(event.type);
    }
    return types;
  }

  SessionConfig m_config;
  RoundRobinScheduler m_scheduler;
  EventQueue m_events;
  StreamProcessor m_processor;
  StreamStateMachine m_stream;
};

TEST_F(StreamStateMachineTest, EveryCellMatchesTheTable) {
  for (size_t state = 0U; state < STATE_COUNT; ++state) {
    for (size_t input = 0U; input < INPUT_MAX; ++input) {
      SCOPED_TRACE("state " + std::to_string(state) + " input " +
                   std::to_string(input));
      m_scheduler.clear();
      popEventTypes();
      m_stream.setState(static_cast<StreamState>(state));

      const StreamStep step = fire(static_cast<Input>(input));
      const uint8_t expected = TABLE[state][input];

      if (expected == INVALID) {
        EXPECT_FALSE(step.valid);
        EXPECT_TRUE(popFrameTypes().empty());
        EXPECT_TRUE(popEventTypes().empty());
      } else {
        EXPECT_TRUE(step.valid);
        EXPECT_EQ(step.next, static_cast<StreamState>(expected));
      }
      // A handler never changes state itself: the manager applies the step.
      EXPECT_EQ(m_stream.state(), static_cast<StreamState>(state));
    }
  }
}

TEST_F(StreamStateMachineTest, EveryStateReportsItsOwnLabel) {
  for (size_t state = 0U; state < STATE_COUNT; ++state) {
    m_stream.setState(static_cast<StreamState>(state));
    EXPECT_EQ(m_stream.state(), static_cast<StreamState>(state));
  }
}

TEST_F(StreamStateMachineTest, ValidCellsHaveTheRightEffects) {
  struct Effect {
    StreamState state;
    Input input;
    std::vector<FrameType> frames;
    std::vector<EventType> events;
  };
  const std::vector<Effect> effects{
      {SO, INPUT_OPEN_OK, {}, {EVENT_TYPE_STREAM_ACCEPTED}},
      {SE, INPUT_OPEN_OK, {}, {EVENT_TYPE_STREAM_ACCEPTED}},
      {SO, INPUT_OPEN_ERR, {}, {}},
      {SO, INPUT_WRITE, {FRAME_TYPE_DATA}, {}},
      {OP, INPUT_WRITE, {FRAME_TYPE_DATA}, {}},
      {HR, INPUT_WRITE, {FRAME_TYPE_DATA}, {}},
      {SO, INPUT_SEND_EOF, {FRAME_TYPE_EOF}, {}},
      {OP, INPUT_SEND_EOF, {FRAME_TYPE_EOF}, {}},
      {HR, INPUT_SEND_EOF, {FRAME_TYPE_EOF}, {}},
      {RO, INPUT_DATA, {}, {EVENT_TYPE_DATA}},
      {OP, INPUT_DATA, {}, {EVENT_TYPE_DATA}},
      {HL, INPUT_DATA, {}, {EVENT_TYPE_DATA}},
      {RO, INPUT_EOF, {}, {EVENT_TYPE_EOF}},
      {OP, INPUT_EOF, {}, {EVENT_TYPE_EOF}},
      {HL, INPUT_EOF, {}, {EVENT_TYPE_EOF}},
      {RO, INPUT_ACCEPT, {FRAME_TYPE_OPEN_OK}, {}},
      {RE, INPUT_ACCEPT, {FRAME_TYPE_OPEN_OK}, {}},
      {RO, INPUT_REJECT, {FRAME_TYPE_OPEN_ERR}, {}},
      {RE, INPUT_REJECT, {FRAME_TYPE_OPEN_ERR}, {}},
      {OP, INPUT_RST, {}, {}},
      {OP, INPUT_RESET, {FRAME_TYPE_RST}, {}},
  };
  for (const Effect& effect : effects) {
    SCOPED_TRACE("state " + std::to_string(effect.state) + " input " +
                 std::to_string(effect.input));
    m_scheduler.clear();
    m_scheduler.openCredit(STREAM_ID, DEFAULT_RECEIVE_WINDOW);
    popEventTypes();
    m_stream.setState(effect.state);
    fire(effect.input);
    EXPECT_EQ(popFrameTypes(), effect.frames);
    EXPECT_EQ(popEventTypes(), effect.events);
  }
}

TEST_F(StreamStateMachineTest, CloseCodes) {
  m_stream.setState(HL);
  EXPECT_EQ(fire(INPUT_EOF).closeCode, REASON_CODE_OK);
  m_stream.setState(HR);
  EXPECT_EQ(fire(INPUT_SEND_EOF).closeCode, REASON_CODE_OK);
  m_stream.setState(OP);
  EXPECT_EQ(fire(INPUT_RST).closeCode, REASON_CODE_KILLED);
  m_stream.setState(OP);
  EXPECT_EQ(fire(INPUT_RESET).closeCode, REASON_CODE_KILLED);
  m_stream.setState(SO);
  EXPECT_EQ(fire(INPUT_OPEN_ERR).closeCode, REASON_CODE_POLICY_DENIED);
  m_stream.setState(RO);
  EXPECT_EQ(fire(INPUT_REJECT).closeCode, REASON_CODE_POLICY_DENIED);
}

TEST_F(StreamStateMachineTest, ResetWithOkIsNotReportedAsCleanClose) {
  m_stream.setState(OP);
  const StreamStep sent = m_stream.onReset(REASON_CODE_OK);
  EXPECT_EQ(sent.closeCode, REASON_CODE_PEER_CLOSED);

  m_stream.setState(OP);
  const StreamStep received = m_stream.onFrame(
      {.type = FRAME_TYPE_RST, .streamId = STREAM_ID, .code = REASON_CODE_OK});
  EXPECT_EQ(received.closeCode, REASON_CODE_PEER_CLOSED);
}

TEST_F(StreamStateMachineTest, ResetDropsQueuedFrames) {
  m_stream.setState(OP);
  fire(INPUT_WRITE);
  fire(INPUT_RESET);
  EXPECT_EQ(popFrameTypes(), std::vector<FrameType>{FRAME_TYPE_RST});
}

TEST_F(StreamStateMachineTest, ReceivedRstDropsQueuedFrames) {
  m_stream.setState(OP);
  fire(INPUT_WRITE);
  fire(INPUT_RST);
  EXPECT_TRUE(popFrameTypes().empty());
}

TEST_F(StreamStateMachineTest, EofStaysAfterQueuedData) {
  m_stream.setState(OP);
  fire(INPUT_WRITE);
  fire(INPUT_SEND_EOF);
  EXPECT_EQ(popFrameTypes(),
            (std::vector<FrameType>{FRAME_TYPE_DATA, FRAME_TYPE_EOF}));
}

TEST_F(StreamStateMachineTest, StatisticsCountBothDirections) {
  m_stream.setState(OP);
  m_processor.setNow(NOW_MS + 5U);
  fire(INPUT_WRITE);
  m_processor.setNow(NOW_MS + 9U);
  fire(INPUT_DATA);
  fire(INPUT_DATA);

  const StreamInfo info = m_stream.info();
  EXPECT_EQ(info.streamId, STREAM_ID);
  EXPECT_EQ(info.kind, "tcp-forward");
  EXPECT_EQ(info.state, OP);
  EXPECT_EQ(info.bytesSent, 3U);
  EXPECT_EQ(info.bytesReceived, 6U);
  EXPECT_EQ(info.lastActivityMs, NOW_MS + 9U);
}

TEST(StreamManagerTest, StopsAtTheEndOfTheIdSpace) {
  const SessionConfig config;
  RoundRobinScheduler scheduler;
  EventQueue events;
  StreamManager manager(config, &scheduler, &events);

  manager.setNextStreamId(0xFFFFFFFFULL);  // the last odd ID
  uint32_t streamId = 0U;
  EXPECT_EQ(manager.open("a", {}, &streamId), SESSION_STATUS_OK);
  EXPECT_EQ(streamId, 0xFFFFFFFFU);
  EXPECT_EQ(manager.open("a", {}, &streamId), SESSION_STATUS_IDS_EXHAUSTED);
  EXPECT_EQ(manager.count(), 1U);
}

TEST(StreamManagerTest, BadKindDoesNotUseAnId) {
  const SessionConfig config;
  RoundRobinScheduler scheduler;
  EventQueue events;
  StreamManager manager(config, &scheduler, &events);

  uint32_t streamId = 0U;
  EXPECT_EQ(manager.open("not_valid", {}, &streamId),
            SESSION_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(manager.count(), 0U);
  EXPECT_TRUE(scheduler.empty());
  EXPECT_EQ(manager.open("valid", {}, &streamId), SESSION_STATUS_OK);
  EXPECT_EQ(streamId, 1U);
}

}  // namespace
}  // namespace packetmux
