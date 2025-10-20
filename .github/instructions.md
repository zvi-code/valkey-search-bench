# Valkey Vector Benchmark - AI Agent Instructions

## Project Scope & Focus

This is the **standalone Valkey Vector Benchmark** package - a specialized toolkit for vector search performance testing.

**Primary Purpose**: 
- Benchmark vector search operations on Valkey/Redis clusters
- Dataset download/conversion pipeline for vector search testing
- Performance analysis and recall validation
- Integration with vectordb-bench datasets

**This is NOT the full Valkey database** - this package contains only the benchmarking tools extracted from the main Valkey repository.

## Architecture Overview

### 1. Core Source Files

**Benchmark & Search**:
- `src/valkey-benchmark.c` - Main benchmark tool (focus on search functionality)
- `src/valkey-benchmark-vgen.{c,h}` - Vector generator integration
- `src/valkey-benchmark-utils.{c,h}` - Shared cluster utilities

**Dataset Infrastructure**:
- `src/dataset_api.{c,h}` - Binary dataset format reader (4KB-aligned headers)
- `src/cluster-scan.{c,h}` - Generic parallel cluster scanner framework
- `src/vector-id-mapping.{c,h}` - Vector ID to cluster tag mapping

**Core Utilities** (from Valkey):
- `src/core/` - Essential Valkey utilities (zmalloc, sds, dict, etc.)
- Required for benchmark functionality

**Testing & Utilities**:
- `vector-testing/*` - Multi-dataset testing scripts and workflows
- `utils/datasets/*` - Dataset preparation and conversion utilities
- `utils/vgenerator/*` - Vector generation library

### 2. Memory Allocator (CRITICAL)

**This package uses jemalloc** - the same memory allocator as Valkey:

- **Location**: `build-debug/jemalloc-build/` (copied from Valkey build)
- **Library**: `libjemalloc.a` (42.8MB static library)
- **Why required**: Core utilities expect jemalloc functions (je_malloc, je_nallocx)
- **Build flag**: `USE_JEMALLOC` must be defined

**CMake Configuration**:
- `deps/CMakeLists.txt` - Imports pre-built jemalloc as imported target
- `src/CMakeLists.txt` - Links jemalloc and sets compile definitions

**DO NOT** try to build with system allocator - it will cause segfaults.

### 3. Dataset Pipeline Architecture

**Three-Stage Conversion**: Parquet (vectordb-bench) → HDF5 (intermediate) → Binary (Valkey format)

**Critical Files**:
- `src/dataset_api.{c,h}` - Binary dataset format reader (4KB-aligned headers)
- `convert_parquet_to_hdf5_fast.py` - Memory-optimized converter (5000 vector batches)
- `utils/datasets/prepare_binary.py` - HDF5 to binary converter
- `convert_vectordb_dataset.sh` - End-to-end wrapper script

**Dataset Binary Format** (`dataset_api.h`):
```c
#define DATASET_MAGIC 0xDECDB001
typedef struct {
    uint32_t magic, version;
    char dataset_name[256];
    uint32_t dim;
    uint64_t num_vectors, num_queries;
    uint32_t num_neighbors;
    uint64_t vectors_offset, queries_offset, ground_truth_offset;
    uint8_t reserved[3808];  // Pad to 4KB
} dataset_header_t;
```

**Why This Matters**: All ground truth (nearest neighbors) is precomputed in datasets. This enables accurate recall validation during benchmarks.

### 4. Cluster Tag Mapping System

**Purpose**: Enable recall validation in Redis cluster mode by tracking vector ID → cluster tag mappings.

**Components**:
- `src/cluster-scan.{c,h}` - Generic parallel cluster scanner framework
- `src/vector-id-mapping.{c,h}` - Vector-specific key processing
- `src/cluster-utils.{c,h}` - Shared key parsing utilities

**How It Works**:
1. At benchmark start, parallel workers scan cluster for `prefix*` keys
2. Extract vector IDs and cluster tags: `zvec_large_{06S}:000000000123456`
3. Build in-memory hash table: `vecid → cluster_tag`
4. During recall validation, reconstruct proper keys for missing neighbors

**Performance**: 1M+ keys/second scanning, scales with cluster node count.

### 5. Adaptive Load Optimizer

**Purpose**: Automatically tune benchmark parameters (clients, threads, ef_search) to maximize or minimize objectives while satisfying constraints.

**Components**:
- `src/load_optimizer.{c,h}` - Core optimization engine
- Integrated into `src/valkey-benchmark.c`

**Key Algorithmic Features**:
1. **Binary Search for RECALL Phase**: Finds minimal ef_search satisfying recall constraints in ~7 iterations (vs gradient descent's many more)
2. **Grid Search for THROUGHPUT Phase**: Exhaustively explores parameter space with coarse (exponential) + fine (linear) stages
3. **Phased Optimization**: RECALL → THROUGHPUT → HILL_CLIMB → REFINEMENT for domain-aware convergence
4. **Parameter Locking**: Preserves optimal values from earlier phases (e.g., ef_search locked after RECALL)

**Optimization Phases**:
```
INIT (1 iter)
  ↓
FEASIBILITY (1-10 iters) - Find any working config
  ↓
RECALL (binary search, ~7 iters) - Optimize ef_search for recall constraints
  Lock ef_search at minimal value
  ↓
THROUGHPUT (grid search, ~20 iters) - Exhaustively test clients/threads
  Coarse: exponential steps (10→20→40→80...)
  Fine: linear steps around best (±2*step_size)
  ↓
HILL_CLIMB (gradient descent, 5-10 iters) - Fine-tune all params together
  ↓
REFINEMENT (coordinate descent, 3-5 iters) - Final adjustments
  ↓
CONVERGED
```

**Usage Example**:
```bash
# Maximize QPS with recall ≥ 0.95
./bin/valkey-benchmark --optimize \
  --optimize-objective "maximize:qps" \
  --optimize-constraint "recall_avg:gt:0.95" \
  -h $HOST --cluster --dataset cohere-medium-1m.bin \
  -t vec-query --search --vector-dim 768
```

**Documentation**: See `docs/GRID_SEARCH_IMPLEMENTATION.md` for detailed algorithm explanation.

## Critical Workflows

### Build System (CMake-based)

```bash
# From repository root
mkdir -p build-debug
cd build-debug
cmake -DCMAKE_BUILD_TYPE=Debug ..
make

# For release builds
mkdir -p build-release
cd build-release
cmake -DCMAKE_BUILD_TYPE=Release ..
make
```

**Binary location**: `build-debug/bin/valkey-benchmark` or `build-release/bin/valkey-benchmark`

**Important**: The jemalloc library must exist at `build-debug/jemalloc-build/` before building. It was copied from the original Valkey repository during migration.

### Dataset Download & Conversion

**Python Environment** (required first):
```bash
# Setup on NVMe for large datasets
cd /mnt/data
python3 -m venv vectordb-bench-env
source vectordb-bench-env/bin/activate
pip install vectordb-bench==1.0.10 h5py pandas pyarrow numpy
```

**Download Dataset**:
```bash
# Uses download_dataset.py (generic script)
/mnt/data/vectordb-bench-env/bin/python download_dataset.py COHERE 1000000
/mnt/data/vectordb-bench-env/bin/python download_dataset.py OPENAI 5000000
```

**Convert to Binary**:
```bash
# All-in-one wrapper
./convert_vectordb_dataset.sh cohere/cohere_medium_1m cohere-medium-1m COSINE
./convert_vectordb_dataset.sh openai/openai_large_5m openai-large-5m COSINE
```

**Available Datasets**: COHERE (100K/1M/10M), OPENAI (500K/5M), SIFT (500K/5M), GIST (100K/1M), LAION (100M). See `VECTORDB_BENCH_COMPLETE_GUIDE.md`.

### Running Benchmarks

**Two-Phase Pattern** (CRITICAL - always use):

```bash
# Phase 1: Ground Truth Insertion (load ALL vectors first)
./bin/valkey-benchmark -h $HOST --cluster --rfr no --dataset cohere-medium-1m.bin -t vec-ground-truth --search --vector-dim 768 --search-name cohere_1m --search-prefix zvec_: -n 1000000 -c 10 --clean

# Phase 2: Query Benchmark (with recall tracking)
./bin/valkey-benchmark -h $HOST --cluster --rfr no --dataset cohere-medium-1m.bin -t vec-query --search --vector-dim 768 --search-name cohere_1m --search-prefix zvec_: -n 10000 -c 10 --threads 10
```

**Why Two Phases**: Ground truth must be fully indexed before queries, otherwise recall validation fails. This is NOT optional.

### ef_search Parameter Testing

```bash
# Quick demo (3 values, 30 seconds)
./demo_ef_search_simple.sh

# Full analysis (8 values, 5-10 minutes)
./test_ef_search_working.sh

# Results saved to timestamped CSV
```

**What ef_search Controls**: HNSW search accuracy vs speed tradeoff. Higher values = better recall, slower queries. Typical range: 50-500.

## Project-Specific Conventions

### 1. Single-Line Commands (CRITICAL)

**ALWAYS use single-line commands** for valkey-benchmark - shell backslash continuations cause silent failures:

```bash
# ✅ CORRECT - single line
./bin/valkey-benchmark -h $HOST --cluster --rfr no --dataset file.bin -t vec-query --search --vector-dim 768 -n 10000

# ❌ WRONG - backslash continuation
./bin/valkey-benchmark -h $HOST --cluster \
  --rfr no --dataset file.bin \
  -t vec-query --search
```

This is documented in `DATASET_TESTING_GUIDE.md` line 77 and repeatedly emphasized.

### 2. NVMe Storage Pattern

**All datasets MUST use NVMe** (`/mnt/data/`):
- Parquet files: `/mnt/data/datasets/<name>/`
- HDF5 files: `/mnt/data/datasets/<name>.hdf5`
- Binary files: `/mnt/data/build-datasets/<name>.bin`
- Symlinks in: `build-debug/<name>.bin` (symlinks to /mnt/data/build-datasets/)

**Why**: Datasets are 30-40GB+. Root filesystem is too small. This is non-negotiable.

### 3. Thread Safety & Placeholders

**Old System** (removed): Global `randoms` array caused thread-safety issues.

**New System**: Per-client placeholder state in `client` struct:
```c
typedef struct _client {
    // ...
    char **randoms;           // Per-client random values
    int randptr[BENCHMARK_PLACEHOLDER_COUNT];
    int seq_counters[BENCHMARK_PLACEHOLDER_COUNT];
    // ...
} client;
```

**When adding placeholders**: Never use global state. Always use per-client arrays.

### 4. Cluster Mode Defaults

**Always assume cluster mode** unless explicitly testing standalone:
- Use `--cluster` flag
- Use `--rfr no` (read-from-replica: no) for predictable routing
- Use `-c --no-auth-warning` for CLI commands
- Set `HOST` environment variable for test scripts

### 5. Recall Validation Pattern

```c
// In dataset_compute_recall():
if (recall < 0.80) {  // Threshold for validation
    // Reconstruct key using cluster tag mapping
    char proper_key[256];
    snprintf(proper_key, sizeof(proper_key), "%s{%s}:%012lu", 
             prefix, cluster_tag, neighbor_id);
    // Log missing neighbor for debugging
}
```

**Purpose**: Distinguish between "vector missing from index" vs "HNSW algorithm didn't find it".

## Testing & Validation

### Quick Validation Checklist

After ANY valkey-benchmark changes:

1. **Build test**: `cd build-debug && make`
2. **Connection test**: `./bin/valkey-benchmark --help` (verify binary runs)
3. **Small dataset test**: Use GloVe-25 (1.18M vectors, fastest)
4. **Recall validation**: Check for "Average recall: XX%" in output
5. **Performance check**: QPS should be 700+ for 1M+ vectors

### Common Issues & Solutions

**"Connection refused"**: Check `HOST` env var, verify target cluster is running.

**"Index limit reached"**: List indexes with `FT._LIST`, drop old ones.

**"Recall validation failed"**: Ensure Phase 1 (ground truth insertion) completed fully.

**"Segmentation fault"**: Check jemalloc is properly linked - run `nm bin/valkey-benchmark | grep je_malloc`

**"Low recall (<70%)"**: Either missing vectors or ef_search too low. Validate index count with `FT.INFO`.

**"Undefined reference to je_*"**: Jemalloc not linked properly. Check CMake configuration.

## Documentation Hierarchy

Read in this order for fastest onboarding:

1. `README.md` - Package overview and quick start
2. `VECTORDB_BENCH_COMPLETE_GUIDE.md` - Full dataset pipeline walkthrough
3. `DATASET_TESTING_GUIDE.md` - GloVe-25 step-by-step example
4. `EF_SEARCH_TESTING_GUIDE.md` - Parameter tuning methodology
5. `PRECOMPUTE_IMPLEMENTATION.md` - Ground truth precomputation (optional)

**Quick reference**: `VECTORDB_DATASETS.md` for dataset sizes and conversion commands.

## Key File Locations

**Core benchmark code**:
- `src/valkey-benchmark.c` - Main benchmark tool (search functionality)
- `src/valkey-benchmark-vgen.{c,h}` - Vector generator integration
- `src/valkey-benchmark-utils.{c,h}` - Shared cluster utilities

**Core utilities** (from Valkey):
- `src/core/` - Essential Valkey utilities (zmalloc, sds, dict, etc.)

**Dataset handling**:
- `src/dataset_api.{c,h}` - Binary format reader
- `utils/datasets/*` - Dataset preparation scripts (HDF5→Binary conversion)
- `convert_parquet_to_hdf5_fast.py` - Parquet→HDF5 (memory-optimized)

**Cluster utilities**:
- `src/cluster-scan.{c,h}` - Generic parallel scanner
- `src/vector-id-mapping.{c,h}` - Vector ID extraction

**Testing infrastructure**:
- `vector-testing/*` - Multi-dataset test scripts
- `utils/vgenerator/*` - Vector generation library
- `test_ef_search_working.sh` - ef_search parameter analysis
- `demo_ef_search_simple.sh` - Quick ef_search demo
- `convert_vectordb_dataset.sh` - All-in-one dataset conversion

**Build system**:
- `CMakeLists.txt` - Root CMake configuration
- `deps/CMakeLists.txt` - Dependency configuration (jemalloc import)
- `src/CMakeLists.txt` - Benchmark binary configuration
- `build-debug/jemalloc-build/` - Pre-built jemalloc library

## Performance Expectations

| Scale | Vectors | Recall | QPS | Latency | Dataset |
|-------|---------|--------|-----|---------|---------|
| Small | 100K | 95%+ | 2000+ | <5ms | COHERE-100K |
| Medium | 1M | 90%+ | 1500+ | <8ms | COHERE-1M |
| Large | 5-10M | 85%+ | 700+ | <15ms | OPENAI-5M |

**Cluster scanning**: 1M+ keys/second, scales linearly with node count.

**Conversion speed**: ~50-100K vectors/second (parquet→HDF5), depends on dimensions.

## Development Workflow Rules

### 1. Documentation Maintenance (CRITICAL)

**Always keep documentation synchronized with code changes**:

- ✅ **Added script argument?** → Update documentation for that script immediately
- ✅ **New package required?** → Add installation step to prerequisites section
- ✅ **Changed command syntax?** → Update all examples in guides
- ✅ **New environment variable?** → Document it with examples
- ✅ **Modified workflow?** → Update step-by-step instructions

**Goal**: New users should be able to download, build, and run benchmarks by following the guide without asking questions.

**Affected files to update**:
- `README.md` - Package overview and quick start
- `VECTORDB_BENCH_COMPLETE_GUIDE.md` - Dataset download/conversion pipeline
- `DATASET_TESTING_GUIDE.md` - GloVe and general testing workflows
- `EF_SEARCH_TESTING_GUIDE.md` - Parameter tuning workflows
- Script-specific sections for any modified `.sh` files

### 2. Git Commit Discipline

**Commit at major milestones with descriptive messages**:

```bash
# ✅ Good commit points:
# - Feature complete (e.g., "Add cluster tag mapping for recall validation")
# - Before major refactoring (e.g., "Checkpoint before modularizing benchmark architecture")
# - After fixing critical bug (e.g., "Fix thread-safety issue in placeholder system")
# - Documentation synchronized (e.g., "Update guides for ef_search parameter testing")

# ✅ Good commit message format:
git commit -m "Add parallel cluster scanner for vector ID mapping

- Implement generic cluster-scan framework with callbacks
- Add vector-id-mapping module for recall validation
- Update DATASET_TESTING_GUIDE.md with cluster scanning section
- Performance: 1M+ keys/sec scanning rate"
```

**ALWAYS commit before**:
- Major architectural changes
- Risky refactoring operations
- Deleting significant amounts of code
- Changing core benchmark logic

### 3. File Operations (MUST ASK FIRST)

**NEVER do these without explicit user permission**:

❌ `rm -rf` or deleting files  
❌ `git reset --hard`  
❌ `git clean -fd`  
❌ Creating new files (`.c`, `.h`, `.py`, `.sh`, `.md`)  
❌ Renaming files or directories  
❌ Moving files between directories  

**Always ask**: "Should I create `<filename>` for this functionality?" or "Should I delete `<filename>`?"

### 4. Communication Style

**Keep responses concise and action-oriented**:

- ❌ **Don't**: Write long summaries of what was changed unless explicitly asked
- ✅ **Do**: Update the relevant `*_GUIDE.md` file with the changes
- ✅ **Do**: Show brief output of key commands (build status, test results)
- ✅ **Do**: Highlight errors or unexpected behavior immediately

**Example good response**:
```
Updated cluster-scan.c with batch size optimization.
Build successful. Updated CLUSTER_SCANNING_ARCHITECTURE.md section 3.2.
```

**Example bad response** (unless user asks for summary):
```
I've made several changes to improve the cluster scanning performance. 
First, I modified the batch size from 1000 to 5000... [long explanation]
The changes affect three main components... [detailed breakdown]
Here's what each change does... [more details]
```

### 5. Safety Checks Before Major Operations

Before ANY operation that could lose work:

1. Check for uncommitted changes: `git status`
2. Suggest commit if working changes exist
3. Get explicit confirmation for destructive operations

## When to Ask for Clarification

- New distance metrics or index types (requires server-side support)
- Performance degradation >20% without obvious cause
- Recall drops below 70% (may indicate data corruption)
- Build failures or linking errors
- **Creating any new files** (scripts, source files, documentation)
- **Deleting existing files** or performing git hard resets
- **Major architectural changes** before committing checkpoint
- **Modifying jemalloc configuration** (critical for stability)

## Package-Specific Notes

### Differences from Original Valkey Repository

1. **Standalone package**: Does not contain Valkey server code
2. **Jemalloc**: Uses pre-built jemalloc copied from Valkey build
3. **Focus**: Only vector benchmarking, dataset tools, and testing scripts
4. **Size**: ~150 files vs 1000+ in full Valkey repository
5. **Build**: Only builds `valkey-benchmark` binary, not full server

### Integration with Valkey Clusters

This package is designed to benchmark **existing Valkey/Redis clusters**:

- Does not start or manage server instances
- Requires external cluster with vector search enabled
- Use `--cluster` flag and `-h <hostname>` to connect
- Supports MemoryDB, ElastiCache, and open-source Valkey/Redis

### Binary Size

- **With jemalloc**: ~6.3MB (includes 42.8MB static library)
- **Without jemalloc**: Would be ~920KB but will segfault
- This is expected and required for compatibility

## Examples from Codebase

**Dataset binary format** (`dataset_api.h`):
```c
#define DATASET_MAGIC 0xDECDB001
typedef struct {
    uint32_t magic, version;
    char dataset_name[256];
    uint32_t dim;
    uint64_t num_vectors, num_queries;
    uint32_t num_neighbors;
    uint64_t vectors_offset, queries_offset, ground_truth_offset;
    uint8_t reserved[3808];  // Pad to 4KB
} dataset_header_t;
```

**Parallel cluster scan** (`cluster-scan.c`):
```c
clusterScanConfig config = {
    .match_pattern = "zvec_*",
    .nodes = cluster->nodes,
    .node_count = cluster->nodes_count,
    .key_processor = processVectorKey,
    .user_data = &mapping_data,
};
scanCluster(&config, &results);
```

**Vector ID mapping integration** (`vector-id-mapping.c`):
```c
// Extract vector ID and cluster tag from key
// Format: "zvec_large_{06S}:000000000123456"
int processVectorKey(const char *key, void *user_data, int thread_id) {
    uint64_t vec_id;
    char cluster_tag[16];
    if (parseVectorKey(key, prefix, &vec_id, cluster_tag)) {
        storeMapping(vec_id, cluster_tag);
    }
    return 0;
}
```
