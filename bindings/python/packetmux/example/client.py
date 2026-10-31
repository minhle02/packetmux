"""A PacketMux client over WebSocket.

    python client.py [port]

Start server.py first. Needs the websockets package.
"""

import asyncio
import sys
from dataclasses import dataclass

from packetmux import ClientOptions, Transport, TransportClosed, connect, stream_type
from websockets.asyncio.client import ClientConnection
from websockets.asyncio.client import connect as ws_connect
from websockets.exceptions import ConnectionClosed


class WebSocketTransport(Transport):
    def __init__(self, socket: ClientConnection) -> None:
        self._socket = socket

    async def send(self, message: bytes) -> None:
        if len(message) > self.max_message_size:
            raise ValueError("message too large")
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

    async def close(self) -> None:
        await self._socket.close()


@stream_type("greet")
@dataclass
class Greet:
    name: str
    shout: bool = False


async def main(port: int) -> None:
    async def dial(session_id: str) -> WebSocketTransport:
        """Called to connect, and again after every lost connection."""
        return WebSocketTransport(await ws_connect(f"ws://127.0.0.1:{port}?token=token-1"))

    async with await connect(dial, options=ClientOptions(connect_timeout=5)) as client:
        client.on_state_change(lambda state: print(f"state: {state.value}"))
        print(f"session {client.session_id}")

        stream = await client.open_stream("echo")
        await stream.write(b"hello")
        await stream.send_eof()  # we are done writing, so the server's loop ends
        async for chunk in stream:
            print(f"echo: {chunk.decode()}")

        stream = await client.open_stream(Greet(name="alice", shout=True))
        print(f"greet: {(await stream.read()).decode()}")
        await stream.send_eof()


if __name__ == "__main__":
    asyncio.run(main(int(sys.argv[1]) if len(sys.argv) > 1 else 8765))
