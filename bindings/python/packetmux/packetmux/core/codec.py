"""Frame encoding and decoding: mainly for reading the first HELLO on a server."""

import ctypes
from ctypes import byref, c_size_t
from dataclasses import dataclass

from . import _native
from ._native import copy_of, lib, view_of
from .constants import CodecStatus, FrameType, Reason

_ENCODE_OVERHEAD = 32  # header and length fields


class CodecError(Exception):
    def __init__(self, status: int) -> None:
        self.status = CodecStatus(status)
        super().__init__(self.status.name)


DEFAULT_RECEIVE_WINDOW = 256 * 1024


@dataclass
class Frame:
    type: FrameType = FrameType.UNKNOWN
    stream_id: int = 0
    version: int = 2
    resuming: bool = False
    session_id: str = ""
    capabilities: bytes = b""
    kind: str = ""
    params: bytes = b""
    code: Reason = Reason.OK
    message: bytes = b""
    data: bytes = b""
    nonce: int = 0
    receive_window: int = DEFAULT_RECEIVE_WINDOW
    credit: int = 0


def _limits(max_data_payload: int | None, max_control_payload: int | None) -> _native.Limits:
    config = _native.Config()
    lib.packetmux_config_init(ctypes.byref(config))
    limits = config.limits
    if max_data_payload is not None:
        limits.max_data_payload = max_data_payload
    if max_control_payload is not None:
        limits.max_control_payload = max_control_payload
    return limits


def decode_frame(
    message: bytes,
    max_data_payload: int | None = None,
    max_control_payload: int | None = None,
) -> Frame:
    """Decode one message. Everything is copied, so message can be released."""
    message = bytes(message)
    limits = _limits(max_data_payload, max_control_payload)
    native = _native.Frame()
    status = lib.packetmux_frame_decode(message, len(message), byref(limits), byref(native))
    if status != CodecStatus.OK:
        raise CodecError(status)
    return Frame(
        type=FrameType(native.type),
        stream_id=native.stream_id,
        version=native.version,
        resuming=native.resuming,
        session_id=copy_of(native.session_id).decode("ascii"),
        capabilities=copy_of(native.capabilities),
        kind=copy_of(native.kind).decode("ascii"),
        params=copy_of(native.params),
        code=Reason(native.code),
        message=copy_of(native.message),
        data=copy_of(native.data),
        nonce=native.nonce,
        receive_window=native.receive_window,
        credit=native.credit,
    )


def encode_frame(
    frame: Frame,
    max_data_payload: int | None = None,
    max_control_payload: int | None = None,
) -> bytes:
    limits = _limits(max_data_payload, max_control_payload)
    session_id = frame.session_id.encode("ascii", "surrogateescape")
    kind = frame.kind.encode("ascii", "surrogateescape")
    native = _native.Frame(
        type=frame.type,
        stream_id=frame.stream_id,
        version=frame.version,
        resuming=frame.resuming,
        session_id=view_of(session_id),
        capabilities=view_of(frame.capabilities),
        kind=view_of(kind),
        params=view_of(frame.params),
        code=frame.code,
        message=view_of(frame.message),
        data=view_of(frame.data),
        nonce=frame.nonce,
        receive_window=frame.receive_window,
        credit=frame.credit,
    )
    capacity = (
        len(session_id)
        + len(frame.capabilities)
        + len(kind)
        + len(frame.params)
        + len(frame.message)
        + len(frame.data)
        + _ENCODE_OVERHEAD
    )
    buffer = ctypes.create_string_buffer(capacity)
    size = c_size_t()
    status = lib.packetmux_frame_encode(
        byref(native), byref(limits), buffer, capacity, byref(size)
    )
    if status != CodecStatus.OK:
        raise CodecError(status)
    return buffer.raw[: size.value]
