# piccolino: the self-hosted Multitap file server. One headless binary: a
# Multitap host that offers SHARE (file transfer) and SETTINGS and is never a
# client. piccolo and piccoletto pair with it on the LAN and copy games,
# saves, states, screenshots and recordings to and from its folders.
#
# Phase 1 (this build): the headless core. No SDL, ImGui, Vulkan,
# frontends-common or libdatachannel; the admin page, the tray, the beacon
# and WebRTC come in later phases.
#
#   git submodule update --init --checkout
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
#   cmake --build build && ctest --test-dir build

cmake_minimum_required(VERSION 3.25)
project(piccolino LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MULTITAP_WARNINGS_AS_ERRORS "Treat warnings in our own code as errors" OFF)
option(MULTITAP_SANITIZE           "Build our own code with ASan + UBSan"      OFF)
option(MULTITAP_BUILD_TESTS        "Build tests and register CTest tests"      ON)
# The end-to-end test's client: frontends-common's net_probe, built elsewhere
# (it needs SDL, ImGui and Vulkan, which piccolino doesn't link). Empty = the
# end-to-end test isn't registered.
set(PICCOLINO_NET_PROBE "" CACHE FILEPATH "net_probe binary for the end-to-end test")

set(PICCOLINO_VERSION_BASE "0.1.0")

foreach(_dep common/hosts-common/CMakeLists.txt common/multitap-net/CMakeLists.txt enet/include/enet.h
             monocypher/src/monocypher.c beacon/wire/beacon_wire.cpp)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/deps/${_dep}")
        message(FATAL_ERROR "deps/${_dep} is missing. Run: git submodule update --init --checkout")
    endif()
endforeach()

if(MULTITAP_BUILD_TESTS)
    enable_testing()
endif()

# The libraries, hosts-common first (it brings multitap_set_project_flags).
# Their own tests run in their own CI and in piccolo's: off here, so ctest
# runs piccolino's (and the beacon server isn't built).
set(MULTITAP_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps")
set(PICCOLINO_BUILD_TESTS ${MULTITAP_BUILD_TESTS})
set(MULTITAP_BUILD_TESTS OFF)
add_subdirectory(deps/common/hosts-common)
add_subdirectory(deps/common/multitap-net)
set(MULTITAP_BUILD_TESTS ${PICCOLINO_BUILD_TESTS})

multitap_version(PICCOLINO_VERSION ${PICCOLINO_VERSION_BASE} "${CMAKE_CURRENT_SOURCE_DIR}")
set(PICCOLINO_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
multitap_write_version_header("${PICCOLINO_GENERATED}" piccolino PICCOLINO_VERSION "${PICCOLINO_VERSION}")

# Everything but main(), so the tests link the same code.
add_library(piccolino_core STATIC
    src/piccolino_config.cpp
    src/piccolino_lan.cpp
    src/piccolino_server.cpp
)
target_include_directories(piccolino_core PUBLIC src "${PICCOLINO_GENERATED}")
target_link_libraries(piccolino_core PUBLIC multitap_net host_common)
multitap_set_project_flags(piccolino_core)

add_executable(piccolino src/main.cpp)
target_link_libraries(piccolino PRIVATE piccolino_core)
multitap_set_project_flags(piccolino)

if(MULTITAP_BUILD_TESTS)
    set(PICCOLINO_TEST_FAIL "FAIL|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")

    # piccolino.cfg: defaults, first run, reading back, bad values.
    add_executable(piccolino_config_test tests/piccolino_config_test.cpp)
    target_link_libraries(piccolino_config_test PRIVATE piccolino_core)
    multitap_set_project_flags(piccolino_config_test)
    add_test(NAME piccolino_config
             COMMAND piccolino_config_test "${CMAKE_CURRENT_BINARY_DIR}/piccolino_config_test")
    set_tests_properties(piccolino_config PROPERTIES
                         PASS_REGULAR_EXPRESSION "piccolino config: all checks passed"
                         FAIL_REGULAR_EXPRESSION "${PICCOLINO_TEST_FAIL}")

    # Which source addresses count as the LAN (pairing is answered only from them).
    add_executable(piccolino_lan_test tests/piccolino_lan_test.cpp)
    target_link_libraries(piccolino_lan_test PRIVATE piccolino_core)
    multitap_set_project_flags(piccolino_lan_test)
    add_test(NAME piccolino_lan COMMAND piccolino_lan_test)
    set_tests_properties(piccolino_lan PROPERTIES
                         PASS_REGULAR_EXPRESSION "piccolino lan: all checks passed"
                         FAIL_REGULAR_EXPRESSION "${PICCOLINO_TEST_FAIL}")

    # The server in one process, with multitap-net's own client over
    # loopback: discovery v2, pairing with the code, the grants from the
    # config, a listing, a read-only refusal.
    add_executable(piccolino_server_test tests/piccolino_server_test.cpp)
    target_link_libraries(piccolino_server_test PRIVATE piccolino_core)
    multitap_set_project_flags(piccolino_server_test)
    add_test(NAME piccolino_server
             COMMAND piccolino_server_test "${CMAKE_CURRENT_BINARY_DIR}/piccolino_server_test")
    set_tests_properties(piccolino_server PROPERTIES
                         PASS_REGULAR_EXPRESSION "piccolino server: all checks passed"
                         FAIL_REGULAR_EXPRESSION "${PICCOLINO_TEST_FAIL}"
                         TIMEOUT 120)

    # End to end: the piccolino binary, net_probe as the client (pairs with
    # the code piccolino prints, then copies files both ways).
    if(PICCOLINO_NET_PROBE)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_test(NAME piccolino_e2e
                 COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e_net_probe.py"
                         $<TARGET_FILE:piccolino> "${PICCOLINO_NET_PROBE}" "${CMAKE_CURRENT_BINARY_DIR}/e2e")
        set_tests_properties(piccolino_e2e PROPERTIES
                             PASS_REGULAR_EXPRESSION "piccolino e2e: all checks passed"
                             FAIL_REGULAR_EXPRESSION "${PICCOLINO_TEST_FAIL}"
                             TIMEOUT 300)
    endif()
endif()
