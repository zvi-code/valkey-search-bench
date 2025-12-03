# Benchmarking Guide

Complete guide for running vector search benchmarks with Valkey.

## Quick Start

### Basic Benchmark Workflow

1. **Load vectors into Valkey** (ground truth phase)
2. **Run query benchmarks** with recall validation
3. **Analyze results** (QPS, latency, recall)

### Example: COHERE 1M Dataset

```bash
cd build

# Step 1: Load all vectors into Valkey
./bin/valkey-benchmark \
  -h localhost --cluster --rfr no \
  --dataset cohere-medium-1m.bin \
  -t vec-load \
  --search --vector-dim 768 \
  --search-name cohere_1m --search-prefix zvec_: \
  -n 1000000 -c 10 --clean

# Step 2: Run query benchmark
./bin/valkey-benchmark \
  -h localhost --cluster --rfr no \
  --dataset cohere-medium-1m.bin \
  -t vec-query \
  --search --vector-dim 768 \
  --search-name cohere_1m --search-prefix zvec_: \
  -n 10000 -c 10 --threads 10
```

## Understanding Command Options

### Connection Options

```bash
-h <host>           # Valkey/Redis host (default: localhost)
-p <port>           # Port (default: 6379)
--cluster           # Enable cluster mode
--rfr no            # Disable redirect following (cluster mode)
```

### Dataset Options

```bash
--dataset <file>    # Path to binary dataset file
--vector-dim <n>    # Vector dimensions (must match index)
```

### Index Options

```bash
--search                    # Enable vector search mode
--search-name <name>        # Index name (e.g., cohere_1m)
--search-prefix <prefix>    # Key prefix (e.g., zvec_:)
--search-query-params <json># Index creation params (for ground-truth phase)
```

**Example index params:**
```bash
--search-query-params '{"TYPE":"FLOAT32","DIM":"768","DISTANCE_METRIC":"COSINE","INITIAL_CAP":"1000000"}'
```

### Performance Options

```bash
-n <count>          # Number of operations
-c <clients>        # Number of parallel clients
--threads <n>       # Number of client threads
--pipeline <n>      # Pipeline requests (default: 1)
```

### Utility Options

```bash
--clean             # Delete all keys before benchmark (WARNING: destructive!)
--verbose           # Show detailed progress
--csv <file>        # Save results to CSV
```

## Session-Specific Configuration Persistence

When running multiple benchmarks from different terminals or tmux sessions, configurations are automatically isolated per session to prevent conflicts.

### How It Works

The benchmark automatically detects your terminal or tmux session and creates unique configuration files:

- **Tmux sessions**: Each tmux pane gets its own config (e.g., `config-tmux-1.conf`, `config-tmux-2.conf`)
- **Terminal sessions**: Each terminal window gets its own config (e.g., `config-term12345.conf`)
- **Same session**: Multiple runs in the same terminal/tmux pane share the same config file

### Configuration File Locations

1. **Working directory** (highest priority): `.valkey-benchmark-<session>.conf`
2. **User global directory**: `~/.valkey-benchmark/config-<session>.conf`

### Usage Examples

**Example 1: Running benchmarks in different tmux panes**

```bash
# Tmux pane 1 - Testing COHERE dataset
./bin/valkey-benchmark \
  --dataset cohere-medium-1m.bin \
  -t vec-query --search --vector-dim 768 \
  --search-name cohere_1m -n 10000 -c 20
# Config saved to: ~/.valkey-benchmark/config-tmux-1.conf

# Tmux pane 2 - Testing OPENAI dataset (runs independently)
./bin/valkey-benchmark \
  --dataset openai-large-5m.bin \
  -t vec-query --search --vector-dim 1536 \
  --search-name openai_5m -n 10000 -c 40
# Config saved to: ~/.valkey-benchmark/config-tmux-2.conf
```

**Example 2: Reusing configuration in the same session**

```bash
# Terminal 1 - First run with full parameters
./bin/valkey-benchmark \
  --dataset sift-128.bin \
  -t vec-query --search --vector-dim 128 \
  --search-name sift_index -n 10000 -c 10 --threads 4

# Terminal 1 - Subsequent runs reuse saved config
./bin/valkey-benchmark  # Automatically loads previous settings
# Config loaded from: ~/.valkey-benchmark/config-term54321.conf
```

**Example 3: Manual session naming**

If you need explicit control over session naming:

```bash
# Terminal 1 - Experiment A
export VALKEY_BENCHMARK_SESSION=experiment_a
./bin/valkey-benchmark --dataset foo.bin ...
# Config: ~/.valkey-benchmark/config-experiment-a.conf

# Terminal 2 - Experiment B
export VALKEY_BENCHMARK_SESSION=experiment_b
./bin/valkey-benchmark --dataset bar.bin ...
# Config: ~/.valkey-benchmark/config-experiment-b.conf
```

### Managing Configurations

**View current configuration:**
```bash
./bin/valkey-benchmark --show-config
```

**Clear current session's configuration:**
```bash
./bin/valkey-benchmark --clear-config
```

**List all saved configurations:**
```bash
ls -la ~/.valkey-benchmark/
```

### Benefits

- **No conflicts**: Run different benchmarks in parallel without configurations interfering
- **Automatic persistence**: Settings are saved and restored automatically
- **Easy experimentation**: Switch between terminals/panes to compare different configurations
- **Clean isolation**: Each benchmark session maintains its own history

## Benchmark Phases

### Phase 1: Ground Truth Loading (vec-load)

Loads all training vectors into Valkey and creates the index.

```bash
./bin/valkey-benchmark \
  --dataset sift-128.bin \
  -t vec-load \
  --search --vector-dim 128 \
  --search-name sift_index \
  --search-prefix vec_: \
  --search-query-params '{"TYPE":"FLOAT32","DIM":"128","DISTANCE_METRIC":"L2","INITIAL_CAP":"1000000"}' \
  -n 1000000 -c 10 --clean
```

**What happens:**
1. Creates index with specified parameters
2. Inserts all vectors with ID mapping
3. Reports insertion throughput

**Typical output:**
```
Ground Truth Insertion: 1000000 vectors
Throughput: 12,543 ops/sec
Latency: avg=0.80ms, p50=0.75ms, p99=1.50ms
```

### Phase 2: Query Benchmark (vec-query)

Runs test queries and validates recall against ground truth.

```bash
./bin/valkey-benchmark \
  --dataset sift-128.bin \
  -t vec-query \
  --search --vector-dim 128 \
  --search-name sift_index \
  --search-prefix vec_: \
  -n 10000 -c 10 --threads 10
```

**What happens:**
1. Runs k-NN queries from test set
2. Compares results to pre-computed ground truth
3. Reports QPS, latency, and recall

**Typical output:**
```
Query Performance: 10000 queries
QPS: 1,892
Latency: avg=5.29ms, p50=5.12ms, p95=7.21ms, p99=8.93ms

Recall Analysis:
  Average recall: 95.3%
  Min recall: 78.0%
  Max recall: 100.0%
  Queries below 90%: 523 (5.2%)
```

## Parameter Tuning: ef_search

The `ef_search` parameter controls the HNSW search quality vs speed trade-off.

### Quick Demo (30 seconds)

```bash
cd build
../test/demo_ef_search_simple.sh
```

**Example output:**
```
| ef_search | Recall  | QPS     | Avg Latency |
|-----------|---------|---------|-------------|
| 50        |   70.6% |    2000 |    1.42 ms  |
| 200       |   73.6% |    1992 |    2.02 ms  |
| 500       |   74.1% |    2000 |    3.10 ms  |
```

### Full Analysis (5-10 minutes)

```bash
cd build
../test/test_ef_search_working.sh
```

Tests multiple ef_search values and generates detailed metrics.

### Manual ef_search Testing

Modify index creation to test specific ef_search values:

```bash
# Create index with ef_search=100
./bin/valkey-benchmark \
  --search-query-params '{"TYPE":"FLOAT32","DIM":"768","DISTANCE_METRIC":"COSINE","EF_RUNTIME":"100"}' \
  ...
```

**Recommended values:**

| ef_search | Recall | Latency | Use Case |
|-----------|--------|---------|----------|
| 50-75 | 70-72% | ~1.5ms | High throughput, low latency |
| 100-200 | 73-74% | ~2ms | **Balanced (recommended)** |
| 250-400 | 74-75% | ~3ms | High accuracy |
| 500+ | 74-75% | ~4ms+ | Maximum accuracy (diminishing returns) |

## Automatic Optimization

The optimizer automatically finds the best configuration.

### Basic Optimization

```bash
./bin/valkey-benchmark --optimize \
  --optimize-objective "maximize:qps" \
  --optimize-constraint "recall_avg:gt:0.95" \
  --dataset cohere-medium-1m.bin \
  -t vec-query \
  --search --vector-dim 768 \
  --search-name cohere_1m \
  -h localhost --cluster
```

**What the optimizer does:**
1. **RECALL phase**: Binary search for minimal `ef_search` satisfying recall constraint (~7 iterations)
2. **THROUGHPUT phase**: Grid search for optimal `clients` and `threads` (~20-30 iterations)
3. **HILL_CLIMB phase**: Fine-tune all parameters with gradient descent (~10-15 iterations)

**Example output:**
```
[Optimizer] Starting optimization...

[RECALL Phase] Binary search for ef_search
  Iteration 1: ef_search=200 → recall=0.931 (below target)
  Iteration 2: ef_search=400 → recall=0.972 (above target)
  Iteration 3: ef_search=300 → recall=0.951 (acceptable)
  → Locked ef_search=300

[THROUGHPUT Phase] Grid search for clients, threads
  [clients] Testing: 10 → 20 → 40 → 80 → 160 → 320
  → Best: clients=160 (QPS=5,700)
  
  [threads] Testing: 0 → 1 → 2 → 4 → 8
  → Best: threads=2 (QPS=5,850)

[HILL_CLIMB Phase] Fine-tuning...
  Iteration 1: clients=170, threads=2 → QPS=5,920 (improved)
  Iteration 2: clients=180, threads=2 → QPS=5,880 (worse)
  → Converged

Final Configuration:
  ef_search: 300
  clients: 170
  threads: 2
  QPS: 5,920
  Recall: 95.1%
```

### Optimization Options

```bash
--optimize                              # Enable optimizer
--optimize-objective <target>           # "maximize:qps" or "minimize:lat_avg"
--optimize-constraint <condition>       # e.g., "recall_avg:gt:0.95"
--optimize-csv <file>                   # Save all iterations to CSV
--optimize-max-iterations <n>           # Max iterations (default: 100)
```

**Multiple constraints:**
```bash
--optimize-constraint "recall_avg:gt:0.95" \
--optimize-constraint "lat_p99:lt:10.0"
```

## Interpreting Results

### Throughput Metrics

- **QPS (Queries Per Second)**: Higher is better
  - Good: >1,000 QPS
  - Excellent: >5,000 QPS

### Latency Metrics

- **avg**: Average latency
- **p50**: Median (50th percentile)
- **p95**: 95th percentile (5% of queries are slower)
- **p99**: 99th percentile (1% of queries are slower)
- **max**: Worst case

**What to watch:**
- High p99/p50 ratio: Inconsistent performance
- p99 > 10× avg: Possible outliers or GC pauses

### Recall Metrics

- **Average recall**: Most important - percentage of true neighbors found
- **Min recall**: Worst case (check for anomalies)
- **Max recall**: Usually 100%
- **Queries below threshold**: How many queries have poor recall

**Target recall:**
- Production: ≥95%
- High quality: ≥98%
- Research: ≥99%

## Common Benchmark Scenarios

### Scenario 1: Quick Performance Check

```bash
# Test with 1,000 queries
./bin/valkey-benchmark \
  --dataset mnist.bin \
  -t vec-query \
  --search --vector-dim 784 \
  --search-name mnist \
  -n 1000 -c 10
```

### Scenario 2: Production Simulation

```bash
# High concurrency, long duration
./bin/valkey-benchmark \
  --dataset cohere-medium-1m.bin \
  -t vec-query \
  --search --vector-dim 768 \
  --search-name prod_test \
  -n 100000 -c 100 --threads 10
```

### Scenario 3: Latency-Optimized

```bash
# Low concurrency, measure tail latency
./bin/valkey-benchmark \
  --dataset sift-128.bin \
  -t vec-query \
  --search --vector-dim 128 \
  --search-name low_latency \
  -n 10000 -c 1 --threads 1
```

### Scenario 4: Throughput-Optimized

```bash
# High concurrency, maximize QPS
./bin/valkey-benchmark \
  --dataset gist-960.bin \
  -t vec-query \
  --search --vector-dim 960 \
  --search-name high_qps \
  -n 50000 -c 200 --threads 20 --pipeline 10
```

## Multi-Dataset Testing

Test multiple datasets in sequence:

```bash
cd build
../bench/test_multi_dataset.sh
```

This script tests several datasets and generates comparison CSV.

## Filtered Search Benchmarks

For datasets with metadata (like YFCC-10M):

```bash
./bin/valkey-benchmark \
  --dataset yfcc-10m.bin \
  --filtered \
  -t vec-query \
  --search --vector-dim 192 \
  --search-name yfcc \
  -n 10000 -c 10
```

The `--filtered` flag enables metadata-aware ground truth matching.

## Troubleshooting

### Low Recall

**Problem:** Recall < 90%

**Solutions:**
1. Increase ef_search: `--search-query-params '{"EF_RUNTIME":"400"}'`
2. Check vector normalization (for COSINE metric)
3. Verify dataset integrity: `./prep_datasets/dataset.sh verify dataset.bin`

### Low QPS

**Problem:** QPS < expected

**Solutions:**
1. Increase concurrency: `-c 50 --threads 10`
2. Use pipelining: `--pipeline 10`
3. Check cluster distribution: Ensure vectors spread across shards
4. Monitor server resources: CPU, memory, network

### High Latency

**Problem:** p99 latency very high

**Solutions:**
1. Reduce ef_search for faster queries
2. Decrease concurrency to reduce contention
3. Check server load and GC pauses
4. Verify network latency: `ping <server>`

### Connection Errors

**Problem:** Connection refused or timeouts

**Solutions:**
```bash
# Verify server is running
redis-cli -h localhost -p 6379 ping

# Check cluster status
redis-cli -h localhost -p 6379 cluster info

# Test basic connectivity
./bin/valkey-benchmark -h localhost -t ping -n 1000
```

### Memory Issues

**Problem:** Server OOM during ground truth loading

**Solutions:**
1. Load in smaller batches: Split dataset
2. Increase server memory
3. Use compression: `FT.CREATE ... COMPRESSION ON`
4. Reduce INITIAL_CAP in index params

## Performance Tips

### Optimal Concurrency

Rule of thumb: `clients = 10-20 × num_cores`

```bash
# For 8-core server
-c 100 --threads 10
```

### Cluster Considerations

- Ensure even shard distribution
- Use cluster-aware clients
- Test with `--cluster --rfr no`

### Monitoring

Watch server metrics during benchmarks:

```bash
# Monitor in separate terminal
watch -n 1 'redis-cli -h localhost INFO memory'
watch -n 1 'redis-cli -h localhost INFO stats'
```

## CSV Output Analysis

Save results for analysis:

```bash
./bin/valkey-benchmark \
  --dataset cohere-medium-1m.bin \
  -t vec-query \
  --csv results.csv \
  ...
```

**CSV columns:**
```
test,ops/sec,avg_latency_ms,min_latency_ms,p50_latency_ms,p95_latency_ms,p99_latency_ms,max_latency_ms,avg_recall,min_recall,max_recall
```

Load in spreadsheet or Python for analysis:

```python
import pandas as pd
df = pd.read_csv('results.csv')
print(df.describe())
```

## Next Steps

- **Advanced Features**: See [ADVANCED.md](ADVANCED.md) for optimizer internals and metadata filtering
- **Dataset Management**: See [DATASETS.md](DATASETS.md) for more datasets
- **Installation**: See [INSTALLATION.md](INSTALLATION.md) for environment setup
