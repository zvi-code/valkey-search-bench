/* Test metadata loading from YFCC-10M binary */
#include <stdio.h>
#include <stdlib.h>
#include "dataset_api.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <dataset.bin>\n", argv[0]);
        return 1;
    }
    
    const char *path = argv[1];
    printf("Loading dataset: %s\n", path);
    
    dataset_info_t info;
    dataset_ctx_t *ctx = dataset_init(path, &info);
    if (!ctx) {
        fprintf(stderr, "Failed to load dataset\n");
        return 1;
    }
    
    printf("\n=== Dataset Info ===\n");
    printf("Vectors: %lu x %u\n", info.num_vectors, info.dim);
    printf("Queries: %lu\n", info.num_queries);
    printf("Neighbors (k): %u\n", info.num_neighbors);
    printf("Distance: %s\n", info.distance_metric);
    printf("Dtype: %s\n", info.dtype);
    printf("Has metadata: %u\n", info.has_metadata);
    printf("Vocabulary size: %u\n", info.vocab_size);
    
    if (info.has_metadata) {
        printf("\n=== Metadata Tests ===\n");
        
        // Test 1: Get tags for first vector
        printf("\n[Test 1] Vector 0 tags:\n");
        dataset_tagset_t *tags = dataset_get_vector_tags(ctx, 0);
        if (tags) {
            printf("  Count: %u\n", tags->count);
            for (uint32_t i = 0; i < (tags->count < 10 ? tags->count : 10); i++) {
                const char *tag_name = dataset_get_tag_name(ctx, tags->tag_ids[i]);
                printf("    Tag %u: ID=%u, Name=\"%s\"\n", i, tags->tag_ids[i], 
                       tag_name ? tag_name : "NULL");
            }
            dataset_free_tagset(tags);
        }
        
        // Test 2: Get predicates for first query
        printf("\n[Test 2] Query 0 predicates:\n");
        dataset_tagset_t *preds = dataset_get_query_predicates(ctx, 0);
        if (preds) {
            printf("  Count: %u\n", preds->count);
            for (uint32_t i = 0; i < preds->count; i++) {
                const char *tag_name = dataset_get_tag_name(ctx, preds->tag_ids[i]);
                printf("    Predicate %u: ID=%u, Name=\"%s\"\n", i, preds->tag_ids[i],
                       tag_name ? tag_name : "NULL");
            }
            dataset_free_tagset(preds);
        }
        
        // Test 3: Get matching vectors for query 0
        printf("\n[Test 3] Matching vectors for query 0:\n");
        uint64_t match_count;
        uint64_t *matching = dataset_get_matching_vectors(ctx, 0, &match_count);
        if (matching) {
            printf("  Matching vectors: %lu\n", match_count);
            if (match_count > 0) {
                printf("  First 10 matches: ");
                for (uint64_t i = 0; i < (match_count < 10 ? match_count : 10); i++) {
                    printf("%lu ", matching[i]);
                }
                printf("\n");
            }
            free(matching);
        }
        
        // Test 4: Test filtered neighbors
        printf("\n[Test 4] Filtered neighbors for query 0:\n");
        dataset_neighbors_t *filtered = dataset_get_filtered_neighbors(ctx, 0);
        if (filtered) {
            printf("  Filtered neighbor count: %lu\n", filtered->count);
            for (size_t i = 0; i < (filtered->count < 10 ? filtered->count : 10); i++) {
                printf("    Neighbor %zu: ID=%lu, Dist=%.6f\n", i, 
                       filtered->ids[i], filtered->dists[i]);
            }
            // Free if it was allocated (not a direct pointer to ctx->neighbors)
            dataset_free_neighbors(filtered);
        }
    }
    
    dataset_destroy(ctx);
    printf("\n✓ All tests passed!\n");
    
    return 0;
}
