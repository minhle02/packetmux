"""Puts the native library in the wheel, and tags the wheel for that platform.

scripts/build.py sets the two variables. A wheel with no library in it would not work, so
building without them is an error.
"""

import os
from pathlib import Path
from typing import Any

from hatchling.builders.hooks.plugin.interface import BuildHookInterface


class CustomBuildHook(BuildHookInterface):
    def initialize(self, version: str, build_data: dict[str, Any]) -> None:
        library = os.environ.get("PACKETMUX_WHEEL_LIBRARY")
        platform = os.environ.get("PACKETMUX_WHEEL_PLATFORM")
        if not library or not platform:
            raise RuntimeError("build the wheel with: uv run scripts/build.py wheel")
        build_data["pure_python"] = False
        build_data["tag"] = f"py3-none-{platform}"
        build_data["force_include"][library] = f"packetmux/core/{Path(library).name}"
