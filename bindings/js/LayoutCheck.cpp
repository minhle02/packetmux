/**
 * Fails the WASM build if a C struct changes without bindings/js/layout.js. The
 * numbers here repeat that file, so the two must be edited together.
 */
#include <cstddef>

#include "packetmux/PacketMuxC.h"

static_assert(sizeof(void*) == 4U, "layout.js describes wasm32");

static_assert(sizeof(packetmux_bytes_t) == 8U);
static_assert(offsetof(packetmux_bytes_t, data) == 0U);
static_assert(offsetof(packetmux_bytes_t, size) == 4U);

static_assert(sizeof(packetmux_config_t) == 88U);
static_assert(offsetof(packetmux_config_t, role) == 0U);
static_assert(offsetof(packetmux_config_t, session_id) == 4U);
static_assert(offsetof(packetmux_config_t, capabilities) == 12U);
static_assert(offsetof(packetmux_config_t, limits) == 20U);
static_assert(offsetof(packetmux_config_t, max_streams) == 28U);
static_assert(offsetof(packetmux_config_t, max_queued_bytes) == 32U);
static_assert(offsetof(packetmux_config_t, max_queued_bytes_per_stream) == 36U);
static_assert(offsetof(packetmux_config_t, max_queued_control_frames) == 40U);
static_assert(offsetof(packetmux_config_t, hello_timeout_ms) == 48U);
static_assert(offsetof(packetmux_config_t, ping_interval_ms) == 56U);
static_assert(offsetof(packetmux_config_t, dead_after_ms) == 64U);
static_assert(offsetof(packetmux_config_t, drain_timeout_ms) == 72U);
static_assert(offsetof(packetmux_config_t, max_outstanding_pings) == 80U);

static_assert(sizeof(packetmux_limits_t) == 8U);
static_assert(offsetof(packetmux_limits_t, max_data_payload) == 0U);
static_assert(offsetof(packetmux_limits_t, max_control_payload) == 4U);

static_assert(sizeof(packetmux_event_t) == 52U);
static_assert(offsetof(packetmux_event_t, type) == 0U);
static_assert(offsetof(packetmux_event_t, stream_id) == 4U);
static_assert(offsetof(packetmux_event_t, code) == 8U);
static_assert(offsetof(packetmux_event_t, codec_status) == 9U);
static_assert(offsetof(packetmux_event_t, peer_session_id) == 12U);
static_assert(offsetof(packetmux_event_t, capabilities) == 20U);
static_assert(offsetof(packetmux_event_t, kind) == 28U);
static_assert(offsetof(packetmux_event_t, params) == 36U);
static_assert(offsetof(packetmux_event_t, data) == 44U);

static_assert(sizeof(packetmux_stream_info_t) == 48U);
static_assert(offsetof(packetmux_stream_info_t, stream_id) == 0U);
static_assert(offsetof(packetmux_stream_info_t, kind) == 4U);
static_assert(offsetof(packetmux_stream_info_t, opener) == 12U);
static_assert(offsetof(packetmux_stream_info_t, state) == 13U);
static_assert(offsetof(packetmux_stream_info_t, bytes_sent) == 16U);
static_assert(offsetof(packetmux_stream_info_t, bytes_received) == 24U);
static_assert(offsetof(packetmux_stream_info_t, created_at_ms) == 32U);
static_assert(offsetof(packetmux_stream_info_t, last_activity_ms) == 40U);

static_assert(sizeof(packetmux_frame_t) == 72U);
static_assert(offsetof(packetmux_frame_t, type) == 0U);
static_assert(offsetof(packetmux_frame_t, stream_id) == 4U);
static_assert(offsetof(packetmux_frame_t, version) == 8U);
static_assert(offsetof(packetmux_frame_t, resuming) == 10U);
static_assert(offsetof(packetmux_frame_t, session_id) == 12U);
static_assert(offsetof(packetmux_frame_t, capabilities) == 20U);
static_assert(offsetof(packetmux_frame_t, kind) == 28U);
static_assert(offsetof(packetmux_frame_t, params) == 36U);
static_assert(offsetof(packetmux_frame_t, code) == 44U);
static_assert(offsetof(packetmux_frame_t, message) == 48U);
static_assert(offsetof(packetmux_frame_t, data) == 56U);
static_assert(offsetof(packetmux_frame_t, nonce) == 64U);
