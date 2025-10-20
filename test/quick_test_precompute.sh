#!/bin/bash
# Quick test: Compare query performance with/without precompute

# HOST="${HOST:-ec-search-zvi-ec-1shard-no-tls-0001-001.ajfdds.0001.euw1devo.cache.amazonaws.com}"
if [ -z "${HOST:-}" ]; then
    echo "ERROR: HOST environment variable not set"
    echo "Set HOST to the Valkey/Redis cluster endpoint"
    exit 1
fi
echo "=============================================="
echo "Quick Precompute Test"
echo "=============================================="
echo ""

# Clean and setup
echo "[1/4] Setup..."
./bin/valkey-cli -h $HOST -c --no-auth-warning FLUSHALL > /dev/null 2>&1
./bin/valkey-benchmark -h $HOST --cluster --rfr 'no' \
    --search --vector-dim 8 --search-name new_8 --search-prefix zvec_gen_8: \
    -t create-default-search-indexes -n 1 > /dev/null 2>&1
echo "✓ Database ready"
echo ""

# Ingest ground truth
echo "[2/4] Ingesting 50K ground truth vectors..."
./bin/valkey-benchmark -h $HOST --cluster --rfr 'no' --use_vgen --vgen-seed 42 --vgen-capacity 1000000 -t vec-ground-truth --search --vector-dim 8 --search-name new_8 --search-prefix zvec_gen_8: -n 50000 -c 4 2>&1 | grep "throughput summary"
echo ""

# Test WITHOUT precompute
echo "[3/4] Running 100 queries WITHOUT --vgen-precompute..."
./bin/valkey-benchmark -h $HOST --cluster --rfr 'no' \
    --use_vgen --vgen-seed 42 --vgen-capacity 1000000 \
    -t vec-query --search --vector-dim 8 \
    --search-name new_8 --search-prefix zvec_gen_8: \
    -n 100 -c 1 -r 50000 2>&1 | grep -E "(throughput|Recall)"
echo ""

# Re-ingest for clean test
echo "Re-ingesting for clean test..."
./bin/valkey-cli -h $HOST -c --no-auth-warning FLUSHALL > /dev/null 2>&1
./bin/valkey-benchmark -h $HOST --cluster --rfr 'no' \
    --search --vector-dim 8 --search-name new_8 --search-prefix zvec_gen_8: \
    -t create-default-search-indexes -n 1 > /dev/null 2>&1
./bin/valkey-benchmark -h $HOST --cluster --rfr 'no' \
    --use_vgen --vgen-seed 42 --vgen-capacity 1000000 \
    -t vec-ground-truth --search --vector-dim 8 \
    --search-name new_8 --search-prefix zvec_gen_8: \
    -n 50000 -c 4 > /dev/null 2>&1
echo ""

# Test WITH precompute
echo "[4/4] Running 100 queries WITH --vgen-precompute..."
echo "(Watch for warm-up phase progress)"
echo ""
./bin/valkey-benchmark -h $HOST --cluster --rfr 'no' \
    --use_vgen --vgen-seed 42 --vgen-capacity 1000000 \
    --vgen-precompute \
    -t vec-query --search --vector-dim 8 \
    --search-name new_8 --search-prefix zvec_gen_8: \
    -n 100 -c 1 -r 50000 2>&1 | grep -E "(throughput|Recall|Warm-up|VG]|Precomputing|Progress)"

echo ""
echo "=============================================="
echo "Done! Compare the throughput numbers above."
echo "Expected: WITH precompute should be 2-10x faster"
echo "=============================================="
