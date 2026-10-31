"""PacketMux: many byte streams over one connection, and a session that survives reconnects.

    from packetmux import connect

    async with await connect(dial) as client:
        stream = await client.open_stream("echo")
        await stream.write(b"hello")

You implement a Transport, and a dial function that opens one. The sync binding is packetmux.core.
"""

from . import core
from .client import Client, ClientOptions, ClientState, Dial, connect
from .core import Reason
from .errors import (
    GiveUp,
    InvalidParams,
    LimitExceeded,
    NoConnection,
    PacketMuxError,
    Refuse,
    Reject,
    SessionClosed,
    SessionNotFound,
    StreamClosed,
    StreamReset,
    TransportClosed,
)
from .params import define_stream, stream_type
from .server import Authorize, ConnectRequest, Server, ServerOptions, ServerSession
from .stream import Handler, Handlers, IncomingStream, Stream, StreamInfo
from .transport import Transport

__all__ = [
    "Authorize",
    "Client",
    "ClientOptions",
    "ClientState",
    "ConnectRequest",
    "Dial",
    "GiveUp",
    "Handler",
    "Handlers",
    "IncomingStream",
    "InvalidParams",
    "LimitExceeded",
    "NoConnection",
    "PacketMuxError",
    "Reason",
    "Refuse",
    "Reject",
    "Server",
    "ServerOptions",
    "ServerSession",
    "SessionClosed",
    "SessionNotFound",
    "Stream",
    "StreamClosed",
    "StreamInfo",
    "StreamReset",
    "Transport",
    "TransportClosed",
    "connect",
    "define_stream",
    "stream_type",
]
