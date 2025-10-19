/**
 * Vector ID to Cluster Tag Mapping Implementation
 *
 * This module implements vector ID mapping functionality using the generic
 * cluster scanner. It provides specialized key processing for vector keys
 * to extract vector IDs and their corresponding cluster tags.
 */

#include "vector-id-mapping.h"
#include "cluster-scan.h"
#include "zmalloc.h"
#include "progress-bar.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <assert.h>
#include <unistd.h>

void initClusterTagMap(clusterTagMap *tag_map, uint64_t initial_capacity) {
    memset(tag_map, 0, sizeof(clusterTagMap));
    tag_map->capacity = initial_capacity;
    tag_map->mappings = zcalloc(tag_map->capacity * sizeof(vectorClusterMapping));
    tag_map->progress_bar = NULL;
    pthread_mutex_init(&tag_map->mutex, NULL);
}

void addClusterTagMapping(clusterTagMap *tag_map, uint64_t vector_id, const char *cluster_tag) {
    if (!tag_map) return;
    /* Expand capacity if needed */
    assert(vector_id < tag_map->capacity);

    /* Always increment keys_scanned for progress tracking */
    tag_map->keys_scanned++;
    
    /* Update progress bar based on keys scanned */
    if (tag_map->progress_bar) {
        updateProgressBar(tag_map->progress_bar, tag_map->keys_scanned, 0);
    }

    if (tag_map->mappings[vector_id].cluster_tag[0] == '\0') {
        if (tag_map->count > tag_map->capacity) {
            fprintf(stderr, "Error: tag_map count %lu exceeds capacity %lu for vector_id %lu cluster_tag %s\n",
                    tag_map->count, tag_map->capacity, vector_id, cluster_tag);
            fflush(stderr);
            assert(0);
        }
        if (tag_map->is_cluster_mode_enabled) {
            assert(cluster_tag[0] == '{');
        } else {
            // set dummy {CMD} tag for non-cluster mode
            memcpy(tag_map->mappings[vector_id].cluster_tag, "{CMD}", 5);
            tag_map->mappings[vector_id].cluster_tag[5] = '\0';
        }

        /* New entry */
        assert(tag_map->count <= tag_map->capacity);
        tag_map->count++;
    }
    if (tag_map->is_cluster_mode_enabled) {
        /* Add new mapping */
        memcpy(tag_map->mappings[vector_id].cluster_tag, cluster_tag, 5);
        tag_map->mappings[vector_id].cluster_tag[5] = '\0';
    }

}

// void addClusterTagMapping(clusterTagMap *tag_map, uint64_t vector_id, const char *cluster_tag) {
//     if (!tag_map || !cluster_tag) return;
//     assert(vector_id < 1000000000);  // Prevent overflow on doubling capacity
//     pthread_mutex_lock(&tag_map->mutex);

//     /* Expand capacity if needed */
//     if (2 * vector_id >= tag_map->capacity) {
//         tag_map->mappings = zrealloc(tag_map->mappings,
//                                    2 * vector_id * sizeof(vectorClusterMapping));
//         memset(&tag_map->mappings[tag_map->capacity], 0,
//                (2 * vector_id - tag_map->capacity) * sizeof(vectorClusterMapping));
//         tag_map->capacity = 2 * vector_id;
//     }
//     if (tag_map->mappings[vector_id].cluster_tag[0] == '\0') {
//         if (tag_map->count > tag_map->capacity) {
//             fprintf(stderr, "Error: tag_map count %lu exceeds capacity %lu for vector_id %lu cluster_tag %s\n",
//                     tag_map->count, tag_map->capacity, vector_id, cluster_tag);
//             fflush(stderr);
//             assert(0);
//         }
//         assert(cluster_tag[0] == '{');
//         /* New entry */
//         assert(tag_map->count <= tag_map->capacity);
//         tag_map->count++;
//         if (tag_map->count % 10000 == 0) {
//             printf("[VECTOR-MAPPING] Added %lu mappings, current capacity %lu\n",
//                    tag_map->count, tag_map->capacity);
//         }
//     }
//     pthread_mutex_unlock(&tag_map->mutex);
    
//     /* Add new mapping */
//     memcpy(tag_map->mappings[vector_id].cluster_tag, cluster_tag, 5);
//     tag_map->mappings[vector_id].cluster_tag[5] = '\0';

// }

const char* getClusterTagForVector(clusterTagMap *tag_map, uint64_t vector_id) {
    if (!tag_map) return NULL;
    if (vector_id >= tag_map->capacity) return NULL;
    if (!tag_map->is_cluster_mode_enabled) return NULL;
    if (tag_map->mappings[vector_id].cluster_tag[0] == '\0') return NULL;
    assert(tag_map->mappings[vector_id].cluster_tag[0] == '{');
    return tag_map->mappings[vector_id].cluster_tag;
}

int checkVectorExistsInCluster(clusterTagMap *tag_map, uint64_t vector_id) {
    if (!tag_map) return 0;
    if (vector_id >= tag_map->capacity) return 0;
    if (tag_map->mappings[vector_id].cluster_tag[0] == '\0') return 0;
    assert(tag_map->mappings[vector_id].cluster_tag[0] == '{');
    return 1;
}

void vectorMappingProgressCallback(uint64_t keys_processed, int active_threads, void *user_data) {
    /* Progress is now handled by the progress bar in addClusterTagMapping */
    (void)keys_processed;
    (void)active_threads;
    (void)user_data;
}

int buildVectorIdMappings(int is_cluster_mode_enabled, const char *prefix,
                         struct clusterNode **nodes,
                         int node_count,
                         clusterTagMap *tag_map,
                        keyProcessorCallback key_processor) {
    if (!prefix || !nodes || !tag_map) {
        return -1;
    }

    /* Create scan pattern: prefix* */
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "%s*", prefix);
    tag_map->prefix = zstrdup(prefix);
    tag_map->prefix_len = strlen(prefix);
    tag_map->is_cluster_mode_enabled = is_cluster_mode_enabled;
    
    /* Initialize progress bar */
    progressBar progress;
    tag_map->progress_bar = &progress;
    initProgressBar(&progress, tag_map->capacity, 0, "Building vector ID mappings");
    
    /* Configure cluster scan */
    clusterScanConfig scan_config;
    initClusterScanConfig(&scan_config, pattern, nodes, node_count,
                         key_processor, tag_map);

    /* Enable silent mode to avoid interfering with progress bar */
    scan_config.silent_mode = 1;

    /* Set performance parameters for vector scanning */
    setClusterScanPerformance(&scan_config, 1000, node_count, 50000);
    setClusterScanProgressCallback(&scan_config, vectorMappingProgressCallback);

    /* Execute the scan */
    clusterScanResults results;

    int scan_result = executeClusterScan(&scan_config, &results);
    
    /* Update progress bar to show actual total scanned */
    if (progress.enabled) {
        pthread_mutex_lock(&progress.mutex);
        progress.total = results.total_keys_processed;
        progress.current = results.total_keys_processed;
        pthread_mutex_unlock(&progress.mutex);
    }
    
    /* Finish progress bar - will show 100% with actual totals */
    finishProgressBar(&progress);
    tag_map->progress_bar = NULL;
    
    /* Ensure all output is flushed and terminal is in clean state */
    fflush(stdout);
    fflush(stderr);
    /* Small delay to ensure terminal processes the output */
    usleep(10000); /* 10ms = 10,000 microseconds */

    if (scan_result == 0) {
        printf("[VECTOR-MAPPING] Successfully built %lu vector ID mappings\n", tag_map->count);
        printf("[VECTOR-MAPPING] Scan completed: %lu keys in %.2f seconds (%.1f keys/sec)\n",
               results.total_keys_processed,
               results.total_scan_time_ms / 1000.0,
               results.keys_per_second);
    } else {
        printf("[VECTOR-MAPPING] Scan failed with error code %d\n", scan_result);
    }

    return scan_result;
}

void cleanupClusterTagMap(clusterTagMap *tag_map) {
    if (!tag_map) return;

    if (tag_map->mappings) {
        zfree(tag_map->mappings);
        tag_map->mappings = NULL;
    }
    pthread_mutex_destroy(&tag_map->mutex);
    memset(tag_map, 0, sizeof(clusterTagMap));
}

size_t getClusterTagMapCount(clusterTagMap *tag_map) {
    if (!tag_map) return 0;
    return tag_map->count;
}