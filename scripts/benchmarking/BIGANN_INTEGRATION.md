# Big-ANN Benchmark Integration for Valkey Vector Testing

This document describes the complete integration of all Big-ANN benchmark datasets with the Valkey vector search testing framework.

## Overview

The solution provides comprehensive support for:
- **Standard ANN-Benchmarks** (ann-benchmarks.com) - 13 datasets
- **Big-ANN Benchmarks** (big-ann-benchmarks.com) - 10 datasets
- **NeurIPS Competition Datasets** - Future support planned
- **Custom Datasets** - Easy to add new datasets

All datasets are converted to the unified binary format (`0xDECDB001` magic) that valkey-benchmark natively supports.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     Dataset Sources                          │
├────────────┬────────────────┬─────────────┬─────────────────┤
│ ANN-Bench  │   Big-ANN      │  NeurIPS    │    Custom       │
│  (HDF5)    │ (BVECS/FVECS)  │   (TAR)     │   (Various)     │
└──────┬─────┴───────┬────────┴──────┬──────┴────────┬────────┘
       │             │                │               │
       ▼             ▼                ▼               ▼
┌──────────────────────────────────────────────────────────────┐
│           Unified Download & Conversion Pipeline             │
│                  download_bigann_datasets.sh                 │
├──────────────────────────────────────────────────────────────┤
│  1. Download from source (with caching)                      │
│  2. Convert to HDF5 intermediate format                      │
│  3. Use prepare_binary.py for final conversion               │
└────────────────────────┬─────────────────────────────────────┘
                         │
                         ▼
┌──────────────────────────────────────────────────────────────┐
│              Valkey Binary Format (.bin)                     │
│         Magic: 0xDECDB001 | Version: 1 | 4KB Header         │
└────────────────────────┬─────────────────────────────────────┘
                         │
                         ▼
┌──────────────────────────────────────────────────────────────┐
│                    valkey-benchmark                          │
│              Native dataset API (dataset_api.c)              │
└──────────────────────────────────────────────────────────────┘
```

## Quick Start

### 1. Download and Test Small Datasets

```bash
cd /home/ubuntu/valkey/vector-testing

# Download quick test set (3 small datasets)
./download_bigann_datasets.sh --quick

# Test with valkey-benchmark
./test_bigann_datasets.sh --quick
```

### 2. Download Big-ANN Datasets

```bash
# Download specific Big-ANN dataset (1M subset)
./download_bigann_datasets.sh bigann-1M

# Download all Big-ANN datasets (warning: large!)
./download_bigann_datasets.sh --bigann
```

### 3. Run Comprehensive Tests

```bash
# Test all available datasets
./test_bigann_datasets.sh --comprehensive

# Test specific dataset
./test_bigann_datasets.sh --dataset bigann-10M
```

## Supported Datasets

### Standard ANN-Benchmarks (Small to Medium Scale)

**All datasets include embeddings, queries, and precomputed ground truth for accurate recall evaluation.**

| Dataset | Dimensions | Embeddings | Queries | Metric | Size | Description |
|---------|------------|------------|---------|--------|------|-------------|
| glove-25 | 25 | 1.18M | 10K | Cosine | 120MB | Word embeddings + ground truth |
| glove-50 | 50 | 1.18M | 10K | Cosine | 240MB | Word embeddings + ground truth |
| glove-100 | 100 | 1.18M | 10K | Cosine | 480MB | Word embeddings + ground truth |
| glove-200 | 200 | 1.18M | 10K | Cosine | 960MB | Word embeddings + ground truth |
| sift-128 | 128 | 1M | 10K | L2 | 500MB | SIFT descriptors + ground truth |
| gist-960 | 960 | 1M | 1K | L2 | 3.6GB | GIST descriptors + ground truth |
| deep-96 | 96 | 10M | 10K | Cosine | 3.6GB | Deep embeddings + ground truth |
| fashion-mnist | 784 | 60K | 10K | L2 | 200MB | Fashion images + ground truth |
| mnist | 784 | 60K | 10K | L2 | 200MB | Handwritten digits + ground truth |
| lastfm-64 | 64 | 292K | 50K | IP | 75MB | Music embeddings + ground truth |
| nytimes-256 | 256 | 290K | 10K | Cosine | 300MB | Article embeddings + ground truth |
| nytimes-16 | 16 | 290K | 10K | Cosine | 20MB | Article embeddings + ground truth |

### Big-ANN Benchmarks (Large Scale)

**All datasets include embeddings, queries, and precomputed ground truth for accurate recall evaluation.**

| Dataset | Dimensions | Embeddings | Queries | Metric | Size | Description |
|---------|------------|------------|---------|--------|------|-------------|
| bigann-1M | 128 | 1M | 10K | L2 | 500MB | SIFT subset + ground truth |
| bigann-10M | 128 | 10M | 10K | L2 | 5GB | SIFT subset + ground truth |
| bigann-100M | 128 | 100M | 10K | L2 | 50GB | SIFT subset + ground truth |
| deep-1M | 96 | 1M | 10K | Cosine | 400MB | Deep images + ground truth |
| deep-10M | 96 | 10M | 10K | Cosine | 4GB | Deep images + ground truth |
| text2image-1M | 200 | 1M | 100K | IP | 800MB | Cross-modal + ground truth |
| text2image-10M | 200 | 10M | 100K | IP | 8GB | Cross-modal + ground truth |
| msturing-1M | 100 | 1M | 100K | L2 | 400MB | Web search + ground truth |
| msturing-10M | 100 | 10M | 100K | L2 | 4GB | Web search + ground truth |
| msspacev-1M | 100 | 1M | 100K | L2 | 400MB | SpaceV + ground truth |

## Dataset Components

### What's Included in Each Dataset

Every dataset provides three essential components for comprehensive vector search evaluation:

1. **Base Embeddings**: The main vector database for indexing and searching
   - Pre-computed vector representations (float32 format)
   - Range from 60K (fashion-mnist) to 100M+ (bigann-100M) vectors

2. **Query Vectors**: Test queries for performance evaluation
   - Separate set of vectors not in the base embeddings
   - Used to test search accuracy and performance
   - Range from 1K to 100K queries depending on dataset

3. **Ground Truth**: Precomputed exact k-nearest neighbors
   - For each query, the true k-nearest neighbors in the base set
   - Used to calculate recall metrics (what percentage of true neighbors were found)
   - Computed using exact brute-force search for 100% accuracy
   - Typically k=100 neighbors per query

### Ground Truth Quality

- **ANN-Benchmarks**: Precomputed exact k-NN using brute-force search
- **Big-ANN**: Ground truth computed from full billion-scale datasets
- **All formats**: Verified for correctness and consistency

This ensures that recall measurements are accurate and comparable across different vector search algorithms and configurations.

## Binary Format Specification

All datasets are converted to the unified binary format:

```c
typedef struct __attribute__((packed)) {
    uint32_t magic;           // 0xDECDB001
    uint32_t version;         // 1
    char dataset_name[256];   // Dataset name
    uint8_t distance_metric;  // 0=L2, 1=COSINE, 2=IP
    uint8_t dtype;           // 0=FLOAT32
    uint32_t dim;            // Vector dimensions
    uint64_t num_vectors;    // Number of vectors
    uint64_t num_queries;    // Number of queries
    uint32_t num_neighbors;  // K neighbors (typically 100)
    uint64_t vectors_offset; // Offset to vectors data
    uint64_t queries_offset; // Offset to queries data
    uint64_t ground_truth_offset; // Offset to ground truth
    uint8_t reserved[3808];  // Pad to 4KB
} dataset_header_t;
```

## Integration with valkey-benchmark

The datasets integrate seamlessly with valkey-benchmark's native dataset API:

```bash
# Basic usage
valkey-benchmark \
    --dataset bigann-1M \
    --search-index-name "bigann-1M-1M-128-100" \
    --search-ef-runtime 100 \
    --search -t FT.SEARCH \
    -n 1000

# Advanced testing with multiple ef_search values
for ef in 50 100 200 400; do
    valkey-benchmark \
        --dataset sift-128 \
        --search-ef-runtime $ef \
        --search -t FT.SEARCH \
        -n 10000 \
        -c 50
done
```

## Performance Testing

### Test Parameters

```bash
# Environment variables
export HOST="your-valkey-host"
export NUM_QUERIES=10000
export EF_SEARCH_VALUES="50,100,200,400,800"
export CONCURRENCY=50
export THREADS=20

# Run comprehensive test
./test_bigann_datasets.sh --comprehensive
```

### Expected Performance

| Dataset Size | ef_search | Expected Recall | QPS | Latency |
|--------------|-----------|-----------------|-----|---------|
| 1M vectors | 100 | ~75% | 2000+ | <2ms |
| 10M vectors | 200 | ~80% | 1000+ | <5ms |
| 100M vectors | 400 | ~85% | 500+ | <10ms |

## Adding Custom Datasets

### Method 1: Add to download_bigann_datasets.sh

```bash
# Add to DATASETS array
DATASETS["my-dataset"]="http://url/to/dataset.hdf5,dims,vectors,metric,description"
```

### Method 2: Manual conversion

```python
# Convert custom format to HDF5
import h5py
import numpy as np

with h5py.File('my-dataset.hdf5', 'w') as f:
    f.create_dataset('train', data=vectors, dtype='float32')
    f.create_dataset('test', data=queries, dtype='float32')
    f.create_dataset('neighbors', data=ground_truth, dtype='int64')
```

Then convert to binary:
```bash
python3 utils/datasets/prepare_binary.py \
    my-dataset.hdf5 \
    build-debug/my-dataset.bin \
    my-dataset \
    --metric COSINE
```

## Troubleshooting

### Dataset Download Issues

```bash
# Check disk space
df -h /home/ubuntu/valkey/datasets

# Clear cache
rm -rf /home/ubuntu/valkey/datasets/.cache

# Retry with verbose output
bash -x ./download_bigann_datasets.sh bigann-1M
```

### Binary Format Issues

```bash
# Verify binary format
xxd -l 64 build-debug/dataset.bin

# Check magic number (should be: 01b0 cdde)
# Check dataset_api.c can read it
./build-debug/bin/valkey-benchmark --dataset-info dataset-name
```

### Memory Issues

For large datasets (>10M vectors):
- Ensure sufficient RAM (6x dataset size recommended)
- Use smaller query batches
- Reduce concurrency

## Advanced Features

### Batch Processing

```bash
# Process multiple datasets in parallel
parallel -j 3 ./download_bigann_datasets.sh {} ::: \
    bigann-1M bigann-10M deep-1M

# Test multiple datasets sequentially
for dataset in bigann-1M deep-1M text2image-1M; do
    ./test_bigann_datasets.sh --dataset $dataset
done
```

### Result Analysis

```bash
# Combine results from multiple tests
cat ef_search_results_*.csv | \
    awk -F',' 'NR==1 || !/^dataset/' | \
    sort -t',' -k1,1 -k2,2n > combined_results.csv

# Generate performance report
python3 _util_analyze_results.py --input combined_results.csv
```

## Implementation Details

### Key Components

1. **download_bigann_datasets.sh**: Main download and conversion script
   - Supports multiple dataset sources
   - Automatic format detection
   - Caching for efficiency

2. **bigann_to_hdf5.py**: Converter for Big-ANN binary formats
   - Handles BVECS/FVECS/IVECS formats
   - Subset extraction
   - Ground truth truncation

3. **test_bigann_datasets.sh**: Testing orchestration
   - Multiple ef_search values
   - Result collection
   - Performance analysis

4. **prepare_binary.py**: HDF5 to Valkey binary converter
   - Existing tool reused
   - 64-byte alignment
   - Header generation

### Data Flow

1. **Download**: Fetch from source URL
2. **Cache**: Store in `.cache` directory
3. **Convert**: Transform to HDF5 intermediate
4. **Prepare**: Convert HDF5 to Valkey binary
5. **Test**: Use with valkey-benchmark

## Future Enhancements

- [ ] Support for filtered search datasets
- [ ] Streaming dataset support
- [ ] Sparse vector datasets
- [ ] Custom distance metrics
- [ ] Distributed testing framework
- [ ] Real-time performance dashboard

## References

- [ANN-Benchmarks](http://ann-benchmarks.com/)
- [Big-ANN Benchmarks](https://big-ann-benchmarks.com/)
- [NeurIPS 2023 Competition](https://big-ann-benchmarks.com/neurips23.html)
- [Valkey Vector Search](https://github.com/valkey-io/valkey)

## License

This integration follows the same license as the Valkey project.