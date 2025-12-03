include(CheckIncludeFiles)
include(ProcessorCount)
include(Utils)

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# Generate compile_commands.json file for IDEs code completion support
set(CMAKE_EXPORT_COMPILE_COMMANDS 1)

processorcount(VALKEY_PROCESSOR_COUNT)
message(STATUS "Processor count: ${VALKEY_PROCESSOR_COUNT}")

# Installed executables will have this permissions
set(VALKEY_EXE_PERMISSIONS
    OWNER_EXECUTE
    OWNER_WRITE
    OWNER_READ
    GROUP_EXECUTE
    GROUP_READ
    WORLD_EXECUTE
    WORLD_READ)

set(VALKEY_SERVER_CFLAGS "")
set(VALKEY_SERVER_LDFLAGS "")

# ----------------------------------------------------
# Helper functions & macros
# ----------------------------------------------------
macro (add_valkey_server_compiler_options value)
    set(VALKEY_SERVER_CFLAGS "${VALKEY_SERVER_CFLAGS} ${value}")
endmacro ()

macro (add_valkey_server_linker_option value)
    list(APPEND VALKEY_SERVER_LDFLAGS ${value})
endmacro ()

macro (get_valkey_server_linker_option return_value)
    list(JOIN VALKEY_SERVER_LDFLAGS " " ${value} ${return_value})
endmacro ()

set(IS_FREEBSD 0)
if (CMAKE_SYSTEM_NAME MATCHES "^.*BSD$|DragonFly")
    message(STATUS "Building for FreeBSD compatible system")
    set(IS_FREEBSD 1)
    include_directories("/usr/local/include")
    add_valkey_server_compiler_options("-DUSE_BACKTRACE")
endif ()

# Helper function for creating symbolic link so that: link -> source
macro (valkey_create_symlink source link)
    install(
        CODE "execute_process(                      \
    COMMAND /bin/bash ${CMAKE_BINARY_DIR}/CreateSymlink.sh \
    ${source} \
    ${link}   \
    )"
        COMPONENT "valkey")
endmacro ()

# Install a binary
macro (valkey_install_bin target)
    # Install cli tool and create a redis symbolic link
    install(
        TARGETS ${target}
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        PERMISSIONS ${VALKEY_EXE_PERMISSIONS}
        COMPONENT "valkey")
endmacro ()

# Helper function that defines, builds and installs `target` In addition, it creates a symbolic link between the target
# and `link_name`
macro (valkey_build_and_install_bin target sources ld_flags libs link_name)
    add_executable(${target} ${sources})

    if (USE_JEMALLOC
        OR USE_TCMALLOC
        OR USE_TCMALLOC_MINIMAL)
        # Using custom allocator
        target_link_libraries(${target} ${ALLOCATOR_LIB})
    endif ()

    # Place this line last to ensure that ${ld_flags} is placed last on the linker line
    target_link_libraries(${target} ${libs} ${ld_flags})
    target_link_libraries(${target} valkey::valkey)
    if (USE_TLS)
        # Add required libraries needed for TLS
        target_link_libraries(${target} OpenSSL::SSL valkey::valkey_tls)
    endif ()

    if (USE_RDMA)
        # Add required libraries needed for RDMA
        target_link_libraries(${target} valkey::valkey_rdma)
    endif ()

    if (IS_FREEBSD)
        target_link_libraries(${target} execinfo)
    endif ()

    # Enable all warnings + fail on warning
    target_compile_options(${target} PRIVATE -Werror -Wall)

    # Install cli tool and create a redis symbolic link
    valkey_install_bin(${target})
    valkey_create_symlink(${target} ${link_name})
endmacro ()

# Determine if we are building in Release or Debug mode
if (CMAKE_BUILD_TYPE MATCHES Debug OR CMAKE_BUILD_TYPE MATCHES DebugFull)
    set(VALKEY_DEBUG_BUILD 1)
    set(VALKEY_RELEASE_BUILD 0)
    message(STATUS "Building in debug mode")
else ()
    set(VALKEY_DEBUG_BUILD 0)
    set(VALKEY_RELEASE_BUILD 1)
    message(STATUS "Building in release mode")
endif ()

# ----------------------------------------------------
# Helper functions - end
# ----------------------------------------------------

# ----------------------------------------------------
# Build options (allocator, tls, rdma et al)
# ----------------------------------------------------

if (NOT BUILD_MALLOC)
    if (APPLE)
        set(BUILD_MALLOC "libc")
    elseif (UNIX)
        set(BUILD_MALLOC "jemalloc")
    endif ()
endif ()

# User may pass different allocator library. Using -DBUILD_MALLOC=<libname>, make sure it is a valid value
if (BUILD_MALLOC)
    if ("${BUILD_MALLOC}" STREQUAL "jemalloc")
        set(MALLOC_LIB "jemalloc")
        # Use the jemalloc we build from the submodule, not system jemalloc
        set(ALLOCATOR_LIB "${CMAKE_BINARY_DIR}/jemalloc-build/lib/libjemalloc.a")
        add_valkey_server_compiler_options("-DUSE_JEMALLOC")
        set(USE_JEMALLOC 1)
    elseif ("${BUILD_MALLOC}" STREQUAL "libc")
        set(MALLOC_LIB "libc")
    elseif ("${BUILD_MALLOC}" STREQUAL "tcmalloc")
        set(MALLOC_LIB "tcmalloc")
        valkey_pkg_config(libtcmalloc ALLOCATOR_LIB)

        add_valkey_server_compiler_options("-DUSE_TCMALLOC")
        set(USE_TCMALLOC 1)
    elseif ("${BUILD_MALLOC}" STREQUAL "tcmalloc_minimal")
        set(MALLOC_LIB "tcmalloc_minimal")
        valkey_pkg_config(libtcmalloc_minimal ALLOCATOR_LIB)

        add_valkey_server_compiler_options("-DUSE_TCMALLOC")
        set(USE_TCMALLOC_MINIMAL 1)
    else ()
        message(FATAL_ERROR "BUILD_MALLOC can be one of: jemalloc, libc, tcmalloc or tcmalloc_minimal")
    endif ()
endif ()

message(STATUS "Using ${MALLOC_LIB}")

# TLS support
if (BUILD_TLS)
    valkey_parse_build_option(${BUILD_TLS} USE_TLS)
    if (USE_TLS EQUAL 1)
        # Only search for OpenSSL if needed
        find_package(OpenSSL REQUIRED)
        message(STATUS "OpenSSL include dir: ${OPENSSL_INCLUDE_DIR}")
        message(STATUS "OpenSSL libraries: ${OPENSSL_LIBRARIES}")
        include_directories(${OPENSSL_INCLUDE_DIR})
    endif ()

    if (USE_TLS EQUAL 1)
        add_valkey_server_compiler_options("-DUSE_OPENSSL=1")
        add_valkey_server_compiler_options("-DBUILD_TLS_MODULE=0")
    else ()
        # Build TLS as a module RDMA can only be built as a module. So disable it
        message(WARNING "BUILD_TLS can be one of: [ON | OFF | 1 | 0], but '${BUILD_TLS}' was provided")
        message(STATUS "TLS support is disabled")
        set(USE_TLS 0)
    endif ()
else ()
    # By default, TLS is disabled
    message(STATUS "TLS is disabled")
    set(USE_TLS 0)
endif ()

if (BUILD_RDMA)
    set(BUILD_RDMA_MODULE 0)
    # RDMA support (Linux only)
    if (LINUX AND NOT APPLE)
        valkey_parse_build_option(${BUILD_RDMA} USE_RDMA)
        find_package(PkgConfig REQUIRED)
        # Locate librdmacm & libibverbs, fail if we can't find them
        valkey_pkg_config(librdmacm RDMACM_LIBS)
        valkey_pkg_config(libibverbs IBVERBS_LIBS)
        message(STATUS "${RDMACM_LIBS};${IBVERBS_LIBS}")
        list(APPEND RDMA_LIBS "${RDMACM_LIBS};${IBVERBS_LIBS}")

        if (USE_RDMA EQUAL 2) # Module
            message(STATUS "Building RDMA as module")
            add_valkey_server_compiler_options("-DUSE_RDMA=2")
            set(BUILD_RDMA_MODULE 2)
        elseif (USE_RDMA EQUAL 1) # Builtin
            message(STATUS "Building RDMA as builtin")
            add_valkey_server_compiler_options("-DUSE_RDMA=1")
            add_valkey_server_compiler_options("-DBUILD_RDMA_MODULE=0")
            list(APPEND SERVER_LIBS "${RDMA_LIBS}")
        endif ()
    else ()
        message(WARNING "RDMA is only supported on Linux platforms")
    endif ()
else ()
    # By default, RDMA is disabled
    message(STATUS "RDMA is disabled")
    set(USE_RDMA 0)
endif ()

set(BUILDING_ARM64 0)
set(BUILDING_ARM32 0)

if ("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "arm64" OR "${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "aarch64")
    set(BUILDING_ARM64 1)
endif ()

if ("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "arm")
    set(BUILDING_ARM32 1)
endif ()

message(STATUS "Building on ${CMAKE_HOST_SYSTEM_NAME}")
if (BUILDING_ARM64)
    message(STATUS "Compiling valkey for ARM64")
    add_valkey_server_linker_option("-funwind-tables")
endif ()

if (APPLE)
    add_valkey_server_linker_option("-rdynamic")
    add_valkey_server_linker_option("-ldl")
elseif (UNIX)
    add_valkey_server_linker_option("-rdynamic")
    add_valkey_server_linker_option("-pthread")
    add_valkey_server_linker_option("-ldl")
    add_valkey_server_linker_option("-lm")
endif ()

if (VALKEY_DEBUG_BUILD)
    # Debug build, use enable "-fno-omit-frame-pointer"
    add_valkey_server_compiler_options("-fno-omit-frame-pointer")
endif ()

# Check for Atomic
check_include_files(stdatomic.h HAVE_C11_ATOMIC)
if (HAVE_C11_ATOMIC)
    add_valkey_server_compiler_options("-std=gnu11")
else ()
    add_valkey_server_compiler_options("-std=c99")
endif ()

# Sanitizer
if (BUILD_SANITIZER)
    # Common CFLAGS
    list(APPEND VALKEY_SANITAIZER_CFLAGS "-fno-sanitize-recover=all")
    list(APPEND VALKEY_SANITAIZER_CFLAGS "-fno-omit-frame-pointer")
    if ("${BUILD_SANITIZER}" STREQUAL "address")
        list(APPEND VALKEY_SANITAIZER_CFLAGS "-fsanitize=address")
        list(APPEND VALKEY_SANITAIZER_LDFLAGS "-fsanitize=address")
    elseif ("${BUILD_SANITIZER}" STREQUAL "thread")
        list(APPEND VALKEY_SANITAIZER_CFLAGS "-fsanitize=thread")
        list(APPEND VALKEY_SANITAIZER_LDFLAGS "-fsanitize=thread")
    elseif ("${BUILD_SANITIZER}" STREQUAL "undefined")
        list(APPEND VALKEY_SANITAIZER_CFLAGS "-fsanitize=undefined")
        list(APPEND VALKEY_SANITAIZER_LDFLAGS "-fsanitize=undefined")
    else ()
        message(FATAL_ERROR "Unknown sanitizer: ${BUILD_SANITIZER}")
    endif ()
endif ()

# Valkey submodule paths
set(VALKEY_SRC_DIR "${CMAKE_SOURCE_DIR}/deps/valkey/src")
set(VALKEY_DEPS_DIR "${CMAKE_SOURCE_DIR}/deps/valkey/deps")

# Build jemalloc FIRST if needed (must be done before other deps)
# jemalloc uses configure/make, not cmake subdirectory, so we use its CMakeLists.txt wrapper
if (USE_JEMALLOC)
    add_subdirectory("${VALKEY_DEPS_DIR}/jemalloc" "${CMAKE_BINARY_DIR}/valkey-deps/jemalloc")
    include_directories("${CMAKE_BINARY_DIR}/jemalloc-build/include")
endif ()

# Include directories for deps we use directly (not VALKEY_SRC_DIR!)
# NOTE: Do NOT add VALKEY_SRC_DIR to global includes - it would pollute
# libvalkey with Valkey server's cluster.h instead of libvalkey's own
include_directories("${VALKEY_DEPS_DIR}/libvalkey/include")
include_directories("${VALKEY_DEPS_DIR}/linenoise")
include_directories("${VALKEY_DEPS_DIR}/hdr_histogram")
include_directories("${VALKEY_DEPS_DIR}/fpconv")

# Set include paths for libvalkey to find dict.h and sds.h from Valkey
# Must be set as non-cache variables right before libvalkey is added
set(DICT_INCLUDE_DIR "${VALKEY_SRC_DIR}")
set(SDS_INCLUDE_DIR "${VALKEY_SRC_DIR}")

# libvalkey options - must be set before add_subdirectory
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries")
set(DISABLE_TESTS ON CACHE BOOL "If tests should be compiled or not")

# Build dependencies directly (not using deps/CMakeLists.txt which has wrong paths)
# We only need: libvalkey, linenoise, fpconv, hdr_histogram
add_subdirectory("${VALKEY_DEPS_DIR}/libvalkey" "${CMAKE_BINARY_DIR}/valkey-deps/libvalkey")
add_subdirectory("${VALKEY_DEPS_DIR}/linenoise" "${CMAKE_BINARY_DIR}/valkey-deps/linenoise")
add_subdirectory("${VALKEY_DEPS_DIR}/fpconv" "${CMAKE_BINARY_DIR}/valkey-deps/fpconv")
add_subdirectory("${VALKEY_DEPS_DIR}/hdr_histogram" "${CMAKE_BINARY_DIR}/valkey-deps/hdr_histogram")

# Common compiler flags
add_valkey_server_compiler_options("-pedantic")

# ----------------------------------------------------
# Build options (allocator, tls, rdma et al) - end
# ----------------------------------------------------

# ----------------------------------------------------------
# All our source files are defined in SourceFiles.cmake file
# ----------------------------------------------------------
include(SourceFiles)

# Clear the below variables from the cache
unset(CMAKE_C_FLAGS CACHE)
unset(VALKEY_SERVER_LDFLAGS CACHE)
unset(VALKEY_SERVER_CFLAGS CACHE)
unset(PYTHON_EXE CACHE)
unset(HAVE_C11_ATOMIC CACHE)
unset(USE_TLS CACHE)
unset(USE_RDMA CACHE)
unset(BUILD_TLS CACHE)
unset(BUILD_RDMA CACHE)
unset(BUILD_MALLOC CACHE)
unset(USE_JEMALLOC CACHE)
unset(BUILD_TLS_MODULE CACHE)
unset(BUILD_TLS_BUILTIN CACHE)
