"""The Python and TypeScript SDKs against each other, in both directions, over a real WebSocket."""

import asyncio
import hashlib
import json
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import pytest
from packetmux import ClientOptions, IncomingStream, Reason, Server, connect, stream_type
from packetmux.testing import TRANSPORT_CHECKS, run_transport_check
from websockets.asyncio.client import connect as ws_connect
from websockets.asyncio.server import ServerConnection, serve

from ws_transport import WsTransport

SCRIPT = Path(__file__).resolve().parents[3] / "js" / "test" / "sdkInterop.js"
MAX_SIZE = 1 << 21


@stream_type("forward")
@dataclass
class Forward:
    host: str
    port: int


def pattern(size: int) -> bytes:
    return bytes((i * 31 + 7) % 251 for i in range(size))


async def echo(request: IncomingStream) -> None:
    stream = request.accept()
    async for chunk in stream:
        await stream.write(chunk)


async def forward(request: IncomingStream[Forward]) -> None:
    target = request.params
    await request.accept().write(f"{target.host}:{target.port}".encode())


def check_result(result: dict[str, Any]) -> None:
    assert result["first"]["sent"] == result["first"]["received"]
    assert result["afterReconnect"]["sent"] == result["afterReconnect"]["received"]
    assert result["typed"] == "h:7"
    assert result["lostCode"] == Reason.TRANSPORT_LOST
    assert result["state"] == "closed"


async def python_server_typescript_client() -> None:
    server = Server(handlers=[("echo", echo), (Forward, forward)])
    connections = 0

    async def on_socket(socket: ServerConnection) -> None:
        nonlocal connections
        connections += 1
        await server.serve(WsTransport(socket))

    async with serve(on_socket, "127.0.0.1", 0, max_size=MAX_SIZE) as listener:
        port = listener.sockets[0].getsockname()[1]
        node = await asyncio.create_subprocess_exec(
            "node", str(SCRIPT), "client", f"ws://127.0.0.1:{port}", stdout=subprocess.PIPE
        )
        output, _ = await asyncio.wait_for(node.communicate(), 60)
    assert node.returncode == 0
    check_result(json.loads(output))
    assert connections == 2  # one session, reconnected once
    assert server.sessions() == []


async def exchange(client: Any, size: int) -> None:
    stream = await client.open_stream("echo")
    data = pattern(size)
    reader = asyncio.create_task(read_all(stream))
    await stream.write(data)
    await stream.send_eof()
    assert hashlib.sha256(await reader).digest() == hashlib.sha256(data).digest()


async def read_all(stream: Any) -> bytes:
    received = bytearray()
    async for chunk in stream:
        received += chunk
    return bytes(received)


async def typescript_server_python_client() -> None:
    node = await asyncio.create_subprocess_exec(
        "node", str(SCRIPT), "server", stdout=subprocess.PIPE
    )
    try:
        port = int(await asyncio.wait_for(node.stdout.readline(), 30))
        transports: list[WsTransport] = []
        session_ids: list[str] = []

        async def dial(session_id: str) -> WsTransport:
            session_ids.append(session_id)
            socket = await ws_connect(f"ws://127.0.0.1:{port}", max_size=MAX_SIZE)
            transports.append(WsTransport(socket))
            return transports[-1]

        options = ClientOptions(reconnect_backoff=[0.01])
        async with await connect(dial, options=options) as client:
            await exchange(client, 3 << 20)

            typed = await client.open_stream(Forward(host="h", port=7))
            assert await typed.read() == b"h:7"

            lost = await client.open_stream("echo")
            await transports[-1].close()  # the connection dies
            assert await asyncio.wait_for(lost.wait_closed(), 10) == Reason.TRANSPORT_LOST
            while len(transports) < 2 or client.state.value != "connected":
                await asyncio.sleep(0.005)
            await exchange(client, 1 << 20)
        assert client.state.value == "closed"
        assert session_ids == [client.session_id] * 2
    finally:
        node.terminate()
        await node.wait()


def test_python_server_typescript_client() -> None:
    asyncio.run(asyncio.wait_for(python_server_typescript_client(), 90))


def test_typescript_server_python_client() -> None:
    asyncio.run(asyncio.wait_for(typescript_server_python_client(), 90))


async def socket_pair() -> tuple[WsTransport, WsTransport]:
    accepted: asyncio.Future = asyncio.get_running_loop().create_future()

    async def on_socket(socket: ServerConnection) -> None:
        accepted.set_result(socket)
        await socket.wait_closed()

    listener = await serve(on_socket, "127.0.0.1", 0, max_size=MAX_SIZE)
    port = listener.sockets[0].getsockname()[1]
    client = await ws_connect(f"ws://127.0.0.1:{port}", max_size=MAX_SIZE)
    return WsTransport(client), WsTransport(await accepted)


@pytest.mark.parametrize("name", TRANSPORT_CHECKS)
def test_transport_checks_over_a_websocket(name: str) -> None:
    asyncio.run(asyncio.wait_for(run_transport_check(name, socket_pair), 60))
