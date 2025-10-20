#!/bin/bash
# Build and run grid search test

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
