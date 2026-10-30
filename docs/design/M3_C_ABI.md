# M3 — C ABI: design

Status: implemented. Exit criterion met on macOS arm64 with Apple Clang 17 (§5).
Requirements: `docs/requirements/INTENT.md` (FR-API-4/5, NFR-MNT-2).

Every function forwards to the C++ API. No protocol logic lives in this layer.

New files: `include/packetmux/PacketMuxC.h`, `src/capi/PacketMuxC.cpp` and
`tests/CApiAcceptanceTest.c`.

## 1. API

```c
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
typedef uint8_t packetmux_status_t;  /* PACKETMUX_STATUS_*: same values as SessionStatus */
typedef uint8_t packetmux_codec_status_t;  /* PACKETMUX_CODEC_STATUS_* */
typedef uint8_t packetmux_role_t;  /* PACKETMUX_ROLE_* */
typedef uint8_t packetmux_reason_t;  /* PACKETMUX_REASON_* */
typedef uint8_t packetmux_frame_type_t;  /* PACKETMUX_FRAME_TYPE_* */
typedef uint8_t packetmux_event_type_t;  /* PACKETMUX_EVENT_TYPE_* */
typedef uint8_t packetmux_session_state_t;  /* PACKETMUX_SESSION_STATE_* */
typedef uint8_t packetmux_stream_state_t;  /* PACKETMUX_STREAM_STATE_* */

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
  packetmux_bytes_t session_id;
  packetmux_bytes_t capabilities;
  packetmux_bytes_t kind;
  packetmux_bytes_t params;
  packetmux_reason_t code;
  packetmux_bytes_t message;
  packetmux_bytes_t data;
  uint64_t nonce;
} packetmux_frame_t;

typedef struct packetmux_session packetmux_session_t;  /* opaque */

/* Lifetime. The session copies the config, so its buffers can be freed after create. */
PACKETMUX_API void packetmux_config_init(packetmux_config_t* o_config) PACKETMUX_NOEXCEPT;
PACKETMUX_API packetmux_session_t* packetmux_session_create(const packetmux_config_t* i_config) PACKETMUX_NOEXCEPT;
PACKETMUX_API void packetmux_session_destroy(packetmux_session_t* i_session) PACKETMUX_NOEXCEPT;

/* Connection */
PACKETMUX_API packetmux_status_t packetmux_session_attach_connection(packetmux_session_t* i_session, uint64_t i_now_ms) PACKETMUX_NOEXCEPT;
PACKETMUX_API packetmux_status_t packetmux_session_on_message(packetmux_session_t* i_session, const uint8_t* i_message, size_t i_size, uint64_t i_now_ms) PACKETMUX_NOEXCEPT;
PACKETMUX_API packetmux_status_t packetmux_session_on_connection_lost(packetmux_session_t* i_session) PACKETMUX_NOEXCEPT;
PACKETMUX_API packetmux_status_t packetmux_session_on_tick(packetmux_session_t* i_session, uint64_t i_now_ms) PACKETMUX_NOEXCEPT;
PACKETMUX_API uint64_t packetmux_session_next_deadline_ms(const packetmux_session_t* i_session) PACKETMUX_NOEXCEPT;
PACKETMUX_API packetmux_status_t packetmux_session_close(packetmux_session_t* i_session, packetmux_reason_t i_code, uint64_t i_now_ms) PACKETMUX_NOEXCEPT;

/* Streams */
PACKETMUX_API packetmux_status_t packetmux_session_open_stream(packetmux_session_t* i_session, const uint8_t* i_kind, size_t i_kind_size, const uint8_t* i_params, size_t i_params_size, uint64_t i_now_ms, uint32_t* o_stream_id) PACKETMUX_NOEXCEPT;
PACKETMUX_API packetmux_status_t packetmux_session_accept_stream(packetmux_session_t* i_session, uint32_t i_stream_id) PACKETMUX_NOEXCEPT;
PACKETMUX_API packetmux_status_t packetmux_session_reject_stream(packetmux_session_t* i_session, uint32_t i_stream_id, packetmux_reason_t i_code, const uint8_t* i_message, size_t i_size) PACKETMUX_NOEXCEPT;
PACKETMUX_API packetmux_status_t packetmux_session_write(packetmux_session_t* i_session, uint32_t i_stream_id, const uint8_t* i_bytes, size_t i_size, uint64_t i_now_ms, size_t* o_accepted) PACKETMUX_NOEXCEPT;
PACKETMUX_API packetmux_status_t packetmux_session_send_eof(packetmux_session_t* i_session, uint32_t i_stream_id) PACKETMUX_NOEXCEPT;
PACKETMUX_API packetmux_status_t packetmux_session_reset_stream(packetmux_session_t* i_session, uint32_t i_stream_id, packetmux_reason_t i_code) PACKETMUX_NOEXCEPT;

/* Output. Same buffer rules as Session.h. */
PACKETMUX_API bool packetmux_session_poll_transmit(packetmux_session_t* i_session, packetmux_bytes_t* o_message) PACKETMUX_NOEXCEPT;
PACKETMUX_API bool packetmux_session_poll_event(packetmux_session_t* i_session, packetmux_event_t* o_event) PACKETMUX_NOEXCEPT;

/* Inspection. kind stays valid until the next packetmux_session_list_streams. */
PACKETMUX_API packetmux_session_state_t packetmux_session_state(const packetmux_session_t* i_session) PACKETMUX_NOEXCEPT;
PACKETMUX_API bool packetmux_session_get_rtt_ms(const packetmux_session_t* i_session, uint64_t* o_rtt_ms) PACKETMUX_NOEXCEPT;
PACKETMUX_API size_t packetmux_session_list_streams(packetmux_session_t* i_session) PACKETMUX_NOEXCEPT;
PACKETMUX_API void packetmux_session_stream_at(const packetmux_session_t* i_session, size_t i_index, packetmux_stream_info_t* o_info) PACKETMUX_NOEXCEPT;

/* Codec. encode writes nothing and returns INVALID_ARGUMENT if the buffer is too small. */
PACKETMUX_API packetmux_codec_status_t packetmux_frame_decode(const uint8_t* i_message, size_t i_size, const packetmux_limits_t* i_limits, packetmux_frame_t* o_frame) PACKETMUX_NOEXCEPT;
PACKETMUX_API packetmux_codec_status_t packetmux_frame_encode(const packetmux_frame_t* i_frame, const packetmux_limits_t* i_limits, uint8_t* o_buffer, size_t i_capacity, size_t* o_size) PACKETMUX_NOEXCEPT;

#ifdef __cplusplus
}
#endif
```

The enum constants are not listed here. There is one `PACKETMUX_*` constant for each value in
`Frames.h` and `SessionTypes.h`.

Pointers that are required must not be `NULL`; this is not checked. A `NULL` data pointer with size
0 is a valid empty buffer. A bad enum value is safe: `validateConfig` and the sendable-code check
already refuse it.

## 2. Wiring

```cpp
// The opaque handle. The vector is the one piece of state this layer needs.
struct packetmux_session {
  packetmux::Session session;
  std::vector<packetmux::StreamInfo> listed;
};

// Each C constant is tied to its C++ value at compile time.
static_assert(PACKETMUX_STATUS_OK == SESSION_STATUS_OK);
...one per constant...

packetmux_config_init(o_config):
    *o_config = toC(SessionConfig{})          // one source of defaults

packetmux_session_create(i_config):
    return new packetmux_session{Session(toCpp(*i_config)), {}}

packetmux_session_destroy(i_session):
    delete i_session                          // NULL is fine

// Almost every other function is this shape:
packetmux_session_write(i_session, i_stream_id, i_bytes, i_size, i_now_ms, o_accepted):
    return i_session->session.write(i_stream_id, span(i_bytes, i_size), i_now_ms, o_accepted)

packetmux_session_poll_event(i_session, o_event):
    Event event
    if !i_session->session.pollEvent(&event): return false
    *o_event = toC(event)                     // views are copied, bytes are not
    return true

packetmux_session_poll_transmit(i_session, o_message):
    span message
    if !i_session->session.pollTransmit(&message): return false
    *o_message = {message.data(), message.size()}
    return true

packetmux_session_list_streams(i_session):
    i_session->session.listStreams(&i_session->listed)
    return i_session->listed.size()

packetmux_session_stream_at(i_session, i_index, o_info):
    *o_info = toC(i_session->listed[i_index])  // kind points into listed

packetmux_frame_decode(i_message, i_size, i_limits, o_frame):
    FrameView frame
    status = Codec::decode(span(i_message, i_size), toCpp(*i_limits), &frame)
    *o_frame = toC(frame)
    return status

packetmux_frame_encode(i_frame, i_limits, o_buffer, i_capacity, o_size):
    vector message
    status = Codec::encode(toCpp(*i_frame), toCpp(*i_limits), &message)
    if status != OK: return status
    if message.size() > i_capacity: return INVALID_ARGUMENT
    copy message to o_buffer, *o_size = message.size()
    return OK
```

`toC` and `toCpp` are file-local helpers that copy fields one by one. `packetmux_bytes_t` converts to
and from `std::span` and `std::string_view` without copying bytes.

## 3. Where it is not 1:1

| What | Why |
|---|---|
| Stream listing is two calls | `listStreams` returns a vector of objects, which C cannot take. The handle keeps a snapshot, and the host reads it by index |
| `packetmux_frame_decode` and `_encode` | The server needs decode to read the first `HELLO` (M2 §2.2). M4's golden vectors need both |
| `noexcept` on every function | The library is built without exceptions, but a build with them on could throw `bad_alloc`, which must not cross into C. `noexcept` turns that into a clean stop |

## 4. Verification

`tests/CApiAcceptanceTest.c` is compiled as C and uses only `packetmux_*` calls. It shows that the
ABI calls the C++ API correctly and that the lifecycle can be driven through it. Edge cases and
performance stay with the M2 tests, which already cover them below this layer.

## 5. Verification results (2026-10-30)

| Check | Result |
|---|---|
| Acceptance test: handshake, request and reply, reject, reset, keepalive, reconnect, close, frame codec | Passes as C99, in Debug, Release and UBSan builds |
| Every function in the header is called by the test | 23 of 23 |
| Shared library build | Exports all 23 functions; the test passes against it |
| Existing tests, plus the new one | 181 of 181, in Debug and Release |
| Deliberate wiring bugs | 8 of 8 caught, after fixing one gap: the reset test used `KILLED`, which is also the C++ default |

Not done: the Windows export macro is unbuilt, like every other non-macOS target.
