# M1 — Build skeleton and frame codec: design

Status: implemented. Exit criteria met on macOS arm64 with Apple Clang 17 (§11).
Requirements: `docs/requirements/INTENT.md` (FR-WIRE-\*, FR-FRAME-\*, NFR-PORT-1/2/4/5/7, NFR-MNT-\*).

## 1. Goal and boundaries

M1 delivers:
- a CMake library target `packetmux` and a GoogleTest test target;
- the byte-exact layout of every v2 frame;
- a stateless codec that encodes and decodes one frame per message;
- golden-vector files that every later binding re-runs.

Not in M1: stream ID parity, "HELLO first", stream state, scheduling and everything else that
needs session state (M2); the C ABI (M3); bindings and the WASM build (M4); the length-prefix
framer, fuzzing and performance work (M5).

## 2. Wire format

### 2.1 Conventions
- All integers are unsigned and **big-endian** (FR-WIRE-2).
- `u8`, `u16`, `u32`, `u64` are 1, 2, 4 and 8 bytes.
- `id16` = `u16` length + that many identifier characters (§2.5). Length MUST be ≥ 1.
- `bytes32` = `u32` length + that many opaque bytes. Length MAY be 0.
- A payload MUST end exactly after its last field. Bytes left over are an error (FR-WIRE-4a).

### 2.2 Header

```
offset  size  field
0       1     type        u8  (§2.3)
1       4     stream_id   u32
5       ...   payload     (rest of the message)
```
`HEADER_SIZE = 5`. There is no length field: one transport message is one frame.

### 2.3 Frame types

| Wire value | Name | stream_id | Payload |
|---|---|---|---|
| 0 | (`FRAME_TYPE_UNKNOWN`) | — | never valid on the wire |
| 1 | `HELLO` | 0 | `version u16 \| session_id id16 \| capabilities bytes32` |
| 2 | `OPEN` | ≠ 0 | `kind id16 \| params bytes32` |
| 3 | `OPEN_OK` | ≠ 0 | empty |
| 4 | `OPEN_ERR` | ≠ 0 | `code u8 \| message bytes32` |
| 5 | `DATA` | ≠ 0 | raw bytes, length 1 … `maxDataPayload` |
| 6 | `EOF` | ≠ 0 | empty |
| 7 | `RST` | ≠ 0 | `code u8` |
| 8 | `PING` | 0 | `nonce u64` |
| 9 | `PONG` | 0 | `nonce u64` |
| 10 | `GOAWAY` | 0 | `code u8` |

Size limits:
- `HELLO`, `OPEN` and `OPEN_ERR` payloads are each at most `maxControlPayload` (default 32 KiB).
- `DATA` is at most `maxDataPayload` (default 16 000).

**`HELLO.version` stays at payload offset 0 in every future protocol version.** A v2 peer can then
always read a v3 peer's version and reject it cleanly, even if the rest of the layout changes.

### 2.4 Reason codes

| Wire value | Name | Notes |
|---|---|---|
| 0 | `OK` | |
| 1 | `PEER_CLOSED` | |
| 2 | `CONNECT_FAILED` | |
| 3 | `POLICY_DENIED` | |
| 4 | `LIMIT_EXCEEDED` | |
| 5 | `TRANSPORT_LOST` | local only, never encoded (FR-FRAME-6) |
| 6 | `KILLED` | |
| 7 | `AUTH_REVOKED` | |
| 8 | `TIMEOUT` | |
| 9 | `PROTOCOL_ERROR` | |
| 10 | `UNSUPPORTED_KIND` | |

On decode, a code ≥ 11 (unknown) or equal to 5 (`TRANSPORT_LOST`) becomes `PROTOCOL_ERROR`. The
frame itself is still valid (FR-FRAME-5). On encode, `TRANSPORT_LOST` and values ≥ 11 are refused.

### 2.5 Identifiers

`kind` and `session_id` are identifiers (FR-WIRE-4b). Every byte MUST be one of the ASCII
characters `a-z`, `A-Z`, `0-9` or `-`. Spaces, `_`, `.`, NUL and every byte ≥ 0x80 are refused.

Both directions check this: the encoder refuses to produce such a frame, and the decoder rejects
it. No Unicode handling is needed, and bindings can always turn an identifier into a native
string.

### 2.6 Examples

```
HELLO   session "s1", no capabilities
        01 00000000 | 0002 | 0002 7331 | 00000000
OPEN    stream 1, kind "tcp-forward", params "{}"
        02 00000001 | 000b 7463702d666f7277617264 | 00000002 7b7d
OPEN_OK 03 00000001
OPEN_ERR stream 1, POLICY_DENIED, message "no"
        04 00000001 | 03 | 00000002 6e6f
DATA    stream 3, "hi"
        05 00000003 | 6869
EOF     06 00000003
RST     stream 3, KILLED
        07 00000003 | 06
PING    nonce 1
        08 00000000 | 0000000000000001
PONG    09 00000000 | 0000000000000001
GOAWAY  AUTH_REVOKED
        0a 00000000 | 07
```

## 3. Decoding rules and error order

`Codec::decode` applies these checks **in this order** and returns the first failure. A fixed order
makes the status deterministic for inputs with several faults, so golden vectors can assert it.

| # | Check | Status on failure |
|---|---|---|
| 1 | message ≥ 5 bytes | `TRUNCATED` |
| 2 | type is 1 … 10 | `UNKNOWN_TYPE` |
| 3 | stream_id is 0 for `HELLO`/`PING`/`PONG`/`GOAWAY`, ≠ 0 otherwise | `BAD_STREAM_ID` |
| 4 | payload ≤ its limit (`DATA`: `maxDataPayload`; `HELLO`/`OPEN`/`OPEN_ERR`: `maxControlPayload`) | `PAYLOAD_TOO_LARGE` |
| 5 | fixed sizes: `OPEN_OK`/`EOF` = 0, `RST`/`GOAWAY` = 1, `PING`/`PONG` = 8, `DATA` ≥ 1 | `BAD_PAYLOAD_SIZE` |
| 6 | fields, left to right. `HELLO`: version is read first and must be 2 | `UNSUPPORTED_VERSION` |
| 7 | a field or length prefix runs past the end | `TRUNCATED` |
| 8 | an `id16` is empty or has a character outside §2.5 | `BAD_IDENTIFIER` |
| 9 | bytes left after the last field | `BAD_PAYLOAD_SIZE` |

Steps 6–9 run as one left-to-right pass: the first field that fails decides the status.

Lengths are compared with the bytes remaining (`length > remaining`). They are never added to an
offset, so a hostile `u32` length cannot overflow `size_t`, including on wasm32.

## 4. Public API (`include/packetmux/`)

### 4.1 `definitions/ProtocolDef.h`
These constants are shared by several modules, so they get their own header (STYLE.md).

```cpp
namespace packetmux {
constexpr uint16_t PROTOCOL_VERSION = 2U;
constexpr size_t HEADER_SIZE = 5U;
constexpr uint32_t CONNECTION_STREAM_ID = 0U;
constexpr size_t DEFAULT_MAX_DATA_PAYLOAD = 16000U;
constexpr size_t DEFAULT_MAX_CONTROL_PAYLOAD = 32U * 1024U;
}  // namespace packetmux
```

### 4.2 `Frames.h`
The wire enums and plain data types. This replaces the current `Frame` class, which owned a copy
of the payload and could only report a decode error by throwing.

```cpp
namespace packetmux {

enum FrameType : uint8_t {
  FRAME_TYPE_UNKNOWN = 0U,
  FRAME_TYPE_HELLO,
  FRAME_TYPE_OPEN,
  FRAME_TYPE_OPEN_OK,
  FRAME_TYPE_OPEN_ERR,
  FRAME_TYPE_DATA,
  FRAME_TYPE_EOF,
  FRAME_TYPE_RST,
  FRAME_TYPE_PING,
  FRAME_TYPE_PONG,
  FRAME_TYPE_GOAWAY,
  FRAME_TYPE_MAX,
};

enum ReasonCode : uint8_t {
  REASON_CODE_OK = 0U,
  REASON_CODE_PEER_CLOSED,
  REASON_CODE_CONNECT_FAILED,
  REASON_CODE_POLICY_DENIED,
  REASON_CODE_LIMIT_EXCEEDED,
  REASON_CODE_TRANSPORT_LOST,
  REASON_CODE_KILLED,
  REASON_CODE_AUTH_REVOKED,
  REASON_CODE_TIMEOUT,
  REASON_CODE_PROTOCOL_ERROR,
  REASON_CODE_UNSUPPORTED_KIND,
  REASON_CODE_MAX,
};

enum CodecStatus : uint8_t {
  CODEC_STATUS_OK = 0U,
  CODEC_STATUS_TRUNCATED,
  CODEC_STATUS_UNKNOWN_TYPE,
  CODEC_STATUS_BAD_STREAM_ID,
  CODEC_STATUS_PAYLOAD_TOO_LARGE,
  CODEC_STATUS_BAD_PAYLOAD_SIZE,
  CODEC_STATUS_UNSUPPORTED_VERSION,
  CODEC_STATUS_BAD_IDENTIFIER,
  CODEC_STATUS_INVALID_ARGUMENT,  // encode only: e.g. a reason code that may not be sent
  CODEC_STATUS_MAX,
};

struct FrameLimits {
  size_t maxDataPayload = DEFAULT_MAX_DATA_PAYLOAD;
  size_t maxControlPayload = DEFAULT_MAX_CONTROL_PAYLOAD;
};

// One frame, as plain fields. Spans and string views BORROW memory: after Codec::decode they
// point into the decoded message; for Codec::encode they point to the caller's data.
// Only the fields of the frame's type are used; the encoder ignores the others.
struct FrameView {
  FrameType type = FRAME_TYPE_UNKNOWN;
  uint32_t streamId = CONNECTION_STREAM_ID;
  uint16_t version = PROTOCOL_VERSION;    // HELLO
  std::string_view sessionId;             // HELLO
  std::span<const uint8_t> capabilities;  // HELLO
  std::string_view kind;                  // OPEN
  std::span<const uint8_t> params;        // OPEN
  ReasonCode code = REASON_CODE_OK;       // OPEN_ERR, RST, GOAWAY
  std::span<const uint8_t> message;       // OPEN_ERR
  std::span<const uint8_t> data;          // DATA
  uint64_t nonce = 0U;                    // PING, PONG
};

}  // namespace packetmux
```

### 4.3 `Codec.h`
The codec has no state, so `Codec` only has static methods. A call site reads `Codec::decode`,
which shows where the code lives.

```cpp
namespace packetmux {

class Codec {
 public:
  Codec() = delete;

  static CodecStatus decode(const std::span<const uint8_t> i_message,
                            const FrameLimits& i_limits, FrameView* const o_frame);

  static CodecStatus encode(const FrameView& i_frame, const FrameLimits& i_limits,
                            std::vector<uint8_t>* const o_message);
};

}  // namespace packetmux
```

Contract:
- **`Codec::decode`**
  - It first resets `*o_frame` to a default `FrameView`. It never allocates, and it never reads
    outside `i_message`.
  - On success, every field for the type is set. The views stay valid while `i_message` does.
  - On failure, `type` and `streamId` are meaningful only if the header was read (step ≥ 2).
    `version` is meaningful for `UNSUPPORTED_VERSION`.
- **`Codec::encode`**
  - It **appends** one complete message to `*o_message`. Appending lets M2 encode straight into
    its send queue (NFR-PERF-2), and lets M5 put a length prefix in front.
  - It applies the same rules as decode: stream_id rule, limits, sizes, identifiers, `version == 2`,
    and sendable reason codes.
  - On failure, `*o_message` is left **unchanged**. Every field is validated before anything is
    written.
  - The views in `i_frame` MUST NOT point into `*o_message`, because appending can reallocate it.
  - For any `FrameView` that decodes successfully and has no remapped reason code:
    `Codec::encode(Codec::decode(m)) == m`.
- Neither function throws, keeps state, or touches globals (FR-ERR-3, FR-API-6).

Usage, with C++20 designated initializers:

```cpp
std::vector<uint8_t> out;
const packetmux::CodecStatus status = packetmux::Codec::encode(
    {.type = packetmux::FRAME_TYPE_DATA, .streamId = 3U, .data = payload}, limits, &out);
```

## 5. Internal modules (`src/`)

| File | Content |
|---|---|
| `Codec.cpp` | `Codec::decode`, `Codec::encode`, and the identifier check as a file-local helper |
| `ByteReader.h/.cpp` | `ByteReader`: bounds-checked big-endian cursor |
| `ByteWriter.h/.cpp` | `ByteWriter`: static big-endian `append*` methods |

Byte order uses shifts only. There is no `memcpy` + `htonl`, no `std::byteswap`, and no check of
the host's byte order (NFR-PORT-1, FR-WIRE-3):

```cpp
void ByteWriter::appendU32(const uint32_t i_value, std::vector<uint8_t>* const o_out) {
  o_out->push_back(static_cast<uint8_t>(i_value >> 24U));
  o_out->push_back(static_cast<uint8_t>(i_value >> 16U));
  o_out->push_back(static_cast<uint8_t>(i_value >> 8U));
  o_out->push_back(static_cast<uint8_t>(i_value));
}

class ByteReader {
 public:
  explicit ByteReader(const std::span<const uint8_t> i_data);
  bool readU8(uint8_t* const o_value);
  bool readU16(uint16_t* const o_value);
  bool readU32(uint32_t* const o_value);
  bool readU64(uint64_t* const o_value);
  bool readBytes(const size_t i_length, std::span<const uint8_t>* const o_bytes);
  size_t remaining() const;

 private:
  std::span<const uint8_t> m_data;
  size_t m_offset;
};
```
Each `read*` returns `false` and leaves the cursor unchanged if not enough bytes remain.

## 6. Build

### 6.1 Layout after M1
```
CMakeLists.txt
include/packetmux/Frames.h
include/packetmux/Codec.h
include/packetmux/definitions/ProtocolDef.h
src/Codec.cpp  src/ByteReader.h  src/ByteReader.cpp  src/ByteWriter.h  src/ByteWriter.cpp
tests/CMakeLists.txt
tests/ByteIoTest.cpp  tests/CodecTest.cpp  tests/GoldenVectorsTest.cpp
tests/CheckHeaders.cmake
tests/vectors/FramesV2.txt
```
`src/main.cpp` (hello world) is removed. So is the reference to the missing `src/my_lib.cpp`.

### 6.2 Top-level `CMakeLists.txt`
```cmake
cmake_minimum_required(VERSION 3.23)
project(PacketMux VERSION 1.0.0 LANGUAGES CXX)

option(BUILD_SHARED_LIBS "Build shared libraries instead of static" OFF)
option(PACKETMUX_BUILD_TESTS "Build the tests" ${PROJECT_IS_TOP_LEVEL})
option(PACKETMUX_NO_EXCEPTIONS "Build the library without exceptions and RTTI" ON)
option(PACKETMUX_WERROR "Treat warnings as errors" OFF)

add_library(packetmux src/Codec.cpp src/ByteReader.cpp src/ByteWriter.cpp)
add_library(packetmux::packetmux ALIAS packetmux)
target_compile_features(packetmux PUBLIC cxx_std_20)
set_target_properties(packetmux PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(packetmux
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  PRIVATE ${PROJECT_SOURCE_DIR}/src)

if(MSVC)
  target_compile_options(packetmux PRIVATE /W4 /permissive- $<$<BOOL:${PACKETMUX_WERROR}>:/WX>)
  if(PACKETMUX_NO_EXCEPTIONS)
    target_compile_options(packetmux PRIVATE /EHs-c- /GR-)
    target_compile_definitions(packetmux PRIVATE _HAS_EXCEPTIONS=0)
  endif()
else()
  target_compile_options(packetmux PRIVATE -Wall -Wextra -Wpedantic -Wconversion
    -Wsign-conversion $<$<BOOL:${PACKETMUX_WERROR}>:-Werror>)
  if(PACKETMUX_NO_EXCEPTIONS)
    target_compile_options(packetmux PRIVATE -fno-exceptions -fno-rtti)
  endif()
endif()

if(PACKETMUX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
```
- **Static by default:** until M3 adds an export macro, a shared build on Windows would export
  nothing.
- **No exceptions or RTTI:** only the library target is built this way. The tests are built
  normally, because GoogleTest needs exceptions, and they link against the no-exceptions library
  (NFR-PORT-5).

### 6.3 `tests/CMakeLists.txt`
```cmake
include(FetchContent)
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(packetmux_tests ByteIoTest.cpp CodecTest.cpp GoldenVectorsTest.cpp)
target_link_libraries(packetmux_tests PRIVATE packetmux::packetmux GTest::gtest_main)
target_include_directories(packetmux_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(packetmux_tests PRIVATE
  PACKETMUX_VECTORS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/vectors")

include(GoogleTest)
gtest_discover_tests(packetmux_tests)

add_test(NAME packetmux_header_check
  COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckHeaders.cmake)
```
`CheckHeaders.cmake` fails if `include/` or `src/` contains any of:
- a forbidden include: `<thread>`, `<mutex>`, `<atomic>`, `<sys/`, `<arpa/`, `<netinet/`,
  `<windows.h>`, `<winsock`;
- the text `_clock::now`.

This turns NFR-PORT-1/4 into a test that runs on every build.

## 7. Golden vectors

`tests/vectors/FramesV2.txt` is language-neutral. C++ reads it in M1; the Python and JS bindings
re-run it in M4. It is a simple INI-like format, so any language can parse it in a few lines:

```
# Comment lines start with '#'. Hex values may contain spaces.
[hello_basic]
wire = 01 00000000 0002 0002 7331 00000000
type = HELLO
stream_id = 0
version = 2
session_id = 7331

[rst_unknown_code]
wire = 07 00000003 ff
type = RST
stream_id = 3
code = PROTOCOL_ERROR
roundtrip = false

[open_kind_bad_char]
wire = 02 00000001 0003 615f62 00000000
status = BAD_IDENTIFIER
```

| Key | Meaning |
|---|---|
| `wire` | The full message, in hex |
| `status` | Expected `CodecStatus` name without prefix. Default `OK` |
| `type`, `code` | Enum names without prefix |
| `stream_id`, `version`, `nonce` | Decimal |
| `session_id`, `kind`, `capabilities`, `params`, `message`, `data` | Hex (so invalid identifiers can be written). Omitted = empty |
| `max_data_payload`, `max_control_payload` | Decimal. `FrameLimits` for this vector, so the size limits can be tested without huge vectors. Omitted = default |
| `roundtrip` | Default `true`: re-encoding the decoded fields must reproduce `wire` exactly. Set to `false` when decoding remaps a value |

For `status = OK` vectors, the test checks decode → expected fields and, when `roundtrip` is true,
also encode → `wire`. For error vectors it checks decode → `status` only.

Minimum set:
- **Valid:** one vector per frame type (the §2.6 examples), plus empty `capabilities`, `params`
  and `message`, a `kind` using every allowed character class, and `nonce = 2^64 − 1`.
- **Remapped:** reason code `ff` and reason code `05` (`TRANSPORT_LOST`), both decoding to
  `PROTOCOL_ERROR`.
- **Rejected:** one vector per row of §3:
  - empty input and 4 bytes;
  - type `00` and type `0b`;
  - `HELLO` on stream 1 and `DATA` on stream 0;
  - `OPEN_OK` with 1 byte, `RST` with 0 and 2 bytes, `PING` with 7 bytes, empty `DATA`;
  - `HELLO` version 1 and version 3;
  - a `bytes32` length past the end;
  - an empty `kind`, and `kind` or `session_id` containing `_`, a space, NUL or a byte ≥ 0x80;
  - one byte left after the last field.

## 8. Unit tests (beyond the vectors)

| File | Covers |
|---|---|
| `ByteIoTest.cpp` | Known big-endian bytes for each width (`0x0102 → 01 02`, …); reads past the end fail and leave the cursor unchanged |
| `CodecTest.cpp` | Encode/decode round trip for every type; custom `FrameLimits` (payload exactly at the limit and one byte over); identifier rules (§2.5) in both directions; encode refuses `TRANSPORT_LOST`, `REASON_CODE_MAX` and `version != 2`; encode failure leaves output unchanged; encode appends to existing content; a decoded view points into the input buffer (no copy) |

## 9. Traceability

| Requirement | Where |
|---|---|
| FR-WIRE-1/2/3 | §2.2, §5 (shift-based I/O), `ByteIoTest` |
| FR-WIRE-4/4a/4b/5 | §2.1, §2.3, §2.5 |
| FR-WIRE-6/8 | §2.3, §2.4 tables |
| FR-FRAME-1…4 | §3, rejection vectors |
| FR-FRAME-5/6 | §2.4, remap vectors, `CodecTest` |
| FR-ERR-3 | §4.3 contract (status codes, no exceptions) |
| NFR-PORT-1/4 | `CheckHeaders.cmake` |
| NFR-PORT-2/5/7 | §6.2, §6.3 |
| NFR-SEC-1 | §3 bounds rules; fuzzing follows in M5 |

## 10. Decisions from review

1. Plain data structs have no `m_` prefix (STYLE.md, Struct).
2. Source, test and vector files are PascalCase (STYLE.md, File).
3. The codec is a class `Codec` with static methods, not free functions (STYLE.md, Class).
4. Empty `DATA` is a protocol error.
5. `kind` and `session_id` are ASCII `[a-zA-Z0-9-]`, checked by encoder and decoder (FR-WIRE-4b).
6. `BUILD_SHARED_LIBS` defaults to `OFF` until M3 (§6.2).

## 11. Verification results (2026-10-30)

Platform: macOS arm64, Apple Clang 17, with `PACKETMUX_NO_EXCEPTIONS=ON`.

| Check | Result |
|---|---|
| Debug and Release builds with `-Werror` | Clean |
| CTest: 31 GoogleTest tests plus the header check, 56 golden vectors | All pass |
| UBSan plus libc++ hardening (bounds-checked `std::span`) | All pass |
| One-off stress run: 5 million random and mutated messages under the same flags | No crash; every one of the 1 439 744 decoded frames without a remapped code re-encoded byte for byte |
| Header check given a banned include; vectors given a wrong byte or status | Each one fails, as it should |

Not done:
- **AddressSanitizer:** on this machine even an empty `main()` hangs at startup under ASan, so it
  could not be run. NFR-SEC-4 stays open for M5, on a host where ASan works.
- **Other targets:** GCC, MSVC, Linux, Windows and Emscripten (not installed) are unbuilt
  (NFR-PORT-3). The code only uses standard C++20, but that has not been tested on them.
