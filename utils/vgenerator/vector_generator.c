/**
 * vector_generator.c - Complete implementation with reserved key ranges
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

// use zmalloc for Valkey memory tracking
#include "zmalloc.h"
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

#include "vector_generator.h"

/* Hash functions */
static inline uint64_t hash64_finalize(uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

static inline uint64_t vg_hash64(uint64_t key, uint64_t seed) {
    const uint64_t prime1 = 0x9E3779B185EBCA87ULL;
    const uint64_t prime2 = 0xC2B2AE3D27D4EB4FULL;
    const uint64_t prime4 = 0x27D4EB2F165667C5ULL;
    
    uint64_t h64 = seed + prime4;
    h64 += key * prime2;
    h64 = ((h64 << 31) | (h64 >> 33)) * prime1;
    h64 = hash64_finalize(h64);
    
    return h64;
}

/* PCG PRNG */
typedef struct {
    uint64_t state;
    uint64_t inc;
} pcg32_random_t;

static inline uint32_t pcg32_random_r(pcg32_random_t* rng) {
    uint64_t oldstate = rng->state;
    rng->state = oldstate * 6364136223846793005ULL + rng->inc;
    uint32_t xorshifted = ((oldstate >> 18u) ^ oldstate) >> 27u;
    uint32_t rot = oldstate >> 59u;
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}

static inline void pcg32_srandom_r(pcg32_random_t* rng, uint64_t initstate, uint64_t initseq) {
    rng->state = 0U;
    rng->inc = (initseq << 1u) | 1u;
    pcg32_random_r(rng);
    rng->state += initstate;
    pcg32_random_r(rng);
}

static inline float uint32_to_float(uint32_t val) {
    const float scale = 2.0f / 4294967295.0f;
    return val * scale - 1.0f;
}

/* Bloom filter operations */
static bloom_filter_t* bloom_create(size_t expected_items) {
    bloom_filter_t *bloom = zmalloc(sizeof(bloom_filter_t));
    if (!bloom) return NULL;
    
    bloom->size_bytes = (expected_items * 10) / 8;
    if (bloom->size_bytes < 1024) bloom->size_bytes = 1024;
    
    bloom->bits = zcalloc(bloom->size_bytes * 1);
    if (!bloom->bits) {
        zfree(bloom);
        return NULL;
    }
    
    bloom->num_hashes = 7;
    return bloom;
}

static void bloom_destroy(bloom_filter_t *bloom) {
    if (bloom) {
        zfree(bloom->bits);
        zfree(bloom);
    }
}

static void bloom_add(bloom_filter_t *bloom, uint64_t key) {
    for (uint32_t i = 0; i < bloom->num_hashes; i++) {
        uint64_t hash = vg_hash64(key, i * 0x9E3779B97F4A7C15ULL);
        size_t bit_pos = hash % (bloom->size_bytes * 8);
        size_t byte_pos = bit_pos / 8;
        uint8_t bit_mask = 1 << (bit_pos % 8);
        bloom->bits[byte_pos] |= bit_mask;
    }
}

static int bloom_check(bloom_filter_t *bloom, uint64_t key) {
    for (uint32_t i = 0; i < bloom->num_hashes; i++) {
        uint64_t hash = vg_hash64(key, i * 0x9E3779B97F4A7C15ULL);
        size_t bit_pos = hash % (bloom->size_bytes * 8);
        size_t byte_pos = bit_pos / 8;
        uint8_t bit_mask = 1 << (bit_pos % 8);
        if (!(bloom->bits[byte_pos] & bit_mask)) {
            return 0;
        }
    }
    return 1;
}

/* Vector generation */
void vg_generate_vector_from_key(
    const vector_generator_t* gen,
    vector_key_t key,
    float* output) {
    
    uint32_t dim = gen->dimensions;
    
    pcg32_random_t rng;
    uint64_t seed1 = vg_hash64(key, gen->config.seed);
    uint64_t seed2 = vg_hash64(key, gen->config.seed + 1);
    pcg32_srandom_r(&rng, seed1, seed2 | 1);
    
    /* Step 1: Generate base random vector */
    for (uint32_t i = 0; i < dim; i++) {
        uint32_t val = pcg32_random_r(&rng);
        output[i] = uint32_to_float(val);
        
        /* Apply sparsity: set values to zero based on sparsity probability */
        if (gen->config.sparsity > 0.0f) {
            uint32_t sparsity_val = pcg32_random_r(&rng);
            float sparsity_rand = (float)sparsity_val / (float)UINT32_MAX;
            if (sparsity_rand < gen->config.sparsity) {
                output[i] = 0.0f;
            }
        }
    }
    
    /* Step 2: Apply centroid clustering if enabled */
    if (gen->config.num_centroids > 0 && gen->config.radius > 0.0f) {
        /* Select a centroid based on the key */
        uint32_t centroid_idx = key % gen->config.num_centroids;
        
        /* Generate centroid coordinates deterministically and apply directly */
        pcg32_random_t centroid_rng;
        uint64_t centroid_seed = vg_hash64(centroid_idx, gen->config.seed + 42);
        pcg32_srandom_r(&centroid_rng, centroid_seed, centroid_seed + 1);
        
        /* Apply clustering: modify each dimension based on its centroid */
        float radius_scale = gen->config.radius * 0.3f; /* Scale factor for radius */
        for (uint32_t i = 0; i < dim; i++) {
            /* Generate centroid coordinate for this dimension */
            uint32_t val = pcg32_random_r(&centroid_rng);
            float centroid_val = uint32_to_float(val) * gen->config.radius * 0.5f;
            
            if (output[i] != 0.0f) {  /* Don't modify sparse zeros */
                output[i] = centroid_val + (output[i] * radius_scale);
            }
        }
    } else if (gen->config.radius > 0.0f) {
        /* Apply radius scaling without clustering */
        float scale = gen->config.radius * 0.3f;
        for (uint32_t i = 0; i < dim; i++) {
            if (output[i] != 0.0f) {  /* Don't modify sparse zeros */
                output[i] *= scale;
            }
        }
    }
}

/* L2 distance computation */
float vg_compute_l2_distance(
    const float* v1,
    const float* v2,
    uint32_t dimensions) {
    
    float sum = 0.0f;
    for (uint32_t i = 0; i < dimensions; i++) {
        float diff = v1[i] - v2[i];
        sum += diff * diff;
    }
    return sum;
}

/* During initialization, compute ACTUAL nearest neighbors */
static void compute_query_ground_truth(
    vector_generator_t* gen,
    uint32_t query_idx) {
    
    /* CRITICAL: Validate dependencies before computation */
    assert(gen != NULL);
    assert(query_idx < gen->num_query_vectors);
    assert(gen->query_ground_truth != NULL);
    
    query_ground_truth_t* gt = &gen->query_ground_truth[query_idx];
    
    // pthread_mutex_lock(&gt->compute_mutex);
    if (gt->computed) {
        // pthread_mutex_unlock(&gt->compute_mutex);
        return;
    }
    
    float* query_vec = zmalloc(gen->dimensions * sizeof(float));
    assert(query_vec != NULL && "Failed to allocate memory for query vector");
    vg_generate_vector_from_key(gen, gt->query_key, query_vec);
    
    /* Search ALL reserved keys for actual nearest neighbors */
    typedef struct {
        vector_key_t key;
        float distance;
    } candidate_t;
    
    /* IMPORTANT: search_size must match the number of vectors actually ingested!
     * Use the ground_truth_dataset_size if set, otherwise fall back to a reasonable default. */
    uint32_t search_size;
    
    if (gen->ground_truth_dataset_size > 0) {
        /* Use the explicitly set dataset size (set via vg_set_ground_truth_dataset_size) */
        search_size = (uint32_t)gen->ground_truth_dataset_size;
        
        static _Atomic int debug_once = 0;
        if (atomic_fetch_add(&debug_once, 1) == 0) {
            printf("[VG] Computing ground truth with dataset_size: %lu\n", gen->ground_truth_dataset_size);
        }
    } else {
        /* CRITICAL ERROR: Ground truth dataset size MUST be set before queries!
         * This indicates vg_set_ground_truth_dataset_size() was not called properly. */
        // fprintf(stderr, "FATAL: Ground truth dataset size not set! Call vg_set_ground_truth_dataset_size() before running queries.\n");
        // fprintf(stderr, "       This will result in incorrect recall measurements.\n");
        // fflush(stderr);
        
        /* Assert in debug builds, but provide fallback in release */
        // assert(gen->ground_truth_dataset_size > 0 && "Ground truth dataset size MUST be set before computing ground truth!");
        
        /* Fallback: Use pinned_threshold (this is a bug if reached) */
        search_size = (uint32_t)gen->pinned_threshold;
        
        static _Atomic int debug_once2 = 0;
        if (atomic_fetch_add(&debug_once2, 1) == 0) {
            printf("[VG] WARNING: Using fallback pinned_threshold: %lu (THIS IS A BUG!)\n", gen->pinned_threshold);
        }
        
        /* Cap at RESERVED_KEY_RANGE to avoid going beyond reserved range */
        if (search_size > RESERVED_KEY_RANGE) {
            search_size = RESERVED_KEY_RANGE;
        }
        
        /* Minimum of 1K for reasonable ground truth quality */
        if (search_size < 1000) {
            search_size = 1000;
        }
    }
    
    candidate_t* candidates = zmalloc(search_size * sizeof(candidate_t));
    assert(candidates != NULL && "Failed to allocate memory for candidates");
    
    float* candidate_vec = zmalloc(gen->dimensions * sizeof(float));
    assert(candidate_vec != NULL && "Failed to allocate memory for candidate vector");
    
    /* Search all reserved keys including key 0 and the query itself */
    for (uint32_t i = 0; i < search_size; i++) {
        if (i == query_idx) {
            candidates[i].key = i;
            candidates[i].distance = 0.0f;
            continue;
        }
        vector_key_t key = i;  /* Start from 0 to include all keys */
        vg_generate_vector_from_key(gen, key, candidate_vec);
        candidates[i].key = key;
        candidates[i].distance = vg_compute_l2_distance(gt->query_vector, candidate_vec, gen->dimensions);
    }
    
    /* Sort and take top 10 */
    for (int i = 0; i < NEIGHBORS_PER_QUERY; i++) {
        uint32_t min_idx = i;
        for (uint32_t j = i + 1; j < search_size; j++) {
            if (candidates[j].distance < candidates[min_idx].distance) {
                min_idx = j;
            }
        }
        if (min_idx != i) {
            candidate_t temp = candidates[i];
            candidates[i] = candidates[min_idx];
            candidates[min_idx] = temp;
        }
        
        /* Store actual nearest neighbors */
        gt->neighbor_keys[i] = candidates[i].key;
        gt->neighbor_distances[i] = candidates[i].distance;
    }
    
    gt->computed = true;
    // pthread_mutex_unlock(&gt->compute_mutex);
    
    zfree(candidate_vec);
    zfree(candidates);
}

/* Compute ground truth for a specific query - alternative implementation */
static void compute_query_ground_truth2(
    vector_generator_t* gen,
    uint32_t query_idx) __attribute__((unused));
    
static void compute_query_ground_truth2(
    vector_generator_t* gen,
    uint32_t query_idx) {
    
    if (query_idx >= gen->num_query_vectors) return;
    
    query_ground_truth_t* gt = &gen->query_ground_truth[query_idx];
    
    pthread_mutex_lock(&gt->compute_mutex);
    if (gt->computed) {
        pthread_mutex_unlock(&gt->compute_mutex);
        return;
    }
    
    float* query_vec = zmalloc(gen->dimensions * sizeof(float));
    vg_generate_vector_from_key(gen, gt->query_key, query_vec);
    
    float* neighbor_vec = zmalloc(gen->dimensions * sizeof(float));
    for (int i = 0; i < NEIGHBORS_PER_QUERY; i++) {
        vg_generate_vector_from_key(gen, gt->neighbor_keys[i], neighbor_vec);
        gt->neighbor_distances[i] = vg_compute_l2_distance(
            gt->query_vector, neighbor_vec, gen->dimensions);
    }
    
    gt->computed = true;
    pthread_mutex_unlock(&gt->compute_mutex);
    
    zfree(query_vec);
    zfree(neighbor_vec);
}

/* Initialize generator */
vector_generator_t* vg_init(const generator_config_t* config) {
    if (!config || config->dimensions < 4 || config->dimensions > 2048) {
        errno = EINVAL;
        return NULL;
    }
    
    vector_generator_t* gen = zcalloc(1 * sizeof(vector_generator_t));
    if (!gen) return NULL;
    
    memcpy(&gen->config, config, sizeof(generator_config_t));
    gen->dimensions = config->dimensions;
    gen->pinned_threshold = config->initial_capacity / 2;
    gen->ground_truth_dataset_size = MAX_QUERY_VECTORS;  /* Will be set by vg_set_ground_truth_dataset_size */
    
    /* Set up reserved key ranges */
    gen->query_key_start = 0;
    gen->query_key_end = RESERVED_KEY_RANGE;
    gen->general_key_start = RESERVED_KEY_RANGE;
    gen->num_query_vectors = MAX_QUERY_VECTORS;
    
    /* Initialize query ground truth */
    gen->query_ground_truth = zcalloc(gen->num_query_vectors * sizeof(query_ground_truth_t));
    if (!gen->query_ground_truth) goto error;
    
    /* Pre-assign query and neighbor keys using prime number spacing for better distribution */
    for (uint32_t i = 0; i < gen->num_query_vectors; i++) {
        /* Use prime spacing (997) to distribute query keys across reserved range */
        gen->query_ground_truth[i].query_key = i;
        gen->query_ground_truth[i].query_vector = zmalloc(gen->dimensions * sizeof(float));
        vg_generate_vector_from_key(gen, gen->query_ground_truth[i].query_key, gen->query_ground_truth[i].query_vector);
        for (int j = 0; j < NEIGHBORS_PER_QUERY; j++) {
            gen->query_ground_truth[i].neighbor_keys[j] = 
                gen->query_ground_truth[i].query_key + j + 1;
        }
        
        gen->query_ground_truth[i].computed = false;
        pthread_mutex_init(&gen->query_ground_truth[i].compute_mutex, NULL);
    }
    
    /* Initialize allocator to start after reserved range */
    atomic_init(&gen->allocator.next_key, gen->general_key_start);
    atomic_init(&gen->allocator.deleted_count, 0);
    atomic_init(&gen->allocator.active_count, 0);
    atomic_init(&gen->query_index, 0);
    
    /* Initialize deletion tracking */
    gen->deleted_keys = bloom_create(config->initial_capacity);
    if (!gen->deleted_keys) goto error;
    
    pthread_rwlock_init(&gen->delete_lock, NULL);
    
    return gen;

error:
    vg_destroy(gen);
    return NULL;
}

/* Destroy generator */
void vg_destroy(vector_generator_t* gen) {
    if (!gen) return;
    
    if (gen->query_ground_truth) {
        for (uint32_t i = 0; i < gen->num_query_vectors; i++) {
            pthread_mutex_destroy(&gen->query_ground_truth[i].compute_mutex);
        }
        zfree(gen->query_ground_truth);
    }
    
    bloom_destroy(gen->deleted_keys);
    pthread_rwlock_destroy(&gen->delete_lock);
    zfree(gen);
}

/* Set ground truth dataset size */
void vg_set_ground_truth_dataset_size(vector_generator_t* gen, uint64_t dataset_size) {
    assert(gen != NULL && "Generator must not be NULL");
    assert(dataset_size > 0 && "Dataset size must be > 0");
    assert(dataset_size <= RESERVED_KEY_RANGE && "Dataset size must fit in reserved range");
    
    /* Cap at RESERVED_KEY_RANGE */
    if (dataset_size > RESERVED_KEY_RANGE) {
        dataset_size = RESERVED_KEY_RANGE;
    }
    
    gen->ground_truth_dataset_size = dataset_size;
    
    /* Invalidate any already-computed ground truth since dataset size changed */
    if (gen->query_ground_truth) {
        for (uint32_t i = 0; i < gen->num_query_vectors; i++) {
            gen->query_ground_truth[i].computed = false;
        }
    }
}
static atomic_int completed_ground_truth_vectors = 0;
// #ifdef USE_PTHREADS_FOR_GT_COMPUTE
#include <pthread.h>
#define NUM_THREADS 10
typedef struct {
    vector_generator_t* gen;
    uint32_t start_idx;
    uint32_t end_idx;
    atomic_int *completed;
} thread_arg_t;


void* thread_func(void* arg) {
    thread_arg_t* t_arg = (thread_arg_t*)arg;
    for (uint32_t i = t_arg->start_idx; i < t_arg->end_idx; i++) {
        compute_query_ground_truth(t_arg->gen, i);
        atomic_fetch_add(t_arg->completed, 1);
        if (*t_arg->completed % 100 == 0) {
            fprintf(stderr, "[VG] Progress: %d/%d queries (%.1f%%)\n",
                    *t_arg->completed, t_arg->gen->num_query_vectors,
                    100.0 * *t_arg->completed / t_arg->gen->num_query_vectors);
        }
    }
    return NULL;
}

/* Precompute all ground truths for query vectors */
void vg_precompute_all_ground_truths(vector_generator_t* gen) {
    assert(gen != NULL && "Generator must not be NULL");
    assert(gen->query_ground_truth != NULL && "Query ground truth must be initialized");
    
    if (gen->ground_truth_dataset_size == 0) {
        fprintf(stderr, "[VG WARNING] Ground truth dataset size not set. Call vg_set_ground_truth_dataset_size() first.\n");
        return;
    }
    
    fprintf(stderr, "[VG] Precomputing ground truths for %u queries with dataset_size=%lu...\n", 
            gen->num_query_vectors, gen->ground_truth_dataset_size);
    
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    // create threads each thread will compute a portion of the queries
    // and give each thread its own copy of gen to avoid contention
    // use pthreads for portability
    // #define USE_PTHREADS_FOR_GT_COMPUTE
    // use atomic to track progress and print every 100 queries

    pthread_t threads[NUM_THREADS];
    thread_arg_t thread_args[NUM_THREADS];
    uint32_t queries_per_thread = gen->num_query_vectors / NUM_THREADS;
    for (int t = 0; t < NUM_THREADS; t++) {
        fprintf(stderr, "[VG] Starting thread %d for queries %d to %d\n", 
            t, t * queries_per_thread, (t + 1) * queries_per_thread - 1);
        thread_args[t].gen = gen;
        thread_args[t].start_idx = t * queries_per_thread;
        thread_args[t].completed = &completed_ground_truth_vectors;
        if (t == NUM_THREADS - 1) {
            thread_args[t].end_idx = gen->num_query_vectors - (t * queries_per_thread);
        } else {
            thread_args[t].end_idx = (t + 1) * queries_per_thread;
        }
        pthread_create(&threads[t], NULL, thread_func, &thread_args[t]);
    }
    for (int t = 0; t < NUM_THREADS; t++) {
        pthread_join(threads[t], NULL);
    }

    
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    
    fprintf(stderr, "[VG] Ground truth precomputation complete! Computed %u queries in %.2f seconds (%.0f queries/sec)\n",
            gen->num_query_vectors, elapsed, gen->num_query_vectors / elapsed);
}

/* Create ingestion iterator */
vector_iterator_t* vg_get_ingestion_iterator(
    vector_generator_t* gen,
    uint64_t count,
    generation_order_t order) {
    
    /* CRITICAL: Validate dependencies */
    assert(gen != NULL && "Generator must not be NULL");
    assert(count > 0 && "Iterator count must be > 0");
    assert(order == GEN_ORDER_RANDOM || order == GEN_ORDER_CENTROID);
    
    vector_iterator_t* iter = zcalloc(1 * sizeof(vector_iterator_t));
    if (!iter) return NULL;
    
    iter->generator = gen;
    iter->type = ITER_TYPE_INGESTION;
    iter->order = order;
    iter->count = count;
    iter->current = 0;
    
    iter->key_sequence = zmalloc(count * sizeof(uint64_t));
    if (!iter->key_sequence) {
        zfree(iter);
        return NULL;
    }
    uint64_t next_key_batch = atomic_fetch_add(&gen->allocator.next_key, count);
    
    /* Validate we're not overlapping with reserved range */
    assert(next_key_batch >= gen->general_key_start && 
           "Ingestion keys must be in general range, not reserved range!");
    
    /* Generate keys from general range only */
    for (uint64_t i = 0; i < count; i++) {
        iter->key_sequence[i] = next_key_batch + i;
    }
    
    atomic_fetch_add(&gen->allocator.active_count, count);
    
    return iter;
}

/* Get next vector */
bool vg_iterator_next(vector_iterator_t* iter, vector_t* vec) {
    assert(iter != NULL && "Iterator must not be NULL");
    assert(vec != NULL && "Output vector must not be NULL");
    assert(iter->type == ITER_TYPE_INGESTION && "Wrong iterator type for vg_iterator_next");
    
    if (iter->current >= iter->count) {
        return false;
    }
    
    vector_key_t key = iter->key_sequence[iter->current];
    vec->key = key;
    if (vec->data) {                
        vg_generate_vector_from_key(iter->generator, key, vec->data);
    }
    iter->current++;
    
    return true;
}

/* Create query iterator */
vector_iterator_t* vg_get_query_iterator(
    vector_generator_t* gen,
    uint64_t count) {
    
    /* CRITICAL: Validate dependencies */
    assert(gen != NULL && "Generator must not be NULL");
    assert(gen->query_ground_truth != NULL && "Query ground truth must be initialized");
    assert(gen->num_query_vectors > 0 && "Must have query vectors configured");
    
    vector_iterator_t* iter = zcalloc(1 * sizeof(vector_iterator_t));
    if (!iter) return NULL;
    
    iter->generator = gen;
    iter->type = ITER_TYPE_QUERY;
    iter->count = count;
    iter->current = 0;
    
    return iter;
}

/* The query iterator should ONLY return reserved keys */
bool vg_iterator_next_query(vector_iterator_t* iter, query_vector_t* query) {
    assert(iter != NULL && "Iterator must not be NULL");
    assert(query != NULL && "Output query must not be NULL");
    assert(iter->type == ITER_TYPE_QUERY && "Wrong iterator type for vg_iterator_next_query");
    assert(iter->generator != NULL && "Generator must not be NULL");
    
    if (iter->current >= iter->count) {
        return false;
    }
    
    /* Always use pre-defined queries from reserved range */
    uint32_t query_idx = iter->current % iter->generator->num_query_vectors;
    assert(query_idx < iter->generator->num_query_vectors && "Query index out of bounds");
    
    /* Compute ground truth if needed */
    compute_query_ground_truth(iter->generator, query_idx);
    
    query_ground_truth_t* gt = &iter->generator->query_ground_truth[query_idx];
    assert(gt->computed && "Ground truth must be computed at this point");
    
    /* Return query from RESERVED range */
    query->vector.key = gt->query_key;
    assert(query->vector.key < RESERVED_KEY_RANGE && "Query key must be in reserved range");

    query->vector.data = gt->query_vector;
    assert(query->vector.data != NULL && "Failed to allocate memory for query vector");
        
    /* Ground truth neighbors are also from RESERVED range */
    for (int i = 0; i < NEIGHBORS_PER_QUERY; i++) {
        query->ground_truth[i].key = gt->neighbor_keys[i];
        query->ground_truth[i].distance = gt->neighbor_distances[i];
        
        /* Validate neighbor keys are in correct range */
        assert(query->ground_truth[i].key < RESERVED_KEY_RANGE && 
               "Ground truth neighbor keys must be in reserved range");
    }
    
    iter->current++;
    return true;
}

/* Create deletion iterator */
vector_iterator_t* vg_get_deletion_iterator(
    vector_generator_t* gen,
    uint64_t count) {
    
    uint64_t active = atomic_load(&gen->allocator.active_count);
    uint64_t deleted = atomic_load(&gen->allocator.deleted_count);
    
    if (active - deleted <= gen->pinned_threshold) {
        return NULL;
    }
    
    uint64_t max_delete = (active - deleted) - gen->pinned_threshold;
    if (count > max_delete) {
        count = max_delete;
    }
    
    vector_iterator_t* iter = zcalloc(1 * sizeof(vector_iterator_t));
    if (!iter) return NULL;
    
    iter->generator = gen;
    iter->type = ITER_TYPE_DELETION;
    iter->count = count;
    iter->current = 0;
    
    iter->key_sequence = zmalloc(count * sizeof(uint64_t));
    if (!iter->key_sequence) {
        zfree(iter);
        return NULL;
    }
    
    pcg32_random_t rng;
    uint64_t current_key = atomic_load(&gen->allocator.next_key);
    pcg32_srandom_r(&rng, gen->config.seed, active + deleted);
    
    /* Only delete from general range, never reserved keys */
    for (uint64_t i = 0; i < count; i++) {
        uint64_t key;
        int attempts = 0;
        do {
            uint32_t r = pcg32_random_r(&rng);
            /* Only select from general range */
            uint64_t range = current_key - gen->general_key_start;
            if (range == 0) range = 1;
            key = gen->general_key_start + (r % range);
            
            attempts++;
            if (attempts > 1000) {
                key = gen->general_key_start + i;
                break;
            }
        } while (bloom_check(gen->deleted_keys, key));
        
        iter->key_sequence[i] = key;
        
        pthread_rwlock_wrlock(&gen->delete_lock);
        bloom_add(gen->deleted_keys, key);
        pthread_rwlock_unlock(&gen->delete_lock);
    }
    
    atomic_fetch_add(&gen->allocator.deleted_count, count);
    
    return iter;
}

/* Get next deletion key */
bool vg_iterator_next_key(vector_iterator_t* iter, vector_key_t* key) {
    if (iter->current >= iter->count) {
        return false;
    }
    
    *key = iter->key_sequence[iter->current++];
    return true;
}

/* Destroy iterator */
void vg_iterator_destroy(vector_iterator_t* iter) {
    if (!iter) return;
    
    zfree(iter->key_sequence);
    zfree(iter->vector_buffer);
    zfree(iter);
}

/* Get stats */
void vg_get_stats(const vector_generator_t* gen, generator_stats_t* stats) {
    stats->total_generated = atomic_load(&gen->allocator.next_key) - gen->general_key_start;
    stats->total_deleted = atomic_load(&gen->allocator.deleted_count);
    stats->active_vectors = atomic_load(&gen->allocator.active_count);
    stats->pinned_vectors = gen->pinned_threshold;
}

/* For testing, vg_find_ground_truth should handle both cases properly */
void vg_find_ground_truth(
    const vector_generator_t* gen,
    const vector_t* query,
    ground_truth_entry_t* ground_truth,
    uint32_t k) {
    
    /* Check if this is one of our pre-defined queries */
    for (uint32_t i = 0; i < gen->num_query_vectors; i++) {
        if (gen->query_ground_truth[i].query_key == query->key) {
            /* This is a reserved query - return its pre-computed ground truth */
            if (!gen->query_ground_truth[i].computed) {
                compute_query_ground_truth((vector_generator_t*)gen, i);
            }
            
            for (uint32_t j = 0; j < k && j < NEIGHBORS_PER_QUERY; j++) {
                ground_truth[j].key = gen->query_ground_truth[i].neighbor_keys[j];
                ground_truth[j].distance = gen->query_ground_truth[i].neighbor_distances[j];
            }
            return;
        }
    }
    
    /* For arbitrary test queries, do brute force on reserved range */
    float* candidate_vec = zmalloc(gen->dimensions * sizeof(float));
    typedef struct {
        vector_key_t key;
        float distance;
    } candidate_t;
    
    /* Only search within reserved range for consistency */
    uint32_t max_search = RESERVED_KEY_RANGE;  /* Limit for performance */
    candidate_t* candidates = zmalloc(max_search * sizeof(candidate_t));
    
    for (uint32_t i = 0; i < max_search; i++) {
        vector_key_t key = i;
        vg_generate_vector_from_key(gen, key, candidate_vec);
        candidates[i].key = key;
        candidates[i].distance = vg_compute_l2_distance(query->data, candidate_vec, gen->dimensions);
    }
    
    /* Sort and return top k */
    for (uint32_t i = 0; i < k && i < max_search; i++) {
        uint32_t min_idx = i;
        for (uint32_t j = i + 1; j < max_search; j++) {
            if (candidates[j].distance < candidates[min_idx].distance) {
                min_idx = j;
            }
        }
        if (min_idx != i) {
            candidate_t temp = candidates[i];
            candidates[i] = candidates[min_idx];
            candidates[min_idx] = temp;
        }
        ground_truth[i].key = candidates[i].key;
        ground_truth[i].distance = candidates[i].distance;
    }
    
    zfree(candidate_vec);
    zfree(candidates);
}