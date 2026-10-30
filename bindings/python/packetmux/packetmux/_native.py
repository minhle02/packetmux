"""ctypes declarations for include/packetmux/PacketMuxC.h."""

import ctypes
import os
import sys
from ctypes import POINTER, Structure, c_bool, c_char_p, c_size_t, c_uint8, c_uint16, c_uint32, c_uint64, c_void_p
from pathlib import Path

_LIBRARY_NAMES = {"darwin": "libpacketmux.dylib", "win32": "packetmux.dll"}


class Bytes(Structure):
    _fields_ = [("data", c_void_p), ("size", c_size_t)]


class Limits(Structure):
    _fields_ = [("max_data_payload", c_size_t), ("max_control_payload", c_size_t)]


class Config(Structure):
    _fields_ = [
        ("role", c_uint8),
        ("session_id", Bytes),
        ("capabilities", Bytes),
        ("limits", Limits),
        ("max_streams", c_uint32),
        ("max_queued_bytes", c_size_t),
        ("max_queued_bytes_per_stream", c_size_t),
        ("max_queued_control_frames", c_size_t),
        ("hello_timeout_ms", c_uint64),
        ("ping_interval_ms", c_uint64),
        ("dead_after_ms", c_uint64),
        ("drain_timeout_ms", c_uint64),
        ("max_outstanding_pings", c_size_t),
    ]


class Event(Structure):
    _fields_ = [
        ("type", c_uint8),
        ("stream_id", c_uint32),
        ("code", c_uint8),
        ("codec_status", c_uint8),
        ("peer_session_id", Bytes),
        ("capabilities", Bytes),
        ("kind", Bytes),
        ("params", Bytes),
        ("data", Bytes),
    ]


class StreamInfo(Structure):
    _fields_ = [
        ("stream_id", c_uint32),
        ("kind", Bytes),
        ("opener", c_uint8),
        ("state", c_uint8),
        ("bytes_sent", c_uint64),
        ("bytes_received", c_uint64),
        ("created_at_ms", c_uint64),
        ("last_activity_ms", c_uint64),
    ]


class Frame(Structure):
    _fields_ = [
        ("type", c_uint8),
        ("stream_id", c_uint32),
        ("version", c_uint16),
        ("session_id", Bytes),
        ("capabilities", Bytes),
        ("kind", Bytes),
        ("params", Bytes),
        ("code", c_uint8),
        ("message", Bytes),
        ("data", Bytes),
        ("nonce", c_uint64),
    ]


def _find_library() -> str:
    configured = os.environ.get("PACKETMUX_LIBRARY")
    if configured:
        return configured
    name = _LIBRARY_NAMES.get(sys.platform, "libpacketmux.so")
    bundled = Path(__file__).parent / name
    if bundled.exists():
        return str(bundled)
    raise OSError(
        f"{name} not found: build with -DBUILD_SHARED_LIBS=ON and set PACKETMUX_LIBRARY"
    )


def _declare(library: ctypes.CDLL) -> None:
    session = c_void_p
    data = c_char_p
    status = c_uint8
    signatures = {
        "packetmux_config_init": (None, [POINTER(Config)]),
        "packetmux_session_create": (session, [POINTER(Config)]),
        "packetmux_session_destroy": (None, [session]),
        "packetmux_session_attach_connection": (status, [session, c_uint64]),
        "packetmux_session_on_message": (status, [session, data, c_size_t, c_uint64]),
        "packetmux_session_on_connection_lost": (status, [session]),
        "packetmux_session_on_tick": (status, [session, c_uint64]),
        "packetmux_session_next_deadline_ms": (c_uint64, [session]),
        "packetmux_session_close": (status, [session, c_uint8, c_uint64]),
        "packetmux_session_open_stream": (
            status,
            [session, data, c_size_t, data, c_size_t, c_uint64, POINTER(c_uint32)],
        ),
        "packetmux_session_accept_stream": (status, [session, c_uint32]),
        "packetmux_session_reject_stream": (status, [session, c_uint32, c_uint8, data, c_size_t]),
        "packetmux_session_write": (
            status,
            [session, c_uint32, data, c_size_t, c_uint64, POINTER(c_size_t)],
        ),
        "packetmux_session_send_eof": (status, [session, c_uint32]),
        "packetmux_session_reset_stream": (status, [session, c_uint32, c_uint8]),
        "packetmux_session_poll_transmit": (c_bool, [session, POINTER(Bytes)]),
        "packetmux_session_poll_event": (c_bool, [session, POINTER(Event)]),
        "packetmux_session_state": (c_uint8, [session]),
        "packetmux_session_get_rtt_ms": (c_bool, [session, POINTER(c_uint64)]),
        "packetmux_session_list_streams": (c_size_t, [session]),
        "packetmux_session_stream_at": (None, [session, c_size_t, POINTER(StreamInfo)]),
        "packetmux_frame_decode": (c_uint8, [data, c_size_t, POINTER(Limits), POINTER(Frame)]),
        "packetmux_frame_encode": (
            c_uint8,
            [POINTER(Frame), POINTER(Limits), c_char_p, c_size_t, POINTER(c_size_t)],
        ),
    }
    for name, (result, arguments) in signatures.items():
        function = getattr(library, name)
        function.restype = result
        function.argtypes = arguments


def view_of(value: bytes) -> Bytes:
    """A borrowed view of value. The caller keeps value alive while it is used."""
    return Bytes(ctypes.cast(c_char_p(value), c_void_p), len(value))


def copy_of(view: Bytes) -> bytes:
    return ctypes.string_at(view.data, view.size) if view.size else b""


lib = ctypes.CDLL(_find_library())
_declare(lib)
