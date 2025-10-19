# Valkey Vector Benchmark

A standalone benchmarking toolkit for evaluating vector search performance in Valkey and Redis clusters, with support for standard vectordb-bench datasets and comprehensive recall validation.

## Overview

This package extracts the vector search benchmarking capabilities from the Valkey project into a standalone tool. It provides:

- **Dataset Pipeline**: Download, convert, and prepare vectordb-bench datasets (COHERE, OPENAI, SIFT, GIST, LAION, BIGANN)
- **Binary Format**: Custom 4KB-aligned binary format with precomputed ground truth for recall validation
- **Cluster Support**: Parallel cluster scanning with vector ID to cluster tag mapping
- **Recall Validation**: Accurate recall measurement with missing neighbor reconstruction
- **Performance Testing**: Throughput (QPS), latency (p50/p99), and recall analysis
- **Parameter Tuning**: ef_search parameter sweep and optimization

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

```bash
# Download COHERE 1M dataset
python scripts/conversion/download_any_dataset.py COHERE 1000000

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

## Documentation

- **[Complete Guide](docs/VECTORDB_BENCH_COMPLETE_GUIDE.md)** - Full dataset pipeline walkthrough
- **[Testing Guide](docs/DATASET_TESTING_GUIDE.md)** - Testing workflows and examples
- **[ef_search Tuning](docs/EF_SEARCH_TESTING_GUIDE.md)** - Parameter optimization
- **[Dataset Catalog](docs/VECTORDB_DATASETS.md)** - Available datasets and sizes
- **[Architecture](docs/BENCHMARK_MODULES.md)** - Module design and organization

## Features

### Dataset Support

| Dataset | Dimensions | Sizes Available | Distance Metric |
|---------|-----------|-----------------|-----------------|
| COHERE  | 768       | 100K, 1M, 10M   | COSINE          |
| OPENAI  | 1536      | 500K, 5M        | COSINE          |
| SIFT    | 128       | 500K, 5M        | L2              |
| GIST    | 960       | 100K, 1M        | L2              |
| LAION   | 768       | 100M            | COSINE          |
| BIGANN  | 128       | 10M, 100M       | L2              |

### Performance Metrics

- **Throughput**: Queries per second (QPS)
- **Latency**: p50, p95, p99, p99.9 percentiles
- **Recall**: Percentage of ground truth neighbors found
- **Cluster Scanning**: 1M+ keys/second with parallel workers

### Key Capabilities

✅ Two-phase benchmarking (ground truth insertion + query testing)  
✅ Cluster mode support with read-from-replica options  
✅ Parallel cluster scanning for recall validation  
✅ Vector ID to cluster tag mapping for missing neighbor reconstruction  
✅ HDR histogram for accurate latency percentiles  
✅ Progress bars and real-time monitoring  
✅ Comprehensive result analysis and CSV export  

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
