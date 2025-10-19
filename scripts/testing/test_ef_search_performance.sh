#!/bin/bash

# Script to test ef_search parameter impact on recall, QPS, and latency
# Usage: ./test_ef_search_performance.sh [host]

set -e

# Configuration
# HOST=${1:-"ec-search-zvi-ec-1shard-no-tls-0001-001.ajfdds.0001.euw1devo.cache.amazonaws.com"}
if [ -z "${HOST:-}" ]; then
    echo "ERROR: HOST environment variable not set"
    echo "Set HOST to the Valkey/Redis cluster endpoint"
    exit 1
fi
DATASET_FILE="large_dataset.bin"
INDEX_NAME="ef_search_test"
PREFIX="ef_test_"
VECTOR_DIM=25
NUM_QUERIES=1000
CONCURRENCY=10
THREADS=10

# ef_search values to test (HNSW parameter that controls search accuracy vs speed)
EF_SEARCH_VALUES=(50 100 150 200 250 300 400 500)

# Results file
RESULTS_FILE="ef_search_results_$(date +%Y%m%d_%H%M%S).csv"

echo "=== ef_search Performance Testing ==="
echo "Host: $HOST"
echo "Dataset: $DATASET_FILE"
echo "Queries: $NUM_QUERIES"
echo "Results file: $RESULTS_FILE"
echo ""

# Check if dataset exists
if [ ! -f "$DATASET_FILE" ]; then
    echo "ERROR: Dataset file $DATASET_FILE not found!"
    echo "Please run: python3 create_large_dataset.py"
    exit 1
fi

# Test connection
echo "Testing connection..."
if ! timeout 5 ./build-debug/bin/valkey-cli -h "$HOST" -c --no-auth-warning PING > /dev/null 2>&1; then
    echo "ERROR: Cannot connect to $HOST"
    exit 1
fi
echo "Connection OK"

# Create CSV header
echo "ef_search,avg_recall,min_recall,max_recall,total_queries,qps,avg_latency_ms,p50_latency_ms,p95_latency_ms,p99_latency_ms" > "$RESULTS_FILE"

# Function to extract recall statistics from output
extract_recall_stats() {
    local output="$1"
    local avg_recall=$(echo "$output" | grep -o "Average recall: [0-9.]*%" | grep -o "[0-9.]*")
    local min_recall=$(echo "$output" | grep -o "Min recall: [0-9.]*%" | grep -o "[0-9.]*")
    local max_recall=$(echo "$output" | grep -o "Max recall: [0-9.]*%" | grep -o "[0-9.]*")
    local total_queries=$(echo "$output" | grep -o "Total queries: [0-9]*" | grep -o "[0-9]*")

    echo "$avg_recall,$min_recall,$max_recall,$total_queries"
}

# Function to extract performance statistics
extract_perf_stats() {
    local output="$1"
    local qps=$(echo "$output" | grep "VEC-QUERY:" | tail -1 | grep -o "rps=[0-9.]*" | grep -o "[0-9.]*")
    local avg_latency=$(echo "$output" | grep "VEC-QUERY:" | tail -1 | grep -o "avg_msec=[0-9.]*" | grep -o "[0-9.]*")

    # Extract latency percentiles if available (these might not be in current output)
    local p50_latency=$(echo "$output" | grep -o "p50=[0-9.]*ms" | grep -o "[0-9.]*" | head -1)
    local p95_latency=$(echo "$output" | grep -o "p95=[0-9.]*ms" | grep -o "[0-9.]*" | head -1)
    local p99_latency=$(echo "$output" | grep -o "p99=[0-9.]*ms" | grep -o "[0-9.]*" | head -1)

    # Use placeholders if percentiles not available
    p50_latency=${p50_latency:-"N/A"}
    p95_latency=${p95_latency:-"N/A"}
    p99_latency=${p99_latency:-"N/A"}

    echo "$qps,$avg_latency,$p50_latency,$p95_latency,$p99_latency"
}

# Setup: Insert dataset once (if not already present)
echo "Setting up dataset..."
echo "Checking if index exists..."

INDEX_EXISTS=$(./build-debug/bin/valkey-cli -h "$HOST" -c --no-auth-warning FT._LIST | grep -c "$INDEX_NAME" || echo "0")

if [ "$INDEX_EXISTS" -eq "0" ]; then
    echo "Index not found. Inserting dataset..."

    # Clear any existing data
    ./build-debug/bin/valkey-cli -h "$HOST" -c --no-auth-warning FLUSHALL

    # Insert the dataset
    echo "Inserting 1.18M vectors (this may take 2-3 minutes)..."
    ./build-debug/bin/valkey-benchmark -h "$HOST" --cluster --rfr no \
        --dataset "$DATASET_FILE" \
        -t vec-ground-truth --search --vector-dim "$VECTOR_DIM" \
        --search-name "$INDEX_NAME" --search-prefix "$PREFIX" \
        -n 1183514 -c 10 --clean > /dev/null

    echo "Dataset insertion completed."
else
    echo "Index $INDEX_NAME already exists, skipping insertion."
fi

# Verify dataset is ready
DOC_COUNT=$(./build-debug/bin/valkey-cli -h "$HOST" -c --no-auth-warning FT.INFO "$INDEX_NAME" | grep "num_docs" | awk '{print $2}')
echo "Index contains $DOC_COUNT documents"

if [ "$DOC_COUNT" -lt "1000000" ]; then
    echo "WARNING: Index only has $DOC_COUNT documents, expected ~1.18M"
fi

echo ""
echo "Starting ef_search performance testing..."
echo "Testing ef_search values: ${EF_SEARCH_VALUES[*]}"
echo ""

# Test each ef_search value
for ef_search in "${EF_SEARCH_VALUES[@]}"; do
    echo "=== Testing ef_search = $ef_search ==="

    # Run the benchmark with specific ef_search value
    echo "Running $NUM_QUERIES queries with ef_search=$ef_search..."

    OUTPUT=$(./build-debug/bin/valkey-benchmark -h "$HOST" --cluster --rfr no \
        --dataset "$DATASET_FILE" \
        -t vec-query --search --vector-dim "$VECTOR_DIM" \
        --search-name "$INDEX_NAME" --search-prefix "$PREFIX" \
        --ef-search "$ef_search" \
        -n "$NUM_QUERIES" -c "$CONCURRENCY" --threads "$THREADS" 2>&1)

    # Extract statistics
    RECALL_STATS=$(extract_recall_stats "$OUTPUT")
    PERF_STATS=$(extract_perf_stats "$OUTPUT")

    # Combine results
    RESULT_LINE="$ef_search,$RECALL_STATS,$PERF_STATS"

    # Save to CSV
    echo "$RESULT_LINE" >> "$RESULTS_FILE"

    # Display results
    echo "Results: ef_search=$ef_search"
    echo "  Recall: $(echo $RECALL_STATS | cut -d',' -f1)% avg, $(echo $RECALL_STATS | cut -d',' -f2)% min, $(echo $RECALL_STATS | cut -d',' -f3)% max"
    echo "  Performance: $(echo $PERF_STATS | cut -d',' -f1) QPS, $(echo $PERF_STATS | cut -d',' -f2)ms avg latency"
    echo ""

    # Small delay between tests
    sleep 2
done

echo "=== Testing Complete ==="
echo ""
echo "Results saved to: $RESULTS_FILE"
echo ""

# Display summary table
echo "=== SUMMARY TABLE ==="
echo "ef_search | Avg Recall | QPS    | Avg Latency"
echo "----------|------------|--------|------------"

while IFS=',' read -r ef_search avg_recall min_recall max_recall total_queries qps avg_latency p50 p95 p99; do
    if [ "$ef_search" != "ef_search" ]; then  # Skip header
        printf "%-9s | %-10s | %-6s | %-11s\n" "$ef_search" "${avg_recall}%" "$qps" "${avg_latency}ms"
    fi
done < "$RESULTS_FILE"

echo ""
echo "=== ANALYSIS ==="

# Find best recall and best performance
BEST_RECALL_LINE=$(tail -n +2 "$RESULTS_FILE" | sort -t',' -k2 -nr | head -1)
BEST_QPS_LINE=$(tail -n +2 "$RESULTS_FILE" | sort -t',' -k6 -nr | head -1)

BEST_RECALL_EF=$(echo "$BEST_RECALL_LINE" | cut -d',' -f1)
BEST_RECALL_VALUE=$(echo "$BEST_RECALL_LINE" | cut -d',' -f2)
BEST_QPS_EF=$(echo "$BEST_QPS_LINE" | cut -d',' -f1)
BEST_QPS_VALUE=$(echo "$BEST_QPS_LINE" | cut -d',' -f6)

echo "Best recall: ${BEST_RECALL_VALUE}% at ef_search=$BEST_RECALL_EF"
echo "Best QPS: ${BEST_QPS_VALUE} at ef_search=$BEST_QPS_EF"

# Calculate recall vs performance tradeoff
echo ""
echo "Recommendation:"
echo "- For highest accuracy: use ef_search=$BEST_RECALL_EF"
echo "- For highest throughput: use ef_search=$BEST_QPS_EF"

# Find balanced option (good recall with reasonable performance)
echo ""
echo "Balanced options (recall >= 80%, sorted by QPS):"
tail -n +2 "$RESULTS_FILE" | awk -F',' '$2 >= 80 {print $1","$2","$6}' | sort -t',' -k3 -nr | head -3 | while IFS=',' read -r ef recall qps; do
    echo "  ef_search=$ef: ${recall}% recall, ${qps} QPS"
done

echo ""
echo "To visualize results:"
echo "  Open $RESULTS_FILE in a spreadsheet application"
echo "  Plot ef_search vs avg_recall and ef_search vs qps"
echo ""
echo "To re-run with different parameters:"
echo "  $0 [hostname]"