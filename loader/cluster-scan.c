/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2024-present, Zvi Schneider
 * 
 * Generic Parallel Cluster Scanner Implementation
 *
 * This module implements a high-performance, parallel cluster scanning framework
 * for Redis clusters. It provides a generic, callback-based architecture that
 * can be used for various cluster-wide operations.
 *
 * This file is part of valkey-search-benchmark and is licensed under the
 * BSD 3-Clause License. See the LICENSE file in the root directory.
 */

/**
 * DETAILED IMPLEMENTATION PLAN:
 *
 * 1. CORE SCANNING ENGINE:
 *    - Multi-threaded worker pool with one worker per cluster node
 *    - Each worker executes SCAN commands with MATCH pattern filtering
 *    - Streaming key processing via user-provided callbacks
 *    - Thread-safe progress tracking and error handling
 *
 * 2. WORKER THREAD ARCHITECTURE:
 *    - Per-node connection management for parallel execution
 *    - SCAN cursor management for complete key coverage
 *    - Batch processing with configurable SCAN COUNT parameter
 *    - Graceful error handling and connection recovery
 *
 * 3. SYNCHRONIZATION AND SAFETY:
 *    - Mutex-protected shared counters for progress tracking
 *    - Atomic error flag for early termination
 *    - Thread-safe callback execution with user data isolation
 *    - Proper resource cleanup on completion or failure
 *
 * 4. PERFORMANCE OPTIMIZATIONS:
 *    - Efficient SCAN command batching (default 1000 keys/batch)
 *    - Parallel node scanning for maximum throughput
 *    - Minimal memory footprint with streaming processing
 *    - Configurable concurrency for resource management
 *
 * 5. USE CASE SPECIALIZATIONS:
 *    - Vector ID mapping: Extract vector IDs and cluster tags from keys
 *    - Dataset discovery: Build lists of existing keys for deduplication
 *    - Pattern analysis: Count and categorize keys by patterns
 *    - Data validation: Verify key format and existence
 *
 * 6. ERROR HANDLING AND RECOVERY:
 *    - Connection failure detection and reporting
 *    - Partial scan results with error status
 *    - Resource cleanup on early termination
 *    - Detailed error reporting for debugging
 *
 * PERFORMANCE EXPECTATIONS:
 * - 100K+ keys/second throughput on typical cluster setups
 * - Linear scaling with cluster node count
 * - Memory usage: O(workers) + user callback storage
 * - Network efficiency: Batched SCAN commands reduce round trips
 */

#include "cluster-scan.h"
#include "valkey-benchmark-utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>
#include <valkey/valkey.h>
#include "zmalloc.h"

/* Default configuration values */
#define DEFAULT_SCAN_BATCH_SIZE 1000
#define DEFAULT_PROGRESS_INTERVAL 10000

/* Error codes */
#define SCAN_ERROR_CONNECTION -1
#define SCAN_ERROR_COMMAND -2
#define SCAN_ERROR_CALLBACK -3
#define SCAN_ERROR_THREAD -4

/**
 * Get current timestamp in milliseconds
 */
static uint64_t getCurrentTimeMs(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

/**
 * Worker thread function for scanning a single cluster node
 */
static void* scanWorkerThread(void *arg) {
    scanWorker *worker = (scanWorker*)arg;
    uint64_t cursor = 0;
    uint64_t keys_processed_by_worker = 0;
    int error_occurred = 0;

    do {
        /* Execute SCAN command with pattern matching */
        valkeyReply *reply = valkeyCommand(worker->context,
            "SCAN %lu MATCH %s COUNT %d",
            cursor, worker->match_pattern, worker->scan_batch_size);

        if (!reply) {
            fprintf(stderr, "[SCAN] Worker %d: Connection error to node\n", worker->thread_id);
            error_occurred = 1;
            break;
        }

        if (reply->type != VALKEY_REPLY_ARRAY || reply->elements != 2) {
            fprintf(stderr, "[SCAN] Worker %d: Invalid SCAN response format\n", worker->thread_id);
            freeReplyObject(reply);
            error_occurred = 1;
            break;
        }

        /* Extract new cursor */
        if (reply->element[0]->type == VALKEY_REPLY_STRING) {
            cursor = strtoull(reply->element[0]->str, NULL, 10);
        } else {
            cursor = reply->element[0]->integer;
        }

        /* Process returned keys */
        valkeyReply *keys_array = reply->element[1];
        if (keys_array->type == VALKEY_REPLY_ARRAY) {
            for (size_t i = 0; i < keys_array->elements; i++) {
                valkeyReply *key_reply = keys_array->element[i];
                if (key_reply->type == VALKEY_REPLY_STRING) {
                    /* Call user's key processor callback */
                    int callback_result = worker->processor(key_reply->str,
                                                          worker->user_data,
                                                          worker->thread_id);
                    if (callback_result != 0) {
                        fprintf(stderr, "[SCAN] Worker %d: Key processor callback failed\n",
                               worker->thread_id);
                        error_occurred = 1;
                        freeReplyObject(reply);
                        goto cleanup;
                    }
                    keys_processed_by_worker++;
                }
            }
        }

        freeReplyObject(reply);

        /* Update shared progress counters */
        pthread_mutex_lock(worker->progress_mutex);
        *worker->total_keys_processed += keys_processed_by_worker;
        keys_processed_by_worker = 0;  /* Reset local counter */
        pthread_mutex_unlock(worker->progress_mutex);

        /* Check if any other worker encountered an error */
        if (*worker->error_occurred) {
            break;
        }

    } while (cursor != 0);

cleanup:
    /* Update error status if needed */
    if (error_occurred) {
        pthread_mutex_lock(worker->progress_mutex);
        *worker->error_occurred = 1;
        pthread_mutex_unlock(worker->progress_mutex);
    }

    /* Update active threads counter */
    pthread_mutex_lock(worker->progress_mutex);
    (*worker->active_threads)--;
    pthread_mutex_unlock(worker->progress_mutex);

    return NULL;
}

/**
 * Create Redis connection for a cluster node
 */
static valkeyContext* createNodeConnection(struct clusterNode *node) {
    if (!node || !node->ip || node->port <= 0) {
        return NULL;
    }

    /* Create basic TCP connection to the node */
    valkeyContext *context = valkeyConnect(node->ip, node->port);
    if (!context || context->err) {
        if (context) {
            fprintf(stderr, "[SCAN] Connection error to %s:%d: %s\n",
                   node->ip, node->port, context->errstr);
            valkeyFree(context);
        }
        return NULL;
    }

    return context;
}

void initClusterScanConfig(clusterScanConfig *config,
                          const char *match_pattern,
                          struct clusterNode **nodes,
                          int node_count,
                          keyProcessorCallback key_processor,
                          void *user_data) {
    memset(config, 0, sizeof(clusterScanConfig));

    config->match_pattern = match_pattern;
    config->nodes = nodes;
    config->node_count = node_count;
    config->key_processor = key_processor;
    config->user_data = user_data;

    /* Set default performance parameters */
    config->scan_batch_size = DEFAULT_SCAN_BATCH_SIZE;
    config->max_concurrent_workers = node_count;
    config->progress_report_interval = DEFAULT_PROGRESS_INTERVAL;
}

void setClusterScanPerformance(clusterScanConfig *config,
                              int batch_size,
                              int max_workers,
                              int progress_interval) {
    if (batch_size > 0) config->scan_batch_size = batch_size;
    if (max_workers > 0) config->max_concurrent_workers = max_workers;
    if (progress_interval > 0) config->progress_report_interval = progress_interval;
}

void setClusterScanProgressCallback(clusterScanConfig *config,
                                   scanProgressCallback progress_callback) {
    config->progress_callback = progress_callback;
}

int executeClusterScan(clusterScanConfig *config, clusterScanResults *results) {
    if (!config || !config->nodes || !config->key_processor) {
        return SCAN_ERROR_CALLBACK;
    }

    uint64_t start_time = getCurrentTimeMs();
    int actual_workers = (config->max_concurrent_workers < config->node_count) ?
                        config->max_concurrent_workers : config->node_count;

    /* Allocate worker array */
    scanWorker *workers = zcalloc(actual_workers * sizeof(scanWorker));
    if (!workers) {
        return SCAN_ERROR_THREAD;
    }

    /* Shared synchronization variables */
    pthread_mutex_t progress_mutex = PTHREAD_MUTEX_INITIALIZER;
    uint64_t total_keys_processed = 0;
    int active_threads = 0;
    int error_occurred = 0;

    /* Initialize and start worker threads */
    int threads_created = 0;
    for (int i = 0; i < actual_workers; i++) {
        scanWorker *worker = &workers[i];

        worker->node = config->nodes[i % config->node_count];
        worker->context = createNodeConnection(worker->node);

        if (!worker->context) {
            fprintf(stderr, "[SCAN] Failed to connect to node %d\n", i);
            continue;
        }

        worker->match_pattern = config->match_pattern;
        worker->scan_batch_size = config->scan_batch_size;
        worker->processor = config->key_processor;
        worker->user_data = config->user_data;
        worker->thread_id = i;

        worker->progress_mutex = &progress_mutex;
        worker->total_keys_processed = &total_keys_processed;
        worker->active_threads = &active_threads;
        worker->error_occurred = &error_occurred;

        if (pthread_create(&worker->thread, NULL, scanWorkerThread, worker) == 0) {
            threads_created++;
            active_threads++;
        } else {
            fprintf(stderr, "[SCAN] Failed to create worker thread %d\n", i);
            if (worker->context) {
                valkeyFree(worker->context);
                worker->context = NULL;
            }
        }
    }

    if (threads_created == 0) {
        zfree(workers);
        return SCAN_ERROR_THREAD;
    }

    if (!config->silent_mode) {
        printf("[SCAN] Started %d worker threads scanning pattern '%s'\n",
               threads_created, config->match_pattern);
    }

    /* Progress monitoring loop */
    uint64_t last_reported_keys = 0;
    while (active_threads > 0) {
        sleep(1);  /* Check progress every second */

        pthread_mutex_lock(&progress_mutex);
        uint64_t current_keys = total_keys_processed;
        int current_active = active_threads;
        pthread_mutex_unlock(&progress_mutex);

        /* Report progress if callback is provided */
        if (config->progress_callback &&
            (current_keys - last_reported_keys) >= config->progress_report_interval) {
            config->progress_callback(current_keys, current_active, config->user_data);
            last_reported_keys = current_keys;
        }
    }

    /* Wait for all threads to complete and cleanup */
    for (int i = 0; i < actual_workers; i++) {
        if (workers[i].context) {
            pthread_join(workers[i].thread, NULL);
            valkeyFree(workers[i].context);
        }
    }

    uint64_t end_time = getCurrentTimeMs();
    uint64_t total_time = end_time - start_time;

    /* Fill results structure if provided */
    if (results) {
        results->total_keys_processed = total_keys_processed;
        results->total_scan_time_ms = total_time;
        results->nodes_scanned = threads_created;
        results->errors_encountered = error_occurred ? 1 : 0;
        results->keys_per_second = total_time > 0 ?
            (double)total_keys_processed * 1000.0 / total_time : 0.0;
    }

    if (!config->silent_mode) {
        printf("[SCAN] Completed: %lu keys processed in %lu ms (%.1f keys/sec)\n",
               total_keys_processed, total_time,
               total_time > 0 ? (double)total_keys_processed * 1000.0 / total_time : 0.0);
    }

    zfree(workers);
    pthread_mutex_destroy(&progress_mutex);

    return error_occurred ? SCAN_ERROR_COMMAND : 0;
}