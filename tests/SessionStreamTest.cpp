#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "packetmux/Session.h"
#include "support/SessionHarness.h"

namespace packetmux {
namespace {

using EventTypes = std::vector<EventType>;
using FrameTypes = std::vector<FrameType>;

constexpr SessionStatus OK = SESSION_STATUS_OK;
constexpr uint32_t PEER_STREAM = 2U;  // a client's peer opens even IDs

StreamState stateOf(const SessionHarness& i_harness,
                    const uint32_t i_streamId) {
  std::vector<StreamInfo> streams;
  const_cast<SessionHarness&>(i_harness).session().listStreams(&streams);
  for (const StreamInfo& info : streams) {
    if (info.streamId == i_streamId) {
      return info.state;
    }
  }
  ADD_FAILURE() << "stream " << i_streamId << " not listed";
  return STREAM_STATE_MAX;
}

size_t streamCount(SessionHarness* const o_harness) {
  std::vector<StreamInfo> streams;
  o_harness->session().listStreams(&streams);
  return streams.size();
}

// --- IDs
// ----------------------------------------------------------------------

TEST(SessionStreamTest, ClientOpensOddIdsAndServerOpensEvenIds) {
  SessionHarness client(ROLE_CLIENT);
  client.establish();
  EXPECT_EQ(client.open(), 1U);
  EXPECT_EQ(client.open(), 3U);

  SessionHarness server(ROLE_SERVER);
  server.establish();
  EXPECT_EQ(server.open(), 2U);
  EXPECT_EQ(server.open(), 4U);
}

TEST(SessionStreamTest, EitherRoleAcceptsThePeersStreams) {
  SessionHarness client(ROLE_CLIENT);
  client.establish();
  ASSERT_EQ(client.rx({.type = FRAME_TYPE_OPEN, .streamId = 2U, .kind = "k"}),
            OK);
  EXPECT_EQ(typesOf(client.takeEvents()), EventTypes{EVENT_TYPE_STREAM_OPENED});

  SessionHarness server(ROLE_SERVER);
  server.establish();
  ASSERT_EQ(server.rx({.type = FRAME_TYPE_OPEN, .streamId = 1U, .kind = "k"}),
            OK);
  EXPECT_EQ(typesOf(server.takeEvents()), EventTypes{EVENT_TYPE_STREAM_OPENED});
}

TEST(SessionStreamTest, IdsContinueAcrossReconnect) {
  SessionHarness harness;
  harness.establish();
  EXPECT_EQ(harness.open(), 1U);
  ASSERT_EQ(harness.lost(), OK);
  harness.establish();
  EXPECT_EQ(harness.open(), 3U);
}

TEST(SessionStreamTest, PeerIdsMayNotGoBackAcrossReconnect) {
  SessionHarness harness;
  harness.establish();
  harness.peerOpenAccepted(4U);
  ASSERT_EQ(harness.lost(), OK);
  harness.establish();
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_OPEN, .streamId = 4U, .kind = "k"}),
            OK);
  EXPECT_EQ(harness.state(), SESSION_STATE_DRAINING);
}

TEST(SessionStreamTest, WrongParityIsAConnectionError) {
  SessionHarness harness;
  harness.establish();
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_OPEN, .streamId = 1U, .kind = "k"}),
            OK);
  EXPECT_EQ(harness.state(), SESSION_STATE_DRAINING);
}

TEST(SessionStreamTest, IdAlreadyInUseIsAConnectionError) {
  SessionHarness harness;
  harness.establish();
  harness.peerOpenAccepted(PEER_STREAM);
  ASSERT_EQ(
      harness.rx(
          {.type = FRAME_TYPE_OPEN, .streamId = PEER_STREAM, .kind = "k"}),
      OK);
  EXPECT_EQ(harness.state(), SESSION_STATE_DRAINING);
}

TEST(SessionStreamTest, DecreasingIdIsAConnectionError) {
  SessionHarness harness;
  harness.establish();
  harness.peerOpenAccepted(6U);
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_OPEN, .streamId = 4U, .kind = "k"}),
            OK);
  EXPECT_EQ(harness.state(), SESSION_STATE_DRAINING);
}

TEST(SessionStreamTest, ClosedIdCannotBeReused) {
  SessionHarness harness;
  harness.establish();
  harness.peerOpenAccepted(PEER_STREAM);
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_RST,
                        .streamId = PEER_STREAM,
                        .code = REASON_CODE_KILLED}),
            OK);
  ASSERT_EQ(
      harness.rx(
          {.type = FRAME_TYPE_OPEN, .streamId = PEER_STREAM, .kind = "k"}),
      OK);
  EXPECT_EQ(harness.state(), SESSION_STATE_DRAINING);
}

// --- S1, S2, S3: opening
// ------------------------------------------------------

TEST(SessionStreamTest, OpenSendsKindAndParams) {
  SessionHarness harness;
  harness.establish();
  const Bytes params{9U, 8U};
  uint32_t streamId = 0U;
  ASSERT_EQ(harness.session().openStream("tcp-forward", params, harness.nowMs(),
                                         &streamId),
            OK);
  const std::vector<OwnedFrame> frames = harness.takeFrames();
  ASSERT_EQ(typesOf(frames), FrameTypes{FRAME_TYPE_OPEN});
  EXPECT_EQ(frames[0].streamId, streamId);
  EXPECT_EQ(frames[0].kind, "tcp-forward");
  EXPECT_EQ(frames[0].params, params);
  EXPECT_EQ(stateOf(harness, streamId), STREAM_STATE_SENDER_OPENING);
}

TEST(SessionStreamTest, PeerOpenReachesTheHost) {
  SessionHarness harness;
  harness.establish();
  const Bytes params{1U, 2U};
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_OPEN,
                        .streamId = PEER_STREAM,
                        .kind = "logcat",
                        .params = params}),
            OK);
  const std::vector<OwnedEvent> events = harness.takeEvents();
  ASSERT_EQ(typesOf(events), EventTypes{EVENT_TYPE_STREAM_OPENED});
  EXPECT_EQ(events[0].streamId, PEER_STREAM);
  EXPECT_EQ(events[0].kind, "logcat");
  EXPECT_EQ(events[0].params, params);
  EXPECT_EQ(stateOf(harness, PEER_STREAM), STREAM_STATE_RECEIVER_OPENING);
  EXPECT_TRUE(harness.takeFrames().empty());  // the host has not answered
}

TEST(SessionStreamTest, OpenOkReachesTheHost) {
  SessionHarness harness;
  harness.establish();
  const uint32_t streamId = harness.open();
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_OPEN_OK, .streamId = streamId}), OK);
  const std::vector<OwnedEvent> events = harness.takeEvents();
  ASSERT_EQ(typesOf(events), EventTypes{EVENT_TYPE_STREAM_ACCEPTED});
  EXPECT_EQ(events[0].streamId, streamId);
  EXPECT_EQ(stateOf(harness, streamId), STREAM_STATE_OPEN);
}

TEST(SessionStreamTest, AcceptSendsOpenOk) {
  SessionHarness harness;
  harness.establish();
  ASSERT_EQ(
      harness.rx(
          {.type = FRAME_TYPE_OPEN, .streamId = PEER_STREAM, .kind = "k"}),
      OK);
  harness.takeEvents();
  ASSERT_EQ(harness.session().acceptStream(PEER_STREAM), OK);
  EXPECT_EQ(typesOf(harness.takeFrames()), FrameTypes{FRAME_TYPE_OPEN_OK});
  EXPECT_EQ(stateOf(harness, PEER_STREAM), STREAM_STATE_OPEN);
}

TEST(SessionStreamTest, OpenErrEndsTheStreamWithTheReceivedCode) {
  SessionHarness harness;
  harness.establish();
  const uint32_t streamId = harness.open();
  harness.write(streamId, Bytes{1U});
  harness.takeFrames(1U);  // OPEN only: DATA stays queued
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_OPEN_ERR,
                        .streamId = streamId,
                        .code = REASON_CODE_POLICY_DENIED}),
            OK);
  const std::vector<OwnedEvent> events = harness.takeEvents();
  ASSERT_EQ(typesOf(events), EventTypes{EVENT_TYPE_STREAM_CLOSED});
  EXPECT_EQ(events[0].code, REASON_CODE_POLICY_DENIED);
  EXPECT_TRUE(harness.takeFrames().empty());
}

TEST(SessionStreamTest, RejectSendsCodeAndMessage) {
  SessionHarness harness;
  harness.establish();
  ASSERT_EQ(
      harness.rx(
          {.type = FRAME_TYPE_OPEN, .streamId = PEER_STREAM, .kind = "k"}),
      OK);
  harness.takeEvents();
  const Bytes message{0x6EU, 0x6FU};
  ASSERT_EQ(harness.session().rejectStream(PEER_STREAM,
                                           REASON_CODE_POLICY_DENIED, message),
            OK);
  const std::vector<OwnedFrame> frames = harness.takeFrames();
  ASSERT_EQ(typesOf(frames), FrameTypes{FRAME_TYPE_OPEN_ERR});
  EXPECT_EQ(frames[0].code, REASON_CODE_POLICY_DENIED);
  EXPECT_EQ(frames[0].message, message);

  const std::vector<OwnedEvent> events = harness.takeEvents();
  ASSERT_EQ(typesOf(events), EventTypes{EVENT_TYPE_STREAM_CLOSED});
  EXPECT_EQ(events[0].code, REASON_CODE_POLICY_DENIED);
}

TEST(SessionStreamTest, FramesForARejectedStreamAreDroppedSilently) {
  SessionHarness harness;
  harness.establish();
  ASSERT_EQ(
      harness.rx(
          {.type = FRAME_TYPE_OPEN, .streamId = PEER_STREAM, .kind = "k"}),
      OK);
  ASSERT_EQ(harness.session().rejectStream(PEER_STREAM,
                                           REASON_CODE_POLICY_DENIED, {}),
            OK);
  harness.takeFrames();
  harness.takeEvents();
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_DATA,
                        .streamId = PEER_STREAM,
                        .data = Bytes{1U}}),
            OK);
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_EOF, .streamId = PEER_STREAM}), OK);
  EXPECT_TRUE(harness.takeEvents().empty());
  EXPECT_TRUE(harness.takeFrames().empty());
}

// --- Pipelining
// ---------------------------------------------------------------

TEST(SessionStreamTest, SenderMayPipelineDataAndEofBeforeOpenOk) {
  SessionHarness harness;
  harness.establish();
  const uint32_t streamId = harness.open();
  EXPECT_EQ(harness.write(streamId, Bytes{1U}), 1U);
  ASSERT_EQ(harness.session().sendEof(streamId), OK);
  EXPECT_EQ(stateOf(harness, streamId), STREAM_STATE_SENDER_OPENING_EOF_SENT);
  EXPECT_EQ(typesOf(harness.takeFrames()),
            (FrameTypes{FRAME_TYPE_OPEN, FRAME_TYPE_DATA, FRAME_TYPE_EOF}));

  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_OPEN_OK, .streamId = streamId}), OK);
  EXPECT_EQ(stateOf(harness, streamId), STREAM_STATE_HALF_CLOSED_LOCAL);

  ASSERT_EQ(
      harness.rx(
          {.type = FRAME_TYPE_DATA, .streamId = streamId, .data = Bytes{2U}}),
      OK);
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_EOF, .streamId = streamId}), OK);
  const std::vector<OwnedEvent> events = harness.takeEvents();
  ASSERT_EQ(typesOf(events),
            (EventTypes{EVENT_TYPE_STREAM_ACCEPTED, EVENT_TYPE_DATA,
                        EVENT_TYPE_EOF, EVENT_TYPE_STREAM_CLOSED}));
  EXPECT_EQ(events[3].code, REASON_CODE_OK);
}

TEST(SessionStreamTest, WriteAfterPipelinedEofIsRefused) {
  SessionHarness harness;
  harness.establish();
  const uint32_t streamId = harness.open();
  ASSERT_EQ(harness.session().sendEof(streamId), OK);
  size_t accepted = 7U;
  EXPECT_EQ(
      harness.session().write(streamId, Bytes{1U}, harness.nowMs(), &accepted),
      SESSION_STATUS_INVALID_STATE);
  EXPECT_EQ(accepted, 0U);
  EXPECT_EQ(harness.session().sendEof(streamId), SESSION_STATUS_INVALID_STATE);
}

TEST(SessionStreamTest, OpenErrAfterPipelinedEof) {
  SessionHarness harness;
  harness.establish();
  const uint32_t streamId = harness.open();
  ASSERT_EQ(harness.session().sendEof(streamId), OK);
  harness.takeFrames();
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_OPEN_ERR,
                        .streamId = streamId,
                        .code = REASON_CODE_CONNECT_FAILED}),
            OK);
  const std::vector<OwnedEvent> events = harness.takeEvents();
  ASSERT_EQ(typesOf(events), EventTypes{EVENT_TYPE_STREAM_CLOSED});
  EXPECT_EQ(events[0].code, REASON_CODE_CONNECT_FAILED);
}

TEST(SessionStreamTest, InboundDataBeforeAcceptIsDeliveredInOrder) {
  SessionHarness harness;
  harness.establish();
  const Bytes first{1U};
  const Bytes second{2U};
  ASSERT_EQ(
      harness.rx(
          {.type = FRAME_TYPE_OPEN, .streamId = PEER_STREAM, .kind = "k"}),
      OK);
  ASSERT_EQ(
      harness.rx(
          {.type = FRAME_TYPE_DATA, .streamId = PEER_STREAM, .data = first}),
      OK);
  ASSERT_EQ(
      harness.rx(
          {.type = FRAME_TYPE_DATA, .streamId = PEER_STREAM, .data = second}),
      OK);
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_EOF, .streamId = PEER_STREAM}), OK);
  const std::vector<OwnedEvent> events = harness.takeEvents();
  ASSERT_EQ(typesOf(events),
            (EventTypes{EVENT_TYPE_STREAM_OPENED, EVENT_TYPE_DATA,
                        EVENT_TYPE_DATA, EVENT_TYPE_EOF}));
  EXPECT_EQ(events[1].data, first);
  EXPECT_EQ(events[2].data, second);
  EXPECT_EQ(stateOf(harness, PEER_STREAM),
            STREAM_STATE_RECEIVER_OPENING_EOF_RECEIVED);
}

TEST(SessionStreamTest, AcceptAfterPeerEofAllowsTheReply) {
  SessionHarness harness;
  harness.establish();
  ASSERT_EQ(
      harness.rx(
          {.type = FRAME_TYPE_OPEN, .streamId = PEER_STREAM, .kind = "k"}),
      OK);
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_EOF, .streamId = PEER_STREAM}), OK);
  harness.takeEvents();

  ASSERT_EQ(harness.session().acceptStream(PEER_STREAM), OK);
  EXPECT_EQ(stateOf(harness, PEER_STREAM), STREAM_STATE_HALF_CLOSED_REMOTE);
  EXPECT_EQ(harness.write(PEER_STREAM, Bytes{1U}), 1U);
  ASSERT_EQ(harness.session().sendEof(PEER_STREAM), OK);

  EXPECT_EQ(typesOf(harness.takeFrames()),
            (FrameTypes{FRAME_TYPE_OPEN_OK, FRAME_TYPE_DATA, FRAME_TYPE_EOF}));
  const std::vector<OwnedEvent> events = harness.takeEvents();
  ASSERT_EQ(typesOf(events), EventTypes{EVENT_TYPE_STREAM_CLOSED});
  EXPECT_EQ(events[0].code, REASON_CODE_OK);
}

TEST(SessionStreamTest, ReceiverCannotWriteOrEofBeforeAccepting) {
  SessionHarness harness;
  harness.establish();
  ASSERT_EQ(
      harness.rx(
          {.type = FRAME_TYPE_OPEN, .streamId = PEER_STREAM, .kind = "k"}),
      OK);
  size_t accepted = 0U;
  EXPECT_EQ(harness.session().write(PEER_STREAM, Bytes{1U}, harness.nowMs(),
                                    &accepted),
            SESSION_STATUS_INVALID_STATE);
  EXPECT_EQ(harness.session().sendEof(PEER_STREAM),
            SESSION_STATUS_INVALID_STATE);
  EXPECT_TRUE(harness.takeFrames().empty());
}

// --- Frames invalid for the stream's state
// ------------------------------------

void expectStreamReset(SessionHarness* const o_harness,
                       const uint32_t i_streamId) {
  const std::vector<OwnedFrame> frames = o_harness->takeFrames();
  ASSERT_EQ(typesOf(frames), FrameTypes{FRAME_TYPE_RST});
  EXPECT_EQ(frames[0].streamId, i_streamId);
  EXPECT_EQ(frames[0].code, REASON_CODE_PROTOCOL_ERROR);

  const std::vector<OwnedEvent> events = o_harness->takeEvents();
  ASSERT_EQ(typesOf(events), EventTypes{EVENT_TYPE_STREAM_CLOSED});
  EXPECT_EQ(events[0].streamId, i_streamId);
  EXPECT_EQ(events[0].code, REASON_CODE_PROTOCOL_ERROR);

  EXPECT_EQ(o_harness->state(), SESSION_STATE_ESTABLISHED);
}

TEST(SessionStreamTest, DataBeforeOpenOkResetsTheStream) {
  SessionHarness harness;
  harness.establish();
  const uint32_t streamId = harness.open();
  harness.takeFrames();
  ASSERT_EQ(
      harness.rx(
          {.type = FRAME_TYPE_DATA, .streamId = streamId, .data = Bytes{1U}}),
      OK);
  expectStreamReset(&harness, streamId);
}

TEST(SessionStreamTest, EofBeforeOpenOkResetsTheStream) {
  SessionHarness harness;
  harness.establish();
  const uint32_t streamId = harness.open();
  harness.takeFrames();
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_EOF, .streamId = streamId}), OK);
  expectStreamReset(&harness, streamId);
}

TEST(SessionStreamTest, DataAfterPeerEofResetsOnlyThatStream) {
  SessionHarness harness;
  harness.establish();
  const uint32_t streamId = harness.openAccepted();
  const uint32_t other = harness.openAccepted();
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_EOF, .streamId = streamId}), OK);
  harness.takeEvents();
  ASSERT_EQ(
      harness.rx(
          {.type = FRAME_TYPE_DATA, .streamId = streamId, .data = Bytes{1U}}),
      OK);
  expectStreamReset(&harness, streamId);
  EXPECT_EQ(stateOf(harness, other), STREAM_STATE_OPEN);
}

TEST(SessionStreamTest, SecondEofResetsTheStream) {
  SessionHarness harness;
  harness.establish();
  const uint32_t streamId = harness.openAccepted();
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_EOF, .streamId = streamId}), OK);
  harness.takeEvents();
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_EOF, .streamId = streamId}), OK);
  expectStreamReset(&harness, streamId);
}

TEST(SessionStreamTest, DuplicateOpenOkResetsTheStream) {
  SessionHarness harness;
  harness.establish();
  const uint32_t streamId = harness.openAccepted();
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_OPEN_OK, .streamId = streamId}), OK);
  expectStreamReset(&harness, streamId);
}

TEST(SessionStreamTest, OpenOkOnAReceiverStreamResetsIt) {
  SessionHarness harness;
  harness.establish();
  ASSERT_EQ(
      harness.rx(
          {.type = FRAME_TYPE_OPEN, .streamId = PEER_STREAM, .kind = "k"}),
      OK);
  harness.takeEvents();
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_OPEN_OK, .streamId = PEER_STREAM}),
            OK);
  expectStreamReset(&harness, PEER_STREAM);
}

TEST(SessionStreamTest, OpenErrOnAnAcceptedStreamResetsIt) {
  SessionHarness harness;
  harness.establish();
  const uint32_t streamId = harness.openAccepted();
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_OPEN_ERR,
                        .streamId = streamId,
                        .code = REASON_CODE_KILLED}),
            OK);
  expectStreamReset(&harness, streamId);
}

// --- Host calls refused
// --------------------------------------------------------

TEST(SessionStreamTest, HostCannotAcceptOrRejectItsOwnStream) {
  SessionHarness harness;
  harness.establish();
  const uint32_t streamId = harness.open();
  EXPECT_EQ(harness.session().acceptStream(streamId),
            SESSION_STATUS_INVALID_STATE);
  EXPECT_EQ(harness.session().rejectStream(streamId, REASON_CODE_KILLED, {}),
            SESSION_STATUS_INVALID_STATE);
  EXPECT_EQ(stateOf(harness, streamId), STREAM_STATE_SENDER_OPENING);
}

TEST(SessionStreamTest, AcceptTwiceIsRefused) {
  SessionHarness harness;
  harness.establish();
  harness.peerOpenAccepted(PEER_STREAM);
  EXPECT_EQ(harness.session().acceptStream(PEER_STREAM),
            SESSION_STATUS_INVALID_STATE);
  EXPECT_EQ(harness.session().rejectStream(PEER_STREAM, REASON_CODE_KILLED, {}),
            SESSION_STATUS_INVALID_STATE);
  EXPECT_TRUE(harness.takeFrames().empty());
}

TEST(SessionStreamTest, WriteAfterEofFailsAndSendsNothing) {
  SessionHarness harness;
  harness.establish();
  const uint32_t streamId = harness.openAccepted();
  ASSERT_EQ(harness.session().sendEof(streamId), OK);
  harness.takeFrames();
  size_t accepted = 0U;
  EXPECT_EQ(
      harness.session().write(streamId, Bytes{1U}, harness.nowMs(), &accepted),
      SESSION_STATUS_INVALID_STATE);
  EXPECT_TRUE(harness.takeFrames().empty());
}

TEST(SessionStreamTest, UnknownStreamInEveryStreamCall) {
  SessionHarness harness;
  harness.establish();
  size_t accepted = 0U;
  Session& session = harness.session();
  EXPECT_EQ(session.acceptStream(99U), SESSION_STATUS_UNKNOWN_STREAM);
  EXPECT_EQ(session.rejectStream(99U, REASON_CODE_KILLED, {}),
            SESSION_STATUS_UNKNOWN_STREAM);
  EXPECT_EQ(session.write(99U, Bytes{1U}, harness.nowMs(), &accepted),
            SESSION_STATUS_UNKNOWN_STREAM);
  EXPECT_EQ(session.sendEof(99U), SESSION_STATUS_UNKNOWN_STREAM);
  EXPECT_EQ(session.resetStream(99U), SESSION_STATUS_UNKNOWN_STREAM);
}

TEST(SessionStreamTest, StreamsAreUnknownOnceDisconnected) {
  SessionHarness harness;
  harness.establish();
  const uint32_t streamId = harness.openAccepted();
  ASSERT_EQ(harness.lost(), OK);
  size_t accepted = 0U;
  EXPECT_EQ(
      harness.session().write(streamId, Bytes{1U}, harness.nowMs(), &accepted),
      SESSION_STATUS_UNKNOWN_STREAM);
}

TEST(SessionStreamTest, BadArgumentsAreRefused) {
  SessionHarness harness;
  harness.establish();
  uint32_t streamId = 0U;
  EXPECT_EQ(
      harness.session().openStream("bad kind", {}, harness.nowMs(), &streamId),
      SESSION_STATUS_INVALID_ARGUMENT);

  const uint32_t mine = harness.openAccepted();
  EXPECT_EQ(harness.session().resetStream(mine, REASON_CODE_TRANSPORT_LOST),
            SESSION_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(stateOf(harness, mine), STREAM_STATE_OPEN);

  ASSERT_EQ(
      harness.rx(
          {.type = FRAME_TYPE_OPEN, .streamId = PEER_STREAM, .kind = "k"}),
      OK);
  EXPECT_EQ(harness.session().rejectStream(PEER_STREAM,
                                           REASON_CODE_TRANSPORT_LOST, {}),
            SESSION_STATUS_INVALID_ARGUMENT);
  const Bytes tooLarge(DEFAULT_MAX_CONTROL_PAYLOAD, 0U);
  EXPECT_EQ(
      harness.session().rejectStream(PEER_STREAM, REASON_CODE_KILLED, tooLarge),
      SESSION_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(stateOf(harness, PEER_STREAM), STREAM_STATE_RECEIVER_OPENING);
}

// --- S13: aborts
// ---------------------------------------------------------------

TEST(SessionStreamTest, ResetDefaultsToKilledAndDiscardsQueuedFrames) {
  SessionHarness harness;
  harness.establish();
  const uint32_t streamId = harness.openAccepted();
  harness.write(streamId, Bytes(100U, 1U));
  ASSERT_EQ(harness.session().resetStream(streamId), OK);

  const std::vector<OwnedFrame> frames = harness.takeFrames();
  ASSERT_EQ(typesOf(frames), FrameTypes{FRAME_TYPE_RST});
  EXPECT_EQ(frames[0].code, REASON_CODE_KILLED);
  const std::vector<OwnedEvent> events = harness.takeEvents();
  ASSERT_EQ(typesOf(events), EventTypes{EVENT_TYPE_STREAM_CLOSED});
  EXPECT_EQ(events[0].code, REASON_CODE_KILLED);
}

TEST(SessionStreamTest, ReceivedRstClosesTheStreamAndSendsNothing) {
  SessionHarness harness;
  harness.establish();
  const uint32_t streamId = harness.openAccepted();
  harness.write(streamId, Bytes(100U, 1U));
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_RST,
                        .streamId = streamId,
                        .code = REASON_CODE_TIMEOUT}),
            OK);
  EXPECT_TRUE(harness.takeFrames().empty());
  const std::vector<OwnedEvent> events = harness.takeEvents();
  ASSERT_EQ(typesOf(events), EventTypes{EVENT_TYPE_STREAM_CLOSED});
  EXPECT_EQ(events[0].code, REASON_CODE_TIMEOUT);
}

TEST(SessionStreamTest, OkIsOnlyReportedForACleanClose) {
  SessionHarness harness;
  harness.establish();

  const uint32_t received = harness.openAccepted();
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_RST,
                        .streamId = received,
                        .code = REASON_CODE_OK}),
            OK);
  std::vector<OwnedEvent> events = harness.takeEvents();
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].code, REASON_CODE_PEER_CLOSED);

  const uint32_t sent = harness.openAccepted();
  ASSERT_EQ(harness.session().resetStream(sent, REASON_CODE_OK), OK);
  events = harness.takeEvents();
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].code, REASON_CODE_PEER_CLOSED);

  const uint32_t clean = harness.openAccepted();
  ASSERT_EQ(harness.session().sendEof(clean), OK);
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_EOF, .streamId = clean}), OK);
  events = harness.takeEvents();
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[1].code, REASON_CODE_OK);
}

TEST(SessionStreamTest, LateFramesAfterALocalResetAreIgnored) {
  SessionHarness harness;
  harness.establish();
  const uint32_t streamId = harness.openAccepted();
  ASSERT_EQ(harness.session().resetStream(streamId), OK);
  harness.takeFrames();
  harness.takeEvents();
  ASSERT_EQ(
      harness.rx(
          {.type = FRAME_TYPE_DATA, .streamId = streamId, .data = Bytes{1U}}),
      OK);
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_EOF, .streamId = streamId}), OK);
  EXPECT_TRUE(harness.takeEvents().empty());
  EXPECT_TRUE(harness.takeFrames().empty());
  EXPECT_EQ(harness.state(), SESSION_STATE_ESTABLISHED);
}

TEST(SessionStreamTest, FramesForAnUnknownStreamAreIgnored) {
  SessionHarness harness;
  harness.establish();
  ASSERT_EQ(
      harness.rx({.type = FRAME_TYPE_DATA, .streamId = 77U, .data = Bytes{1U}}),
      OK);
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_OPEN_OK, .streamId = 77U}), OK);
  EXPECT_TRUE(harness.takeEvents().empty());
  EXPECT_TRUE(harness.takeFrames().empty());
}

TEST(SessionStreamTest, ConnectionLossEndsStreamsInAscendingOrder) {
  SessionHarness harness;
  harness.establish();
  const uint32_t first = harness.open();
  const uint32_t second = harness.open();
  const uint32_t third = harness.open();
  ASSERT_EQ(harness.lost(), OK);
  const std::vector<OwnedEvent> events = harness.takeEvents();
  ASSERT_EQ(events.size(), 4U);
  EXPECT_EQ(events[0].streamId, first);
  EXPECT_EQ(events[1].streamId, second);
  EXPECT_EQ(events[2].streamId, third);
}

// --- S11, S12: clean close
// -----------------------------------------------------

TEST(SessionStreamTest, CleanCloseStillSendsQueuedFrames) {
  SessionHarness harness;
  harness.establish();
  const uint32_t streamId = harness.openAccepted();
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_EOF, .streamId = streamId}), OK);
  harness.takeEvents();
  harness.write(streamId, Bytes{1U, 2U});
  ASSERT_EQ(harness.session().sendEof(streamId), OK);

  // The stream is already closed, and its last frames are still on the way.
  const std::vector<OwnedEvent> events = harness.takeEvents();
  ASSERT_EQ(typesOf(events), EventTypes{EVENT_TYPE_STREAM_CLOSED});
  EXPECT_EQ(events[0].code, REASON_CODE_OK);
  EXPECT_EQ(streamCount(&harness), 0U);
  EXPECT_EQ(typesOf(harness.takeFrames()),
            (FrameTypes{FRAME_TYPE_DATA, FRAME_TYPE_EOF}));
}

// --- Limits and listing
// --------------------------------------------------------

TEST(SessionStreamTest, MaxStreamsAppliesToOurOpens) {
  SessionConfig config = SessionHarness::configFor(ROLE_CLIENT);
  config.maxStreams = 2U;
  SessionHarness harness(config);
  harness.establish();
  harness.open();
  const uint32_t second = harness.open();
  uint32_t streamId = 0U;
  EXPECT_EQ(harness.session().openStream("k", {}, harness.nowMs(), &streamId),
            SESSION_STATUS_LIMIT_EXCEEDED);

  ASSERT_EQ(harness.session().resetStream(second), OK);
  EXPECT_EQ(harness.session().openStream("k", {}, harness.nowMs(), &streamId),
            OK);
}

TEST(SessionStreamTest, MaxStreamsRefusesPeerOpensWithoutTheHost) {
  SessionConfig config = SessionHarness::configFor(ROLE_CLIENT);
  config.maxStreams = 1U;
  SessionHarness harness(config);
  harness.establish();
  harness.open();
  harness.takeFrames();
  ASSERT_EQ(
      harness.rx(
          {.type = FRAME_TYPE_OPEN, .streamId = PEER_STREAM, .kind = "k"}),
      OK);
  EXPECT_TRUE(harness.takeEvents().empty());
  const std::vector<OwnedFrame> frames = harness.takeFrames();
  ASSERT_EQ(typesOf(frames), FrameTypes{FRAME_TYPE_OPEN_ERR});
  EXPECT_EQ(frames[0].code, REASON_CODE_LIMIT_EXCEEDED);

  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_DATA,
                        .streamId = PEER_STREAM,
                        .data = Bytes{1U}}),
            OK);
  EXPECT_TRUE(harness.takeEvents().empty());
}

TEST(SessionStreamTest, ListStreamsReportsEveryField) {
  SessionHarness harness;
  harness.establish();
  const uint64_t openedAtMs = harness.nowMs();
  uint32_t mine = 0U;
  ASSERT_EQ(harness.session().openStream("mine", {}, openedAtMs, &mine), OK);
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_OPEN_OK, .streamId = mine}), OK);
  ASSERT_EQ(
      harness.rx(
          {.type = FRAME_TYPE_OPEN, .streamId = PEER_STREAM, .kind = "theirs"}),
      OK);

  harness.advance(10U);
  harness.write(mine, Bytes(5U, 0U));
  harness.advance(10U);
  ASSERT_EQ(harness.rx({.type = FRAME_TYPE_DATA,
                        .streamId = PEER_STREAM,
                        .data = Bytes(3U, 0U)}),
            OK);

  std::vector<StreamInfo> streams;
  harness.session().listStreams(&streams);
  ASSERT_EQ(streams.size(), 2U);

  EXPECT_EQ(streams[0].streamId, mine);
  EXPECT_EQ(streams[0].kind, "mine");
  EXPECT_EQ(streams[0].opener, ROLE_CLIENT);
  EXPECT_EQ(streams[0].state, STREAM_STATE_OPEN);
  EXPECT_EQ(streams[0].bytesSent, 5U);
  EXPECT_EQ(streams[0].bytesReceived, 0U);
  EXPECT_EQ(streams[0].createdAtMs, openedAtMs);
  EXPECT_EQ(streams[0].lastActivityMs, openedAtMs + 10U);

  EXPECT_EQ(streams[1].streamId, PEER_STREAM);
  EXPECT_EQ(streams[1].kind, "theirs");
  EXPECT_EQ(streams[1].opener, ROLE_SERVER);
  EXPECT_EQ(streams[1].state, STREAM_STATE_RECEIVER_OPENING);
  EXPECT_EQ(streams[1].bytesReceived, 3U);
  EXPECT_EQ(streams[1].lastActivityMs, openedAtMs + 20U);
}

}  // namespace
}  // namespace packetmux
