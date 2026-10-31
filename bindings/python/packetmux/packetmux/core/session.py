"""The Session class: one session with one peer."""

from ctypes import byref, c_size_t, c_uint32, c_uint64
from dataclasses import dataclass

from . import _native
from ._native import copy_of, lib, view_of
from .constants import (
    NO_DEADLINE,
    CodecStatus,
    EventType,
    Reason,
    Role,
    SessionState,
    Status,
    StreamState,
)


class SessionError(Exception):
    def __init__(self, status: int) -> None:
        self.status = Status(status)
        super().__init__(self.status.name)


@dataclass
class Event:
    """One event. It owns its bytes, so it stays valid after the next call."""

    type: EventType
    stream_id: int = 0
    code: Reason = Reason.OK
    codec_status: CodecStatus = CodecStatus.OK
    peer_session_id: str = ""
    capabilities: bytes = b""
    kind: str = ""
    params: bytes = b""
    data: bytes = b""


@dataclass
class StreamInfo:
    stream_id: int
    kind: str
    opener: Role
    state: StreamState
    bytes_sent: int
    bytes_received: int
    created_at_ms: int
    last_activity_ms: int
    send_credit: int


def config_defaults() -> dict[str, int]:
    """The core's default for every numeric setting, so no binding repeats them."""
    config = _native.Config()
    lib.packetmux_config_init(byref(config))
    numbers = {name: getattr(config, name) for name, _ in _native.Config._fields_}
    del numbers["role"], numbers["session_id"], numbers["capabilities"], numbers["limits"]
    numbers["max_data_payload"] = config.limits.max_data_payload
    numbers["max_control_payload"] = config.limits.max_control_payload
    return numbers


def _check(status: int) -> None:
    if status != Status.OK:
        raise SessionError(status)


class Session:
    """See include/packetmux/Session.h. A status other than OK raises SessionError.

    Keyword arguments set any field of packetmux_config_t, such as max_streams.
    Use it as a context manager, or call destroy(), to free the native session.
    """

    def __init__(
        self,
        role: Role,
        session_id: str,
        capabilities: bytes = b"",
        max_data_payload: int | None = None,
        max_control_payload: int | None = None,
        **config_fields: int,
    ) -> None:
        session_id_bytes = session_id.encode("ascii", "surrogateescape")
        config = _native.Config()
        lib.packetmux_config_init(byref(config))
        config.role = role
        config.session_id = view_of(session_id_bytes)
        config.capabilities = view_of(capabilities)
        if max_data_payload is not None:
            config.limits.max_data_payload = max_data_payload
        if max_control_payload is not None:
            config.limits.max_control_payload = max_control_payload
        for name, value in config_fields.items():
            setattr(config, name, value)
        self._handle = lib.packetmux_session_create(byref(config))
        self._message = b""  # events borrow this until they are polled

    def destroy(self) -> None:
        if self._handle is not None:
            lib.packetmux_session_destroy(self._handle)
            self._handle = None

    def __enter__(self) -> "Session":
        return self

    def __exit__(self, *_: object) -> None:
        self.destroy()

    def __del__(self) -> None:
        self.destroy()

    def attach_connection(self, now_ms: int) -> None:
        _check(lib.packetmux_session_attach_connection(self._handle, now_ms))

    def on_message(self, message: bytes, now_ms: int) -> None:
        message = bytes(message)
        _check(lib.packetmux_session_on_message(self._handle, message, len(message), now_ms))
        self._message = message

    def on_connection_lost(self) -> None:
        _check(lib.packetmux_session_on_connection_lost(self._handle))

    def on_tick(self, now_ms: int) -> None:
        _check(lib.packetmux_session_on_tick(self._handle, now_ms))

    def next_deadline_ms(self) -> int | None:
        deadline = lib.packetmux_session_next_deadline_ms(self._handle)
        return None if deadline == NO_DEADLINE else deadline

    def close(self, code: Reason, now_ms: int) -> None:
        _check(lib.packetmux_session_close(self._handle, code, now_ms))

    def open_stream(self, kind: str, params: bytes, now_ms: int) -> int:
        kind_bytes = kind.encode("ascii", "surrogateescape")
        stream_id = c_uint32()
        _check(
            lib.packetmux_session_open_stream(
                self._handle,
                kind_bytes,
                len(kind_bytes),
                params,
                len(params),
                now_ms,
                byref(stream_id),
            )
        )
        return stream_id.value

    def accept_stream(self, stream_id: int) -> None:
        _check(lib.packetmux_session_accept_stream(self._handle, stream_id))

    def reject_stream(self, stream_id: int, code: Reason, message: bytes = b"") -> None:
        _check(
            lib.packetmux_session_reject_stream(
                self._handle, stream_id, code, message, len(message)
            )
        )

    def write(self, stream_id: int, data: bytes, now_ms: int) -> int:
        """Queue data. Returns how many bytes were accepted, which may be 0."""
        data = bytes(data)
        accepted = c_size_t()
        _check(
            lib.packetmux_session_write(
                self._handle, stream_id, data, len(data), now_ms, byref(accepted)
            )
        )
        return accepted.value

    def consume(self, stream_id: int, count: int) -> None:
        """The consumer has taken count DATA bytes. Without this the stream stalls."""
        _check(lib.packetmux_session_consume(self._handle, stream_id, count))

    def send_eof(self, stream_id: int) -> None:
        _check(lib.packetmux_session_send_eof(self._handle, stream_id))

    def reset_stream(self, stream_id: int, code: Reason = Reason.KILLED) -> None:
        _check(lib.packetmux_session_reset_stream(self._handle, stream_id, code))

    def poll_transmit(self) -> bytes | None:
        message = _native.Bytes()
        if not lib.packetmux_session_poll_transmit(self._handle, byref(message)):
            return None
        return copy_of(message)

    def poll_event(self) -> Event | None:
        native = _native.Event()
        if not lib.packetmux_session_poll_event(self._handle, byref(native)):
            return None
        return Event(
            type=EventType(native.type),
            stream_id=native.stream_id,
            code=Reason(native.code),
            codec_status=CodecStatus(native.codec_status),
            peer_session_id=copy_of(native.peer_session_id).decode("ascii"),
            capabilities=copy_of(native.capabilities),
            kind=copy_of(native.kind).decode("ascii"),
            params=copy_of(native.params),
            data=copy_of(native.data),
        )

    @property
    def state(self) -> SessionState:
        return SessionState(lib.packetmux_session_state(self._handle))

    @property
    def rtt_ms(self) -> int | None:
        rtt_ms = c_uint64()
        if not lib.packetmux_session_get_rtt_ms(self._handle, byref(rtt_ms)):
            return None
        return rtt_ms.value

    def list_streams(self) -> list[StreamInfo]:
        count = lib.packetmux_session_list_streams(self._handle)
        streams = []
        for index in range(count):
            native = _native.StreamInfo()
            lib.packetmux_session_stream_at(self._handle, index, byref(native))
            streams.append(
                StreamInfo(
                    stream_id=native.stream_id,
                    kind=copy_of(native.kind).decode("ascii"),
                    opener=Role(native.opener),
                    state=StreamState(native.state),
                    bytes_sent=native.bytes_sent,
                    bytes_received=native.bytes_received,
                    created_at_ms=native.created_at_ms,
                    last_activity_ms=native.last_activity_ms,
                    send_credit=native.send_credit,
                )
            )
        return streams
