#!/bin/bash
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Copyright (c) 2024-present, Zvi Schneider
#
# Working ef_search performance test script
# Tests recall and performance with different ef_search values
#
# This file is part of valkey-search-benchmark and is licensed under the
# BSD 3-Clause License. See the LICENSE file in the root directory.
#

set -e

# Configuration - EDIT THESE FOR YOUR SETUP
# HOST="ec-search-zvi-ec-1shard-no-tls-0001-001.ajfdds.0001.euw1devo.cache.amazonaws.com"
if [ -z "${HOST:-}" ]; then
    echo "ERROR: HOST environment variable not set"
    echo "Set HOST to the Valkey/Redis cluster endpoint"
    exit 1
fi
DATASET_FILE="large_dataset.bin"
INDEX_NAME="large_scale_25"
PREFIX="zvec_large_:"
VECTOR_DIM=25
NUM_QUERIES=10000  # Adjust this - 1000 for quick test, 10000 for full test
ALGORITHM="hnsw"  # "flat" or "hnsw"
# Must run from build-debug directory
if [ ! -f "bin/valkey-benchmark" ]; then
    echo "ERROR: Must run from valkey build-debug directory"
    echo "Run: cd /home/ubuntu/valkey/build-debug && ../test_ef_search_working.sh"
    exit 1
fi

if [ ! -f "$DATASET_FILE" ]; then
    echo "ERROR: Dataset file $DATASET_FILE not found"
    echo "Please ensure you're in the build-debug directory with the dataset file"
    exit 1
fi

echo "=== ef_search Performance Test ==="
echo "Dataset: GloVe-25 (1.18M vectors)"
echo "Index: $INDEX_NAME"
echo "Queries: $NUM_QUERIES per ef_search value"
echo "Host: $HOST"
echo ""

# Test connection
echo "Testing connection..."
if ! ./bin/valkey-cli -h "$HOST" -c --no-auth-warning PING >/dev/null 2>&1; then
    echo "ERROR: Cannot connect to $HOST"
    exit 1
fi
echo "Connection OK"

# Verify index exists
if ! ./bin/valkey-cli -h "$HOST" -c --no-auth-warning FT.INFO "$INDEX_NAME" >/dev/null 2>&1; then
    echo "ERROR: Index $INDEX_NAME not found"
    echo "Please run the dataset insertion first"
    exit 1
fi

echo "Index verified"
echo ""

# Results file
RESULTS_FILE="ef_search_results_$(date +%Y%m%d_%H%M%S).csv"
echo "ef_search,avg_recall,min_recall,max_recall,qps,lat_avg,lat_p50,lat_p95,lat_p99,lat_max" > "$RESULTS_FILE"

printf "| %-9s | %-10s | %-7s | %-7s | %-7s | %-7s | %-7s | %-7s |\n" "ef_search" "Avg Recall" "QPS" "Avg" "P50" "P95" "P99" "Max"
printf "|%-11s|%-12s|%-9s|%-9s|%-9s|%-9s|%-9s|%-9s|\n" "-----------" "------------" "---------" "---------" "---------" "---------" "---------" "---------"
echo "| (value)   | (%)        | (req/s) | (ms)    | (ms)    | (ms)    | (ms)    | (ms)    |"

# Test different ef_search values
for ef_search in 50 100 150 200 250 300 400 500; do
    # echo "Testing ef_search=$ef_search with $NUM_QUERIES queries..."

    # Run benchmark - SINGLE LINE COMMAND (no backslashes!)
    OUTPUT=$(./bin/valkey-benchmark -h "$HOST" --cluster --rfr no --dataset "$DATASET_FILE" -t vec-query --search --vector-dim "$VECTOR_DIM" --search-name "$INDEX_NAME" --search-alg "$ALGORITHM" --search-prefix "$PREFIX" --ef-search "$ef_search" -n "$NUM_QUERIES" -c 10 --threads 10 2>&1)

    # Extract recall statistics
    AVG_RECALL=$(echo "$OUTPUT" | grep "Average:" | grep -o "[0-9.]*" | head -1)
    MIN_RECALL=$(echo "$OUTPUT" | grep "Min:" | grep -o "[0-9.]*" | head -1)
    MAX_RECALL=$(echo "$OUTPUT" | grep "Max:" | grep -o "[0-9.]*" | head -1)

    # Extract performance statistics
    QPS=$(echo "$OUTPUT" | grep "throughput summary:" | grep -o "[0-9.]*" | head -1)

    # Extract all latency metrics from the latency summary line
    LATENCY_LINE=$(echo "$OUTPUT" | grep -A2 "latency summary" | tail -1)
    LAT_AVG=$(echo "$LATENCY_LINE" | awk '{print $1}')
    LAT_MIN=$(echo "$LATENCY_LINE" | awk '{print $2}')
    LAT_P50=$(echo "$LATENCY_LINE" | awk '{print $3}')
    LAT_P95=$(echo "$LATENCY_LINE" | awk '{print $4}')
    LAT_P99=$(echo "$LATENCY_LINE" | awk '{print $5}')
    LAT_MAX=$(echo "$LATENCY_LINE" | awk '{print $6}')

    # Default values if extraction failed
    AVG_RECALL=${AVG_RECALL:-0}
    MIN_RECALL=${MIN_RECALL:-0}
    MAX_RECALL=${MAX_RECALL:-0}
    QPS=${QPS:-0}
    LAT_AVG=${LAT_AVG:-0}
    LAT_P50=${LAT_P50:-0}
    LAT_P95=${LAT_P95:-0}
    LAT_P99=${LAT_P99:-0}
    LAT_MAX=${LAT_MAX:-0}

    # Save to CSV
    echo "$ef_search,$AVG_RECALL,$MIN_RECALL,$MAX_RECALL,$QPS,$LAT_AVG,$LAT_P50,$LAT_P95,$LAT_P99,$LAT_MAX" >> "$RESULTS_FILE"

    # Display results
    printf "| %-9d | %10.1f | %7.0f | %7.2f | %7.2f | %7.2f | %7.2f | %7.2f |\n" \
        "$ef_search" "$AVG_RECALL" "$QPS" "$LAT_AVG" "$LAT_P50" "$LAT_P95" "$LAT_P99" "$LAT_MAX"
done

echo ""
echo "=== Results Summary ==="

# Find best configurations
echo "Results saved to: $RESULTS_FILE"
echo ""

# Show best recall and best performance
BEST_RECALL_LINE=$(tail -n +2 "$RESULTS_FILE" | sort -t',' -k2 -nr | head -1)
BEST_QPS_LINE=$(tail -n +2 "$RESULTS_FILE" | sort -t',' -k5 -nr | head -1)

BEST_RECALL_EF=$(echo "$BEST_RECALL_LINE" | cut -d',' -f1)
BEST_RECALL_VAL=$(echo "$BEST_RECALL_LINE" | cut -d',' -f2)
BEST_QPS_EF=$(echo "$BEST_QPS_LINE" | cut -d',' -f1)
BEST_QPS_VAL=$(echo "$BEST_QPS_LINE" | cut -d',' -f5)

echo "Best configurations:"
echo "  Highest recall: ${BEST_RECALL_VAL}% at ef_search=$BEST_RECALL_EF"
echo "  Highest QPS: ${BEST_QPS_VAL} at ef_search=$BEST_QPS_EF"

echo ""
echo "Recommendations:"
echo "  For high throughput: ef_search=50-100 (faster queries, ~70% recall)"
echo "  For balanced workload: ef_search=200 (good recall/performance tradeoff)"
echo "  For high accuracy: ef_search=300+ (best recall, slower queries)"

echo ""
echo "To view detailed results: cat $RESULTS_FILE"
echo "To run with more queries: edit NUM_QUERIES variable in script"