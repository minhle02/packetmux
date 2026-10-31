#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <deque>
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

constexpr SessionStatus OK = SESSION_STATUS_OK;
constexpr uint32_t RANDOM_SEED = 20261030U;

Bytes patternOf(const size_t i_size, const uint32_t i_seed) {
  Bytes bytes(i_size);
  std::mt19937 generator(i_seed);
  for (uint8_t& byte : bytes) {
    byte = static_cast<uint8_t>(generator());
  }
  return bytes;
}

/**
 * A host: pushes each stream's pending bytes when it may, and collects what
 * arrives. Follows the loop the Session header describes.
 */
class PairHost {
 public:
  explicit PairHost(const Role i_role)
      : m_session(SessionHarness::configFor(i_role)),
        m_nowMs(HARNESS_START_MS) {}

  Session& session() { return m_session; }

  // What each stream still has to send, and whether to send EOF after that.
  void queueSend(const uint32_t i_streamId, const Bytes& i_bytes,
                 const bool i_thenEof) {
    m_toSend[i_streamId] = {i_bytes, 0U, i_thenEof, false};
  }

  void autoReply(const Bytes& i_bytes) { m_autoReply = i_bytes; }

  void onMessage(const Bytes& i_message) {
    ASSERT_EQ(m_session.onMessage(i_message, m_nowMs), OK);
    drainEvents();
  }

  void pump() {
    pushWrites();
    drainEvents();
  }

  // Up to i_limit frames. Returns false if there was nothing to send.
  bool transmit(std::deque<Bytes>* const o_wire, const size_t i_limit) {
    bool sent = false;
    std::span<const uint8_t> message;
    for (size_t index = 0U; index < i_limit; ++index) {
      if (!m_session.pollTransmit(&message)) {
        break;
      }
      o_wire->emplace_back(message.begin(), message.end());
      sent = true;
    }
    drainEvents();
    return sent;
  }

  void drainEvents() {
    Event event;
    while (m_session.pollEvent(&event)) {
      m_seen.push_back(event.type);
      switch (event.type) {
        case EVENT_TYPE_STREAM_OPENED:
          ASSERT_EQ(m_session.acceptStream(event.streamId), OK);
          if (!m_autoReply.empty()) {
            queueSend(event.streamId, m_autoReply, true);
          }
          break;
        case EVENT_TYPE_DATA:
          m_received[event.streamId].insert(m_received[event.streamId].end(),
                                            event.data.begin(),
                                            event.data.end());
          ASSERT_EQ(m_session.consume(event.streamId, event.data.size()), OK);
          break;
        case EVENT_TYPE_EOF:
          m_eofReceived.insert(event.streamId);
          break;
        case EVENT_TYPE_STREAM_CLOSED:
          m_closed[event.streamId] = event.code;
          break;
        default:
          break;
      }
    }
  }

  void pushWrites() {
    for (std::pair<const uint32_t, Pending>& entry : m_toSend) {
      Pending& pending = entry.second;
      if (m_closed.count(entry.first) != 0U) {
        continue;
      }
      if (pending.offset < pending.bytes.size()) {
        size_t accepted = 0U;
        const std::span<const uint8_t> rest =
            std::span<const uint8_t>(pending.bytes).subspan(pending.offset);
        ASSERT_EQ(m_session.write(entry.first, rest, m_nowMs, &accepted), OK);
        pending.offset += accepted;
      }
      if (pending.offset == pending.bytes.size() && pending.thenEof &&
          !pending.eofSent) {
        ASSERT_EQ(m_session.sendEof(entry.first), OK);
        pending.eofSent = true;
      }
    }
  }

  uint32_t open() {
    uint32_t streamId = 0U;
    EXPECT_EQ(m_session.openStream("test", {}, m_nowMs, &streamId), OK);
    return streamId;
  }

  const Bytes& received(const uint32_t i_streamId) {
    return m_received[i_streamId];
  }

  bool isClosed(const uint32_t i_streamId) const {
    return m_closed.count(i_streamId) != 0U;
  }

  ReasonCode closeCode(const uint32_t i_streamId) const {
    return m_closed.at(i_streamId);
  }

  bool sawEof(const uint32_t i_streamId) const {
    return m_eofReceived.count(i_streamId) != 0U;
  }

  size_t seenCount(const EventType i_type) const {
    return static_cast<size_t>(
        std::count(m_seen.begin(), m_seen.end(), i_type));
  }

 private:
  struct Pending {
    Bytes bytes;
    size_t offset = 0U;
    bool thenEof = false;
    bool eofSent = false;
  };

  Session m_session;
  uint64_t m_nowMs;
  std::map<uint32_t, Pending> m_toSend;
  std::map<uint32_t, Bytes> m_received;
  std::map<uint32_t, ReasonCode> m_closed;
  std::set<uint32_t> m_eofReceived;
  std::vector<EventType> m_seen;
  Bytes m_autoReply;
};

/**
 * Two hosts joined by in-memory wires. The frames moved per step are random, so
 * the two sides do not run in lockstep.
 */
class Pair {
 public:
  Pair()
      : m_client(ROLE_CLIENT), m_server(ROLE_SERVER), m_random(RANDOM_SEED) {}

  PairHost& client() { return m_client; }
  PairHost& server() { return m_server; }

  void connect() {
    m_toServer.clear();
    m_toClient.clear();
    ASSERT_EQ(m_client.session().attachConnection(HARNESS_START_MS), OK);
    ASSERT_EQ(m_server.session().attachConnection(HARNESS_START_MS), OK);
    run();
    ASSERT_EQ(m_client.session().state(), SESSION_STATE_ESTABLISHED);
    ASSERT_EQ(m_server.session().state(), SESSION_STATE_ESTABLISHED);
  }

  // Moves frames until both sides are idle.
  void run() {
    size_t idleSteps = 0U;
    while (idleSteps < 3U) {
      m_client.pump();
      m_server.pump();
      const bool clientSent = m_client.transmit(&m_toServer, burst());
      const bool serverSent = m_server.transmit(&m_toClient, burst());
      const bool moved = deliver(&m_toServer, &m_server, burst()) |
                         deliver(&m_toClient, &m_client, burst());
      idleSteps = clientSent || serverSent || moved ? 0U : idleSteps + 1U;
      if (::testing::Test::HasFatalFailure()) {
        return;
      }
    }
  }

  // Moves a bounded number of frames, then stops mid-transfer.
  void runFor(const size_t i_steps) {
    for (size_t step = 0U; step < i_steps; ++step) {
      m_client.pump();
      m_server.pump();
      m_client.transmit(&m_toServer, 4U);
      m_server.transmit(&m_toClient, 4U);
      deliver(&m_toServer, &m_server, 4U);
      deliver(&m_toClient, &m_client, 4U);
    }
  }

  void dropConnection() {
    m_toServer.clear();
    m_toClient.clear();
    ASSERT_EQ(m_client.session().onConnectionLost(), OK);
    ASSERT_EQ(m_server.session().onConnectionLost(), OK);
    m_client.drainEvents();
    m_server.drainEvents();
  }

 private:
  size_t burst() { return 1U + m_random() % 32U; }

  bool deliver(std::deque<Bytes>* const o_wire, PairHost* const o_host,
               const size_t i_limit) {
    bool moved = false;
    for (size_t index = 0U; index < i_limit && !o_wire->empty(); ++index) {
      o_host->onMessage(o_wire->front());
      o_wire->pop_front();
      moved = true;
    }
    return moved;
  }

  PairHost m_client;
  PairHost m_server;
  std::deque<Bytes> m_toServer;
  std::deque<Bytes> m_toClient;
  std::mt19937 m_random;
};

TEST(SessionPairTest, HandshakeBothWays) {
  Pair pair;
  pair.connect();
  EXPECT_EQ(pair.client().seenCount(EVENT_TYPE_CONNECTED), 1U);
  EXPECT_EQ(pair.server().seenCount(EVENT_TYPE_CONNECTED), 1U);
}

TEST(SessionPairTest, RequestAndReplyWithCleanClose) {
  Pair pair;
  const Bytes request = patternOf(100U, 1U);
  const Bytes reply = patternOf(50000U, 2U);
  pair.server().autoReply(reply);
  pair.connect();

  const uint32_t streamId = pair.client().open();
  pair.client().queueSend(streamId, request, true);
  pair.run();

  EXPECT_EQ(pair.server().received(streamId), request);
  EXPECT_EQ(pair.client().received(streamId), reply);
  EXPECT_TRUE(pair.client().sawEof(streamId));
  EXPECT_TRUE(pair.server().sawEof(streamId));
  EXPECT_EQ(pair.client().closeCode(streamId), REASON_CODE_OK);
  EXPECT_EQ(pair.server().closeCode(streamId), REASON_CODE_OK);
}

TEST(SessionPairTest, ServerOpensAStreamToo) {
  Pair pair;
  const Bytes request = patternOf(70000U, 3U);
  const Bytes reply = patternOf(40000U, 4U);
  pair.client().autoReply(reply);
  pair.connect();

  const uint32_t streamId = pair.server().open();
  EXPECT_EQ(streamId % 2U, 0U);
  pair.server().queueSend(streamId, request, true);
  pair.run();

  EXPECT_EQ(pair.client().received(streamId), request);
  EXPECT_EQ(pair.server().received(streamId), reply);
  EXPECT_EQ(pair.client().closeCode(streamId), REASON_CODE_OK);
  EXPECT_EQ(pair.server().closeCode(streamId), REASON_CODE_OK);
}

TEST(SessionPairTest, HalfCloseKeepsTheOtherDirectionOpen) {
  Pair pair;
  pair.connect();
  const uint32_t streamId = pair.client().open();
  pair.client().queueSend(streamId, patternOf(10U, 5U), true);
  pair.run();
  EXPECT_TRUE(pair.server().sawEof(streamId));
  EXPECT_FALSE(pair.server().isClosed(streamId));

  // The server keeps sending long after the client finished its direction.
  const Bytes tail = patternOf(300000U, 6U);
  pair.server().queueSend(streamId, tail, false);
  pair.run();
  EXPECT_EQ(pair.client().received(streamId), tail);
  EXPECT_FALSE(pair.client().isClosed(streamId));

  pair.server().queueSend(streamId, {}, true);
  pair.run();
  EXPECT_EQ(pair.client().closeCode(streamId), REASON_CODE_OK);
  EXPECT_EQ(pair.server().closeCode(streamId), REASON_CODE_OK);
}

TEST(SessionPairTest, ConcurrentStreamsArriveIntact) {
  Pair pair;
  pair.connect();

  constexpr size_t STREAM_COUNT = 16U;
  constexpr size_t MAX_STREAM_BYTES = 8U * 1024U * 1024U;
  std::vector<uint32_t> streams;
  std::vector<Bytes> sentByClient;
  std::vector<Bytes> sentByServer;
  for (size_t index = 0U; index < STREAM_COUNT; ++index) {
    // Sizes from a few bytes up to 8 MiB, on both sides of the frame size.
    const size_t size = MAX_STREAM_BYTES >> index;
    const uint32_t streamId = pair.client().open();
    streams.push_back(streamId);
    sentByClient.push_back(patternOf(size, static_cast<uint32_t>(index)));
    sentByServer.push_back(
        patternOf(size / 2U, static_cast<uint32_t>(index + 100U)));
    pair.client().queueSend(streamId, sentByClient.back(), true);
  }
  pair.run();
  // The server's replies start once all requests are in, so both directions
  // of every stream end up carrying data.
  for (size_t index = 0U; index < STREAM_COUNT; ++index) {
    pair.server().queueSend(streams[index], sentByServer[index], true);
  }
  pair.run();

  for (size_t index = 0U; index < STREAM_COUNT; ++index) {
    SCOPED_TRACE("stream index " + std::to_string(index));
    EXPECT_EQ(pair.server().received(streams[index]), sentByClient[index]);
    EXPECT_EQ(pair.client().received(streams[index]), sentByServer[index]);
    EXPECT_EQ(pair.client().closeCode(streams[index]), REASON_CODE_OK);
    EXPECT_EQ(pair.server().closeCode(streams[index]), REASON_CODE_OK);
  }
}

TEST(SessionPairTest, ConnectionLossMidTransferThenNewStreamsWork) {
  Pair pair;
  pair.connect();
  const uint32_t before = pair.client().open();
  pair.client().queueSend(before, patternOf(5000000U, 7U), true);
  pair.runFor(10U);
  ASSERT_FALSE(pair.client().isClosed(before));

  pair.dropConnection();
  EXPECT_EQ(pair.client().closeCode(before), REASON_CODE_TRANSPORT_LOST);
  EXPECT_EQ(pair.server().closeCode(before), REASON_CODE_TRANSPORT_LOST);
  EXPECT_EQ(pair.client().session().state(), SESSION_STATE_DISCONNECTED);

  pair.connect();
  const uint32_t after = pair.client().open();
  EXPECT_GT(after, before);
  const Bytes payload = patternOf(100000U, 8U);
  pair.client().queueSend(after, payload, true);
  pair.run();
  EXPECT_EQ(pair.server().received(after), payload);
}

TEST(SessionPairTest, CloseFromTheClientEndsBothSessions) {
  Pair pair;
  pair.connect();
  const uint32_t streamId = pair.client().open();
  pair.run();
  ASSERT_EQ(
      pair.client().session().close(REASON_CODE_AUTH_REVOKED, HARNESS_START_MS),
      OK);
  pair.run();
  EXPECT_EQ(pair.client().session().state(), SESSION_STATE_CLOSED);
  EXPECT_EQ(pair.server().session().state(), SESSION_STATE_CLOSED);
  EXPECT_EQ(pair.client().closeCode(streamId), REASON_CODE_AUTH_REVOKED);
  EXPECT_EQ(pair.server().closeCode(streamId), REASON_CODE_AUTH_REVOKED);
}

TEST(SessionPairTest, CloseFromTheServerEndsBothSessions) {
  Pair pair;
  pair.connect();
  ASSERT_EQ(pair.server().session().close(REASON_CODE_OK, HARNESS_START_MS),
            OK);
  pair.run();
  EXPECT_EQ(pair.client().session().state(), SESSION_STATE_CLOSED);
  EXPECT_EQ(pair.server().session().state(), SESSION_STATE_CLOSED);
  EXPECT_EQ(pair.client().seenCount(EVENT_TYPE_SESSION_CLOSED), 1U);
  EXPECT_EQ(pair.server().seenCount(EVENT_TYPE_SESSION_CLOSED), 1U);
}

TEST(SessionPairTest, ProtocolErrorClosesThePeerAndDisconnectsTheDetector) {
  Pair pair;
  pair.connect();

  const Bytes garbage{0xFFU};
  pair.server().onMessage(garbage);
  EXPECT_EQ(pair.server().session().state(), SESSION_STATE_DRAINING);
  pair.run();

  // The known asymmetry: the detecting side keeps its session.
  EXPECT_EQ(pair.server().session().state(), SESSION_STATE_DISCONNECTED);
  EXPECT_EQ(pair.client().session().state(), SESSION_STATE_CLOSED);
}

}  // namespace
}  // namespace packetmux
