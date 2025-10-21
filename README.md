# Valkey Vector Benchmark 
[This is still a work in progress. Feedback and contributions are welcome!]

A standalone benchmarking toolkit for evaluating vector search (and other valkey workloads) performance in Valkey and Redis clusters, with support for standard vectordb-bench datasets and comprehensive recall validation.

## Overview

This package extracts the vector search benchmarking capabilities from the Valkey project into a standalone tool. It provides:
s
- **Dataset Pipeline**: Download, convert, and prepare vectordb-bench datasets (COHERE, OPENAI, SIFT, GIST, LAION, BIGANN)
- **Binary Format**: Custom 4KB-aligned binary format with precomputed ground truth for recall validation
- **Cluster Support**: Parallel cluster scanning with vector ID to cluster tag mapping
- **Recall Validation**: Accurate recall measurement with missing neighbor reconstruction
- **Performance Testing**: Throughput (QPS), latency (p50/p99), and recall analysis
- **Baseline Network Latency**: Automatic network RTT measurement to separate network overhead from processing time
- **Runtime Configuration**: Apply server-side configurations before benchmarks for automated testing
- **Adaptive Optimization**: Automatic parameter tuning with multi-phase optimization (binary search + grid search)
- **Parameter Tuning**: ef_search parameter sweep and exhaustive exploration

## Quick Start

### Prerequisites

```bash
# System dependencies
sudo apt-get install cmake gcc python3 python3-venv

# Python environment for dataset conversion
python3 -m venv venv
source venv/bin/activate
pip install vectordb-bench==1.0.10 h5py pandas pyarrow numpy
```

**Important**: This benchmark requires jemalloc. See [INSTALLATION.md](INSTALLATION.md) for complete setup instructions.

### Build

```bash
# First-time setup: Run the jemalloc setup script (auto-detects Valkey build)
./setup_jemalloc.sh
# Or manually specify Valkey build path:
# ./setup_jemalloc.sh /path/to/valkey/build-release

# Build benchmark
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make valkey-benchmark -j$(nproc)

# Verify jemalloc is linked
nm bin/valkey-benchmark | grep je_malloc
```

For detailed jemalloc setup options, see [INSTALLATION.md](INSTALLATION.md#2-setup-jemalloc-required).

### Download and Convert Dataset

**New Unified Dataset Manager** (recommended):

```bash
# List all available datasets
./scripts/dataset.sh list

# Download and convert in one command
./scripts/dataset.sh get cohere-medium-1m

# Or for datasets with metadata (filtered search)
./scripts/dataset.sh get yfcc-10m
```

**Legacy method** (still works):

```bash
# Download COHERE 1M dataset
python prep_datasets/download_dataset.py COHERE 1000000

# Convert to binary format (see prep_datasets/ for conversion scripts)
python prep_datasets/convert_parquet_to_hdf5.py \
    /mnt/data/datasets/cohere/cohere_medium_1m \
    cohere-medium-1m.hdf5 \
    --name cohere-medium-1m
```

### Run Benchmark

```bash
# Phase 1: Insert ground truth (all vectors)
./bin/valkey-benchmark -h localhost --cluster --rfr no \
  --dataset cohere-medium-1m.bin \
  -t vec-load --search --vector-dim 768 \
  --search-name cohere_1m --search-prefix zvec_: \
  -n 1000000 -c 10 --clean

# Phase 2: Query benchmark with recall validation
./bin/valkey-benchmark -h localhost --cluster --rfr no \
  --dataset cohere-medium-1m.bin \
  -t vec-query --search --vector-dim 768 \
  --search-name cohere_1m --search-prefix zvec_: \
  -n 10000 -c 10 --threads 10
```

### Adaptive Optimization (Automatic Parameter Tuning)

```bash
# Automatically find optimal configuration
./bin/valkey-benchmark --optimize \
  --optimize-objective "maximize:qps" \
  --optimize-constraint "recall_avg:gt:0.95" \
  -h localhost --cluster --rfr no \
  --dataset cohere-medium-1m.bin \
  -t vec-query --search --vector-dim 768 \
  --search-name cohere_1m --search-prefix zvec_: \
  --optimize-csv results.csv
```

**What the optimizer does:**
1. **Binary search** for minimal `ef_search` satisfying recall constraints (~7 iterations)
2. **Grid search** for optimal `clients` and `threads` for max QPS (~20 iterations)
3. **Fine-tuning** with gradient descent for final adjustments
4. Exports complete optimization history to CSV

## Documentation

📚 **Complete documentation organized in 5 guides:**

1. **[Installation Guide](docs/INSTALLATION.md)** - Setup, dependencies, and building
2. **[Dataset Guide](docs/DATASETS.md)** - Downloading, converting, and managing datasets
3. **[Benchmarking Guide](docs/BENCHMARKING.md)** - Running benchmarks and interpreting results
4. **[Advanced Guide](docs/ADVANCED.md)** - Optimizer internals, metadata filtering, data formats
5. **[Runtime Configuration](RUNTIME_CONFIG.md)** - Server-side configuration management

**Quick navigation:**
- New to the project? Start with [Installation](docs/INSTALLATION.md)
- Need datasets? See [Dataset Guide](docs/DATASETS.md)
- Running benchmarks? Check [Benchmarking Guide](docs/BENCHMARKING.md)
- Advanced features? Read [Advanced Guide](docs/ADVANCED.md)
- Server configuration? See [Runtime Configuration](RUNTIME_CONFIG.md)

## Features

### Dataset Support

**Unified Dataset Manager** - One command to download and convert any dataset:

```bash
./scripts/dataset.sh list                    # Show all datasets
./scripts/dataset.sh get <dataset-name>      # Download + convert
./scripts/dataset.sh verify datasets/*.bin   # Verify integrity
```

**Preconfigured Datasets:**

| Source | Dataset | Dimensions | Vectors | Distance | Description |
|--------|---------|-----------|---------|----------|-------------|
| **ANN-Benchmarks** | sift-128 | 128 | 1M | L2 | SIFT image descriptors |
| | gist-960 | 960 | 1M | L2 | GIST image descriptors |
| | glove-25/50/100 | 25-100 | 1.18M | COSINE | GloVe word embeddings |
| | mnist | 784 | 60K | L2 | MNIST digits |
| | fashion-mnist | 784 | 60K | L2 | Fashion images |
| | deep-96 | 96 | 10M | COSINE | Deep1B subset |
| **BigANN** | bigann-10m | 128 | 10M | L2 | SIFT 10M subset |
| | deep-10m | 256 | 10M | L2 | Deep-1B 10M subset |
| **BigANN+Metadata** | yfcc-10m | 192 | 10M | L2 | **200K tags** (filtered search) |
| **VectorDBBench** | cohere-small-100k | 768 | 100K | COSINE | Cohere embeddings |
| | cohere-medium-1m | 768 | 1M | COSINE | Cohere embeddings |
| | cohere-large-10m | 768 | 10M | COSINE | Cohere embeddings |
| | openai-medium-500k | 1536 | 500K | COSINE | OpenAI embeddings |
| | openai-large-5m | 1536 | 5M | COSINE | OpenAI embeddings |

**Metadata Filtering** (NEW):
- `yfcc-10m` includes 200,386 tags for filtered vector search
- Use with `--filtered` flag for metadata-aware benchmarking
- See [Metadata Filtering Guide](#metadata-filtering-support)

### Performance Metrics

- **Throughput**: Queries per second (QPS)
- **Latency**: p50, p95, p99, p99.9 percentiles
- **Recall**: Percentage of ground truth neighbors found
- **Cluster Scanning**: 1M+ keys/second with parallel workers

### Key Capabilities

✅ **Unified dataset manager** - One tool for all download/conversion operations
✅ Two-phase benchmarking (ground truth insertion + query testing)
✅ **Baseline network latency measurement** - Automatic RTT measurement (enabled by default)
✅ **Metadata filtering support** - Tag-based filtered vector search (YFCC-10M)
✅ **Configuration persistence** - Automatic saving and loading of benchmark parameters
✅ Cluster mode support with read-from-replica options
✅ Parallel cluster scanning for recall validation
✅ Vector ID to cluster tag mapping for missing neighbor reconstruction
✅ **Adaptive load optimizer with multi-phase optimization**
✅ **Binary search for recall optimization (ef_search tuning)**
✅ **Grid search for throughput optimization (clients/threads tuning)**
✅ HDR histogram for accurate latency percentiles
✅ Progress bars and real-time monitoring
✅ Comprehensive result analysis and CSV export  

## Metadata Filtering Support

**NEW:** Benchmark filtered vector search with metadata predicates (BigANN NeurIPS 2023 Filtered Search Track).

### YFCC-10M Dataset with Metadata

The `yfcc-10m` dataset includes 200,386 tags (image descriptions, camera models, years, countries) for testing filtered search:

```bash
# Download YFCC-10M with metadata (2.6GB download → 8.1GB binary)
./scripts/dataset.sh get yfcc-10m

# Verify metadata is loaded
./scripts/dataset.sh verify datasets/yfcc-10m.bin
```

### Running Filtered Search Benchmarks

Use the `--filtered` flag to enable metadata-aware recall calculation:

```bash
# Benchmark with metadata filtering
./bin/valkey-benchmark -h localhost --cluster \
  --dataset yfcc-10m.bin \
  --filtered \
  -t vec-query --search --vector-dim 192 \
  --search-name yfcc_10m --search-prefix zvec_: \
  -n 10000 -c 10
```

**How it works:**
1. Queries have 1-2 tag predicates (e.g., "camera_Canon AND year_2015")
2. Ground truth is filtered to only include vectors matching those tags
3. Recall is computed against filtered ground truth
4. Simulates real-world filtered search scenarios

**Dataset Statistics:**
- 10M vectors with 192-dim CLIP embeddings
- 200,386 unique tags
- 108M tag assignments (~11 tags per vector avg)
- 100K queries with 138K predicates (~1.4 predicates per query)

**Note:** Currently, the `--filtered` flag affects recall calculation only. Full integration (adding metadata to HSET commands and predicates to FT.SEARCH queries) is documented in `METADATA_IMPLEMENTATION_STATUS.md`. See [Advanced Guide - Metadata Filtering](docs/ADVANCED.md#metadata-filtering) for complete details.

## Adaptive Load Optimizer

The optimizer automatically tunes benchmark parameters to achieve your performance goals:

### Optimization Phases

1. **INIT** - Collect baseline measurements
2. **FEASIBILITY** - Find any configuration that works
3. **RECALL** - Binary search for minimal `ef_search` satisfying recall constraints
4. **THROUGHPUT** - Grid search for optimal `clients` and `threads`
   - Coarse phase: Exponential steps (10 → 20 → 40 → 80 → 160...)
   - Fine phase: Linear steps around best value (±2 steps)
5. **HILL_CLIMB** - Gradient descent for multi-parameter fine-tuning
6. **REFINEMENT** - Coordinate descent for final adjustments

### Usage Examples

```bash
# Maximize QPS while maintaining recall ≥ 0.95
./bin/valkey-benchmark --optimize \
  --optimize-objective "maximize:qps" \
  --optimize-constraint "recall_avg:gt:0.95" \
  -h localhost --cluster --dataset cohere-medium-1m.bin \
  -t vec-query --search --vector-dim 768

# Minimize p99 latency while maintaining recall ≥ 0.90
./bin/valkey-benchmark --optimize \
  --optimize-objective "minimize:p99_latency" \
  --optimize-constraint "recall_avg:gt:0.90" \
  -h localhost --cluster --dataset cohere-medium-1m.bin \
  -t vec-query --search --vector-dim 768

# Multiple constraints
./bin/valkey-benchmark --optimize \
  --optimize-objective "maximize:qps" \
  --optimize-constraint "recall_avg:gt:0.95" \
  --optimize-constraint "p99_latency:lt:10.0" \
  --optimize-csv optimization_results.csv \
  -h localhost --cluster --dataset cohere-medium-1m.bin \
  -t vec-query --search --vector-dim 768
```

### Available Metrics

**Objectives** (maximize or minimize):
- `qps` - Queries per second
- `avg_latency`, `p50_latency`, `p90_latency`, `p95_latency`, `p99_latency`, `max_latency`
- `recall_avg`, `recall_min`, `recall_max`

**Constraints** (gt or lt):
- Same as objectives
- Example: `recall_avg:gt:0.95` means "recall must be greater than 0.95"

See [Advanced Guide - Optimizer Internals](docs/ADVANCED.md#optimizer-internals) for algorithm details.

## Configuration Persistence

The benchmark tool now supports automatic configuration persistence to reduce repetitive command-line arguments and maintain configuration state between runs.

### How It Works

- **Automatic Saving**: Configuration is automatically saved after successful runs
- **Smart Loading**: Saved configuration is loaded as defaults for subsequent runs
- **Selective Override**: Command-line arguments override saved values
- **Transient Exclusion**: Connection-specific options (`-t`, `-h`) are never persisted

### Configuration File Locations

The tool checks for configuration in this order:

1. **Working directory**: `./.valkey-benchmark.conf` (current directory)
2. **User-global**: `~/.valkey-benchmark/config.conf` (home directory)

**Session Isolation**: Set `VALKEY_BENCHMARK_SESSION=name` for session-specific configs:
- Working directory: `./.valkey-benchmark-{session}.conf`
- User-global: `~/.valkey-benchmark/config-{session}.conf`

### Usage Examples

```bash
# First run - specify all options (gets saved automatically)
./bin/valkey-benchmark -h localhost --cluster --rfr no \
  --dataset cohere-medium-1m.bin \
  -t vec-query --search --vector-dim 768 \
  --search-name cohere_1m --search-prefix zvec_: \
  -n 10000 -c 10 --threads 4

# Second run - reuses saved configuration
./bin/valkey-benchmark -t vec-query

# Override specific parameters while keeping others
./bin/valkey-benchmark -t vec-query -c 20 --threads 8

# View current saved configuration
./bin/valkey-benchmark --show-config

# Clear saved configuration
./bin/valkey-benchmark --clear-config

# Run without saving this configuration
./bin/valkey-benchmark -t vec-query --dataset test-dataset --no-save-config

# Session isolation - different sessions don't interfere
export VALKEY_BENCHMARK_SESSION=experiment1
./bin/valkey-benchmark -t vec-query -c 10 --threads 4

export VALKEY_BENCHMARK_SESSION=experiment2
./bin/valkey-benchmark -t vec-query -c 20 --threads 8

# Each session maintains its own config independently
```

### Configuration Management Flags

- `--save-config`: Force save configuration after this run
- `--no-save-config`: Skip saving configuration for this run
- `--clear-config`: Delete saved configuration and exit
- `--show-config`: Display current saved configuration and exit

### Saved Parameters

The following parameters are automatically persisted:

**Basic Parameters:**
- Number of clients (`-c`)
- Number of threads (`--threads`)
- Pipeline size (`-P`)
- Number of requests (`-n`)
- Key space length (`-r`)
- Database number (`--dbnum`)
- Output format (`--csv`)
- Loop mode (`-l`)
- Precision (`--precision`)
- Cluster mode (`--cluster`)
- RESP3 mode (`-3`)

**Search Parameters:**
- Dataset (`--dataset`)
- Search index name (`--search-name`)
- Vector algorithm (`--search-alg`)
- Vector field name (`--vector-field`)
- Vector dimensions (`--vector-dim`)
- Tag and numeric fields (`--tag-field`, `--numeric-field`)
- Search parameters (`--ef-search`, `--ef-construction`, `--m`, `--k`)
- Distance metric (`--metric`)
- Search options (`--nocontent`, `--localonly`)
- Filtered search (`--filtered`)

**Optimizer Parameters:**
- Optimization mode (`--optimize`)
- Optimization objective (`--optimize-objective`)
- Constraints (`--optimize-constraint`)
- CSV output (`--optimize-csv`)
- Iteration limits (`--optimize-max-iterations`, `--optimize-min-requests`)

**Authentication & TLS:**
- Authentication (`-a`, `--user`)
- TLS certificates and settings

### Benefits

- **Faster Iteration**: No need to retype long command lines
- **Consistent Testing**: Ensures consistent parameters across benchmark runs
- **Session Isolation**: Multiple experiments can run independently with separate configs
- **Team Workflows**: Share workspace-local configs via version control
- **Parameter Memory**: Never lose working configurations

## Baseline Network Latency Measurement

The benchmark tool automatically measures baseline network latency to help you separate network RTT overhead from operation-specific processing time. This feature is **enabled by default** and runs silently before your benchmarks.

### How It Works

- **Automatic Measurement**: Runs 10,000 PING operations (single-threaded, single-client) before benchmarks
- **Silent Operation**: No output during measurement - results shown only in final reports
- **Accurate Baseline**: Measures pure network RTT + minimal Redis/Valkey processing overhead
- **Processing Overhead**: Shows `operation latency - baseline latency` breakdown

### Example Output

**Non-CSV Mode:**
```
Summary:
  throughput summary: 100000.00 requests per second
  latency summary (msec):
          avg       min       p50       p95       p99       max
        0.361     0.112     0.311     0.495     1.639     1.751

  baseline network latency (msec):
          avg       p50       p95       p99
        0.185     0.143     0.351     0.359
  processing overhead (msec) = latency - baseline:
          avg       p50       p95       p99
        0.176     0.168     0.144     1.280
```

**CSV Mode:**
```csv
"test","rps","avg_latency_ms",...,"baseline_avg_ms","baseline_p50_ms","baseline_p95_ms","baseline_p99_ms"
"GET","58823.53","0.156",...,"0.133","0.127","0.143","0.239"
```

### Usage

```bash
# Default - baseline enabled automatically
./bin/valkey-benchmark -h host -t get -n 5000

# Disable baseline if needed
./bin/valkey-benchmark -h host -t get -n 5000 --no-baseline

# Works with vector search too
./bin/valkey-benchmark --dataset vectors.bin -t vec-query
```

### Benefits

- ✅ **No configuration needed** - Works out of the box
- ✅ **Silent operation** - Doesn't clutter output
- ✅ **Accurate analysis** - Separates network from processing latency
- ✅ **CSV compatible** - Baseline columns included automatically
- ✅ **Remote testing** - Essential for benchmarking remote clusters

## Runtime Configuration Management

Apply server-side configurations before running benchmarks to test performance under different settings automatically.

### Quick Example

```bash
# Create a config file
cat > perf-config.conf <<EOF
io-threads 8
tcp-backlog 4096
maxclients 50000
save ""
appendonly no
EOF

# Run benchmark with runtime config
./valkey-benchmark -h localhost -t vec-query \
    --dataset openai-large-5m.bin \
    --runtime-config perf-config.conf \
    --restore-config
```

### Features

- **Automatic Application**: Configs applied to all cluster nodes before benchmarks
- **Original Value Preservation**: Saves original values automatically
- **Restoration**: Optionally restore original configs after benchmarks
- **Simple Format**: Key-value pairs like `io-threads 8` or `maxmemory 10gb`

### Common Use Cases

```bash
# Test with different IO thread counts
echo "io-threads 8" > config.conf
./valkey-benchmark -t ping --runtime-config config.conf --restore-config

# Optimize for vector search
cat > vector-opt.conf <<EOF
io-threads 8
io-threads-do-reads yes
maxmemory 20gb
save ""
appendonly no
EOF
./valkey-benchmark -t vec-query --runtime-config vector-opt.conf --restore-config

# Test memory pressure scenarios
echo "maxmemory 5gb" > memory-test.conf
./valkey-benchmark -t vec-query --runtime-config memory-test.conf --restore-config
```

See [RUNTIME_CONFIG.md](RUNTIME_CONFIG.md) for detailed documentation.

## Project Structure

```
valkey-search-benchmark/
├── loader/                # Core C source files (benchmark + dataset API)
├── prep_datasets/         # Dataset download/conversion scripts
├── bench/                 # Benchmarking scripts and utilities
├── test/                  # Testing workflows and validation
├── utils/
│   └── datasets/          # Python dataset conversion toolkit
├── docs/                  # Comprehensive documentation
└── examples/              # Configuration examples
```

## Development

### Building from Source

```bash
# Debug build
mkdir build-debug && cd build-debug
cmake -DCMAKE_BUILD_TYPE=Debug ..
make valkey-benchmark

# With custom allocator
cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_MALLOC=jemalloc ..
make valkey-benchmark
```

### Running Tests

```bash
# Quick ef_search demo (30 seconds)
./test/demo_ef_search_simple.sh

# Full parameter sweep (5-10 minutes)
./test/test_ef_search_working.sh

# Multi-dataset testing
./bench/test_multi_dataset.sh
```

## Performance Expectations

TBD - Performance benchmarks will be added soon.

## Contributing

This is a standalone extraction from the Valkey project. For contributions:

1. Focus on benchmarking capabilities only
2. Maintain compatibility with existing datasets
3. Keep documentation synchronized with code changes
4. Test with multiple datasets before committing

## License

BSD 3-Clause License (inherited from Valkey/Redis)

See [LICENSE](LICENSE) for full text.

## Credits

Extracted from [Valkey](https://github.com/valkey-io/valkey) project.

Original Redis benchmark tool by Redis Ltd.

Vector search benchmarking extensions and dataset pipeline by the Valkey community.

## Support

- **Documentation**: See `docs/` directory
- **Issues**: Report benchmarking-specific issues
- **Valkey Project**: https://valkey.io
