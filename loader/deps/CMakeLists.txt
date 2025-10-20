# Dependencies for valkey-benchmark

# Use pre-built jemalloc from copied jemalloc-build directory
set(JEMALLOC_LIB_DIR ${CMAKE_BINARY_DIR}/jemalloc-build/lib)
set(JEMALLOC_INCLUDE_DIR ${CMAKE_BINARY_DIR}/jemalloc-build/include)

# Create imported target for jemalloc
add_library(jemalloc STATIC IMPORTED)
set_target_properties(jemalloc PROPERTIES
    IMPORTED_LOCATION ${JEMALLOC_LIB_DIR}/libjemalloc.a
    INTERFACE_INCLUDE_DIRECTORIES ${JEMALLOC_INCLUDE_DIR}
    INTERFACE_COMPILE_DEFINITIONS USE_JEMALLOC
)

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

# Let libvalkey use sds and dict provided by valkey
set(DICT_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/loader/core)
set(SDS_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/loader/core)

add_subdirectory(libvalkey)
add_subdirectory(linenoise)
add_subdirectory(fpconv)
add_subdirectory(hdr_histogram)

# Clear cached variables
unset(BUILD_SHARED_LIBS CACHE)
unset(DISABLE_TESTS CACHE)
unset(ENABLE_TLS CACHE)
unset(ENABLE_RDMA CACHE)
