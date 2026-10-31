"""A Transport over a real WebSocket, for the tests. It is also the shape a consumer writes."""

from typing import Any

from packetmux import Transport, TransportClosed
from websockets.exceptions import ConnectionClosed


class WsTransport(Transport):
    def __init__(self, socket: Any) -> None:
        self._socket = socket

    async def send(self, message: bytes) -> None:
        if len(message) > self.max_message_size:
            raise ValueError(f"message of {len(message)} bytes is over the limit")
        try:
            await self._socket.send(message)
        except ConnectionClosed as error:
            raise TransportClosed() from error

    async def recv(self) -> bytes:
        try:
            return await self._socket.recv()
        except ConnectionClosed as error:
            raise TransportClosed() from error

    @property
    def buffered_amount(self) -> int:
        return self._socket.transport.get_write_buffer_size()

    async def wait_buffer_below(self, limit: int) -> None:
        if self._socket.close_code is not None:
            raise TransportClosed()
        await super().wait_buffer_below(limit)

    async def close(self) -> None:
        await self._socket.close()
