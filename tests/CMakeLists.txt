include(FetchContent)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(packetmux_tests
    ByteIoTest.cpp
    CodecTest.cpp
    GoldenVectorsTest.cpp
    KeepAliveTest.cpp
    RoundRobinSchedulerTest.cpp
    SessionBackpressureTest.cpp
    SessionLifecycleTest.cpp
    SessionPairTest.cpp
    SessionRobustnessTest.cpp
    SessionStreamTest.cpp
    StreamStateMachineTest.cpp
)
target_link_libraries(packetmux_tests PRIVATE packetmux::packetmux GTest::gtest_main)
target_include_directories(packetmux_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(packetmux_tests PRIVATE
    PACKETMUX_VECTORS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/vectors"
)

include(GoogleTest)
gtest_discover_tests(packetmux_tests)

add_test(NAME packetmux_header_check
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckHeaders.cmake
)
