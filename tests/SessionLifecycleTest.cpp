#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "packetmux/Session.h"
#include "support/SessionHarness.h"

namespace packetmux {
namespace {

using EventTypes = std::vector<EventType>;
using FrameTypes = std::vector<FrameType>;

const Bytes GARBAGE{0xFFU, 0x00U};

enum SessionEvent : uint8_t {
  EVENT_ATTACH = 0U,
  EVENT_MESSAGE,
  EVENT_LOST,
  EVENT_TICK,
  EVENT_CLOSE,
  EVENT_OPEN,
  EVENT_DRAINED,
  EVENT_MAX,
};

struct Cell {
  SessionStatus status;
  SessionState next;
};

constexpr SessionStatus OK = SESSION_STATUS_OK;
constexpr SessionStatus CLOSED_STATUS = SESSION_STATUS_CLOSED;
constexpr SessionStatus NOT_CONNECTED = SESSION_STATUS_NOT_CONNECTED;
constexpr SessionStatus ALREADY = SESSION_STATUS_ALREADY_CONNECTED;
constexpr SessionState DISC = SESSION_STATE_DISCONNECTED;
constexpr SessionState HAND = SESSION_STATE_HANDSHAKING;
constexpr SessionState ESTA = SESSION_STATE_ESTABLISHED;
constexpr SessionState DRAI = SESSION_STATE_DRAINING;
constexpr SessionState CLNG = SESSION_STATE_CLOSING;
constexpr SessionState CLSD = SESSION_STATE_CLOSED;

// Written out here on purpose: this is the table from section 4.2 of
// docs/spec/PROTOCOL.md, kept independent of the state classes.
constexpr Cell TABLE[SESSION_STATE_MAX][EVENT_MAX] = {
    // attach         message              lost                 tick close open
    // drained
    {{OK, HAND},
     {NOT_CONNECTED, DISC},
     {OK, DISC},
     {OK, DISC},
     {OK, CLSD},
     {NOT_CONNECTED, DISC},
     {OK, DISC}},
    {{ALREADY, HAND},
     {OK, ESTA},
     {OK, DISC},
     {OK, DISC},
     {OK, CLNG},
     {OK, HAND},
     {OK, HAND}},
    {{ALREADY, ESTA},
     {OK, ESTA},
     {OK, DISC},
     {OK, DISC},
     {OK, CLNG},
     {OK, ESTA},
     {OK, ESTA}},
    {{ALREADY, DRAI},
     {OK, DRAI},
     {OK, DISC},
     {OK, DISC},
     {OK, CLNG},
     {NOT_CONNECTED, DRAI},
     {OK, DISC}},
    {{CLOSED_STATUS, CLNG},
     {OK, CLNG},
     {OK, CLSD},
     {OK, CLSD},
     {CLOSED_STATUS, CLNG},
     {CLOSED_STATUS, CLNG},
     {OK, CLSD}},
    {{CLOSED_STATUS, CLSD},
     {CLOSED_STATUS, CLSD},
     {OK, CLSD},
     {OK, CLSD},
     {CLOSED_STATUS, CLSD},
     {CLOSED_STATUS, CLSD},
     {OK, CLSD}},
};

void enterState(SessionHarness* const o_harness, const SessionState i_state) {
  switch (i_state) {
    case SESSION_STATE_HANDSHAKING:
      ASSERT_EQ(o_harness->attach(), OK);
      break;
    case SESSION_STATE_ESTABLISHED:
      o_harness->establish();
      break;
    case SESSION_STATE_DRAINING:
      o_harness->establish();
      ASSERT_EQ(o_harness->rxRaw(GARBAGE), OK);
      break;
    case SESSION_STATE_CLOSING:
      o_harness->establish();
      ASSERT_EQ(o_harness->close(REASON_CODE_OK), OK);
      break;
    case SESSION_STATE_CLOSED:
      ASSERT_EQ(o_harness->close(REASON_CODE_OK), OK);
      break;
    default:
      break;
  }
  ASSERT_EQ(o_harness->state(), i_state);
}

SessionStatus fire(SessionHarness* const o_harness,
                   const SessionEvent i_event) {
  uint32_t streamId = 0U;
  switch (i_event) {
    case EVENT_ATTACH:
      return o_harness->attach();
    case EVENT_MESSAGE:
      // The one message that keeps ESTABLISHED and leaves HANDSHAKING.
      return o_harness->state() == SESSION_STATE_HANDSHAKING
                 ? o_harness->rxHello()
                 : o_harness->rx({.type = FRAME_TYPE_PING, .nonce = 1U});
    case EVENT_LOST:
      return o_harness->lost();
    case EVENT_TICK:
      o_harness->advance(1000000U);  // past every deadline
      return o_harness->tick();
    case EVENT_CLOSE:
      return o_harness->close(REASON_CODE_OK);
    case EVENT_OPEN:
      return o_harness->session().openStream("kind", {}, o_harness->nowMs(),
                                             &streamId);
    default:
      o_harness->takeFrames();
      return OK;
  }
}

TEST(SessionLifecycleTest, EveryCellMatchesTheTable) {
  for (size_t state = 0U; state < SESSION_STATE_MAX; ++state) {
    for (size_t event = 0U; event < EVENT_MAX; ++event) {
      SCOPED_TRACE("state " + std::to_string(state) + " event " +
                   std::to_string(event));
      SessionHarness harness;
      enterState(&harness, static_cast<SessionState>(state));
      const Cell expected = TABLE[state][event];
      EXPECT_EQ(fire(&harness, static_cast<SessionEvent>(event)),
                expected.status);
      EXPECT_EQ(harness.state(), expected.next);
    }
  }
}

// --- T1: DISCONNECTED -> HANDSHAKING
// ------------------------------------------

TEST(SessionLifecycleTest, NewSessionIsDisconnectedAndSilent) {
  SessionHarness harness;
  EXPECT_EQ(harness.state(), DISC);
  EXPECT_TRUE(harness.takeEvents().empty());
  EXPECT_TRUE(harness.takeFrames().empty());
  EXPECT_EQ(harness.session().nextDeadlineMs(), NO_DEADLINE);
}

TEST(SessionLifecycleTest, HelloIsTheFirstFrameWithOurDetails) {
  SessionConfig config = SessionHarness::configFor(ROLE_CLIENT);
  config.capabilities = {7U, 8U};
  SessionHarness harness(config);
  ASSERT_EQ(harness.attach(), OK);
  const std::vector<OwnedFrame> frames = harness.takeFrames();
  ASSERT_EQ(frames.size(), 1U);
  EXPECT_EQ(frames[0].type, FRAME_TYPE_HELLO);
  EXPECT_EQ(frames[0].sessionId, "session-1");
}

TEST(SessionLifecycleTest, BadConfigurationIsRefusedAndNothingChanges) {
  const std::vector<SessionConfig> configs = [] {
    std::vector<SessionConfig> bad(9U, SessionHarness::configFor(ROLE_CLIENT));
    bad[0].sessionId = "not_valid";
    bad[1].sessionId = "";
    bad[2].limits.maxDataPayload = 0U;
    bad[3].maxStreams = 0U;
    bad[4].maxQueuedBytes = 0U;
    bad[5].pingIntervalMs = 0U;
    bad[6].deadAfterMs = bad[6].pingIntervalMs;
    bad[7].maxQueuedControlFrames = 3U * bad[7].maxStreams;
    bad[8].role = ROLE_MAX;
    return bad;
  }();
  for (const SessionConfig& config : configs) {
    SessionHarness harness(config);
    EXPECT_EQ(harness.attach(), SESSION_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(harness.state(), DISC);
    EXPECT_TRUE(harness.takeFrames().empty());
  }
}

// --- T2: HANDSHAKING -> ESTABLISHED
// -------------------------------------------

TEST(SessionLifecycleTest, PeerHelloIsHandedToTheHost) {
  SessionHarness harness;
  ASSERT_EQ(harness.attach(), OK);
  const Bytes capabilities{1U, 2U};
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_HELLO,
                        .sessionId = "peer-7",
                        .capabilities = capabilities}),
            OK);
  const std::vector<OwnedEvent> events = harness.takeEvents();
  ASSERT_EQ(typesOf(events), EventTypes{EVENT_TYPE_CONNECTED});
  EXPECT_EQ(events[0].peerSessionId, "peer-7");
  EXPECT_EQ(events[0].capabilities, capabilities);
}

TEST(SessionLifecycleTest, StreamOpenedDuringHandshakeGoesOutAfterHello) {
  SessionHarness harness;
  ASSERT_EQ(harness.attach(), OK);
  const uint32_t streamId = harness.open();
  EXPECT_EQ(harness.write(streamId, Bytes{1U}), 1U);
  EXPECT_EQ(typesOf(harness.takeFrames()),
            (FrameTypes{FRAME_TYPE_HELLO, FRAME_TYPE_OPEN, FRAME_TYPE_DATA}));
}

// --- T3: connection lost
// ------------------------------------------------------

TEST(SessionLifecycleTest, HostReportsLostConnection) {
  SessionHarness harness;
  harness.establish();
  const uint32_t streamId = harness.openAccepted();
  ASSERT_EQ(harness.lost(), OK);

  const std::vector<OwnedEvent> events = harness.takeEvents();
  ASSERT_EQ(typesOf(events),
            (EventTypes{EVENT_TYPE_STREAM_CLOSED, EVENT_TYPE_DISCONNECTED}));
  EXPECT_EQ(events[0].streamId, streamId);
  EXPECT_EQ(events[0].code, REASON_CODE_TRANSPORT_LOST);
  EXPECT_EQ(events[1].code, REASON_CODE_TRANSPORT_LOST);
  EXPECT_TRUE(harness.takeFrames().empty());
}

TEST(SessionLifecycleTest, LostAfterTheConnectionAlreadyEndedDoesNothing) {
  SessionHarness harness;
  harness.establish();
  ASSERT_EQ(harness.lost(), OK);
  ASSERT_EQ(typesOf(harness.takeEvents()),
            EventTypes{EVENT_TYPE_DISCONNECTED});

  EXPECT_EQ(harness.lost(), OK);
  EXPECT_EQ(harness.state(), DISC);
  EXPECT_TRUE(harness.takeEvents().empty());

  ASSERT_EQ(harness.close(REASON_CODE_OK), OK);
  ASSERT_EQ(typesOf(harness.takeEvents()),
            EventTypes{EVENT_TYPE_SESSION_CLOSED});

  EXPECT_EQ(harness.lost(), OK);
  EXPECT_EQ(harness.state(), CLSD);
  EXPECT_TRUE(harness.takeEvents().empty());
}

TEST(SessionLifecycleTest, HelloTimeoutFiresAtExactlyTheDeadline) {
  SessionHarness harness;
  ASSERT_EQ(harness.attach(), OK);
  const uint64_t deadline = harness.session().nextDeadlineMs();
  EXPECT_EQ(deadline, harness.nowMs() + DEFAULT_HELLO_TIMEOUT_MS);

  harness.advance(DEFAULT_HELLO_TIMEOUT_MS - 1U);
  ASSERT_EQ(harness.tick(), OK);
  EXPECT_EQ(harness.state(), HAND);

  harness.advance(1U);
  ASSERT_EQ(harness.tick(), OK);
  EXPECT_EQ(harness.state(), DISC);
  const std::vector<OwnedEvent> events = harness.takeEvents();
  ASSERT_EQ(typesOf(events), EventTypes{EVENT_TYPE_DISCONNECTED});
  EXPECT_EQ(events[0].code, REASON_CODE_TIMEOUT);
}

TEST(SessionLifecycleTest, KeepaliveSendsPingsAndDetectsADeadPeer) {
  SessionHarness harness;
  harness.establish();
  EXPECT_EQ(harness.session().nextDeadlineMs(),
            harness.nowMs() + DEFAULT_PING_INTERVAL_MS);

  harness.advance(DEFAULT_PING_INTERVAL_MS);
  ASSERT_EQ(harness.tick(), OK);
  EXPECT_EQ(typesOf(harness.takeFrames()), FrameTypes{FRAME_TYPE_PING});

  harness.advance(DEFAULT_DEAD_AFTER_MS - DEFAULT_PING_INTERVAL_MS);
  ASSERT_EQ(harness.tick(), OK);
  EXPECT_EQ(harness.state(), DISC);
  const std::vector<OwnedEvent> events = harness.takeEvents();
  ASSERT_EQ(typesOf(events), EventTypes{EVENT_TYPE_DISCONNECTED});
  EXPECT_EQ(events[0].code, REASON_CODE_TIMEOUT);
}

TEST(SessionLifecycleTest, AnyFramePushesTheDeadlineBack) {
  SessionHarness harness;
  harness.establish();
  const uint32_t streamId = harness.openAccepted();
  harness.advance(DEFAULT_DEAD_AFTER_MS - 1U);
  ASSERT_EQ(
      harness.rx(
          {.type = FRAME_TYPE_DATA, .streamId = streamId, .data = Bytes{1U}}),
      OK);
  harness.advance(1U);
  ASSERT_EQ(harness.tick(), OK);
  EXPECT_EQ(harness.state(), ESTA);
}

TEST(SessionLifecycleTest, EvenAFrameForAnUnknownStreamCountsAsAlive) {
  SessionHarness harness;
  harness.establish();
  harness.advance(DEFAULT_DEAD_AFTER_MS - 1U);
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_EOF, .streamId = 99U}), OK);
  harness.advance(1U);
  ASSERT_EQ(harness.tick(), OK);
  EXPECT_EQ(harness.state(), ESTA);
}

TEST(SessionLifecycleTest, PingIsAnsweredWithTheSameNonce) {
  SessionHarness harness;
  harness.establish();
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_PING, .nonce = 42U}), OK);
  const std::vector<OwnedFrame> frames = harness.takeFrames();
  ASSERT_EQ(typesOf(frames), FrameTypes{FRAME_TYPE_PONG});
  EXPECT_EQ(frames[0].nonce, 42U);
}

TEST(SessionLifecycleTest, PongGivesTheRtt) {
  SessionHarness harness;
  harness.establish();
  uint64_t rttMs = 0U;
  EXPECT_FALSE(harness.session().getRttMs(&rttMs));

  harness.advance(DEFAULT_PING_INTERVAL_MS);
  ASSERT_EQ(harness.tick(), OK);
  const std::vector<OwnedFrame> frames = harness.takeFrames();
  ASSERT_EQ(frames.size(), 1U);
  harness.advance(30U);
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_PONG, .nonce = frames[0].nonce}),
            OK);
  ASSERT_TRUE(harness.session().getRttMs(&rttMs));
  EXPECT_EQ(rttMs, 30U);
}

TEST(SessionLifecycleTest, ControlCapEndsTheConnection) {
  SessionConfig config = SessionHarness::configFor(ROLE_CLIENT);
  config.maxQueuedControlFrames = DEFAULT_MAX_STREAMS * 3U + 1U;
  SessionHarness harness(config);
  harness.establish();
  for (size_t index = 0U; index < config.maxQueuedControlFrames; ++index) {
    ASSERT_EQ(harness.rx({.type = FRAME_TYPE_PING, .nonce = 1U}), OK);
    ASSERT_EQ(harness.state(), ESTA);
  }
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_PING, .nonce = 1U}), OK);
  EXPECT_EQ(harness.state(), DISC);
  const std::vector<OwnedEvent> events = harness.takeEvents();
  ASSERT_EQ(typesOf(events), EventTypes{EVENT_TYPE_DISCONNECTED});
  EXPECT_EQ(events[0].code, REASON_CODE_LIMIT_EXCEEDED);
  EXPECT_TRUE(harness.takeFrames().empty());
}

// --- T4: connection-level protocol errors
// -------------------------------------

struct ProtocolErrorCase {
  const char* name;
  bool established;
  Bytes message;
  CodecStatus codecStatus;
};

TEST(SessionLifecycleTest, ProtocolErrorsDrainThenDisconnect) {
  const Bytes secondHello{0x01, 0, 0, 0, 0, 0, 2, 0, 0, 1, 0x61, 0, 0, 0, 0};
  const Bytes wrongParity{0x02, 0, 0, 0, 1, 0, 1, 0x61, 0, 0, 0, 0};
  const Bytes dataFrame{0x05, 0, 0, 0, 1, 0x61};
  const Bytes wrongVersion{0x01, 0, 0, 0, 0, 0, 3};
  const std::vector<ProtocolErrorCase> cases{
      {"garbage in handshake", false, GARBAGE, CODEC_STATUS_TRUNCATED},
      {"wrong version", false, wrongVersion, CODEC_STATUS_UNSUPPORTED_VERSION},
      {"frame before HELLO", false, dataFrame, CODEC_STATUS_OK},
      {"garbage when established", true, GARBAGE, CODEC_STATUS_TRUNCATED},
      {"second HELLO", true, secondHello, CODEC_STATUS_OK},
      {"wrong parity", true, wrongParity, CODEC_STATUS_OK},
  };
  for (const ProtocolErrorCase& testCase : cases) {
    SCOPED_TRACE(testCase.name);
    SessionHarness harness;  // client: a peer OPEN must be even
    if (testCase.established) {
      harness.establish();
    } else {
      ASSERT_EQ(harness.attach(), OK);
      harness.takeFrames();
    }
    ASSERT_EQ(harness.rxRaw(testCase.message), OK);
    EXPECT_EQ(harness.state(), DRAI);

    const std::vector<OwnedFrame> frames = harness.takeFrames();
    ASSERT_EQ(typesOf(frames), FrameTypes{FRAME_TYPE_GOAWAY});
    EXPECT_EQ(frames[0].code, REASON_CODE_PROTOCOL_ERROR);
    EXPECT_EQ(harness.state(), DISC);

    const std::vector<OwnedEvent> events = harness.takeEvents();
    ASSERT_EQ(typesOf(events), EventTypes{EVENT_TYPE_DISCONNECTED});
    EXPECT_EQ(events[0].code, REASON_CODE_PROTOCOL_ERROR);
    EXPECT_EQ(events[0].codecStatus, testCase.codecStatus);
  }
}

TEST(SessionLifecycleTest, DrainingEndsStreamsAtOnceAndDropsTheirData) {
  SessionHarness harness;
  harness.establish();
  const uint32_t streamId = harness.openAccepted();
  harness.write(streamId, Bytes{1U, 2U});
  ASSERT_EQ(harness.rxRaw(GARBAGE), OK);

  const std::vector<OwnedEvent> events = harness.takeEvents();
  ASSERT_EQ(typesOf(events), EventTypes{EVENT_TYPE_STREAM_CLOSED});
  EXPECT_EQ(events[0].code, REASON_CODE_TRANSPORT_LOST);
  EXPECT_EQ(typesOf(harness.takeFrames()), FrameTypes{FRAME_TYPE_GOAWAY});
}

TEST(SessionLifecycleTest, ControlFramesQueuedEarlierGoBeforeTheGoaway) {
  SessionHarness harness;
  harness.establish();
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_PING, .nonce = 1U}), OK);
  ASSERT_EQ(harness.rxRaw(GARBAGE), OK);
  EXPECT_EQ(typesOf(harness.takeFrames(1U)), FrameTypes{FRAME_TYPE_PONG});
  EXPECT_EQ(harness.state(), DRAI);  // GOAWAY not taken yet
  EXPECT_EQ(typesOf(harness.takeFrames()), FrameTypes{FRAME_TYPE_GOAWAY});
  EXPECT_EQ(harness.state(), DISC);
}

// --- T5: DRAINING -> DISCONNECTED
// ---------------------------------------------

TEST(SessionLifecycleTest, DrainTimeoutWhenTheHostCannotFlush) {
  SessionHarness harness;
  harness.establish();
  ASSERT_EQ(harness.rxRaw(GARBAGE), OK);
  EXPECT_EQ(harness.session().nextDeadlineMs(),
            harness.nowMs() + DEFAULT_DRAIN_TIMEOUT_MS);

  harness.advance(DEFAULT_DRAIN_TIMEOUT_MS - 1U);
  ASSERT_EQ(harness.tick(), OK);
  EXPECT_EQ(harness.state(), DRAI);
  harness.advance(1U);
  ASSERT_EQ(harness.tick(), OK);
  EXPECT_EQ(harness.state(), DISC);

  const std::vector<OwnedEvent> events = harness.takeEvents();
  ASSERT_EQ(typesOf(events), EventTypes{EVENT_TYPE_DISCONNECTED});
  EXPECT_EQ(events[0].code, REASON_CODE_PROTOCOL_ERROR);
  EXPECT_TRUE(harness.takeFrames().empty());
}

TEST(SessionLifecycleTest, TransportClosingWhileDrainingKeepsTheReason) {
  SessionHarness harness;
  harness.establish();
  ASSERT_EQ(harness.rxRaw(GARBAGE), OK);
  ASSERT_EQ(harness.lost(), OK);
  const std::vector<OwnedEvent> events = harness.takeEvents();
  ASSERT_EQ(typesOf(events), EventTypes{EVENT_TYPE_DISCONNECTED});
  EXPECT_EQ(events[0].code, REASON_CODE_PROTOCOL_ERROR);
}

TEST(SessionLifecycleTest, InboundMessagesAreIgnoredWhileDraining) {
  SessionHarness harness;
  harness.establish();
  ASSERT_EQ(harness.rxRaw(GARBAGE), OK);
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_PING, .nonce = 1U}), OK);
  EXPECT_EQ(typesOf(harness.takeFrames()), FrameTypes{FRAME_TYPE_GOAWAY});
}

// --- T6 and T7: closing
// -------------------------------------------------------

TEST(SessionLifecycleTest, CloseEndsStreamsAndSendsGoaway) {
  SessionHarness harness;
  harness.establish();
  const uint32_t streamId = harness.openAccepted();
  harness.write(streamId, Bytes{1U});
  ASSERT_EQ(harness.close(REASON_CODE_AUTH_REVOKED), OK);
  EXPECT_EQ(harness.state(), CLNG);

  std::vector<OwnedEvent> events = harness.takeEvents();
  ASSERT_EQ(typesOf(events), EventTypes{EVENT_TYPE_STREAM_CLOSED});
  EXPECT_EQ(events[0].code, REASON_CODE_AUTH_REVOKED);

  const std::vector<OwnedFrame> frames = harness.takeFrames();
  ASSERT_EQ(typesOf(frames), FrameTypes{FRAME_TYPE_GOAWAY});
  EXPECT_EQ(frames[0].code, REASON_CODE_AUTH_REVOKED);
  EXPECT_EQ(harness.state(), CLSD);

  events = harness.takeEvents();
  ASSERT_EQ(typesOf(events), EventTypes{EVENT_TYPE_SESSION_CLOSED});
  EXPECT_EQ(events[0].code, REASON_CODE_AUTH_REVOKED);
}

TEST(SessionLifecycleTest, CloseWithOkReportsPeerClosedToStreams) {
  SessionHarness harness;
  harness.establish();
  harness.openAccepted();
  ASSERT_EQ(harness.close(REASON_CODE_OK), OK);
  const std::vector<OwnedEvent> events = harness.takeEvents();
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].code, REASON_CODE_PEER_CLOSED);
}

TEST(SessionLifecycleTest, CloseDuringHandshakeKeepsHelloFirst) {
  SessionHarness harness;
  ASSERT_EQ(harness.attach(), OK);
  ASSERT_EQ(harness.close(REASON_CODE_OK), OK);
  EXPECT_EQ(typesOf(harness.takeFrames()),
            (FrameTypes{FRAME_TYPE_HELLO, FRAME_TYPE_GOAWAY}));
  EXPECT_EQ(harness.state(), CLSD);
}

TEST(SessionLifecycleTest, CloseWhileDrainingKeepsTheQueuedGoaway) {
  SessionHarness harness;
  harness.establish();
  ASSERT_EQ(harness.rxRaw(GARBAGE), OK);
  ASSERT_EQ(harness.close(REASON_CODE_AUTH_REVOKED), OK);
  EXPECT_EQ(harness.state(), CLNG);

  const std::vector<OwnedFrame> frames = harness.takeFrames();
  ASSERT_EQ(typesOf(frames), FrameTypes{FRAME_TYPE_GOAWAY});
  EXPECT_EQ(frames[0].code, REASON_CODE_PROTOCOL_ERROR);
  const std::vector<OwnedEvent> events = harness.takeEvents();
  ASSERT_EQ(typesOf(events), EventTypes{EVENT_TYPE_SESSION_CLOSED});
  EXPECT_EQ(events[0].code, REASON_CODE_AUTH_REVOKED);
}

TEST(SessionLifecycleTest, ClosingTimesOutWhenTheHostCannotFlush) {
  SessionHarness harness;
  harness.establish();
  ASSERT_EQ(harness.close(REASON_CODE_OK), OK);
  EXPECT_EQ(harness.session().nextDeadlineMs(),
            harness.nowMs() + DEFAULT_DRAIN_TIMEOUT_MS);
  harness.advance(DEFAULT_DRAIN_TIMEOUT_MS - 1U);
  ASSERT_EQ(harness.tick(), OK);
  EXPECT_EQ(harness.state(), CLNG);
  harness.advance(1U);
  ASSERT_EQ(harness.tick(), OK);
  EXPECT_EQ(harness.state(), CLSD);
  EXPECT_TRUE(harness.takeFrames().empty());
}

TEST(SessionLifecycleTest, CloseRefusesAnUnsendableCodeInEveryState) {
  SessionHarness harness;
  EXPECT_EQ(harness.close(REASON_CODE_TRANSPORT_LOST),
            SESSION_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(harness.state(), DISC);
}

// --- T8 and T9
// ----------------------------------------------------------------

TEST(SessionLifecycleTest, CloseWithoutAConnectionSendsNothing) {
  SessionHarness harness;
  ASSERT_EQ(harness.close(REASON_CODE_KILLED), OK);
  EXPECT_TRUE(harness.takeFrames().empty());
  const std::vector<OwnedEvent> events = harness.takeEvents();
  ASSERT_EQ(typesOf(events), EventTypes{EVENT_TYPE_SESSION_CLOSED});
  EXPECT_EQ(events[0].code, REASON_CODE_KILLED);
}

TEST(SessionLifecycleTest, PeerGoawayEndsTheSession) {
  SessionHarness harness;
  harness.establish();
  const uint32_t streamId = harness.openAccepted();
  ASSERT_EQ(
      harness.rx({.type = FRAME_TYPE_GOAWAY, .code = REASON_CODE_AUTH_REVOKED}),
      OK);
  EXPECT_EQ(harness.state(), CLSD);

  const std::vector<OwnedEvent> events = harness.takeEvents();
  ASSERT_EQ(typesOf(events),
            (EventTypes{EVENT_TYPE_STREAM_CLOSED, EVENT_TYPE_SESSION_CLOSED}));
  EXPECT_EQ(events[0].streamId, streamId);
  EXPECT_EQ(events[0].code, REASON_CODE_AUTH_REVOKED);
  EXPECT_EQ(events[1].code, REASON_CODE_AUTH_REVOKED);
  EXPECT_TRUE(harness.takeFrames().empty());
}

TEST(SessionLifecycleTest, PeerGoawayWithOkReportsPeerClosedToStreams) {
  SessionHarness harness;
  harness.establish();
  harness.openAccepted();
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_GOAWAY, .code = REASON_CODE_OK}),
            OK);
  const std::vector<OwnedEvent> events = harness.takeEvents();
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[0].code, REASON_CODE_PEER_CLOSED);
  EXPECT_EQ(events[1].code, REASON_CODE_OK);
}

// --- Across states
// ------------------------------------------------------------

TEST(SessionLifecycleTest, EachConnectionEndsWithExactlyOneFinalEvent) {
  SessionHarness harness;
  for (size_t round = 0U; round < 3U; ++round) {
    harness.establish();
    ASSERT_EQ(harness.lost(), OK);
    EXPECT_EQ(typesOf(harness.takeEvents()),
              EventTypes{EVENT_TYPE_DISCONNECTED});
  }
}

TEST(SessionLifecycleTest, HelloIsFirstAfterEveryAttach) {
  SessionHarness harness;
  harness.establish();
  harness.open();
  ASSERT_EQ(harness.lost(), OK);
  ASSERT_EQ(harness.attach(), OK);
  EXPECT_EQ(typesOf(harness.takeFrames()), FrameTypes{FRAME_TYPE_HELLO});
}

TEST(SessionLifecycleTest, NoDeadlineWhenThereIsNothingToWaitFor) {
  SessionHarness disconnected;
  EXPECT_EQ(disconnected.session().nextDeadlineMs(), NO_DEADLINE);
  SessionHarness closed;
  ASSERT_EQ(closed.close(REASON_CODE_OK), OK);
  EXPECT_EQ(closed.session().nextDeadlineMs(), NO_DEADLINE);
}

TEST(SessionLifecycleTest, EventsMustBeDrainedBeforeTheNextMessage) {
  SessionHarness harness;
  ASSERT_EQ(harness.attach(), OK);

  const Bytes hello{0x01, 0, 0, 0, 0, 0, 2, 0, 0, 1, 0x61, 0, 0, 0, 0};
  const uint64_t nowMs = harness.nowMs();
  ASSERT_EQ(harness.session().onMessage(hello, nowMs), OK);

  // CONNECTED is still undrained, and it borrows that message.
  const Bytes ping{0x08, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  EXPECT_EQ(harness.session().onMessage(ping, nowMs),
            SESSION_STATUS_EVENTS_PENDING);
  harness.takeFrames();
  EXPECT_TRUE(harness.takeFrames().empty());  // the PING was not processed

  Event event;
  while (harness.session().pollEvent(&event)) {
  }
  EXPECT_EQ(harness.session().onMessage(ping, nowMs), OK);
  EXPECT_EQ(typesOf(harness.takeFrames()), FrameTypes{FRAME_TYPE_PONG});
}

TEST(SessionLifecycleTest, SessionCanBeMoved) {
  Session first(SessionHarness::configFor(ROLE_CLIENT));
  ASSERT_EQ(first.attachConnection(HARNESS_START_MS), OK);
  Session second = std::move(first);
  EXPECT_EQ(second.state(), HAND);
  std::span<const uint8_t> message;
  EXPECT_TRUE(second.pollTransmit(&message));
}

}  // namespace
}  // namespace packetmux
