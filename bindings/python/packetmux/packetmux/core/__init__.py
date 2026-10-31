"""Python binding for PacketMux, a thin layer over the C ABI.

The session is sans-I/O and synchronous: the host owns the transport, feeds in
messages and the time, and polls frames and events out. See
include/packetmux/Session.h for the host loop.
"""

from .codec import CodecError, Frame, decode_frame, encode_frame
from .constants import (
    NO_DEADLINE,
    CodecStatus,
    EventType,
    FrameType,
    Reason,
    Role,
    SessionState,
    Status,
    StreamState,
)
from .session import Event, Session, SessionError, StreamInfo, config_defaults

__all__ = [
    "NO_DEADLINE",
    "CodecError",
    "CodecStatus",
    "Event",
    "EventType",
    "Frame",
    "FrameType",
    "Reason",
    "Role",
    "Session",
    "SessionError",
    "SessionState",
    "Status",
    "StreamInfo",
    "StreamState",
    "config_defaults",
    "decode_frame",
    "encode_frame",
]
