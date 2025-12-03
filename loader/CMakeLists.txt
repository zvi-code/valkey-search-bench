project(valkey-benchmark)

set(INSTALL_BIN_PATH ${CMAKE_INSTALL_PREFIX}/bin)
set_directory_properties(PROPERTIES CLEAN_NO_CUSTOM 1)

# Valkey submodule path
set(VALKEY_SRC ${CMAKE_SOURCE_DIR}/deps/valkey/src)

# Add include directories for this target only
# VALKEY_SRC must be included here for valkey-benchmark sources to find
# Valkey's headers (ae.h, sds.h, etc.) but NOT globally to avoid
# polluting libvalkey's build with Valkey server's cluster.h
include_directories("${CMAKE_SOURCE_DIR}/loader")
include_directories("${VALKEY_SRC}")

# Only include jemalloc paths if using jemalloc
if (USE_JEMALLOC)
    include_directories("${CMAKE_BINARY_DIR}/jemalloc-build/include")
endif()

# Set up compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${VALKEY_SERVER_CFLAGS}")
message(STATUS "CFLAGS: ${CMAKE_C_FLAGS}")

# Get linker options
get_valkey_server_linker_option(VALKEY_SERVER_LDFLAGS)

# jemalloc is linked via ALLOCATOR_LIB in valkey_build_and_install_bin macro

# Target: valkey-benchmark
list(APPEND BENCH_LIBS "fpconv")
list(APPEND BENCH_LIBS "hdr_histogram")
valkey_build_and_install_bin(valkey-benchmark "${VALKEY_BENCHMARK_SRCS}" "${VALKEY_SERVER_LDFLAGS}" "${BENCH_LIBS}"
                             "redis-benchmark")

# Add USE_JEMALLOC compile definition only if using jemalloc
if (USE_JEMALLOC)
    target_compile_definitions(valkey-benchmark PRIVATE USE_JEMALLOC)
endif()

# NOTE: Removed unused code generation target dependencies:
# - generate_commands_def (commands.def not used)
# - generate_fmtargs_h (FMTARGS macro not used)
# These were legacy from Valkey server, not needed for benchmark client

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
