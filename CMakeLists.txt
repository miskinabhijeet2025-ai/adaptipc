cmake_minimum_required(VERSION 3.16)
project(AdaptIPC
    VERSION 0.1.0
    DESCRIPTION "Adaptive IPC routing middleware (SHM ring buffer + UDS fallback)"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

add_library(adaptipc
    src/shm_ringbuffer.c
    src/uds_fallback.c
    src/adapt_ipc.c)
target_include_directories(adaptipc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

find_package(Threads REQUIRED)
target_link_libraries(adaptipc PUBLIC Threads::Threads)

option(ADAPTIPC_BUILD_TESTS "Build AdaptIPC tests" ON)
if(ADAPTIPC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
