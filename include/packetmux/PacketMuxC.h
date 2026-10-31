/**
 * The C ABI of PacketMux. Every function forwards to the C++ API in Session.h
 * and Codec.h, so the rules there apply here too, including the buffer
 * lifetimes.
 *
 * This header is valid C, so it cannot use the packetmux namespace. Its symbols
 * are prefixed packetmux_ instead.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#define PACKETMUX_NOEXCEPT noexcept
#else
#define PACKETMUX_NOEXCEPT
#endif

/* Windows export for shared builds. */
#if defined(_WIN32) && defined(PACKETMUX_BUILDING_DLL)
#define PACKETMUX_API __declspec(dllexport)
#elif defined(_WIN32) && defined(PACKETMUX_USING_DLL)
#define PACKETMUX_API __declspec(dllimport)
#else
#define PACKETMUX_API
#endif

/* Enums are passed as uint8_t, because a C enum's size is not fixed. */
typedef uint8_t
    packetmux_status_t; /* PACKETMUX_STATUS_*: same values as SessionStatus */
typedef uint8_t packetmux_codec_status_t;  /* PACKETMUX_CODEC_STATUS_* */
typedef uint8_t packetmux_role_t;          /* PACKETMUX_ROLE_* */
typedef uint8_t packetmux_reason_t;        /* PACKETMUX_REASON_* */
typedef uint8_t packetmux_frame_type_t;    /* PACKETMUX_FRAME_TYPE_* */
typedef uint8_t packetmux_event_type_t;    /* PACKETMUX_EVENT_TYPE_* */
typedef uint8_t packetmux_session_state_t; /* PACKETMUX_SESSION_STATE_* */
typedef uint8_t packetmux_stream_state_t;  /* PACKETMUX_STREAM_STATE_* */

enum {
  PACKETMUX_STATUS_OK = 0,
  PACKETMUX_STATUS_CLOSED,
  PACKETMUX_STATUS_NOT_CONNECTED,
  PACKETMUX_STATUS_ALREADY_CONNECTED,
  PACKETMUX_STATUS_EVENTS_PENDING,
  PACKETMUX_STATUS_UNKNOWN_STREAM,
  PACKETMUX_STATUS_INVALID_STATE,
  PACKETMUX_STATUS_LIMIT_EXCEEDED,
  PACKETMUX_STATUS_IDS_EXHAUSTED,
  PACKETMUX_STATUS_INVALID_ARGUMENT,
  PACKETMUX_STATUS_MAX
};

enum {
  PACKETMUX_CODEC_STATUS_OK = 0,
  PACKETMUX_CODEC_STATUS_TRUNCATED,
  PACKETMUX_CODEC_STATUS_UNKNOWN_TYPE,
  PACKETMUX_CODEC_STATUS_BAD_STREAM_ID,
  PACKETMUX_CODEC_STATUS_PAYLOAD_TOO_LARGE,
  PACKETMUX_CODEC_STATUS_BAD_PAYLOAD_SIZE,
  PACKETMUX_CODEC_STATUS_UNSUPPORTED_VERSION,
  PACKETMUX_CODEC_STATUS_BAD_IDENTIFIER,
  PACKETMUX_CODEC_STATUS_INVALID_ARGUMENT,
  PACKETMUX_CODEC_STATUS_BAD_FLAG,
  PACKETMUX_CODEC_STATUS_MAX
};

enum { PACKETMUX_ROLE_CLIENT = 0, PACKETMUX_ROLE_SERVER, PACKETMUX_ROLE_MAX };

enum {
  PACKETMUX_REASON_OK = 0,
  PACKETMUX_REASON_PEER_CLOSED,
  PACKETMUX_REASON_CONNECT_FAILED,
  PACKETMUX_REASON_POLICY_DENIED,
  PACKETMUX_REASON_LIMIT_EXCEEDED,
  PACKETMUX_REASON_TRANSPORT_LOST,
  PACKETMUX_REASON_KILLED,
  PACKETMUX_REASON_AUTH_REVOKED,
  PACKETMUX_REASON_TIMEOUT,
  PACKETMUX_REASON_PROTOCOL_ERROR,
  PACKETMUX_REASON_UNSUPPORTED_KIND,
  PACKETMUX_REASON_SESSION_NOT_FOUND,
  PACKETMUX_REASON_MAX
};

enum {
  PACKETMUX_FRAME_TYPE_UNKNOWN = 0,
  PACKETMUX_FRAME_TYPE_HELLO,
  PACKETMUX_FRAME_TYPE_OPEN,
  PACKETMUX_FRAME_TYPE_OPEN_OK,
  PACKETMUX_FRAME_TYPE_OPEN_ERR,
  PACKETMUX_FRAME_TYPE_DATA,
  PACKETMUX_FRAME_TYPE_EOF,
  PACKETMUX_FRAME_TYPE_RST,
  PACKETMUX_FRAME_TYPE_PING,
  PACKETMUX_FRAME_TYPE_PONG,
  PACKETMUX_FRAME_TYPE_GOAWAY,
  PACKETMUX_FRAME_TYPE_MAX
};

enum {
  PACKETMUX_EVENT_TYPE_UNKNOWN = 0,
  PACKETMUX_EVENT_TYPE_CONNECTED,
  PACKETMUX_EVENT_TYPE_STREAM_OPENED,
  PACKETMUX_EVENT_TYPE_STREAM_ACCEPTED,
  PACKETMUX_EVENT_TYPE_DATA,
  PACKETMUX_EVENT_TYPE_EOF,
  PACKETMUX_EVENT_TYPE_WRITABLE,
  PACKETMUX_EVENT_TYPE_STREAM_CLOSED,
  PACKETMUX_EVENT_TYPE_DISCONNECTED,
  PACKETMUX_EVENT_TYPE_SESSION_CLOSED,
  PACKETMUX_EVENT_TYPE_MAX
};

enum {
  PACKETMUX_SESSION_STATE_DISCONNECTED = 0,
  PACKETMUX_SESSION_STATE_HANDSHAKING,
  PACKETMUX_SESSION_STATE_ESTABLISHED,
  PACKETMUX_SESSION_STATE_DRAINING,
  PACKETMUX_SESSION_STATE_CLOSING,
  PACKETMUX_SESSION_STATE_CLOSED,
  PACKETMUX_SESSION_STATE_MAX
};

enum {
  PACKETMUX_STREAM_STATE_SENDER_OPENING = 0,
  PACKETMUX_STREAM_STATE_SENDER_OPENING_EOF_SENT,
  PACKETMUX_STREAM_STATE_RECEIVER_OPENING,
  PACKETMUX_STREAM_STATE_RECEIVER_OPENING_EOF_RECEIVED,
  PACKETMUX_STREAM_STATE_OPEN,
  PACKETMUX_STREAM_STATE_HALF_CLOSED_LOCAL,
  PACKETMUX_STREAM_STATE_HALF_CLOSED_REMOTE,
  PACKETMUX_STREAM_STATE_CLOSED,
  PACKETMUX_STREAM_STATE_MAX
};

#define PACKETMUX_NO_DEADLINE UINT64_MAX

/* Borrowed bytes. Text is ASCII and NOT NUL-terminated. */
typedef struct {
  const uint8_t* data;
  size_t size;
} packetmux_bytes_t;

typedef struct {
  size_t max_data_payload;
  size_t max_control_payload;
} packetmux_limits_t;

typedef struct {
  packetmux_role_t role;
  packetmux_bytes_t session_id;
  packetmux_bytes_t capabilities;
  packetmux_limits_t limits;
  uint32_t max_streams;
  size_t max_queued_bytes;
  size_t max_queued_bytes_per_stream;
  size_t max_queued_control_frames;
  uint64_t hello_timeout_ms;
  uint64_t ping_interval_ms;
  uint64_t dead_after_ms;
  uint64_t drain_timeout_ms;
  size_t max_outstanding_pings;
} packetmux_config_t;

typedef struct {
  packetmux_event_type_t type;
  uint32_t stream_id;
  packetmux_reason_t code;
  packetmux_codec_status_t codec_status;
  packetmux_bytes_t peer_session_id;
  packetmux_bytes_t capabilities;
  packetmux_bytes_t kind;
  packetmux_bytes_t params;
  packetmux_bytes_t data;
} packetmux_event_t;

typedef struct {
  uint32_t stream_id;
  packetmux_bytes_t kind;
  packetmux_role_t opener;
  packetmux_stream_state_t state;
  uint64_t bytes_sent;
  uint64_t bytes_received;
  uint64_t created_at_ms;
  uint64_t last_activity_ms;
} packetmux_stream_info_t;

typedef struct {
  packetmux_frame_type_t type;
  uint32_t stream_id;
  uint16_t version;
  bool resuming;
  packetmux_bytes_t session_id;
  packetmux_bytes_t capabilities;
  packetmux_bytes_t kind;
  packetmux_bytes_t params;
  packetmux_reason_t code;
  packetmux_bytes_t message;
  packetmux_bytes_t data;
  uint64_t nonce;
} packetmux_frame_t;

typedef struct packetmux_session packetmux_session_t; /* opaque */

/* Lifetime. The session copies the config, so its buffers can be freed after
 * create. */
PACKETMUX_API void packetmux_config_init(packetmux_config_t* o_config)
    PACKETMUX_NOEXCEPT;
PACKETMUX_API packetmux_session_t* packetmux_session_create(
    const packetmux_config_t* i_config) PACKETMUX_NOEXCEPT;
PACKETMUX_API void packetmux_session_destroy(packetmux_session_t* i_session)
    PACKETMUX_NOEXCEPT;

/* Connection */
PACKETMUX_API packetmux_status_t packetmux_session_attach_connection(
    packetmux_session_t* i_session, uint64_t i_now_ms) PACKETMUX_NOEXCEPT;
PACKETMUX_API packetmux_status_t packetmux_session_on_message(
    packetmux_session_t* i_session, const uint8_t* i_message, size_t i_size,
    uint64_t i_now_ms) PACKETMUX_NOEXCEPT;
PACKETMUX_API packetmux_status_t packetmux_session_on_connection_lost(
    packetmux_session_t* i_session) PACKETMUX_NOEXCEPT;
PACKETMUX_API packetmux_status_t packetmux_session_on_tick(
    packetmux_session_t* i_session, uint64_t i_now_ms) PACKETMUX_NOEXCEPT;
PACKETMUX_API uint64_t packetmux_session_next_deadline_ms(
    const packetmux_session_t* i_session) PACKETMUX_NOEXCEPT;
PACKETMUX_API packetmux_status_t packetmux_session_close(
    packetmux_session_t* i_session, packetmux_reason_t i_code,
    uint64_t i_now_ms) PACKETMUX_NOEXCEPT;

/* Streams */
PACKETMUX_API packetmux_status_t packetmux_session_open_stream(
    packetmux_session_t* i_session, const uint8_t* i_kind, size_t i_kind_size,
    const uint8_t* i_params, size_t i_params_size, uint64_t i_now_ms,
    uint32_t* o_stream_id) PACKETMUX_NOEXCEPT;
PACKETMUX_API packetmux_status_t packetmux_session_accept_stream(
    packetmux_session_t* i_session, uint32_t i_stream_id) PACKETMUX_NOEXCEPT;
PACKETMUX_API packetmux_status_t packetmux_session_reject_stream(
    packetmux_session_t* i_session, uint32_t i_stream_id,
    packetmux_reason_t i_code, const uint8_t* i_message,
    size_t i_size) PACKETMUX_NOEXCEPT;
PACKETMUX_API packetmux_status_t packetmux_session_write(
    packetmux_session_t* i_session, uint32_t i_stream_id,
    const uint8_t* i_bytes, size_t i_size, uint64_t i_now_ms,
    size_t* o_accepted) PACKETMUX_NOEXCEPT;
PACKETMUX_API packetmux_status_t packetmux_session_send_eof(
    packetmux_session_t* i_session, uint32_t i_stream_id) PACKETMUX_NOEXCEPT;
PACKETMUX_API packetmux_status_t packetmux_session_reset_stream(
    packetmux_session_t* i_session, uint32_t i_stream_id,
    packetmux_reason_t i_code) PACKETMUX_NOEXCEPT;

/* Output. Same buffer rules as Session.h. */
PACKETMUX_API bool packetmux_session_poll_transmit(
    packetmux_session_t* i_session,
    packetmux_bytes_t* o_message) PACKETMUX_NOEXCEPT;
PACKETMUX_API bool packetmux_session_poll_event(packetmux_session_t* i_session,
                                                packetmux_event_t* o_event)
    PACKETMUX_NOEXCEPT;

/* Inspection. kind stays valid until the next packetmux_session_list_streams.
 */
PACKETMUX_API packetmux_session_state_t packetmux_session_state(
    const packetmux_session_t* i_session) PACKETMUX_NOEXCEPT;
PACKETMUX_API bool packetmux_session_get_rtt_ms(
    const packetmux_session_t* i_session,
    uint64_t* o_rtt_ms) PACKETMUX_NOEXCEPT;
PACKETMUX_API size_t packetmux_session_list_streams(
    packetmux_session_t* i_session) PACKETMUX_NOEXCEPT;
PACKETMUX_API void packetmux_session_stream_at(
    const packetmux_session_t* i_session, size_t i_index,
    packetmux_stream_info_t* o_info) PACKETMUX_NOEXCEPT;

/* Codec. encode writes nothing and returns INVALID_ARGUMENT if the buffer is
 * too small. */
PACKETMUX_API packetmux_codec_status_t packetmux_frame_decode(
    const uint8_t* i_message, size_t i_size, const packetmux_limits_t* i_limits,
    packetmux_frame_t* o_frame) PACKETMUX_NOEXCEPT;
PACKETMUX_API packetmux_codec_status_t packetmux_frame_encode(
    const packetmux_frame_t* i_frame, const packetmux_limits_t* i_limits,
    uint8_t* o_buffer, size_t i_capacity, size_t* o_size) PACKETMUX_NOEXCEPT;

#ifdef __cplusplus
}
#endif
