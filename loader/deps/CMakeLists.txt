# Dependencies for valkey-benchmark

# Build jemalloc from Valkey submodule
if (USE_JEMALLOC)
    add_subdirectory("${CMAKE_SOURCE_DIR}/deps/valkey/deps/jemalloc" "${CMAKE_BINARY_DIR}/jemalloc")
endif()

# Set libvalkey options
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries")
set(DISABLE_TESTS ON CACHE BOOL "If tests should be compiled or not")

if (USE_TLS)
    message(STATUS "Building valkey_tls")
    set(ENABLE_TLS ON CACHE BOOL "If TLS support should be compiled or not")
endif ()

if (USE_RDMA)
    message(STATUS "Building valkey_rdma")
    set(ENABLE_RDMA ON CACHE BOOL "If RDMA support should be compiled or not")
endif ()

# Let libvalkey use sds and dict provided by valkey submodule
set(DICT_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/deps/valkey/src)
set(SDS_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/deps/valkey/src)

add_subdirectory(libvalkey)
add_subdirectory(linenoise)
add_subdirectory(fpconv)
add_subdirectory(hdr_histogram)

# Clear cached variables
unset(BUILD_SHARED_LIBS CACHE)
unset(DISABLE_TESTS CACHE)
unset(ENABLE_TLS CACHE)
unset(ENABLE_RDMA CACHE)
