"""Enum values of the C ABI. tests/test_constants.py checks them against the header."""

from enum import IntEnum

NO_DEADLINE = 2**64 - 1


class Status(IntEnum):
    OK = 0
    CLOSED = 1
    NOT_CONNECTED = 2
    ALREADY_CONNECTED = 3
    EVENTS_PENDING = 4
    UNKNOWN_STREAM = 5
    INVALID_STATE = 6
    LIMIT_EXCEEDED = 7
    IDS_EXHAUSTED = 8
    INVALID_ARGUMENT = 9


class CodecStatus(IntEnum):
    OK = 0
    TRUNCATED = 1
    UNKNOWN_TYPE = 2
    BAD_STREAM_ID = 3
    PAYLOAD_TOO_LARGE = 4
    BAD_PAYLOAD_SIZE = 5
    UNSUPPORTED_VERSION = 6
    BAD_IDENTIFIER = 7
    INVALID_ARGUMENT = 8
    BAD_FLAG = 9
    BAD_WINDOW = 10


class Role(IntEnum):
    CLIENT = 0
    SERVER = 1


class Reason(IntEnum):
    OK = 0
    PEER_CLOSED = 1
    CONNECT_FAILED = 2
    POLICY_DENIED = 3
    LIMIT_EXCEEDED = 4
    TRANSPORT_LOST = 5
    KILLED = 6
    AUTH_REVOKED = 7
    TIMEOUT = 8
    PROTOCOL_ERROR = 9
    UNSUPPORTED_KIND = 10
    SESSION_NOT_FOUND = 11


class FrameType(IntEnum):
    UNKNOWN = 0
    HELLO = 1
    OPEN = 2
    OPEN_OK = 3
    OPEN_ERR = 4
    DATA = 5
    EOF = 6
    RST = 7
    PING = 8
    PONG = 9
    GOAWAY = 10
    WINDOW = 11


class EventType(IntEnum):
    UNKNOWN = 0
    CONNECTED = 1
    STREAM_OPENED = 2
    STREAM_ACCEPTED = 3
    DATA = 4
    EOF = 5
    WRITABLE = 6
    STREAM_CLOSED = 7
    DISCONNECTED = 8
    SESSION_CLOSED = 9


class SessionState(IntEnum):
    DISCONNECTED = 0
    HANDSHAKING = 1
    ESTABLISHED = 2
    DRAINING = 3
    CLOSING = 4
    CLOSED = 5


class StreamState(IntEnum):
    SENDER_OPENING = 0
    SENDER_OPENING_EOF_SENT = 1
    RECEIVER_OPENING = 2
    RECEIVER_OPENING_EOF_RECEIVED = 3
    OPEN = 4
    HALF_CLOSED_LOCAL = 5
    HALF_CLOSED_REMOTE = 6
    CLOSED = 7
