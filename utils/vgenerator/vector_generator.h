/**
 * vector_generator.h - Complete header with all required functions
 */

#ifndef VECTOR_GENERATOR_H
#define VECTOR_GENERATOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <pthread.h>

/* Constants */
#define MAX_NUM_THREADS 64
#define MAX_QUERY_VECTORS 20000
#define RESERVED_KEY_RANGE 1000000
#define NEIGHBORS_PER_QUERY 10

/* Type definitions */
typedef uint64_t vector_key_t;

typedef struct {
    vector_key_t key;
    float* data;
} vector_t;

typedef struct {
    vector_key_t key;
    float distance;
} ground_truth_entry_t;

typedef struct {
    vector_t vector;
    ground_truth_entry_t ground_truth[NEIGHBORS_PER_QUERY]; /* Store top-100 neighbors */
} query_vector_t;

typedef enum {
    GEN_ORDER_RANDOM,
    GEN_ORDER_CENTROID
} generation_order_t;



typedef struct {
    uint32_t dimensions;
    uint64_t initial_capacity;
    float sparsity;
    float radius;
    uint32_t num_centroids;
    uint64_t seed;
} generator_config_t;

typedef struct {
    float* coordinates;
    uint64_t start_key;
    uint64_t vector_count;
} centroid_t;

typedef struct {
    uint8_t* bits;
    size_t size_bytes;
    uint32_t num_hashes;
} bloom_filter_t;

typedef struct {
    atomic_uint_fast64_t next_key;
    atomic_uint_fast64_t deleted_count;
    atomic_uint_fast64_t active_count;
} key_allocator_t;

/* Pre-computed query ground truth entry */
typedef struct {
    vector_key_t query_key;
    float* query_vector;
    vector_key_t neighbor_keys[NEIGHBORS_PER_QUERY];
    float neighbor_distances[NEIGHBORS_PER_QUERY];
    bool computed;
    pthread_mutex_t compute_mutex;
} query_ground_truth_t;

/* Main generator structure */
typedef struct vector_generator {
    generator_config_t config;
    uint32_t dimensions;
    uint64_t pinned_threshold;
    
    centroid_t* centroids;
    uint32_t num_centroids;
    
    key_allocator_t allocator;
    bloom_filter_t* deleted_keys;
    pthread_rwlock_t delete_lock;
    
    /* Reserved key management */
    uint64_t query_key_start;       /* Start of reserved range (1) */
    uint64_t query_key_end;         /* End of reserved range (1M) */
    uint64_t general_key_start;     /* Start of general keys (1M+1) */
    
    /* Pre-computed ground truth */
    query_ground_truth_t* query_ground_truth;
    uint32_t num_query_vectors;
    atomic_uint_fast32_t query_index;
    uint64_t ground_truth_dataset_size;  /* Actual number of vectors ingested for ground truth */
} vector_generator_t;

typedef enum {
    ITER_TYPE_INGESTION = 0,
    ITER_TYPE_QUERY = 1,
    ITER_TYPE_DELETION = 2
} iterator_type_t;

#define MAX_DATASET_VECTORS 10000000

/**
 * vector_iterator_t - Iterator for generating or accessing vectors
 * 
 * An iterator provides sequential access to vectors from the generator.
 * Different iterator types serve different purposes:
 * 
 * Iterator Types:
 *   - ITER_TYPE_INGESTION: Generates new vectors for database insertion
 *     Uses: vg_get_ingestion_iterator() / vg_iterator_next()
 *     Key Range: General range (RESERVED_KEY_RANGE + 1 onwards)
 * 
 *   - ITER_TYPE_QUERY: Provides query vectors with pre-computed ground truth
 *     Uses: vg_get_query_iterator() / vg_iterator_next_query()
 *     Key Range: Reserved range (1 to RESERVED_KEY_RANGE)
 * 
 *   - ITER_TYPE_DELETION: Selects vectors for deletion
 *     Uses: vg_get_deletion_iterator() / vg_iterator_next_key()
 *     Key Range: Previously generated keys from general range
 * 
 * Fields:
 *   @generator: Pointer to parent vector_generator_t instance
 *               Used to access configuration, centroids, and generation functions
 *               Must remain valid for iterator lifetime
 * 
 *   @type: Iterator type (INGESTION, QUERY, or DELETION)
 *          Determines which vg_iterator_next_*() function to use
 * 
 *   @order: Generation order for ingestion iterators
 *           - GEN_ORDER_RANDOM: Random distribution across centroids
 *           - GEN_ORDER_CENTROID: Sequential by centroid
 *           Ignored for query and deletion iterators
 * 
 *   @count: Total number of vectors this iterator will produce
 *           Set during iterator creation, remains constant
 * 
 *   @current: Current position in iteration (0 to count-1)
 *             Incremented with each vg_iterator_next*() call
 *             Iteration complete when current >= count
 * 
 *   @key_sequence: Pre-allocated array of vector keys to iterate over
 *                  Size: count * sizeof(uint64_t)
 *                  For INGESTION: Atomically allocated keys from general range
 *                  For QUERY: Sequential keys from reserved range [1, count]
 *                  For DELETION: Randomly selected previously-generated keys
 * 
 *   @current_centroid: For GEN_ORDER_CENTROID ingestion only
 *                      Tracks which centroid is currently being generated
 *                      Range: [0, num_centroids-1]
 * 
 *   @vectors_in_centroid: For GEN_ORDER_CENTROID ingestion only
 *                         Number of vectors generated for current centroid
 *                         Used to know when to advance to next centroid
 * 
 *   @vector_buffer: Reusable buffer for vector data generation
 *                   Size: dimensions * sizeof(float)
 *                   Populated by vg_generate_vector_from_key()
 *                   Pointer returned in vector_t.data points here
 *                   Valid until next vg_iterator_next*() call
 * 
 * Memory Management:
 *   - Created by: vg_get_*_iterator() functions
 *   - Destroyed by: vg_iterator_destroy()
 *   - Never manually allocate or free this structure
 * 
 * Thread Safety:
 *   - Each iterator is NOT thread-safe
 *   - Each thread must create its own iterator
 *   - Multiple iterators from same generator ARE thread-safe
 *   - Do not share a single iterator across threads
 * 
 * Example Usage:
 *   // Ingestion iterator
 *   vector_iterator_t* iter = vg_get_ingestion_iterator(gen, 1000, GEN_ORDER_RANDOM);
 *   vector_t vec;
 *   while (vg_iterator_next(iter, &vec)) {
 *       // vec.key is unique key
 *       // vec.data points to iter->vector_buffer (valid until next call)
 *   }
 *   vg_iterator_destroy(iter);
 * 
 *   // Query iterator
 *   vector_iterator_t* qiter = vg_get_query_iterator(gen, 100);
 *   query_vector_t query;
 *   while (vg_iterator_next_query(qiter, &query)) {
 *       // query.vector.data is query vector
 *       // query.ground_truth[0..9] contains 10 nearest neighbors
 *   }
 *   vg_iterator_destroy(qiter);
 */
typedef struct vector_iterator {
    vector_generator_t* generator;
    iterator_type_t type;
    generation_order_t order;
    
    uint64_t count;
    uint64_t current;
    uint64_t* key_sequence;
    
    uint32_t current_centroid;
    uint64_t vectors_in_centroid;
    
    float* vector_buffer;
} vector_iterator_t;

typedef struct {
    uint64_t total_generated;
    uint64_t total_deleted;
    uint64_t active_vectors;
    uint64_t pinned_vectors;
} generator_stats_t;

/* ============================================================================
 * Public API - Vector Generator Functions
 * ============================================================================ */

/**
 * vg_init - Initialize a new vector generator instance
 * 
 * Creates and initializes a vector generator with the specified configuration.
 * This function allocates centroids, initializes the key allocator, sets up
 * the bloom filter for deleted keys, and pre-computes ground truth for query
 * vectors in the reserved key range.
 * 
 * Key Range Allocation:
 *   - Reserved range: [1, RESERVED_KEY_RANGE] (default: 1 to 1,000,000)
 *     Used for query vectors with pre-computed ground truth
 *   - General range: [RESERVED_KEY_RANGE + 1, ∞)
 *     Used for regular ingestion vectors
 * 
 * @param config Configuration parameters including:
 *               - dimensions: Vector dimensionality
 *               - initial_capacity: Expected number of vectors
 *               - sparsity: Sparsity factor (0.0 to 1.0)
 *               - radius: Centroid radius for clustering
 *               - num_centroids: Number of cluster centroids
 *               - seed: Random seed for reproducibility
 * 
 * @return Pointer to initialized vector_generator_t, or NULL on failure
 * 
 * Thread Safety: This function is NOT thread-safe. Call once during initialization.
 * 
 * Memory: Caller must call vg_destroy() to free all allocated resources.
 */
vector_generator_t* vg_init(const generator_config_t* config);

/**
 * vg_destroy - Destroy vector generator and free all resources
 * 
 * Frees all memory allocated by the vector generator including centroids,
 * bloom filter, ground truth cache, and the generator structure itself.
 * 
 * @param gen Vector generator instance to destroy
 * 
 * Thread Safety: NOT thread-safe. Ensure all iterators are destroyed and
 *                no other threads are accessing the generator before calling.
 */
void vg_destroy(vector_generator_t* gen);

/**
 * vg_set_ground_truth_dataset_size - Set the actual number of vectors in ground truth dataset
 * 
 * This function must be called BEFORE any queries are executed to ensure ground truth
 * is computed against the correct dataset size. The ground truth computation will
 * search only the specified number of vectors (starting from key 0) to find nearest
 * neighbors.
 * 
 * @param gen Vector generator instance
 * @param dataset_size Number of vectors actually ingested for ground truth
 *                     Should match the -n parameter used with vec-ground-truth
 * 
 * Example:
 *   // If you ingest 50K vectors for ground truth:
 *   vg_set_ground_truth_dataset_size(gen, 50000);
 * 
 * Thread Safety: Should be called before any concurrent operations
 */
void vg_set_ground_truth_dataset_size(vector_generator_t* gen, uint64_t dataset_size);

/**
 * vg_precompute_all_ground_truths - Eagerly compute all query ground truths
 * 
 * This function implements the "warm-up phase" pattern recommended in the USER_GUIDE.md.
 * It precomputes ground truth (nearest neighbors) for all query vectors before benchmarking
 * begins. This eliminates the lazy computation overhead during query execution, providing
 * more accurate and consistent performance measurements.
 * 
 * When to use:
 *   - Call AFTER vg_set_ground_truth_dataset_size() has been called
 *   - Call AFTER ground truth vectors have been ingested into the database
 *   - Call BEFORE starting query performance measurements
 * 
 * Behavior:
 *   - Computes ground truth for all MAX_QUERY_VECTORS queries
 *   - Performs brute-force search across ground_truth_dataset_size vectors
 *   - Displays progress updates every 100 queries
 *   - Skips queries that already have computed ground truth
 *   - Reports total time and throughput at completion
 * 
 * Performance:
 *   - Computational cost: O(num_queries × dataset_size × dimensions)
 *   - For 1000 queries × 50K dataset × 8 dims ≈ 10-30 seconds
 *   - This is a one-time cost that eliminates per-query overhead
 * 
 * @param gen Vector generator instance (must have ground_truth_dataset_size set)
 * 
 * Example usage:
 *   // 1. Initialize generator
 *   generator_config_t config = {.dimensions = 8, ...};
 *   vector_generator_t* gen = vg_init(&config);
 *   
 *   // 2. Set dataset size (must match ingested vectors)
 *   vg_set_ground_truth_dataset_size(gen, 50000);
 *   
 *   // 3. Ingest ground truth vectors
 *   // ... insert 50K vectors into database ...
 *   
 *   // 4. Precompute all ground truths (warm-up phase)
 *   vg_precompute_all_ground_truths(gen);
 *   
 *   // 5. Now run queries - no ground truth computation overhead!
 *   // ... benchmark query performance ...
 * 
 * Thread Safety: Should be called from a single thread before concurrent query operations.
 *                The underlying compute_query_ground_truth() uses mutexes for safety.
 */
void vg_precompute_all_ground_truths(vector_generator_t* gen);

/**
 * vg_get_ingestion_iterator - Create iterator for vector ingestion
 * 
 * Creates an iterator for generating vectors to insert into the database.
 * Vectors are generated from the general key range (RESERVED_KEY_RANGE + 1 onwards).
 * Keys are allocated atomically to ensure uniqueness across threads.
 * 
 * Generation Orders:
 *   - GEN_ORDER_RANDOM: Vectors distributed randomly across all centroids
 *   - GEN_ORDER_CENTROID: Vectors generated sequentially by centroid
 *                        (all vectors for centroid 0, then centroid 1, etc.)
 * 
 * @param gen Vector generator instance
 * @param count Number of vectors to generate
 * @param order Generation order (random or centroid-based)
 * 
 * @return Pointer to iterator, or NULL on failure
 * 
 * Thread Safety: Thread-safe. Multiple threads can create iterators concurrently.
 *                Keys are allocated using atomic operations.
 * 
 * Usage:
 *   iterator = vg_get_ingestion_iterator(gen, 10000, GEN_ORDER_RANDOM);
 *   while (vg_iterator_next(iterator, &vec)) {
 *       // Insert vec.key and vec.data into database
 *   }
 *   vg_iterator_destroy(iterator);
 */
vector_iterator_t* vg_get_ingestion_iterator(vector_generator_t* gen, uint64_t count, generation_order_t order);

/**
 * vg_get_query_iterator - Create iterator for query vectors with ground truth
 * 
 * Creates an iterator for generating query vectors from the reserved key range.
 * Each query vector includes pre-computed ground truth (nearest neighbors) for
 * accuracy validation. Ground truth is computed lazily on first access per query.
 * 
 * Reserved Range: Keys from [1, RESERVED_KEY_RANGE] are used for queries
 * Ground Truth: Each query includes 10 nearest neighbors with exact distances
 * 
 * @param gen Vector generator instance
 * @param count Number of query vectors to generate (max: MAX_QUERY_VECTORS)
 * 
 * @return Pointer to iterator, or NULL on failure
 * 
 * Thread Safety: Thread-safe. Ground truth computation is protected by per-query mutex.
 *                Multiple threads can query concurrently; ground truth computed once.
 * 
 * Usage:
 *   iterator = vg_get_query_iterator(gen, 100);
 *   while (vg_iterator_next_query(iterator, &query)) {
 *       // Execute search with query.vector.data
 *       // Compare results against query.ground_truth
 *   }
 *   vg_iterator_destroy(iterator);
 */
vector_iterator_t* vg_get_query_iterator(vector_generator_t* gen, uint64_t count);

/**
 * vg_get_deletion_iterator - Create iterator for vector deletion
 * 
 * Creates an iterator for selecting vectors to delete. Returns keys of
 * previously generated vectors from the general key range. Deleted keys
 * are tracked in a bloom filter to avoid re-use.
 * 
 * @param gen Vector generator instance
 * @param count Number of vectors to delete
 * 
 * @return Pointer to iterator, or NULL on failure
 * 
 * Thread Safety: Thread-safe. Bloom filter updates are protected by rwlock.
 * 
 * Usage:
 *   iterator = vg_get_deletion_iterator(gen, 1000);
 *   while (vg_iterator_next_key(iterator, &key)) {
 *       // Delete vector with this key from database
 *   }
 *   vg_iterator_destroy(iterator);
 */
vector_iterator_t* vg_get_deletion_iterator(vector_generator_t* gen, uint64_t count);

/**
 * vg_iterator_next - Get next vector from ingestion/deletion iterator
 * 
 * Retrieves the next vector from an ingestion iterator. The vector data is
 * generated deterministically from the key using the generator's centroid
 * configuration and PRNG.
 * 
 * @param iter Iterator created by vg_get_ingestion_iterator()
 * @param vec Output parameter to receive vector key and data
 *            vec->key: Unique vector key
 *            vec->data: Pointer to vector data (dimensions * sizeof(float))
 *                      Valid until next call to vg_iterator_next() or vg_iterator_destroy()
 * 
 * @return true if vector retrieved, false if iteration complete
 * 
 * Thread Safety: NOT thread-safe per iterator. Each thread needs its own iterator.
 * 
 * Note: Do NOT call this function on query or deletion iterators.
 *       Use vg_iterator_next_query() or vg_iterator_next_key() instead.
 */
bool vg_iterator_next(vector_iterator_t* iter, vector_t* vec);

/**
 * vg_iterator_next_query - Get next query vector with ground truth
 * 
 * Retrieves the next query vector from a query iterator. Each query includes
 * the vector data and pre-computed ground truth (10 nearest neighbors with
 * exact L2 distances).
 * 
 * @param iter Iterator created by vg_get_query_iterator()
 * @param query Output parameter to receive query vector and ground truth
 *              query->vector.key: Query vector key
 *              query->vector.data: Query vector data
 *              query->ground_truth[]: Array of 10 nearest neighbors
 *                                    Each entry contains neighbor key and distance
 * 
 * @return true if query retrieved, false if iteration complete
 * 
 * Thread Safety: Thread-safe. Ground truth is computed once per query with mutex protection.
 * 
 * Note: Only call this function on query iterators created by vg_get_query_iterator().
 */
bool vg_iterator_next_query(vector_iterator_t* iter, query_vector_t* query);

/**
 * vg_iterator_next_key - Get next key from deletion iterator
 * 
 * Retrieves the next key to delete from a deletion iterator.
 * 
 * @param iter Iterator created by vg_get_deletion_iterator()
 * @param key Output parameter to receive the key to delete
 * 
 * @return true if key retrieved, false if iteration complete
 * 
 * Thread Safety: NOT thread-safe per iterator. Each thread needs its own iterator.
 * 
 * Note: Only call this function on deletion iterators.
 */
bool vg_iterator_next_key(vector_iterator_t* iter, vector_key_t* key);

/**
 * vg_iterator_destroy - Destroy iterator and free resources
 * 
 * Frees all memory associated with an iterator including key sequence
 * and vector buffer.
 * 
 * @param iter Iterator to destroy
 * 
 * Thread Safety: NOT thread-safe. Do not destroy an iterator while another
 *                thread is using it.
 */
void vg_iterator_destroy(vector_iterator_t* iter);

/**
 * vg_get_stats - Get vector generator statistics
 * 
 * Retrieves current statistics about the vector generator including
 * generation counts and active vector tracking.
 * 
 * @param gen Vector generator instance
 * @param stats Output parameter to receive statistics:
 *              - total_generated: Total vectors generated (atomic counter)
 *              - total_deleted: Total vectors marked as deleted
 *              - active_vectors: Currently active vectors (generated - deleted)
 *              - pinned_vectors: Vectors in reserved range (query vectors)
 * 
 * Thread Safety: Thread-safe. Reads atomic counters.
 */
void vg_get_stats(const vector_generator_t* gen, generator_stats_t* stats);

/**
 * vg_generate_vector_from_key - Generate vector data from key
 * 
 * Deterministically generates vector data from a given key. The same key
 * always produces the same vector. This allows regenerating vectors on-demand
 * without storing all vectors in memory.
 * 
 * Algorithm:
 *   1. Hash key to select centroid
 *   2. Seed PRNG with key
 *   3. Generate sparse offset from centroid
 *   4. Apply sparsity mask
 * 
 * @param gen Vector generator instance
 * @param key Vector key (must be valid - either from reserved or general range)
 * @param output Output buffer to receive vector data
 *               Must be pre-allocated with size: dimensions * sizeof(float)
 * 
 * Thread Safety: Thread-safe (read-only access to generator config and centroids)
 * 
 * Usage:
 *   float vector[128];
 *   vg_generate_vector_from_key(gen, 12345, vector);
 */
void vg_generate_vector_from_key(const vector_generator_t* gen, vector_key_t key, float* output);

/**
 * vg_compute_l2_distance - Compute L2 (Euclidean) distance between vectors
 * 
 * Calculates the L2 distance: sqrt(sum((v1[i] - v2[i])^2))
 * Optimized with loop unrolling for better performance.
 * 
 * @param v1 First vector
 * @param v2 Second vector
 * @param dimensions Number of dimensions (must match for both vectors)
 * 
 * @return L2 distance as a float
 * 
 * Thread Safety: Thread-safe (no shared state)
 * 
 * Note: Used internally for ground truth computation and can be used
 *       by callers for validation.
 */
float vg_compute_l2_distance(const float* v1, const float* v2, uint32_t dimensions);

/* For testing ground truth accuracy */
void vg_find_ground_truth(const vector_generator_t* gen, const vector_t* query, 
                         ground_truth_entry_t* ground_truth, uint32_t k);

static inline uint32_t vg_get_dimensions(const vector_generator_t* gen) {
    return gen->dimensions;
}

/* Test helper functions - inline implementations */
static inline uint32_t vg_prng_next(uint64_t* state) {
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return (uint32_t)(*state >> 32);
}

static inline float vg_prng_to_float(uint32_t prng_val) {
    return ((float)prng_val / (float)UINT32_MAX) * 2.0f - 1.0f;
}

#endif /* VECTOR_GENERATOR_H */