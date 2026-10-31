#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "packetmux/Codec.h"
#include "packetmux/Session.h"

namespace packetmux {
namespace {

using Bytes = std::vector<uint8_t>;
using Json = nlohmann::json;

constexpr uint64_t START_MS = 1000U;

const std::string CONFORMANCE_DIR = PACKETMUX_CONFORMANCE_DIR;

const std::map<std::string, SessionStatus> STATUS_NAMES{
    {"OK", SESSION_STATUS_OK},
    {"CLOSED", SESSION_STATUS_CLOSED},
    {"NOT_CONNECTED", SESSION_STATUS_NOT_CONNECTED},
    {"ALREADY_CONNECTED", SESSION_STATUS_ALREADY_CONNECTED},
    {"EVENTS_PENDING", SESSION_STATUS_EVENTS_PENDING},
    {"UNKNOWN_STREAM", SESSION_STATUS_UNKNOWN_STREAM},
    {"INVALID_STATE", SESSION_STATUS_INVALID_STATE},
    {"LIMIT_EXCEEDED", SESSION_STATUS_LIMIT_EXCEEDED},
    {"IDS_EXHAUSTED", SESSION_STATUS_IDS_EXHAUSTED},
    {"INVALID_ARGUMENT", SESSION_STATUS_INVALID_ARGUMENT},
};

const std::map<std::string, CodecStatus> CODEC_NAMES{
    {"OK", CODEC_STATUS_OK},
    {"TRUNCATED", CODEC_STATUS_TRUNCATED},
    {"UNSUPPORTED_VERSION", CODEC_STATUS_UNSUPPORTED_VERSION},
    {"BAD_FLAG", CODEC_STATUS_BAD_FLAG},
};

const std::map<std::string, ReasonCode> REASON_NAMES{
    {"OK", REASON_CODE_OK},
    {"PEER_CLOSED", REASON_CODE_PEER_CLOSED},
    {"CONNECT_FAILED", REASON_CODE_CONNECT_FAILED},
    {"POLICY_DENIED", REASON_CODE_POLICY_DENIED},
    {"LIMIT_EXCEEDED", REASON_CODE_LIMIT_EXCEEDED},
    {"TRANSPORT_LOST", REASON_CODE_TRANSPORT_LOST},
    {"KILLED", REASON_CODE_KILLED},
    {"AUTH_REVOKED", REASON_CODE_AUTH_REVOKED},
    {"TIMEOUT", REASON_CODE_TIMEOUT},
    {"PROTOCOL_ERROR", REASON_CODE_PROTOCOL_ERROR},
    {"UNSUPPORTED_KIND", REASON_CODE_UNSUPPORTED_KIND},
    {"SESSION_NOT_FOUND", REASON_CODE_SESSION_NOT_FOUND},
};

const std::map<std::string, FrameType> FRAME_NAMES{
    {"HELLO", FRAME_TYPE_HELLO},     {"OPEN", FRAME_TYPE_OPEN},
    {"OPEN_OK", FRAME_TYPE_OPEN_OK}, {"OPEN_ERR", FRAME_TYPE_OPEN_ERR},
    {"DATA", FRAME_TYPE_DATA},       {"EOF", FRAME_TYPE_EOF},
    {"RST", FRAME_TYPE_RST},         {"PING", FRAME_TYPE_PING},
    {"PONG", FRAME_TYPE_PONG},       {"GOAWAY", FRAME_TYPE_GOAWAY},
};

const std::map<std::string, EventType> EVENT_NAMES{
    {"CONNECTED", EVENT_TYPE_CONNECTED},
    {"STREAM_OPENED", EVENT_TYPE_STREAM_OPENED},
    {"STREAM_ACCEPTED", EVENT_TYPE_STREAM_ACCEPTED},
    {"DATA", EVENT_TYPE_DATA},
    {"EOF", EVENT_TYPE_EOF},
    {"WRITABLE", EVENT_TYPE_WRITABLE},
    {"STREAM_CLOSED", EVENT_TYPE_STREAM_CLOSED},
    {"DISCONNECTED", EVENT_TYPE_DISCONNECTED},
    {"SESSION_CLOSED", EVENT_TYPE_SESSION_CLOSED},
};

const std::map<std::string, SessionState> STATE_NAMES{
    {"DISCONNECTED", SESSION_STATE_DISCONNECTED},
    {"HANDSHAKING", SESSION_STATE_HANDSHAKING},
    {"ESTABLISHED", SESSION_STATE_ESTABLISHED},
    {"DRAINING", SESSION_STATE_DRAINING},
    {"CLOSING", SESSION_STATE_CLOSING},
    {"CLOSED", SESSION_STATE_CLOSED},
};

const std::map<std::string, StreamState> STREAM_STATE_NAMES{
    {"SENDER_OPENING", STREAM_STATE_SENDER_OPENING},
    {"SENDER_OPENING_EOF_SENT", STREAM_STATE_SENDER_OPENING_EOF_SENT},
    {"RECEIVER_OPENING", STREAM_STATE_RECEIVER_OPENING},
    {"RECEIVER_OPENING_EOF_RECEIVED",
     STREAM_STATE_RECEIVER_OPENING_EOF_RECEIVED},
    {"OPEN", STREAM_STATE_OPEN},
    {"HALF_CLOSED_LOCAL", STREAM_STATE_HALF_CLOSED_LOCAL},
    {"HALF_CLOSED_REMOTE", STREAM_STATE_HALF_CLOSED_REMOTE},
};

Json readJson(const std::string& i_path) {
  std::ifstream file(CONFORMANCE_DIR + "/" + i_path);
  EXPECT_TRUE(file.is_open()) << i_path;
  return Json::parse(file);
}

Bytes fromHex(const std::string& i_hex) {
  Bytes bytes;
  for (size_t index = 0U; index + 1U < i_hex.size(); index += 2U) {
    bytes.push_back(
        static_cast<uint8_t>(std::stoul(i_hex.substr(index, 2U), nullptr, 16)));
  }
  return bytes;
}

Bytes dataOf(const Json& i_value) {
  if (i_value.is_string()) {
    return fromHex(i_value.get<std::string>());
  }
  const Bytes unit = fromHex(i_value.at("repeat").get<std::string>());
  Bytes data;
  for (size_t count = i_value.at("count").get<size_t>(); count > 0U; --count) {
    data.insert(data.end(), unit.begin(), unit.end());
  }
  return data;
}

Bytes bytesField(const Json& i_fields, const char* const i_name) {
  return i_fields.contains(i_name) ? dataOf(i_fields.at(i_name)) : Bytes{};
}

std::string textField(const Json& i_fields, const char* const i_name) {
  return i_fields.value(i_name, std::string{});
}

template <typename Value>
void expectBytes(const Json& i_expected, const char* const i_name,
                 const Value& i_actual) {
  if (i_expected.contains(i_name)) {
    EXPECT_EQ(Bytes(i_actual.begin(), i_actual.end()),
              dataOf(i_expected.at(i_name)))
        << i_expected.dump();
  }
}

template <typename Item>
void expectCommon(const Json& i_expected, const Item& i_actual) {
  EXPECT_EQ(i_actual.streamId, i_expected.value("stream_id", 0U))
      << i_expected.dump();
  if (i_expected.contains("code")) {
    EXPECT_EQ(i_actual.code, REASON_NAMES.at(i_expected.at("code")))
        << i_expected.dump();
  }
  expectBytes(i_expected, "capabilities", i_actual.capabilities);
  expectBytes(i_expected, "params", i_actual.params);
  expectBytes(i_expected, "data", i_actual.data);
  if (i_expected.contains("kind")) {
    EXPECT_EQ(i_actual.kind, i_expected.at("kind").get<std::string>());
  }
}

/** One session under test, against the peer that the scenario plays. */
class Harness {
 public:
  explicit Harness(const Json& i_scenario)
      : m_config(configOf(i_scenario)),
        m_session(m_config),
        m_nowMs(START_MS),
        m_lastPing(0U) {}

  void run(const Json& i_step) {
    const std::string op = i_step.at("op");
    const std::map<std::string, std::function<void()>>::const_iterator found =
        m_steps.find(op);
    ASSERT_NE(found, m_steps.end()) << "unknown step " << op;
    m_step = &i_step;
    found->second();
  }

  void expectNothingLeft() {
    EXPECT_EQ(takeFrames(nullptr), 0U);
    EXPECT_EQ(takeEvents(nullptr), 0U);
  }

 private:
  static SessionConfig configOf(const Json& i_scenario) {
    SessionConfig config;
    config.role =
        i_scenario.at("role") == "server" ? ROLE_SERVER : ROLE_CLIENT;
    config.sessionId = "session-1";
    const Json fields = i_scenario.value("config", Json::object());
    config.capabilities = bytesField(fields, "capabilities");
    config.limits.maxDataPayload =
        fields.value("max_data_payload", config.limits.maxDataPayload);
    config.maxStreams = fields.value("max_streams", config.maxStreams);
    config.maxQueuedBytes =
        fields.value("max_queued_bytes", config.maxQueuedBytes);
    config.maxQueuedBytesPerStream = fields.value(
        "max_queued_bytes_per_stream", config.maxQueuedBytesPerStream);
    config.maxQueuedControlFrames = fields.value(
        "max_queued_control_frames", config.maxQueuedControlFrames);
    config.helloTimeoutMs =
        fields.value("hello_timeout_ms", config.helloTimeoutMs);
    config.pingIntervalMs =
        fields.value("ping_interval_ms", config.pingIntervalMs);
    config.deadAfterMs = fields.value("dead_after_ms", config.deadAfterMs);
    return config;
  }

  const Json& step() const { return *m_step; }

  void check(const SessionStatus i_status) const {
    EXPECT_EQ(i_status,
              STATUS_NAMES.at(step().value("expect_status", std::string("OK"))))
        << step().dump();
  }

  void rx() {
    Bytes message;
    if (step().contains("wire")) {
      message = fromHex(step().at("wire"));
    } else {
      const Json& fields = step().at("frame");
      const Bytes capabilities = bytesField(fields, "capabilities");
      const Bytes params = bytesField(fields, "params");
      const Bytes text = bytesField(fields, "message");
      const Bytes data = bytesField(fields, "data");
      const std::string sessionId = textField(fields, "session_id");
      const std::string kind = textField(fields, "kind");
      const Json nonce = fields.value("nonce", Json(0U));
      const FrameView frame{
          .type = FRAME_NAMES.at(fields.at("type")),
          .streamId = fields.value("stream_id", 0U),
          .resuming = fields.value("resuming", false),
          .sessionId = sessionId,
          .capabilities = capabilities,
          .kind = kind,
          .params = params,
          .code =
              REASON_NAMES.at(fields.value("code", std::string("OK"))),
          .message = text,
          .data = data,
          .nonce = nonce.is_string() ? m_lastPing : nonce.get<uint64_t>(),
      };
      ASSERT_EQ(Codec::encode(frame, FrameLimits{}, &message), CODEC_STATUS_OK);
    }
    // Events borrow the message, and a refused call leaves the last one in use.
    m_messages.push_back(std::move(message));
    check(m_session.onMessage(m_messages.back(), m_nowMs));
  }

  void open() {
    uint32_t streamId = 0U;
    const Bytes params = bytesField(step(), "params");
    const std::string kind = step().at("kind");
    check(m_session.openStream(kind, params, m_nowMs, &streamId));
    if (step().contains("expect_id")) {
      EXPECT_EQ(streamId, step().at("expect_id").get<uint32_t>());
    }
  }

  void write() {
    size_t accepted = 0U;
    const Bytes data = dataOf(step().at("data"));
    check(m_session.write(step().at("stream"), data, m_nowMs, &accepted));
    if (step().contains("expect_accepted")) {
      EXPECT_EQ(accepted, step().at("expect_accepted").get<size_t>());
    }
  }

  // With i_expected: compares every frame in order. Returns how many there were.
  size_t takeFrames(const Json* const i_expected) {
    size_t count = 0U;
    std::span<const uint8_t> message;
    while (m_session.pollTransmit(&message)) {
      FrameView frame;
      EXPECT_EQ(Codec::decode(message, FrameLimits{}, &frame), CODEC_STATUS_OK);
      if (frame.type == FRAME_TYPE_PING) {
        m_lastPing = frame.nonce;
      }
      if (i_expected != nullptr) {
        EXPECT_LT(count, i_expected->size()) << step().dump();
        if (count < i_expected->size()) {
          const Json& fields = i_expected->at(count);
          EXPECT_EQ(frame.type, FRAME_NAMES.at(fields.at("type")))
              << fields.dump();
          expectCommon(fields, frame);
          expectBytes(fields, "message", frame.message);
          if (fields.contains("session_id")) {
            EXPECT_EQ(frame.sessionId,
                      fields.at("session_id").get<std::string>());
          }
          if (fields.contains("resuming")) {
            EXPECT_EQ(frame.resuming, fields.at("resuming").get<bool>());
          }
          if (fields.contains("nonce")) {
            EXPECT_EQ(frame.nonce, fields.at("nonce").get<uint64_t>());
          }
        }
      }
      ++count;
    }
    if (i_expected != nullptr) {
      EXPECT_EQ(count, i_expected->size()) << step().dump();
    }
    return count;
  }

  size_t takeEvents(const Json* const i_expected) {
    size_t count = 0U;
    Event event;
    while (m_session.pollEvent(&event)) {
      if (i_expected != nullptr) {
        EXPECT_LT(count, i_expected->size()) << step().dump();
        if (count < i_expected->size()) {
          const Json& fields = i_expected->at(count);
          EXPECT_EQ(event.type, EVENT_NAMES.at(fields.at("type")))
              << fields.dump();
          expectCommon(fields, event);
          if (fields.contains("peer_session_id")) {
            EXPECT_EQ(event.peerSessionId,
                      fields.at("peer_session_id").get<std::string>());
          }
          if (fields.contains("codec_status")) {
            EXPECT_EQ(event.codecStatus,
                      CODEC_NAMES.at(fields.at("codec_status")));
          }
        }
      }
      ++count;
    }
    if (i_expected != nullptr) {
      EXPECT_EQ(count, i_expected->size()) << step().dump();
    }
    return count;
  }

  void expectDeadline() const {
    const uint64_t deadline = m_session.nextDeadlineMs();
    if (step().at("ms").is_null()) {
      EXPECT_EQ(deadline, NO_DEADLINE);
    } else {
      EXPECT_EQ(deadline - m_nowMs, step().at("ms").get<uint64_t>());
    }
  }

  void expectRtt() const {
    uint64_t rttMs = 0U;
    const bool hasRtt = m_session.getRttMs(&rttMs);
    EXPECT_EQ(hasRtt, !step().at("ms").is_null());
    if (hasRtt) {
      EXPECT_EQ(rttMs, step().at("ms").get<uint64_t>());
    }
  }

  void expectStreams() const {
    std::vector<StreamInfo> actual;
    m_session.listStreams(&actual);
    const Json& expected = step().at("streams");
    ASSERT_EQ(actual.size(), expected.size());
    std::sort(actual.begin(), actual.end(),
              [](const StreamInfo& i_left, const StreamInfo& i_right) {
                return i_left.streamId < i_right.streamId;
              });
    for (size_t index = 0U; index < actual.size(); ++index) {
      const Json& fields = expected.at(index);
      EXPECT_EQ(actual[index].streamId, fields.at("stream_id").get<uint32_t>());
      if (fields.contains("state")) {
        EXPECT_EQ(actual[index].state, STREAM_STATE_NAMES.at(fields.at("state")));
      }
      if (fields.contains("kind")) {
        EXPECT_EQ(actual[index].kind, fields.at("kind").get<std::string>());
      }
      if (fields.contains("opener")) {
        EXPECT_EQ(static_cast<uint32_t>(actual[index].opener),
                  fields.at("opener").get<uint32_t>());
      }
      EXPECT_EQ(actual[index].bytesSent,
                fields.value("bytes_sent", actual[index].bytesSent));
      EXPECT_EQ(actual[index].bytesReceived,
                fields.value("bytes_received", actual[index].bytesReceived));
    }
  }

  const SessionConfig m_config;
  Session m_session;
  uint64_t m_nowMs;
  uint64_t m_lastPing;
  std::deque<Bytes> m_messages;
  const Json* m_step = nullptr;

  const std::map<std::string, std::function<void()>> m_steps{
      {"attach", [this] { check(m_session.attachConnection(m_nowMs)); }},
      {"rx", [this] { rx(); }},
      {"lost", [this] { check(m_session.onConnectionLost()); }},
      {"advance", [this] { m_nowMs += step().at("ms").get<uint64_t>(); }},
      {"tick", [this] { check(m_session.onTick(m_nowMs)); }},
      {"open", [this] { open(); }},
      {"accept", [this] { check(m_session.acceptStream(step().at("stream"))); }},
      {"reject",
       [this] {
         const Bytes message = bytesField(step(), "message");
         check(m_session.rejectStream(
             step().at("stream"), REASON_NAMES.at(step().at("code")), message));
       }},
      {"write", [this] { write(); }},
      {"eof", [this] { check(m_session.sendEof(step().at("stream"))); }},
      {"reset",
       [this] {
         check(m_session.resetStream(step().at("stream"),
                                     REASON_NAMES.at(step().at("code"))));
       }},
      {"close",
       [this] {
         check(m_session.close(REASON_NAMES.at(step().at("code")), m_nowMs));
       }},
      {"drain",
       [this] {
         takeFrames(nullptr);
         takeEvents(nullptr);
       }},
      {"drain_frames", [this] { takeFrames(nullptr); }},
      {"drain_events", [this] { takeEvents(nullptr); }},
      {"expect_frames", [this] { takeFrames(&step().at("frames")); }},
      {"expect_events", [this] { takeEvents(&step().at("events")); }},
      {"expect_state",
       [this] {
         EXPECT_EQ(m_session.state(), STATE_NAMES.at(step().at("state")));
       }},
      {"expect_deadline", [this] { expectDeadline(); }},
      {"expect_rtt", [this] { expectRtt(); }},
      {"expect_streams", [this] { expectStreams(); }},
  };
};

TEST(ConformanceTest, SyncScenarios) {
  const Json manifest = readJson("manifest.json");
  const Json setups = readJson("sync/setups.json");
  size_t scenarios = 0U;
  for (const std::string path : manifest.at("sync")) {
    const Json file = readJson(path);
    for (const Json& scenario : file.at("scenarios")) {
      SCOPED_TRACE(scenario.at("name").get<std::string>());
      Harness harness(scenario);
      for (const Json& step :
           setups.at(scenario.value("setup", std::string("disconnected")))) {
        harness.run(step);
      }
      for (const Json& step : scenario.at("steps")) {
        harness.run(step);
      }
      harness.expectNothingLeft();
      ++scenarios;
    }
  }
  EXPECT_GE(scenarios, 200U);
}

TEST(ConformanceTest, ManifestProtocolVersionMatches) {
  EXPECT_EQ(readJson("manifest.json").at("protocol_version").get<uint16_t>(),
            PROTOCOL_VERSION);
}

// A runner loads files from the manifest, so a file left out of it never runs.
TEST(ConformanceTest, ManifestListsEveryFile) {
  const Json manifest = readJson("manifest.json");
  std::set<std::string> listed{"sync/setups.json"};
  for (const char* const layer : {"vectors", "sync"}) {
    for (const std::string path : manifest.at(layer)) {
      listed.insert(path);
    }
  }
  for (const char* const layer : {"vectors", "sync"}) {
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(CONFORMANCE_DIR + "/" + layer)) {
      const std::string path =
          std::filesystem::relative(entry.path(), CONFORMANCE_DIR).string();
      EXPECT_TRUE(listed.contains(path)) << path;
    }
  }
}

}  // namespace
}  // namespace packetmux
