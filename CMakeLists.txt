cmake_minimum_required(VERSION 3.16)
project(xmr_rebuild LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(XMR_BUILD_TESTS "Build unit tests and the mock Stratum pool" ON)
set(RANDOMX_TAG "v2.0.1" CACHE STRING "RandomX git tag used by FetchContent")

# ---------------------------------------------------------------------------
# RandomX VM core.
# XMRig itself does not re-implement the RandomX VM from scratch: it vendors a
# tuned fork of tevador's reference library. We do the same, but consume the
# upstream library so the build stays reproducible.
# ---------------------------------------------------------------------------
include(FetchContent)
set(RANDOMX_BUILD_TESTS OFF CACHE BOOL "Build RandomX tests" FORCE)
FetchContent_Declare(randomx
    GIT_REPOSITORY https://github.com/tevador/RandomX.git
    GIT_TAG        ${RANDOMX_TAG}
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(randomx)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

set(XMR_SOURCES
    src/json.cpp
    src/config.cpp
    src/memory_manager.cpp
    src/hardware_optimizer.cpp
    src/msr_tuner.cpp
    src/randomx_engine.cpp
    src/stratum.cpp
    src/miner.cpp)

add_library(xmr_core STATIC ${XMR_SOURCES})
# The upstream randomx CMake target does not publish its include directory.
target_include_directories(xmr_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/xmr
    ${randomx_SOURCE_DIR}/src)
target_link_libraries(xmr_core PUBLIC randomx Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
target_compile_definitions(xmr_core PUBLIC XMR_HAS_TLS=1)
message(STATUS "xmr_rebuild: OpenSSL ${OPENSSL_VERSION}")

# RANDOMX_FLAG_V2 is an enum constant (not a macro), so detect it from the
# fetched header instead of using #ifdef in C++.
file(READ "${randomx_SOURCE_DIR}/src/randomx.h" XMR_RANDOMX_HEADER)
if (XMR_RANDOMX_HEADER MATCHES "RANDOMX_FLAG_V2")
    target_compile_definitions(xmr_core PRIVATE RANDOMX_HAS_V2=1)
endif()

target_compile_definitions(xmr_core PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
    $<$<CXX_COMPILER_ID:MSVC>:NOMINMAX>
    $<$<CXX_COMPILER_ID:MSVC>:WIN32_LEAN_AND_MEAN>)

if (WIN32)
    target_compile_definitions(xmr_core PUBLIC _WIN32_WINNT=0x0601)
    target_link_libraries(xmr_core PUBLIC ws2_32)
else()
    target_link_libraries(xmr_core PUBLIC dl)
endif()

add_executable(xmr-miner src/main.cpp)
target_link_libraries(xmr-miner PRIVATE xmr_core)

# Shared OpenSSL DLLs must sit next to the executable on Windows. System-wide
# installs or static builds have no matching files, so this is intentionally a
# copy-if-present step.
if (WIN32)
    file(GLOB XMR_OPENSSL_RUNTIME_LIBS
         "${OPENSSL_ROOT_DIR}/bin/libssl*.dll"
         "${OPENSSL_ROOT_DIR}/bin/libcrypto*.dll")
    foreach (dll IN LISTS XMR_OPENSSL_RUNTIME_LIBS)
        add_custom_command(TARGET xmr-miner POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:xmr-miner>")
    endforeach()
endif()

add_executable(mock-pool tools/mock_pool.cpp)
target_link_libraries(mock-pool PRIVATE xmr_core)

if (XMR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "xmr_rebuild: RandomX tag = ${RANDOMX_TAG}")
