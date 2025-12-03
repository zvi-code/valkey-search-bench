# Valkey Vector Benchmark

A standalone benchmarking toolkit for evaluating vector search performance in Valkey and Redis clusters, with support for standard vectordb-bench datasets and comprehensive recall validation.

## Overview

This toolkit provides everything needed to benchmark vector search operations:

- **Dataset Pipeline**: Download, convert, and prepare vectordb-bench datasets (COHERE, OPENAI, SIFT, GIST, BIGANN, YFCC)
- **Binary Format**: Custom 4KB-aligned binary format with precomputed ground truth for recall validation
- **Cluster Support**: Full cluster mode with parallel scanning and vector ID to cluster tag mapping
- **Recall Validation**: Accurate recall measurement with missing neighbor reconstruction
- **Performance Metrics**: Throughput (QPS), latency percentiles (p50/p95/p99), and baseline network latency
- **Adaptive Optimization**: Automatic parameter tuning (ef_search, clients, threads) with binary/grid search
- **Metadata Filtering**: Tag-based filtered vector search benchmarking (YFCC-10M dataset)
- **Runtime Configuration**: Apply server-side configs before benchmarks for automated testing

## Getting Started

👉 **See [INSTALLATION.md](INSTALLATION.md) for complete setup instructions.**

Quick overview:
1. Install dependencies (cmake, gcc, python3)
2. Clone with `--recursive` flag
3. Build with cmake
4. Download a dataset
5. Run benchmarks

## Documentation

| Guide | Description |
|-------|-------------|
| **[INSTALLATION.md](INSTALLATION.md)** | Setup, build, and first benchmark |
| **[DATASETS.md](DATASETS.md)** | Download and manage vector datasets |
| **[BENCHMARKING.md](BENCHMARKING.md)** | Run benchmarks and interpret results |
| **[ADVANCED.md](ADVANCED.md)** | Optimizer internals, metadata filtering, binary format |
| **[RUNTIME_CONFIG.md](RUNTIME_CONFIG.md)** | Server-side configuration management |

## Available Datasets

The unified dataset manager supports 16+ preconfigured datasets:

| Category | Datasets | Vectors | Dimensions |
|----------|----------|---------|------------|
| **Small** (testing) | mnist, fashion-mnist | 60K | 784 |
| **Medium** (1M) | sift-128, gist-960, glove-*, cohere-medium-1m | 1M | 25-960 |
| **Large** (5-10M) | cohere-large-10m, openai-large-5m, bigann-10m | 5-10M | 128-1536 |
| **With Metadata** | yfcc-10m (200K tags for filtered search) | 10M | 192 |

See [DATASETS.md](DATASETS.md) for the complete list and download instructions.

## Key Features

### Two-Phase Benchmarking
1. **Load phase**: Insert vectors with ground truth
2. **Query phase**: Run KNN searches with recall validation

### Adaptive Load Optimizer
Automatically finds optimal parameters:
- Binary search for `ef_search` (recall optimization)
- Grid search for `clients` and `threads` (throughput optimization)
- Supports constraints like "maximize QPS where recall > 95%"

### Baseline Network Latency
Automatically measures network RTT to separate network overhead from processing time.

### Metadata Filtering (YFCC-10M)
Benchmark filtered vector search with tag predicates (BigANN NeurIPS 2023 track).

### Configuration Persistence
Saves benchmark parameters between runs - no need to retype long command lines.

## Project Structure

```
valkey-search-bench/
├── loader/           # Core C source (benchmark tool + dataset API)
├── prep_datasets/    # Dataset download/conversion scripts (Python)
├── bench/            # Benchmarking utilities and wrappers
├── test/             # Test scripts and validation
├── deps/valkey/      # Valkey submodule (core utilities + jemalloc)
└── cmake/            # Build configuration
```

## Performance Metrics

The benchmark measures:
- **Throughput**: Queries per second (QPS)
- **Latency**: avg, p50, p95, p99, p99.9, max
- **Recall@K**: Percentage of ground truth neighbors found
- **Baseline RTT**: Network latency overhead

## Requirements

- **OS**: Ubuntu 20.04+ or compatible Linux
- **CPU**: x86_64 or ARM64 (optimized for AWS Graviton)
- **RAM**: 8GB minimum, 32GB+ recommended for large datasets
- **Storage**: Dedicated volume recommended for datasets (10GB-40GB+)

See [INSTALLATION.md - System Requirements](INSTALLATION.md#appendix-a-system-requirements) for details.

## License

BSD 3-Clause License (inherited from Valkey/Redis)

See [LICENSE](LICENSE) for full text.

## Credits

- Extracted from [Valkey](https://github.com/valkey-io/valkey) project
- Original Redis benchmark tool by Redis Ltd.
- Vector search extensions by the Valkey community
