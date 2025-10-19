/**
 * valkey-benchmark-vgen.c - Vector Generator Integration Implementation
 * 
 * This file implements the integration between valkey-benchmark and the vector
 * generator library. It provides thread-safe vector generation, placeholder
 * replacement, and recall tracking for vector search benchmarks.
 */

#include "valkey-benchmark-vgen.h"
#include "fmacros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>

#include "sds.h"
#include "zmalloc.h"
#include <valkey/valkey.h>

/* Include vector generator from utils/vgenerator */
#include "../../utils/vgenerator/vector_generator.h"

/* Global vector generator instance */
static vector_generator_t *vgen_instance = NULL;
static pthread_rwlock_t vgen_lock = PTHREAD_RWLOCK_INITIALIZER;

/* Configuration values stored at init time */
static int vgen_cluster_mode = 0;
static char vgen_prefix[256] = "";

/* Ground truth storage for recall calculation */
/* Note: ground_truth_entry_t is defined in vector_generator.h */
#define MAX_GROUND_TRUTH_ENTRIES (RESERVED_KEY_RANGE * NEIGHBORS_PER_QUERY)
typedef struct {
    vector_key_t query_key;
    ground_truth_entry_t neighbors[NEIGHBORS_PER_QUERY];  /* Store top-10 neighbors */
    int neighbor_count;
} stored_ground_truth_t;

typedef struct {
    stored_ground_truth_t *entries;
    uint64_t capacity;
    _Atomic uint64_t count;  /* Atomic counter for lock-free access */
} ground_truth_storage_t;

static ground_truth_storage_t ground_truth = {
    .entries = NULL,
    .capacity = 0,
    .count = 0
};

/* Recall tracking structure */
typedef struct {
    uint64_t total_queries;
    double total_recall;
    double min_recall;
    double max_recall;
    uint64_t current_query_index;  /* Index for matching replies to ground truth */
    pthread_mutex_t lock;
} recall_tracker_t;

static recall_tracker_t recall_tracker = {
    .total_queries = 0,
    .total_recall = 0.0,
    .min_recall = 1.0,
    .max_recall = 0.0,
    .current_query_index = 0,
    .lock = PTHREAD_MUTEX_INITIALIZER
};

/**
 * Iterator types for pool management
 */
typedef enum {
    ITER_QUERY,    /* Query iterator for reserved range (1-1M) */
    ITER_INSERT    /* Insert iterator for general range (1M+) */
} IteratorType;

/**
 * Vector Generator Iterator Pool
 * 
 * Manages a pool of iterators for multi-threaded vector generation.
 * Thread-safe allocation/deallocation using atomic operations.
 * 
 * Design:
 * - Query iterators: One per thread for query workloads
 * - Insert iterators: One per thread for ingestion workloads  
 * - Lock-free design using atomic flags for slot acquisition
 * 
 * Usage:
 *   VectorGeneratorIterator *iter = vgen_get_thread_iterator(thread_id, ITER_QUERY);
 *   // ... use iterator ...
 *   vgen_release_thread_iterator(thread_id, ITER_QUERY);
 */
typedef struct {
    vector_iterator_t **query_iterators;   /* Pool for query ops */
    vector_iterator_t **insert_iterators;  /* Pool for insert ops */
    int pool_size;                         /* = num_threads or 8 */
    _Atomic int *query_in_use;             /* Track allocation */
    _Atomic int *insert_in_use;            /* Track allocation */
} VgenIteratorPool;

static VgenIteratorPool *iterator_pool = NULL;

/**
 * Initialize iterator pool with specified number of threads.
 * 
 * Creates a pool of iterators for thread-safe vector generation.
 * Each slot in the pool can hold one query iterator and one insert iterator.
 * 
 * @param num_threads Number of threads that will use the pool
 * @return 1 on success, 0 on failure
 * 
 * Thread Safety: Must be called from a single thread (typically main thread).
 */
static int vgen_init_iterator_pool(int num_threads) {
    if (iterator_pool != NULL) {
        fprintf(stderr, "Warning: Iterator pool already initialized\n");
        return 0;
    }
    
    int pool_size = (num_threads > 0) ? num_threads : 8;
    
    /* Allocate pool structure */
    iterator_pool = zcalloc(sizeof(VgenIteratorPool));
    if (!iterator_pool) {
        fprintf(stderr, "Error: Failed to allocate iterator pool\n");
        return 0;
    }
    
    iterator_pool->pool_size = pool_size;
    
    /* Allocate query iterator pool */
    iterator_pool->query_iterators = zcalloc(pool_size * sizeof(vector_iterator_t*));
    iterator_pool->query_in_use = zcalloc(pool_size * sizeof(_Atomic int));
    
    if (!iterator_pool->query_iterators || !iterator_pool->query_in_use) {
        fprintf(stderr, "Error: Failed to allocate query iterator pool\n");
        if (iterator_pool->query_iterators) zfree(iterator_pool->query_iterators);
        if (iterator_pool->query_in_use) zfree(iterator_pool->query_in_use);
        zfree(iterator_pool);
        iterator_pool = NULL;
        return 0;
    }
    
    /* Allocate insert iterator pool */
    iterator_pool->insert_iterators = zcalloc(pool_size * sizeof(vector_iterator_t*));
    iterator_pool->insert_in_use = zcalloc(pool_size * sizeof(_Atomic int));
    
    if (!iterator_pool->insert_iterators || !iterator_pool->insert_in_use) {
        fprintf(stderr, "Error: Failed to allocate insert iterator pool\n");
        zfree(iterator_pool->query_iterators);
        zfree(iterator_pool->query_in_use);
        if (iterator_pool->insert_iterators) zfree(iterator_pool->insert_iterators);
        if (iterator_pool->insert_in_use) zfree(iterator_pool->insert_in_use);
        zfree(iterator_pool);
        iterator_pool = NULL;
        return 0;
    }
    
    /* Initialize atomic flags to 0 (not in use) */
    for (int i = 0; i < pool_size; i++) {
        atomic_init(&iterator_pool->query_in_use[i], 0);
        atomic_init(&iterator_pool->insert_in_use[i], 0);
        iterator_pool->query_iterators[i] = vg_get_query_iterator(vgen_instance, MAX_DATASET_VECTORS);
        iterator_pool->insert_iterators[i] = vg_get_ingestion_iterator(vgen_instance, MAX_DATASET_VECTORS, GEN_ORDER_RANDOM);
    }
    
    return 1;
}

/**
 * Get or create a thread-local iterator from the pool.
 * 
 * @param thread_id Thread identifier (-1 for main thread, 0+ for worker threads)
 * @param type Iterator type (ITER_QUERY or ITER_INSERT)
 * @return Iterator pointer or NULL on error
 * 
 * Thread Safety: Uses atomic operations for lock-free slot acquisition.
 * If the preferred slot is busy, searches for a free slot.
 */
static vector_iterator_t* vgen_get_thread_iterator(int thread_id, IteratorType type) {
    assert(iterator_pool && vgen_instance);
    assert(thread_id < iterator_pool->pool_size);
    /* Calculate preferred slot based on thread_id */
    int slot = thread_id;
    
    vector_iterator_t **pool = (type == ITER_QUERY) 
        ? iterator_pool->query_iterators 
        : iterator_pool->insert_iterators;
    _Atomic int *in_use = (type == ITER_QUERY)
        ? iterator_pool->query_in_use
        : iterator_pool->insert_in_use;
    /* Lazy allocation on first use */
    assert(pool[slot]);
    /* Try to acquire preferred slot atomically */
    int expected = 0;    
    if (!atomic_compare_exchange_strong(&in_use[slot], &expected, 1)) {
        fprintf(stderr, "Error: Failed to acquire iterator slot (thread_id=%d, slot=%d)\n", 
                thread_id, slot);
        fflush(stdout);
        fflush(stderr);
        assert(0);
    }
    /* Successfully acquired slot */       
    return pool[slot];
}

/**
 * Release a thread-local iterator back to the pool.
 * 
 * @param thread_id Thread identifier (-1 for main thread, 0+ for worker threads)
 * @param type Iterator type (ITER_QUERY or ITER_INSERT)
 * 
 * Thread Safety: Uses atomic operations for lock-free release.
 */
static void vgen_release_thread_iterator(int thread_id, IteratorType type) {
    assert(iterator_pool && vgen_instance);
    
    /* Calculate slot based on thread_id */
    int slot = (thread_id >= 0) ? thread_id % iterator_pool->pool_size : 0;
    
    _Atomic int *in_use = (type == ITER_QUERY)
        ? iterator_pool->query_in_use
        : iterator_pool->insert_in_use;
    
    /* Release atomically */
    atomic_store(&in_use[slot], 0);
}

/**
 * Cleanup iterator pool and destroy all iterators.
 * 
 * Thread Safety: Must be called when no threads are actively using the pools.
 */
static void vgen_cleanup_iterator_pool(void) {
    assert(iterator_pool);
    
    /* Destroy query iterators */
    for (int i = 0; i < iterator_pool->pool_size; i++) {
        if (iterator_pool->query_iterators[i]) {
            vg_iterator_destroy(iterator_pool->query_iterators[i]);
            iterator_pool->query_iterators[i] = NULL;
        }
        if (iterator_pool->insert_iterators[i]) {
            vg_iterator_destroy(iterator_pool->insert_iterators[i]);
            iterator_pool->insert_iterators[i] = NULL;
        }
    }
    
    /* Free pool resources */
    zfree(iterator_pool->query_iterators);
    zfree(iterator_pool->insert_iterators);
    zfree(iterator_pool->query_in_use);
    zfree(iterator_pool->insert_in_use);
    zfree(iterator_pool);
    iterator_pool = NULL;
}

/**
 * Initialize the vector generator from benchmark configuration.
 */
int vgen_init_from_config(uint32_t dimensions, uint64_t initial_capacity,
                           uint32_t num_centroids, float radius,
                           float sparsity, uint64_t seed,
                           int cluster_mode, const char *prefix, int num_threads) {    
    if (vgen_instance != NULL) {
        fprintf(stderr, "Warning: Vector generator already initialized\n");
        assert(0);
    }
    
    /* Store configuration values for later use */
    vgen_cluster_mode = cluster_mode;
    if (prefix) {
        snprintf(vgen_prefix, sizeof(vgen_prefix)+1, "%s", prefix);
    }
    
    /* Create generator configuration from parameters */
    generator_config_t vgen_config = {
        .dimensions = dimensions > 0 ? dimensions : 128,
        .initial_capacity = initial_capacity,
        .sparsity = sparsity,
        .radius = radius,
        .num_centroids = num_centroids,
        .seed = seed
    };
    
    /* Validate configuration */
    if (vgen_config.dimensions < 4 || vgen_config.dimensions > 2048) {
        fprintf(stderr, "Error: Vector dimensions must be between 4 and 2048 (got %u)\n", 
                vgen_config.dimensions);
        assert(0);
    }
    
    if (vgen_config.sparsity < 0.0f || vgen_config.sparsity > 1.0f) {
        fprintf(stderr, "Error: Sparsity must be between 0.0 and 1.0 (got %.2f)\n", 
                vgen_config.sparsity);
        assert(0);
    }
    
    /* Initialize the vector generator */
    vgen_instance = vg_init(&vgen_config);
    if (vgen_instance == NULL) {
        fprintf(stderr, "Error: Failed to initialize vector generator\n");
        assert(0);
    }
    
    /* Initialize iterator pool */
    if (num_threads > 0) {
        if (!vgen_init_iterator_pool(num_threads)) {
            fprintf(stderr, "Error: Failed to initialize iterator pool\n");
            vg_destroy(vgen_instance);
            vgen_instance = NULL;
            assert(0);
        }
    } else {
        /* Default pool size of 8 for single-threaded mode */
        if (!vgen_init_iterator_pool(8)) {
            fprintf(stderr, "Error: Failed to initialize iterator pool\n");
            vg_destroy(vgen_instance);
            vgen_instance = NULL;
            assert(0);
        }
    }
    
    /* Initialize ground truth storage */
    ground_truth.capacity = MAX_GROUND_TRUTH_ENTRIES;
    atomic_init(&ground_truth.count, 0);
    ground_truth.entries = calloc(ground_truth.capacity, sizeof(stored_ground_truth_t));
    if (ground_truth.entries == NULL) {
        fprintf(stderr, "Error: Failed to allocate ground truth storage\n");
        vgen_cleanup_iterator_pool();
        vg_destroy(vgen_instance);
        assert(0);
    }
    
    /* Initialize recall tracker */
    pthread_mutex_init(&recall_tracker.lock, NULL);
    vgen_reset_recall_stats();
    
    printf("Vector generator initialized:\n");
    printf("  Dimensions: %u\n", vgen_config.dimensions);
    printf("  Initial capacity: %lu\n", vgen_config.initial_capacity);
    printf("  Centroids: %u\n", vgen_config.num_centroids);
    printf("  Radius: %.2f\n", vgen_config.radius);
    printf("  Sparsity: %.2f\n", vgen_config.sparsity);
    printf("  Seed: %lu\n", vgen_config.seed);
    return 0;
}

/**
 * Cleanup and destroy the vector generator instance.
 */
void vgen_cleanup(void) {
    pthread_rwlock_wrlock(&vgen_lock);
    
    if (vgen_instance == NULL) {
        pthread_rwlock_unlock(&vgen_lock);
        return;
    }
    
    /* Cleanup iterator pool first */
    vgen_cleanup_iterator_pool();
    
    /* Cleanup ground truth storage */
    if (ground_truth.entries) {
        zfree(ground_truth.entries);
        ground_truth.entries = NULL;
        atomic_store(&ground_truth.count, 0);
        ground_truth.capacity = 0;
    }
    
    /* Destroy vector generator */
    vg_destroy(vgen_instance);
    vgen_instance = NULL;
    
    /* Cleanup recall tracker */
    pthread_mutex_destroy(&recall_tracker.lock);
    
    pthread_rwlock_unlock(&vgen_lock);
    pthread_rwlock_destroy(&vgen_lock);
}

/**
 * Set ground truth dataset size.
 */
void vgen_set_ground_truth_size(uint64_t num_vectors) {
    pthread_rwlock_rdlock(&vgen_lock);
    if (vgen_instance) {
        printf("[VGEN] Setting ground truth dataset size to: %lu\n", num_vectors);
        vg_set_ground_truth_dataset_size(vgen_instance, num_vectors);
    }
    pthread_rwlock_unlock(&vgen_lock);
}

/**
 * Precompute all ground truths for query vectors (warm-up phase).
 */
void vgen_precompute_ground_truths(void) {
    pthread_rwlock_rdlock(&vgen_lock);
    if (vgen_instance) {
        vg_precompute_all_ground_truths(vgen_instance);
    } else {
        fprintf(stderr, "[VGEN ERROR] Cannot precompute: generator not initialized\n");
    }
    pthread_rwlock_unlock(&vgen_lock);
}

/**
 * Replace both key and vector placeholders for ground truth ingestion.
 * This ingests the reserved-range vectors that serve as ground truth neighbors.
 * 
 * @param thread_id Thread identifier (must be < MAX_THREADS)
 * @param key_indices Array of key placeholder positions
 * @param key_count Number of key placeholders
 * @param vec_indices Array of vector placeholder positions
 * @param vec_count Number of vector placeholders
 * @param cmd Command buffer to modify in-place
 * @param key_counter Unused for vgen (kept for API compatibility)
 * @param vector_counter Unused for vgen (kept for API compatibility)
 */
void vgen_replace_ground_truth_placeholder(int thread_id, const size_t *key_indices, const size_t key_count,
                                            const size_t *vec_indices, const size_t vec_count,
                                            char *cmd, uint64_t *key_counter,
                                            uint64_t *vector_counter) {
    assert(iterator_pool && vgen_instance);
    if (key_count == 0 && vec_count == 0) return;
    
    /* Should have matching counts */
    if (key_count != vec_count) {
        fprintf(stderr, "Warning: key_count (%zu) != vec_count (%zu) in ground truth ingestion\n",
                key_count, vec_count);
        assert(key_count == vec_count);
    }
    
    /* For ground truth, we need to iterate over ALL possible neighbor keys from reserved range.
     * The vector generator pre-allocates neighbor keys for each query in the reserved range.
     * We need to extract all unique neighbor keys and generate vectors for them. */
    
    /* Get the total number of ground truth vectors we need to ingest.
     * This is: num_query_vectors * NEIGHBORS_PER_QUERY unique keys from reserved range */
    extern vector_generator_t* vgen_instance;
    
    /* For now, use a simple approach: generate vectors for keys in reserved range (1-999999)
     * that will be used as neighbors. We'll use a separate static counter. */
    
    static _Atomic uint64_t ground_truth_key_index = 0;
    
    /* Get dimensions (read-only after init, no lock needed) */
    uint32_t dims = vg_get_dimensions(vgen_instance);
    
    /* Generate vectors for reserved range keys */
    for (size_t i = 0; i < key_count; i++) {
        /* Use sequential keys from reserved range starting at 0 (not 1) to match ground truth */
        uint64_t current_index = atomic_fetch_add(&ground_truth_key_index, 1);
        vector_key_t key = current_index; 
        
        /* DEBUG: Print ground truth key being generated */
        static _Atomic int gt_debug_count = 0;
        int current_debug_count = atomic_fetch_add(&gt_debug_count, 1);
        if (current_debug_count < 20) {
            printf("[VGEN GROUND_TRUTH] Ingesting reserved key: %lu (iteration %d)\n", key, current_debug_count);
        }
        
        /* Replace key placeholder */
        if (i < key_count) {
            char key_buf[32];
            int key_len = snprintf(key_buf, sizeof(key_buf), "%lu", key);
            
            char *key_placeholder = cmd + key_indices[i];
            memset(key_placeholder, 0, 16);
            memcpy(key_placeholder, key_buf, key_len < 16 ? key_len : 16);
            
            uint32_t actual_key_len = (uint32_t)key_len;
            memcpy(key_placeholder + 16, &actual_key_len, 4);
        }
        
        /* Replace vector placeholder - generate vector for this reserved key */
        if (i < vec_count) {
            char *vec_placeholder = cmd + vec_indices[i];
            
            /* Generate vector from the reserved key (thread-safe) */
            float *vector_data = zmalloc(dims * sizeof(float));
            if (vector_data) {
                vg_generate_vector_from_key(vgen_instance, key, vector_data);
                memcpy(vec_placeholder, vector_data, dims * sizeof(float));
                zfree(vector_data);
            }
        }
    }
    
    (void)key_counter;
    (void)vector_counter;
    (void)thread_id;  /* Not used for ground truth - uses atomic counter instead */
}

/**
 * Replace key placeholder for deletion operations.
 * 
 * @param thread_id Thread identifier (unused for deletion, uses atomic counter)
 * @param indices Array of placeholder positions in command buffer
 * @param count Number of placeholders to replace
 * @param cmd Command buffer to modify in-place
 * @param key_counter Unused for vgen (kept for API compatibility)
 */
void vgen_replace_key_placeholder(int thread_id, const size_t *indices, const size_t count,
                                   char *cmd, uint64_t *key_counter) {
    assert(vgen_is_initialized() && count > 0);
    /* Use a simple atomic counter for deletion keys (lock-free) */
    static _Atomic uint64_t deletion_key_counter = 1000000;  /* Start after reserved range */
    
    /* Get keys and replace placeholders */
    for (size_t i = 0; i < count; i++) {
        /* Get next key atomically */
        vector_key_t key = atomic_fetch_add(&deletion_key_counter, 1);
        
        /* Format key - key number only, prefix already in template */
        char key_buf[32];
        int key_len = snprintf(key_buf, sizeof(key_buf), "%lu", key);
        
        /* Replace placeholder in command buffer */
        /* The placeholder location includes the prefix{tag}: already */
        /* We need to replace just the 16-byte VGEN_KEY_PLACEHOLDER + 4 byte length */
        char *placeholder = cmd + indices[i];
        
        /* Write the actual key number into the 12-byte placeholder space */
        memset(placeholder, 0, 12);  /* Clear placeholder first */
        memcpy(placeholder, key_buf, key_len < 12 ? key_len : 12);
        
        /* Write the actual key length in the 4-byte length field */
        uint32_t actual_key_len = (uint32_t)key_len;
        memcpy(placeholder + 12, &actual_key_len, 4);
    }
    
    (void)key_counter; /* Unused for vgen */
    (void)thread_id;   /* Not used - atomic counter instead */
}

/**
 * Replace vector placeholder for query operations.
 * 
 * @param thread_id Thread identifier (must be < MAX_THREADS)
 * @param indices Array of placeholder positions in command buffer
 * @param count Number of placeholders to replace
 * @param cmd Command buffer to modify in-place
 * @param vector_counter Unused for vgen (kept for API compatibility)
 * @return Query index for recall tracking, or UINT64_MAX if no query generated
 */
uint64_t vgen_replace_vector_placeholder_query(int thread_id, const size_t *indices, const size_t count,
                                            char *cmd, uint64_t *vector_counter) {
    if (!vgen_is_initialized() || count == 0) return UINT64_MAX;
    
    /* Get query iterator from pool (lock-free using atomics) */
    vector_iterator_t *iter = vgen_get_thread_iterator(thread_id, ITER_QUERY);
    if (iter == NULL) {
        fprintf(stderr, "Error: Failed to get query iterator for thread %d\n", thread_id);
        assert(0);
    }
    
    uint64_t query_idx = UINT64_MAX;
    
    /* Get dimensions (read-only after init, no lock needed) */
    uint32_t dims = vg_get_dimensions(vgen_instance);
    size_t vector_bytes = dims * sizeof(float);
    
    /* Get query vectors and replace placeholders */
    for (size_t i = 0; i < count; i++) {
        query_vector_t query;
        
        /* Access thread-local iterator (no lock needed) */
        int success = vg_iterator_next_query(iter, &query);
        if (!success) {
            fprintf(stderr, "Error: Failed to get query vector (thread_id=%d)\n", thread_id);
            fflush(stderr);
            fflush(stdout);
            assert(0);
            /* Iterator exhausted, reset it to cycle through queries again */
            iter->current = 0;
            success = vg_iterator_next_query(iter, &query);
        }
        
        if (!success) {
            fprintf(stderr, "Error: No query vectors available after reset (thread_id=%d)\n", thread_id);
            fflush(stderr);
            fflush(stdout);
            assert(0);
            /* Still no vectors available - skip */
            continue;
        }
        
        /* Replace placeholder with vector data (no lock needed) */
        char *placeholder = cmd + indices[i];
        memcpy(placeholder, query.vector.data, vector_bytes);
        
        /* Store ground truth for recall calculation using atomic index (lock-free) */
        uint64_t slot = atomic_fetch_add(&ground_truth.count, 1);
        if (slot < ground_truth.capacity) {
            query_idx = slot;  /* This is the index for this query */
            stored_ground_truth_t *entry = &ground_truth.entries[slot];
            entry->query_key = query.vector.key;
            
            /* DEBUG: Print query key and ground truth */
            static _Atomic int query_debug_count = 0;
            int debug_val = atomic_fetch_add(&query_debug_count, 1);
            if (debug_val < 5) {
                printf("[VGEN QUERY] Query key: %lu, Expected neighbors: ", query.vector.key);
                for (int j = 0; j < NEIGHBORS_PER_QUERY && j < 20; j++) {
                    printf("%lu ", query.ground_truth[j].key);
                }
                printf("...\n");
            }
            
            /* Copy ground truth neighbors from query */
            entry->neighbor_count = NEIGHBORS_PER_QUERY;
            for (int j = 0; j < NEIGHBORS_PER_QUERY; j++) {
                entry->neighbors[j] = query.ground_truth[j];
            }
        }
    }
    /* Release iterator back to pool (lock-free using atomics) */
    vgen_release_thread_iterator(thread_id, ITER_QUERY);
    
    (void)vector_counter; /* Unused for vgen */
    return query_idx;
}

/**
 * Replace both key and vector placeholders for ingestion operations.
 * 
 * @param thread_id Thread identifier (must be < MAX_THREADS)
 * @param key_indices Array of key placeholder positions
 * @param key_count Number of key placeholders
 * @param vec_indices Array of vector placeholder positions
 * @param vec_count Number of vector placeholders
 * @param cmd Command buffer to modify in-place
 * @param key_counter Unused for vgen (kept for API compatibility)
 * @param vector_counter Unused for vgen (kept for API compatibility)
 */
void vgen_replace_vector_and_key_placeholder(int thread_id, const size_t *key_indices, const size_t key_count,
                                              const size_t *vec_indices, const size_t vec_count,
                                              char *cmd, uint64_t *key_counter,
                                              uint64_t *vector_counter) {
    assert(vgen_is_initialized() && !(key_count == 0 && vec_count == 0));
    
    /* Should have matching counts for ingestion */
    assert(key_count == vec_count);
    
    /* Get ingestion iterator from pool (lock-free using atomics) */
    vector_iterator_t *iter = vgen_get_thread_iterator(thread_id, ITER_INSERT);
    if (iter == NULL) {
        fprintf(stderr, "Error: Failed to get ingestion iterator for thread %d\n", thread_id);
        assert(0);
    }
    
   
    /* Get vectors from ingestion iterator and replace both keys and vectors */
    for (size_t i = 0; i < key_count; i++) {
        vector_t vec;
        /* Replace vector placeholder */
        if (i < vec_count) {
            vec.data = (float *)(cmd + vec_indices[i]);
        }
        /* Access thread-local iterator (no lock needed) */
        if (!vg_iterator_next(iter, &vec)) {
            /* Iterator exhausted, reset it */
            iter->current = 0;
            if (!vg_iterator_next(iter, &vec)) {
                fprintf(stderr, "Error: No vectors available after reset (thread_id=%d)\n", thread_id);
                fflush(stderr);
                fflush(stdout);
                assert(0);
                /* Still no vectors available - skip */
                continue;
            }
        }
        
        /* Replace key placeholder */
        if (i < key_count) {
            /* Format key number only - prefix already in template */
            char key_buf[32];
            int key_len = snprintf(key_buf, sizeof(key_buf), "%lu", vec.key);
            
            /* DEBUG: Print key being generated for ingestion */
            static int debug_count = 0;
            if (debug_count < 20) {
                printf("[VGEN INGESTION] Generated key: %lu (iteration %d)\n", vec.key, debug_count);
                debug_count++;
            }
            
            char *key_placeholder = cmd + key_indices[i];
            /* Write key number into 16-byte placeholder space */
            memset(key_placeholder, 0, 16);  /* Clear first */
            memcpy(key_placeholder, key_buf, key_len < 16 ? key_len : 16);
            
            /* Write actual key length in 4-byte length field */
            uint32_t actual_key_len = (uint32_t)key_len;
            memcpy(key_placeholder + 16, &actual_key_len, 4);
        }
    }
    
    /* Release iterator back to pool (lock-free using atomics) */
    vgen_release_thread_iterator(thread_id, ITER_INSERT);
    
    (void)key_counter;
    (void)vector_counter;
}

/**
 * Compute recall for a search result.
 */
void vgen_compute_recall(uint64_t query_idx, void *reply) {
    if (!vgen_is_initialized() || !reply) return;
    
    valkeyReply *r = (valkeyReply *)reply;
    
    /* FT.SEARCH returns an array: [total_count, key1, fields1, key2, fields2, ...] */
    if (r->type != VALKEY_REPLY_ARRAY || r->elements < 1) {
        fprintf(stderr, "Error: Invalid reply format (query_idx=%lu)\n", query_idx);
        fflush(stderr);
        fflush(stdout);
        assert(0);
    }
    
    /* Skip if no valid query index */
    if (query_idx == UINT64_MAX) {
        fprintf(stderr, "Error: Invalid query index (query_idx=%lu)\n", query_idx);
        fflush(stderr);
        fflush(stdout);
        assert(0);
    }
    
    /* Copy ground truth data while holding lock (minimize lock time) */
    ground_truth_entry_t expected_neighbors_arr[NEIGHBORS_PER_QUERY];
    int expected_neighbors = 0;
    uint64_t query_key = 0;
    
    /* Read from ground truth storage (lock-free with atomic count) */
    uint64_t current_count = atomic_load(&ground_truth.count);
    if (query_idx < current_count) {
        stored_ground_truth_t *gt_entry = &ground_truth.entries[query_idx];
        expected_neighbors = gt_entry->neighbor_count;
        query_key = gt_entry->query_key;
        /* Copy ground truth neighbors to local buffer */
        for (int i = 0; i < expected_neighbors; i++) {
            expected_neighbors_arr[i] = gt_entry->neighbors[i];
        }
    }
    
    if (expected_neighbors == 0) {
        fprintf(stderr, "Error: No ground truth neighbors for query_idx=%lu (query_idx=%lu)\n", 
                query_idx, query_idx);
        fflush(stderr);
        fflush(stdout);
        assert(0);
    }
    
    /* Extract returned keys from the reply */
    /* Format: [count, key1, fields1, key2, fields2, ...] */
    /* Note: HNSW includes the query document in results, but ground truth excludes it */
    vector_key_t returned_keys[NEIGHBORS_PER_QUERY];
    int returned_count = 0;
    
    /* Parse the reply to extract keys 
     * Start from i=1 to get: element[0]=count, element[1]=result1_key (could be query itself), 
     * element[2]=fields1, element[3]=result2_key, element[4]=fields2, etc.
     */
    for (size_t i = 1; i < r->elements && returned_count < NEIGHBORS_PER_QUERY; i += 2) {
        if (i >= r->elements) break;
        
        valkeyReply *keyReply = r->element[i];
        if (keyReply && (keyReply->type == VALKEY_REPLY_STRING || keyReply->type == VALKEY_REPLY_STATUS)) {
            /* Parse the key string to extract the vector key (numeric ID) */
            /* Expected format: "vec:<prefix>:<key>" or similar */
            const char *key_str = keyReply->str;
            const char *last_colon = strrchr(key_str, ':');
            if (last_colon != NULL) {
                vector_key_t key = (vector_key_t)atoll(last_colon + 1);
                returned_keys[returned_count++] = key;
            }
        }
    }
    
    /* Compute recall: count how many of the returned keys are in the ground truth neighbors */
    int matches = 0;
    
    /* Debug output for first 20 queries */
    static _Atomic int debug_count = 0;
    int current_debug = atomic_fetch_add(&debug_count, 1);
    int should_debug = (current_debug < 5);  /* Show first 5 queries in detail */
    
    if (should_debug) {
        printf("\n[RECALL DEBUG #%d] Query idx=%lu, query_key=%lu\n", 
               current_debug, query_idx, query_key);
        printf("  Reply elements: %zu, returned_count: %d\n", r->elements, returned_count);
        
        printf("  Expected neighbors (%d): ", expected_neighbors);
        for (int j = 0; j < expected_neighbors; j++) {
            printf("%lu ", expected_neighbors_arr[j].key);
        }
        printf("\n");
        
        printf("  Returned neighbors (%d): ", returned_count);
        for (int i = 0; i < returned_count; i++) {
            printf("%lu ", returned_keys[i]);
        }
        printf("\n");
    }
    
    for (int i = 0; i < returned_count; i++) {
        for (int j = 0; j < expected_neighbors; j++) {
            if (returned_keys[i] == expected_neighbors_arr[j].key) {
                matches++;
                if (should_debug) {
                    printf("  ✓ Match: returned[%d]=%lu == expected[%d]=%lu\n", 
                           i, returned_keys[i], j, expected_neighbors_arr[j].key);
                }
                break;
            }
        }
    }
    
    if (should_debug) {
        printf("  Matches: %d/%d\n", matches, returned_count < expected_neighbors ? returned_count : expected_neighbors);
    }
    
    /* Calculate recall@K where K is the minimum of returned and expected */
    int k = returned_count < expected_neighbors ? returned_count : expected_neighbors;
    double recall = (k > 0) ? ((double)matches / (double)k) : 0.0;
    
    /* Update recall statistics */
    // pthread_mutex_lock(&recall_tracker.lock);
    recall_tracker.total_queries++;
    recall_tracker.total_recall += recall;
    if (recall < recall_tracker.min_recall) {
        recall_tracker.min_recall = recall;
    }
    if (recall > recall_tracker.max_recall) {
        recall_tracker.max_recall = recall;
    }
    // pthread_mutex_unlock(&recall_tracker.lock);
}

/**
 * Get ground truth for a specific query index.
 */
int vgen_get_ground_truth(uint64_t query_idx, uint64_t *neighbors) {
    if (!vgen_is_initialized() || !neighbors) return 0;
    
    /* Read from ground truth storage (lock-free with atomic count) */
    int count = 0;
    uint64_t current_count = atomic_load(&ground_truth.count);
    
    if (query_idx < current_count) {
        stored_ground_truth_t *gt_entry = &ground_truth.entries[query_idx];
        count = gt_entry->neighbor_count;
        
        /* Copy neighbor keys to output buffer */
        for (int i = 0; i < count; i++) {
            neighbors[i] = gt_entry->neighbors[i].key;
        }
    }
    
    return count;
}

/**
 * Get current query index for ground truth lookup.
 */
uint64_t vgen_get_current_query_index(void) {
    if (!vgen_is_initialized()) return 0;
    
    pthread_mutex_lock(&recall_tracker.lock);
    uint64_t idx = recall_tracker.current_query_index;
    pthread_mutex_unlock(&recall_tracker.lock);
    
    return idx;
}

/**
 * Print recall statistics report.
 */
void vgen_print_recall_report(void) {
    pthread_mutex_lock(&recall_tracker.lock);
    
    if (recall_tracker.total_queries == 0) {
        pthread_mutex_unlock(&recall_tracker.lock);
        return;
    }
    
    double avg_recall = recall_tracker.total_recall / recall_tracker.total_queries;
    
    printf("\n");
    printf("====== Recall Statistics ======\n");
    printf("  Total queries: %lu\n", recall_tracker.total_queries);
    printf("  Average recall: %.2f%%\n", avg_recall * 100.0);
    printf("  Min recall: %.2f%%\n", recall_tracker.min_recall * 100.0);
    printf("  Max recall: %.2f%%\n", recall_tracker.max_recall * 100.0);
    printf("\n");
    
    pthread_mutex_unlock(&recall_tracker.lock);
}

/**
 * Get vector generator statistics.
 */
void vgen_get_stats(uint64_t *total_generated, uint64_t *total_deleted,
                    uint64_t *active_vectors) {
    pthread_rwlock_rdlock(&vgen_lock);
    
    if (vgen_instance == NULL) {
        if (total_generated) *total_generated = 0;
        if (total_deleted) *total_deleted = 0;
        if (active_vectors) *active_vectors = 0;
        pthread_rwlock_unlock(&vgen_lock);
        return;
    }
    
    generator_stats_t stats;
    vg_get_stats(vgen_instance, &stats);
    
    if (total_generated) *total_generated = stats.total_generated;
    if (total_deleted) *total_deleted = stats.total_deleted;
    if (active_vectors) *active_vectors = stats.active_vectors;
    
    pthread_rwlock_unlock(&vgen_lock);
}

/**
 * Check if vector generator is initialized.
 */
int vgen_is_initialized(void) {
    int initialized = (vgen_instance != NULL);
    return initialized;
}

/**
 * Reset recall tracking statistics.
 */
void vgen_reset_recall_stats(void) {
    pthread_mutex_lock(&recall_tracker.lock);
    
    recall_tracker.total_queries = 0;
    recall_tracker.total_recall = 0.0;
    recall_tracker.min_recall = 1.0;
    recall_tracker.max_recall = 0.0;
    
    pthread_mutex_unlock(&recall_tracker.lock);
}

/**
 * Get iterator pool statistics for monitoring and debugging.
 * 
 * @param active_ingestion Out: number of active ingestion iterators
 * @param active_query Out: number of active query iterators
 * @param active_deletion Out: number of active deletion iterators (deprecated, always 0)
 */
void vgen_get_iterator_stats(int *active_ingestion, int *active_query, int *active_deletion) {
    if (!iterator_pool) {
        if (active_ingestion) *active_ingestion = 0;
        if (active_query) *active_query = 0;
        if (active_deletion) *active_deletion = 0;
        return;
    }
    
    int ingestion_count = 0;
    int query_count = 0;
    
    /* Count allocated iterators in the pool */
    for (int i = 0; i < iterator_pool->pool_size; i++) {
        if (iterator_pool->insert_iterators[i]) ingestion_count++;
        if (iterator_pool->query_iterators[i]) query_count++;
    }
    
    if (active_ingestion) *active_ingestion = ingestion_count;
    if (active_query) *active_query = query_count;
    if (active_deletion) *active_deletion = 0;  /* Deletion doesn't use pool */
}

void vgen_print_memory_stats(void) {
    printf("\n====== Vector Generator Memory Stats ======\n");
    
    /* Iterator pool stats */
    if (iterator_pool) {
        int ingestion_count = 0, query_count = 0;
        
        for (int i = 0; i < iterator_pool->pool_size; i++) {
            if (iterator_pool->insert_iterators[i]) ingestion_count++;
            if (iterator_pool->query_iterators[i]) query_count++;
        }
        
        printf("  Iterator Pool:\n");
        printf("    Pool size: %d\n", iterator_pool->pool_size);
        printf("    Query iterators allocated: %d\n", query_count);
        printf("    Insert iterators allocated: %d\n", ingestion_count);
        
        /* Estimate memory usage */
        size_t pool_memory = sizeof(VgenIteratorPool);
        pool_memory += iterator_pool->pool_size * sizeof(vector_iterator_t*) * 2;  /* query + insert pools */
        pool_memory += iterator_pool->pool_size * sizeof(_Atomic int) * 2;  /* in_use flags */
        printf("    Pool overhead: ~%zu bytes\n", pool_memory);
    } else {
        printf("  Iterator Pool: Not initialized\n");
    }
    
    /* Ground truth storage stats */
    uint64_t gt_count = atomic_load(&ground_truth.count);
    if (ground_truth.entries) {
        size_t gt_memory = ground_truth.capacity * sizeof(stored_ground_truth_t);
        printf("  Ground Truth Storage:\n");
        printf("    Capacity: %lu entries\n", (unsigned long)ground_truth.capacity);
        printf("    Entries stored: %lu\n", (unsigned long)gt_count);
        printf("    Memory allocated: %zu bytes (%.2f MB)\n", 
               gt_memory, (double)gt_memory / (1024 * 1024));
        printf("    Memory used: ~%zu bytes (%.2f MB)\n",
               gt_count * sizeof(stored_ground_truth_t),
               (double)(gt_count * sizeof(stored_ground_truth_t)) / (1024 * 1024));
        printf("    Utilization: %.1f%%\n", 
               (double)gt_count / ground_truth.capacity * 100.0);
    } else {
        printf("  Ground Truth Storage: Not initialized\n");
    }
    
    /* Recall tracking stats */
    printf("  Recall Tracker:\n");
    printf("    Total queries tracked: %lu\n", (unsigned long)recall_tracker.total_queries);
    if (recall_tracker.total_queries > 0) {
        printf("    Average recall: %.2f%%\n", 
               recall_tracker.total_recall / recall_tracker.total_queries);
        printf("    Min recall: %.2f%%\n", recall_tracker.min_recall);
        printf("    Max recall: %.2f%%\n", recall_tracker.max_recall);
    }
    
    printf("============================================\n\n");
}

