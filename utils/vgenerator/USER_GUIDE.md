# Vector Generator User Guide

## Table of Contents
1. [Overview](#overview)
2. [Dataset Preparation](#dataset-preparation)
3. [Integration with Benchmark Frameworks](#integration-with-benchmark-frameworks)
4. [Workload Patterns](#workload-patterns)
5. [Recall Testing](#recall-testing)
6. [Performance Optimization](#performance-optimization)

## Overview

The Vector Generator is designed for comprehensive benchmarking of vector databases. It provides deterministic vector generation with ground truth computation for recall testing, making it ideal for evaluating vector database performance under various workloads.

### Key Capabilities
- Generate millions of vectors with consistent properties
- Pre-compute ground truth for recall measurement
- Track deletions efficiently with bloom filters
- Support mixed workloads (insert/query/delete/update)

## Dataset Preparation

### 1. Planning Your Dataset

Before generating vectors, determine your benchmark requirements:

```c
// Example: E-commerce product embeddings scenario
// - 10M products with 768-dim embeddings
// - Clustered by category (100 categories)
// - Some sparse features (20% sparsity)
// - Need to test 100K queries with recall measurement

generator_config_t config = {
    .dimensions = 768,
    .initial_capacity = 10000000,  // 10M vectors
    .sparsity = 0.2,                // 20% sparse
    .radius = 5.0,                  // Moderate spread
    .num_centroids = 100,           // 100 clusters (categories)
    .seed = 42                      // Deterministic generation
};
```

### 2. Generating Base Dataset

```c
#include "vector_generator.h"

// Initialize generator
generator_t* gen = generator_create(&config);

// Pre-allocate vectors for bulk loading
size_t batch_size = 100000;
float* batch_vectors = aligned_alloc(32, batch_size * config.dimensions * sizeof(float));

// Generate initial dataset
for (size_t i = 0; i < 10000000; i += batch_size) {
    size_t current_batch = (i + batch_size > 10000000) ? 10000000 - i : batch_size;

    // Generate batch of vectors
    #pragma omp parallel for
    for (size_t j = 0; j < current_batch; j++) {
        uint64_t key = i + j;
        generator_get_vector(gen, key, &batch_vectors[j * config.dimensions]);
    }

    // Insert batch into vector database
    vector_db_bulk_insert(db, i, batch_vectors, current_batch, config.dimensions);
}
```

### 3. Preparing Query Workload

Generate query vectors and their ground truth:

```c
// Generate query set with ground truth
size_t num_queries = 100000;
size_t k = 100;  // Top-100 nearest neighbors

// Allocate query vectors and ground truth
float* query_vectors = aligned_alloc(32, num_queries * config.dimensions * sizeof(float));
uint64_t* ground_truth = zmalloc(num_queries * k * sizeof(uint64_t));
float* gt_distances = zmalloc(num_queries * k * sizeof(float));

// Generate queries from different distribution (offset keys)
uint64_t query_offset = 20000000;  // Keys outside main dataset range
for (size_t i = 0; i < num_queries; i++) {
    generator_get_vector(gen, query_offset + i, &query_vectors[i * config.dimensions]);
}

// Compute ground truth (can be parallelized)
compute_ground_truth(gen, query_vectors, num_queries,
                     0, 10000000,  // Dataset range
                     k, ground_truth, gt_distances);
```

## Integration with Benchmark Frameworks

### Framework Architecture

```c
typedef struct benchmark_context {
    generator_t* generator;
    vector_database_t* db;

    // Workload configuration
    size_t num_operations;
    float insert_ratio;    // e.g., 0.1 = 10% inserts
    float query_ratio;     // e.g., 0.8 = 80% queries
    float delete_ratio;    // e.g., 0.05 = 5% deletes
    float update_ratio;    // e.g., 0.05 = 5% updates

    // Current state
    uint64_t next_key;
    uint64_t* active_keys;
    size_t num_active;

    // Metrics
    double* latencies;
    size_t* recall_scores;
} benchmark_context_t;
```

### Workload Executor

```c
void execute_workload(benchmark_context_t* ctx) {
    for (size_t op = 0; op < ctx->num_operations; op++) {
        double rand_val = (double)rand() / RAND_MAX;

        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

        if (rand_val < ctx->insert_ratio) {
            // INSERT operation
            float* vector = aligned_alloc(32, ctx->generator->config.dimensions * sizeof(float));
            generator_get_vector(ctx->generator, ctx->next_key, vector);

            vector_db_insert(ctx->db, ctx->next_key, vector);

            // Track active key
            ctx->active_keys[ctx->num_active++] = ctx->next_key;
            ctx->next_key++;
            zfree(vector);

        } else if (rand_val < ctx->insert_ratio + ctx->query_ratio) {
            // QUERY operation with recall measurement
            uint64_t query_key = 20000000 + (rand() % 100000);  // From query set
            float* query_vector = aligned_alloc(32, ctx->generator->config.dimensions * sizeof(float));
            generator_get_vector(ctx->generator, query_key, query_vector);

            // Execute query
            size_t k = 100;
            uint64_t* results = zmalloc(k * sizeof(uint64_t));
            float* distances = zmalloc(k * sizeof(float));

            vector_db_query(ctx->db, query_vector, k, results, distances);

            // Compute recall if ground truth available
            if (ctx->recall_scores) {
                uint64_t* ground_truth = get_ground_truth(query_key);
                float recall = compute_recall(results, ground_truth, k);
                ctx->recall_scores[op] = (size_t)(recall * 100);
            }

            zfree(query_vector);
            zfree(results);
            zfree(distances);

        } else if (rand_val < ctx->insert_ratio + ctx->query_ratio + ctx->delete_ratio) {
            // DELETE operation
            if (ctx->num_active > 0) {
                size_t idx = rand() % ctx->num_active;
                uint64_t key_to_delete = ctx->active_keys[idx];

                vector_db_delete(ctx->db, key_to_delete);
                generator_mark_deleted(ctx->generator, key_to_delete);

                // Remove from active keys
                ctx->active_keys[idx] = ctx->active_keys[--ctx->num_active];
            }

        } else {
            // UPDATE operation
            if (ctx->num_active > 0) {
                size_t idx = rand() % ctx->num_active;
                uint64_t key_to_update = ctx->active_keys[idx];

                // Generate slightly modified vector
                float* vector = aligned_alloc(32, ctx->generator->config.dimensions * sizeof(float));
                generator_get_vector(ctx->generator, key_to_update, vector);

                // Add small perturbation
                for (size_t i = 0; i < ctx->generator->config.dimensions; i++) {
                    vector[i] += ((float)rand() / RAND_MAX - 0.5f) * 0.1f;
                }

                vector_db_update(ctx->db, key_to_update, vector);
                zfree(vector);
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &end);
        ctx->latencies[op] = (end.tv_sec - start.tv_sec) * 1e6 +
                             (end.tv_nsec - start.tv_nsec) / 1e3;  // microseconds
    }
}
```

## Workload Patterns

### 1. Bulk Loading Phase
```c
void benchmark_bulk_loading(generator_t* gen, vector_database_t* db, size_t num_vectors) {
    size_t batch_size = 10000;
    float* batch = aligned_alloc(32, batch_size * gen->config.dimensions * sizeof(float));

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (size_t i = 0; i < num_vectors; i += batch_size) {
        size_t current_batch = MIN(batch_size, num_vectors - i);

        // Generate batch in parallel
        #pragma omp parallel for
        for (size_t j = 0; j < current_batch; j++) {
            generator_get_vector(gen, i + j, &batch[j * gen->config.dimensions]);
        }

        // Bulk insert
        vector_db_bulk_insert(db, i, batch, current_batch, gen->config.dimensions);

        if (i % 100000 == 0) {
            printf("Loaded %zu vectors...\n", i);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    printf("Bulk loading: %zu vectors in %.2f seconds (%.0f vectors/sec)\n",
           num_vectors, elapsed, num_vectors / elapsed);

    zfree(batch);
}
```

### 2. Mixed Read-Write Workload
```c
void benchmark_mixed_workload(generator_t* gen, vector_database_t* db) {
    // 70% reads, 20% inserts, 5% updates, 5% deletes
    benchmark_context_t ctx = {
        .generator = gen,
        .db = db,
        .num_operations = 1000000,
        .insert_ratio = 0.20,
        .query_ratio = 0.70,
        .delete_ratio = 0.05,
        .update_ratio = 0.05,
        .next_key = 10000000,  // Start after initial dataset
        .active_keys = zmalloc(1000000 * sizeof(uint64_t)),
        .num_active = 0,
        .latencies = zmalloc(1000000 * sizeof(double))
    };

    execute_workload(&ctx);

    // Compute percentiles
    qsort(ctx.latencies, ctx.num_operations, sizeof(double), compare_double);
    printf("Latency P50: %.2f us\n", ctx.latencies[ctx.num_operations * 50 / 100]);
    printf("Latency P95: %.2f us\n", ctx.latencies[ctx.num_operations * 95 / 100]);
    printf("Latency P99: %.2f us\n", ctx.latencies[ctx.num_operations * 99 / 100]);
}
```

### 3. Deletion-Heavy Workload
```c
void benchmark_deletion_workload(generator_t* gen, vector_database_t* db) {
    // Insert 1M vectors, then delete 50% while querying
    size_t initial_size = 1000000;
    size_t deletions = 500000;

    // Initial population
    for (size_t i = 0; i < initial_size; i++) {
        float* vector = aligned_alloc(32, gen->config.dimensions * sizeof(float));
        generator_get_vector(gen, i, vector);
        vector_db_insert(db, i, vector);
        zfree(vector);
    }

    // Track which keys are deleted
    bool* deleted = zcalloc(initial_size, sizeof(bool));
    size_t num_deleted = 0;

    // Delete random vectors while measuring query performance
    while (num_deleted < deletions) {
        uint64_t key = rand() % initial_size;

        if (!deleted[key]) {
            vector_db_delete(db, key);
            generator_mark_deleted(gen, key);
            deleted[key] = true;
            num_deleted++;

            // Periodic query to measure impact
            if (num_deleted % 10000 == 0) {
                measure_query_performance_after_deletions(gen, db, num_deleted);
            }
        }
    }

    zfree(deleted);
}
```

## Recall Testing

### Computing Ground Truth
```c
typedef struct ground_truth_context {
    generator_t* generator;
    float* query_vectors;
    size_t num_queries;
    size_t dimensions;
    uint64_t dataset_start;
    uint64_t dataset_end;
    size_t k;
    uint64_t* ground_truth;  // [num_queries][k]
    float* distances;         // [num_queries][k]
} ground_truth_context_t;

void compute_ground_truth_parallel(ground_truth_context_t* ctx) {
    #pragma omp parallel for schedule(dynamic)
    for (size_t q = 0; q < ctx->num_queries; q++) {
        float* query = &ctx->query_vectors[q * ctx->dimensions];

        // Priority queue for top-k
        typedef struct {
            uint64_t key;
            float distance;
        } neighbor_t;

        neighbor_t* heap = zmalloc(ctx->k * sizeof(neighbor_t));
        size_t heap_size = 0;

        // Scan dataset
        for (uint64_t key = ctx->dataset_start; key < ctx->dataset_end; key++) {
            if (generator_is_deleted(ctx->generator, key)) continue;

            float* vector = aligned_alloc(32, ctx->dimensions * sizeof(float));
            generator_get_vector(ctx->generator, key, vector);

            float distance = compute_distance_simd(query, vector, ctx->dimensions);

            if (heap_size < ctx->k) {
                heap[heap_size++] = (neighbor_t){key, distance};
                if (heap_size == ctx->k) {
                    make_max_heap(heap, ctx->k);
                }
            } else if (distance < heap[0].distance) {
                heap[0] = (neighbor_t){key, distance};
                heapify_down(heap, ctx->k, 0);
            }

            zfree(vector);
        }

        // Extract results
        sort_neighbors(heap, heap_size);
        for (size_t i = 0; i < heap_size; i++) {
            ctx->ground_truth[q * ctx->k + i] = heap[i].key;
            ctx->distances[q * ctx->k + i] = heap[i].distance;
        }

        zfree(heap);

        if (q % 100 == 0) {
            printf("Computed ground truth for %zu/%zu queries\n", q, ctx->num_queries);
        }
    }
}
```

### Measuring Recall
```c
float measure_recall_at_k(uint64_t* results, uint64_t* ground_truth, size_t k) {
    size_t matches = 0;

    // Create set of ground truth IDs for O(1) lookup
    bool* gt_set = zcalloc(UINT32_MAX, sizeof(bool));  // Or use hash table
    for (size_t i = 0; i < k; i++) {
        if (ground_truth[i] != UINT64_MAX) {
            gt_set[ground_truth[i]] = true;
        }
    }

    // Count matches
    for (size_t i = 0; i < k; i++) {
        if (results[i] != UINT64_MAX && gt_set[results[i]]) {
            matches++;
        }
    }

    zfree(gt_set);
    return (float)matches / k;
}

void benchmark_recall_vs_performance(generator_t* gen, vector_database_t* db) {
    // Test different index configurations
    float ef_values[] = {16, 32, 64, 128, 256, 512};
    size_t num_configs = sizeof(ef_values) / sizeof(ef_values[0]);

    for (size_t c = 0; c < num_configs; c++) {
        // Configure index
        vector_db_set_search_ef(db, ef_values[c]);

        // Run queries
        double total_recall = 0;
        double total_latency = 0;
        size_t num_queries = 1000;

        for (size_t q = 0; q < num_queries; q++) {
            float* query = get_query_vector(gen, q);
            uint64_t* results = zmalloc(100 * sizeof(uint64_t));

            struct timespec start, end;
            clock_gettime(CLOCK_MONOTONIC, &start);

            vector_db_query(db, query, 100, results, NULL);

            clock_gettime(CLOCK_MONOTONIC, &end);
            double latency = (end.tv_sec - start.tv_sec) * 1e6 +
                           (end.tv_nsec - start.tv_nsec) / 1e3;

            uint64_t* gt = get_ground_truth(q);
            float recall = measure_recall_at_k(results, gt, 100);

            total_recall += recall;
            total_latency += latency;

            zfree(results);
        }

        printf("ef=%g: Recall@100=%.3f, Latency=%.2f us\n",
               ef_values[c],
               total_recall / num_queries,
               total_latency / num_queries);
    }
}
```

## Performance Optimization

### 1. Memory Management
```c
// Pre-allocate buffers for better performance
typedef struct buffer_pool {
    float** buffers;
    size_t* buffer_sizes;
    size_t num_buffers;
    pthread_mutex_t* locks;
} buffer_pool_t;

buffer_pool_t* create_buffer_pool(size_t num_threads, size_t dimensions) {
    buffer_pool_t* pool = zmalloc(sizeof(buffer_pool_t));
    pool->num_buffers = num_threads * 2;  // Double buffering
    pool->buffers = zmalloc(pool->num_buffers * sizeof(float*));
    pool->locks = zmalloc(pool->num_buffers * sizeof(pthread_mutex_t));

    for (size_t i = 0; i < pool->num_buffers; i++) {
        pool->buffers[i] = aligned_alloc(32, dimensions * sizeof(float));
        pthread_mutex_init(&pool->locks[i], NULL);
    }

    return pool;
}
```

### 2. Parallel Workload Generation
```c
void parallel_workload_executor(benchmark_context_t* ctx, size_t num_threads) {
    pthread_t* threads = zmalloc(num_threads * sizeof(pthread_t));

    typedef struct thread_context {
        benchmark_context_t* parent;
        size_t thread_id;
        size_t ops_per_thread;
        double* local_latencies;
    } thread_context_t;

    thread_context_t* thread_contexts = zmalloc(num_threads * sizeof(thread_context_t));

    for (size_t t = 0; t < num_threads; t++) {
        thread_contexts[t] = (thread_context_t){
            .parent = ctx,
            .thread_id = t,
            .ops_per_thread = ctx->num_operations / num_threads,
            .local_latencies = zmalloc(ctx->num_operations / num_threads * sizeof(double))
        };

        pthread_create(&threads[t], NULL, workload_thread, &thread_contexts[t]);
    }

    // Wait for completion
    for (size_t t = 0; t < num_threads; t++) {
        pthread_join(threads[t], NULL);
    }

    // Aggregate results
    aggregate_thread_metrics(thread_contexts, num_threads);
}
```

### 3. Batch Processing
```c
void optimized_batch_query(generator_t* gen, vector_database_t* db,
                          uint64_t* query_keys, size_t num_queries) {
    size_t batch_size = 100;
    size_t dimensions = gen->config.dimensions;

    // Pre-allocate batch buffers
    float* batch_queries = aligned_alloc(32, batch_size * dimensions * sizeof(float));
    uint64_t* batch_results = zmalloc(batch_size * 100 * sizeof(uint64_t));

    for (size_t i = 0; i < num_queries; i += batch_size) {
        size_t current_batch = MIN(batch_size, num_queries - i);

        // Generate query batch
        #pragma omp parallel for simd
        for (size_t j = 0; j < current_batch; j++) {
            generator_get_vector(gen, query_keys[i + j],
                               &batch_queries[j * dimensions]);
        }

        // Batch query to database
        vector_db_batch_query(db, batch_queries, current_batch, 100, batch_results);

        // Process results
        process_batch_results(batch_results, current_batch);
    }

    zfree(batch_queries);
    zfree(batch_results);
}
```

## Example Complete Benchmark

```c
int main(int argc, char* argv[]) {
    // 1. Configure generator
    generator_config_t config = {
        .dimensions = 768,
        .initial_capacity = 10000000,
        .sparsity = 0.1,
        .radius = 5.0,
        .num_centroids = 100,
        .seed = 42
    };

    generator_t* gen = generator_create(&config);

    // 2. Initialize vector database
    vector_database_t* db = vector_db_create(config.dimensions);

    // 3. Phase 1: Bulk loading
    printf("Phase 1: Bulk Loading\n");
    benchmark_bulk_loading(gen, db, 10000000);

    // 4. Phase 2: Compute ground truth
    printf("Phase 2: Computing Ground Truth\n");
    compute_and_save_ground_truth(gen, 10000, 100);

    // 5. Phase 3: Mixed workload
    printf("Phase 3: Mixed Workload\n");
    benchmark_mixed_workload(gen, db);

    // 6. Phase 4: Recall testing
    printf("Phase 4: Recall vs Performance\n");
    benchmark_recall_vs_performance(gen, db);

    // 7. Phase 5: Deletion impact
    printf("Phase 5: Deletion Impact\n");
    benchmark_deletion_workload(gen, db);

    // 8. Generate report
    generate_benchmark_report("benchmark_results.json");

    // Cleanup
    generator_destroy(gen);
    vector_db_destroy(db);

    return 0;
}
```

## Best Practices

1. **Warm-up Phase**: Always include a warm-up phase before measuring performance
2. **Statistical Significance**: Run multiple iterations and report percentiles
3. **Monitor System Resources**: Track CPU, memory, and I/O during benchmarks
4. **Vary Parameters**: Test with different vector dimensions, dataset sizes, and sparsity levels
5. **Save Intermediate Results**: Checkpoint long-running benchmarks
6. **Version Control**: Track generator configurations with benchmark results

## Troubleshooting

### Common Issues

1. **Inconsistent Results**: Ensure using same seed and configuration
2. **Memory Issues**: Use batch processing for large datasets
3. **Poor Recall**: Verify ground truth computation and distance metrics
4. **Slow Generation**: Enable SIMD optimizations and parallel processing

### Debug Mode

```c
// Enable debug output
export VECTOR_GEN_DEBUG=1

// Verify vector properties
void verify_dataset_properties(generator_t* gen, size_t sample_size) {
    double avg_magnitude = 0;
    double avg_sparsity = 0;

    for (size_t i = 0; i < sample_size; i++) {
        float* vector = aligned_alloc(32, gen->config.dimensions * sizeof(float));
        generator_get_vector(gen, i, vector);

        // Check magnitude
        float magnitude = compute_magnitude(vector, gen->config.dimensions);
        avg_magnitude += magnitude;

        // Check sparsity
        size_t zeros = count_zeros(vector, gen->config.dimensions);
        avg_sparsity += (double)zeros / gen->config.dimensions;

        zfree(vector);
    }

    printf("Average magnitude: %.3f\n", avg_magnitude / sample_size);
    printf("Average sparsity: %.3f\n", avg_sparsity / sample_size);
}
```