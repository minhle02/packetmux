#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Build PacketMux for one target.

    uv run scripts/build.py wasm
    uv run scripts/build.py macos --build-type Debug --clean

Native targets build the shared library that the Python binding loads, and must run
on that operating system. The wasm target needs Emscripten on PATH, and writes the
module that the JS binding imports.
"""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[1]

# Target -> the sys.platform it has to run on.
NATIVE_PLATFORMS = {"macos": "darwin", "linux": "linux", "windows": "win32"}
TARGETS = ["wasm", *NATIVE_PLATFORMS]


def run(command: list[str]) -> None:
    print("+", " ".join(command), flush=True)
    subprocess.run(command, cwd=REPOSITORY, check=True)


def require_tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        sys.exit(f"error: {name} not found on PATH")
    return path


def configure_command(target: str, build_dir: Path, build_type: str) -> list[str]:
    command = [
        require_tool("cmake"),
        "-S", str(REPOSITORY),
        "-B", str(build_dir),
        f"-DCMAKE_BUILD_TYPE={build_type}",
        "-DPACKETMUX_BUILD_TESTS=OFF",
    ]
    if target == "wasm":
        return [require_tool("emcmake"), *command]
    return [*command, "-DBUILD_SHARED_LIBS=ON"]


def artifacts(target: str, build_dir: Path, build_type: str) -> list[Path]:
    if target == "wasm":
        dist = REPOSITORY / "bindings" / "js" / "dist"
        return [dist / "packetmux.mjs", dist / "packetmux.wasm"]
    if target == "macos":
        return [build_dir / "libpacketmux.dylib"]
    if target == "linux":
        return [build_dir / "libpacketmux.so"]
    return [build_dir / build_type / "packetmux.dll"]  # Visual Studio is multi-config


def main() -> None:
    parser = argparse.ArgumentParser(description="Build PacketMux for one target.")
    parser.add_argument("target", choices=TARGETS)
    parser.add_argument(
        "--build-type",
        default="Release",
        choices=["Debug", "Release", "RelWithDebInfo", "MinSizeRel"],
    )
    parser.add_argument("--clean", action="store_true", help="delete the build directory first")
    args = parser.parse_args()

    required_platform = NATIVE_PLATFORMS.get(args.target)
    if required_platform is not None and sys.platform != required_platform:
        sys.exit(f"error: {args.target} must be built on {args.target}, not {sys.platform}")

    build_dir = REPOSITORY / "build" / args.target
    if args.clean:
        shutil.rmtree(build_dir, ignore_errors=True)

    try:
        run(configure_command(args.target, build_dir, args.build_type))
        run(["cmake", "--build", str(build_dir), "--config", args.build_type, "--parallel"])
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)

    for path in artifacts(args.target, build_dir, args.build_type):
        print(f"built: {path.relative_to(REPOSITORY)}")


if __name__ == "__main__":
    main()
