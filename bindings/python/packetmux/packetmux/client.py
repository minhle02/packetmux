"""The client: connect through a dial function, and reconnect through it too.

The client is a state machine with one class per state, like the core (D12). The dialing states
start the dial loop on entry, so nothing else here knows about retries.
"""

import asyncio
import secrets
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Awaitable, Callable, Mapping, Optional, Sequence

from .core import Reason, Role
from .errors import GiveUp, NoConnection, PacketMuxError, SessionClosed, SessionNotFound
from .host import Connection, SessionHost, handler_table, log, spawn
from .stream import Handlers, Stream, StreamInfo
from .transport import Transport

Dial = Callable[[str], Awaitable[Transport]]


class ClientState(Enum):
    CONNECTING = "connecting"
    CONNECTED = "connected"
    RECONNECTING = "reconnecting"
    CLOSED = "closed"


@dataclass
class ClientOptions:
    connect_timeout: float = 10.0
    reconnect_backoff: Sequence[float] = (0.5, 1, 2, 4, 8)  # the last value repeats
    inflight_limit: int = 64 * 1024
    session: Mapping[str, int] = field(default_factory=dict)  # core config: receive_window, ...


class _State:
    id = ClientState.CONNECTING

    def __init__(self, client: "Client") -> None:
        self.client = client

    def on_enter(self) -> None:
        pass

    def on_exit(self) -> None:
        pass

    def on_connected(self) -> None:
        pass

    def on_lost(self) -> None:
        pass

    async def open_stream(self, target: Any, params: bytes) -> Stream:
        await self.client._wait_ready()
        return await self.client._state.open_stream(target, params)


class _DialingState(_State):
    def on_enter(self) -> None:
        self._task = spawn(self._dial_loop())

    def on_exit(self) -> None:
        self._task.cancel()

    def on_connected(self) -> None:
        self.client._enter(_ConnectedState)

    async def _dial_loop(self) -> None:
        client = self.client
        attempt = 0
        backoff = client._options.reconnect_backoff
        while True:
            try:
                transport = await asyncio.wait_for(
                    client._dial(client.session_id), client._options.connect_timeout
                )
                client._host.check_message_size(transport)
                connection = client._host.attach(transport)
                await connection.settled.wait()
            except GiveUp as error:
                client._fail(error)
                return
            except asyncio.CancelledError:
                raise
            except Exception as error:
                client._last_dial_error = error
            await asyncio.sleep(backoff[min(attempt, len(backoff) - 1)])
            attempt += 1


class _ConnectingState(_DialingState):
    id = ClientState.CONNECTING


class _ReconnectingState(_DialingState):
    id = ClientState.RECONNECTING


class _ConnectedState(_State):
    id = ClientState.CONNECTED

    def on_enter(self) -> None:
        self.client._ready.set()
        if not self.client._first_outcome.done():
            self.client._first_outcome.set_result(None)

    def on_exit(self) -> None:
        self.client._ready.clear()

    def on_lost(self) -> None:
        self.client._enter(_ReconnectingState)

    async def open_stream(self, target: Any, params: bytes) -> Stream:
        return self.client._host.open(target, params)


class _ClosedState(_State):
    id = ClientState.CLOSED

    def on_enter(self) -> None:
        client = self.client
        client._ready.set()  # waiters wake, and find this state
        if not client._first_outcome.done():
            client._first_outcome.set_exception(client.close_error or SessionClosed())

    async def open_stream(self, target: Any, params: bytes) -> Stream:
        raise self.client.close_error or SessionClosed()


class Client:
    """Create one with connect()."""

    def __init__(
        self,
        dial: Dial,
        session_id: str,
        handlers: Optional[Handlers],
        capabilities: bytes,
        options: ClientOptions,
    ) -> None:
        self.session_id = session_id
        self.close_error: Optional[PacketMuxError] = None
        self._dial = dial
        self._options = options
        self._listeners: list[Callable[[ClientState], None]] = []
        self._ready = asyncio.Event()
        self._first_outcome: asyncio.Future = asyncio.get_running_loop().create_future()
        self._last_dial_error: Optional[Exception] = None
        self._last_connection: Optional[Connection] = None
        self._host = SessionHost(
            self,
            Role.CLIENT,
            session_id,
            capabilities,
            handler_table(handlers),
            options.inflight_limit,
            options.session,
        )
        self._state: _State = _State(self)

    @property
    def state(self) -> ClientState:
        return self._state.id

    @property
    def transport(self) -> Optional[Transport]:
        return self._host.active_transport

    @property
    def rtt_ms(self) -> Optional[int]:
        return self._host.rtt_ms

    async def open_stream(self, target: Any, params: bytes = b"") -> Stream:
        """Opens a stream, or waits for a connection first. It does not wait for OPEN_OK."""
        try:
            return await asyncio.wait_for(
                self._state.open_stream(target, params), self._options.connect_timeout
            )
        except TimeoutError:
            raise NoConnection() from None

    def streams(self) -> list[StreamInfo]:
        return self._host.list_streams()

    def on_state_change(self, listener: Callable[[ClientState], None]) -> Callable[[], None]:
        """Returns a function that removes the listener."""
        self._listeners.append(listener)
        return lambda: self._listeners.remove(listener)

    async def close(self, code: Reason = Reason.OK) -> None:
        self._closing_locally = True
        self._host.close(code)
        await self.wait_closed()

    async def wait_closed(self) -> None:
        await self._host.closed.wait()
        if self._last_connection is not None:
            await self._last_connection.closed.wait()

    async def __aenter__(self) -> "Client":
        return self

    async def __aexit__(self, *_: object) -> None:
        await self.close()

    # --- From the host -----------------------------------------------------------------------

    def on_connected(self) -> None:
        self._state.on_connected()

    def on_lost(self, replaced: bool) -> None:
        self._state.on_lost()

    def on_closed(self, code: Reason, connection: Optional[Connection]) -> None:
        self._last_connection = connection
        if not self._closing_locally and self.close_error is None:
            self.close_error = SessionNotFound() if code == Reason.SESSION_NOT_FOUND else SessionClosed(code)
        self._enter(_ClosedState)

    # --- Internals -------------------------------------------------------------------------

    _closing_locally = False

    def _enter(self, state: type[_State]) -> None:
        self._state.on_exit()
        self._state = state(self)
        self._state.on_enter()
        for listener in list(self._listeners):
            try:
                listener(self._state.id)
            except Exception:
                log.exception("state listener failed")

    async def _wait_ready(self) -> None:
        await self._ready.wait()

    def _fail(self, error: PacketMuxError) -> None:
        self.close_error = error
        self._host.close(Reason.OK)


def _new_session_id() -> str:
    return secrets.token_hex(16)


async def connect(
    dial: Dial,
    *,
    handlers: Optional[Handlers] = None,
    capabilities: bytes = b"",
    options: Optional[ClientOptions] = None,
) -> Client:
    """Returns once the handshake has finished. Reconnecting after that is automatic."""
    options = options or ClientOptions()
    client = Client(dial, _new_session_id(), handlers, capabilities, options)
    client._enter(_ConnectingState)
    try:
        await asyncio.wait_for(asyncio.shield(client._first_outcome), options.connect_timeout)
    except TimeoutError:
        client._first_outcome.cancel()
        client._fail(NoConnection(str(client._last_dial_error or "no connection")))
        raise client.close_error from client._last_dial_error
    except asyncio.CancelledError:  # nobody holds the client, so nothing else would close it
        client._first_outcome.cancel()
        client._fail(NoConnection("connect was cancelled"))
        raise
    if client.close_error is not None:  # refused: GOAWAY came right behind HELLO
        raise client.close_error
    return client
