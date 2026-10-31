"""M10 on a real device: adb through a PacketMux tunnel, against adb on its own.

The bulk load is `adb pull` and `adb push`, so USB sets the speed. A persistent `adb shell -T cat` gives
the round trip a person feels, measured all the way to the device and back. The adb server stays on
its usual port, and the tunnel's local listener stands in for it through `adb -P`.

    PYTHONPATH=. PACKETMUX_LIBRARY=<release dylib> uv run python bench/adb_tunnel.py SERIAL out.jsonl

It writes one file to /data/local/tmp on the device, and removes it at the end.
"""

import argparse
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

import latency

BULK_MB = 200
REMOTE_PULL = "/data/local/tmp/packetmux_bench_pull.bin"
REMOTE_PUSH = "/data/local/tmp/packetmux_bench_push.bin"
TUNNEL_PORT = 18900
GAP_S = 0.005


class Adb:
    def __init__(self, serial: str, port: int | None) -> None:
        self.command = ["adb", *(["-P", str(port)] if port else []), "-s", serial]

    def run(self, *arguments: str) -> None:
        subprocess.run([*self.command, *arguments], check=True, capture_output=True, timeout=300)

    def shell_cat(self) -> subprocess.Popen:
        return subprocess.Popen(
            [*self.command, "shell", "-T", "cat"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, bufsize=0,
        )


class RoundTrips(threading.Thread):
    """Sends a line to `cat` on the device and waits for it to come back, until stopped."""

    def __init__(self, adb: Adb) -> None:
        super().__init__(daemon=True)
        self.process = adb.shell_cat()
        self.samples: list[float] = []
        self.halted = threading.Event()

    def run(self) -> None:
        line = b"x" * 63 + b"\n"
        while not self.halted.is_set():
            started = time.perf_counter()
            self.process.stdin.write(line)
            received = 0
            while received < len(line):
                received += len(self.process.stdout.read(len(line) - received))
            self.samples.append((time.perf_counter() - started) * 1000)
            time.sleep(GAP_S)

    def finish(self) -> None:
        self.halted.set()
        self.join(10)
        self.process.kill()


def measure(adb: Adb, name: str, bulk: list[str] | None, control: socket.socket | None) -> dict:
    ask(control, b"reset")
    pings = RoundTrips(adb)
    pings.start()
    time.sleep(0.5)
    pings.samples.clear()
    started = time.perf_counter()
    if bulk is None:
        time.sleep(4)
    else:
        adb.run(*bulk)
    seconds = time.perf_counter() - started
    samples = list(pings.samples)
    pings.finish()
    return {
        "setup": name,
        "seconds": seconds,
        "mb_per_s": 0.0 if bulk is None else BULK_MB / seconds,
        "samples": len(samples),
        "rtt_p50_ms": latency.percentile(samples, 50),
        "rtt_p99_ms": latency.percentile(samples, 99),
        **ask(control, b"report"),
    }


def ask(control: socket.socket | None, command: bytes) -> dict:
    if control is None:
        return {}
    control.sendall(command + b"\n")
    reply = bytearray()
    while not reply.endswith(b"\n"):
        reply += control.recv(65536)
    return json.loads(reply)


def start_tunnel(port: int) -> list[subprocess.Popen]:
    script = str(Path(latency.__file__).resolve())
    python = [sys.executable, script]
    common = ["--port", str(port), "--stack", "tunnel", "--wait", "poll"]
    server = subprocess.Popen([*python, "serve", *common])
    time.sleep(0.4)
    client = subprocess.Popen([*python, "tunnel-client", *common, "--forward-port", "5037"])
    time.sleep(0.4)
    return [server, client]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("serial")
    parser.add_argument("out")
    parser.add_argument("--rounds", type=int, default=3)
    args = parser.parse_args()

    direct = Adb(args.serial, None)
    direct.run("shell", f"dd if=/dev/urandom of={REMOTE_PULL} bs=1048576 count={BULK_MB}")
    with tempfile.TemporaryDirectory() as folder:
        pulled = os.path.join(folder, "pulled.bin")
        to_push = os.path.join(folder, "to_push.bin")
        with open(to_push, "wb") as file:
            for _ in range(BULK_MB):
                file.write(os.urandom(1 << 20))
        pull = ["pull", REMOTE_PULL, pulled]
        push = ["push", to_push, REMOTE_PUSH]
        try:
            with open(args.out, "a") as out:
                for index in range(args.rounds):
                    for tunnelled in (False, True):  # the versions alternate within a round
                        port = TUNNEL_PORT + index * 10
                        running = start_tunnel(port) if tunnelled else []
                        control = socket.create_connection(("127.0.0.1", port + 3)) if tunnelled else None
                        adb = Adb(args.serial, port + 2) if tunnelled else direct
                        prefix = "tunnel" if tunnelled else "direct"
                        try:
                            for name, bulk in (("idle", None), ("pull", pull), ("push", push)):
                                result = measure(adb, f"{prefix} {name}", bulk, control)
                                result["round"] = index
                                out.write(json.dumps(result) + "\n")
                                out.flush()
                                print(f"{index} {prefix} {name}: {result['mb_per_s']:.1f} MB/s, rtt p50 "
                                      f"{result['rtt_p50_ms']:.2f} p99 {result['rtt_p99_ms']:.2f} ms "
                                      f"({result['samples']} samples)", flush=True)
                        finally:
                            for process in running:
                                process.kill()
                                process.wait()
        finally:
            direct.run("shell", f"rm -f {REMOTE_PULL} {REMOTE_PUSH}")


if __name__ == "__main__":
    main()
