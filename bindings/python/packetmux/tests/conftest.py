"""Builds the shared library when PACKETMUX_LIBRARY is not set, so pytest just works."""

import os
import subprocess
import sys
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[4]


def _build_library() -> Path:
    build = REPOSITORY / "build" / "python-shared"
    subprocess.run(
        [
            "cmake", "-S", str(REPOSITORY), "-B", str(build),
            "-DBUILD_SHARED_LIBS=ON", "-DPACKETMUX_BUILD_TESTS=OFF",
            "-DCMAKE_BUILD_TYPE=Release",
        ],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    subprocess.run(
        ["cmake", "--build", str(build), "--config", "Release", "-j8"],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    if sys.platform == "win32":
        return build / "Release" / "packetmux.dll"  # Visual Studio is multi-config
    suffix = ".dylib" if sys.platform == "darwin" else ".so"
    return build / f"libpacketmux{suffix}"


if "PACKETMUX_LIBRARY" not in os.environ:
    os.environ["PACKETMUX_LIBRARY"] = str(_build_library())
