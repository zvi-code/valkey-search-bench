#!/bin/bash

# Full ef_search performance test with 10K queries
# Tests on the already indexed large_scale_25 dataset (1.18M vectors)

set -e

# HOST="ec-search-zvi-ec-1shard-no-tls-0001-001.ajfdds.0001.euw1devo.cache.amazonaws.com"
if [ -z "${HOST:-}" ]; then
    echo "ERROR: HOST environment variable not set"
    echo "Set HOST to the Valkey/Redis cluster endpoint"
    exit 1
fi
INDEX_NAME="large_scale_25"  # Use existing index with 1.18M vectors
DATASET_FILE="/home/ubuntu/valkey/build-debug/large_dataset.bin"
BENCHN="/home/ubuntu/valkey/build-debug/bin/valkey-benchmark"
CLI="/home/ubuntu/valkey/build-debug/bin/valkey-cli"
PREFIX="zvec_large_:"
NUM_QUERIES=1000
CONCURRENCY=20
THREADS=20

# 10 ef_search values from low to high
EF_SEARCH_VALUES=(1 20 50 75 100 125 150 200 250 300 400 500)

RESULTS_FILE="ef_search_full_results_$(date +%Y%m%d_%H%M%S).csv"

echo "=== Full ef_search Performance Test ==="
echo "Dataset: GloVe-25-angular (1.18M vectors)"
echo "Queries: $NUM_QUERIES per test"
echo "Index: $INDEX_NAME"
echo "ef_search values: ${EF_SEARCH_VALUES[*]}"
echo ""

# Check connection
# echo "Checking connection..."
# if ! timeout 5 $CLI -h "$HOST" -c --no-auth-warning PING > /dev/null 2>&1; then
#     echo "ERROR: Cannot connect to $HOST"
#     exit 1
# fi

NUM_DOCS=$("$CLI" -h "$HOST" FT.INFO "$INDEX_NAME" | grep -A 1 'num_docs' | tail -1 | awk '{print $1}')

if [ -z "$NUM_DOCS" ]; then
    echo "ERROR: Index $INDEX_NAME does not exist or cannot retrieve document count $NUM_DOCS"
    exit 1
fi
DOC_COUNT=$NUM_DOCS

echo "Index $INDEX_NAME contains $DOC_COUNT documents"

if [ "$DOC_COUNT" -lt "1000000" ]; then
    echo "ERROR: Index only has $DOC_COUNT documents, expected ~1.18M"
    echo "Please run the full dataset insertion first"
    exit 1
fi

echo ""
echo "Starting tests..."
echo ""

# Create CSV header
echo "ef_search,avg_recall,min_recall,max_recall,queries_processed,qps,avg_latency_ms,timestamp" > "$RESULTS_FILE"

# Results table header
echo "╔═══════════╦════════════╦════════════╦════════════╦═════════╦═══════════╗"
echo "║ ef_search ║ Avg Recall ║ Min Recall ║ Max Recall ║   QPS   ║  Latency  ║"
echo "╠═══════════╬════════════╬════════════╬════════════╬═════════╬═══════════╣"

for ef_search in "${EF_SEARCH_VALUES[@]}"; do
    echo -n "║ "
    printf "%-9s" "$ef_search"
    echo -n " ║"

    # Run the benchmark
    OUTPUT=$("$BENCHN" -h "$HOST" --cluster --rfr no --dataset "$DATASET_FILE" -t vec-query --search --vector-dim 25 --search-name "$INDEX_NAME" --search-prefix "$PREFIX" --ef-search "$ef_search" -n "$NUM_QUERIES" -c "$CONCURRENCY" --threads "$THREADS" 2>&1)

    # Extract statistics from the new output format
    AVG_RECALL=$(echo "$OUTPUT" | grep "Average:" | awk '{print $2}' | sed 's/%//')
    MIN_RECALL=$(echo "$OUTPUT" | grep "Min:" | awk '{print $2}' | sed 's/%//')
    MAX_RECALL=$(echo "$OUTPUT" | grep "Max:" | awk '{print $2}' | sed 's/%//')
    TOTAL_QUERIES=$(echo "$OUTPUT" | grep "Queries evaluated:" | awk '{print $3}')
    QPS=$(echo "$OUTPUT" | grep "throughput summary:" | awk '{print $3}')
    LATENCY=$(echo "$OUTPUT" | grep "avg       min" -A 1 | tail -1 | awk '{print $1}')

    # Default values if not found
    AVG_RECALL=${AVG_RECALL:-"N/A"}
    MIN_RECALL=${MIN_RECALL:-"N/A"}
    MAX_RECALL=${MAX_RECALL:-"N/A"}
    TOTAL_QUERIES=${TOTAL_QUERIES:-"0"}
    QPS=${QPS:-"N/A"}
    LATENCY=${LATENCY:-"N/A"}

    # Save to CSV
    echo "$ef_search,$AVG_RECALL,$MIN_RECALL,$MAX_RECALL,$TOTAL_QUERIES,$QPS,$LATENCY,$(date +%Y-%m-%d_%H:%M:%S)" >> "$RESULTS_FILE"

    # Display in table
    printf " %9s%% ║ %9s%% ║ %9s%% ║ %7s ║ %9s ║\n" \
        "$AVG_RECALL" "$MIN_RECALL" "$MAX_RECALL" "$QPS" "${LATENCY}ms"
done

echo "╚═══════════╩════════════╩════════════╩════════════╩═════════╩═══════════╝"

echo ""
echo "=== ANALYSIS ==="

# Find optimal configurations
BEST_RECALL=$(tail -n +2 "$RESULTS_FILE" | sort -t',' -k2 -nr | head -1)
BEST_QPS=$(tail -n +2 "$RESULTS_FILE" | sort -t',' -k6 -nr | head -1)

BEST_RECALL_EF=$(echo "$BEST_RECALL" | cut -d',' -f1)
BEST_RECALL_VALUE=$(echo "$BEST_RECALL" | cut -d',' -f2)
BEST_QPS_EF=$(echo "$BEST_QPS" | cut -d',' -f1)
BEST_QPS_VALUE=$(echo "$BEST_QPS" | cut -d',' -f6)

echo "Best configurations:"
echo "  Highest recall: ${BEST_RECALL_VALUE}% at ef_search=$BEST_RECALL_EF"
echo "  Highest QPS: ${BEST_QPS_VALUE} at ef_search=$BEST_QPS_EF"

echo ""
echo "Sweet spots (recall ≥ 80% with best QPS):"
tail -n +2 "$RESULTS_FILE" | awk -F',' '$2 >= 80 {printf "  ef_search=%s: %.1f%% recall, %.0f QPS, %.1fms latency\n", $1, $2, $6, $7}' | sort -t':' -k2 -nr | head -3

echo ""
echo "Results saved to: $RESULTS_FILE"
echo ""

# Generate a simple ASCII chart
echo "=== Recall vs ef_search (ASCII Chart) ==="
echo "ef_search | Recall %"
echo "----------+------------------------------------------------------------------------ 100%"
tail -n +2 "$RESULTS_FILE" | while IFS=',' read -r ef recall rest; do
    # Skip if recall is N/A or empty
    if [[ "$recall" == "N/A" ]] || [[ -z "$recall" ]]; then
        printf "  %-6s  | (no data)\n" "$ef"
        continue
    fi
    # Calculate bar length (scale to 70 chars for 100%)
    bar_length=$(echo "$recall * 0.7" | bc 2>/dev/null | cut -d'.' -f1)
    bar_length=${bar_length:-0}
    printf "  %-6s  | " "$ef"
    for ((i=1; i<=$bar_length; i++)); do echo -n "█"; done
    printf " %.1f%%\n" "$recall"
done
echo "----------+------------------------------------------------------------------------ 100%"

echo ""
echo "Test complete!"