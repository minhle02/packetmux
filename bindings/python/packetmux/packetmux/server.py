"""The server: hand it each accepted Transport, and it finds or creates the session."""

import asyncio
from dataclasses import dataclass, field
from typing import Any, Awaitable, Callable, Mapping, Optional

from .core import CodecError, Frame, FrameType, Reason, Role, config_defaults, decode_frame, encode_frame
from .errors import GiveUp, Refuse, SessionClosed, TransportClosed
from .host import Connection, SessionHost, handler_table, log, spawn, wait_at_most
from .stream import Handlers, Stream, StreamInfo
from .transport import Transport


@dataclass
class ServerOptions:
    grace_period: float = 120.0
    max_sessions: int = 1024
    inflight_limit: int = 64 * 1024
    session: Mapping[str, int] = field(default_factory=dict)  # core config: receive_window, ...


class ServerSession:
    def __init__(self, server: "Server", session_id: str, data: Any) -> None:
        self.session_id = session_id
        self.data = data  # what authorize returned
        self._server = server
        self._grace: Optional[asyncio.TimerHandle] = None
        options = server._options
        self._host = SessionHost(
            self,
            Role.SERVER,
            session_id,
            server._capabilities,
            server._handlers,
            options.inflight_limit,
            options.session,
        )

    @property
    def connected(self) -> bool:
        return self._host.connected

    @property
    def transport(self) -> Optional[Transport]:
        return self._host.active_transport

    @property
    def rtt_ms(self) -> Optional[int]:
        return self._host.rtt_ms

    async def open_stream(self, target: Any, params: bytes = b"") -> Stream:
        """Fails with NoConnection while disconnected: the client may never come back."""
        return self._host.open(target, params)

    def streams(self) -> list[StreamInfo]:
        return self._host.list_streams()

    async def close(self, code: Reason = Reason.OK) -> None:
        self._host.close(code)
        await self._host.closed.wait()

    # --- From the host -----------------------------------------------------------------------

    def on_connected(self) -> None:
        pass

    def on_lost(self, replaced: bool) -> None:
        if not replaced:  # a replacing connection is already here
            self._cancel_grace()
            self._grace = asyncio.get_running_loop().call_later(
                self._server._options.grace_period, self._expire
            )

    def on_closed(self, code: Reason, connection: Optional[Connection]) -> None:
        self._cancel_grace()
        self._server._sessions.pop(self.session_id, None)
        self._server._notify_closed(self, code)

    # --- Internals -------------------------------------------------------------------------

    def _cancel_grace(self) -> None:
        if self._grace is not None:
            self._grace.cancel()
            self._grace = None

    def _expire(self) -> None:
        self._grace = None
        spawn(self.close(Reason.TIMEOUT))


@dataclass
class ConnectRequest:
    session_id: str
    capabilities: bytes
    resuming: bool
    context: Any  # what the consumer passed to serve()
    session: Optional[ServerSession]  # the existing one, or None


Authorize = Callable[[ConnectRequest], Awaitable[Any]]  # returns data, or raises Refuse
OnSessionClosed = Callable[[ServerSession, Reason], None]


class Server:
    def __init__(
        self,
        handlers: Optional[Handlers] = None,
        authorize: Optional[Authorize] = None,
        capabilities: bytes = b"",
        options: Optional[ServerOptions] = None,
        on_session_closed: Optional[OnSessionClosed] = None,
    ) -> None:
        self._handlers = handler_table(handlers)
        self._authorize = authorize
        self._on_session_closed = on_session_closed
        self._capabilities = capabilities
        self._options = options or ServerOptions()
        self._sessions: dict[str, ServerSession] = {}
        defaults = {**config_defaults(), **self._options.session}
        self._hello_timeout = defaults["hello_timeout_ms"] / 1000
        self._drain_timeout = defaults["drain_timeout_ms"] / 1000

    def sessions(self) -> list[ServerSession]:
        return list(self._sessions.values())

    async def serve(self, transport: Transport, context: Any = None) -> None:
        """Runs one connection. Returns when it ends."""
        try:
            first = await asyncio.wait_for(transport.recv(), self._hello_timeout)
            hello = decode_frame(first)
        except (TimeoutError, TransportClosed, CodecError):
            await transport.close()  # no session has been touched
            return
        if hello.type != FrameType.HELLO:
            await transport.close()
            return

        data = None
        try:
            if self._authorize is not None:
                data = await self._authorize(
                    ConnectRequest(
                        hello.session_id,
                        hello.capabilities,
                        hello.resuming,
                        context,
                        self._sessions.get(hello.session_id),
                    )
                )
        except Refuse as refused:
            await self._refuse(transport, hello, refused.code)
            return
        except Exception:
            await transport.close()
            raise

        session = self._sessions.get(hello.session_id)  # authorize may have waited
        if session is None:
            if hello.resuming:
                await self._refuse(transport, hello, Reason.SESSION_NOT_FOUND)
                return
            if len(self._sessions) >= self._options.max_sessions:
                await self._refuse(transport, hello, Reason.LIMIT_EXCEEDED)
                return
            session = self._create(hello.session_id, data)

        try:
            session._host.check_message_size(transport)
        except GiveUp:
            await transport.close()
            raise
        session._cancel_grace()
        try:
            connection = session._host.attach(transport, first)
        except SessionClosed:  # it ended while this connection was arriving
            await self._refuse(transport, hello, Reason.SESSION_NOT_FOUND)
            return
        await connection.closed.wait()

    async def close(self, code: Reason = Reason.OK) -> None:
        await asyncio.gather(*(session.close(code) for session in self.sessions()))

    def _notify_closed(self, session: ServerSession, code: Reason) -> None:
        if self._on_session_closed is not None:
            try:
                self._on_session_closed(session, code)
            except Exception:
                log.exception("session closed listener failed")

    def _create(self, session_id: str, data: Any) -> ServerSession:
        session = ServerSession(self, session_id, data)
        self._sessions[session_id] = session
        return session

    async def _refuse(self, transport: Transport, hello: Frame, code: Reason) -> None:
        """HELLO first: anything else before it is a protocol error."""
        try:
            await transport.send(
                encode_frame(
                    Frame(
                        type=FrameType.HELLO,
                        session_id=hello.session_id,
                        capabilities=self._capabilities,
                    )
                )
            )
            await transport.send(encode_frame(Frame(type=FrameType.GOAWAY, code=code)))
        except TransportClosed:
            pass
        await wait_at_most(transport.wait_buffer_below(1), self._drain_timeout)
        await transport.close()
