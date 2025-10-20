project(valkey-benchmark)

set(INSTALL_BIN_PATH ${CMAKE_INSTALL_PREFIX}/bin)
set_directory_properties(PROPERTIES CLEAN_NO_CUSTOM 1)

# Add include directories
include_directories("${CMAKE_SOURCE_DIR}/loader")
include_directories("${CMAKE_SOURCE_DIR}/loader/core")
include_directories("${CMAKE_BINARY_DIR}/jemalloc-build/include")

# Set up compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${VALKEY_SERVER_CFLAGS}")
message(STATUS "CFLAGS: ${CMAKE_C_FLAGS}")

# Get linker options
get_valkey_server_linker_option(VALKEY_SERVER_LDFLAGS)

# Add jemalloc static library to linker flags
set(VALKEY_SERVER_LDFLAGS "${VALKEY_SERVER_LDFLAGS} ${CMAKE_BINARY_DIR}/jemalloc-build/lib/libjemalloc.a")

# Target: valkey-benchmark with jemalloc
list(APPEND BENCH_LIBS "fpconv")
list(APPEND BENCH_LIBS "hdr_histogram")
valkey_build_and_install_bin(valkey-benchmark "${VALKEY_BENCHMARK_SRCS}" "${VALKEY_SERVER_LDFLAGS}" "${BENCH_LIBS}"
                             "redis-benchmark")

# Add USE_JEMALLOC compile definition
target_compile_definitions(valkey-benchmark PRIVATE USE_JEMALLOC)

add_dependencies(valkey-benchmark generate_commands_def)
add_dependencies(valkey-benchmark generate_fmtargs_h)

if (VALKEY_RELEASE_BUILD)
    # Enable LTO for Release build
    set_property(TARGET valkey-benchmark PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif ()

if (BUILD_SANITIZER)
    message(STATUS "Adding sanitizer flags for target valkey-benchmark")
    target_compile_options(valkey-benchmark PRIVATE ${VALKEY_SANITAIZER_CFLAGS})
    target_link_options(valkey-benchmark PRIVATE ${VALKEY_SANITAIZER_LDFLAGS})
endif ()
unset(BUILD_SANITIZER CACHE)
