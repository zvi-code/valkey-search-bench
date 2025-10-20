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

#ifndef DATASET_API_H
#define DATASET_API_H

#include <stdint.h>
#include <stddef.h>

#define DATASET_MAGIC 0xDECDB001
#define DATASET_VERSION 1
#define DATASET_VERSION_METADATA 2

/* Distance metrics */
typedef enum {
    DISTANCE_L2 = 0,
    DISTANCE_COSINE = 1,
    DISTANCE_IP = 2
} distance_metric_t;

/* Data types */
typedef enum {
    DTYPE_FLOAT32 = 0,
    DTYPE_FLOAT16 = 1
} dtype_t;

/* Dataset metadata */
typedef struct {
    char distance_metric[32];
    char dtype[32];
    uint32_t dim;
    uint64_t num_vectors;
    uint64_t num_queries;
    uint32_t num_neighbors;
    uint32_t vocab_size;        /* Number of unique tags (0 if no metadata) */
    uint32_t has_metadata;      /* 0=no metadata, 1=metadata present */
} dataset_info_t;

/* Metadata sparse matrix (CSR format) - mmap-friendly */
typedef struct {
    uint32_t nrow;           /* Number of rows (vectors or queries) */
    uint32_t ncol;           /* Vocabulary size */
    uint32_t nnz;            /* Number of non-zero entries */
    uint32_t *indptr;        /* Row pointers [nrow+1] - mmap'd */
    uint32_t *indices;       /* Column indices (tag IDs) [nnz] - mmap'd */
    float *data;             /* Values [nnz] - mmap'd (typically all 1.0) */
} dataset_metadata_csr_t;

/* Tag set for filtering */
typedef struct {
    uint32_t *tag_ids;       /* Array of tag IDs */
    uint32_t count;          /* Number of tags */
} dataset_tagset_t;

/* Vocabulary */
typedef struct {
    uint32_t vocab_size;
    char **words;            /* Array of vocab_size strings */
    void *string_data;       /* Pointer to mmap'd string data */
} dataset_vocabulary_t;

typedef struct {
    uint64_t* ids; // array of vector IDs
    float* dists; // array of distances
    size_t count; // number of results
} dataset_neighbors_t;

/* 4KB-aligned header for cache efficiency */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    char dataset_name[256];
    uint8_t distance_metric;
    uint8_t dtype;
    uint8_t has_metadata;        /* NEW: 0=no metadata, 1=metadata present */
    uint8_t padding[1];
    uint32_t dim;
    uint64_t num_vectors;
    uint64_t num_queries;
    uint32_t num_neighbors;
    uint32_t vocab_size;         /* NEW: Size of tag vocabulary (0 if no metadata) */
    
    /* Existing offsets */
    uint64_t vectors_offset;
    uint64_t queries_offset;
    uint64_t ground_truth_offset;
    
    /* NEW: Metadata offsets (0 if no metadata) */
    uint64_t vector_metadata_offset;
    uint64_t query_metadata_offset;
    uint64_t vocab_offset;
    uint64_t gt_metadata_offset;     /* Reserved for future use */
    
    uint8_t reserved[3744];  /* Pad to exactly 4096 bytes (352 + 3744 = 4096) */
} dataset_header_t;

/* Opaque context handle */
typedef struct dataset_ctx dataset_ctx_t;

/* API Functions */
dataset_ctx_t* dataset_init(const char *dataset_name, dataset_info_t *info);
int datasetGetVector(dataset_ctx_t *ctx, uint64_t index, uint64_t *id_out, float *vec_out);
int datasetSetQueryVec(dataset_ctx_t *ctx, uint64_t query_index, float *query_vec_out);
dataset_neighbors_t* datasetGetNeighbors(dataset_ctx_t *ctx, uint64_t query_index);
int dataset_get_info(dataset_ctx_t *ctx, dataset_info_t *info);
void dataset_destroy(dataset_ctx_t *ctx);
uint64_t datasetGetQueryIxByNeighbor(dataset_ctx_t *ctx, uint64_t neighbor_index, uint32_t ix);
float calculateDistance(const float *vec1, const float *vec2, int dim, const char *metric);
float datasetGetDistanceFromQueryVector(dataset_ctx_t *ctx, uint64_t query_index, uint64_t returned_neighbor_index);

/* NEW: Metadata API Functions */
/* Get tags for a vector */
dataset_tagset_t* dataset_get_vector_tags(dataset_ctx_t *ctx, uint64_t vec_id);

/* Get predicates for a query */
dataset_tagset_t* dataset_get_query_predicates(dataset_ctx_t *ctx, uint64_t query_id);

/* Check if vector matches query predicates */
int dataset_vector_matches_predicates(dataset_ctx_t *ctx, uint64_t vec_id, uint64_t query_id);

/* Get all vectors matching query predicates (using inverted index) */
uint64_t* dataset_get_matching_vectors(dataset_ctx_t *ctx, uint64_t query_id, uint64_t *count_out);

/* Get tag name from vocabulary */
const char* dataset_get_tag_name(dataset_ctx_t *ctx, uint32_t tag_id);

/* Get filtered ground truth (only neighbors matching predicates)
 * Returns newly allocated dataset_neighbors_t* if filtering succeeded,
 * or NULL if no metadata/predicates available.
 * Caller must free with dataset_free_neighbors() if non-NULL. */
dataset_neighbors_t* dataset_get_filtered_neighbors(dataset_ctx_t *ctx, uint64_t query_index);

/* Free tag set */
void dataset_free_tagset(dataset_tagset_t *tagset);

/* Free filtered neighbors */
void dataset_free_neighbors(dataset_neighbors_t *neighbors);

#endif /* DATASET_API_H */