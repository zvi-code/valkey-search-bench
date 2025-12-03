#!/bin/bash
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Copyright (c) 2024-present, Zvi Schneider
#
# Setup jemalloc for valkey-search-benchmark
#
# This script builds jemalloc from the Valkey submodule (deps/valkey).
# jemalloc is REQUIRED for building the benchmark tool.
#
# Usage:
#   ./setup_jemalloc.sh
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=================================================="
echo "valkey-search-benchmark jemalloc setup"
echo "=================================================="
echo ""

# Target location for jemalloc (in build directory)
BUILD_DIR="build"
JEMALLOC_TARGET="${BUILD_DIR}/jemalloc-build"

# Check if jemalloc already exists
if [ -f "${JEMALLOC_TARGET}/lib/libjemalloc.a" ]; then
    SIZE=$(du -h "${JEMALLOC_TARGET}/lib/libjemalloc.a" | cut -f1)
    echo -e "${GREEN}✓ jemalloc already exists${NC}"
    echo "  Location: ${JEMALLOC_TARGET}"
    echo "  Size: ${SIZE}"
    echo ""
    echo "To rebuild, remove it first:"
    echo "  rm -rf ${JEMALLOC_TARGET}"
    exit 0
fi

# Check if submodule is initialized
if [ ! -f "deps/valkey/src/ae.c" ]; then
    echo -e "${YELLOW}Initializing Valkey submodule...${NC}"
    git submodule update --init --recursive
fi

# Check if Valkey submodule exists
if [ ! -d "deps/valkey" ]; then
    echo -e "${RED}✗ Valkey submodule not found${NC}"
    echo "Please run: git submodule update --init --recursive"
    exit 1
fi

echo -e "${GREEN}✓ Found Valkey submodule${NC}"
echo ""

# Build Valkey (which builds jemalloc)
echo -e "${YELLOW}Building Valkey (this will also build jemalloc)...${NC}"
echo "This may take a few minutes..."
echo ""

mkdir -p deps/valkey/build-release
cd deps/valkey/build-release
cmake -DCMAKE_BUILD_TYPE=Release .. > /dev/null 2>&1
make -j$(nproc) 2>&1 | tail -5

cd "$SCRIPT_DIR"

# Check if jemalloc was built
VALKEY_JEMALLOC="deps/valkey/build-release/jemalloc-build"
if [ ! -f "${VALKEY_JEMALLOC}/lib/libjemalloc.a" ]; then
    echo -e "${RED}✗ jemalloc build failed${NC}"
    echo "Check deps/valkey/build-release for errors"
    exit 1
fi

# Copy jemalloc to build directory
echo ""
echo "Copying jemalloc to build directory..."
mkdir -p "${BUILD_DIR}"
cp -r "${VALKEY_JEMALLOC}" "${JEMALLOC_TARGET}"

if [ -f "${JEMALLOC_TARGET}/lib/libjemalloc.a" ]; then
    SIZE=$(du -h "${JEMALLOC_TARGET}/lib/libjemalloc.a" | cut -f1)
    echo ""
    echo -e "${GREEN}✓ jemalloc setup complete${NC}"
    echo "  Location: ${JEMALLOC_TARGET}"
    echo "  Size: ${SIZE}"
    echo ""
    echo "You can now build the benchmark:"
    echo "  cd build && cmake -DCMAKE_BUILD_TYPE=Release .. && make valkey-benchmark -j\$(nproc)"
else
    echo -e "${RED}✗ Copy failed${NC}"
    exit 1
fi
