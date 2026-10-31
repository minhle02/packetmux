# PacketMux

Many byte streams over one connection, with a session that survives reconnects.

One C++ library holds the protocol, with no dependencies beyond the C++ standard library. Each language
has an async SDK on top of it, so an application implements a `Transport` over its own connection, and
the SDK does the rest.

| Language | How to integrate |
|---|---|
| Python | [bindings/python/packetmux/README.md](bindings/python/packetmux/README.md) |
| TypeScript | [bindings/js/README.md](bindings/js/README.md) |

Each binding has a complete client and server in its `example/` folder.

## How to build

You need:
- CMake 3.23 or newer, and a C++20 compiler
- [uv](https://docs.astral.sh/uv/) to run the build script
- [Emscripten](https://emscripten.org/) on your `PATH`, for the `wasm` target

Build for one target:

```sh
uv run scripts/build.py wasm      # module for TypeScript / JS
uv run scripts/build.py macos     # shared library for Python
uv run scripts/build.py linux
uv run scripts/build.py windows
uv run scripts/build.py wheel     # Python package, for this operating system
uv run scripts/build.py npm       # TypeScript package
```

| Target | Output |
|---|---|
| `wasm` | `bindings/js/dist/packetmux.mjs`, `packetmux.wasm` |
| `macos` | `build/macos/libpacketmux.dylib` |
| `linux` | `build/linux/libpacketmux.so` |
| `windows` | `build/windows/<build type>/packetmux.dll` |
| `wheel` | `build/packages/packetmux-<version>-py3-none-<platform>.whl` |
| `npm` | `build/packages/packetmux-<version>.tgz` |

Options: `--build-type Debug|Release|RelWithDebInfo|MinSizeRel` (default `Release`) and `--clean`.

`macos`, `linux` and `windows` must each be built on that operating system. So far only `macos` and
`wasm` have been tested; `linux` and `windows` have not been built yet.

To run the tests:

```sh
# C++, including the conformance suite
cmake -S . -B build/tests
cmake --build build/tests --parallel
ctest --test-dir build/tests

# Python, including real WebSocket tests against JS (build the wasm target first, and install JS)
(cd bindings/python/packetmux && uv run pytest)

# JS (build the wasm target first)
(cd bindings/js && npm install && npm test)
```

## How to install a package

The packages are built locally for now. A wheel contains native code, so it is built per platform.

```sh
pip install build/packages/packetmux-0.2.0-py3-none-macosx_11_0_arm64.whl
npm install build/packages/packetmux-0.2.0.tgz
```

In a `pyproject.toml`:

```toml
[project]
dependencies = ["packetmux @ file:///path/to/packetmux-0.2.0-py3-none-macosx_11_0_arm64.whl"]
```

To run a binding from this repository instead, as the tests do, build its `macos` or `wasm` target. The
Python binding then needs both of these:

```sh
export PYTHONPATH=$PWD/bindings/python/packetmux
export PACKETMUX_LIBRARY=$PWD/build/macos/libpacketmux.dylib
```

## How to add a language

Start with [docs/spec/BINDING.md](docs/spec/BINDING.md). [docs/spec/PROTOCOL.md](docs/spec/PROTOCOL.md)
defines the protocol, and [conformance/](conformance/README.md) tests it.
