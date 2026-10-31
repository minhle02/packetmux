"""A PacketMux server over WebSocket.

    python server.py [port]

Then run client.py. Needs the websockets package.
"""

import asyncio
import sys
from dataclasses import dataclass
from urllib.parse import parse_qs, urlsplit

from packetmux import (
    ConnectRequest,
    IncomingStream,
    Reason,
    Refuse,
    Server,
    Transport,
    TransportClosed,
    stream_type,
)
from websockets.asyncio.server import ServerConnection, serve
from websockets.exceptions import ConnectionClosed

TOKENS = {"token-1": "alice"}  # your real check goes here


class WebSocketTransport(Transport):
    def __init__(self, socket: ServerConnection) -> None:
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


async def echo(request: IncomingStream) -> None:
    stream = request.accept()
    async for chunk in stream:
        await stream.write(chunk)


async def greet(request: IncomingStream[Greet]) -> None:
    greeting = f"hello, {request.params.name}"
    if request.params.shout:
        greeting = greeting.upper()
    await request.accept().write(greeting.encode())


async def authorize(request: ConnectRequest) -> str:
    """Runs before any existing session is touched. What it returns becomes session.data."""
    user = TOKENS.get(request.context)
    if user is None:
        raise Refuse(Reason.AUTH_REVOKED)
    if request.session is not None and request.session.data != user:
        raise Refuse(Reason.AUTH_REVOKED)  # someone else's session
    return user


server = Server(handlers=[("echo", echo), (Greet, greet)], authorize=authorize)


async def on_socket(socket: ServerConnection) -> None:
    query = parse_qs(urlsplit(socket.request.path).query)
    await server.serve(WebSocketTransport(socket), context=query.get("token", [""])[0])


async def main(port: int) -> None:
    async with serve(on_socket, "127.0.0.1", port) as listener:
        print(f"listening on port {listener.sockets[0].getsockname()[1]}", flush=True)
        await asyncio.get_running_loop().create_future()  # until interrupted


if __name__ == "__main__":
    asyncio.run(main(int(sys.argv[1]) if len(sys.argv) > 1 else 8765))
