/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2024-present, Zvi Schneider
 * 
 * Valkey Benchmark Utilities - Header
 *
 * Utility functions for the Valkey vector search benchmark tool.
 *
 * This file is part of valkey-search-benchmark and is licensed under the
 * BSD 3-Clause License. See the LICENSE file in the root directory.
 */

#ifndef __VALKEY_BENCHMARK_UTILS_H
#define __VALKEY_BENCHMARK_UTILS_H
#include <stdio.h>
#include <stdlib.h>
#include "sds.h"
#include <valkey/valkey.h>

/* Engine types */
typedef enum {
    ENGINE_TYPE_UNKNOWN = 0,
    ENGINE_TYPE_OSS_VALKEY,           /* Open Source Valkey */
    ENGINE_TYPE_ELASTICACHE_VALKEY,   /* ElastiCache Valkey (provisioned) */
    ENGINE_TYPE_ELASTICACHE_SERVERLESS, /* ElastiCache Serverless */
    ENGINE_TYPE_MEMORYDB              /* Amazon MemoryDB */
} EngineType;
/* Forward declarations */
struct serverConfig;

/* Cluster node - shared between valkey-benchmark.c and search_utils.c */
typedef struct clusterNode {
    valkeyContext *ctx;
    int selected; /* Whether this node is selected for benchmarking */
    int is_replica; /* Whether this node is a replica */
    char *ip;
    int port;
    sds name;
    int flags;
    sds replicate; /* Primary ID if node is a replica */
    int *slots;
    int slots_count;
    int *updated_slots;      /* Used by updateClusterSlotsConfiguration */
    int updated_slots_count; /* Used by updateClusterSlotsConfiguration */
    int replicas_count;
    struct serverConfig *server_config;
} clusterNode;

/* Field snapshot for temporal diff calculations */
typedef struct fieldSnapshot {
    sds field_name;
    char* value_str;
    long long value;
    long long*  per_node_values;  /* Array of per-node values */
    int node_count;
    int valid;
} fieldSnapshot;

/* Cluster snapshot at a point in time */
typedef struct clusterSnapshot {
    long long timestamp_ms;
    int num_fields;
    fieldSnapshot *fields;
    int num_nodes;
    sds *node_identifiers;  /* Store node IP:port for validation */
} clusterSnapshot;

/* Callback types */
typedef int (*matcherCallBack)(const char *line, const char *prefix);


/* Read from replica options */
typedef enum readFromReplica {
    FROM_PRIMARY_ONLY = 0, /* default option */
    FROM_REPLICA_ONLY,
    FROM_ALL
} readFromReplica;

typedef enum {
    DIFF_NONE = 0,
    DIFF_RATE_COUNT,        /* delta / time_sec */
    DIFF_RATE_MICROSEC,     /* (delta / 1M) / time_sec - for microsecond counters */
    DIFF_MEMORY_GROWTH,     /* (delta_bytes / MB) / time_sec */
    DIFF_PERCENTAGE_CHANGE  /* (delta / old_value) * 100 */
} DiffType;

typedef enum {
    DISPLAY_INTEGER,
    DISPLAY_MEMORY_MB,
    DISPLAY_MEMORY_HUMAN,
    DISPLAY_PERCENTAGE,  /* From fixed-point scaled by 1000 */
    DISPLAY_FLOAT,       /* From fixed-point scaled by 1000 */
    DISPLAY_LATENCY_USEC,
    DISPLAY_MINMAX
} DisplayFormat;

typedef enum {
    AGG_SUM,
    AGG_AVERAGE,
    AGG_MAX,
    AGG_MINMAX
} AggregationType;

/* Generic numeric parser with conversion strategy */
typedef enum {
    PARSE_INTEGER,
    PARSE_MEMORY,      /* Handles K/M/G suffixes */
    PARSE_FLOAT_FIXED, /* Returns value * 1000 */
    PARSE_PERCENTILE,   /* Extracts p50/p99/p99.9 from string */
    PARSE_CMDSTATS    /* Extracts from cmdstat_<cmd>: calls=...,usec=...,usec_per_call=...,rejected=...,failed=... */
} ParseStrategy;

typedef struct {
    ParseStrategy strategy;
    const char *key;  /* For percentile: "p50", "p99", "p99.9" */
} ParseConfig;

/* Extended field type with temporal diff support */
typedef struct infoFieldType {
    char* prefix_match;
    matcherCallBack match;
    ParseConfig parse_config;
    AggregationType aggregation_type;
    DisplayFormat display_format;
    DiffType diff_type;
    int track_per_node;          /* Whether to store per-node values */
    readFromReplica nodes_to_aggregate; /* Whether to read from replicas */
    int is_last; /* Whether we should re-process this line for next field, or stop */
} infoFieldType;

// getMemoryInfoClusterGeneric();
clusterSnapshot* getSearchInfo(int cluster_node_count, clusterNode **cluster_nodes,
                                enum valkeyConnectionType ct, 
                                long long *search_memory, long long *search_reclaimable, 
                   long long *search_total_docs, long long *search_ingest_field_vector, 
                   long long *search_background_indexing_status);
clusterSnapshot* getInfoCluster(int cluster_node_count, clusterNode **cluster_nodes,
                                enum valkeyConnectionType ct);
clusterSnapshot* getFtInfoStatistics(const char *index_name,
int cluster_node_count, clusterNode **cluster_nodes,
                                enum valkeyConnectionType ct);
void getFullInfo(const char *index_name, 
                int cluster_node_count, clusterNode **cluster_nodes,
                enum valkeyConnectionType ct);
void* compareInfoSnapshots(int cluster_node_count, clusterNode **cluster_nodes,
                                      enum valkeyConnectionType ct, 
                                      clusterSnapshot *old_infoall, clusterSnapshot *new_snap_infoall, clusterSnapshot *old_ftinfo, clusterSnapshot *new_snap_ftinfo, clusterSnapshot *old_infosearch, clusterSnapshot *new_snap_infosearch);
void freeClusterSnapshot(clusterSnapshot *snapshot);
valkeyContext *getValkeyContext(enum valkeyConnectionType ct, const char *ip_or_path, int port);
void waitForIndexBackfillComplete(EngineType engine_type, int cluster_node_count, clusterNode **cluster_nodes,
                                        enum valkeyConnectionType ct, const char **index_names, int num_indexes);
EngineType getEngineType(const char *ip_or_path, int port, enum valkeyConnectionType ct);
int isClusterModeEnabled(valkeyContext *ctx);

/* Extract prefix from FT.INFO response */
sds extractPrefixFromFtInfo(valkeyReply *reply, EngineType engine_type);

/* Runtime Configuration Management */
typedef struct runtimeConfigEntry {
    char *key;
    char *value;
    char *original_value;  /* Store original value for restoration */
} runtimeConfigEntry;

typedef struct runtimeConfigContext {
    runtimeConfigEntry *entries;
    int num_entries;
    int capacity;
    int applied;  /* Whether configs have been applied */
} runtimeConfigContext;

/* Load runtime configuration from file */
runtimeConfigContext* loadRuntimeConfig(const char *config_file);

/* Apply runtime configuration to server(s) */
int applyRuntimeConfig(runtimeConfigContext *ctx, 
                       int cluster_node_count, 
                       clusterNode **cluster_nodes,
                       enum valkeyConnectionType ct,
                       int verbose);

/* Restore original configuration */
int restoreRuntimeConfig(runtimeConfigContext *ctx,
                         int cluster_node_count,
                         clusterNode **cluster_nodes,
                         enum valkeyConnectionType ct,
                         int verbose);

/* Free runtime configuration context */
void freeRuntimeConfig(runtimeConfigContext *ctx);

#endif /* __VALKEY_BENCHMARK_UTILS_H */