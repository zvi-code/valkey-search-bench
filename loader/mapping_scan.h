/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2024-present, Zvi Schneider
 * 
 * Generic Parallel Cluster Scanner - Header
 *
 * This file is part of valkey-search-benchmark and is licensed under the
 * BSD 3-Clause License. See the LICENSE file in the root directory.
 */

#ifndef CLUSTER_SCAN_H
#define CLUSTER_SCAN_H

#include <stdint.h>
#include <pthread.h>

/* Forward declarations */
struct clusterNode;
typedef struct valkeyContext valkeyContext;
typedef struct valkeyReply valkeyReply;

/**
 * Generic Parallel Cluster Scanner
 *
 * OVERVIEW:
 * This module provides a generic, parallel cluster scanning framework for Redis clusters.
 * It can be used for various purposes such as:
 * - Building vector ID to cluster tag mappings for recall validation
 * - Extending existing datasets with cluster-wide key discovery
 * - Data migration and analysis operations
 * - Key pattern analysis and statistics collection
 *
 * DESIGN PRINCIPLES:
 * 1. Generic callback-based architecture for flexible key processing
 * 2. Parallel execution across cluster nodes for performance
 * 3. Configurable scan parameters (batch size, concurrency, filters)
 * 4. Thread-safe operation with proper synchronization
 * 5. Modular components that can be composed for different use cases
 *
 * IMPLEMENTATION PLAN:
 * 1. Core cluster scanning engine with configurable callbacks
 * 2. Per-node scan workers running in parallel threads
 * 3. SCAN command execution with MATCH pattern filtering
 * 4. Thread-safe result aggregation via user-provided callbacks
 * 5. Progress reporting and error handling
 * 6. Resource cleanup and connection management
 *
 * USAGE PATTERNS:
 * - Vector ID mapping: Scan keys matching "prefix*", extract vector IDs and cluster tags
 * - Dataset extension: Discover existing keys to avoid duplicates during insertion
 * - Key analysis: Count keys per pattern, analyze distribution, etc.
 * - Data validation: Verify key existence and format consistency
 *
 * PERFORMANCE CHARACTERISTICS:
 * - Expected throughput: 100K+ keys/second scanning rate
 * - Scales with cluster size (more nodes = more parallel workers)
 * - Memory efficient streaming processing (no need to store all keys)
 * - Configurable concurrency to balance performance vs resource usage
 */

/* Key processing callback function type */
typedef int64_t (*keyProcessorCallback)(const char *key, void *user_data, int64_t thread_id);

/* Scan progress callback function type */
typedef void (*scanProgressCallback)(uint64_t keys_processed, int64_t active_threads, void *user_data);

/* Connection factory callback - creates a valkeyContext for a given node */
typedef valkeyContext* (*connectionFactoryCallback)(struct clusterNode *node);

/* Per-node scan worker configuration */
typedef struct {
    struct clusterNode *node;     /* Target cluster node */
    valkeyContext *context;       /* Redis connection context */
    const char *match_pattern;    /* SCAN MATCH pattern */
    int64_t scan_batch_size;         /* Keys per SCAN call */
    keyProcessorCallback processor; /* Key processing function */
    void *user_data;             /* User data for callbacks */
    int64_t thread_id;               /* Worker thread identifier */

    /* Thread synchronization */
    pthread_t thread;
    pthread_mutex_t *progress_mutex;
    uint64_t *total_keys_processed;
    int64_t *active_threads;
    int64_t *error_occurred;
} scanWorker;

/* Main cluster scan configuration */
typedef struct {
    /* Input parameters */
    const char *match_pattern;      /* Key pattern to match (e.g., "prefix*") */
    struct clusterNode **nodes;     /* Array of cluster nodes to scan */
    int64_t node_count;                /* Number of nodes */
    int64_t scan_batch_size;           /* SCAN batch size (default: 1000) */
    int64_t max_concurrent_workers;    /* Max parallel workers (default: node_count) */
    int64_t silent_mode;               /* Suppress [SCAN] output messages (for progress bars) */

    /* Connection factory - creates connections with proper TLS/auth if needed */
    connectionFactoryCallback connection_factory; /* NULL = use default valkeyConnect */

    /* Callback functions */
    keyProcessorCallback key_processor;     /* Process each discovered key */
    scanProgressCallback progress_callback; /* Progress reporting (optional) */
    void *user_data;                       /* User data passed to callbacks */

    /* Progress tracking */
    uint64_t total_keys_processed;
    int64_t progress_report_interval;          /* Report progress every N keys */
} clusterScanConfig;

/* Scan operation results */
typedef struct {
    uint64_t total_keys_processed;
    uint64_t total_scan_time_ms;
    int64_t nodes_scanned;
    int64_t errors_encountered;
    double keys_per_second;
} clusterScanResults;

/**
 * Initialize cluster scan configuration with defaults
 * @param config Configuration structure to initialize
 * @param match_pattern Key pattern to scan for (e.g., "prefix*")
 * @param nodes Array of cluster nodes
 * @param node_count Number of nodes in array
 * @param key_processor Callback function to process each key
 * @param user_data User data passed to callbacks
 */
void initClusterScanConfig(clusterScanConfig *config,
                          const char *match_pattern,
                          struct clusterNode **nodes,
                          int64_t node_count,
                          keyProcessorCallback key_processor,
                          void *user_data);

/**
 * Execute parallel cluster scan operation
 * @param config Scan configuration
 * @param results Output results structure (optional)
 * @return 0 on success, negative error code on failure
 */
int executeClusterScan(clusterScanConfig *config, clusterScanResults *results);

/**
 * Set scan performance parameters
 * @param config Configuration to modify
 * @param batch_size Keys per SCAN call
 * @param max_workers Maximum concurrent workers
 * @param progress_interval Progress report interval
 */
void setClusterScanPerformance(clusterScanConfig *config,
                              int64_t batch_size,
                              int64_t max_workers,
                              int64_t progress_interval);

/**
 * Set progress callback for scan monitoring
 * @param config Configuration to modify
 * @param progress_callback Progress reporting function
 */
void setClusterScanProgressCallback(clusterScanConfig *config,
                                   scanProgressCallback progress_callback);

/**
 * Set connection factory for creating node connections
 * This allows the caller to provide TLS-enabled connections
 * @param config Configuration to modify
 * @param factory Connection factory function (NULL = use default valkeyConnect)
 */
void setClusterScanConnectionFactory(clusterScanConfig *config,
                                     connectionFactoryCallback factory);

#endif /* CLUSTER_SCAN_H */