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
    if(PACKETMUX_NO_EXCEPTIONS)
        target_compile_options(packetmux PRIVATE -fno-exceptions -fno-rtti)
    endif()
endif()

if(PACKETMUX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
