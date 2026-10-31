"""The one thing a consumer implements: a connection that carries whole messages."""

import asyncio
from abc import ABC, abstractmethod


class Transport(ABC):
    """One connection delivering whole messages, reliably and in order.

    After it closes, for any reason, send, recv and wait_buffer_below raise TransportClosed.
    A message that arrives before recv is called must not be lost.
    """

    max_message_size: int = 1 << 20

    @abstractmethod
    async def send(self, message: bytes) -> None:
        """Hand over one message. Accepted does not mean delivered. May wait."""

    @abstractmethod
    async def recv(self) -> bytes:
        """The next whole message."""

    @property
    @abstractmethod
    def buffered_amount(self) -> int:
        """Bytes accepted by send and not yet on the wire."""

    @abstractmethod
    async def close(self) -> None:
        """Idempotent."""

    async def wait_buffer_below(self, limit: int) -> None:
        """Wait until buffered_amount is below limit. Override with an event where possible."""
        while self.buffered_amount >= limit:
            await asyncio.sleep(0.002)
