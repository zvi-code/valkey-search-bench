# ef_search Parameter Testing Guide

This guide explains how to test and optimize the `ef_search` parameter for HNSW vector search performance.

## Overview

The `ef_search` parameter controls the size of the dynamic candidate list during HNSW graph search:
- **Higher values**: Better recall, slower queries
- **Lower values**: Faster queries, lower recall

## Prerequisites

1. **Dataset must be loaded first**: Follow DATASET_TESTING_GUIDE.md steps 1-7
2. **Verify vectors are indexed**: Check that your index contains 1.18M documents
3. **Run from build-debug directory**: All scripts expect this location

## Quick Start

### 1. Quick Demo (30 seconds)
```bash
cd /home/ubuntu/valkey/build-debug
../demo_ef_search_simple.sh
```

**Output Example:**
```
| ef_search | Recall  | QPS     | Avg     | P99     |
|-----------|---------|---------|---------|---------|
| 50        |   70.6% |    2000 |    1.42 |    3.50 |
| 200       |   73.6% |    1992 |    2.02 |    4.21 |
| 500       |   74.1% |    2000 |    3.10 |    5.45 |
```

### 2. Full Analysis (5-10 minutes)
```bash
cd /home/ubuntu/valkey/build-debug
../test_ef_search_working.sh
```

**Output Example:**
```
| ef_search | Avg Recall | QPS     | Avg     | P50     | P95     | P99     | Max     |
|-----------|------------|---------|---------|---------|---------|---------|---------|
| 50        |       70.7 |    3968 |    1.29 |    1.30 |    1.61 |    2.02 |    3.11 |
| 200       |       73.7 |    3984 |    2.03 |    1.94 |    2.74 |    3.87 |    4.14 |
| 500       |       74.2 |    1996 |    3.31 |    2.98 |    5.28 |    6.22 |    7.81 |
```

## Customization

### Modify Test Parameters

Edit `test_ef_search_working.sh`:

```bash
# Configuration - EDIT THESE FOR YOUR SETUP
HOST="your-cluster-endpoint.com"
NUM_QUERIES=10000  # Adjust: 1000 for quick test, 10000 for full test
```

### Test Different ef_search Values

Edit the script to test specific values:
```bash
# Change this line in the script:
for ef_search in 50 100 150 200 250 300 400 500; do
# To test your specific values:
for ef_search in 100 200 400; do
```

## Results Analysis

### CSV Output

Results are saved to timestamped CSV files with complete metrics:
```
ef_search,avg_recall,min_recall,max_recall,qps,lat_avg,lat_p50,lat_p95,lat_p99,lat_max
50,70.7,0.0,100.0,3968.06,1.29,1.30,1.61,2.02,3.11
200,73.7,0.0,100.0,3984.06,2.03,1.94,2.74,3.87,4.14
```

### Performance Recommendations

| ef_search Range | Best For | Recall | Latency | Use Case |
|-----------------|----------|--------|---------|----------|
| 50-75 | High Throughput | 70-72% | ~1.5ms avg | Real-time apps, low latency requirements |
| 100-200 | Balanced | 73-74% | ~2ms avg | Most production workloads |
| 250-400 | High Accuracy | 74-75% | ~3ms avg | Quality-critical applications |
| 500+ | Maximum Accuracy | 74-75% | ~4ms+ avg | Offline analysis, diminishing returns |

### Interpreting Results

**Recall Metrics:**
- `avg_recall`: Average percentage of true neighbors found (most important)
- `min_recall`: Worst-case recall (check for outliers)
- `max_recall`: Best-case recall (usually 100%)

**Performance Metrics:**
- `qps`: Queries per second (higher is better)
- `lat_avg`: Average latency (lower is better)
- `lat_p99`: 99th percentile latency (tail latency)

**Decision Guidelines:**
1. **Start with ef_search=200** (good default)
2. **Increase to 300-400** if you need higher recall
3. **Decrease to 50-100** if you need higher throughput
4. **Monitor p99 latency** for tail latency requirements

## Troubleshooting

### Common Issues

1. **"Index not found" error**
   ```bash
   # Verify index exists
   ./bin/valkey-cli -h $HOST -c FT._LIST
   # Should show 'large_scale_25'
   ```

2. **"Dataset file not found" error**
   ```bash
   # Verify you're in build-debug directory
   pwd  # Should be /home/ubuntu/valkey/build-debug
   ls large_dataset.bin  # Should exist
   ```

3. **Connection errors**
   ```bash
   # Test connection
   ./bin/valkey-cli -h $HOST -c PING
   # Should return PONG
   ```

4. **Low recall across all ef_search values**
   - Check that dataset was fully loaded (1.18M vectors)
   - Verify index creation completed successfully
   - Consider running dataset insertion again

### Performance Tuning

**For Higher Throughput:**
```bash
# Reduce concurrency if hitting limits
# Edit script: -c 5 --threads 5 (instead of -c 10 --threads 10)
```

**For More Stable Results:**
```bash
# Increase query count for better averages
# Edit script: NUM_QUERIES=5000
```

## Understanding HNSW ef_search

### Technical Background

HNSW (Hierarchical Navigable Small World) uses ef_search to control the search beam width:

1. **Search Process**: Maintains dynamic candidate list during graph traversal
2. **ef_search Size**: Larger list → more exploration → better recall → slower search
3. **Trade-off**: Accuracy vs Speed is fundamental to approximate nearest neighbor search

### Expected Behavior

- **Linear recall improvement**: ef_search 50→200 gives bigger gains than 200→500
- **Diminishing returns**: Beyond ef_search=300, gains are minimal
- **Dataset dependent**: Optimal values vary by data distribution and index parameters

### Production Considerations

- **Monitor p95/p99 latency**: Not just average
- **Test with your queries**: Different query patterns may behave differently
- **Consider query caching**: For repeated queries
- **Benchmark regularly**: Performance can change with index growth

This testing framework provides the foundation for optimizing HNSW performance for your specific use case and requirements.