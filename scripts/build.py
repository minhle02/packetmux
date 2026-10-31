#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Build PacketMux for one target.

    uv run scripts/build.py wasm
    uv run scripts/build.py macos --build-type Debug --clean
    uv run scripts/build.py wheel      # for this operating system
    uv run scripts/build.py npm

Native targets build the shared library that the Python binding loads, and must run
on that operating system. The wasm target needs Emscripten on PATH, and writes the
module that the JS binding imports. The wheel and npm targets package those builds
into build/packages.
"""

import argparse
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[1]

# Target -> the sys.platform it has to run on.
NATIVE_PLATFORMS = {"macos": "darwin", "linux": "linux", "windows": "win32"}
TARGETS = ["wasm", *NATIVE_PLATFORMS, "wheel", "npm"]
PACKAGES = REPOSITORY / "build" / "packages"
OLDEST_MACOS = "11_0"  # CMAKE_OSX_DEPLOYMENT_TARGET


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


def cached_build_type(build_dir: Path) -> str | None:
    cache = build_dir / "CMakeCache.txt"
    if not cache.exists():
        return None
    for line in cache.read_text().splitlines():
        if line.startswith("CMAKE_BUILD_TYPE:"):
            return line.partition("=")[2]
    return None


def build(target: str, build_type: str, clean: bool) -> list[Path]:
    required_platform = NATIVE_PLATFORMS.get(target)
    if required_platform is not None and sys.platform != required_platform:
        sys.exit(f"error: {target} must be built on {target}, not {sys.platform}")

    build_dir = REPOSITORY / "build" / target
    # Make compares whole-second timestamps on macOS, so objects built in the same second as
    # the new flags count as up to date, and the library ends up mixing two build types.
    # Visual Studio is multi-config and keeps each build type in its own directory.
    type_changed = target != "windows" and cached_build_type(build_dir) not in (None, build_type)
    if clean or type_changed:
        shutil.rmtree(build_dir, ignore_errors=True)

    try:
        run(configure_command(target, build_dir, build_type))
        run(["cmake", "--build", str(build_dir), "--config", build_type, "--parallel"])
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)
    return artifacts(target, build_dir, build_type)


def wheel_platform_tag() -> str:
    machine = platform.machine().lower()
    if sys.platform == "darwin":
        return f"macosx_{OLDEST_MACOS}_{machine}"
    if sys.platform == "win32":
        return "win_amd64"
    # CI builds in a manylinux container, and says so. A wheel built anywhere else stays linux_.
    return f"{os.environ.get('PACKETMUX_LINUX_TAG', 'linux')}_{machine}"


def build_wheel(build_type: str, clean: bool) -> None:
    """ctypes has no Python ABI dependency, so one wheel serves every Python from 3.12 up."""
    target = next(name for name, value in NATIVE_PLATFORMS.items() if value == sys.platform)
    library = build(target, build_type, clean)[0]
    environment = {
        **os.environ,
        "PACKETMUX_WHEEL_LIBRARY": str(library),
        "PACKETMUX_WHEEL_PLATFORM": wheel_platform_tag(),
    }
    command = [require_tool("uv"), "build", "--wheel", "--out-dir", str(PACKAGES)]
    print("+", " ".join(command), flush=True)
    subprocess.run(
        command, cwd=REPOSITORY / "bindings/python/packetmux", env=environment, check=True
    )


def build_npm(build_type: str, clean: bool) -> None:
    build("wasm", build_type, clean)
    PACKAGES.mkdir(parents=True, exist_ok=True)
    command = [require_tool("npm"), "pack", "--pack-destination", str(PACKAGES)]
    print("+", " ".join(command), flush=True)
    subprocess.run(command, cwd=REPOSITORY / "bindings/js", check=True)


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

    if args.target == "wheel":
        build_wheel(args.build_type, args.clean)
        return
    if args.target == "npm":
        build_npm(args.build_type, args.clean)
        return

    for path in build(args.target, args.build_type, args.clean):
        print(f"built: {path.relative_to(REPOSITORY)}")


if __name__ == "__main__":
    main()
