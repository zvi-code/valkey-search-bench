# Valkey Vector Benchmark

A standalone benchmarking toolkit for evaluating vector search performance in Valkey and Redis clusters, with support for standard vectordb-bench datasets and comprehensive recall validation.

## Overview

This package extracts the vector search benchmarking capabilities from the Valkey project into a standalone tool. It provides:

- **Dataset Pipeline**: Download, convert, and prepare vectordb-bench datasets (COHERE, OPENAI, SIFT, GIST, LAION, BIGANN)
- **Binary Format**: Custom 4KB-aligned binary format with precomputed ground truth for recall validation
- **Cluster Support**: Parallel cluster scanning with vector ID to cluster tag mapping
- **Recall Validation**: Accurate recall measurement with missing neighbor reconstruction
- **Performance Testing**: Throughput (QPS), latency (p50/p99), and recall analysis
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

### Build

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make valkey-benchmark
```

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
python scripts/conversion/download_dataset.py COHERE 1000000

# Convert to binary format
./scripts/conversion/convert_vectordb_dataset.sh \
    cohere/cohere_medium_1m \
    cohere-medium-1m \
    COSINE
```

### Run Benchmark

```bash
# Phase 1: Insert ground truth (all vectors)
./bin/valkey-benchmark -h localhost --cluster --rfr no \
  --dataset cohere-medium-1m.bin \
  -t vec-ground-truth --search --vector-dim 768 \
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

📚 **Complete documentation organized in 4 guides:**

1. **[Installation Guide](docs/INSTALLATION.md)** - Setup, dependencies, and building
2. **[Dataset Guide](docs/DATASETS.md)** - Downloading, converting, and managing datasets
3. **[Benchmarking Guide](docs/BENCHMARKING.md)** - Running benchmarks and interpreting results
4. **[Advanced Guide](docs/ADVANCED.md)** - Optimizer internals, metadata filtering, data formats

**Quick navigation:**
- New to the project? Start with [Installation](docs/INSTALLATION.md)
- Need datasets? See [Dataset Guide](docs/DATASETS.md)
- Running benchmarks? Check [Benchmarking Guide](docs/BENCHMARKING.md)
- Advanced features? Read [Advanced Guide](docs/ADVANCED.md)

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
✅ **Metadata filtering support** - Tag-based filtered vector search (YFCC-10M)  
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

## Project Structure

```
valkey-search-benchmark/
├── src/                    # Core C source files
├── utils/
│   └── datasets/          # Python dataset conversion toolkit
├── scripts/
│   ├── conversion/        # Dataset download/conversion scripts
│   ├── testing/           # Testing workflows
│   └── benchmarking/      # Multi-dataset benchmarking
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
./scripts/testing/demo_ef_search_simple.sh

# Full parameter sweep (5-10 minutes)
./scripts/testing/test_ef_search_working.sh

# Multi-dataset testing
./scripts/benchmarking/test_multi_dataset.sh
```

## Performance Expectations

| Scale | Vectors | Recall | QPS  | Latency | Dataset       |
|-------|---------|--------|------|---------|---------------|
| Small | 100K    | 95%+   | 2000+| <5ms    | COHERE-100K   |
| Medium| 1M      | 90%+   | 1500+| <8ms    | COHERE-1M     |
| Large | 5-10M   | 85%+   | 700+ | <15ms   | OPENAI-5M     |

## Contributing

This is a standalone extraction from the Valkey project. For contributions:

1. Focus on benchmarking capabilities only
2. Maintain compatibility with vectordb-bench datasets
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
