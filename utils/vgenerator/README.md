# Vector Generator for Benchmarking

A high-performance, deterministic vector generator designed for benchmarking vector databases and similarity search systems. Optimized for ARM Graviton processors with NEON SIMD instructions.

## Features

- **Deterministic Generation**: Same key always produces identical vectors
- **Configurable Parameters**: Dimensions, sparsity, clustering, and scaling
- **High Performance**: Optimized for ARM NEON with millions of vectors/second
- **Thread Safety**: Concurrent vector generation and iteration
- **Ground Truth Support**: Pre-computed nearest neighbors for recall testing
- **Memory Efficient**: Bloom filters for deletion tracking, minimal memory overhead

## Quick Start

```bash
# Build all test programs
make all

# Run comprehensive tests
make test_comprehensive && ./test_comprehensive

# Run performance benchmarks
make test_vector_generator && ./test_vector_generator -d 768 -n 100000
```

## Configuration Parameters

The generator supports various configuration options through `generator_config_t`:

### Core Parameters
- **`dimensions`**: Vector dimensionality (4-2048, typical: 128, 768, 1536)
- **`initial_capacity`**: Expected number of vectors for memory allocation
- **`seed`**: Random seed for deterministic generation

### Advanced Features
- **`sparsity`**: Fraction of zero values (0.0 = dense, 0.95 = very sparse)
- **`radius`**: Vector magnitude scaling factor (affects clustering spread)
- **`num_centroids`**: Number of cluster centers (0 = no clustering)

### Example Configurations

```c
// Dense embeddings (typical for neural networks)
generator_config_t dense_config = {
    .dimensions = 768,
    .initial_capacity = 1000000,
    .sparsity = 0.0,
    .radius = 1.0,
    .num_centroids = 0,
    .seed = 42
};

// Sparse vectors with clustering
generator_config_t sparse_clustered = {
    .dimensions = 1536,
    .initial_capacity = 100000,
    .sparsity = 0.8,        // 80% zeros
    .radius = 10.0,         // Larger spread
    .num_centroids = 20,    // 20 cluster centers
    .seed = 12345
};
```

## API Usage

### Basic Vector Generation

```c
#include "vector_generator.h"

// Initialize generator
generator_config_t config = {
    .dimensions = 128,
    .initial_capacity = 10000,
    .sparsity = 0.1,
    .radius = 5.0,
    .num_centroids = 5,
    .seed = 42
};

vector_generator_t* gen = vg_init(&config);

// Generate single vector
float* vector = zmalloc(128 * sizeof(float));
vg_generate_vector_from_key(gen, 12345, vector);

// Cleanup
free(vector);
vg_destroy(gen);
```

### Bulk Generation with Iterators

```c
// Create ingestion iterator for bulk generation
vector_iterator_t* iter = vg_get_ingestion_iterator(gen, 10000, GEN_ORDER_RANDOM);

vector_t vec;
while (vg_iterator_next(iter, &vec)) {
    // Process vector: vec.key, vec.data
    // Remember to zfree(vec.data) after use
    zfree(vec.data);
}

vg_iterator_destroy(iter);
```

### Query Generation with Ground Truth

```c
// Generate query vectors with pre-computed nearest neighbors
vector_iterator_t* query_iter = vg_get_query_iterator(gen, 100);

query_vector_t query;
while (vg_iterator_next_query(query_iter, &query)) {
    // Query vector: query.vector.key, query.vector.data
    // Ground truth: query.ground_truth[0..9] (top-10 neighbors)
    
    printf("Query %lu has %d ground truth neighbors\n", 
           query.vector.key, 10);
    
    zfree(query.vector.data);
}

vg_iterator_destroy(query_iter);
```

## Performance Characteristics

### Generation Speed (ARM Graviton 3)
- **128-dim**: ~5.4M vectors/second
- **768-dim**: ~930K vectors/second  
- **1536-dim**: ~470K vectors/second

### Distance Computation Speed
- **128-dim**: ~2.2M L2 distances/second
- **768-dim**: ~367K L2 distances/second
- **1536-dim**: ~184K L2 distances/second

### Memory Usage
- **Per vector**: `dimensions * sizeof(float)` (4 bytes per dimension)
- **Generator overhead**: <1MB for typical configurations
- **Bloom filter**: ~10 bits per expected vector for deletion tracking

## Key Management

The generator uses a sophisticated key allocation system:

- **Reserved Range**: Keys 1-1,000,000 for queries and ground truth
- **General Range**: Keys 1,000,001+ for regular vectors
- **Thread Safe**: Atomic key allocation prevents collisions
- **Deletion Tracking**: Bloom filter tracks deleted keys efficiently

## Testing and Validation

### Comprehensive Test Suite

```bash
# Run all validation tests
./test_comprehensive
```

Tests include:
- ✅ **Vector uniqueness**: No duplicate vectors generated
- ✅ **Deterministic reproduction**: Same key → same vector
- ✅ **Sparsity accuracy**: Correct zero/non-zero ratios
- ✅ **Radius scaling**: Proper magnitude scaling
- ✅ **Centroid clustering**: Vectors cluster around centroids
- ✅ **Ground truth accuracy**: 100% recall on test queries
- ✅ **Thread safety**: Concurrent generation works correctly

### Recall Testing

```bash
# Test ground truth accuracy with different configurations
./test_recall_enhanced
```

### Performance Benchmarks

```bash
# Test generation speed across dimensions
./test_vector_generator -d 768 -n 100000 -t 4
```

## Implementation Details

### Deterministic Generation
- Uses PCG (Permuted Congruential Generator) for high-quality randomness
- Hash-based key-to-seed mapping ensures reproducibility
- Each vector dimension generated independently

### SIMD Optimization
- ARM NEON instructions for vectorized operations
- Optimized L2 distance computation
- Compiler auto-vectorization friendly code

### Memory Management
- Stack-based arrays for small vectors
- Efficient bloom filters for large-scale deletion tracking
- Thread-safe atomic operations for key allocation

## Building from Source

### Prerequisites
- GCC with C11 support
- ARM processor with NEON support (or x86_64 fallback)
- POSIX threads (pthread)
- Math library (libm)

### Build Options
```bash
# Standard build
make all

# With specific optimizations
GRAVITON=3 make all    # Graviton 3 optimized
GRAVITON=4 make all    # Graviton 4 optimized

# Run tests
make run-tests

# Clean build
make clean
```

### Compiler Flags
```bash
# Optimized for ARM Graviton
-std=c11 -Wall -O3 -march=armv8.2-a+crypto+fp16+simd -D__ARM_NEON -D_GNU_SOURCE
```

## Use Cases

- **Vector Database Benchmarking**: Generate realistic datasets for performance testing
- **Similarity Search Evaluation**: Ground truth for recall/precision measurement  
- **Machine Learning**: Synthetic embeddings for algorithm development
- **Performance Testing**: High-throughput vector generation for load testing

## License

Open source implementation for research and benchmarking purposes.

---

For questions or contributions, see the test programs for detailed usage examples.