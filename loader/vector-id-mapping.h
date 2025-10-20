/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2024-present, Zvi Schneider
 * 
 * Vector ID to Cluster Tag Mapping - Header
 *
 * This file is part of valkey-search-benchmark and is licensed under the
 * BSD 3-Clause License. See the LICENSE file in the root directory.
 */

#ifndef VECTOR_ID_MAPPING_H
#define VECTOR_ID_MAPPING_H

#include "cluster-scan.h"
#include <stdint.h>

/* Forward declaration to avoid circular dependency */
struct progressBar;

/**
 * Vector ID to Cluster Tag Mapping Module
 *
 * This module provides specialized functionality for building and managing
 * vector ID to cluster tag mappings using the generic cluster scanner.
 * It's designed specifically for recall validation in dataset benchmarking.
 */

/* Vector ID to cluster tag mapping entry */
typedef struct {
    uint64_t vector_id;
    char cluster_tag[6];  /* Up to 5 chars + null terminator */
} vectorClusterMapping;

/* Thread-safe cluster tag mapping table */
typedef struct {
    char* prefix;  // Padding for cache alignment
    int prefix_len; // Length of the prefix
    vectorClusterMapping *mappings;
    uint64_t capacity;
    uint64_t count;
    uint64_t keys_scanned;  /* Number of keys scanned (for progress tracking) */
    int is_cluster_mode_enabled;
    pthread_mutex_t mutex;
    struct progressBar *progress_bar;  /* Progress bar for visual feedback */
} clusterTagMap;

/**
 * Initialize the cluster tag mapping table
 * @param tag_map Pointer to mapping table to initialize
 * @param initial_capacity Initial capacity for mappings
 */
void initClusterTagMap(clusterTagMap *tag_map, uint64_t initial_capacity);

/**
 * Add a vector ID to cluster tag mapping
 * @param tag_map Mapping table
 * @param vector_id Vector identifier
 * @param cluster_tag Cluster tag string
 */
void addClusterTagMapping(clusterTagMap *tag_map, uint64_t vector_id, const char *cluster_tag);

/**
 * Get cluster tag for a vector ID
 * @param tag_map Mapping table
 * @param vector_id Vector identifier
 * @return Cluster tag string or NULL if not found
 */
const char* getClusterTagForVector(clusterTagMap *tag_map, uint64_t vector_id);
int checkVectorExistsInCluster(clusterTagMap *tag_map, uint64_t vector_id);
/**
 * Build vector ID mappings by scanning cluster for vector keys
 * @param prefix Vector key prefix to scan for
 * @param nodes Array of cluster nodes
 * @param node_count Number of cluster nodes
 * @param tag_map Output mapping table
 * @return 0 on success, negative error code on failure
 */
int buildVectorIdMappings(int is_cluster_mode_enabled, const char *prefix,
                         struct clusterNode **nodes,
                         int node_count,
                         clusterTagMap *tag_map,
                        keyProcessorCallback key_processor);

size_t getClusterTagMapCount(clusterTagMap *tag_map);
/**
 * Progress callback for vector mapping scan
 * @param keys_processed Number of keys processed so far
 * @param active_threads Number of active worker threads
 * @param user_data User data (unused)
 */
void vectorMappingProgressCallback(uint64_t keys_processed, int active_threads, void *user_data);

/**
 * Cleanup cluster tag mapping table
 * @param tag_map Mapping table to cleanup
 */
void cleanupClusterTagMap(clusterTagMap *tag_map);

#endif /* VECTOR_ID_MAPPING_H */