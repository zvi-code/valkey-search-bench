/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2024-present, Zvi Schneider
 * 
 * Dataset API - Binary dataset file format reading and manipulation
 * 
 * This file is part of valkey-search-benchmark and is licensed under the
 * BSD 3-Clause License. See the LICENSE file in the root directory.
 */

#include "dataset_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <math.h>

#define MAX_QUERY_VEC_PER_NEIGHBORS 1
// packed struct of uint64_t array of size MAX_QUERY_VEC_PER_NEIGHBORS
typedef struct __attribute__((packed)) {
    uint64_t neighbors[MAX_QUERY_VEC_PER_NEIGHBORS+1];
} query_vec_neighbors_t;

/* Internal context */
struct dataset_ctx {
    int fd;
    void *mmap_base;
    size_t mmap_size;
    dataset_header_t *header;
    float *vectors;
    float *queries;
    int64_t *ground_truth;
    dataset_neighbors_t* neighbors; // array of neighbors for each query with pre-calculated distances
    query_vec_neighbors_t *query_neighbors; // a reverse map from neighbor index to query index
    
    /* NEW: Metadata fields */
    dataset_metadata_csr_t *vector_metadata;    /* Vector tags (NULL if no metadata) */
    dataset_metadata_csr_t *query_metadata;     /* Query predicates (NULL if no metadata) */
    dataset_vocabulary_t *vocabulary;           /* Tag names (NULL if no metadata) */
    uint32_t **inverted_index;                  /* Tag → vector IDs (cached in RAM) */
};

static const char *distance_metric_names[] = {
    [DISTANCE_L2] = "L2",
    [DISTANCE_COSINE] = "COSINE",
    [DISTANCE_IP] = "IP"
};

static const char *dtype_names[] = {
    [DTYPE_FLOAT32] = "FLOAT32",
    [DTYPE_FLOAT16] = "FLOAT16"
};

/* Path resolution: try multiple locations */
static int resolve_dataset_path(const char *name, char *out, size_t size) {
    const char *search_paths[] = {
        "%s",                           /* Direct path */
        "./datasets/%s",                /* Datasets directory (primary) */
        "./datasets/%s.bin",            /* Datasets directory with .bin */
        "/var/datasets/%s.bin",         /* System location */
        NULL
    };

    for (int i = 0; search_paths[i]; i++) {
        snprintf(out, size, search_paths[i], name);
        if (access(out, R_OK) == 0) return 0;
    }

    fprintf(stderr, "Dataset not found: %s\n", name);
    return -1;
}

/** Calculate distance between query vector and its neighbors */
float calculateDistance(const float *vec1, const float *vec2, int dim, const char *metric) {
    if (strcmp(metric, "L2") == 0) {
        float sum = 0.0f;
        for (int i = 0; i < dim; i++) {
            float diff = vec1[i] - vec2[i];
            sum += diff * diff;
        }
        return sqrtf(sum);
    } else if (strcmp(metric, "COSINE") == 0) {
        float dot = 0.0f, norm1 = 0.0f, norm2 = 0.0f;
        for (int i = 0; i < dim; i++) {
            dot += vec1[i] * vec2[i];
            norm1 += vec1[i] * vec1[i];
            norm2 += vec2[i] * vec2[i];
        }
        if (norm1 == 0 || norm2 == 0) return 1.0f; // Avoid division by zero
        return 1.0f - (dot / (sqrtf(norm1) * sqrtf(norm2))); // Cosine distance
    } else {
        fprintf(stderr, "Unknown metric: %s\n", metric);
        return -1.0f;
    }
}

/* Helper: Load CSR sparse matrix from mmap */
static dataset_metadata_csr_t* load_csr_from_mmap(void *base, uint64_t offset, uint32_t expected_nrow) {
    if (offset == 0) return NULL;
    
    dataset_metadata_csr_t *csr = malloc(sizeof(dataset_metadata_csr_t));
    if (!csr) return NULL;
    
    /* Read CSR header: nrow, ncol, nnz */
    uint32_t *header = (uint32_t*)((uint8_t*)base + offset);
    csr->nrow = header[0];
    csr->ncol = header[1];
    csr->nnz = header[2];
    
    if (csr->nrow != expected_nrow) {
        fprintf(stderr, "CSR row count mismatch: expected %u, got %u\n", expected_nrow, csr->nrow);
        free(csr);
        return NULL;
    }
    
    /* Point to mmap'd arrays (zero-copy) */
    uint8_t *ptr = (uint8_t*)base + offset + 12;  /* After 3x uint32 header */
    csr->indptr = (uint32_t*)ptr;
    ptr += (csr->nrow + 1) * sizeof(uint32_t);
    
    csr->indices = (uint32_t*)ptr;
    ptr += csr->nnz * sizeof(uint32_t);
    
    csr->data = (float*)ptr;
    
    return csr;
}

/* Helper: Load vocabulary from mmap */
static dataset_vocabulary_t* load_vocabulary_from_mmap(void *base, uint64_t offset, uint32_t vocab_size) {
    if (offset == 0 || vocab_size == 0) return NULL;
    
    dataset_vocabulary_t *vocab = malloc(sizeof(dataset_vocabulary_t));
    if (!vocab) return NULL;
    
    vocab->vocab_size = vocab_size;
    vocab->words = malloc(vocab_size * sizeof(char*));
    if (!vocab->words) {
        free(vocab);
        return NULL;
    }
    
    /* String data is null-terminated strings in sequence */
    vocab->string_data = (uint8_t*)base + offset;
    char *ptr = (char*)vocab->string_data;
    
    for (uint32_t i = 0; i < vocab_size; i++) {
        vocab->words[i] = ptr;
        ptr += strlen(ptr) + 1;  /* Move to next string */
    }
    
    return vocab;
}

/* Helper: Build inverted index (tag_id -> [vector_ids]) */
static uint32_t** build_inverted_index(dataset_metadata_csr_t *csr) {
    if (!csr) return NULL;
    
    /* Allocate array of lists (one per tag) */
    uint32_t **inverted = calloc(csr->ncol, sizeof(uint32_t*));
    if (!inverted) return NULL;
    
    uint32_t *counts = calloc(csr->ncol, sizeof(uint32_t));
    if (!counts) {
        free(inverted);
        return NULL;
    }
    
    /* Count vectors per tag */
    for (uint32_t i = 0; i < csr->nnz; i++) {
        counts[csr->indices[i]]++;
    }
    
    /* Allocate lists (store count at index 0) */
    for (uint32_t tag = 0; tag < csr->ncol; tag++) {
        if (counts[tag] > 0) {
            inverted[tag] = malloc((counts[tag] + 1) * sizeof(uint32_t));
            if (inverted[tag]) {
                inverted[tag][0] = counts[tag];  /* Store count */
            }
        }
    }
    
    /* Reset counts for insertion */
    memset(counts, 0, csr->ncol * sizeof(uint32_t));
    
    /* Fill inverted index */
    for (uint32_t vec_id = 0; vec_id < csr->nrow; vec_id++) {
        uint32_t start = csr->indptr[vec_id];
        uint32_t end = csr->indptr[vec_id + 1];
        
        for (uint32_t i = start; i < end; i++) {
            uint32_t tag_id = csr->indices[i];
            if (inverted[tag_id]) {
                uint32_t pos = ++counts[tag_id];  /* Increment then use */
                inverted[tag_id][pos] = vec_id;
            }
        }
    }
    
    free(counts);
    return inverted;
}

dataset_ctx_t* dataset_init(const char *dataset_name, dataset_info_t *info) {
    char path[1024];
    if (resolve_dataset_path(dataset_name, path, sizeof(path)) != 0) {
        return NULL;
    }

    /* Open file read-only */
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Failed to open: %s (%s)\n", path, strerror(errno));
        return NULL;
    }

    /* Get file size */
    struct stat st;
    if (fstat(fd, &st) != 0) {
        fprintf(stderr, "Failed to stat: %s\n", strerror(errno));
        close(fd);
        return NULL;
    }

    /* mmap entire file - OS handles caching */
    void *base = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        fprintf(stderr, "Failed to mmap: %s\n", strerror(errno));
        close(fd);
        return NULL;
    }

    /* Advise sequential access for prefill, random for queries */
    madvise(base, st.st_size, MADV_WILLNEED);

    /* Validate header */
    dataset_header_t *header = (dataset_header_t*)base;
    if (header->magic != DATASET_MAGIC) {
        fprintf(stderr, "Invalid magic: 0x%x (expected 0x%x)\n", header->magic, DATASET_MAGIC);
        munmap(base, st.st_size);
        close(fd);
        return NULL;
    }

    /* Allocate context */
    dataset_ctx_t *ctx = calloc(1, sizeof(dataset_ctx_t));
    if (!ctx) {
        munmap(base, st.st_size);
        close(fd);
        return NULL;
    }

    /* Initialize pointers into mmap */
    ctx->fd = fd;
    ctx->mmap_base = base;
    ctx->mmap_size = st.st_size;
    ctx->header = header;
    ctx->vectors = (float*)((uint8_t*)base + header->vectors_offset);
    ctx->queries = (float*)((uint8_t*)base + header->queries_offset);
    ctx->ground_truth = (int64_t*)((uint8_t*)base + header->ground_truth_offset);
    ctx->neighbors = malloc(sizeof(dataset_neighbors_t) * header->num_queries);
    memset(ctx->neighbors, 0, sizeof(dataset_neighbors_t) * header->num_queries);
    // pre-calculate distances and store in neighbors
    for (uint64_t q = 0; q < header->num_queries; q++) {
        ctx->neighbors[q].ids = malloc(header->num_neighbors * sizeof(uint64_t));
        ctx->neighbors[q].dists = malloc(header->num_neighbors * sizeof(float));
        ctx->neighbors[q].count = header->num_neighbors;
        const float *query_vec = ctx->queries + (q * header->dim);
        for (uint32_t n = 0; n < header->num_neighbors; n++) {
            int64_t neighbor_idx = ctx->ground_truth[q * header->num_neighbors + n];
            assert(neighbor_idx >= 0 && neighbor_idx < header->num_vectors);
            const float *neighbor_vec = ctx->vectors + (neighbor_idx * header->dim);
            // calculate L2 distance
            float dist = calculateDistance(query_vec, neighbor_vec, header->dim, distance_metric_names[header->distance_metric]);
            ctx->neighbors[q].ids[n] = (uint64_t)neighbor_idx;
            ctx->neighbors[q].dists[n] = dist;            
        }
    }
    // a reverse map from neighbor index to query index
    //
    ctx->query_neighbors = malloc(sizeof(query_vec_neighbors_t) * header->num_vectors*header->num_neighbors);
    memset(ctx->query_neighbors, 0, sizeof(query_vec_neighbors_t) * header->num_vectors*header->num_neighbors);
    
    /* NEW: Load metadata if present (version 2) */
    if (header->version >= DATASET_VERSION_METADATA && header->has_metadata) {
        printf("Loading metadata...\n");
        
        /* Load vector metadata CSR */
        ctx->vector_metadata = load_csr_from_mmap(base, header->vector_metadata_offset, header->num_vectors);
        if (!ctx->vector_metadata) {
            fprintf(stderr, "Failed to load vector metadata\n");
            munmap(base, st.st_size);
            close(fd);
            free(ctx->neighbors);
            free(ctx->query_neighbors);
            free(ctx);
            return NULL;
        }
        
        /* Load query metadata CSR */
        ctx->query_metadata = load_csr_from_mmap(base, header->query_metadata_offset, header->num_queries);
        if (!ctx->query_metadata) {
            fprintf(stderr, "Failed to load query metadata\n");
            free(ctx->vector_metadata);
            munmap(base, st.st_size);
            close(fd);
            free(ctx->neighbors);
            free(ctx->query_neighbors);
            free(ctx);
            return NULL;
        }
        
        /* Load vocabulary */
        ctx->vocabulary = load_vocabulary_from_mmap(base, header->vocab_offset, header->vocab_size);
        if (!ctx->vocabulary) {
            fprintf(stderr, "Failed to load vocabulary\n");
            free(ctx->vector_metadata);
            free(ctx->query_metadata);
            munmap(base, st.st_size);
            close(fd);
            free(ctx->neighbors);
            free(ctx->query_neighbors);
            free(ctx);
            return NULL;
        }
        
        /* Build inverted index (tag_id → [vector_ids]) */
        printf("Building inverted index...\n");
        ctx->inverted_index = build_inverted_index(ctx->vector_metadata);
        if (!ctx->inverted_index) {
            fprintf(stderr, "Failed to build inverted index\n");
            free(ctx->vocabulary->words);
            free(ctx->vocabulary);
            free(ctx->vector_metadata);
            free(ctx->query_metadata);
            munmap(base, st.st_size);
            close(fd);
            free(ctx->neighbors);
            free(ctx->query_neighbors);
            free(ctx);
            return NULL;
        }
        
        printf("Metadata loaded: %u tags, %u vector tags, %u query predicates\n",
               header->vocab_size,
               ctx->vector_metadata->nnz,
               ctx->query_metadata->nnz);
    } else {
        ctx->vector_metadata = NULL;
        ctx->query_metadata = NULL;
        ctx->vocabulary = NULL;
        ctx->inverted_index = NULL;
    }
    
    /* Return metadata */
    if (info) {
        snprintf(info->distance_metric, sizeof(info->distance_metric), "%s", distance_metric_names[ctx->header->distance_metric]);
        snprintf(info->dtype, sizeof(info->dtype), "%s", dtype_names[ctx->header->dtype]);
        info->dim = header->dim;
        info->num_vectors = header->num_vectors;
        info->num_queries = header->num_queries;
        info->num_neighbors = header->num_neighbors;
        info->vocab_size = header->has_metadata ? header->vocab_size : 0;
        info->has_metadata = header->has_metadata;
    }
    int max_queries_per_neighbor = 0;
    for (uint64_t q = 0; q < header->num_queries; q++) {        
        for (uint32_t n = 0; n < header->num_neighbors; n++) {
            int64_t neighbor_idx = ctx->ground_truth[q * header->num_neighbors + n];
            // build the reverse map
            if (neighbor_idx >= 0 && neighbor_idx < header->num_vectors) {
                query_vec_neighbors_t *qvn = &ctx->query_neighbors[neighbor_idx];
                for (int i = 0; i < MAX_QUERY_VEC_PER_NEIGHBORS; i++) {
                    if (qvn->neighbors[i] == 0) {
                        qvn->neighbors[i] = q + 1; // store query index + 1 to distinguish from empty
                        break;
                    }
                }
                qvn->neighbors[1]++; // count of queries that have this neighbor
                if (qvn->neighbors[1] > max_queries_per_neighbor) {
                    max_queries_per_neighbor = qvn->neighbors[1];
                }
            }
        }
    }
    
    printf("\n=== Dataset Loaded ===\n");
    printf("  Name: %s\n", header->dataset_name);
    printf("  File size: %.2f GB\n", (double)st.st_size / (1024*1024*1024));
    printf("  Vectors: %lu\n", (unsigned long)header->num_vectors);
    printf("  Queries: %lu\n", (unsigned long)header->num_queries);
    printf("  Dimensions: %u\n", header->dim);
    printf("  Neighbors (k): %u\n", header->num_neighbors);
    printf("  Distance metric: %s\n", distance_metric_names[header->distance_metric]);
    printf("  Data type: %s\n", dtype_names[header->dtype]);
    printf("  Max queries per neighbor: %d\n", max_queries_per_neighbor);
    
    if (header->has_metadata && ctx->vocabulary) {
        printf("  Metadata: Available\n");
        printf("    Vocabulary size: %u tags\n", header->vocab_size);
        if (ctx->vector_metadata) {
            printf("    Vector tags: %u (%.1f avg per vector)\n", 
                   ctx->vector_metadata->nnz,
                   (float)ctx->vector_metadata->nnz / header->num_vectors);
        }
        if (ctx->query_metadata) {
            printf("    Query predicates: %u (%.1f avg per query)\n", 
                   ctx->query_metadata->nnz,
                   (float)ctx->query_metadata->nnz / header->num_queries);
        }
        
        /* Show sample tags */
        if (header->vocab_size > 0) {
            printf("    Sample tags: ");
            uint32_t sample_count = (header->vocab_size < 10) ? header->vocab_size : 10;
            for (uint32_t i = 0; i < sample_count; i++) {
                printf("%s%s", i > 0 ? ", " : "", ctx->vocabulary->words[i]);
            }
            if (header->vocab_size > 10) {
                printf(" ... (%u more)", header->vocab_size - 10);
            }
            printf("\n");
        }
    } else {
        printf("  Metadata: Not available\n");
    }
    printf("======================\n\n");

    return ctx;
}

uint64_t datasetGetQueryIxByNeighbor(dataset_ctx_t *ctx, uint64_t neighbor_index, uint32_t ix) {
    if (!ctx || neighbor_index >= ctx->header->num_vectors || ix > MAX_QUERY_VEC_PER_NEIGHBORS) {
        assert(0);
        return (uint64_t)-1;
    }
    uint64_t qix = ctx->query_neighbors[neighbor_index].neighbors[ix];
    if (qix == 0) return (uint64_t)-1;
    return qix - 1; // stored as index + 1
}

float datasetGetDistanceFromQueryVector(dataset_ctx_t *ctx, uint64_t query_index, uint64_t returned_neighbor_index) {
    if (!ctx || query_index >= ctx->header->num_queries || returned_neighbor_index >= ctx->header->num_vectors) {
        assert(0);
        return -1.0f;
    }
    const float *src = ctx->vectors + (returned_neighbor_index * ctx->header->dim);
    const float *query_vec = ctx->queries + (query_index * ctx->header->dim);
    return calculateDistance(query_vec, src, ctx->header->dim, distance_metric_names[ctx->header->distance_metric]);
}

int datasetGetVector(dataset_ctx_t *ctx, uint64_t index,
                    uint64_t *id_out, float *vec_out) {
    assert(ctx);
    assert(index < ctx->header->num_vectors);

    /* ID is the index itself (can extend later) */
    *id_out = index;

    /* Copy vector - cache-friendly sequential access */
    if (vec_out) {
        const uint32_t dim = ctx->header->dim;
        const float *src = ctx->vectors + (index * dim);
        memcpy(vec_out, src, dim * sizeof(float));
    }

    return 0;
}

int datasetSetQueryVec(dataset_ctx_t *ctx, uint64_t query_index,
                  float *query_vec_out) {
    if (!ctx || query_index >= ctx->header->num_queries) {
        return -1;
    }

    const uint32_t dim = ctx->header->dim;
    // const uint32_t num_neighbors = ctx->header->num_neighbors;

    /* Copy query vector */
    const float *query_src = ctx->queries + (query_index * dim);
    memcpy(query_vec_out, query_src, dim * sizeof(float));

    return 0;
}

int dataset_get_info(dataset_ctx_t *ctx, dataset_info_t *info) {
    if (!ctx || !info) return -1;

    snprintf(info->distance_metric, sizeof(info->distance_metric), "%s", distance_metric_names[ctx->header->distance_metric]);
    snprintf(info->dtype, sizeof(info->dtype), "%s", dtype_names[ctx->header->dtype]);
    info->dim = ctx->header->dim;
    info->num_vectors = ctx->header->num_vectors;
    info->num_queries = ctx->header->num_queries;
    info->num_neighbors = ctx->header->num_neighbors;

    return 0;
}

dataset_neighbors_t* datasetGetNeighbors(dataset_ctx_t *ctx, uint64_t query_index) {
    if (!ctx || query_index >= ctx->header->num_queries) return NULL;
    assert(ctx->neighbors);
    return &ctx->neighbors[query_index];
}

void dataset_destroy(dataset_ctx_t *ctx) {
    if (!ctx) return;

    /* Free metadata structures */
    if (ctx->inverted_index && ctx->vector_metadata) {
        for (uint32_t i = 0; i < ctx->vector_metadata->ncol; i++) {
            if (ctx->inverted_index[i]) {
                free(ctx->inverted_index[i]);
            }
        }
        free(ctx->inverted_index);
    }
    
    if (ctx->vocabulary) {
        free(ctx->vocabulary->words);
        free(ctx->vocabulary);
    }
    
    if (ctx->vector_metadata) free(ctx->vector_metadata);
    if (ctx->query_metadata) free(ctx->query_metadata);
    
    /* Free neighbors */
    if (ctx->neighbors) {
        for (uint64_t i = 0; i < ctx->header->num_queries; i++) {
            if (ctx->neighbors[i].ids) free(ctx->neighbors[i].ids);
            if (ctx->neighbors[i].dists) free(ctx->neighbors[i].dists);
        }
        free(ctx->neighbors);
    }
    
    if (ctx->query_neighbors) free(ctx->query_neighbors);

    if (ctx->mmap_base) munmap(ctx->mmap_base, ctx->mmap_size);
    if (ctx->fd >= 0) close(ctx->fd);
    free(ctx);
}

/* NEW: Metadata API implementations */

dataset_tagset_t* dataset_get_vector_tags(dataset_ctx_t *ctx, uint64_t vec_id) {
    if (!ctx || !ctx->vector_metadata || vec_id >= ctx->header->num_vectors) {
        return NULL;
    }
    
    dataset_tagset_t *tagset = malloc(sizeof(dataset_tagset_t));
    if (!tagset) return NULL;
    
    uint32_t start = ctx->vector_metadata->indptr[vec_id];
    uint32_t end = ctx->vector_metadata->indptr[vec_id + 1];
    tagset->count = end - start;
    
    if (tagset->count > 0) {
        tagset->tag_ids = malloc(tagset->count * sizeof(uint32_t));
        if (!tagset->tag_ids) {
            free(tagset);
            return NULL;
        }
        memcpy(tagset->tag_ids, &ctx->vector_metadata->indices[start], 
               tagset->count * sizeof(uint32_t));
    } else {
        tagset->tag_ids = NULL;
    }
    
    return tagset;
}

dataset_tagset_t* dataset_get_query_predicates(dataset_ctx_t *ctx, uint64_t query_id) {
    if (!ctx || !ctx->query_metadata || query_id >= ctx->header->num_queries) {
        return NULL;
    }
    
    dataset_tagset_t *tagset = malloc(sizeof(dataset_tagset_t));
    if (!tagset) return NULL;
    
    uint32_t start = ctx->query_metadata->indptr[query_id];
    uint32_t end = ctx->query_metadata->indptr[query_id + 1];
    tagset->count = end - start;
    
    if (tagset->count > 0) {
        tagset->tag_ids = malloc(tagset->count * sizeof(uint32_t));
        if (!tagset->tag_ids) {
            free(tagset);
            return NULL;
        }
        memcpy(tagset->tag_ids, &ctx->query_metadata->indices[start], 
               tagset->count * sizeof(uint32_t));
    } else {
        tagset->tag_ids = NULL;
    }
    
    return tagset;
}

int dataset_vector_matches_predicates(dataset_ctx_t *ctx, uint64_t vec_id, uint64_t query_id) {
    if (!ctx || !ctx->vector_metadata || !ctx->query_metadata) {
        return 1;  /* No metadata = match all */
    }
    
    /* Get query predicates */
    uint32_t q_start = ctx->query_metadata->indptr[query_id];
    uint32_t q_end = ctx->query_metadata->indptr[query_id + 1];
    
    if (q_start == q_end) return 1;  /* No predicates = match all */
    
    /* Get vector tags */
    uint32_t v_start = ctx->vector_metadata->indptr[vec_id];
    uint32_t v_end = ctx->vector_metadata->indptr[vec_id + 1];
    
    /* Check if all query predicates are in vector tags */
    for (uint32_t qi = q_start; qi < q_end; qi++) {
        uint32_t pred = ctx->query_metadata->indices[qi];
        int found = 0;
        
        for (uint32_t vi = v_start; vi < v_end; vi++) {
            if (ctx->vector_metadata->indices[vi] == pred) {
                found = 1;
                break;
            }
        }
        
        if (!found) return 0;  /* Predicate not found */
    }
    
    return 1;  /* All predicates found */
}

uint64_t* dataset_get_matching_vectors(dataset_ctx_t *ctx, uint64_t query_id, uint64_t *count_out) {
    if (!ctx || !ctx->query_metadata) {
        /* No metadata = return all vectors */
        *count_out = ctx->header->num_vectors;
        uint64_t *all = malloc(*count_out * sizeof(uint64_t));
        if (all) {
            for (uint64_t i = 0; i < *count_out; i++) all[i] = i;
        }
        return all;
    }
    
    /* Get query predicates */
    uint32_t start = ctx->query_metadata->indptr[query_id];
    uint32_t end = ctx->query_metadata->indptr[query_id + 1];
    uint32_t num_predicates = end - start;
    
    if (num_predicates == 0) {
        /* No predicates = match all */
        *count_out = ctx->header->num_vectors;
        uint64_t *all = malloc(*count_out * sizeof(uint64_t));
        if (all) {
            for (uint64_t i = 0; i < *count_out; i++) all[i] = i;
        }
        return all;
    }
    
    /* Get first predicate's matching vectors */
    uint32_t tag0 = ctx->query_metadata->indices[start];
    uint32_t *list0 = ctx->inverted_index[tag0];
    if (!list0) {
        *count_out = 0;
        return NULL;
    }
    uint32_t count0 = list0[0];
    
    if (num_predicates == 1) {
        /* Single predicate - return inverted index list */
        uint64_t *result = malloc(count0 * sizeof(uint64_t));
        if (result) {
            for (uint32_t i = 0; i < count0; i++) {
                result[i] = list0[i + 1];
            }
        }
        *count_out = count0;
        return result;
    }
    
    /* Multiple predicates - intersect lists */
    uint32_t tag1 = ctx->query_metadata->indices[start + 1];
    uint32_t *list1 = ctx->inverted_index[tag1];
    if (!list1) {
        *count_out = 0;
        return NULL;
    }
    uint32_t count1 = list1[0];
    
    /* Allocate result (max size is min of two lists) */
    uint32_t max_result = (count0 < count1) ? count0 : count1;
    uint64_t *result = malloc(max_result * sizeof(uint64_t));
    if (!result) {
        *count_out = 0;
        return NULL;
    }
    
    /* Simple intersection (assuming sorted lists for production, use hash for unsorted) */
    uint32_t i = 1, j = 1, k = 0;
    while (i <= count0 && j <= count1) {
        if (list0[i] == list1[j]) {
            result[k++] = list0[i];
            i++; j++;
        } else if (list0[i] < list1[j]) {
            i++;
        } else {
            j++;
        }
    }
    
    *count_out = k;
    return result;
}

const char* dataset_get_tag_name(dataset_ctx_t *ctx, uint32_t tag_id) {
    if (!ctx || !ctx->vocabulary || tag_id >= ctx->vocabulary->vocab_size) {
        return NULL;
    }
    return ctx->vocabulary->words[tag_id];
}

dataset_neighbors_t* dataset_get_filtered_neighbors(dataset_ctx_t *ctx, uint64_t query_index) {
    if (!ctx || query_index >= ctx->header->num_queries) {
        return NULL;
    }
    
    if (!ctx->query_metadata) {
        /* No metadata - return NULL to indicate filtering not available */
        return NULL;
    }
    
    /* Get matching vectors for this query */
    uint64_t match_count;
    uint64_t *matching = dataset_get_matching_vectors(ctx, query_index, &match_count);
    if (!matching) {
        /* No predicates for this query - return NULL */
        return NULL;
    }
    
    /* Create a simple set for O(1) lookup (for small sets) */
    /* For production, use hash table for large match_count */
    
    /* Filter ground truth neighbors */
    dataset_neighbors_t *original = &ctx->neighbors[query_index];
    dataset_neighbors_t *filtered = malloc(sizeof(dataset_neighbors_t));
    if (!filtered) {
        free(matching);
        return NULL;
    }
    
    filtered->ids = malloc(original->count * sizeof(uint64_t));
    filtered->dists = malloc(original->count * sizeof(float));
    if (!filtered->ids || !filtered->dists) {
        if (filtered->ids) free(filtered->ids);
        if (filtered->dists) free(filtered->dists);
        free(filtered);
        free(matching);
        return NULL;
    }
    filtered->count = 0;
    
    /* Filter neighbors */
    for (size_t i = 0; i < original->count; i++) {
        uint64_t neighbor_id = original->ids[i];
        
        /* Check if neighbor is in matching set (linear search for now) */
        int found = 0;
        for (uint64_t j = 0; j < match_count; j++) {
            if (matching[j] == neighbor_id) {
                found = 1;
                break;
            }
        }
        
        if (found) {
            filtered->ids[filtered->count] = neighbor_id;
            filtered->dists[filtered->count] = original->dists[i];
            filtered->count++;
        }
    }
    
    free(matching);
    return filtered;
}

void dataset_free_tagset(dataset_tagset_t *tagset) {
    if (tagset) {
        if (tagset->tag_ids) free(tagset->tag_ids);
        free(tagset);
    }
}

void dataset_free_neighbors(dataset_neighbors_t *neighbors) {
    if (neighbors) {
        if (neighbors->ids) free(neighbors->ids);
        if (neighbors->dists) free(neighbors->dists);
        free(neighbors);
    }
}