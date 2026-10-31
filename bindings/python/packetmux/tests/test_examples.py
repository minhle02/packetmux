"""Runs the example server and client as a consumer would, so the docs cannot go stale."""

import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXAMPLE = ROOT / "example"


def test_example_client_talks_to_the_example_server() -> None:
    environment = {**os.environ, "PYTHONPATH": str(ROOT), "PYTHONUNBUFFERED": "1"}
    server = subprocess.Popen(
        [sys.executable, str(EXAMPLE / "server.py"), "0"],
        env=environment,
        stdout=subprocess.PIPE,
        text=True,
    )
    try:
        port = re.search(r"port (\d+)", server.stdout.readline()).group(1)
        client = subprocess.run(
            [sys.executable, str(EXAMPLE / "client.py"), port],
            env=environment,
            capture_output=True,
            text=True,
            timeout=20,
        )
    finally:
        server.terminate()
        server.wait()
    assert client.returncode == 0, client.stderr
    assert "echo: hello" in client.stdout
    assert "greet: HELLO, ALICE" in client.stdout
