"""Test tools for the SDK and for consumers: a memory transport with fault injection, and a
network of them, plus the checks any Transport has to pass (FR-CONF-5).
"""

import asyncio
from collections import deque
from typing import Any, Callable, Optional

from .errors import GiveUp, TransportClosed
from .host import spawn
from .server import Server
from .transport import Transport


class MemoryTransport(Transport):
    """One side of a memory link. Tests can stall it, release it and break it."""

    def __init__(self, max_message_size: int = 1 << 20) -> None:
        self.max_message_size = max_message_size
        self.peer: Optional["MemoryTransport"] = None
        self._inbox: asyncio.Queue[Optional[bytes]] = asyncio.Queue()
        self._closed = False
        self._stalled = False
        self._held: deque[bytes] = deque()
        self._room = asyncio.Event()
        self._room.set()

    @property
    def buffered_amount(self) -> int:
        return sum(len(message) for message in self._held)

    async def send(self, message: bytes) -> None:
        if self._closed:
            raise TransportClosed()
        if len(message) > self.max_message_size:
            raise ValueError(f"message of {len(message)} bytes is over the limit")
        if self._stalled:
            self._held.append(bytes(message))
        else:
            self._deliver(bytes(message))

    async def recv(self) -> bytes:
        if self._closed:
            raise TransportClosed()
        message = await self._inbox.get()
        if message is None:
            self._inbox.put_nowait(None)  # every later recv fails too
            raise TransportClosed()
        return message

    async def wait_buffer_below(self, limit: int) -> None:
        while self._stalled and not self._closed:
            self._room.clear()
            await self._room.wait()
        if self._closed:
            raise TransportClosed()

    async def close(self) -> None:
        if not self._closed:
            self._shut()
            if self.peer is not None:
                self.peer._on_peer_closed()

    # --- Fault injection ---------------------------------------------------------------------

    def stall(self) -> None:
        """The link is full: the sender waits, and nothing is delivered."""
        self._stalled = True

    def release(self) -> None:
        self._stalled = False
        while self._held:
            self._deliver(self._held.popleft())
        self._room.set()

    async def break_connection(self) -> None:
        """The link dies, on both sides."""
        await self.close()

    def fail_locally(self) -> None:
        """This side sees the link die. The peer still thinks it is alive."""
        self._shut()

    def inject(self, message: bytes) -> None:
        """A message arrives on this side, from nobody."""
        self._inbox.put_nowait(message)

    # --- Internals -------------------------------------------------------------------------

    def _deliver(self, message: bytes) -> None:
        if self.peer is not None and not self.peer._closed:
            self.peer._inbox.put_nowait(message)

    def _shut(self) -> None:
        self._closed = True
        self._inbox.put_nowait(None)
        self._room.set()

    def _on_peer_closed(self) -> None:
        if not self._closed:
            self._inbox.put_nowait(None)  # what is already queued is still read first


def memory_transport_pair(max_message_size: int = 1 << 20) -> tuple[MemoryTransport, MemoryTransport]:
    a, b = MemoryTransport(max_message_size), MemoryTransport(max_message_size)
    a.peer, b.peer = b, a
    return a, b


class MemoryNetwork:
    """Connects clients to a Server over memory transports. network.dial is a dial function."""

    def __init__(self, server: Server, context: Any = None) -> None:
        self.links: list[tuple[MemoryTransport, MemoryTransport]] = []
        self.dials = 0
        self.context = context
        self._server = server
        self._failures = 0
        self._give_up = False
        self._flowing = asyncio.Event()
        self._flowing.set()

    def fail_next(self, count: int) -> None:
        self._failures = count

    def give_up(self) -> None:
        self._give_up = True

    def hang(self, value: bool = True) -> None:
        """Dials wait until this is called with False."""
        self._flowing.clear() if value else self._flowing.set()

    async def dial(self, session_id: str) -> MemoryTransport:
        self.dials += 1
        if self._give_up:
            raise GiveUp("refused for good")
        if self._failures > 0:
            self._failures -= 1
            raise OSError("no route to host")
        await self._flowing.wait()
        client, server = memory_transport_pair()
        self.links.append((client, server))
        spawn(self._server.serve(server, self.context))
        return client


TransportPair = Callable[[], Any]  # returns two connected transports, or awaits to


async def _pair(factory: TransportPair) -> tuple[Transport, Transport]:
    pair = factory()
    return await pair if asyncio.iscoroutine(pair) else pair


async def _recv(transport: Transport) -> bytes:
    return await asyncio.wait_for(transport.recv(), 10)


async def _check_order(a: Transport, b: Transport) -> None:
    largest = min(a.max_message_size, 60_000)
    messages = [bytes([i % 256]) * (1 + i * 37 % largest) for i in range(80)]
    for message in messages:
        await a.send(message)
    assert [await _recv(b) for _ in messages] == messages


async def _check_both_directions(a: Transport, b: Transport) -> None:
    await asyncio.gather(a.send(b"ping"), b.send(b"pong"))
    assert await _recv(b) == b"ping"
    assert await _recv(a) == b"pong"


async def _check_oversize(a: Transport, b: Transport) -> None:
    try:
        await a.send(b"\x00" * (a.max_message_size + 1))
    except ValueError:
        return
    raise AssertionError("an oversize message was accepted")


async def _check_bulk(a: Transport, b: Transport) -> None:
    chunk = bytes(i % 251 for i in range(16_000))
    count = 320  # about 5 MB

    async def send_all() -> None:
        for _ in range(count):
            await a.wait_buffer_below(1 << 20)
            await a.send(chunk)

    async def receive_all() -> None:
        for _ in range(count):
            assert await asyncio.wait_for(b.recv(), 30) == chunk

    await asyncio.gather(send_all(), receive_all())
    await asyncio.wait_for(a.wait_buffer_below(1), 10)
    assert a.buffered_amount == 0


async def _check_buffer_wait(a: Transport, b: Transport) -> None:
    await a.send(b"x" * 1000)
    await asyncio.wait_for(a.wait_buffer_below(1 << 20), 10)  # well above what is buffered
    assert await _recv(b) == b"x" * 1000
    await asyncio.wait_for(a.wait_buffer_below(1), 10)


async def _check_early_messages(a: Transport, b: Transport) -> None:
    await a.send(b"early")
    await asyncio.sleep(0.05)  # nobody is in recv while it arrives
    assert await _recv(b) == b"early"


async def _expect_closed(call: Any) -> None:
    try:
        await asyncio.wait_for(call, 10)
    except TransportClosed:
        return
    raise AssertionError("a closed transport did not raise TransportClosed")


async def _check_close_fails_operations(a: Transport, b: Transport) -> None:
    await a.close()
    await _expect_closed(a.send(b"x"))
    await _expect_closed(a.recv())


async def _check_peer_close(a: Transport, b: Transport) -> None:
    await a.close()
    await _expect_closed(b.recv())


async def _check_close_is_idempotent(a: Transport, b: Transport) -> None:
    await a.close()
    await a.close()


TRANSPORT_CHECKS = {
    "order": _check_order,
    "both-directions": _check_both_directions,
    "oversize": _check_oversize,
    "bulk": _check_bulk,
    "buffer-wait": _check_buffer_wait,
    "early-messages": _check_early_messages,
    "close-fails-operations": _check_close_fails_operations,
    "peer-close": _check_peer_close,
    "close-is-idempotent": _check_close_is_idempotent,
}


async def run_transport_check(name: str, factory: TransportPair) -> None:
    a, b = await _pair(factory)
    try:
        await TRANSPORT_CHECKS[name](a, b)
    finally:
        await a.close()
        await b.close()


async def check_transport(factory: TransportPair) -> None:
    """Runs every check on your Transport. factory returns a connected pair of it."""
    for name in TRANSPORT_CHECKS:
        await run_transport_check(name, factory)
