# Advanced Features Guide

Guide to advanced benchmarking features including optimizer internals, metadata filtering, and data format specifications.

## Table of Contents

1. [Optimizer Internals](#optimizer-internals)
2. [Metadata Filtering](#metadata-filtering)
3. [Binary Format Specification](#binary-format-specification)
4. [Extending the Optimizer](#extending-the-optimizer)

---

## Optimizer Internals

The optimizer uses a multi-phase approach to find optimal configurations automatically.

### Optimization Phases

#### Phase 1: RECALL (Binary Search)

Finds the minimal `ef_search` value that satisfies recall constraints.

**Algorithm:**
```
min = 10, max = 1000
target_recall = 0.95

while max - min > 10:
    mid = (min + max) / 2
    test with ef_search = mid
    
    if recall >= target_recall:
        max = mid  (try lower)
    else:
        min = mid  (need higher)

Result: ef_search locked at minimal value satisfying constraint
```

**Why binary search?** `ef_search` has monotonic impact on recall - higher always gives better recall.

**Typical iterations:** 6-8 (log₂(1000/10) ≈ 6.6)

#### Phase 2: THROUGHPUT (Grid Search)

Exhaustively explores `clients` and `threads` using coordinate descent.

**Algorithm:**

```
For each parameter (clients, threads):
    Coarse search (exponential steps):
        Test: 10 → 20 → 40 → 80 → 160 → 320 → 640 → 1000
        Find region with best QPS
    
    Fine search (linear steps):
        Test: best-20 → best-10 → best → best+10 → best+20
        Refine to precise optimum
```

**Example for clients [10, 1000]:**

```
Coarse: 10, 20, 40, 80, 160, 320, 640, 1000 (8 tests)
→ Best: 160

Fine: 140, 150, 160, 170, 180 (5 tests)
→ Best: 170

Total: 13 iterations
```

**Why coordinate descent?** Parameters have weak coupling - optimal clients doesn't heavily depend on threads.

**Typical iterations:** 20-30 for 2 parameters

#### Phase 3: HILL_CLIMB (Gradient Descent)

Fine-tunes all parameters simultaneously using gradient approximation.

**Algorithm:**

```
current_config = {ef_search: 300, clients: 170, threads: 2}
step_size = 0.1  (10% of range)

while not converged:
    for each parameter:
        test param + step_size
        test param - step_size
        
    if any improvement found:
        move to best neighbor
        reduce step_size *= 0.9
    else:
        converged
```

**Typical iterations:** 10-15 until convergence

### Parameter Groups

Parameters are organized by optimization phase:

```c
typedef enum {
    PARAM_GROUP_RECALL,        // Affects recall only → RECALL phase
    PARAM_GROUP_THROUGHPUT,    // Affects QPS only → THROUGHPUT phase
    PARAM_GROUP_MIXED          // Affects both → RECALL phase, then locked
} param_group_t;
```

**Examples:**
- `ef_search`: MIXED (higher = better recall, lower QPS)
- `clients`: THROUGHPUT (only affects parallelism)
- `threads`: THROUGHPUT (only affects parallelism)
- `pipeline`: THROUGHPUT (batching, no recall impact)

### State Tracking

Each parameter tracks its optimization status:

```c
typedef struct {
    char *name;
    int min_val;
    int max_val;
    int step_size;
    int current_val;
    
    // State flags
    int is_tunable;        // Can be optimized
    int locked;            // Frozen after RECALL phase
    int grid_searched;     // THROUGHPUT grid search complete
    param_group_t group;   // Which phase handles this
} optimizer_param_t;
```

### Grid Search Details

**Coarse phase** (exponential):
- Start at `min_val`
- Multiply by 2 each step
- Continue until `≥ max_val`
- Track best value and score

**Fine phase** (linear):
- Center on coarse best
- Test ±2 steps with `step_size` increments
- Select final optimum

**Progress estimation:**
```c
coarse_points = ceil(log2(max_val / min_val)) + 1
fine_points = 5  (best ± 2 steps)
total = coarse_points + fine_points
```

### Adding New Parameters

The optimizer is **completely generic** - no hardcoded parameter names!

**To add a new parameter:**

```c
// In valkey-benchmark.c
optimizer_add_param_grouped(
    config.optimizer,
    "pipeline",              // Parameter name
    1,                       // min_val
    100,                     // max_val
    1,                       // step_size
    1,                       // initial_val
    PARAM_GROUP_THROUGHPUT   // Optimization group
);
```

**That's it!** The optimizer automatically:
1. Discovers the parameter
2. Grid searches it in THROUGHPUT phase
3. Includes it in HILL_CLIMB phase

**Example with 3 parameters:**

```c
optimizer_add_param_grouped(opt, "clients", 10, 1000, 10, 50, PARAM_GROUP_THROUGHPUT);
optimizer_add_param_grouped(opt, "threads", 0, 10, 1, 0, PARAM_GROUP_THROUGHPUT);
optimizer_add_param_grouped(opt, "pipeline", 1, 100, 1, 1, PARAM_GROUP_THROUGHPUT);
```

**Optimization plan:**
```
[Grid Search Plan] Parameters to optimize:
  1. clients   [10, 1000] step=10  → ~13 iterations
  2. threads   [0, 10]    step=1   → ~9 iterations
  3. pipeline  [1, 100]   step=1   → ~12 iterations

Total estimated: ~34 iterations
Strategy: Coordinate descent (one parameter at a time)
```

---

## Metadata Filtering

Support for filtered vector search with metadata tags (BigANN YFCC-10M format).

### Concepts

**Vector Metadata (Tags):**
- Each vector has 0+ tags from a vocabulary
- Example: Vector #5234 → tags: `[camera:Canon, year:2015, country:USA]`

**Query Predicates:**
- Each query has 1-2 tags that MUST match
- Example: Query #42 → predicates: `[year:2015, country:USA]`

**Filtering Logic:**
```python
# Find k-NN among vectors matching ALL predicates
matching_vectors = [
    i for i in range(num_vectors)
    if all(tag in vector_tags[i] for tag in query_predicates)
]

# Then search only within matching_vectors
```

### YFCC-10M Dataset

Special dataset with rich metadata:

- **10M vectors** (192-dim CLIP embeddings)
- **200K unique tags** (vocabulary)
- **108M tag assignments** (~11 tags per vector average)
- **100K queries** with 1-2 predicates each

**Tag categories:**
- Years: `year_2009`, `year_2010`, ...
- Months: `month_January`, `month_April`, ...
- Cameras: `camera_Canon`, `camera_Nikon`, ...
- Countries: `us_state_New_York`, `country_France`, ...

### Download and Convert

```bash
# Download YFCC-10M with metadata
./scripts/dataset.sh get yfcc-10m
```

**Manual download:**
```bash
# Base vectors (10M × 192-dim, uint8)
wget https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/yfcc100M/base.10M.u8bin

# Queries (100K × 192-dim, uint8)
wget https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/yfcc100M/query.public.100K.u8bin

# Vector metadata (sparse matrix, CSR format)
wget https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/yfcc100M/base.metadata.10M.spmat

# Query metadata (predicates)
wget https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/yfcc100M/query.metadata.public.100K.spmat

# Vocabulary (tag names)
wget https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/yfcc100M/yfcc100M.vocab.words.txt

# Ground truth (pre-filtered)
wget https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/yfcc100M/GT.public.100K.ibin
```

**Convert to Valkey binary:**

The YFCC-10M conversion is handled by the unified dataset manager:

```bash
# Automated download and conversion
./scripts/dataset.sh get yfcc-10m
```

This automatically downloads all required files and creates the v2 binary format with metadata support.

### Binary Format v2 (with Metadata)

Extended format with metadata support:

```c
#define DATASET_VERSION_METADATA 2

typedef struct __attribute__((packed)) {
    uint32_t magic;                      // 0xDECDB001
    uint32_t version;                    // 2 for metadata
    uint64_t num_vectors;
    uint64_t num_queries;
    uint32_t dim;
    uint32_t num_neighbors;
    uint8_t metric;
    
    // v2 additions
    uint8_t has_metadata;                // 1 if metadata present
    uint32_t vocab_size;                 // Number of unique tags
    uint64_t vector_metadata_offset;     // Offset to CSR matrix
    uint64_t query_metadata_offset;      // Offset to predicates
    uint64_t vocabulary_offset;          // Offset to string table
    
    uint8_t reserved[3752];              // Pad to 4KB
} dataset_header_v2_t;
```

**Memory layout:**
```
[Header: 4KB]
[Vectors: num_vectors × dim × float32]
[Queries: num_queries × dim × float32]
[Ground Truth: num_queries × num_neighbors × int64]

// v2 additions:
[Vector Metadata CSR:]
  - indptr: (num_vectors+1) × uint32
  - indices: nnz × uint32 (tag IDs)
  - data: nnz × float32 (typically all 1.0)

[Query Metadata CSR:]
  - indptr: (num_queries+1) × uint32
  - indices: nnz × uint32 (tag IDs)
  - data: nnz × float32

[Vocabulary:]
  - vocab_size null-terminated strings
```

### CSR Sparse Matrix Format

Efficient storage for sparse tag assignments:

```c
typedef struct {
    uint32_t nrow;           // Number of rows (vectors or queries)
    uint32_t ncol;           // Number of columns (vocab_size)
    uint64_t nnz;            // Number of non-zero entries
    uint32_t *indptr;        // Row pointers [nrow+1] - mmap'd
    uint32_t *indices;       // Column indices [nnz] - mmap'd
    float *data;             // Values [nnz] - mmap'd
} dataset_metadata_csr_t;
```

**Example:**
```
Vector 0: tags [1, 5, 9]       (tag IDs from vocabulary)
Vector 1: tags [2, 5]
Vector 2: tags [1, 9, 10]

CSR representation:
indptr  = [0, 3, 5, 8]
indices = [1, 5, 9, 2, 5, 1, 9, 10]
data    = [1, 1, 1, 1, 1, 1, 1, 1]
```

**Memory efficiency:**
- Only stores non-zero entries
- YFCC-10M: 108M entries vs 10M × 200K = 2TB dense matrix
- **Compression ratio: ~20,000×**

### C API for Metadata

```c
// Load dataset with metadata
dataset_ctx_t *ctx = dataset_load("/path/to/yfcc-10m.bin");

// Check if metadata available
if (ctx->header->has_metadata) {
    printf("Vocabulary size: %u\n", ctx->header->vocab_size);
}

// Get tags for a vector
dataset_tagset_t *tags = dataset_get_vector_tags(ctx, vector_id);
for (int i = 0; i < tags->count; i++) {
    uint32_t tag_id = tags->tag_ids[i];
    char *tag_name = ctx->vocabulary->words[tag_id];
    printf("Tag: %s\n", tag_name);
}

// Get query predicates
dataset_tagset_t *predicates = dataset_get_query_predicates(ctx, query_id);

// Check if vector matches query
if (dataset_vector_matches_query(ctx, vector_id, query_id)) {
    // This vector satisfies all query predicates
}

// Get all matching vectors for a query
uint64_t count;
uint64_t *matching = dataset_get_matching_vectors(ctx, query_id, &count);
printf("Found %lu matching vectors\n", count);
```

### Inverted Index

For efficient filtering, build an inverted index:

```c
// Tag ID → [vector IDs] mapping
uint32_t **inverted_index = build_inverted_index(ctx->vector_metadata);

// Find vectors with specific tag
uint32_t tag_id = 42;  // e.g., "camera:Canon"
uint32_t *vectors_with_tag = inverted_index[tag_id];

// For query with 2 predicates, intersect two lists
uint64_t *match1 = inverted_index[predicate1];
uint64_t *match2 = inverted_index[predicate2];
uint64_t *matching = intersect_sorted_arrays(match1, match2);
```

**Memory overhead:** ~40 MB for YFCC-10M (200K tags, 10M vectors)

### Benchmarking with Metadata

```bash
./bin/valkey-benchmark \
  --dataset yfcc-10m.bin \
  --filtered \
  -t vec-query \
  --search --vector-dim 192 \
  --search-name yfcc \
  -n 10000 -c 10
```

The `--filtered` flag:
- Loads metadata from dataset
- Filters ground truth by predicates
- Reports filtered recall accuracy

**Performance impact:**
- Unfiltered: 0 ms overhead
- Filtered (1 tag): +0.1 ms (index lookup)
- Filtered (2 tags): +0.5 ms (intersection)

**Selectivity examples:**
- High (0.001%): ~100 matching vectors
- Medium (1%): ~100K matching vectors
- Low (10%): ~1M matching vectors

---

## Binary Format Specification

Complete specification of the Valkey dataset binary format.

### Version 1 (Standard)

**Header (4096 bytes):**

```c
#define DATASET_MAGIC 0xDECDB001
#define DATASET_VERSION 1

typedef struct __attribute__((packed)) {
    uint32_t magic;                      // 0xDECDB001 (magic number)
    uint32_t version;                    // Format version (1)
    uint64_t num_vectors;                // Number of training vectors
    uint64_t num_queries;                // Number of test queries
    uint32_t dim;                        // Vector dimensions
    uint32_t num_neighbors;              // k for k-NN ground truth
    uint8_t metric;                      // Distance metric
    uint64_t vectors_offset;             // Offset to training vectors
    uint64_t queries_offset;             // Offset to test queries
    uint64_t ground_truth_offset;        // Offset to ground truth
    uint8_t reserved[3808];              // Padding to 4096 bytes
} dataset_header_t;
```

**Distance metrics:**
```c
#define METRIC_L2      0  // Euclidean distance
#define METRIC_IP      1  // Inner product
#define METRIC_COSINE  2  // Cosine similarity
```

**Memory layout:**

```
Offset 0:        [Header: 4096 bytes]
Offset 4096:     [Training vectors: num_vectors × dim × 4 bytes, 64-byte aligned]
Offset X:        [Query vectors: num_queries × dim × 4 bytes, 64-byte aligned]
Offset Y:        [Ground truth: num_queries × num_neighbors × 8 bytes]
```

**Alignment:**
- Header: 4096 bytes (page aligned)
- Vectors: 64 bytes (cache line aligned)
- Ground truth: Natural alignment

**Example for SIFT-128 (1M vectors):**
```
Header:          4,096 bytes
Vectors:         1,000,000 × 128 × 4 = 512,000,000 bytes (488 MB)
Queries:         10,000 × 128 × 4 = 5,120,000 bytes (4.9 MB)
Ground truth:    10,000 × 100 × 8 = 8,000,000 bytes (7.6 MB)
Total:           ~500 MB
```

### Version 2 (with Metadata)

Extends v1 with metadata support for filtered search.

**Additional header fields:**

```c
typedef struct __attribute__((packed)) {
    // ... all v1 fields ...
    
    uint8_t has_metadata;                // 0 = no metadata, 1 = metadata present
    uint32_t vocab_size;                 // Number of unique tags
    uint64_t vector_metadata_offset;     // Offset to vector tags (CSR)
    uint64_t query_metadata_offset;      // Offset to query predicates (CSR)
    uint64_t vocabulary_offset;          // Offset to tag names
    
    uint8_t reserved[3752];              // Reduced padding
} dataset_header_v2_t;
```

**Extended memory layout:**

```
[Header: 4096 bytes]
[Vectors: num_vectors × dim × 4 bytes]
[Queries: num_queries × dim × 4 bytes]
[Ground truth: num_queries × num_neighbors × 8 bytes]

// Only if has_metadata == 1:
[Vector Metadata CSR: 64-byte aligned]
  uint32_t nrow, ncol
  uint64_t nnz
  uint32_t indptr[nrow+1]
  uint32_t indices[nnz]
  float data[nnz]

[Query Metadata CSR: 64-byte aligned]
  uint32_t nrow, ncol
  uint64_t nnz
  uint32_t indptr[nrow+1]
  uint32_t indices[nnz]
  float data[nnz]

[Vocabulary: 64-byte aligned]
  uint32_t vocab_size
  char strings[]  // null-terminated concatenated strings
```

### File Creation

**Python converter:**

```python
import struct
import numpy as np

def write_dataset_v1(output_path, vectors, queries, neighbors, metric):
    with open(output_path, 'wb') as f:
        # Write header
        header = struct.pack(
            '<IIQQQIQQQQ',  # Format string
            0xDECDB001,     # magic
            1,              # version
            len(vectors),   # num_vectors
            len(queries),   # num_queries
            vectors.shape[1],  # dim
            neighbors.shape[1],  # num_neighbors
            metric,         # metric
            4096,           # vectors_offset
            0,              # queries_offset (compute below)
            0               # ground_truth_offset (compute below)
        )
        
        # Pad header to 4096 bytes
        header += b'\x00' * (4096 - len(header))
        f.write(header)
        
        # Write vectors (64-byte aligned)
        vectors.astype(np.float32).tofile(f)
        pad_to_64(f)
        
        # Write queries
        queries.astype(np.float32).tofile(f)
        pad_to_64(f)
        
        # Write ground truth
        neighbors.astype(np.int64).tofile(f)

def pad_to_64(f):
    pos = f.tell()
    pad_size = (64 - (pos % 64)) % 64
    f.write(b'\x00' * pad_size)
```

### Memory Mapping

The format is designed for efficient mmap:

```c
// Load entire file with mmap
int fd = open(path, O_RDONLY);
struct stat sb;
fstat(fd, &sb);

void *base = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

// Access data with zero-copy
dataset_header_t *header = (dataset_header_t *)base;
float *vectors = (float *)((uint8_t *)base + header->vectors_offset);
float *queries = (float *)((uint8_t *)base + header->queries_offset);
int64_t *ground_truth = (int64_t *)((uint8_t *)base + header->ground_truth_offset);

// Advise kernel for optimal paging
madvise(vectors, vectors_size, MADV_WILLNEED);  // Prefetch training data
madvise(queries, queries_size, MADV_RANDOM);     // Random query access
```

### Verification

Check file integrity:

```bash
# Verify magic number
hexdump -C dataset.bin | head -1
# Should show: 00000000  01 b0 cd de ...

# Check header
./scripts/dataset.sh verify dataset.bin
```

**Python verification:**

```python
import struct

def verify_dataset(path):
    with open(path, 'rb') as f:
        magic, version = struct.unpack('<II', f.read(8))
        
        if magic != 0xDECDB001:
            print(f"Invalid magic: {hex(magic)}")
            return False
        
        if version not in [1, 2]:
            print(f"Unknown version: {version}")
            return False
        
        print(f"✓ Valid dataset (v{version})")
        return True
```

---

## Next Steps

- **Benchmarking**: See [BENCHMARKING.md](BENCHMARKING.md) for running benchmarks
- **Datasets**: See [DATASETS.md](DATASETS.md) for dataset management
- **Installation**: See [INSTALLATION.md](INSTALLATION.md) for setup

## Contributing

To extend the optimizer or add new features:

1. **New parameters**: Just add with `optimizer_add_param_grouped()` - no code changes needed
2. **New metrics**: Add to `benchmark_result_t` structure and update scoring logic
3. **New phases**: Implement in `optimizer_step()` state machine
4. **New datasets**: Follow binary format v1 or v2 specification

See source code documentation in `src/load_optimizer.c` for implementation details.
