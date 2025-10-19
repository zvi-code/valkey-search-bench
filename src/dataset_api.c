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
        "./_datasets_prepared/%s.bin",  /* Local prepared */
        "./utils/datasets/%s.bin",      /* Utils location */
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
    /* Return metadata */
    if (info) {
        snprintf(info->distance_metric, sizeof(info->distance_metric), "%s", distance_metric_names[ctx->header->distance_metric]);
        snprintf(info->dtype, sizeof(info->dtype), "%s", dtype_names[ctx->header->dtype]);
        info->dim = header->dim;
        info->num_vectors = header->num_vectors;
        info->num_queries = header->num_queries;
        info->num_neighbors = header->num_neighbors;
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
    printf("Dataset loaded: %s (%.2f GB, %lu vecs, %u dims, dtype %s, distance %s, max queries per neighbor %d)\n",
            header->dataset_name, 
            (double)st.st_size / (1024*1024*1024),
            header->num_vectors, header->dim, 
            dtype_names[header->dtype], distance_metric_names[header->distance_metric], 
            max_queries_per_neighbor);

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

    if (ctx->mmap_base) munmap(ctx->mmap_base, ctx->mmap_size);
    if (ctx->fd >= 0) close(ctx->fd);
    free(ctx);
}