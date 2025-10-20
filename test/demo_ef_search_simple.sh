#!/bin/bash
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Copyright (c) 2024-present, Zvi Schneider
#
# Simple ef_search demo - tests 3 values quickly
#
# This file is part of valkey-search-benchmark and is licensed under the
# BSD 3-Clause License. See the LICENSE file in the root directory.
#

set -e

HOST="ec-search-zvi-ec-1shard-no-tls-0001-001.ajfdds.0001.euw1devo.cache.amazonaws.com"

echo "=== Quick ef_search Demo ==="
echo "Testing 3 ef_search values on large_scale_25 index"
echo ""

if [ ! -f "bin/valkey-benchmark" ]; then
    echo "ERROR: Run from build-debug directory"
    exit 1
fi

printf "| %-9s | %-7s | %-7s | %-7s | %-7s |\n" "ef_search" "Recall" "QPS" "Avg" "P99"
printf "|%-11s|%-9s|%-9s|%-9s|%-9s|\n" "-----------" "---------" "---------" "---------" "---------"
printf "| %-9s | %-7s | %-7s | %-7s | %-7s |\n" "" "(%)" "(req/s)" "(ms)" "(ms)"

for ef in 50 200 500; do
    echo "Testing ef_search=$ef..."

    # Single-line command with fixed number of queries
    OUT=$(./bin/valkey-benchmark -h $HOST --cluster --rfr no --dataset large_dataset.bin -t vec-query --search --vector-dim 25 --search-name large_scale_25 --search-prefix zvec_large_: --ef-search $ef -n 500 -c 10 --threads 10 2>&1)

    RECALL=$(echo "$OUT" | grep "Average:" | grep -o "[0-9.]*" | head -1)
    QPS=$(echo "$OUT" | grep "throughput summary:" | grep -o "[0-9.]*" | head -1)

    # Extract key latency metrics
    LATENCY_LINE=$(echo "$OUT" | grep -A2 "latency summary" | tail -1)
    LAT_AVG=$(echo "$LATENCY_LINE" | awk '{print $1}')
    LAT_P99=$(echo "$LATENCY_LINE" | awk '{print $5}')

    # Default values if extraction failed
    RECALL=${RECALL:-0}
    QPS=${QPS:-0}
    LAT_AVG=${LAT_AVG:-0}
    LAT_P99=${LAT_P99:-0}

    printf "| %-9s | %6.1f%% | %7.0f | %7.2f | %7.2f |\n" "$ef" "$RECALL" "$QPS" "$LAT_AVG" "$LAT_P99"
done

echo ""
echo "✓ Higher ef_search = Better recall, Lower performance"
echo "✓ For full testing: run ../test_ef_search_working.sh"