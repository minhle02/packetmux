cmake_minimum_required(VERSION 3.23)
project(PacketMux
    VERSION 1.0.0
    LANGUAGES CXX
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BUILD_SHARED_LIBS "Build shared libraries instead of static" OFF)
option(PACKETMUX_BUILD_TESTS "Build the tests" ${PROJECT_IS_TOP_LEVEL})
option(PACKETMUX_NO_EXCEPTIONS "Build the library without exceptions and RTTI" ON)
option(PACKETMUX_WERROR "Treat warnings as errors" OFF)

add_library(packetmux
    src/Codec.cpp
    src/ByteReader.cpp
    src/ByteWriter.cpp
    src/capi/PacketMuxC.cpp
    src/session/EventQueue.cpp
    src/session/KeepAlive.cpp
    src/session/RoundRobinScheduler.cpp
    src/session/Session.cpp
    src/session/SessionProcessor.cpp
    src/session/SessionStateMachine.cpp
    src/session/state/ClosedState.cpp
    src/session/state/ClosingState.cpp
    src/session/state/DisconnectedState.cpp
    src/session/state/DrainingState.cpp
    src/session/state/EstablishedState.cpp
    src/session/state/HandshakingState.cpp
    src/stream/StreamManager.cpp
    src/stream/StreamProcessor.cpp
    src/stream/StreamStateMachine.cpp
    src/stream/state/HalfClosedLocalState.cpp
    src/stream/state/HalfClosedRemoteState.cpp
    src/stream/state/OpenState.cpp
    src/stream/state/ReceiverOpeningEofReceivedState.cpp
    src/stream/state/ReceiverOpeningState.cpp
    src/stream/state/SenderOpeningEofSentState.cpp
    src/stream/state/SenderOpeningState.cpp
    src/stream/state/StreamStateBase.cpp
)
add_library(packetmux::packetmux ALIAS packetmux)

target_compile_features(packetmux PUBLIC cxx_std_20)
set_target_properties(packetmux PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(packetmux
    PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    PRIVATE ${PROJECT_SOURCE_DIR}/src
)

if(BUILD_SHARED_LIBS)
    target_compile_definitions(packetmux
        PRIVATE PACKETMUX_BUILDING_DLL
        INTERFACE PACKETMUX_USING_DLL
    )
endif()

if(MSVC)
    target_compile_options(packetmux PRIVATE /W4 /permissive- $<$<BOOL:${PACKETMUX_WERROR}>:/WX>)
    if(PACKETMUX_NO_EXCEPTIONS)
        target_compile_options(packetmux PRIVATE /EHs-c- /GR-)
        target_compile_definitions(packetmux PRIVATE _HAS_EXCEPTIONS=0)
    endif()
else()
    target_compile_options(packetmux PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        $<$<BOOL:${PACKETMUX_WERROR}>:-Werror>
    )
    # Partial {.type = ...} initializers are deliberate: unset fields use their
    # defaults. Newer Clang warns about them; older compilers do not know the flag.
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-Wno-missing-designated-field-initializers
        PACKETMUX_HAS_NO_MISSING_DESIGNATED_WARNING)
    if(PACKETMUX_HAS_NO_MISSING_DESIGNATED_WARNING)
        target_compile_options(packetmux PRIVATE
            -Wno-missing-designated-field-initializers)
    endif()
    if(PACKETMUX_NO_EXCEPTIONS)
        target_compile_options(packetmux PRIVATE -fno-exceptions -fno-rtti)
    endif()
endif()

if(EMSCRIPTEN)
    # The module for bindings/js. The exported list comes from the C header, so a
    # new function cannot be forgotten.
    file(READ include/packetmux/PacketMuxC.h PACKETMUX_C_HEADER)
    string(REGEX MATCHALL "packetmux_(session|frame|config)_[a-z_]+\\("
        PACKETMUX_C_FUNCTIONS "${PACKETMUX_C_HEADER}")
    list(REMOVE_DUPLICATES PACKETMUX_C_FUNCTIONS)
    list(TRANSFORM PACKETMUX_C_FUNCTIONS REPLACE "\\($" "")
    list(TRANSFORM PACKETMUX_C_FUNCTIONS PREPEND "_")
    list(APPEND PACKETMUX_C_FUNCTIONS _malloc _free)
    list(JOIN PACKETMUX_C_FUNCTIONS "," PACKETMUX_EXPORTED_FUNCTIONS)

    add_executable(packetmux_wasm bindings/js/LayoutCheck.cpp)
    target_link_libraries(packetmux_wasm PRIVATE packetmux)
    set_target_properties(packetmux_wasm PROPERTIES
        OUTPUT_NAME packetmux
        SUFFIX .mjs
        RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bindings/js/dist
    )
    target_link_options(packetmux_wasm PRIVATE
        -Os -fno-exceptions -fno-rtti
        -sWASM_BIGINT -sMODULARIZE -sEXPORT_ES6 -sALLOW_MEMORY_GROWTH
        -sEXPORTED_FUNCTIONS=${PACKETMUX_EXPORTED_FUNCTIONS}
        -sEXPORTED_RUNTIME_METHODS=HEAPU8
    )
endif()

if(PACKETMUX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
