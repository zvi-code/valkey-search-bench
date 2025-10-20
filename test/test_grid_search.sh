#!/bin/bash
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Copyright (c) 2024-present, Zvi Schneider
#
# Build and run grid search test
#
# This file is part of valkey-search-benchmark and is licensed under the
# BSD 3-Clause License. See the LICENSE file in the root directory.
#

echo "Building grid search test..."
gcc -DTEST_GRID_SEARCH \
    -I/home/ubuntu/valkey-search-benchmark/src \
    -o /tmp/test_grid_search \
    src/load_optimizer.c \
    -lm \
    -g

if [ $? -eq 0 ]; then
    echo -e "\n========================================="
    echo "Running grid search test..."
    echo -e "=========================================\n"
    /tmp/test_grid_search
    exit_code=$?
    echo -e "\n========================================="
    echo "Test exit code: $exit_code"
    echo "========================================="
    exit $exit_code
else
    echo "Build failed!"
    exit 1
fi
