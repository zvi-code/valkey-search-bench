#ifndef DATASET_API_H
#define DATASET_API_H

#include <stdint.h>
#include <stddef.h>

#define DATASET_MAGIC 0xDECDB001
#define DATASET_VERSION 1

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
} dataset_info_t;

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
    uint8_t padding[2];
    uint32_t dim;
    uint64_t num_vectors;
    uint64_t num_queries;
    uint32_t num_neighbors;
    uint8_t padding2[4];
    uint64_t vectors_offset;
    uint64_t queries_offset;
    uint64_t ground_truth_offset;
    uint8_t reserved[3808];  /* Pad to 4KB */
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
#endif /* DATASET_API_H */