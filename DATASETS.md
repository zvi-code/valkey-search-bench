# Dataset Management Guide

Complete guide for downloading, converting, and managing vector datasets for benchmarking.

## Quick Start

### Using the Unified Dataset Manager (Recommended)

The simplest way to get datasets is using the shell wrapper:

```bash
# List all available datasets
./scripts/dataset.sh list

# Filter by name
./scripts/dataset.sh list cohere

# Download and convert in one command
./scripts/dataset.sh get mnist                    # Small (60K vectors)
./scripts/dataset.sh get sift-128                 # Medium (1M vectors)
./scripts/dataset.sh get cohere-medium-1m         # Large (1M vectors, 768-dim)
./scripts/dataset.sh get yfcc-10m                 # Extra large (10M vectors with metadata)

# Force re-download
./scripts/dataset.sh get cohere-medium-1m --force

# Verify integrity
./scripts/dataset.sh verify datasets/*.bin
```

The tool automatically:
1. Downloads source files
2. Converts to intermediate HDF5 format (cached)
3. Generates Valkey binary format
4. Verifies data integrity

## Available Datasets

### Small Datasets (< 100K vectors)

Perfect for quick testing and CI/CD:

| Dataset | Vectors | Dimensions | Metric | Size | Command |
|---------|---------|-----------|--------|------|---------|
| mnist | 60K | 784 | L2 | 180MB | `./scripts/dataset.sh get mnist` |
| fashion-mnist | 60K | 784 | L2 | 180MB | `./scripts/dataset.sh get fashion-mnist` |
| cohere-small-100k | 100K | 768 | COSINE | 290MB | `./scripts/dataset.sh get cohere-small-100k` |

### Medium Datasets (1M vectors)

Good balance of scale and manageability:

| Dataset | Vectors | Dimensions | Metric | Size | Command |
|---------|---------|-----------|--------|------|---------|
| sift-128 | 1M | 128 | L2 | 500MB | `./scripts/dataset.sh get sift-128` |
| gist-960 | 1M | 960 | L2 | 3.6GB | `./scripts/dataset.sh get gist-960` |
| glove-25 | 1.18M | 25 | COSINE | 120MB | `./scripts/dataset.sh get glove-25` |
| glove-50 | 1.18M | 50 | COSINE | 240MB | `./scripts/dataset.sh get glove-50` |
| glove-100 | 1.18M | 100 | COSINE | 480MB | `./scripts/dataset.sh get glove-100` |
| cohere-medium-1m | 1M | 768 | COSINE | 2.9GB | `./scripts/dataset.sh get cohere-medium-1m` |

### Large Datasets (5-10M vectors)

Production-scale testing:

| Dataset | Vectors | Dimensions | Metric | Size | Command |
|---------|---------|-----------|--------|------|---------|
| deep-96 | 10M | 96 | COSINE | 3.6GB | `./scripts/dataset.sh get deep-96` |
| bigann-10m | 10M | 128 | L2 | 5GB | `./scripts/dataset.sh get bigann-10m` |
| **yfcc-10m** | 10M | 192 | L2 | 8.1GB | `./scripts/dataset.sh get yfcc-10m` |
| cohere-large-10m | 10M | 768 | COSINE | 29GB | `./scripts/dataset.sh get cohere-large-10m` |
| openai-medium-500k | 500K | 1536 | COSINE | 2.9GB | `./scripts/dataset.sh get openai-medium-500k` |
| openai-large-5m | 5M | 1536 | COSINE | 29GB | `./scripts/dataset.sh get openai-large-5m` |

**Bold** = Includes metadata for filtered search

## Dataset Sources

Datasets come from multiple sources:

### VectorDBBench (COHERE, OPENAI, SIFT, GIST)

Modern embedding datasets from vectordb-bench library:

```bash
# Download using Python script
source venv/bin/activate
python prep_datasets/download_dataset.py COHERE 1000000
python prep_datasets/download_dataset.py OPENAI 5000000

# Or use unified manager
./prep_datasets/dataset.sh get cohere-medium-1m
./prep_datasets/dataset.sh get openai-large-5m
```

**Available sizes:**
- COHERE: 100K, 1M, 10M (768-dim, COSINE)
- OPENAI: 500K, 5M (1536-dim, COSINE)
- SIFT: 500K, 5M (128-dim, L2)
- GIST: 100K, 1M (960-dim, L2)

### ANN-Benchmarks (MNIST, Fashion-MNIST, GloVe)

Standard ML/NLP datasets:

```bash
./scripts/dataset.sh get mnist
./scripts/dataset.sh get glove-100
```

### BigANN (SIFT1B, Deep1B Subsets)

Billion-scale competition datasets:

```bash
./scripts/dataset.sh get bigann-10m
./scripts/dataset.sh get deep-10m
```

**Important**: BigANN subsets require **subset-specific** ground truth files, not the full 1B ground truth!

#### BigANN Ground Truth URLs

- **bigann-10M**: `https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/GT_10M/bigann-10M`
- **bigann-100M**: `https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/GT_100M/bigann-100M`
- **deep-10M**: `https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/GT_10M/deep-10M`

### YFCC-10M (Metadata Filtering)

Special dataset with metadata tags for filtered search:

```bash
./scripts/dataset.sh get yfcc-10m
```

**Metadata features:**
- 10M vectors (192-dim CLIP embeddings)
- 200,386 unique tags (vocabulary)
- 108M tag assignments (~11 tags per vector)
- 100K queries with predicates
- Tags include: years, months, cameras, countries

**Example tags:**
- `year_2015`, `month_April`
- `camera_Canon`, `camera_Nikon`
- `us_state_New_York`, `country_France`

## Manual Conversion Workflow

For custom datasets or understanding the pipeline:

### Step 1: Download Raw Data

```bash
# VectorDBBench datasets
source venv/bin/activate
python prep_datasets/download_dataset.py COHERE 1000000
```

Output: `/mnt/data/datasets/cohere/cohere_medium_1m/` (parquet files)

### Step 2: Convert Parquet → HDF5

```bash
python prep_datasets/convert_parquet_to_hdf5.py \
  /mnt/data/datasets/cohere/cohere_medium_1m \
  /mnt/data/datasets/cohere-medium-1m.hdf5 \
  --name cohere-medium-1m
```

Output: `/mnt/data/datasets/cohere-medium-1m.hdf5`

**HDF5 structure:**
```
/train        - Training vectors [N × dim] float32
/test         - Query vectors [Q × dim] float32
/neighbors    - Ground truth neighbors [Q × k] int32
/distances    - Ground truth distances [Q × k] float32
```

### Step 3: Convert HDF5 → Valkey Binary

```bash
python utils/datasets/prepare_binary.py \
  /mnt/data/datasets/cohere-medium-1m.hdf5 \
  /mnt/data/build-datasets/cohere-medium-1m.bin \
  --metric COSINE \
  --max-neighbors 100
```

Output: `/mnt/data/build-datasets/cohere-medium-1m.bin`

### Automated Conversion

For automatic conversion, use the unified dataset manager:

```bash
./scripts/dataset.sh get cohere-medium-1m
```

This automatically handles all three steps (download, convert to HDF5, convert to binary).

## File Locations

Standard directory structure:

```
/mnt/data/datasets/                     # Raw downloads + HDF5 cache
├── cohere/
│   ├── cohere_small_100k/             # Parquet files
│   ├── cohere_medium_1m/
│   └── cohere_large_10m/
├── cohere-small-100k.hdf5             # Intermediate HDF5
└── cohere-medium-1m.hdf5

/mnt/data/build-datasets/              # Final binary format
├── cohere-small-100k.bin              # Valkey binary
├── cohere-medium-1m.bin
└── cohere-large-10m.bin

build/datasets/                         # Symlinks for convenience
├── cohere-small-100k.bin -> /mnt/data/build-datasets/cohere-small-100k.bin
└── ...
```

## Binary Format Specification

Valkey uses a custom memory-mapped binary format:

```c
#define DATASET_MAGIC 0xDECDB001
#define DATASET_VERSION 1

typedef struct __attribute__((packed)) {
    uint32_t magic;                      // 0xDECDB001
    uint32_t version;                    // Format version (1 or 2)
    uint64_t num_vectors;                // Number of training vectors
    uint64_t num_queries;                // Number of test queries
    uint32_t dim;                        // Vector dimensions
    uint32_t num_neighbors;              // k for k-NN
    uint8_t metric;                      // 0=L2, 1=IP, 2=COSINE
    uint8_t reserved[3803];              // Padding to 4KB
} dataset_header_t;
```

**Memory layout:**

```
Offset 0:        [Header: 4KB aligned]
Offset 4096:     [Training vectors: num_vectors × dim × float32, 64-byte aligned]
Offset X:        [Query vectors: num_queries × dim × float32, 64-byte aligned]
Offset Y:        [Ground truth neighbors: num_queries × num_neighbors × int64]
```

**Version 2 additions** (for metadata):

```c
// Additional header fields in v2
uint8_t has_metadata;                    // 0 or 1
uint32_t vocab_size;                     // Number of unique tags
uint64_t vector_metadata_offset;         // Offset to CSR matrix
uint64_t query_metadata_offset;          // Offset to predicates
```

Followed by CSR sparse matrices for tags.

## Storage Management

### Check Space

```bash
# Check available space
df -h /mnt/data

# Check dataset sizes
du -sh /mnt/data/datasets/*
du -sh /mnt/data/build-datasets/*
```

### Clean Cache

```bash
# Remove HDF5 cache (can regenerate)
rm /mnt/data/datasets/*.hdf5

# Remove specific dataset
rm /mnt/data/build-datasets/cohere-large-10m.bin
rm -rf /mnt/data/datasets/cohere/cohere_large_10m
```

### Manage Datasets

```bash
# List all binary datasets
ls -lh /mnt/data/build-datasets/

# Verify dataset integrity
./scripts/dataset.sh verify /mnt/data/build-datasets/*.bin

# Clean all datasets
./scripts/dataset.sh clean
```

## Performance Considerations

### Memory Mapping

Binary files use mmap for zero-copy access:
- Training vectors: Sequential prefill with `MADV_WILLNEED`
- Query vectors: Efficient random access pattern
- Ground truth: Pre-computed for fast recall validation

### Cache Efficiency

- **64-byte alignment**: Optimal for CPU cache lines
- **4KB pages**: Matches memory page size
- **Sequential layout**: Minimizes page faults

### Storage Recommendations

| Dataset Size | Storage Type | Notes |
|-------------|-------------|-------|
| < 1GB | Local SSD | Fast enough |
| 1-10GB | NVMe | Recommended |
| > 10GB | NVMe | Required for good performance |

## Troubleshooting

### Download Issues

**Error: `ModuleNotFoundError: No module named 'vectordb_bench'`**

```bash
source venv/bin/activate
pip install vectordb-bench==1.0.10
```

**Error: Connection timeout**

```bash
# Retry with longer timeout
export DATASET_TIMEOUT=300
./scripts/dataset.sh get cohere-medium-1m
```

### Conversion Issues

**Error: `OutOfMemoryError` during conversion**

```bash
# Use memory-efficient converter
python scripts/conversion/convert_parquet_to_hdf5.py \
  --chunk-size 10000  # Smaller chunks
```

**Error: Invalid binary format**

```bash
# Verify magic number
hexdump -C dataset.bin | head -20
# Should show: 01 b0 cd de (0xDECDB001)
```

### Space Issues

**Error: No space left on device**

```bash
# Use NVMe storage (see INSTALLATION.md)
# Or clean up old files
./scripts/dataset.sh clean
```

## Custom Dataset Integration

To add your own dataset:

1. **Prepare HDF5** with required structure:
   ```python
   import h5py
   with h5py.File('custom.hdf5', 'w') as f:
       f.create_dataset('train', data=train_vectors)  # [N, dim]
       f.create_dataset('test', data=test_vectors)    # [Q, dim]
       f.create_dataset('neighbors', data=gt_neighbors)  # [Q, k]
       f.create_dataset('distances', data=gt_distances)  # [Q, k]
   ```

2. **Convert to binary**:
   ```bash
   python utils/datasets/prepare_binary.py \
     custom.hdf5 custom.bin \
     --metric L2 --max-neighbors 100
   ```

3. **Verify**:
   ```bash
   ./scripts/dataset.sh verify custom.bin
   ```

## Next Steps

- **Running Benchmarks**: See [BENCHMARKING.md](BENCHMARKING.md)
- **Advanced Features**: See [ADVANCED.md](ADVANCED.md) for metadata filtering
- **Installation**: See [INSTALLATION.md](INSTALLATION.md) for environment setup
