cmake_minimum_required(VERSION 3.10)

# Must be done first
if (APPLE)
    # Force clang compiler on macOS
    find_program(CLANGPP "clang++")
    find_program(CLANG "clang")
    if (CLANG AND CLANGPP)
        message(STATUS "Found ${CLANGPP}, ${CLANG}")
        set(CMAKE_CXX_COMPILER ${CLANGPP})
        set(CMAKE_C_COMPILER ${CLANG})
    endif ()
endif ()

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/Modules/")
project("valkey-search-benchmark")
add_compile_options(-Wundef)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

include(ValkeySetup)
add_subdirectory(loader)

# Clear cached variables from the cache
unset(CLANGPP CACHE)
unset(CLANG CACHE)
unset(BUILD_RDMA_MODULE CACHE)
unset(BUILD_TLS_MODULE CACHE)
unset(USE_TLS CACHE)
