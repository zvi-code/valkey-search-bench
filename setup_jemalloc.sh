#!/bin/bash
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Copyright (c) 2024-present, Zvi Schneider
#
# Setup jemalloc for valkey-search-benchmark
#
# This file is part of valkey-search-benchmark and is licensed under the
# BSD 3-Clause License. See the LICENSE file in the root directory.
#
# This script helps you setup jemalloc which is REQUIRED for building
# the benchmark tool. Without jemalloc, the binary will segfault.
#
# Usage:
#   ./setup_jemalloc.sh [valkey-build-dir]
#
# Examples:
#   ./setup_jemalloc.sh /path/to/valkey/build-release
#   ./setup_jemalloc.sh  # Auto-detect from common locations
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
JEMALLOC_TARGET="${BUILD_DIR}/jemalloc-build"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=================================================="
echo "valkey-search-benchmark jemalloc setup"
echo "=================================================="
echo ""

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

# Try to find Valkey build directory
VALKEY_BUILD=""

if [ -n "$1" ]; then
    # User provided path
    VALKEY_BUILD="$1"
elif [ -d "$HOME/valkey/build-release" ]; then
    VALKEY_BUILD="$HOME/valkey/build-release"
elif [ -d "$HOME/valkey/build-debug" ]; then
    VALKEY_BUILD="$HOME/valkey/build-debug"
elif [ -d "$HOME/valkey/build" ]; then
    VALKEY_BUILD="$HOME/valkey/build"
elif [ -d "/home/ubuntu/valkey/build-release" ]; then
    VALKEY_BUILD="/home/ubuntu/valkey/build-release"
elif [ -d "/home/ubuntu/valkey/build-debug" ]; then
    VALKEY_BUILD="/home/ubuntu/valkey/build-debug"
fi

# Check if we found jemalloc
if [ -n "$VALKEY_BUILD" ] && [ -f "${VALKEY_BUILD}/jemalloc-build/lib/libjemalloc.a" ]; then
    echo -e "${GREEN}✓ Found Valkey build with jemalloc${NC}"
    echo "  Source: ${VALKEY_BUILD}/jemalloc-build"
    echo ""
    
    # Copy jemalloc
    echo "Copying jemalloc..."
    mkdir -p "${BUILD_DIR}"
    cp -r "${VALKEY_BUILD}/jemalloc-build" "${JEMALLOC_TARGET}"
    
    if [ -f "${JEMALLOC_TARGET}/lib/libjemalloc.a" ]; then
        SIZE=$(du -h "${JEMALLOC_TARGET}/lib/libjemalloc.a" | cut -f1)
        echo -e "${GREEN}✓ jemalloc copied successfully${NC}"
        echo "  Location: ${JEMALLOC_TARGET}"
        echo "  Size: ${SIZE}"
        echo ""
        echo "You can now build the benchmark:"
        echo "  cd build && cmake -DCMAKE_BUILD_TYPE=Release .. && make valkey-benchmark"
        exit 0
    else
        echo -e "${RED}✗ Copy failed${NC}"
        exit 1
    fi
fi

# If we get here, we couldn't find jemalloc automatically
echo -e "${YELLOW}! Could not auto-detect jemalloc${NC}"
echo ""
echo "Please choose one of these options:"
echo ""
echo "Option 1: Build Valkey first, then run this script"
echo "  cd ~/valkey"
echo "  mkdir build-release && cd build-release"
echo "  cmake -DCMAKE_BUILD_TYPE=Release .."
echo "  make -j\$(nproc)"
echo "  cd ${SCRIPT_DIR}"
echo "  ./setup_jemalloc.sh ~/valkey/build-release"
echo ""
echo "Option 2: Manually specify Valkey build directory"
echo "  ./setup_jemalloc.sh /path/to/valkey/build-dir"
echo ""
echo "Option 3: Build jemalloc standalone (see INSTALLATION.md)"
echo "  Section: 'Option C: Build jemalloc standalone'"
echo ""
exit 1
