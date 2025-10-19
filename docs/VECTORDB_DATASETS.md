# Vector DB Benchmark Dataset Integration

This directory contains tools to download and convert datasets from the vectordb-bench library for use with Valkey vector search benchmarks.

## Overview

The workflow consists of three steps:
1. **Download** datasets from vectordb-bench (parquet format)
2. **Convert** parquet → HDF5 → binary format
3. **Use** with valkey-benchmark

## Quick Start

### 1. Download Datasets

Use the `download_any_dataset.py` script to download datasets directly to NVMe:

```bash
cd /mnt/data
./vectordb-bench-env/bin/python download_any_dataset.py COHERE 1000000
./vectordb-bench-env/bin/python download_any_dataset.py OPENAI 5000000
```

Available datasets and sizes:
- **LAION**: 100M vectors (768 dim, L2)
- **GIST**: 100K, 1M vectors (960 dim, L2)
- **COHERE**: 100K, 1M, 10M vectors (768 dim, Cosine)
- **BIOASQ**: 1M, 10M vectors (1024 dim, Cosine)
- **GLOVE**: 1M vectors (200 dim, Cosine)
- **SIFT**: 500K, 5M vectors (128 dim, L2)
- **OPENAI**: 500K, 5M vectors (1536 dim, Cosine)

### 2. Convert to Valkey Format

Use the automated conversion script:

```bash
./convert_vectordb_dataset.sh cohere/cohere_small_100k cohere-small-100k COSINE
./convert_vectordb_dataset.sh cohere/cohere_medium_1m cohere-medium-1m COSINE
./convert_vectordb_dataset.sh openai/openai_large_5m openai-large-5m COSINE
```

The script automatically:
- Converts parquet files to HDF5
- Converts HDF5 to Valkey binary format
- Creates symlinks in `build-debug/`

### 3. Run Benchmarks

```bash
./build-debug/valkey-benchmark --dataset cohere-small-100k
./build-debug/valkey-benchmark --dataset cohere-medium-1m
```

## File Locations

```
/mnt/data/datasets/                      # Downloaded datasets (parquet + HDF5)
├── cohere/
│   ├── cohere_small_100k/              # Parquet files from vectordb-bench
│   ├── cohere_medium_1m/
│   └── cohere_large_10m/
├── openai/
│   └── openai_large_5m/
├── cohere-small-100k.hdf5              # Intermediate HDF5 format
└── cohere-medium-1m.hdf5

/mnt/data/build-datasets/               # Final binary format
├── cohere-small-100k.bin               # Valkey binary format
├── cohere-medium-1m.bin
└── cohere-large-10m.bin

/home/ubuntu/valkey/build-debug/        # Symlinks for convenience
├── cohere-small-100k.bin -> /mnt/data/build-datasets/cohere-small-100k.bin
├── cohere-medium-1m.bin -> /mnt/data/build-datasets/cohere-medium-1m.bin
└── ...
```

## Binary Format

Valkey uses a custom binary format defined in `src/dataset_api.h`:

```c
#define DATASET_MAGIC 0xDECDB001

typedef struct {
    uint32_t magic;                      // 0xDECDB001
    uint32_t version;                    // Format version
    char name[256];                      // Dataset name
    uint8_t distance_metric;             // 0=L2, 1=COSINE, 2=IP
    uint8_t dtype;                       // 0=FLOAT32, 1=FLOAT16
    uint32_t dim;                        // Vector dimensions
    uint64_t num_vectors;                // Training vectors count
    uint64_t num_queries;                // Test queries count
    uint32_t num_neighbors;              // k for k-NN (typically 100)
    uint64_t vectors_offset;             // Offset to training vectors
    uint64_t queries_offset;             // Offset to test queries
    uint64_t ground_truth_offset;        // Offset to ground truth neighbors
} dataset_header_t;
```

The file layout is:
1. **Header** (4KB aligned)
2. **Training vectors** (num_vectors × dim × 4 bytes)
3. **Test queries** (num_queries × dim × 4 bytes)
4. **Ground truth** (num_queries × num_neighbors × 8 bytes)

## Tools

### download_any_dataset.py
Downloads datasets from vectordb-bench to `/mnt/data/datasets/`.

```bash
/mnt/data/vectordb-bench-env/bin/python download_any_dataset.py <DATASET> <SIZE>
```

### convert_parquet_to_hdf5.py
Converts vectordb-bench parquet files to HDF5 format.

```bash
/mnt/data/vectordb-bench-env/bin/python convert_parquet_to_hdf5.py \
    /mnt/data/datasets/cohere/cohere_small_100k \
    /mnt/data/datasets/cohere-small-100k.hdf5 \
    --name cohere-small-100k
```

### utils/datasets/prepare_binary.py
Converts HDF5 to Valkey binary format.

```bash
python3 utils/datasets/prepare_binary.py \
    /mnt/data/datasets/cohere-small-100k.hdf5 \
    /mnt/data/build-datasets/cohere-small-100k.bin \
    --name cohere-small-100k \
    --metric COSINE \
    --max-neighbors 100
```

### convert_vectordb_dataset.sh
All-in-one conversion script (recommended).

```bash
./convert_vectordb_dataset.sh <parquet_dir> <output_name> <metric>
```

## Dataset Comparison

| Dataset | Vectors | Dimensions | Metric | Size (parquet) | Size (binary) |
|---------|---------|------------|--------|----------------|---------------|
| cohere-small-100k | 100K | 768 | COSINE | ~300MB | ~290MB |
| cohere-medium-1m | 1M | 768 | COSINE | ~3GB | ~2.9GB |
| cohere-large-10m | 10M | 768 | COSINE | ~30GB | ~29GB |
| openai-large-5m | 5M | 1536 | COSINE | ~42GB | ~43GB |

## Storage Management

All datasets are stored on the NVMe drive (`/dev/nvme1n1` mounted at `/mnt/data`) to avoid space issues on the root filesystem.

Check available space:
```bash
df -h /mnt/data
```

## Performance Considerations

- **mmap**: Binary files are memory-mapped for zero-copy access
- **Sequential prefill**: `MADV_WILLNEED` for training vectors
- **Random queries**: Efficient for k-NN search patterns
- **Ground truth**: Pre-computed for recall validation

## Troubleshooting

**Missing libraries:**
```bash
/mnt/data/vectordb-bench-env/bin/pip install h5py pandas
```

**Dataset not found:**
Check search paths in `src/dataset_api.c`:
- `<name>` (direct path)
- `./_datasets_prepared/<name>.bin`
- `./utils/datasets/<name>.bin`
- `/var/datasets/<name>.bin`

**Verify binary format:**
```bash
hexdump -C /mnt/data/build-datasets/cohere-small-100k.bin | head -20
# Should show magic: 01 b0 cd de
```

## References

- [vectordb-bench](https://github.com/zilliztech/VectorDBBench) - Dataset source
- [ann-benchmarks](http://ann-benchmarks.com/) - Alternative dataset source
- Valkey dataset API: `src/dataset_api.{c,h}`
- Binary format preparation: `utils/datasets/prepare_binary.py`
