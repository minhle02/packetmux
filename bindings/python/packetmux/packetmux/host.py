"""Runs one core session on asyncio: every host rule in one place (FR-SDK-4).

SessionHost is one per session and owns the core. Connection is one per Transport, and runs the
reader and sender loops. Every core call goes through _call, so no path can skip the drain.
"""

import asyncio
import logging
import time
from typing import Any, Coroutine, Mapping, Optional

from . import core
from .core import Event, EventType, Reason, Role, Session, SessionError, Status
from .errors import GiveUp, InvalidParams, LimitExceeded, NoConnection, Reject, SessionClosed, TransportClosed
from .params import decode_params, encode_params, is_stream_type
from .stream import Handler, Handlers, IncomingStream, Stream, StreamInfo
from .transport import Transport

log = logging.getLogger("packetmux")


def now_ms() -> int:
    return time.monotonic_ns() // 1_000_000


_tasks: set[asyncio.Task] = set()


def spawn(coroutine: Coroutine) -> asyncio.Task:
    """Starts a task, and keeps it referenced until it ends."""
    task = asyncio.get_running_loop().create_task(coroutine)
    _tasks.add(task)
    task.add_done_callback(_tasks.discard)
    return task


async def wait_at_most(awaitable: Any, seconds: float) -> None:
    try:
        await asyncio.wait_for(awaitable, seconds)
    except (TimeoutError, TransportClosed):
        pass


def handler_table(handlers: Optional[Handlers]) -> dict[str, tuple[Handler, Optional[type]]]:
    table: dict[str, tuple[Handler, Optional[type]]] = {}
    for target, handler in handlers or ():
        if is_stream_type(target):
            table[target.__stream_kind__] = (handler, target)
        else:
            table[target] = (handler, None)
    return table


class Connection:
    """One Transport, plus the loops that move messages between it and the host."""

    def __init__(self, host: "SessionHost", transport: Transport) -> None:
        self.transport = transport
        self.settled = asyncio.Event()  # the handshake finished, or the connection ended
        self.closed = asyncio.Event()
        self._host = host
        self._finished = False
        self._work = asyncio.Event()
        self._reader_task: Optional[asyncio.Task] = None
        self._sender_task: Optional[asyncio.Task] = None

    def start(self) -> None:
        self._reader_task = spawn(self._reader())
        self._sender_task = spawn(self._sender())

    def wake(self) -> None:
        self._work.set()

    def finish(self) -> None:
        """The session is done with this connection. A GOAWAY may still be in the sender's hands."""
        if self._finished:
            return
        self._finished = True
        self.settled.set()
        self._work.set()
        spawn(self._shutdown())

    async def _reader(self) -> None:
        try:
            while not self._finished:
                self._host.feed(self, await self.transport.recv())
        except TransportClosed:
            self._host.transport_lost(self)
        except asyncio.CancelledError:
            raise
        except Exception:
            log.exception("reader failed")
            self._host.transport_lost(self)

    async def _sender(self) -> None:
        limit = self._host.inflight_limit
        try:
            while not self._finished:
                await self.transport.wait_buffer_below(limit)
                if self._finished:
                    break  # replaced while waiting: what is queued now is the new connection's
                message = self._host.poll_transmit()
                if message is None:
                    if not self._finished:
                        self._work.clear()
                        await self._work.wait()
                    continue
                await self.transport.send(message)  # a GOAWAY just taken still goes out
        except TransportClosed:
            self._host.transport_lost(self)
        except asyncio.CancelledError:
            raise
        except Exception:
            log.exception("sender failed")
            self._host.transport_lost(self)

    async def _shutdown(self) -> None:
        assert self._sender_task is not None and self._reader_task is not None
        drain = self._host.drain_timeout
        await asyncio.wait({self._sender_task}, timeout=drain)
        await wait_at_most(self.transport.wait_buffer_below(1), drain)
        self._sender_task.cancel()
        self._reader_task.cancel()
        try:
            await self.transport.close()
        except Exception:
            log.exception("transport close failed")
        finally:
            self.closed.set()


class SessionHost:
    """Owns one core session. The owner is the Client or ServerSession around it."""

    def __init__(
        self,
        owner: Any,
        role: Role,
        session_id: str,
        capabilities: bytes,
        handlers: Mapping[str, tuple[Handler, Optional[type]]],
        inflight_limit: int,
        max_read_buffer: int,
        session_options: Mapping[str, int],
    ) -> None:
        self.owner = owner
        self.connection: Optional[Connection] = None
        self.connected = False
        self.inflight_limit = inflight_limit
        self.max_read_buffer = max_read_buffer
        self.closed = asyncio.Event()
        defaults = {**core.config_defaults(), **session_options}
        self.max_write_call = defaults["max_queued_bytes_per_stream"]
        self.drain_timeout = defaults["drain_timeout_ms"] / 1000
        self.largest_frame = 5 + max(defaults["max_data_payload"], defaults["max_control_payload"])
        self._handlers = handlers
        self._core: Optional[Session] = Session(role, session_id, capabilities, **session_options)
        self._streams: dict[int, Stream] = {}
        self._handler_tasks: set[asyncio.Task] = set()
        self._replacing = False
        self._core_ended = False
        self._timer: Optional[asyncio.TimerHandle] = None
        self._timer_at = 0

    @property
    def rtt_ms(self) -> Optional[int]:
        return None if self._core is None else self._core.rtt_ms

    # --- The one path into the core ----------------------------------------------------------

    def _after_call(self) -> None:
        while self._core is not None and (event := self._core.poll_event()) is not None:
            self._on_event(event)
        if self._core_ended:
            self._release_core()
            return
        self._arm_timer()
        if self.connection is not None:
            self.connection.wake()

    def _call(self, method: str, *args: Any) -> Any:
        result = getattr(self._core, method)(*args)
        self._after_call()
        return result

    def _release_core(self) -> None:
        if self._timer is not None:
            self._timer.cancel()
            self._timer = None
        if self._core is not None:
            self._core.destroy()  # WASM has no garbage collector for this, so neither do we
            self._core = None

    def _arm_timer(self) -> None:
        deadline = self._core.next_deadline_ms()
        if deadline is None:
            if self._timer is not None:
                self._timer.cancel()
                self._timer = None
        elif self._timer is None or deadline < self._timer_at:
            # A later deadline can wait: a tick before it is harmless and re-arms for the rest.
            if self._timer is not None:
                self._timer.cancel()
            self._timer_at = deadline
            delay = max(0, deadline - now_ms()) / 1000
            self._timer = asyncio.get_running_loop().call_later(delay, self._on_timer)

    def _on_timer(self) -> None:
        self._timer = None
        if self._core is not None:
            self._call("on_tick", now_ms())

    # --- Connections -------------------------------------------------------------------------

    def check_message_size(self, transport: Transport) -> None:
        if transport.max_message_size < self.largest_frame:
            raise GiveUp(
                f"transport carries {transport.max_message_size} bytes, "
                f"and frames can reach {self.largest_frame}"
            )

    def attach(self, transport: Transport, first_message: Optional[bytes] = None) -> Connection:
        if self._core is None:
            raise SessionClosed()
        if self.connection is not None:  # the client re-dialed before the old one died
            self._replacing = True
            self._call("on_connection_lost")
            self._replacing = False
            if self._core is None:  # it was closing, and that was its connection
                raise SessionClosed()
        self._core.attach_connection(now_ms())
        connection = Connection(self, transport)
        self.connection = connection
        if first_message is not None:  # a server has already read the HELLO
            self._core.on_message(first_message, now_ms())
        connection.start()
        self._after_call()
        return connection

    def feed(self, connection: Connection, message: bytes) -> None:
        if connection is self.connection:  # a replaced connection may report late
            self._call("on_message", message, now_ms())

    def transport_lost(self, connection: Connection) -> None:
        if connection is self.connection:
            self._call("on_connection_lost")

    def poll_transmit(self) -> Optional[bytes]:
        return None if self._core is None else self._call("poll_transmit")

    def close(self, code: Reason) -> None:
        if self._core is not None:
            try:
                self._call("close", code, now_ms())
            except SessionError as error:
                if error.status != Status.CLOSED:  # already closing
                    raise

    # --- Events. Synchronous: they only buffer and wake ---------------------------------------

    def _on_event(self, event: Event) -> None:
        stream = self._streams.get(event.stream_id)
        match event.type:
            case EventType.DATA if stream is not None:
                stream.on_data(event.data)
            case EventType.EOF if stream is not None:
                stream.on_eof()
            case EventType.WRITABLE if stream is not None:
                stream.on_writable()
            case EventType.STREAM_ACCEPTED if stream is not None:
                stream.on_accepted()
            case EventType.STREAM_CLOSED if stream is not None:
                del self._streams[event.stream_id]
                stream.on_closed(event.code)
            case EventType.STREAM_OPENED:
                self._on_stream_opened(event)
            case EventType.CONNECTED:
                self.connected = True
                if self.connection is not None:
                    self.connection.settled.set()
                self.owner.on_connected()
            case EventType.DISCONNECTED:
                self.connected = False
                connection, self.connection = self.connection, None
                if connection is not None:
                    connection.finish()
                self.owner.on_lost(self._replacing)
            case EventType.SESSION_CLOSED:
                self.connected = False
                connection, self.connection = self.connection, None
                if connection is not None:
                    connection.finish()
                for task in list(self._handler_tasks):
                    task.cancel()
                self._core_ended = True
                self.closed.set()
                self.owner.on_closed(event.code, connection)

    def _on_stream_opened(self, event: Event) -> None:
        entry = self._handlers.get(event.kind)
        if entry is None:
            self._core.reject_stream(event.stream_id, Reason.UNSUPPORTED_KIND)
            return
        handler, stream_type = entry
        params: Any = event.params
        if stream_type is not None:
            try:
                params = decode_params(stream_type, event.params)
            except InvalidParams as error:
                self._core.reject_stream(event.stream_id, Reason.PROTOCOL_ERROR, str(error).encode())
                return
        stream = Stream(self, event.stream_id, event.kind, accepted=False)
        self._streams[event.stream_id] = stream
        request = IncomingStream(stream, event.kind, params)
        task = spawn(self._run_handler(handler, request))
        self._handler_tasks.add(task)
        task.add_done_callback(self._handler_tasks.discard)

    async def _run_handler(self, handler: Handler, request: IncomingStream) -> None:
        try:
            await handler(request)
        except Reject as rejected:
            request.on_handler_rejected(rejected.code, rejected.message)
        except asyncio.CancelledError:
            raise
        except Exception:
            log.exception("handler for %s failed", request.kind)
            request.on_handler_failed()
        else:
            await request.on_handler_returned()

    # --- Streams -----------------------------------------------------------------------------

    def open(self, target: Any, params: bytes) -> Stream:
        kind = target
        if is_stream_type(target):
            kind, params = target.__stream_kind__, encode_params(target)
        if self._core is None:
            raise SessionClosed()
        try:
            stream_id = self._core.open_stream(kind, params, now_ms())
        except SessionError as error:
            raise _open_error(error) from error
        stream = Stream(self, stream_id, kind, accepted=False)
        self._streams[stream_id] = stream
        self._after_call()
        return stream

    def write(self, stream_id: int, data: Any) -> int:
        return self._call("write", stream_id, data, now_ms())

    def send_eof(self, stream_id: int) -> None:
        self._call("send_eof", stream_id)

    def reset(self, stream_id: int, code: Reason) -> None:
        self._call("reset_stream", stream_id, code)

    def reset_in_drain(self, stream_id: int, code: Reason) -> None:
        self._core.reset_stream(stream_id, code)  # the drain in progress picks up its events

    def accept(self, stream: Stream) -> None:
        if not stream.is_closed:
            self._call("accept_stream", stream.id)
            stream.on_accepted()

    def reject(self, stream: Stream, code: Reason, message: bytes) -> None:
        if not stream.is_closed:
            self._call("reject_stream", stream.id, code, message)

    def list_streams(self) -> list[StreamInfo]:
        infos = [] if self._core is None else self._core.list_streams()
        return [self._with_high_water(info) for info in infos]

    def stream_info(self, stream: Stream) -> Optional[StreamInfo]:
        return next((info for info in self.list_streams() if info.stream_id == stream.id), None)

    def _with_high_water(self, info: core.StreamInfo) -> StreamInfo:
        stream = self._streams.get(info.stream_id)
        return StreamInfo(
            **vars(info), read_high_water=0 if stream is None else stream.read_high_water
        )


def _open_error(error: SessionError) -> Exception:
    if error.status == Status.NOT_CONNECTED:
        return NoConnection()
    if error.status == Status.CLOSED:
        return SessionClosed()
    if error.status in (Status.LIMIT_EXCEEDED, Status.IDS_EXHAUSTED):
        return LimitExceeded(error.status.name)
    return InvalidParams(error.status.name)  # a kind that is not a valid identifier
