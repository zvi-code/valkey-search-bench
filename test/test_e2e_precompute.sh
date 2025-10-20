#!/bin/bash
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Copyright (c) 2024-present, Zvi Schneider
#
# E2E Test: Validate precompute improves performance while maintaining recall
#
# This file is part of valkey-search-benchmark and is licensed under the
# BSD 3-Clause License. See the LICENSE file in the root directory.
#
# This script runs 3 tests:
# 1. Baseline WITHOUT precompute (lazy evaluation)
# 2. With --vgen-precompute flag (eager precomputation)
# 3. Validation: Compare performance and recall

set -e

# HOST="${HOST:-ec-search-zvi-ec-1shard-no-tls-0001-001.ajfdds.0001.euw1devo.cache.amazonaws.com}"
if [ -z "${HOST:-}" ]; then
    echo "ERROR: HOST environment variable not set"
    echo "Set HOST to the Valkey/Redis cluster endpoint"
    exit 1
fi
BENCHMARK="./bin/valkey-benchmark"

echo "=============================================="
echo "E2E Test: Ground Truth Precomputation"
echo "=============================================="
echo "Host: $HOST"
echo "Date: $(date)"
echo ""

# Common parameters
SEED=42
CAPACITY=1000000
DIMS=8
INDEX_NAME="new_8"
PREFIX="zvec_gen_8:"
GROUND_TRUTH_SIZE=50000
QUERY_COUNT=1000

echo "Configuration:"
echo "  Vector dimensions: $DIMS"
echo "  Ground truth vectors: $GROUND_TRUTH_SIZE"
echo "  Query count: $QUERY_COUNT"
echo "  Seed: $SEED"
echo ""

# Step 1: Clean database
echo "[Step 1/7] Cleaning database..."
./bin/valkey-cli -h $HOST -c --no-auth-warning FLUSHALL > /dev/null 2>&1
echo "✓ Database cleared"
echo ""

# Step 2: Create search index
echo "[Step 2/7] Creating search index..."
$BENCHMARK -h $HOST --cluster --rfr 'no' \
    --search --vector-dim $DIMS --search-name $INDEX_NAME --search-prefix $PREFIX \
    -t create-default-search-indexes -n 1 > /dev/null 2>&1
echo "✓ Index created"
echo ""

# Step 3: Ingest ground truth vectors
echo "[Step 3/7] Ingesting $GROUND_TRUTH_SIZE ground truth vectors..."
echo "Expected: ~8,000-10,000 req/s"
echo ""
$BENCHMARK -h $HOST --cluster --rfr 'no' \
    --use_vgen --vgen-seed $SEED --vgen-capacity $CAPACITY \
    -t vec-ground-truth --search --vector-dim $DIMS \
    --search-name $INDEX_NAME --search-prefix $PREFIX \
    -n $GROUND_TRUTH_SIZE -c 4 2>&1 | tee /tmp/vec_ingest.log | grep -E "(throughput summary|VG])"

INGEST_QPS=$(grep "throughput summary:" /tmp/vec_ingest.log | awk '{print $3}')
echo ""
echo "✓ Ingestion complete: $INGEST_QPS req/s"
echo ""

# Step 4: Test WITHOUT precompute (baseline)
echo "[Step 4/7] Running $QUERY_COUNT queries WITHOUT precompute (baseline)..."
echo "Expected: ~50-100 req/s (includes lazy ground truth computation)"
echo ""
$BENCHMARK -h $HOST --cluster --rfr 'no' \
    --use_vgen --vgen-seed $SEED --vgen-capacity $CAPACITY \
    -t vec-query --search --vector-dim $DIMS \
    --search-name $INDEX_NAME --search-prefix $PREFIX \
    -n $QUERY_COUNT -c 4 --threads 2 -r $GROUND_TRUTH_SIZE 2>&1 | tee /tmp/vec_query_baseline.log | grep -E "(throughput summary|Recall|VG])"

BASELINE_QPS=$(grep "throughput summary:" /tmp/vec_query_baseline.log | awk '{print $3}')
BASELINE_RECALL=$(grep "Average recall:" /tmp/vec_query_baseline.log | awk '{print $3}' | tr -d '%')
echo ""
echo "✓ Baseline test complete"
echo "  QPS: $BASELINE_QPS req/s"
echo "  Recall: $BASELINE_RECALL%"
echo ""

# Step 5: Clear database and re-ingest for second test
echo "[Step 5/7] Re-ingesting ground truth for precompute test..."
./bin/valkey-cli -h $HOST -c --no-auth-warning FLUSHALL > /dev/null 2>&1
$BENCHMARK -h $HOST --cluster --rfr 'no' \
    --search --vector-dim $DIMS --search-name $INDEX_NAME --search-prefix $PREFIX \
    -t create-default-search-indexes -n 1 > /dev/null 2>&1
$BENCHMARK -h $HOST --cluster --rfr 'no' \
    --use_vgen --vgen-seed $SEED --vgen-capacity $CAPACITY \
    -t vec-ground-truth --search --vector-dim $DIMS \
    --search-name $INDEX_NAME --search-prefix $PREFIX \
    -n $GROUND_TRUTH_SIZE -c 4 > /dev/null 2>&1
echo "✓ Ground truth re-ingested"
echo ""

# Step 6: Test WITH precompute
echo "[Step 6/7] Running $QUERY_COUNT queries WITH --vgen-precompute..."
echo "Expected warm-up: 10-30 seconds to precompute ground truths"
echo "Expected query performance: MUCH FASTER (no lazy computation)"
echo ""
$BENCHMARK -h $HOST --cluster --rfr 'no' \
    --use_vgen --vgen-seed $SEED --vgen-capacity $CAPACITY \
    --vgen-precompute \
    -t vec-query --search --vector-dim $DIMS \
    --search-name $INDEX_NAME --search-prefix $PREFIX \
    -n $QUERY_COUNT -c 4 --threads 2 -r $GROUND_TRUTH_SIZE 2>&1 | tee /tmp/vec_query_precompute.log | grep -E "(throughput summary|Recall|VG]|Warm-up|Precomputing)"

PRECOMPUTE_QPS=$(grep "throughput summary:" /tmp/vec_query_precompute.log | awk '{print $3}')
PRECOMPUTE_RECALL=$(grep "Average recall:" /tmp/vec_query_precompute.log | awk '{print $3}' | tr -d '%')
echo ""
echo "✓ Precompute test complete"
echo "  QPS: $PRECOMPUTE_QPS req/s"
echo "  Recall: $PRECOMPUTE_RECALL%"
echo ""

# Step 7: Validate results
echo "[Step 7/7] Validation Results"
echo "=============================================="
echo ""

# Calculate improvement
if [ -n "$BASELINE_QPS" ] && [ -n "$PRECOMPUTE_QPS" ]; then
    IMPROVEMENT=$(echo "scale=2; $PRECOMPUTE_QPS / $BASELINE_QPS" | bc)
    echo "Performance Improvement:"
    echo "  Baseline (lazy):     $BASELINE_QPS req/s"
    echo "  With precompute:     $PRECOMPUTE_QPS req/s"
    echo "  Speedup:             ${IMPROVEMENT}x faster"
    echo ""
    
    # Check if improvement is significant
    THRESHOLD="2.0"
    IMPROVED=$(echo "$IMPROVEMENT > $THRESHOLD" | bc)
    if [ "$IMPROVED" -eq 1 ]; then
        echo "✅ PERFORMANCE TEST PASSED: ${IMPROVEMENT}x improvement (threshold: ${THRESHOLD}x)"
    else
        echo "❌ PERFORMANCE TEST FAILED: Only ${IMPROVEMENT}x improvement (threshold: ${THRESHOLD}x)"
    fi
else
    echo "❌ PERFORMANCE TEST FAILED: Could not parse QPS values"
fi
echo ""

# Check recall is maintained
if [ -n "$BASELINE_RECALL" ] && [ -n "$PRECOMPUTE_RECALL" ]; then
    echo "Recall Accuracy:"
    echo "  Baseline (lazy):     $BASELINE_RECALL%"
    echo "  With precompute:     $PRECOMPUTE_RECALL%"
    echo ""
    
    # Check recall is within 1% (floating point comparison)
    RECALL_DIFF=$(echo "scale=2; ($PRECOMPUTE_RECALL - $BASELINE_RECALL)" | bc | tr -d '-')
    RECALL_OK=$(echo "$RECALL_DIFF < 1.0" | bc)
    
    if [ "$RECALL_OK" -eq 1 ]; then
        echo "✅ RECALL TEST PASSED: Recall maintained within 1% (diff: $RECALL_DIFF%)"
    else
        echo "❌ RECALL TEST FAILED: Recall difference too large: $RECALL_DIFF%"
    fi
else
    echo "❌ RECALL TEST FAILED: Could not parse recall values"
fi

echo ""
echo "=============================================="
echo "Test Summary"
echo "=============================================="
echo "Ingestion:       $INGEST_QPS req/s"
echo "Baseline Query:  $BASELINE_QPS req/s (recall: $BASELINE_RECALL%)"
echo "Precompute Query: $PRECOMPUTE_QPS req/s (recall: $PRECOMPUTE_RECALL%)"
echo "Speedup:         ${IMPROVEMENT}x"
echo ""
echo "Expected behavior:"
echo "  ✓ Precompute should be 2-10x faster than baseline"
echo "  ✓ Recall should be identical (within 1%)"
echo "  ✓ Warm-up phase shows progress (100 queries/update)"
echo ""

# Final status
if [ "$IMPROVED" -eq 1 ] && [ "$RECALL_OK" -eq 1 ]; then
    echo "🎉 ALL TESTS PASSED!"
    exit 0
else
    echo "❌ SOME TESTS FAILED"
    exit 1
fi
