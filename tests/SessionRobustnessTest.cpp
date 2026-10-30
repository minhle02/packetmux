#include <gtest/gtest.h>

#include <cstdint>
#include <deque>
#include <iterator>
#include <map>
#include <random>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "packetmux/Session.h"
#include "support/SessionHarness.h"

namespace packetmux {
namespace {

using EventTypes = std::vector<EventType>;

constexpr SessionStatus OK = SESSION_STATUS_OK;
constexpr uint32_t PEER_STREAM = 2U;

TEST(SessionRobustnessTest, TenThousandOpenCloseCyclesLeaveNothingBehind) {
  SessionHarness harness;
  harness.establish();

  uint32_t peerStream = PEER_STREAM;
  for (size_t cycle = 0U; cycle < 10000U; ++cycle) {
    switch (cycle % 4U) {
      case 0U: {  // clean close
        const uint32_t streamId = harness.openAccepted();
        ASSERT_EQ(harness.session().sendEof(streamId), OK);
        ASSERT_EQ(harness.rx({.type = FRAME_TYPE_EOF, .streamId = streamId}),
                  OK);
        break;
      }
      case 1U: {  // we reset
        const uint32_t streamId = harness.openAccepted();
        harness.write(streamId, Bytes(100U, 0U));
        ASSERT_EQ(harness.session().resetStream(streamId), OK);
        break;
      }
      case 2U: {  // the peer resets
        const uint32_t streamId = harness.openAccepted();
        ASSERT_EQ(harness.rx({.type = FRAME_TYPE_RST,
                              .streamId = streamId,
                              .code = REASON_CODE_KILLED}),
                  OK);
        break;
      }
      default: {  // we reject
        ASSERT_EQ(
            harness.rx(
                {.type = FRAME_TYPE_OPEN, .streamId = peerStream, .kind = "k"}),
            OK);
        ASSERT_EQ(harness.session().rejectStream(peerStream,
                                                 REASON_CODE_POLICY_DENIED, {}),
                  OK);
        peerStream += 2U;
        break;
      }
    }
    harness.takeFrames();
    harness.takeEvents();
  }

  std::vector<StreamInfo> streams;
  harness.session().listStreams(&streams);
  EXPECT_TRUE(streams.empty());
  EXPECT_TRUE(harness.takeFrames().empty());
  EXPECT_TRUE(harness.takeEvents().empty());
  EXPECT_EQ(harness.state(), SESSION_STATE_ESTABLISHED);
}

TEST(SessionRobustnessTest, PingFloodWithoutReadingStaysBounded) {
  SessionHarness harness;
  harness.establish();
  size_t accepted = 0U;
  while (harness.state() == SESSION_STATE_ESTABLISHED) {
    ASSERT_EQ(harness.rx({.type = FRAME_TYPE_PING, .nonce = 1U}), OK);
    ++accepted;
    ASSERT_LT(accepted, 100000U) << "the flood was never stopped";
  }
  EXPECT_EQ(accepted, DEFAULT_MAX_QUEUED_CONTROL_FRAMES + 1U);
  EXPECT_EQ(harness.state(), SESSION_STATE_DISCONNECTED);
  const std::vector<OwnedEvent> events = harness.takeEvents();
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].code, REASON_CODE_LIMIT_EXCEEDED);
  EXPECT_EQ(harness.rx({.type = FRAME_TYPE_PING, .nonce = 1U}),
            SESSION_STATUS_NOT_CONNECTED);
}

TEST(SessionRobustnessTest, RefusedOpenFloodWithoutReadingStaysBounded) {
  SessionConfig config = SessionHarness::configFor(ROLE_CLIENT);
  config.maxStreams = 1U;
  SessionHarness harness(config);
  harness.establish();
  harness.open();

  uint32_t streamId = PEER_STREAM;
  size_t sent = 0U;
  while (harness.state() == SESSION_STATE_ESTABLISHED) {
    ASSERT_EQ(harness.rx(
                  {.type = FRAME_TYPE_OPEN, .streamId = streamId, .kind = "k"}),
              OK);
    streamId += 2U;
    ++sent;
    ASSERT_LT(sent, 100000U) << "the flood was never stopped";
  }
  EXPECT_EQ(harness.state(), SESSION_STATE_DISCONNECTED);
}

TEST(SessionRobustnessTest, InvalidFrameFloodWithoutReadingStaysBounded) {
  SessionHarness harness;
  harness.establish();
  // Every OPEN_OK on an unaccepted stream costs the peer one stream, and each
  // RST reply must be bounded like any other peer-triggered frame.
  uint32_t streamId = PEER_STREAM;
  size_t sent = 0U;
  while (harness.state() == SESSION_STATE_ESTABLISHED) {
    ASSERT_EQ(harness.rx(
                  {.type = FRAME_TYPE_OPEN, .streamId = streamId, .kind = "k"}),
              OK);
    ASSERT_EQ(harness.rx({.type = FRAME_TYPE_OPEN_OK, .streamId = streamId}),
              OK);
    streamId += 2U;
    ++sent;
    ASSERT_LT(sent, 100000U) << "the flood was never stopped";
  }
  EXPECT_EQ(harness.state(), SESSION_STATE_DISCONNECTED);
}

TEST(SessionRobustnessTest, TwoSessionsDoNotShareState) {
  SessionHarness first;
  SessionHarness second;
  first.establish();
  second.establish();
  EXPECT_EQ(first.open(), 1U);
  EXPECT_EQ(first.open(), 3U);
  EXPECT_EQ(second.open(), 1U);

  ASSERT_EQ(first.close(REASON_CODE_OK), OK);
  EXPECT_EQ(second.state(), SESSION_STATE_ESTABLISHED);
  std::vector<StreamInfo> streams;
  second.session().listStreams(&streams);
  EXPECT_EQ(streams.size(), 1U);
}

/**
 * Tracks what a host is entitled to see, and fails on anything else.
 */
class CheckedHost {
 public:
  CheckedHost(const Role i_role, const uint32_t i_seed)
      : m_config(SessionHarness::configFor(i_role)),
        m_session(m_config),
        m_random(i_seed),
        m_transcript(1469598103934665603ULL) {}

  Session& session() { return m_session; }
  uint64_t transcript() const { return m_transcript; }

  void onMessage(const Bytes& i_message, const uint64_t i_nowMs) {
    const SessionStatus status = m_session.onMessage(i_message, i_nowMs);
    ASSERT_TRUE(status == OK || status == SESSION_STATUS_NOT_CONNECTED ||
                status == SESSION_STATUS_CLOSED)
        << "status " << static_cast<uint32_t>(status);
    record(status);
    drainEvents();
  }

  // One random host action.
  void act(const uint64_t i_nowMs) {
    uint32_t streamId = pickStream();
    size_t accepted = 0U;
    const Bytes payload(m_random() % 40000U, static_cast<uint8_t>(m_random()));
    switch (m_random() % 8U) {
      case 0U: {
        const SessionStatus status =
            m_session.openStream("stress", {}, i_nowMs, &streamId);
        record(status);
        if (status == OK) {
          ASSERT_TRUE(m_everSeen.insert(streamId).second);
          m_live.insert(streamId);
        }
        break;
      }
      case 1U:
      case 2U:
      case 3U:
        record(m_session.write(streamId, payload, i_nowMs, &accepted));
        ASSERT_LE(accepted, payload.size());
        ASSERT_LE(accepted, m_config.maxQueuedBytesPerStream);
        break;
      case 4U:
        record(m_session.sendEof(streamId));
        break;
      case 5U:
        record(m_session.resetStream(streamId));
        break;
      case 6U:
        record(m_session.acceptStream(streamId));
        break;
      default:
        record(m_session.rejectStream(streamId, REASON_CODE_POLICY_DENIED, {}));
        break;
    }
    drainEvents();
  }

  void transmit(std::deque<Bytes>* const o_wire) {
    std::span<const uint8_t> message;
    const size_t limit = m_random() % 64U;
    for (size_t index = 0U; index < limit; ++index) {
      if (!m_session.pollTransmit(&message)) {
        break;
      }
      for (const uint8_t byte : message) {
        mix(byte);
      }
      o_wire->emplace_back(message.begin(), message.end());
    }
    drainEvents();
  }

  void tick(const uint64_t i_nowMs) {
    record(m_session.onTick(i_nowMs));
    drainEvents();
  }

  void connectionLost() {
    record(m_session.onConnectionLost());
    drainEvents();
  }

  void attach(const uint64_t i_nowMs) {
    record(m_session.attachConnection(i_nowMs));
    drainEvents();
  }

  void drainEvents() {
    Event event;
    while (m_session.pollEvent(&event)) {
      check(event);
      mix(event.type);
      mix(static_cast<uint8_t>(event.streamId));
      mix(event.code);
    }
  }

  // Every stream must have ended once the connection is gone.
  void expectNoLiveStreams() {
    EXPECT_TRUE(m_live.empty());
    std::vector<StreamInfo> streams;
    m_session.listStreams(&streams);
    EXPECT_TRUE(streams.empty());
  }

  bool isConnected() const {
    const SessionState state = m_session.state();
    return state == SESSION_STATE_HANDSHAKING ||
           state == SESSION_STATE_ESTABLISHED;
  }

 private:
  void check(const Event& i_event) {
    switch (i_event.type) {
      case EVENT_TYPE_STREAM_OPENED:
        ASSERT_TRUE(m_everSeen.insert(i_event.streamId).second)
            << "stream opened twice";
        m_live.insert(i_event.streamId);
        break;
      case EVENT_TYPE_STREAM_ACCEPTED:
      case EVENT_TYPE_DATA:
      case EVENT_TYPE_EOF:
      case EVENT_TYPE_WRITABLE:
        ASSERT_EQ(m_live.count(i_event.streamId), 1U)
            << "event for a stream the host does not have";
        break;
      case EVENT_TYPE_STREAM_CLOSED:
        ASSERT_EQ(m_live.erase(i_event.streamId), 1U)
            << "stream closed twice, or never opened";
        break;
      case EVENT_TYPE_DISCONNECTED:
      case EVENT_TYPE_SESSION_CLOSED:
        ASSERT_TRUE(m_live.empty()) << "connection ended before its streams";
        break;
      default:
        break;
    }
  }

  uint32_t pickStream() {
    if (m_live.empty()) {
      return 12345U;
    }
    std::set<uint32_t>::const_iterator entry = m_live.begin();
    std::advance(entry, m_random() % m_live.size());
    return *entry;
  }

  void record(const SessionStatus i_status) { mix(i_status); }

  void mix(const uint8_t i_byte) {
    m_transcript = (m_transcript ^ i_byte) * 1099511628211ULL;
  }

  SessionConfig m_config;
  Session m_session;
  std::mt19937 m_random;
  std::set<uint32_t> m_live;
  std::set<uint32_t> m_everSeen;
  uint64_t m_transcript;
};

struct RunResult {
  uint64_t clientTranscript = 0U;
  uint64_t serverTranscript = 0U;
};

// Random traffic on a connected pair, with hostile bytes mixed in.
RunResult runRandomPair(const uint32_t i_seed, const size_t i_steps) {
  CheckedHost client(ROLE_CLIENT, i_seed);
  CheckedHost server(ROLE_SERVER, i_seed + 1U);
  std::deque<Bytes> toServer;
  std::deque<Bytes> toClient;
  std::mt19937 random(i_seed + 2U);
  uint64_t nowMs = HARNESS_START_MS;

  const auto deliver = [&nowMs](std::deque<Bytes>* const o_wire,
                                CheckedHost* const o_host,
                                const size_t i_limit) {
    for (size_t index = 0U; index < i_limit && !o_wire->empty(); ++index) {
      o_host->onMessage(o_wire->front(), nowMs);
      o_wire->pop_front();
    }
  };

  client.attach(nowMs);
  server.attach(nowMs);
  for (size_t step = 0U; step < i_steps; ++step) {
    nowMs += random() % 200U;
    client.act(nowMs);
    server.act(nowMs);
    client.transmit(&toServer);
    server.transmit(&toClient);
    deliver(&toServer, &server, random() % 32U);
    deliver(&toClient, &client, random() % 32U);
    client.tick(nowMs);
    server.tick(nowMs);

    const uint32_t chaos = random() % 1000U;
    if (chaos < 3U) {  // a hostile or corrupt message
      Bytes hostile(random() % 32U);
      for (uint8_t& byte : hostile) {
        byte = static_cast<uint8_t>(random());
      }
      server.onMessage(hostile, nowMs);
    }
    if (chaos == 3U) {  // the transport drops out
      client.connectionLost();
      server.connectionLost();
    }
    if (chaos == 4U) {
      nowMs += DEFAULT_DEAD_AFTER_MS;  // long stall: keepalive fires
    }

    if (!client.isConnected() || !server.isConnected()) {
      // One side lost the connection, so the host ends it on the other too.
      toServer.clear();
      toClient.clear();
      client.connectionLost();
      server.connectionLost();
      client.tick(nowMs + DEFAULT_DRAIN_TIMEOUT_MS);
      server.tick(nowMs + DEFAULT_DRAIN_TIMEOUT_MS);
      client.expectNoLiveStreams();
      server.expectNoLiveStreams();
      if (client.session().state() == SESSION_STATE_CLOSED ||
          server.session().state() == SESSION_STATE_CLOSED) {
        break;
      }
      client.attach(nowMs);
      server.attach(nowMs);
    }
    if (::testing::Test::HasFatalFailure()) {
      break;
    }
  }
  return {client.transcript(), server.transcript()};
}

TEST(SessionRobustnessTest, RandomTrafficKeepsEveryInvariant) {
  for (uint32_t seed = 1U; seed <= 20U; ++seed) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    runRandomPair(seed, 2000U);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
  }
}

TEST(SessionRobustnessTest, TheSameSeedGivesTheSameRun) {
  const RunResult first = runRandomPair(77U, 2000U);
  const RunResult second = runRandomPair(77U, 2000U);
  EXPECT_EQ(first.clientTranscript, second.clientTranscript);
  EXPECT_EQ(first.serverTranscript, second.serverTranscript);

  const RunResult other = runRandomPair(78U, 2000U);
  EXPECT_NE(first.clientTranscript, other.clientTranscript);
}

}  // namespace
}  // namespace packetmux
