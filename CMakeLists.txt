cmake_minimum_required(VERSION 3.21)
project(work_stealing_scheduler LANGUAGES CXX)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Warnings)
include(Sanitizers)

option(WSS_BUILD_TESTS "Build unit tests" ${PROJECT_IS_TOP_LEVEL})
option(WSS_BUILD_BENCHMARKS "Build benchmark suite" OFF)
option(WSS_BUILD_EXAMPLES "Build example programs" ${PROJECT_IS_TOP_LEVEL})
set(WSS_SANITIZE "" CACHE STRING "Sanitizer(s) to build with: address | thread | undefined | address,undefined")

add_library(wss STATIC
    src/idle_signal.cpp
    src/injector.cpp
    src/metrics.cpp
    src/affinity_linux.cpp
    src/affinity_macos.cpp
    src/affinity_stub.cpp
    src/work_stealing_scheduler.cpp
    src/global_queue_scheduler.cpp
    src/thread_per_core_scheduler.cpp
    src/fair_scheduler.cpp
)
add_library(wss::wss ALIAS wss)

target_include_directories(wss PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

find_package(Threads REQUIRED)
target_link_libraries(wss PUBLIC Threads::Threads)

wss_set_warnings(wss)
wss_apply_sanitizers(wss)

if(WSS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(WSS_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(WSS_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
