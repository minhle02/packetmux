"""Every error the SDK raises, and the ones a consumer raises to steer it."""

from .core import Reason


class PacketMuxError(Exception):
    pass


class TransportClosed(PacketMuxError):
    """A Transport has closed. Only code that implements a Transport sees this."""


class StreamReset(PacketMuxError):
    def __init__(self, code: Reason) -> None:
        self.code = code
        super().__init__(f"stream reset: {code.name}")


class StreamClosed(PacketMuxError):
    """Writing after send_eof, or after a clean close."""


class NoConnection(PacketMuxError):
    """No connection within connect_timeout, or a server session is disconnected."""


class SessionClosed(PacketMuxError):
    def __init__(self, code: Reason = Reason.OK) -> None:
        self.code = code
        super().__init__(f"session closed: {code.name}")


class SessionNotFound(SessionClosed):
    """The server no longer has this session. Connect again for a new one."""

    def __init__(self) -> None:
        super().__init__(Reason.SESSION_NOT_FOUND)


class LimitExceeded(PacketMuxError):
    """max_streams is reached, or stream IDs are exhausted."""


class InvalidParams(PacketMuxError):
    """Typed params do not match their declared fields."""


class GiveUp(PacketMuxError):
    """Raise from a dial function to stop reconnecting. The client closes with this error."""


class Reject(PacketMuxError):
    """Raise from a handler to reject the stream."""

    def __init__(self, code: Reason, message: bytes = b"") -> None:
        self.code = code
        self.message = message
        super().__init__(f"rejected: {code.name}")


class Refuse(PacketMuxError):
    """Raise from authorize to refuse the connection."""

    def __init__(self, code: Reason = Reason.POLICY_DENIED) -> None:
        self.code = code
        super().__init__(f"refused: {code.name}")
