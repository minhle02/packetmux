/**
 * Acceptance test for the C ABI, compiled as C. It drives two sessions through
 * the whole lifecycle with nothing but packetmux_* calls. Edge cases are
 * covered by the C++ tests, below this layer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "packetmux/PacketMuxC.h"

#define CHECK(i_condition)                                       \
  do {                                                           \
    if (!(i_condition)) {                                        \
      fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, \
              #i_condition);                                     \
      exit(EXIT_FAILURE);                                        \
    }                                                            \
  } while (0)

#define MAX_EVENTS 16
#define MAX_TEXT 64
#define NOW_MS 1000U

typedef struct {
  packetmux_event_type_t type;
  uint32_t stream_id;
  packetmux_reason_t code;
  uint8_t text[MAX_TEXT]; /* data, kind or peer session ID */
  size_t text_size;
  uint8_t params[MAX_TEXT];
  size_t params_size;
} seen_event_t;

/* One side of the connection: its session and the events it has seen. */
typedef struct {
  packetmux_session_t* session;
  seen_event_t events[MAX_EVENTS];
  size_t event_count;
} peer_t;

static const uint8_t* text_of(const char* const i_text) {
  return (const uint8_t*)i_text;
}

static int bytes_equal(const uint8_t* const i_bytes, const size_t i_size,
                       const char* const i_text) {
  return i_size == strlen(i_text) && memcmp(i_bytes, i_text, i_size) == 0;
}

static void copy_bytes(const packetmux_bytes_t i_bytes, uint8_t* const o_out,
                       size_t* const o_size) {
  CHECK(i_bytes.size <= MAX_TEXT);
  if (i_bytes.size > 0U) {
    memcpy(o_out, i_bytes.data, i_bytes.size);
  }
  *o_size = i_bytes.size;
}

static packetmux_session_t* create_session(const packetmux_role_t i_role,
                                           const char* const i_session_id) {
  packetmux_config_t config;
  packetmux_config_init(&config);
  config.role = i_role;
  config.session_id.data = text_of(i_session_id);
  config.session_id.size = strlen(i_session_id);
  return packetmux_session_create(&config);
}

static void drain_events(peer_t* const o_peer) {
  packetmux_event_t event;
  while (packetmux_session_poll_event(o_peer->session, &event)) {
    seen_event_t* seen;
    CHECK(o_peer->event_count < MAX_EVENTS);
    seen = &o_peer->events[o_peer->event_count];
    ++o_peer->event_count;
    memset(seen, 0, sizeof(*seen));
    seen->type = event.type;
    seen->stream_id = event.stream_id;
    seen->code = event.code;
    /* Copied now: event views only live as long as the message. */
    if (event.type == PACKETMUX_EVENT_TYPE_DATA) {
      copy_bytes(event.data, seen->text, &seen->text_size);
    } else if (event.type == PACKETMUX_EVENT_TYPE_STREAM_OPENED) {
      copy_bytes(event.kind, seen->text, &seen->text_size);
      copy_bytes(event.params, seen->params, &seen->params_size);
    } else if (event.type == PACKETMUX_EVENT_TYPE_CONNECTED) {
      copy_bytes(event.peer_session_id, seen->text, &seen->text_size);
    }
  }
}

/* Moves one side's queued frames to the other side. */
static void transfer(peer_t* const o_from, peer_t* const o_to) {
  packetmux_bytes_t message;
  while (packetmux_session_poll_transmit(o_from->session, &message)) {
    CHECK(packetmux_session_on_message(o_to->session, message.data,
                                       message.size,
                                       NOW_MS) == PACKETMUX_STATUS_OK);
    drain_events(o_to);
  }
  drain_events(o_from);
}

static void pump(peer_t* const o_client, peer_t* const o_server) {
  transfer(o_client, o_server);
  transfer(o_server, o_client);
  transfer(o_client, o_server);
}

static const seen_event_t* expect_event(const peer_t* const i_peer,
                                        const size_t i_index,
                                        const packetmux_event_type_t i_type) {
  CHECK(i_index < i_peer->event_count);
  CHECK(i_peer->events[i_index].type == i_type);
  return &i_peer->events[i_index];
}

static void reset_events(peer_t* const o_client, peer_t* const o_server) {
  o_client->event_count = 0U;
  o_server->event_count = 0U;
}

static void test_config_defaults(void) {
  packetmux_config_t config;
  packetmux_config_init(&config);
  CHECK(config.role == PACKETMUX_ROLE_CLIENT);
  CHECK(config.session_id.size == 0U);
  CHECK(config.limits.max_data_payload == 16000U);
  CHECK(config.limits.max_control_payload == 32768U);
  CHECK(config.max_streams == 64U);
  CHECK(config.ping_interval_ms == 15000U);
  CHECK(config.drain_timeout_ms == 1000U);
}

static void connect_pair(peer_t* const o_client, peer_t* const o_server) {
  CHECK(packetmux_session_attach_connection(o_client->session, NOW_MS) ==
        PACKETMUX_STATUS_OK);
  CHECK(packetmux_session_attach_connection(o_server->session, NOW_MS) ==
        PACKETMUX_STATUS_OK);
  CHECK(packetmux_session_state(o_client->session) ==
        PACKETMUX_SESSION_STATE_HANDSHAKING);
  pump(o_client, o_server);
  CHECK(packetmux_session_state(o_client->session) ==
        PACKETMUX_SESSION_STATE_ESTABLISHED);
  CHECK(packetmux_session_state(o_server->session) ==
        PACKETMUX_SESSION_STATE_ESTABLISHED);
}

static void test_handshake(peer_t* const o_client, peer_t* const o_server) {
  const seen_event_t* event;
  connect_pair(o_client, o_server);
  event = expect_event(o_client, 0U, PACKETMUX_EVENT_TYPE_CONNECTED);
  CHECK(bytes_equal(event->text, event->text_size, "server-1"));
  event = expect_event(o_server, 0U, PACKETMUX_EVENT_TYPE_CONNECTED);
  CHECK(bytes_equal(event->text, event->text_size, "client-1"));
  reset_events(o_client, o_server);
}

static void test_stream_request_and_reply(peer_t* const o_client,
                                          peer_t* const o_server) {
  const char* const kind = "tcp-forward";
  const char* const params = "{}";
  const char* const request = "request";
  const char* const reply = "the reply";
  const seen_event_t* event;
  uint32_t stream_id = 0U;
  size_t accepted = 0U;

  CHECK(packetmux_session_open_stream(
            o_client->session, text_of(kind), strlen(kind), text_of(params),
            strlen(params), NOW_MS, &stream_id) == PACKETMUX_STATUS_OK);
  CHECK(stream_id == 1U);
  CHECK(packetmux_session_write(o_client->session, stream_id, text_of(request),
                                strlen(request), NOW_MS,
                                &accepted) == PACKETMUX_STATUS_OK);
  CHECK(accepted == strlen(request));
  CHECK(packetmux_session_send_eof(o_client->session, stream_id) ==
        PACKETMUX_STATUS_OK);
  transfer(o_client, o_server);

  event = expect_event(o_server, 0U, PACKETMUX_EVENT_TYPE_STREAM_OPENED);
  CHECK(event->stream_id == stream_id);
  CHECK(bytes_equal(event->text, event->text_size, kind));
  CHECK(bytes_equal(event->params, event->params_size, params));
  event = expect_event(o_server, 1U, PACKETMUX_EVENT_TYPE_DATA);
  CHECK(bytes_equal(event->text, event->text_size, request));
  expect_event(o_server, 2U, PACKETMUX_EVENT_TYPE_EOF);

  CHECK(packetmux_session_accept_stream(o_server->session, stream_id) ==
        PACKETMUX_STATUS_OK);
  CHECK(packetmux_session_write(o_server->session, stream_id, text_of(reply),
                                strlen(reply), NOW_MS,
                                &accepted) == PACKETMUX_STATUS_OK);
  CHECK(packetmux_session_send_eof(o_server->session, stream_id) ==
        PACKETMUX_STATUS_OK);
  transfer(o_server, o_client);
  event = expect_event(o_server, 3U, PACKETMUX_EVENT_TYPE_STREAM_CLOSED);
  CHECK(event->code == PACKETMUX_REASON_OK);

  expect_event(o_client, 0U, PACKETMUX_EVENT_TYPE_STREAM_ACCEPTED);
  event = expect_event(o_client, 1U, PACKETMUX_EVENT_TYPE_DATA);
  CHECK(bytes_equal(event->text, event->text_size, reply));
  expect_event(o_client, 2U, PACKETMUX_EVENT_TYPE_EOF);
  event = expect_event(o_client, 3U, PACKETMUX_EVENT_TYPE_STREAM_CLOSED);
  CHECK(event->code == PACKETMUX_REASON_OK);
  CHECK(packetmux_session_list_streams(o_client->session) == 0U);
  reset_events(o_client, o_server);
}

static void test_reject_and_reset(peer_t* const o_client,
                                  peer_t* const o_server) {
  const char* const kind = "kind";
  const seen_event_t* event;
  packetmux_stream_info_t info;
  uint32_t rejected = 0U;
  uint32_t reset = 0U;
  size_t accepted = 0U;

  CHECK(packetmux_session_open_stream(o_client->session, text_of(kind),
                                      strlen(kind), NULL, 0U, NOW_MS,
                                      &rejected) == PACKETMUX_STATUS_OK);
  transfer(o_client, o_server);
  CHECK(packetmux_session_reject_stream(
            o_server->session, rejected, PACKETMUX_REASON_POLICY_DENIED,
            text_of("no"), 2U) == PACKETMUX_STATUS_OK);
  transfer(o_server, o_client);
  event = expect_event(o_client, 0U, PACKETMUX_EVENT_TYPE_STREAM_CLOSED);
  CHECK(event->stream_id == rejected);
  CHECK(event->code == PACKETMUX_REASON_POLICY_DENIED);
  reset_events(o_client, o_server);

  CHECK(packetmux_session_open_stream(o_client->session, text_of(kind),
                                      strlen(kind), NULL, 0U, NOW_MS,
                                      &reset) == PACKETMUX_STATUS_OK);
  CHECK(packetmux_session_write(o_client->session, reset, text_of("abc"), 3U,
                                NOW_MS, &accepted) == PACKETMUX_STATUS_OK);

  CHECK(packetmux_session_list_streams(o_client->session) == 1U);
  packetmux_session_stream_at(o_client->session, 0U, &info);
  CHECK(info.stream_id == reset);
  CHECK(bytes_equal(info.kind.data, info.kind.size, kind));
  CHECK(info.opener == PACKETMUX_ROLE_CLIENT);
  CHECK(info.state == PACKETMUX_STREAM_STATE_SENDER_OPENING);
  CHECK(info.bytes_sent == 3U);
  CHECK(info.created_at_ms == NOW_MS);

  /* Not KILLED: that is the C++ default, so it would not prove the code passes.
   */
  CHECK(packetmux_session_reset_stream(o_client->session, reset,
                                       PACKETMUX_REASON_TIMEOUT) ==
        PACKETMUX_STATUS_OK);
  pump(o_client, o_server);
  event = expect_event(o_client, 0U, PACKETMUX_EVENT_TYPE_STREAM_CLOSED);
  CHECK(event->code == PACKETMUX_REASON_TIMEOUT);
  event = expect_event(o_server, 1U, PACKETMUX_EVENT_TYPE_STREAM_CLOSED);
  CHECK(event->code == PACKETMUX_REASON_TIMEOUT);
  CHECK(packetmux_session_reject_stream(o_server->session, reset,
                                        PACKETMUX_REASON_KILLED, NULL,
                                        0U) == PACKETMUX_STATUS_UNKNOWN_STREAM);
  CHECK(packetmux_session_close(o_client->session,
                                PACKETMUX_REASON_TRANSPORT_LOST,
                                NOW_MS) == PACKETMUX_STATUS_INVALID_ARGUMENT);
  reset_events(o_client, o_server);
}

static void test_keepalive(peer_t* const o_client, peer_t* const o_server) {
  uint64_t rtt_ms = 0U;
  packetmux_bytes_t message;
  const uint64_t deadline =
      packetmux_session_next_deadline_ms(o_client->session);

  CHECK(deadline != PACKETMUX_NO_DEADLINE);
  CHECK(!packetmux_session_get_rtt_ms(o_client->session, &rtt_ms));
  CHECK(packetmux_session_on_tick(o_client->session, deadline) ==
        PACKETMUX_STATUS_OK);

  CHECK(packetmux_session_poll_transmit(o_client->session, &message));
  CHECK(message.size > 0U && message.data[0] == PACKETMUX_FRAME_TYPE_PING);
  CHECK(packetmux_session_on_message(o_server->session, message.data,
                                     message.size,
                                     deadline) == PACKETMUX_STATUS_OK);
  CHECK(packetmux_session_poll_transmit(o_server->session, &message));
  CHECK(message.data[0] == PACKETMUX_FRAME_TYPE_PONG);
  CHECK(packetmux_session_on_message(o_client->session, message.data,
                                     message.size,
                                     deadline + 25U) == PACKETMUX_STATUS_OK);
  CHECK(packetmux_session_get_rtt_ms(o_client->session, &rtt_ms));
  CHECK(rtt_ms == 25U);
}

static void test_reconnect(peer_t* const o_client, peer_t* const o_server) {
  const char* const kind = "kind";
  const seen_event_t* event;
  uint32_t stream_id = 0U;

  CHECK(packetmux_session_open_stream(o_client->session, text_of(kind),
                                      strlen(kind), NULL, 0U, NOW_MS,
                                      &stream_id) == PACKETMUX_STATUS_OK);
  CHECK(stream_id == 7U); /* IDs 1, 3 and 5 were used by the earlier tests */
  CHECK(packetmux_session_on_connection_lost(o_client->session) ==
        PACKETMUX_STATUS_OK);
  CHECK(packetmux_session_on_connection_lost(o_server->session) ==
        PACKETMUX_STATUS_OK);
  drain_events(o_client);
  drain_events(o_server);

  event = expect_event(o_client, 0U, PACKETMUX_EVENT_TYPE_STREAM_CLOSED);
  CHECK(event->code == PACKETMUX_REASON_TRANSPORT_LOST);
  event = expect_event(o_client, 1U, PACKETMUX_EVENT_TYPE_DISCONNECTED);
  CHECK(event->code == PACKETMUX_REASON_TRANSPORT_LOST);
  CHECK(packetmux_session_state(o_client->session) ==
        PACKETMUX_SESSION_STATE_DISCONNECTED);
  reset_events(o_client, o_server);

  connect_pair(o_client, o_server);
  CHECK(packetmux_session_open_stream(o_client->session, text_of(kind),
                                      strlen(kind), NULL, 0U, NOW_MS,
                                      &stream_id) == PACKETMUX_STATUS_OK);
  CHECK(stream_id == 9U); /* IDs continue across the reconnect */
  CHECK(packetmux_session_reset_stream(o_client->session, stream_id,
                                       PACKETMUX_REASON_KILLED) ==
        PACKETMUX_STATUS_OK);
  pump(o_client, o_server);
  reset_events(o_client, o_server);
}

static void test_close(peer_t* const o_client, peer_t* const o_server) {
  const seen_event_t* event;
  CHECK(packetmux_session_close(o_client->session,
                                PACKETMUX_REASON_AUTH_REVOKED,
                                NOW_MS) == PACKETMUX_STATUS_OK);
  CHECK(packetmux_session_state(o_client->session) ==
        PACKETMUX_SESSION_STATE_CLOSING);
  transfer(o_client, o_server);

  CHECK(packetmux_session_state(o_client->session) ==
        PACKETMUX_SESSION_STATE_CLOSED);
  CHECK(packetmux_session_state(o_server->session) ==
        PACKETMUX_SESSION_STATE_CLOSED);
  event = expect_event(o_client, 0U, PACKETMUX_EVENT_TYPE_SESSION_CLOSED);
  CHECK(event->code == PACKETMUX_REASON_AUTH_REVOKED);
  event = expect_event(o_server, 0U, PACKETMUX_EVENT_TYPE_SESSION_CLOSED);
  CHECK(event->code == PACKETMUX_REASON_AUTH_REVOKED);
  CHECK(packetmux_session_next_deadline_ms(o_client->session) ==
        PACKETMUX_NO_DEADLINE);
  CHECK(packetmux_session_attach_connection(o_client->session, NOW_MS) ==
        PACKETMUX_STATUS_CLOSED);
}

/* The server's first step: read HELLO to find which session a transport is for.
 */
static void test_frame_codec(void) {
  const packetmux_limits_t limits = {16000U, 32768U};
  const uint8_t hello[] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
                           0x02, 0x73, 0x31, 0x00, 0x00, 0x00, 0x00};
  const uint8_t bad[] = {0x00};
  packetmux_frame_t frame;
  uint8_t buffer[sizeof(hello)];
  size_t size = 0U;

  CHECK(packetmux_frame_decode(hello, sizeof(hello), &limits, &frame) ==
        PACKETMUX_CODEC_STATUS_OK);
  CHECK(frame.type == PACKETMUX_FRAME_TYPE_HELLO);
  CHECK(frame.version == 2U);
  CHECK(bytes_equal(frame.session_id.data, frame.session_id.size, "s1"));

  CHECK(packetmux_frame_encode(&frame, &limits, buffer, sizeof(buffer),
                               &size) == PACKETMUX_CODEC_STATUS_OK);
  CHECK(size == sizeof(hello) && memcmp(buffer, hello, size) == 0);
  CHECK(packetmux_frame_encode(&frame, &limits, buffer, sizeof(buffer) - 1U,
                               &size) ==
        PACKETMUX_CODEC_STATUS_INVALID_ARGUMENT);

  CHECK(packetmux_frame_decode(bad, sizeof(bad), &limits, &frame) ==
        PACKETMUX_CODEC_STATUS_TRUNCATED);
}

int main(void) {
  peer_t client;
  peer_t server;
  memset(&client, 0, sizeof(client));
  memset(&server, 0, sizeof(server));

  test_config_defaults();
  test_frame_codec();

  client.session = create_session(PACKETMUX_ROLE_CLIENT, "client-1");
  server.session = create_session(PACKETMUX_ROLE_SERVER, "server-1");
  CHECK(client.session != NULL && server.session != NULL);

  test_handshake(&client, &server);
  test_stream_request_and_reply(&client, &server);
  test_reject_and_reset(&client, &server);
  test_keepalive(&client, &server);
  test_reconnect(&client, &server);
  test_close(&client, &server);

  packetmux_session_destroy(client.session);
  packetmux_session_destroy(server.session);
  packetmux_session_destroy(NULL);

  puts("C API acceptance test passed");
  return EXIT_SUCCESS;
}
