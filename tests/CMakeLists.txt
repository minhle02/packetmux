include(FetchContent)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(googletest nlohmann_json)

add_executable(packetmux_tests
    ByteIoTest.cpp
    CodecTest.cpp
    ConformanceTest.cpp
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
target_link_libraries(packetmux_tests PRIVATE
    packetmux::packetmux GTest::gtest_main nlohmann_json::nlohmann_json)
target_include_directories(packetmux_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(packetmux_tests PRIVATE
    PACKETMUX_CONFORMANCE_DIR="${PROJECT_SOURCE_DIR}/conformance"
)

# Compiled as C: it proves the public C header is valid C and drives the library
# through nothing but the C ABI.
enable_language(C)
add_executable(packetmux_c_api_test CApiAcceptanceTest.c)
target_link_libraries(packetmux_c_api_test PRIVATE packetmux::packetmux)
set_target_properties(packetmux_c_api_test PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    LINKER_LANGUAGE CXX
)
add_test(NAME packetmux_c_api_test COMMAND packetmux_c_api_test)

include(GoogleTest)
gtest_discover_tests(packetmux_tests)

add_test(NAME packetmux_header_check
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckHeaders.cmake
)
