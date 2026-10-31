"""An async byte stream, and the request a handler gets for an inbound one."""

import asyncio
from collections import deque
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any, AsyncIterator, Awaitable, Callable, Generic, Iterable, TypeVar

from .core import Reason, Role, StreamState
from .errors import StreamClosed, StreamReset

if TYPE_CHECKING:
    from .host import SessionHost

T = TypeVar("T")


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
    read_high_water: int


class Stream:
    def __init__(self, host: "SessionHost", stream_id: int, kind: str, accepted: bool) -> None:
        self.id = stream_id
        self.kind = kind
        self.reset_code: Reason | None = None
        self.read_high_water = 0
        self._host = host
        self._closed = False
        self._close_code = Reason.OK
        self._remote_eof = False
        self._eof_sent = False
        self._read_buffer: deque[bytes] = deque()
        self._read_bytes = 0
        self._readable = asyncio.Event()
        self._writable = asyncio.Event()
        self._accepted = asyncio.Event()
        self._finished = asyncio.Event()
        self._write_lock = asyncio.Lock()
        if accepted:
            self._accepted.set()

    @property
    def session(self) -> Any:
        """The Client or ServerSession this stream belongs to."""
        return self._host.owner

    @property
    def is_closed(self) -> bool:
        return self._closed

    # --- Consumer API ----------------------------------------------------------------------

    async def read(self) -> bytes:
        """The next chunk. b"" means the peer half-closed and everything has been read."""
        while True:
            if self.reset_code is not None:  # a reset wins over buffered data
                raise StreamReset(self.reset_code)
            if self._read_buffer:
                return self._take()
            if self._remote_eof or self._closed:
                return b""
            self._readable.clear()
            await self._readable.wait()

    async def write(self, data: bytes) -> None:
        """Returns once every byte is queued. Cancelling leaves an unknown amount sent."""
        async with self._write_lock:  # FIFO: concurrent writes do not interleave
            self._check_writable()
            view = memoryview(data)
            while view:
                self._writable.clear()  # before the call, so its WRITABLE is not lost
                piece = view[: self._host.max_write_call]  # the binding copies each call
                accepted = self._host.write(self.id, piece)
                view = view[accepted:]
                if accepted < len(piece):
                    await self._writable.wait()
                    self._check_writable()

    async def send_eof(self) -> None:
        async with self._write_lock:  # EOF goes after any write still in progress
            if self.reset_code is not None:
                raise StreamReset(self.reset_code)
            if not self._eof_sent and not self._closed:
                self._eof_sent = True
                self._host.send_eof(self.id)

    def reset(self, code: Reason = Reason.KILLED) -> None:
        if not self._closed:
            self._host.reset(self.id, code)

    async def wait_accepted(self) -> None:
        await self._accepted.wait()
        if self.reset_code is not None:
            raise StreamReset(self.reset_code)

    async def wait_closed(self) -> Reason:
        """Returns the reason. OK means a clean close."""
        await self._finished.wait()
        return self._close_code

    def info(self) -> StreamInfo | None:
        """Details from the core, or None once the stream has closed."""
        return self._host.stream_info(self)

    def __aiter__(self) -> AsyncIterator[bytes]:
        return self

    async def __anext__(self) -> bytes:
        chunk = await self.read()
        if not chunk:
            raise StopAsyncIteration
        return chunk

    # --- From the host. These run inside the drain, so they cannot wait ---------------------

    def on_data(self, chunk: bytes) -> None:
        if self._closed:
            return
        self._read_buffer.append(chunk)
        self._read_bytes += len(chunk)
        self.read_high_water = max(self.read_high_water, self._read_bytes)
        if self._read_bytes > self._host.max_read_buffer:
            self._host.reset_in_drain(self.id, Reason.LIMIT_EXCEEDED)  # STREAM_CLOSED follows
            return
        self._readable.set()

    def on_eof(self) -> None:
        self._remote_eof = True
        self._readable.set()

    def on_accepted(self) -> None:
        self._accepted.set()

    def on_writable(self) -> None:
        self._writable.set()

    def on_closed(self, code: Reason) -> None:
        self._closed = True
        self._close_code = code
        if code != Reason.OK:
            self.reset_code = code
        self._readable.set()
        self._writable.set()
        self._accepted.set()
        self._finished.set()

    # --- Internals -------------------------------------------------------------------------

    def _take(self) -> bytes:
        """The one path bytes leave by, which is where credit will be returned (FR-AST-7)."""
        chunk = self._read_buffer.popleft()
        self._read_bytes -= len(chunk)
        return chunk

    def _check_writable(self) -> None:
        if self.reset_code is not None:
            raise StreamReset(self.reset_code)
        if self._closed or self._eof_sent:
            raise StreamClosed(f"stream {self.id} is closed for writing")


class IncomingStream(Generic[T]):
    """A stream the peer opened. The handler accepts it or rejects it."""

    def __init__(self, stream: Stream, kind: str, params: T) -> None:
        self.kind = kind
        self.params = params
        self._stream = stream
        self._settled = False

    @property
    def session(self) -> Any:
        return self._stream.session

    def accept(self) -> Stream:
        if not self._settled:
            self._settled = True
            self._stream._host.accept(self._stream)
        return self._stream

    def reject(self, code: Reason, message: bytes = b"") -> None:
        if not self._settled:
            self._settled = True
            self._stream._host.reject(self._stream, code, message)

    # --- What happens when the handler finishes ---------------------------------------------

    def on_handler_rejected(self, code: Reason, message: bytes) -> None:
        if self._settled:
            self._stream.reset(code)
        else:
            self.reject(code, message)

    def on_handler_failed(self) -> None:
        self.on_handler_rejected(
            Reason.KILLED if self._settled else Reason.CONNECT_FAILED, b""
        )

    async def on_handler_returned(self) -> None:
        if not self._settled:
            self.reject(Reason.POLICY_DENIED)
        else:  # the stream stays until the peer's EOF, so it can still read the reply
            try:
                await self._stream.send_eof()
            except StreamReset:
                pass


Handler = Callable[[IncomingStream], Awaitable[None]]
Handlers = Iterable[tuple[str | type, Handler]]
