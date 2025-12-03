# -------------------------------------------------
# Define the sources to be built for valkey-benchmark
# -------------------------------------------------
# This project only builds valkey-benchmark, using Valkey submodule for core utilities

# Valkey submodule path
set(VALKEY_SRC ${CMAKE_SOURCE_DIR}/deps/valkey/src)

# valkey-benchmark sources
set(VALKEY_BENCHMARK_SRCS
    # Core Valkey utilities from submodule
    ${VALKEY_SRC}/ae.c
    ${VALKEY_SRC}/anet.c
    ${VALKEY_SRC}/sds.c
    ${VALKEY_SRC}/sha256.c
    ${VALKEY_SRC}/util.c
    ${VALKEY_SRC}/adlist.c
    ${VALKEY_SRC}/dict.c
    ${VALKEY_SRC}/zmalloc.c
    ${VALKEY_SRC}/serverassert.c
    ${VALKEY_SRC}/release.c
    ${VALKEY_SRC}/crcspeed.c
    ${VALKEY_SRC}/crccombine.c
    ${VALKEY_SRC}/crc64.c
    ${VALKEY_SRC}/siphash.c
    ${VALKEY_SRC}/crc16.c
    ${VALKEY_SRC}/monotonic.c
    ${VALKEY_SRC}/cli_common.c
    ${VALKEY_SRC}/mt19937-64.c
    ${VALKEY_SRC}/strl.c
    # Benchmark-specific sources
    ${CMAKE_SOURCE_DIR}/loader/valkey-benchmark.c
    ${CMAKE_SOURCE_DIR}/loader/search_utils.c
    ${CMAKE_SOURCE_DIR}/loader/dataset_api.c
    ${CMAKE_SOURCE_DIR}/loader/mapping_scan.c
    ${CMAKE_SOURCE_DIR}/loader/dataset_id_mapping.c
    ${CMAKE_SOURCE_DIR}/loader/utils.c
    ${CMAKE_SOURCE_DIR}/loader/load_optimizer.c
    ${CMAKE_SOURCE_DIR}/loader/config_persist.c)
