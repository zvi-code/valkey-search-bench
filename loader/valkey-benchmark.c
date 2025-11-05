/* Server benchmark utility.
 *
 * Copyright (c) 2009-2012, Redis Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "search_utils.h"
#include "dataset_api.h"
#include "dataset_id_mapping.h"
#include "fmacros.h"
#include "load_optimizer.h"
#include "config_persist.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <signal.h>
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>

#include "sds.h"
#include "ae.h"
#include "util.h"
#include <valkey/valkey.h>
#include <valkey/alloc.h>
#ifdef USE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <valkey/tls.h>
#endif
#ifdef USE_RDMA
#include <valkey/rdma.h>
#endif
#include "adlist.h"
#include "dict.h"
#include "zmalloc.h"
#include "crc16_slottable.h"
#include "hdr_histogram.h"
#include "cli_common.h"
#include "mt19937-64.h"

extern uint16_t crc16(const char *buf, int64_t len);

static long long nstime(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

#define RANDPTR_INITIAL_SIZE 8
#define DEFAULT_LATENCY_PRECISION 3
#define MAX_LATENCY_PRECISION 4
#define MAX_THREADS 500
#define CLUSTER_SLOTS 16384
#define CONFIG_LATENCY_HISTOGRAM_MIN_VALUE 10L              /* >= 10 usecs */
#define CONFIG_LATENCY_HISTOGRAM_MAX_VALUE 3000000L         /* <= 3 secs(us precision) */
#define CONFIG_LATENCY_HISTOGRAM_INSTANT_MAX_VALUE 3000000L /* <= 3 secs(us precision) */
#define SHOW_THROUGHPUT_INTERVAL 250                        /* 250ms */

#define CLIENT_GET_EVENTLOOP(c) (c->thread_id >= 0 ? config.threads[c->thread_id%config.num_threads]->el : config.el)


#define QUERY_VECTOR "query_vector"

#define DATASET_KEY_PLACEHOLDER    "__d_key_ph__"      // 12 bytes to fit 
#define DATASET_VECTOR_PLACEHOLDER "__d_vec_ph____"   // 16 bytes 

#define DATASET_KEY_PLACEHOLDER_INDEX (VECTOR_PLACEHOLDER_INDEX + 1)
#define DATASET_VECTOR_PLACEHOLDER_INDEX (DATASET_KEY_PLACEHOLDER_INDEX + 1)

#define DATASET_TAG_PLACEHOLDER "___tag_field____"   // 16 bytes 
#define DATASET_TAG_PLACEHOLDER_INDEX (DATASET_VECTOR_PLACEHOLDER_INDEX + 1)

#define DATASET_NUMERIC_TIME_PLACEHOLDER "__time__"   // 8 bytes 
#define DATASET_NUMERIC_TIME_PLACEHOLDER_INDEX (DATASET_TAG_PLACEHOLDER_INDEX + 1)

#define DATASET_NUMERIC_SCORE_PLACEHOLDER "__score__"   // 8 bytes 
#define DATASET_NUMERIC_SCORE_PLACEHOLDER_INDEX (DATASET_NUMERIC_TIME_PLACEHOLDER_INDEX + 1)

#define VECTOR_PLACEHOLDER "__v_rd__"  // Exactly 8 characters for 2 floats
#define VECTOR_NUM_RAND_DIM (8/sizeof(float)) // Number of random dimensions for vector generation
#define VECTOR_PLACEHOLDER_INDEX (CLUSTER_PLACEHOLDER_INDEX + 1)

#define CLUSTER_PLACEHOLDER "{clt}"
#define CLUSTER_PLACEHOLDER_INDEX PLACEHOLDER_NORMAL_NUM_OF


#define PLACEHOLDER_NUM_OF (DATASET_NUMERIC_SCORE_PLACEHOLDER_INDEX + 1)  // Total number of placeholders
#define PLACEHOLDER_NORMAL_NUM_OF 10  // Number of normal placeholders excluding vector and cluster placeholders


// TODO: Use existing vectors\fields in the index as base for vector\tag\numeric generation
static const struct {
    const char *name;
    int64_t len;
} PLACEHOLDERS[] = {
    // initialize the array in exact positions
    [0] = {"__rand_int__", 12}, 
    [1] = {"__rand_1st__", 12}, 
    [2] = {"__rand_2nd__", 12}, 
    [3] = {"__rand_3rd__", 12}, 
    [4] = {"__rand_4th__", 12},
    [5] = {"__rand_5th__", 12}, 
    [6] = {"__rand_6th__", 12}, 
    [7] = {"__rand_7th__", 12}, 
    [8] = {"__rand_8th__", 12}, 
    [9] = {"__rand_9th__", 12},
    [CLUSTER_PLACEHOLDER_INDEX] = {CLUSTER_PLACEHOLDER, 5},
    [VECTOR_PLACEHOLDER_INDEX] = {VECTOR_PLACEHOLDER, 8},  // Vector placeholder
    [DATASET_KEY_PLACEHOLDER_INDEX] = {DATASET_KEY_PLACEHOLDER, 12},
    [DATASET_VECTOR_PLACEHOLDER_INDEX] = {DATASET_VECTOR_PLACEHOLDER, 14},  // "__d_vec_ph____" is 14 chars
    [DATASET_TAG_PLACEHOLDER_INDEX] = {DATASET_TAG_PLACEHOLDER, 16},
    [DATASET_NUMERIC_TIME_PLACEHOLDER_INDEX] = {DATASET_NUMERIC_TIME_PLACEHOLDER, 8},
    [DATASET_NUMERIC_SCORE_PLACEHOLDER_INDEX] = {DATASET_NUMERIC_SCORE_PLACEHOLDER, 9},  // "__score__" is 9 chars
};



struct benchmarkThread;
struct clusterNode;
struct serverConfig;

/* 
FT,INFO index_name
Response:
[ARR][array with 26 elements]
[STA]  Status: index_name
[STA]  Status: grocery_products
[STA]  Status: index_options
[ARR]  [array with 0 elements]
[STA]  Status: index_definition
[ARR]  [array with 6 elements]
[STA]    Status: key_type
[STA]    Status: HASH
[STA]    Status: prefixes
[ARR]    [array with 1 elements]
[STA]      Status: vec:
[STA]    Status: default_score
[STR]    1
[STA]  Status: attributes
[ARR]  [array with 2 elements]
[ARR]    [array with 8 elements]
[STA]      Status: identifier
[STA]      Status: vector_field
[STA]      Status: attribute
[STA]      Status: vector_field
[STA]      Status: type
[STA]      Status: VECTOR
[STA]      Status: index
[ARR]      [array with 12 elements]
[STA]        Status: capacity
[INT]        102400
[STA]        Status: dimensions
[INT]        768
[STA]        Status: distance_metric
[STA]        Status: COSINE
[STA]        Status: size
[STR]        2
[STA]        Status: data_type
[STA]        Status: FLOAT32
[STA]        Status: algorithm
[ARR]        [array with 8 elements]
[STA]          Status: name
[STA]          Status: HNSW
[STA]          Status: m
[INT]          16
[STA]          Status: ef_construction
[INT]          200
[STA]          Status: ef_runtime
[INT]          200
[STA]    Status: curr_vectors
[INT]  100230
[STA]  Status: curr_deleted_vectors
[INT]  100228
[ARR]  [array with 10 elements]
[STA]    Status: identifier
[STA]    Status: category
[STA]    Status: attribute
[STA]    Status: category
[STA]    Status: type
[STA]    Status: TAG
[STA]    Status: SEPARATOR
[STA]    Status: ,
[STA]    Status: size
[STR]    2
[STA]  Status: num_docs
[STR]  2
[STA]  Status: num_terms
[STR]  0
[STA]  Status: num_records
[STR]  4
[STA]  Status: hash_indexing_failures
[STR]  0
[STA]  Status: backfill_in_progress
[STR]  0
[STA]  Status: backfill_complete_percent
[STR]  1.000000
[STA]  Status: mutation_queue_size
[STR]  0
*/

/* Tag distribution structure */
typedef struct tagDistribution {
    sds pattern;            /* Tag pattern with optional placeholders */
    double percentage;      /* Percentage of keys with this tag */
    double cumulative;      /* Cumulative percentage for selection */
} tagDistribution;

typedef struct searchRuntimeConfig {
    /* Tag distribution fields */
    tagDistribution *tag_dists; /* Array of tag distributions */
    int64_t n_dists;               /* Number of distributions */
    sds tag_filter;                      /* Filter pattern for queries */
} searchRuntimeConfig;

/* Search index configuration */
typedef struct searchIndex {
    sds name;               /* Index name */
    sds algorithm;          /* Index algorithm type (e.g., HNSW, FLAT) */
    sds prefix;             /* Index key prefix */
    int64_t nocontent;           /* Use NOCONTENT option for FT.SEARCH */
    int64_t localonly;          /* Use LOCAL option for FT.SEARCH */
    sds vector_field;       /* Vector field name */
    int64_t vector_dim;         /* Vector dimension */    
    sds tag_field;          /* Tag field name if exists*/
    int64_t payload_tag_len;    /* Maximum tag field payload size (for fixed-size templates) */
    sds numeric_field;      /* Numeric field name if exists */
    int64_t ef_construction;    /* EF Construction for vector search */
    int64_t m;                  /* HNSW M parameter */
    int64_t ef_search;          /* EF Search for vector search */
    int64_t k;                  /* Number of nearest neighbors to return */
    sds metric;            /* Distance metric (e.g., L2, COSINE) */
    searchRuntimeConfig curr_conf; /* Runtime configuration for search */
} searchIndex;

/* Vector placeholder callback information */
typedef enum {
    VECTOR_PHASE_PREFILL,
    VECTOR_PHASE_INSERT,
    VECTOR_PHASE_QUERY
} VectorPhase;

/* Callback function type for vector placeholder replacement */
typedef void (*VectorPlaceholderCallback)(char *vector_data, const char *key, VectorPhase phase, int64_t dim);

/* Base vector for efficient vector generation */
static float *base_vector = NULL;
static int64_t base_vector_dim = 0;

/* Locations of the placeholders __rand_int__, __rand_1st__,
 * __rand_2nd, etc. within the RESP encoded command buffer. */
static struct placeholders {
    size_t cmd_len;                     /* length of the command */
    size_t count[PLACEHOLDER_NUM_OF];    /* number of each placeholder in the command */
    size_t len[PLACEHOLDER_NUM_OF];      /* length of each placeholder */
    size_t *indices[PLACEHOLDER_NUM_OF]; /* pointer to indices for each placeholder */
    size_t *index_data;                 /* allocation holding all index data */
} placeholders;

typedef struct _client {
    valkeyContext *context;
    sds obuf;
    char **stagptr;     /* Pointers to slot hashtags (cluster mode only) */
    size_t staglen;     /* Number of pointers in client->stagptr */
    size_t stagfree;    /* Number of unused pointers in client->stagptr */
    size_t written;     /* Bytes of 'obuf' already written */
    long long start;    /* Start time of a request */
    long long latency;  /* Request latency */
    int64_t seqlen;         /* Number of commands in the command sequence */
    int64_t pending;        /* Number of pending requests (replies to consume) */
    int64_t prefix_pending; /* If non-zero, number of pending prefix commands. Commands
                           such as auth and select are prefixed to the pipeline of
                           benchmark commands and discarded after the first send. */
    int64_t prefixlen;      /* Size in bytes of the pending prefix commands */
    int64_t thread_id;
    struct clusterNode *cluster_node;
    int64_t slots_last_update;
    uint64_t *dataset_query_indices; /* Queue of dataset query indices for recall tracking */
    int64_t dataset_query_head;           /* Head position in dataset query index queue */
    int64_t dataset_query_tail;           /* Tail position in dataset query index queue */
    int64_t dataset_query_capacity;       /* Capacity of dataset query index queue */
    uint64_t paused : 1;
    uint64_t reuse : 1;
    int64_t running_queries;
} *client;


/* Threads. */
typedef struct benchmarkThread {
    int64_t index;
    pthread_t thread;
    aeEventLoop *el;
    list *paused_clients;
    int64_t *node_request_counters;  /* Array of request counts per node (current cycle) */
    int64_t *node_quota_remaining;   /* Array of remaining quota per node */
    list *clients;
} benchmarkThread;



/* Cluster - clusterNode is now defined in search_utils.h */

typedef struct serverConfig {
    sds save;
    sds appendonly;
} serverConfig;

static struct config {
    aeEventLoop *el;
    enum valkeyConnectionType ct;
    cliConnInfo conn_info;
    valkeyContext *conn_ctx;
    int tls;
    int64_t mptcp;
    struct cliSSLconfig sslconfig;
    int64_t numclients;
    _Atomic int64_t liveclients;
    int64_t requests;
    _Atomic int64_t requests_issued;
    _Atomic int64_t requests_finished;
    _Atomic int64_t previous_requests_finished;
    int64_t last_printed_bytes;
    long long previous_tick;
    int64_t keysize;
    int64_t datasize;
    int64_t replace_placeholders;
    int64_t keyspacelen;
    int64_t sequential_replacement;
    int64_t keepalive;
    int64_t pipeline;
    long long start;
    long long totlatency;
    const char *title;
    list *clients;
    list *paused_clients;
    int64_t quiet;
    int64_t csv;
    int64_t loop;
    int64_t idlemode;
    sds input_dbnumstr;
    char *tests;
    int64_t stdinarg; /* get last arg from stdin. (-x option) */
    int64_t precision;
    int64_t num_threads;
    struct benchmarkThread **threads;
    int64_t cluster_mode;
    readFromReplica read_from_replica;
    int64_t cluster_node_count;
    struct clusterNode **cluster_nodes;
    int64_t cluster_primary_node_count;
    struct clusterNode **cluster_primary_nodes;
    int64_t selected_node_count;
    struct clusterNode **selected_nodes;
    struct serverConfig *server_config;
    struct hdr_histogram *latency_histogram;
    struct hdr_histogram *current_sec_latency_histogram;
    _Atomic int64_t is_fetching_slots;
    _Atomic int64_t is_updating_slots;
    _Atomic int64_t slots_last_update;
    int64_t enable_tracking;
    int64_t num_functions;
    int64_t num_keys_in_fcall;
    pthread_mutex_t liveclients_mutex;
    pthread_mutex_t is_updating_slots_mutex;
    int64_t resp3; /* use RESP3 */
    int64_t rps;
    atomic_uint_fast64_t last_time_ns;
    uint64_t time_per_token;
    uint64_t time_per_burst;
    int64_t balance_nodes;           /* Enable fair node balancing mode */
    int64_t balance_quota_step;      /* Quota of requests per node per cycle (default: 1000) */
    int64_t balance_tolerance_pct;   /* Tolerance percentage for imbalance (default: 10) */
    int64_t *node_request_counters;  /* Array of request counts per node (current cycle) */
    int64_t *node_quota_remaining;   /* Array of remaining quota per node */
    int64_t clean;
    int64_t use_search; /* Use search indexes */
    searchIndex search;
    int64_t print_search_results; /* Print FT.SEARCH results */
    int64_t search_debug;
    EngineType engine_type; /* True if connected to MemoryDB */
    /* Dataset configuration */
    int64_t use_dataset;              /* Enable dataset mode */
    int64_t use_filtered_search;      /* Enable metadata filtering */
    sds dataset_name;             /* Dataset identifier or path */
    void *dataset_ctx;            /* Opaque dataset context */
    uint64_t dataset_num_vectors; /* Total vectors */
    uint64_t dataset_num_queries; /* Total queries */
    uint32_t dataset_num_neighbors; /* Ground truth k */
    _Atomic uint64_t dataset_prefill_counter;  /* Insert counter */
    _Atomic uint64_t dataset_query_counter;    /* Query counter */
    
    /* Load Optimizer configuration */
    int64_t optimize_enabled;         /* Enable adaptive optimization */
    optimizer_t *optimizer;       /* Optimizer instance */
    sds optimize_objective;       /* e.g., "maximize:qps" or "minimize:p99_latency" */
    sds *optimize_constraints;    /* Array of constraint strings */
    int64_t num_optimize_constraints; /* Number of constraints */
    sds optimize_csv_file;        /* CSV output file for optimization results */
    int64_t optimize_max_iterations;  /* Max optimization iterations */
    int64_t optimize_min_requests;    /* Min requests per benchmark run */
    sds optimize_client_range;    /* Client count range: "min:max" */
    sds optimize_thread_range;    /* Thread count range: "min:max" */
    sds optimize_ef_search_range; /* ef_search range: "min:max" */
    sds optimize_pipeline_range;  /* Pipeline range: "min:max" */

    /* Config persistence flags */
    int64_t save_config;              /* Save configuration after successful run */
    int64_t no_save_config;           /* Skip saving configuration */
    
    /* Runtime configuration management */
    sds runtime_config_file;      /* Path to runtime config file */
    runtimeConfigContext *runtime_config_ctx; /* Runtime config context */
    int64_t restore_runtime_config;   /* Restore original config after benchmark */
    
    /* Baseline latency measurement */
    int64_t measure_baseline;         /* Measure baseline network latency (default: 1) */
    int64_t no_baseline;              /* Disable baseline measurement */
    int64_t baseline_measured;        /* Flag indicating baseline has been measured */
    int64_t skip_latency_report;      /* Skip printing latency report (used internally) */
    int64_t preserve_histograms;      /* Don't free histograms in benchmarkSequence (used for baseline) */
} config = {0};

/* Recall statistics for dataset mode */
typedef struct {
    uint64_t total_queries;
    uint64_t total_matches;
    double sum_recall;
    double min_recall;
    double max_recall;
    uint64_t perfect_recalls;
    uint64_t zero_recalls;
} recallStats;

/* Baseline network latency measurements */
typedef struct {
    double avg_latency_ms;
    double min_latency_ms;
    double p50_latency_ms;
    double p90_latency_ms;
    double p95_latency_ms;
    double p99_latency_ms;
    double max_latency_ms;
    int64_t measured;  /* 1 if baseline has been measured, 0 otherwise */
} baseline_latency_t;

static recallStats dataset_recall_stats;
static recallStats dataset_recall_stats_ext;
static baseline_latency_t baseline_latency = {0};

static clusterTagMap cluster_tag_map;
pthread_mutex_t recall_stats_mutex = PTHREAD_MUTEX_INITIALIZER;

__attribute__((unused))
static void initRecallStats(void) {
    memset(&dataset_recall_stats, 0, sizeof(recallStats));
    memset(&dataset_recall_stats_ext, 0, sizeof(recallStats));
    dataset_recall_stats.min_recall = 1.0;
    dataset_recall_stats_ext.min_recall = 1.0;
}

static void updateRecallStatsInt(recallStats* stats, float recall) {
    pthread_mutex_lock(&recall_stats_mutex);

    stats->total_queries++;
    stats->sum_recall += recall;

    if (recall < stats->min_recall) {
        stats->min_recall = recall;
    }
    if (recall > stats->max_recall) {
        stats->max_recall = recall;
    }
    if (recall >= 0.9999) {
        stats->perfect_recalls++;
    }
    if (recall < 0.0001) {
        stats->zero_recalls++;
    }

    stats->total_matches += (uint64_t)(recall * config.search.k);

    pthread_mutex_unlock(&recall_stats_mutex);
}

static void updateRecallStats(float recall) {
    updateRecallStatsInt(&dataset_recall_stats, recall);
}

static void updateRecallStatsExt(float recall) {
    updateRecallStatsInt(&dataset_recall_stats_ext, recall);
}

/* Parse metric name string to metric_t enum */
static metric_t parseMetricName(const char *name) {
    if (!strcasecmp(name, "qps")) return METRIC_QPS;
    if (!strcasecmp(name, "avg_latency")) return METRIC_AVG_LATENCY;
    if (!strcasecmp(name, "p50_latency")) return METRIC_P50_LATENCY;
    if (!strcasecmp(name, "p90_latency")) return METRIC_P90_LATENCY;
    if (!strcasecmp(name, "p95_latency")) return METRIC_P95_LATENCY;
    if (!strcasecmp(name, "p99_latency")) return METRIC_P99_LATENCY;
    if (!strcasecmp(name, "max_latency")) return METRIC_MAX_LATENCY;
    if (!strcasecmp(name, "min_latency")) return METRIC_MIN_LATENCY;
    if (!strcasecmp(name, "recall_avg")) return METRIC_RECALL_AVG;
    if (!strcasecmp(name, "recall_min")) return METRIC_RECALL_MIN;
    if (!strcasecmp(name, "recall_max")) return METRIC_RECALL_MAX;
    if (!strcasecmp(name, "recall_perfect_pct")) return METRIC_RECALL_PERFECT_PCT;
    if (!strcasecmp(name, "recall_zero_pct")) return METRIC_RECALL_ZERO_PCT;
    
    fprintf(stderr, "Unknown metric name: %s\n", name);
    exit(1);
}

/* Parse and set optimization objective: "maximize:qps" or "minimize:p99_latency" */
static void parseOptimizerObjective(optimizer_t *opt, const char *objective_str) {
    char *str_copy = strdup(objective_str);
    char *colon = strchr(str_copy, ':');
    
    if (!colon) {
        fprintf(stderr, "Invalid objective format: %s (expected 'maximize:metric' or 'minimize:metric')\n", objective_str);
        exit(1);
    }
    
    *colon = '\0';
    const char *type_str = str_copy;
    const char *metric_str = colon + 1;
    
    objective_type_t type;
    if (!strcasecmp(type_str, "maximize")) {
        type = OBJECTIVE_MAXIMIZE;
    } else if (!strcasecmp(type_str, "minimize")) {
        type = OBJECTIVE_MINIMIZE;
    } else {
        fprintf(stderr, "Invalid objective type: %s (expected 'maximize' or 'minimize')\n", type_str);
        exit(1);
    }
    
    metric_t metric = parseMetricName(metric_str);
    optimizer_set_objective(opt, metric, type);
    
    free(str_copy);
}

/* Parse and add constraint: "p99_latency:lt:5.0" or "qps:gt:10000" */
static void parseOptimizerConstraint(optimizer_t *opt, const char *constraint_str) {
    char *str_copy = strdup(constraint_str);
    char *parts[3];
    int64_t part_count = 0;
    
    char *token = strtok(str_copy, ":");
    while (token && part_count < 3) {
        parts[part_count++] = token;
        token = strtok(NULL, ":");
    }
    
    if (part_count != 3) {
        fprintf(stderr, "Invalid constraint format: %s (expected 'metric:op:value')\n", constraint_str);
        exit(1);
    }
    
    metric_t metric = parseMetricName(parts[0]);
    
    constraint_type_t type;
    if (!strcasecmp(parts[1], "lt") || !strcasecmp(parts[1], "<")) {
        type = CONSTRAINT_LESS_THAN;
    } else if (!strcasecmp(parts[1], "gt") || !strcasecmp(parts[1], ">")) {
        type = CONSTRAINT_GREATER_THAN;
    } else {
        fprintf(stderr, "Invalid constraint operator: %s (expected 'lt' or 'gt')\n", parts[1]);
        exit(1);
    }
    
    double threshold = atof(parts[2]);
    optimizer_add_constraint(opt, metric, type, threshold);
    
    free(str_copy);
}

/* Parse range string "min:max" and return min and max values */
/* Returns 1 on success, 0 on failure. If parsing fails, uses default values. */
static int64_t parseOptimizeRange(const char *range_str, int64_t *min_val, int64_t *max_val, int64_t default_min, int64_t default_max) {
    if (!range_str || !min_val || !max_val) {
        *min_val = default_min;
        *max_val = default_max;
        return 0;
    }
    
    char *str_copy = strdup(range_str);
    char *colon = strchr(str_copy, ':');
    
    if (!colon) {
        fprintf(stderr, "Warning: Invalid range format '%s', expected 'min:max'. Using defaults %ld:%ld\n",
                range_str, default_min, default_max);
        *min_val = default_min;
        *max_val = default_max;
        free(str_copy);
        return 0;
    }
    
    *colon = '\0';
    const char *min_str = str_copy;
    const char *max_str = colon + 1;
    
    int64_t parsed_min = atoi(min_str);
    int64_t parsed_max = atoi(max_str);
    
    if (parsed_min >= parsed_max) {
        fprintf(stderr, "Warning: Invalid range '%s', min must be less than max. Using defaults %ld:%ld\n",
                range_str, default_min, default_max);
        *min_val = default_min;
        *max_val = default_max;
        free(str_copy);
        return 0;
    }
    
    *min_val = parsed_min;
    *max_val = parsed_max;
    free(str_copy);
    return 1;
}

/* Collect current metrics for optimizer */
static void collectOptimizerMetrics(double metrics[METRIC_COUNT]) {
    /* Calculate QPS */
    if (config.totlatency > 0) {
        metrics[METRIC_QPS] = (double)config.requests_finished / ((double)config.totlatency / 1000.0);
    } else {
        metrics[METRIC_QPS] = 0.0;
    }
    
    /* Collect latency metrics from histogram */
    if (config.latency_histogram && config.latency_histogram->total_count > 0) {
        metrics[METRIC_AVG_LATENCY] = hdr_mean(config.latency_histogram) / 1000.0;
        metrics[METRIC_MIN_LATENCY] = ((double)hdr_min(config.latency_histogram)) / 1000.0;
        metrics[METRIC_P50_LATENCY] = hdr_value_at_percentile(config.latency_histogram, 50.0) / 1000.0;
        metrics[METRIC_P90_LATENCY] = hdr_value_at_percentile(config.latency_histogram, 90.0) / 1000.0;
        metrics[METRIC_P95_LATENCY] = hdr_value_at_percentile(config.latency_histogram, 95.0) / 1000.0;
        metrics[METRIC_P99_LATENCY] = hdr_value_at_percentile(config.latency_histogram, 99.0) / 1000.0;
        metrics[METRIC_MAX_LATENCY] = ((double)hdr_max(config.latency_histogram)) / 1000.0;
    } else {
        metrics[METRIC_AVG_LATENCY] = 0.0;
        metrics[METRIC_MIN_LATENCY] = 0.0;
        metrics[METRIC_P50_LATENCY] = 0.0;
        metrics[METRIC_P90_LATENCY] = 0.0;
        metrics[METRIC_P95_LATENCY] = 0.0;
        metrics[METRIC_P99_LATENCY] = 0.0;
        metrics[METRIC_MAX_LATENCY] = 0.0;
    }
    
    /* Collect recall metrics (thread-safe, already locked during updates) */
    if (config.use_dataset && dataset_recall_stats.total_queries > 0) {
        metrics[METRIC_RECALL_AVG] = dataset_recall_stats.sum_recall / dataset_recall_stats.total_queries;
        metrics[METRIC_RECALL_MIN] = dataset_recall_stats.min_recall;
        metrics[METRIC_RECALL_MAX] = dataset_recall_stats.max_recall;
        metrics[METRIC_RECALL_PERFECT_PCT] = 
            (double)dataset_recall_stats.perfect_recalls * 100.0 / dataset_recall_stats.total_queries;
        metrics[METRIC_RECALL_ZERO_PCT] = 
            (double)dataset_recall_stats.zero_recalls * 100.0 / dataset_recall_stats.total_queries;
    } else {
        metrics[METRIC_RECALL_AVG] = 0.0;
        metrics[METRIC_RECALL_MIN] = 0.0;
        metrics[METRIC_RECALL_MAX] = 0.0;
        metrics[METRIC_RECALL_PERFECT_PCT] = 0.0;
        metrics[METRIC_RECALL_ZERO_PCT] = 0.0;
    }
}

// create printf wrapper function that prints inder mutex lock
// static pthread_mutex_t print_mutex = PTHREAD_MUTEX_INITIALIZER;


/* Prototypes */
static void writeHandler(aeEventLoop *el, int fd, void *privdata, int mask);
// static long long awakenNodeBalancedClient(struct aeEventLoop *eventLoop, long long id, void *clientData);
static void createMissingClients(char *cmd, int64_t len, int64_t seqlen);
static benchmarkThread *createBenchmarkThread(int64_t index);
static void freeBenchmarkThread(benchmarkThread *thread);
static void freeBenchmarkThreads(void);
static void *execBenchmarkThread(void *ptr);
static void benchmark(const char *title, char *cmd, int64_t len);
static void measureBaselineLatency(void);
static clusterNode *createClusterNode(char *ip, int port);
// static serverConfig *getServerConfig(enum valkeyConnectionType ct, const char *ip_or_path, int port);
static sds selectTagByDistribution(void);
static void parseTagDistributions(const char *distributions_str);
valkeyContext *getValkeyContext(enum valkeyConnectionType ct, const char *ip_or_path, int port);
static void freeServerConfig(serverConfig *cfg);
static int64_t fetchClusterSlotsConfiguration(client c);
static void updateClusterSlotsConfiguration(void);
static long long showThroughput(struct aeEventLoop *eventLoop, long long id, void *clientData);

/* Dict callbacks */
static uint64_t dictSdsHash(const void *key);
static int dictSdsKeyCompare(const void *key1, const void *key2);

#define UNUSED(V) ((void)V)

#define printf_results(...) do { \
    if (config.print_search_results) { \
        printf(__VA_ARGS__); \
    } \
} while(0)

static float checkNeighbors(uint64_t query_ix, 
    uint64_t *returned_neighbors, size_t num_neighbors, 
    dataset_neighbors_t *ground_truth_neighbors);

/* Vector key encoding/decoding functions */

/**
 * Encode a vector key from fixed-size components
 * Format: prefix + cluster_tag + ':' + vector_id
 * The final key format depends on prefix_len, cluster_tag_len, and vector_id_len
 */
static int64_t encode_vector_key_fixed(char *key_out, size_t key_out_size,
                                  const char *prefix,
                                  const char *cluster_tag,
                                  uint64_t vector_id) {
    if (!key_out || key_out_size == 0) {
        return -1;
    }
    size_t prefix_len = strlen(config.search.prefix);
    size_t cluster_tag_len = config.cluster_mode? PLACEHOLDERS[CLUSTER_PLACEHOLDER_INDEX].len : 0;
    size_t key_write_len = prefix_len + cluster_tag_len + 1 + PLACEHOLDERS[DATASET_KEY_PLACEHOLDER_INDEX].len; // +1 for ':' +12 for vector_id
    int64_t ret;
    if (prefix) {
        memcpy(key_out, prefix, prefix_len);
    }
    key_out+=prefix_len;
    key_write_len-=prefix_len;
    if (config.cluster_mode) {
        if (cluster_tag) {
            memcpy(key_out, cluster_tag, cluster_tag_len);
        }
        key_out+=cluster_tag_len;
        key_write_len-=cluster_tag_len;
    }  
    assert(*key_out == ':'); // Separator
    key_out++; // Skip ':'
    key_write_len--;
    // Vector ID placeholder length check
    assert(12 == PLACEHOLDERS[DATASET_KEY_PLACEHOLDER_INDEX].len);
    // Encode vector ID with fixed width - format directly to avoid null terminator
    char temp_buf[13];  // 12 digits + null terminator
    ret = snprintf(temp_buf, sizeof(temp_buf), "%012lu", (unsigned long)vector_id);
    assert(ret == 12);  // Should be exactly 12 characters
    memcpy(key_out, temp_buf, 12);  // Copy without null terminator
    // printf("DEBUG: format='%s', encoded result='%.*s', ret=%ld\n",
    //        format, (int64_t)vector_id_len, p, ret);
    assert(ret == (int64_t)key_write_len); // Should fit exactly
    return (ret >= 0 && ret <= (int64_t)key_write_len) ? 0 : -1;
}

/**
 * Decode a vector key into components using fixed field sizes
 * Format: prefix + cluster_tag + ':' + vector_id (fixed width fields)
 * Returns: 0 on success, -1 on error
 *
 * @param prefix_len: Expected prefix length
 * @param cluster_tag_len: Expected cluster tag length
 * Output parameters can be NULL to skip extracting that field.
 */
static int64_t decode_vector_key_fixed(const char *key,
                                  char *prefix_out, size_t prefix_size,
                                  char *cluster_tag_out, size_t cluster_tag_size,
                                  uint64_t *vector_id_out) {
    if (!key) {
        return -1;
    }
    size_t prefix_len = strlen(config.search.prefix);
    size_t cluster_tag_len = config.cluster_mode? PLACEHOLDERS[CLUSTER_PLACEHOLDER_INDEX].len : 0;
    size_t key_len = strlen(key);
    const char *read_pos = key;

    /* Extract prefix if requested */
    if (prefix_out && prefix_size > 0 && prefix_len > 0) {
        assert(!(prefix_len >= prefix_size || key_len < prefix_len));
        memcpy(prefix_out, read_pos, prefix_len);
        prefix_out[prefix_len] = '\0';
    }    
    read_pos += prefix_len;
    if (config.cluster_mode) {
        /* Extract cluster tag if requested */
        if (cluster_tag_out && cluster_tag_size > 0 && cluster_tag_len > 0) {
            assert(!(cluster_tag_len >= cluster_tag_size || (read_pos - key) + cluster_tag_len > key_len));
            memcpy(cluster_tag_out, read_pos, cluster_tag_len);
            cluster_tag_out[cluster_tag_len] = '\0';
        }
        read_pos += cluster_tag_len;
    } else if (cluster_tag_out && cluster_tag_size > 5) {
        // set to dummy {CMD} tag for non-cluster mode
        memcpy(cluster_tag_out, "{CMD}", 5);
        cluster_tag_out[5] = '\0';
    }
   
    assert(!((read_pos - key) >= key_len));
    /* Skip the ':' separator */
    read_pos++;

    /* Extract vector ID if requested */
    if (vector_id_out) {
        *vector_id_out = (uint64_t)atoll(read_pos);
    }

    return 0;
}

/**
 * Grow an sds string to a specified length, filling new space with a non-zero character.
 * 
 * Similar to sdsgrowzero, but fills with a specified character instead of null bytes.
 * This is useful for creating padding that won't be misinterpreted as string terminators.
 * 
 * @param s The sds string to grow (will be reallocated if needed)
 * @param len The target length
 * @param fill_char The character to fill new space with (e.g., ' ' for space)
 * @return The grown sds string (may be different pointer than input)
 */
static sds sdsgrownonzero(sds s, size_t len, char fill_char) {
    size_t curlen = sdslen(s);
    
    if (len <= curlen) {
        return s;  /* Already at or past target length */
    }
    
    /* Grow to target length (initially filled with zeros) */
    s = sdsgrowzero(s, len);
    
    /* Replace zeros with fill character in the newly allocated space */
    memset(s + curlen, fill_char, len - curlen);
    
    return s;
}

/**
 * Check a buffer for null terminators and optionally report their positions.
 * 
 * @param buffer The buffer to check
 * @param buffer_len Length of the buffer
 * @param context_name Name to use in debug output (e.g., "tag section", "dummy section")
 * @param verbose If true, print positions of null bytes found
 * @return Number of null bytes found
 */
static int checkBufferForNulls(const char* buffer, int64_t buffer_len, 
                                const char* context_name, int verbose) {
    int null_count = 0;
    
    if (verbose) {
        /* Collect positions for verbose output */
        for (int64_t i = 0; i < buffer_len; i++) {
            if (buffer[i] == '\0') {
                if (null_count == 0) {
                    fprintf(stderr, "WARNING: Found null terminators in %s at positions:", context_name);
                }
                fprintf(stderr, " %ld", i);
                null_count++;
            }
        }
        
        if (null_count > 0) {
            fprintf(stderr, " (total %d null bytes)\n", null_count);
        }
    } else {
        /* Just count without verbose output */
        for (int64_t i = 0; i < buffer_len; i++) {
            if (buffer[i] == '\0') {
                null_count++;
            }
        }
    }
    
    return null_count;
}

typedef struct {
    int64_t is_gt; // 1 if ground truth, 0 if returned neighbor
    uint64_t id;
    float distance;
} Neighbor;

static int compareNeighbors(const void *a, const void *b) {
    const Neighbor *na = (const Neighbor *)a;
    const Neighbor *nb = (const Neighbor *)b;
    if (na->distance < nb->distance) return -1;
    if (na->distance > nb->distance) return 1;
    if (na->id < nb->id) return -1;
    if (na->id > nb->id) return 1;
    return 0;
}

static int uint64_cmp(const void *a, const void *b) {
    uint64_t ua = *(const uint64_t *)a, ub = *(const uint64_t *)b;
    return (ua > ub) - (ua < ub);
}

/** evaluate returned number distance  from Query vector comparing to ground truth distances from Query vector */
static float checkNeighbors(uint64_t query_ix, 
    uint64_t *returned_neighbors, size_t num_returned_neighbors, 
    dataset_neighbors_t *ground_truth_neighbors) {
    // Check neighbors and compare the distance with ground truth vector distances
    // for every ground truth neighbor and for every returned vector, fetch the vector from datasetGetVector
    // distance calculation is L2 or COSINE based on config.search.metric
    if (!config.use_dataset || !config.dataset_ctx) {
        return 0.0f;
    }
    dataset_ctx_t *dctx = (dataset_ctx_t *)config.dataset_ctx;
    Neighbor neighbors_dists[2 * num_returned_neighbors];
    memset(neighbors_dists, 0, sizeof(neighbors_dists));
    for (uint32_t i = 0; i < num_returned_neighbors; i++) {
        neighbors_dists[i].id = ground_truth_neighbors->ids[i];
        neighbors_dists[i].distance = ground_truth_neighbors->dists[i];
        neighbors_dists[i].is_gt = 1; // Mark as ground truth
    }
              
    for (int64_t j = 0; j < num_returned_neighbors; j++) {
        int64_t i = j + num_returned_neighbors;
        neighbors_dists[i].id = returned_neighbors[j];
        neighbors_dists[i].distance = datasetGetDistanceFromQueryVector(dctx, query_ix, returned_neighbors[j]);
        neighbors_dists[i].is_gt = 0; // Mark as returned neighbor
    }
    // Sort by distance
    qsort(neighbors_dists, 2 * num_returned_neighbors, sizeof(Neighbor), compareNeighbors);
    int64_t num_returned = 0;
    int64_t num_gt = 0;
    for (int64_t i = 0; i < 2 * num_returned_neighbors; i++) {
        num_returned += !neighbors_dists[i].is_gt;
        num_gt += neighbors_dists[i].is_gt;
        printf_results("Rank %ld: ID=%lu, Distance=%.6f, %s\n", i, neighbors_dists[i].id, neighbors_dists[i].distance,
                neighbors_dists[i].is_gt ? "GT" : "RET");
        if (num_returned >= num_returned_neighbors) break;
        if (num_gt >= num_returned_neighbors) {
            if ((i+1 < 2 * num_returned_neighbors) && !neighbors_dists[i+1].is_gt && 
               (neighbors_dists[i+1].distance == neighbors_dists[i].distance) &&
                neighbors_dists[i].is_gt) {
                // include the tie
                num_returned++;
                printf_results("Including tie at rank %ld for ID %lu with distance %.6f\n",
                           i+1, neighbors_dists[i+1].id, neighbors_dists[i+1].distance);
                i++;
            }
            break;
        }
    }
    // Calculate recall
    float recall = (float)num_returned / (float)config.search.k;
    if (config.print_search_results) {
        printf("Total returned neighbors considered for recall: %ld/%ld\n", num_returned, num_gt);
        // Now check returned neighbors
        int64_t match_count = num_returned;
        pthread_mutex_lock(&recall_stats_mutex);
        printf("Query %ld: Neighbor Comparison (%ld)\n", query_ix, num_returned_neighbors);
        printf("%-6s %-20s %-20s\n", "Rank", "Returned (ID:Dist)", "Ground Truth (ID:Dist)");
        printf("%-6s %-20s %-20s\n", "----", "-------------------", "---------------------");
        Neighbor returned_neighbors_dist[num_returned_neighbors];
        Neighbor ground_truth_neighbors_dist[num_returned_neighbors];
        int64_t ret_idx = 0, gt_idx = 0;
        for (int64_t i = 0; i < num_returned_neighbors + num_returned_neighbors; i++) {
            if (!neighbors_dists[i].is_gt && ret_idx < num_returned_neighbors) {
                returned_neighbors_dist[ret_idx++] = neighbors_dists[i];
            } else if (neighbors_dists[i].is_gt && gt_idx < num_returned_neighbors) {
                ground_truth_neighbors_dist[gt_idx++] = neighbors_dists[i];
            }
        }
        // Print side by side
        for (uint32_t i = 0; i < num_returned_neighbors; i++) {
            char returned_str[32] = "";
            char gt_str[32] = "";

            if (i < num_returned_neighbors) {
                snprintf(returned_str, sizeof(returned_str), "%lu:%.4f",
                        returned_neighbors_dist[i].id, returned_neighbors_dist[i].distance);
            } else {
                valkey_strlcpy(returned_str, "-", sizeof(returned_str));
            }

            if (i < ground_truth_neighbors->count) {
                snprintf(gt_str, sizeof(gt_str), "%lu:%.4f",
                        ground_truth_neighbors_dist[i].id, ground_truth_neighbors_dist[i].distance);
            } else {
                valkey_strlcpy(gt_str, "-", sizeof(gt_str));
            }

            printf("%-6d %-20s %-20s\n", i, returned_str, gt_str);
        }
        
        printf("\n");
        
        printf("Query %lu: Returned %ld/%ld correct neighbors, Recall: %.2f%%\n",
                query_ix, match_count, config.search.k, recall * 100.0);
        pthread_mutex_unlock(&recall_stats_mutex);
    }
    
    return recall;
}

static void printDatasetRecallStats(void) {
    if (!config.use_dataset || dataset_recall_stats.total_queries == 0) {
        return;
    }

    double avg_recall = dataset_recall_stats.sum_recall /
                       dataset_recall_stats.total_queries;
    double avg_recall_ext = dataset_recall_stats_ext.sum_recall /
                        dataset_recall_stats_ext.total_queries;
    printf("\n====== DATASET RECALL STATISTICS ======\n");
    printf("  Dataset: %s\n", config.dataset_name ? config.dataset_name : "unknown");
    if (dataset_recall_stats_ext.total_queries > 0) {
        printf("  Queries evaluated: %lu [Ext: %lu]\n", dataset_recall_stats.total_queries, dataset_recall_stats_ext.total_queries);
        printf("  \n");
        printf("  Recall@%ld:\n", config.search.k);
        printf("    Average:  %.2f%% [Ext: %.2f%%]\n", avg_recall * 100.0, avg_recall_ext * 100.0);
        printf("    Min:      %.2f%% [Ext: %.2f%%]\n", dataset_recall_stats.min_recall * 100.0, dataset_recall_stats_ext.min_recall * 100.0);
        printf("    Max:      %.2f%% [Ext: %.2f%%]\n", dataset_recall_stats.max_recall * 100.0, dataset_recall_stats_ext.max_recall * 100.0);
        printf("  \n");
        printf("  Query distribution:\n");
        printf("    Perfect recall (100%%): %lu (%.1f%%) [Ext: %lu (%.1f%%)]\n",
            dataset_recall_stats.perfect_recalls,
            (float)dataset_recall_stats.perfect_recalls /
            dataset_recall_stats.total_queries * 100.0, dataset_recall_stats_ext.perfect_recalls,
            (float)dataset_recall_stats_ext.perfect_recalls /
            dataset_recall_stats_ext.total_queries * 100.0);
        printf("    Zero recall (0%%):      %lu (%.1f%%) [Ext: %lu (%.1f%%)]\n",
            dataset_recall_stats.zero_recalls,
            (float)dataset_recall_stats.zero_recalls /
            dataset_recall_stats.total_queries * 100.0, 
            dataset_recall_stats_ext.zero_recalls,
            (float)dataset_recall_stats_ext.zero_recalls /
            dataset_recall_stats_ext.total_queries * 100.0);
        printf("  \n");
        printf("  Total matches: %lu/%lu (%.2f%%) [Ext: %lu/%lu (%.2f%%)]\n",
            dataset_recall_stats.total_matches,
            dataset_recall_stats.total_queries * config.search.k,
            (float)dataset_recall_stats.total_matches /
            (dataset_recall_stats.total_queries * config.search.k) * 100.0,
            dataset_recall_stats_ext.total_matches,
            dataset_recall_stats_ext.total_queries * config.search.k,
            (float)dataset_recall_stats_ext.total_matches /
            (dataset_recall_stats_ext.total_queries * config.search.k) * 100.0);
    } else {
        // no Ext stats
        printf("  Queries evaluated: %lu\n", dataset_recall_stats.total_queries);
        printf("  \n");
        printf("  Recall@%ld:\n", config.search.k);
        printf("    Average:  %.2f%%\n", avg_recall * 100.0);
        printf("    Min:      %.2f%%\n", dataset_recall_stats.min_recall * 100.0);
        printf("    Max:      %.2f%%\n", dataset_recall_stats.max_recall * 100.0);
        printf("  \n");
        printf("  Query distribution:\n");
        printf("    Perfect recall (100%%): %lu (%.1f%%)\n",
            dataset_recall_stats.perfect_recalls,
            (float)dataset_recall_stats.perfect_recalls /
            dataset_recall_stats.total_queries * 100.0);
        printf("    Zero recall (0%%):      %lu (%.1f%%)\n",
            dataset_recall_stats.zero_recalls,
            (float)dataset_recall_stats.zero_recalls /
            dataset_recall_stats.total_queries * 100.0);
        printf("  \n");
        printf("  Total matches: %lu/%lu (%.2f%%)\n",
            dataset_recall_stats.total_matches,
            dataset_recall_stats.total_queries * config.search.k,
            (float)dataset_recall_stats.total_matches /
            (dataset_recall_stats.total_queries * config.search.k) * 100.0);
    }
    printf("==========================================\n");
}


/**
 * Key processor callback for vector ID mapping
 * This function is called for each key discovered during cluster scan
 */
static int64_t vectorKeyProcessor(const char *key, void *user_data, int64_t thread_id) {
    char cluster_tag[6];
    uint64_t vector_id;
    int64_t prefix_len = strlen(config.search.prefix);
    char prefix[256];
    cluster_tag[6] = '\0';
    if (decode_vector_key_fixed(key,
                               prefix, sizeof(prefix),
                               cluster_tag, sizeof(cluster_tag) ,
                               &vector_id) != 0) {
        fprintf(stderr, "Failed to decode key: %s\n", key);
        exit(1);
    }
    if (strncmp(prefix, config.search.prefix, prefix_len) != 0) {
        fprintf(stderr, "Key %s prefix mismatch: expected '%s', got '%s'\n", key, config.search.prefix, prefix);
        /* Key prefix doesn't match index prefix */
        exit(1);
    }
    // if vector id is larger than dataset_num_vectors, skip it
    if (config.use_dataset && vector_id >= config.dataset_num_vectors) {
        return 0; /* Continue processing other keys */
    }
    addClusterTagMapping(&cluster_tag_map, vector_id, cluster_tag);
    /* Key format doesn't match expected vector key pattern */
    return 0;  /* Continue processing other keys */
}

/* Fast unique vector generation using key-based deterministic randomization */
static sds createVectorTemplate(uint64_t key_idx) {
    float* vector = zmalloc(config.search.vector_dim * sizeof(float));
    int64_t ph_index = -1;
    char pattern = 0;
    int64_t pattern_offset = 0;
    int64_t ph_offset = 0;
    /* Dataset mode - placeholder for entire vector */
    if (config.use_dataset) {
        ph_index = DATASET_VECTOR_PLACEHOLDER_INDEX;
        pattern = 0xDD;
        pattern_offset = PLACEHOLDERS[ph_index].len;
    } else {
        ph_index = 0;
        // Original implementation: use 
        int64_t dim = config.search.vector_dim - VECTOR_NUM_RAND_DIM;
        ph_offset = dim * sizeof(float);
        /* Use multiple hash passes for better distribution */
        uint64_t hash1 = key_idx * 0x9E3779B97F4A7C15ULL;
        uint64_t hash2 = key_idx * 0xBF58476D1CE4E5B9ULL;
        
        /* Generate full vector with mixed entropy sources */
        for (int64_t i = 0; i < dim; i++) {
            /* Mix key_idx, dimension index, and hash values */
            uint64_t mixed = hash1 ^ (hash2 + i);
            mixed *= 0x94D049BB133111EBULL;
            mixed ^= mixed >> 31;
            mixed *= 0xBF58476D1CE4E5B9ULL;
            mixed ^= mixed >> 31;
            
            /* Convert to float in [-1, 1] with good distribution */
            uint32_t bits = (uint32_t)(mixed >> 32);
            vector[i] = (float)((int32_t)bits) / 2147483648.0f;
        }

        /* Optional: Normalize vector for cosine similarity
        NOT REALLY WORKING BEFORE REPLACEMENT */
        if (strcmp(config.search.metric, "COSINE") == 0) {
            float norm = 0.0f;
            for (int64_t i = 0; i < dim; i++) {
                norm += vector[i] * vector[i];
            }
            norm = sqrtf(norm);
            if (norm > 0.0f) {
                for (int64_t i = 0; i < dim; i++) {
                    vector[i] /= norm;
                }
            }
        }
    }
    memcpy((char*)vector + ph_offset, PLACEHOLDERS[ph_index].name,
           PLACEHOLDERS[ph_index].len);
    /* Fill rest with recognizable pattern */
    memset((uint8_t*)vector + pattern_offset, pattern,
           config.search.vector_dim * sizeof(float) - PLACEHOLDERS[ph_index].len);

    sds vec = sdsnewlen(vector, config.search.vector_dim * sizeof(float));
    zfree(vector);
    return vec;
}


/* Helper function to print reply structure for debugging */
static void debugPrintReplyStructure(valkeyReply *reply, int64_t depth, int64_t max_depth) {
    if (!reply || depth > max_depth) return;
    
    const char *indent = "                                        "; // 40 spaces
    int64_t indent_len = depth * 2;
    if (indent_len > 40) indent_len = 40;
    
    const char *type_str = "UNKNOWN";
    switch (reply->type) {
        case VALKEY_REPLY_STRING: type_str = "STRING"; break;
        case VALKEY_REPLY_ARRAY: type_str = "ARRAY"; break;
        case VALKEY_REPLY_INTEGER: type_str = "INTEGER"; break;
        case VALKEY_REPLY_NIL: type_str = "NIL"; break;
        case VALKEY_REPLY_STATUS: type_str = "STATUS"; break;
        case VALKEY_REPLY_ERROR: type_str = "ERROR"; break;
        default: break;
    }

    fprintf(stderr, "%.*s[%s]", (int)indent_len, indent, type_str);

    switch (reply->type) {
        case VALKEY_REPLY_STRING:
        case VALKEY_REPLY_STATUS:
        case VALKEY_REPLY_ERROR:
            fprintf(stderr, " \"%.*s\"%s\n", 
                    (int)(reply->len > 60 ? 60 : reply->len), 
                    reply->str,
                    reply->len > 60 ? "..." : "");
            break;
        case VALKEY_REPLY_INTEGER:
            fprintf(stderr, " %lld\n", reply->integer);
            break;
        case VALKEY_REPLY_ARRAY:
            fprintf(stderr, " (elements: %zu)\n", reply->elements);
            for (size_t i = 0; i < reply->elements && i < 20; i++) {
                fprintf(stderr, "%.*s  [%zu]: ", (int)indent_len, indent, i);
                debugPrintReplyStructure(reply->element[i], depth + 1, max_depth);
            }
            if (reply->elements > 20) {
                fprintf(stderr, "%.*s  ... (%zu more elements)\n", 
                        (int)indent_len, indent, reply->elements - 20);
            }
            break;
        default:
            fprintf(stderr, "\n");
            break;
    }
    // assert(0);
}

/* Print FT.SEARCH results in a user-friendly format */
static void processQueryResults(valkeyReply *reply, uint64_t query_idx) {
    if (!reply || reply->type != VALKEY_REPLY_ARRAY) {
        fprintf(stderr, "ERROR: Invalid search result format - reply is %s, type is %d\n",
                !reply ? "NULL" : "not an ARRAY", reply ? reply->type : -1);
        if (reply) {
            fprintf(stderr, "Reply structure:\n");
            debugPrintReplyStructure(reply, 0, 3);
        }
        return;
    }
    
    if (reply->elements < 1) {
        fprintf(stderr, "ERROR: No search results - reply->elements %zu\n", reply->elements);
        debugPrintReplyStructure(reply, 0, 3);
        assert(0);
    }
    if (config.print_search_results) {
        // lock mutex to prevent interleaved prints
        pthread_mutex_lock(&recall_stats_mutex);
    }
    int64_t num_elements = 0;

    /* First element is the total number of results */
    if (reply->element[0]->type == VALKEY_REPLY_INTEGER) {
        printf_results("\n===Query %ld Search Results (Total: %lld) ===\n", query_idx, reply->element[0]->integer);
        num_elements = reply->element[0]->integer;
    }
    if (num_elements == 0) {
        printf_results("No search results - reply->elements %ld type %d\n", reply->elements, reply->element[0]->type);
         if (config.print_search_results) {
            // lock mutex to prevent interleaved prints
            pthread_mutex_unlock(&recall_stats_mutex);
        }
        debugPrintReplyStructure(reply, 0, 3);
        // assert(0);
        return;
    }

    /* Get ground truth from dataset */
    dataset_neighbors_t* query_neighbors;
    if (config.use_filtered_search) {
        /* Use filtered neighbors based on query predicates */
        query_neighbors = dataset_get_filtered_neighbors((dataset_ctx_t*)config.dataset_ctx, query_idx);
        
        /* Fallback to regular neighbors if filtered search not available (v1 dataset) */
        if (!query_neighbors) {
            query_neighbors = datasetGetNeighbors((dataset_ctx_t*)config.dataset_ctx, query_idx);
            
            /* Warn only once about missing metadata */
            static int64_t warned_no_metadata = 0;
            if (!warned_no_metadata && query_neighbors) {
                fprintf(stderr, "WARNING: --filtered specified but dataset has no metadata. Using unfiltered ground truth.\n");
                warned_no_metadata = 1;
            }
        }
    } else {
        /* Use regular neighbors */
        query_neighbors = datasetGetNeighbors((dataset_ctx_t*)config.dataset_ctx, query_idx);
    }
    if (!query_neighbors || query_neighbors->count == 0) {
        pthread_mutex_unlock(&recall_stats_mutex);
        printf("Failed to get ground truth for query index %lu\n", query_idx);
        assert(0);
    }
    /* Calculate number of results to display (limit to 20) */
    size_t total_results = (reply->elements - 1) / 2;
    size_t num_vecs_ids = 0;    
    if (config.search.nocontent) {
        total_results = reply->elements - 1; // Each element is a key
    }
    
    /* Debug: Check if we're getting the expected number of results */
    if (config.search_debug) {
        printf_results("DEBUG: config.search.k=%ld, reply->elements=%zu, calculated total_results=%zu, nocontent=%ld\n",
                      config.search.k, reply->elements, total_results, config.search.nocontent);
    }
    
    /* Use config.search.k as the expected size, but cap at actual results */
    size_t expected_results = (size_t)config.search.k;
    if (total_results < expected_results) {
        if (config.search_debug) {
            printf_results("INFO: Expected k=%ld results but reply contains only %zu results\n",
                          config.search.k, total_results);
        }
        expected_results = total_results;
    }
    
    /* Handle zero results case - this can happen with very restrictive filters */
    if (expected_results == 0) {
        if (config.use_filtered_search) {
            printf_results("INFO: Filter returned zero results (k=%ld)\n", config.search.k);
        } else {
            fprintf(stderr, "WARNING: Zero results returned for k=%ld (no filter applied)\n", 
                    config.search.k);
        }
        if (config.print_search_results) {
            pthread_mutex_unlock(&recall_stats_mutex);
        }
        /* Free filtered neighbors if allocated */
        if (config.use_filtered_search && query_neighbors) {
            dataset_free_neighbors(query_neighbors);
        }
        return;
    }
    
    uint64_t result_vec_ids[expected_results];
    uint64_t gt_vec_ids[expected_results];
    // size_t max_display = total_results > 100 ? 100 : total_results;
    // if (total_results > 100) {
    printf_results("  (Showing first 100 of %zu results, expecting k=%ld)\n", total_results, config.search.k);
    // }
    // size_t end_index = total_results * 2;
    
    /* Parse results based on actual reply structure */
    size_t result_idx = 1; /* Start after count element */
    
    while (result_idx < reply->elements && num_vecs_ids < total_results) {
        valkeyReply *current = reply->element[result_idx];
        
        if (!current) {
            fprintf(stderr, "WARNING: NULL element at index %zu, skipping\n", result_idx);
            result_idx++;
            continue;
        }
        
        /* Determine if this is a key or needs further inspection */
        char *vector_key = NULL;
        float score = -1.0;
        
        if (current->type == VALKEY_REPLY_STRING || current->type == VALKEY_REPLY_STATUS) {
            /* This is a key */
            vector_key = current->str;
            result_idx++;
            
            /* Check if next element is a fields array (both NOCONTENT and CONTENT modes in MemoryDB return arrays) */
            int64_t has_fields_array = 0;
            if (result_idx < reply->elements) {
                valkeyReply *next = reply->element[result_idx];
                if (next && next->type == VALKEY_REPLY_ARRAY) {
                    has_fields_array = 1;
                }
            }
            
            if (has_fields_array) {
                valkeyReply *fields = reply->element[result_idx];
                result_idx++;
                
                /* Parse fields array to extract score */
                for (size_t field_idx = 0; field_idx < fields->elements; field_idx += 2) {
                    if (field_idx + 1 >= fields->elements) break;
                    
                    valkeyReply *field_name_reply = fields->element[field_idx];
                    valkeyReply *field_value_reply = fields->element[field_idx + 1];
                    
                    if (!field_name_reply || !field_value_reply) continue;
                    
                    if ((field_name_reply->type == VALKEY_REPLY_STRING || 
                         field_name_reply->type == VALKEY_REPLY_STATUS) &&
                        strstr(field_name_reply->str, "_score") != NULL) {
                        
                        if (field_value_reply->type == VALKEY_REPLY_STRING ||
                            field_value_reply->type == VALKEY_REPLY_STATUS) {
                            score = atof(field_value_reply->str);
                        } else if (field_value_reply->type == VALKEY_REPLY_INTEGER) {
                            score = (float)field_value_reply->integer;
                        }
                        break;
                    }
                }
                
                if (score >= 0) {
                    printf_results("\n  Result %zu: %s [distance: %.6f]\n", 
                                 num_vecs_ids + 1, vector_key, score);
                } else {
                    printf_results("\n  Result %zu: %s\n", num_vecs_ids + 1, vector_key);
                }
            } else {
                /* Pure NOCONTENT mode (ElastiCache): just the key, no fields */
                printf_results("\nResult %zu: %s\n", num_vecs_ids + 1, vector_key);
            }
        } else if (current->type == VALKEY_REPLY_ARRAY) {
            /* Unexpected array - might be out of sync, try to recover */
            fprintf(stderr, "WARNING: Unexpected ARRAY at index %zu (expected key), attempting to skip\n", 
                    result_idx);
            debugPrintReplyStructure(current, 0, 2);
            result_idx++;
            continue;
        } else {
            fprintf(stderr, "ERROR: Unexpected element type %d at index %zu (expected STRING/STATUS for key)\n",
                    current->type, result_idx);
            fprintf(stderr, "Reply structure:\n");
            debugPrintReplyStructure(reply, 0, 3);
            result_idx++;
            continue;
        }
        
        /* Extract vector ID using centralized decoding function */
        if (vector_key) {
            char cluster_tag[16];
            uint64_t vector_id;
            
            if (decode_vector_key_fixed(vector_key,
                                       NULL, 0,
                                       cluster_tag, sizeof(cluster_tag),
                                       &vector_id) != 0) {
                fprintf(stderr, "WARNING: Failed to decode key: %s, skipping\n", vector_key);
                continue;
            }
            
            /* Ensure we don't exceed allocated array bounds */
            if (num_vecs_ids >= expected_results) {
                fprintf(stderr, "FATAL ERROR: Received more results (%zu) than expected (%zu). "
                        "This indicates a mismatch between k=%ld and actual reply structure.\n",
                        num_vecs_ids + 1, expected_results, config.search.k);
                fprintf(stderr, "Reply structure:\n");
                debugPrintReplyStructure(reply, 0, 3);
                assert(0);
            }
            
            gt_vec_ids[num_vecs_ids] = query_neighbors->ids[num_vecs_ids];
            result_vec_ids[num_vecs_ids++] = vector_id;
        }
    }
    
    /* Verify we got the expected number of results */
    /* With filters, we may get fewer results than k */
    if (num_vecs_ids > (size_t)config.search.k) {
        fprintf(stderr, "FATAL ERROR: Received more results (%zu) than requested k=%ld\n", 
                num_vecs_ids, config.search.k);
        fprintf(stderr, "Reply structure:\n");
        debugPrintReplyStructure(reply, 0, 3);
        assert(0);
    }
    
    if (num_vecs_ids != (size_t)config.search.k && !config.use_filtered_search) {
        fprintf(stderr, "WARNING: Expected k=%ld results but got %zu (total_results=%zu). "
                "This is unexpected without filters.\n",
                config.search.k, num_vecs_ids, total_results);
    }
    
    if (config.use_filtered_search && num_vecs_ids < (size_t)config.search.k) {
        if (config.search_debug) {
            printf_results("INFO: Filter reduced results from k=%ld to %zu vectors\n",
                          config.search.k, num_vecs_ids);
        }
    }
    
    /* Sanity check: ensure num_vecs_ids doesn't exceed allocated array size */
    if (num_vecs_ids > expected_results) {
        fprintf(stderr, "FATAL ERROR: Internal error - num_vecs_ids (%zu) exceeds expected_results (%zu)\n",
                num_vecs_ids, expected_results);
        assert(0);
    }
    
    qsort(result_vec_ids, num_vecs_ids, sizeof(uint64_t), uint64_cmp);
    qsort(gt_vec_ids, num_vecs_ids, sizeof(uint64_t), uint64_cmp);
    /* Compare with ground truth if available */
    if (config.use_dataset && num_vecs_ids > 0) {
        int64_t matches = 0;
        int64_t i;
        for (i = 0; i < num_vecs_ids; i++) {
            if (result_vec_ids[i] == gt_vec_ids[i]) {
                matches++;
                int64_t query_index = datasetGetQueryIxByNeighbor((dataset_ctx_t*)config.dataset_ctx, result_vec_ids[i], 0);
                int64_t has_more_query_sources = datasetGetQueryIxByNeighbor((dataset_ctx_t*)config.dataset_ctx, result_vec_ids[i], 1);
                printf_results("  [SR == GT] %lu, [MATCH], is neighbor of query_index %ld, %s %ld\n",
                   result_vec_ids[i], query_index, has_more_query_sources > 0 ? "(multiple query sources)" : "", has_more_query_sources + 1);
            } else {
                break;
            }
        }
        int64_t num_perfect_matches = i;
        printf_results("\n=== ZZZ Ground Truth Comparison [perfect match %ld]===\n", num_perfect_matches);
        for (; i < num_vecs_ids; i++) {
            int64_t found = -1;
            /* Only search within the ground truth we actually have */
            for (uint32_t j = num_perfect_matches; j < num_vecs_ids; j++) {
                if (result_vec_ids[i] == gt_vec_ids[j]) {
                    matches++;
                    found = j;
                    break;
                }
            }
            int64_t query_index = datasetGetQueryIxByNeighbor((dataset_ctx_t*)config.dataset_ctx, result_vec_ids[i], 0);
            int64_t has_more_query_sources = datasetGetQueryIxByNeighbor((dataset_ctx_t*)config.dataset_ctx, result_vec_ids[i], 1);
            printf_results("  [SR vs GT] %lu vs %lu, %s(found in GT at index %ld), is neighbor of query_index %ld, %s %ld\n",
                   result_vec_ids[i], gt_vec_ids[i], (found >= 0) ? "[MATCH]" : "[MISS]", found, query_index, has_more_query_sources > 0 ? "(multiple query sources)" : "", has_more_query_sources + 1);
        }
        float recall = (float)matches / num_vecs_ids;
        printf_results("  Total matches: %ld out of %zu, Recall: %.2f%%\n", matches, num_vecs_ids, recall * 100.0f);

        if (config.print_search_results) {
            pthread_mutex_unlock(&recall_stats_mutex);
        }
        updateRecallStats(recall);
        // perform the extended checkNeighbors for 10% of the responses randomly
        static __thread int64_t num_results = 1;
        if (num_results % 10000000 == 0) {
            recall = checkNeighbors(query_idx, result_vec_ids, num_vecs_ids, query_neighbors);
            updateRecallStatsExt(recall);
        }
        num_results++;
    } else if (config.print_search_results) {
        pthread_mutex_unlock(&recall_stats_mutex);
    }
    
    /* Free filtered neighbors if allocated */
    if (config.use_filtered_search && query_neighbors) {
        dataset_free_neighbors(query_neighbors);
    }
    
    printf_results("\n");
}

int isSelected(int64_t is_primary) {
    if (((config.read_from_replica == FROM_REPLICA_ONLY) && is_primary) || 
        ((config.read_from_replica == FROM_PRIMARY_ONLY) && !is_primary)) {
        return 0;
    }
    return 1;
}



static uint64_t dictSdsHash(const void *key) {
    return dictGenHashFunction((unsigned char *)key, sdslen((char *)key));
}

static int dictSdsKeyCompare(const void *key1, const void *key2) {
    int64_t l1, l2;
    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

static dictType dtype = {
    dictSdsHash,       /* hash function */
    NULL,              /* key dup */
    dictSdsKeyCompare, /* key compare */
    NULL,              /* key destructor */
    NULL,              /* val destructor */
    NULL               /* allow to expand */
};

valkeyContext *getValkeyContext(enum valkeyConnectionType ct, const char *ip_or_path, int port) {
    valkeyContext *ctx = NULL;
    valkeyReply *reply = NULL;
    struct timeval tv = {0};

    ctx = valkeyConnectWrapper(ct, ip_or_path, port, tv, 0, config.mptcp);
    printf("Connecting to %s", (ct != VALKEY_CONN_UNIX ? ip_or_path : ""));
    if (ct != VALKEY_CONN_UNIX) printf(":%d", port);
    printf("... ");
    fflush(stdout);
    if (ctx == NULL || ctx->err) {
        fprintf(stderr, "Could not connect to server at ");
        char *err = (ctx != NULL ? ctx->errstr : "");
        if (ct != VALKEY_CONN_UNIX)
            fprintf(stderr, "%s:%d: %s\n", ip_or_path, port, err);
        else
            fprintf(stderr, "%s: %s\n", ip_or_path, err);
        assert(0);
    }
    if (config.tls == 1) {
        printf("Negotiating TLS connection...\n");
        const char *err = NULL;
        if (cliSecureConnection(ctx, config.sslconfig, &err) == VALKEY_ERR && err) {
            fprintf(stderr, "Could not negotiate a TLS connection: %s\n", err);
            assert(0);
        }
    }
    if (config.conn_info.auth == NULL) goto cleanup;
    if (config.conn_info.user == NULL)
        reply = valkeyCommand(ctx, "AUTH %s", config.conn_info.auth);
    else
        reply = valkeyCommand(ctx, "AUTH %s %s", config.conn_info.user, config.conn_info.auth);
    if (reply != NULL) {
        if (reply->type == VALKEY_REPLY_ERROR) {
            if (ct != VALKEY_CONN_UNIX)
                fprintf(stderr, "Node %s:%d replied with error:\n%s\n", ip_or_path, port, reply->str);
            else
                fprintf(stderr, "Node %s replied with error:\n%s\n", ip_or_path, reply->str);
            freeReplyObject(reply);
            valkeyFree(ctx);
            assert(0);
        }
        goto cleanup;
    }
    fprintf(stderr, "ERROR: failed to fetch reply from ");
    if (ct != VALKEY_CONN_UNIX)
        fprintf(stderr, "%s:%d\n", ip_or_path, port);
    else
        fprintf(stderr, "%s\n", ip_or_path);
cleanup:
    freeReplyObject(reply);
    printf("Connected successfully to %s:%d.\n", ip_or_path, port);
    return ctx;
}

/* Fast vector generation using MT19937-64 */
static inline void generate_vector_fast(float *vector, unsigned int key_idx) {
    /* Use key index to seed global RNG */
    init_genrand64(key_idx * 2654435761U);
    
    /* Generate vector components */
    for (int64_t i = 0; i < config.search.vector_dim; i++) {
        uint64_t r = genrand64_int64();
        /* Convert to float in range [-1, 1] */
        vector[i] = ((float)(r & 0x7FFFFFFF) / 0x40000000) - 1.0f;
    }
}

/* Initialize base vector for efficient generation */
static void initBaseVector(int64_t dim) {
    if (base_vector && base_vector_dim != dim) {
        zfree(base_vector);
        base_vector = NULL;
    }
    
    if (!base_vector) {
        base_vector_dim = dim;
        base_vector = zcalloc(sizeof(float) * dim);
        
        /* Initialize with random values */
        init_genrand64(42); /* Fixed seed for reproducibility */
        
        for (int64_t i = 0; i < dim; i++) {
            uint64_t r = genrand64_int64();
            base_vector[i] = ((float)(r & 0x7FFFFFFF) / 0x40000000) - 1.0f;
        }
    }
}

static sds getSearchKeyTemplate(void) {
    int64_t ph_index = 0;
    size_t key_len = strlen(config.search.prefix) + 1; // +1 for ':'
    if (config.cluster_mode) {
        key_len += PLACEHOLDERS[CLUSTER_PLACEHOLDER_INDEX].len;
    }
    if (config.use_dataset) {
        ph_index = DATASET_KEY_PLACEHOLDER_INDEX;
    } else {
        ph_index = 0; 
    }
    key_len += PLACEHOLDERS[ph_index].len;
    sds key = sdsnewlen("", key_len);
    int64_t ret = 0;
    /* Dataset mode - placeholder for entire vector */
    if (config.cluster_mode) {
        ret = snprintf(key, key_len+1, "%s{clt}:%s", config.search.prefix, PLACEHOLDERS[ph_index].name);
    } else {
        ret = snprintf(key, key_len+1, "%s:%s", config.search.prefix, PLACEHOLDERS[ph_index].name);
    }    
    (void)ret; /* Used in assert */
    assert(ret == (int64_t)(key_len)); // -1 for null terminator    
    return key;
}

// build tag that is of length config.search.payload_tag_len and starts with PLACEHOLDERS[DATASET_TAG_PLACEHOLDER_INDEX].name
static sds createTagTemplate(void) {
    // create DUMMY sds string with 'Z' char pattern of length config.search.payload_tag_len - PLACEHOLDERS[DATASET_TAG_PLACEHOLDER_INDEX].len
    // sds dummy_str = sdsgrownonzero(sdsempty(), config.search.payload_tag_len - PLACEHOLDERS[DATASET_TAG_PLACEHOLDER_INDEX].len, 'Z');

    // create a string of len config.search.payload_tag_len that has repeating 'SHOULD-REPLACE' pattern
    sds tag = sdscatprintf(sdsempty(), "%s", PLACEHOLDERS[DATASET_TAG_PLACEHOLDER_INDEX].name);
    tag = sdsgrownonzero(tag, config.search.payload_tag_len, 'Z');
    // tag = sdsgrowzero(tag, config.search.payload_tag_len);
    // printf("DEBUG: Created tag template: %s (len %zu) (expected len %zu) taglen %zu\n", tag, sdslen(tag), config.search.payload_tag_len, PLACEHOLDERS[DATASET_TAG_PLACEHOLDER_INDEX].len);

    assert(sdslen(tag) == config.search.payload_tag_len);
    // sdsfree(dummy_str);
    return tag;
}

void setArg(const char **argv, size_t *argvlen, int64_t *argc, const char* value, int64_t value_len) {
    assert(*argc < 20); /* Prevent buffer overflow in argv arrays */
    argv[(*argc)] = value;
    argvlen[(*argc)++] = value_len;
}
/* Benchmark function for vector operations with cluster awareness 
 * Supports multiple tag and numeric fields dynamically */
static int64_t createSearchHsetTemplate(char **cmd) {
    /* Generate key with appropriate cluster tag */    
    sds key = getSearchKeyTemplate();
    /* Validation checks */
    assert(config.search.vector_dim > 0 && config.use_search && config.search.vector_dim > VECTOR_NUM_RAND_DIM);        
    /* Build vector data: fixed part + placeholder */
    sds vector_binary = createVectorTemplate(0x736f6d6575736572); // "someusername" as base

    /* Build HSET command using argv approach 
     * Max fields: command + key + vector_field + vector_data + tag_field + tag_data + numeric_field + numeric_data
     * Allow room for multiple tag/numeric fields: 2 + 2 + (2*5) = 14 max args */
    const char *argv[20];
    size_t argvlen[20];
    int64_t argc = 0;
    
    /* Command and key */
    setArg(argv, argvlen, &argc, "HSET", 4);
    setArg(argv, argvlen, &argc, key, sdslen(key));
       
    /* Tag field (optional) - can be extended to support multiple tag fields
     * Future: could loop through an array of tag fields */
    sds selected_tag = NULL;
    if (config.search.tag_field && config.search.curr_conf.tag_dists) {
        selected_tag = createTagTemplate();
        setArg(argv, argvlen, &argc, config.search.tag_field, strlen(config.search.tag_field));
        setArg(argv, argvlen, &argc, selected_tag, sdslen(selected_tag));
        sdsfree(selected_tag);
    }
    
    /* Numeric field (optional) - can be extended to support multiple numeric fields
     * Future: could loop through an array of numeric fields */
    if (config.search.numeric_field) {
        setArg(argv, argvlen, &argc, config.search.numeric_field, strlen(config.search.numeric_field));
        setArg(argv, argvlen, &argc, PLACEHOLDERS[DATASET_NUMERIC_SCORE_PLACEHOLDER_INDEX].name, PLACEHOLDERS[DATASET_NUMERIC_SCORE_PLACEHOLDER_INDEX].len);
    }

    /* Vector field (always present) */
    setArg(argv, argvlen, &argc, config.search.vector_field, strlen(config.search.vector_field));
    setArg(argv, argvlen, &argc, vector_binary, sdslen(vector_binary));     

    int64_t len = valkeyFormatCommandArgv(cmd, argc, argv, argvlen);
    /* Cleanup allocated strings */
    sdsfree(key);
    sdsfree(vector_binary);
    return len;
}

/* Benchmark function for vector operations with cluster awareness */
static int64_t createSearchCmdTemplate(char **cmd) {
    sds index_name = sdsdup(config.search.name);
    /* Validation checks */
    printf("Creating FT.SEARCH command template for index '%s' algorithm %s dimension %ld rand-dim %ld k %ld ef_search %ld vector_field %s tag_field %s tag_filter %s nocontent %ld\n", 
        index_name, config.search.algorithm, config.search.vector_dim, VECTOR_NUM_RAND_DIM, 
        config.search.k, config.search.ef_search, config.search.vector_field, 
        config.search.tag_field, config.search.curr_conf.tag_filter, config.search.nocontent);
    assert(config.search.vector_dim > 0 && config.use_search && config.search.vector_dim > VECTOR_NUM_RAND_DIM);    
    /* Build vector data: fixed part + placeholder */
    sds vector_binary = createVectorTemplate(0x736f6d6575736572 ^ ((uint64_t)pthread_self() << 32)); // "someusername" as base
    
    /* Build KNN query */
    sds query;
    int64_t is_hnsw = (strcasecmp(config.search.algorithm, "hnsw") == 0);
    sds filter;
    if (config.search.curr_conf.tag_filter) {
        filter = sdscatprintf(sdsempty(), "@%s:{%s}", 
            config.search.tag_field, 
            config.search.curr_conf.tag_filter);
    } else {
        filter = sdscatprintf(sdsempty(), "*");
    } 
    if (is_hnsw) {
        query = sdscatprintf(sdsempty(), 
            "%s=>[KNN %ld @%s $%s EF_RUNTIME %ld]", 
            filter,
            config.search.k, 
            config.search.vector_field, 
            QUERY_VECTOR,
            config.search.ef_search);
    } else {
        query = sdscatprintf(sdsempty(), 
            "%s=>[KNN %ld @%s $%s]", 
            filter,
            config.search.k, 
            config.search.vector_field,
            QUERY_VECTOR);
    }   

    /* Build FT.SEARCH command using argv approach */
    sds score_field = sdscatprintf(sdsempty(), "__%s_score", config.search.vector_field);
    sds k_str = sdscatprintf(sdsempty(), "%ld", config.search.k);
    
    const char *argv[20];  /* Max arguments we might need */
    size_t argvlen[20];
    int64_t argc = 0;
    
    /* Command name */
    setArg(argv, argvlen, &argc, "FT.SEARCH", 9);
    
    /* Index name */
    setArg(argv, argvlen, &argc, index_name, sdslen(index_name));    
    
    /* Query */
    setArg(argv, argvlen, &argc, query, sdslen(query));
    
    /* LIMIT 0 k */
    setArg(argv, argvlen, &argc, "LIMIT", 5);
    setArg(argv, argvlen, &argc, "0", 1);
    setArg(argv, argvlen, &argc, k_str, sdslen(k_str));
           
    if (config.search.nocontent) {
        /* RETURN n score_field [vector_field] */
        setArg(argv, argvlen, &argc, "RETURN", 6);
        // setArg(argv, argvlen, &argc, "RETURN", 6);
        setArg(argv, argvlen, &argc, "0", 1);
    }
    /* LOCALONLY if needed */
    if (config.search.localonly) {
        setArg(argv, argvlen, &argc, "LOCALONLY", 9);
    }
    
    /* PARAMS 2 query_vector <vector> */
    setArg(argv, argvlen, &argc, "PARAMS", 6);
    setArg(argv, argvlen, &argc, "2", 1);
    setArg(argv, argvlen, &argc, QUERY_VECTOR, 12);
    setArg(argv, argvlen, &argc, vector_binary, sdslen(vector_binary));
    
    int64_t len = valkeyFormatCommandArgv(cmd, argc, argv, argvlen);
    
    sdsfree(k_str);
    sdsfree(score_field);
    sdsfree(query);
    sdsfree(vector_binary);
    sdsfree(index_name);
    return len;
}

static void replacePlaceholderClusterTag(client c, const size_t *indices, const size_t count, char *cmd, _Atomic uint64_t *key_counter) {
    assert(c->thread_id >= 0);
    clusterNode *node = c->cluster_node;
    assert(node);
    int64_t is_updating_slots = atomic_load_explicit(&config.is_updating_slots, 
                                                 memory_order_relaxed);                                                 
    if (is_updating_slots) updateClusterSlotsConfiguration();
    // update key with counter to ensure different keys
    uint64_t key_idx = atomic_fetch_add_explicit(key_counter, 1, memory_order_relaxed);
    assert(node->slots_count > 0);
    /* Select a random slot from this node */
    int64_t slot = node->slots[key_idx % node->slots_count];
    const char *tag = crc16_slot_table[slot];
    int64_t taglen = strlen(tag);
    assert(taglen <= 3); /* Ensure tag fits within placeholder */
    
    /* Replace all occurrences in-place (exactly 8 bytes) */
    for (size_t j = 0; j < count; j++) {
        char *placeholder = cmd + indices[j];    
        assert(placeholder[0] == '{');
        memcpy(placeholder + 1, tag, taglen);  // Copy tag
        placeholder[1 + taglen] = '}';         // Closing brace
        // Pad remaining bytes with random data if tag is shorter than 3 bytes
        if (taglen < 3) {
            for (int64_t k = 0; k < 3 - taglen; k++) {
                placeholder[1 + taglen + 1 + k] = 'a' + (rand() % 26); // Random lowercase letter
            }
        }
    }
}


// TODO: If index already exists, we shuld check if it matches the current configuration.
// If it does not match, we should drop the index and recreate it.
// If it matches, we can skip index creation.
static void createDefaultSearchIndexes(void) {    
    if (!config.use_search) return;
    // connect to a primary node
    if (config.cluster_primary_nodes[0]) {
        config.conn_info.hostip = config.cluster_primary_nodes[0]->ip;
        config.conn_info.hostport = config.cluster_primary_nodes[0]->port;
    }
    printf("[cluster-mode:%ld] Creating search index '%s' on %s:%d if it does not exist...\n", 
           config.cluster_mode, config.search.name, config.conn_info.hostip, config.conn_info.hostport);
    fflush(stdout);
    valkeyContext *ctx = getValkeyContext(config.ct, config.conn_info.hostip, config.conn_info.hostport);
        
    if (ctx == NULL) {
        fprintf(stderr, "Failed to connect to server for index creation\n");
        fflush(stderr);
        assert(0);
    }
    // }
    int64_t num_indexes = 1;
    sds indexes_to_create[2] = {config.search.name, NULL};
    sds algorithms[2] = {config.search.algorithm, NULL};

    for (int64_t i = 0; i < num_indexes; i++) {        
        /* Check if any indexes exist */
        valkeyReply *list_reply = valkeyCommand(ctx, "FT._LIST");
        int64_t index_exists = 0;

        if (list_reply && list_reply->type == VALKEY_REPLY_ARRAY) {
            printf("Found %zu existing indexes: ", list_reply->elements);
            for (size_t j = 0; j < list_reply->elements; j++) {
                printf("found index '%s' ", list_reply->element[j]->str);
                if (strcmp(list_reply->element[j]->str, indexes_to_create[i]) == 0) {
                    index_exists = 1;
                    getFullInfo(indexes_to_create[i], config.selected_node_count, config.cluster_nodes, config.ct);
                    if (config.clean) {
                        printf("Dropping existing index '%s' as --clean is specified\n", indexes_to_create[i]);
                        valkeyReply *drop_reply = valkeyCommand(ctx, "FT.DROPINDEX %s", indexes_to_create[i]);
                        if (drop_reply && (drop_reply->type == VALKEY_REPLY_STRING || drop_reply->type == VALKEY_REPLY_STATUS)) {
                            printf("Index dropped successfully\n");
                            index_exists = 0; /* Will recreate */
                        } else {
                            fprintf(stderr, "Failed to drop index: %s\n", 
                                    drop_reply ? drop_reply->str : "Unknown error");
                            assert(0);
                        }
                        if (drop_reply) freeReplyObject(drop_reply);
                    } else {
                        printf("Index '%s' already exists, skipping creation.\n", indexes_to_create[i]);
                    }
                }            
            }
            printf("\n");
        } else {
            printf("Found 0 existing indexes: \n");
        }
        
        if (list_reply) {
            freeReplyObject(list_reply);
        }
        if (index_exists) {
            printf("Index '%s' already exists, skipping creation.\n", indexes_to_create[i]);
            continue;
        }
        valkeyReply *reply = NULL;
        if (strcmp(algorithms[i], "hnsw") == 0) {
            /* Add TAG field if configured */
            if (config.search.tag_field) {            
                reply = valkeyCommand(ctx, "FT.CREATE %s PREFIX 1 %s SCHEMA %s TAG %s VECTOR %s 12 TYPE FLOAT32 DIM %ld DISTANCE_METRIC %s M %ld EF_CONSTRUCTION %ld EF_RUNTIME %ld",
                indexes_to_create[i], config.search.prefix, config.search.tag_field, config.search.vector_field, algorithms[i], config.search.vector_dim, config.search.metric, config.search.m,
                config.search.ef_construction, config.search.ef_search);
            } else {
                reply = valkeyCommand(ctx, "FT.CREATE %s PREFIX 1 %s SCHEMA %s VECTOR %s 12 TYPE FLOAT32 DIM %ld DISTANCE_METRIC %s M %ld EF_CONSTRUCTION %ld EF_RUNTIME %ld",
                indexes_to_create[i], config.search.prefix, config.search.vector_field, algorithms[i], config.search.vector_dim, config.search.metric, config.search.m,
                config.search.ef_construction, config.search.ef_search);
            }
        } else {
            /* Add TAG field if configured */
            if (config.search.tag_field) {            
                reply = valkeyCommand(ctx, "FT.CREATE %s PREFIX 1 %s SCHEMA %s TAG %s VECTOR %s 6 TYPE FLOAT32 DIM %ld DISTANCE_METRIC %s",
                indexes_to_create[i], config.search.prefix, config.search.tag_field, config.search.vector_field, algorithms[i], config.search.vector_dim, config.search.metric);
            } else {
                reply = valkeyCommand(ctx, "FT.CREATE %s PREFIX 1 %s SCHEMA %s VECTOR %s 6 TYPE FLOAT32 DIM %ld DISTANCE_METRIC %s",
                indexes_to_create[i], config.search.prefix, config.search.vector_field, algorithms[i], config.search.vector_dim, config.search.metric);
            }
        }
        if (reply && (reply->type == VALKEY_REPLY_STRING || reply->type == VALKEY_REPLY_STATUS)) {
            printf("Index created successfully\n");
        } else {
            fprintf(stderr, "Failed to create index: %s\n", 
                    reply ? reply->str : "Unknown error");
            // if index already exists, we can ignore the error
            if (reply && reply->type == VALKEY_REPLY_ERROR && index_exists) {
                printf("Index '%s' already exists, ignoring error.\n", indexes_to_create[i]);
            } else {
                fprintf(stderr, "Error creating index: %s\n", reply ? reply->str : "Unknown error");
                assert(0);
            }
        }
        if (reply) freeReplyObject(reply);    
    }        
    /* Only free the duplicated index name, not the original config.search.name */
    if (num_indexes > 1) {
        if (algorithms[1]) sdsfree(algorithms[1]);
        if (indexes_to_create[1]) sdsfree(indexes_to_create[1]);
    }
    
    /* Free the context if we created a new one */
    if (ctx != config.conn_ctx) {
        valkeyFree(ctx);
    }
}

/* Best-effort server config fetch: use INFO; skip CONFIG on managed services */
static void safeGetServerConfig(enum valkeyConnectionType ct, const char *host, int port, serverConfig *dst) {
    valkeyContext *ctx = getValkeyContext(ct, host, port);
    if (!ctx) return;

    /* 1) INFO server (non-privileged, should work on ElastiCache) */
    valkeyReply *r = valkeyCommand(ctx, "INFO SERVER");
    if (r && (r->type == VALKEY_REPLY_STRING || r->type == VALKEY_REPLY_STATUS)) {
        /* parse into dst if you want; or store raw */
        /* ... your existing parsing hook ... */
        freeReplyObject(r);
        r = NULL;
    } else if (r) { freeReplyObject(r); r = NULL; }

    /* 2) Optionally INFO memory (also non-privileged) */
    r = valkeyCommand(ctx, "INFO MEMORY");
    if (r && (r->type == VALKEY_REPLY_STRING || r->type == VALKEY_REPLY_STATUS)) {
        /* parse into dst if you want */
        freeReplyObject(r); r = NULL;
    } else if (r) { freeReplyObject(r); r = NULL; }

    valkeyFree(ctx);
}

static void freeServerConfig(serverConfig *cfg) {
    if (cfg->save) sdsfree(cfg->save);
    if (cfg->appendonly) sdsfree(cfg->appendonly);
    zfree(cfg);
}

void resetPlaceholders(void) {
    if (placeholders.index_data)
        zfree(placeholders.index_data); /* indices are a single contiguous allocation */
    memset(&placeholders, 0, sizeof(placeholders));
    for (size_t placeholder = 0; placeholder < PLACEHOLDER_NUM_OF; placeholder++) {
        placeholders.indices[placeholder] = NULL;
        placeholders.count[placeholder] = 0;
        placeholders.len[placeholder] = PLACEHOLDERS[placeholder].len;
               
    }
}

void initPlaceholders(const char *cmd, size_t cmd_len) {
    resetPlaceholders();
    placeholders.cmd_len = cmd_len;

    /* store placeholder locations in temp arrays */
    size_t total_count = 0;
    size_t *temp_indices[PLACEHOLDER_NUM_OF];
    for (size_t placeholder = 0; placeholder < PLACEHOLDER_NUM_OF; placeholder++) {
        size_t *count = &placeholders.count[placeholder];
        *count = 0;

        size_t temp_size = RANDPTR_INITIAL_SIZE;
        temp_indices[placeholder] = zcalloc(sizeof(size_t) * temp_size);
        const char *p = cmd;
        const char *end = cmd + cmd_len;
        while ((p = strstr(p, PLACEHOLDERS[placeholder].name)) != NULL && p < end) {
            if (*count == temp_size) {
                temp_size *= 2;
                temp_indices[placeholder] = zrealloc(temp_indices[placeholder], sizeof(size_t) * temp_size);
            }
            size_t index = p - cmd;
            temp_indices[placeholder][*count] = index;
            (*count)++;
            total_count++;
            /* Move past the placeholder - vector placeholder has different length */
            p += placeholders.len[placeholder];
        }
    }

    /* consolidate temp data into contiguous allocation */
    placeholders.index_data = zcalloc(sizeof(size_t) * total_count);
    size_t overall_index = 0;
    for (size_t placeholder = 0; placeholder < PLACEHOLDER_NUM_OF; placeholder++) {
        placeholders.indices[placeholder] = placeholders.index_data + overall_index;

        const size_t count = placeholders.count[placeholder];
        memcpy(placeholders.indices[placeholder], temp_indices[placeholder],
               sizeof(size_t) * count);
        overall_index += count;

        zfree(temp_indices[placeholder]);
    }
}

static void replacePlaceholder(const size_t *indices, const size_t count, char *cmd, _Atomic uint64_t *key_counter, unsigned placeholder_len) {
    if (count == 0) return;

    uint64_t key = 0;
    if (config.keyspacelen != 0) {
        if (config.sequential_replacement) {
            key = atomic_fetch_add_explicit(key_counter, 1, memory_order_relaxed);
        } else {
            key = random();
        }
        key %= config.keyspacelen;
    }

    /* convert key to string at first location */
    char *p = cmd + indices[0] + placeholder_len - 1;
    for (size_t j = 0; j < placeholder_len; j++) {
        *p = '0' + key % 10;
        key /= 10;
        p--;
    }

    /* copy the first instance to the other locations */
    for (size_t i = 1; i < count; i++) {
        char *placeholder = cmd + indices[i];
        memcpy(placeholder, cmd + indices[0], placeholder_len);
    }
}

static void replacePlaceholderVector(const size_t *indices, const size_t count, 
                                    char *cmd, _Atomic uint64_t *key_counter) {
    if (!config.use_search || count == 0) return;
    
    /* Self-check: ensure placeholder is exactly 8 bytes */
    assert(PLACEHOLDERS[VECTOR_PLACEHOLDER_INDEX].len == 8);
    
    /* Get key for randomization */
    uint64_t key = 0;
    if (config.keyspacelen != 0) {
        if (config.sequential_replacement) {
            key = atomic_load_explicit(key_counter, memory_order_relaxed);
        } else {
            key = random();
        }
        key %= config.keyspacelen;
    }
    
    /* Generate exactly 2 floats (8 bytes) */
    float vector[2];
    uint64_t state = key ? key : 0x123456789ABCDEF0ULL;
    
    for (int64_t i = 0; i < 2; i++) {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        state *= 0x2545F4914F6CDD1DULL;
        
        uint32_t bits = (uint32_t)(state >> 32);
        vector[i] = ((float)(int32_t)bits) / 2147483648.0f;
    }
    
    /* Normalize if using COSINE metric */
    if (config.search.metric && strcmp(config.search.metric, "COSINE") == 0) {
        float norm = sqrtf(vector[0] * vector[0] + vector[1] * vector[1]);
        if (norm > 0.0f) {
            vector[0] /= norm;
            vector[1] /= norm;
        }
    }
    
    /* Replace all occurrences in-place (exactly 8 bytes) */
    for (size_t j = 0; j < count; j++) {
        char *placeholder = cmd + indices[j];        
        memcpy(placeholder, vector, PLACEHOLDERS[VECTOR_PLACEHOLDER_INDEX].len);  // Exactly 8 bytes replacement
    }
}


/* Main dataset replacement function */
// here we need to update the keys and vectors with data from dataset.
// we will need to update in place, on the cmd buffer.
// the indices are tell us the offesets in the cmd buffer to update.
// the count tells us how many commands there are. This is why we expect keys and vectors to be the same count. in case of insert\overwrite.
// in case of search, we will only have indices for vectors, and count for vectors.
// in case of delete, we will only have indices for keys, and count for keys
// Ideally, there should be no memory allocation here, we just need to copy the data to the appropriate place in the cmd buffer.
// the dataset api hides all the key association (what key to provide so it will match the vector, because we will need later to use this to calculate recall)

static void replacePlaceholderDataset(
    client c,
    int64_t thread_id,
    const size_t key_count, const size_t *key_indices,
    _Atomic uint64_t *key_counter,
    const size_t vec_count, const size_t *vec_indices,
    _Atomic uint64_t *vector_counter,
    const size_t cluster_tag_count, const size_t *cluster_tag_indices,
    const size_t tag_count, const size_t *tag_indices,
    _Atomic uint64_t *tag_counter,
    char *cmd)
{
    /* INSERT/PREFILL: both key and vector replacement */
    if (key_count > 0 && vec_count > 0) {
        uint64_t dataset_prefill_counter = atomic_load(&config.dataset_prefill_counter);
        // if dataset_prefill_counter >= keyspacelen, then dataset is prefilled
        int64_t dataset_prefilled = dataset_prefill_counter >= config.keyspacelen;
        /* Validate placeholder consistency */
        assert((key_count == vec_count) || (vec_count == 0) || (key_count == 0));
        // if (config.cluster_mode) {
        // dataset is prefilled
        dataset_prefilled = getClusterTagMapCount(&cluster_tag_map) >= config.dataset_num_vectors;        
        for (size_t i = 0; i < key_count; i++) {
            assert(key_count == tag_count || tag_count == 0);
            assert(key_count == cluster_tag_count || cluster_tag_count == 0);
            uint64_t vector_id;
            char *key_write_pos = cmd + key_indices[i];
            int64_t cluster_tag_len = config.cluster_mode ? PLACEHOLDERS[CLUSTER_PLACEHOLDER_INDEX].len : 0;
            char *key = key_write_pos - strlen(config.search.prefix) - cluster_tag_len - 1;
            int64_t key_len = strlen(config.search.prefix)+ cluster_tag_len + PLACEHOLDERS[DATASET_KEY_PLACEHOLDER_INDEX].len + 1;
            float *vec_write_pos = (float *)(cmd + vec_indices[i]);
            const char* cluster_tag = NULL;
            uint64_t dataset_idx = 0;
            
            if (!dataset_prefilled) {
                /* Determine dataset index to use */
                do {
                    dataset_idx = atomic_fetch_add_explicit(&config.dataset_prefill_counter, 1, memory_order_relaxed);
                    dataset_idx %= config.dataset_num_vectors; 
                    dataset_prefilled = (getClusterTagMapCount(&cluster_tag_map) >= config.dataset_num_vectors);
                    if (!dataset_prefilled && dataset_idx >= config.dataset_num_vectors) {
                        printf("Dataset exhausted while trying to avoid duplicate cluster tags (config.dataset_prefill_counter: %lu). Re-setting prefill.\n", 
                            atomic_load_explicit(&config.dataset_prefill_counter, memory_order_relaxed));
                        assert(config.dataset_prefill_counter < 2 * config.dataset_num_vectors);
                    }
                } while (!dataset_prefilled && getClusterTagForVector(&cluster_tag_map, dataset_idx)); // ensure unique cluster tag mapping
            }
            if (dataset_prefilled) {
                /* Random access after prefill */
                if (config.sequential_replacement) {
                    dataset_idx = atomic_fetch_add_explicit(vector_counter, 1, memory_order_relaxed);
                } else {
                    dataset_idx = random();
                }
                dataset_idx %= config.dataset_num_vectors;
                if (config.cluster_mode) {
                    cluster_tag = getClusterTagForVector(&cluster_tag_map, dataset_idx);
                    if (!cluster_tag) {
                        printf("No cluster tag found for vector ID %lu\n", dataset_idx);
                        assert(0);
                    }
                }
            }
            datasetGetVector((dataset_ctx_t*)config.dataset_ctx, dataset_idx,
                &vector_id, vec_write_pos);
      
            encode_vector_key_fixed(key, key_len,
                                   NULL,
                                   cluster_tag, // cluster tag may be NULL if dataset is prefilled and no tags, but if tags exist, it must be provided and override existing tag. 
                                   // TODO need to handle case of cluster node mismatch
                                   vector_id);
                        /* Handle tag field replacement with padding if configured */
            if (tag_count > 0 && i < tag_count && config.search.tag_field && config.search.payload_tag_len > 0) {
                char *tag_payload_start = cmd + tag_indices[i];
                
                /* Generate actual tag value */
                sds selected_tag = selectTagByDistribution();
                int64_t actual_tag_len = selected_tag ? sdslen(selected_tag) : 0;
                assert(actual_tag_len <= config.search.payload_tag_len);
                assert(actual_tag_len > 0);
                /* Calculate RESP header length - number of digits needed for payload_tag_len */
                memcpy(tag_payload_start, selected_tag, actual_tag_len);
                if (actual_tag_len < config.search.payload_tag_len) {
                    // add ','
                    tag_payload_start[actual_tag_len] = ',';
                }
                
                if (selected_tag) sdsfree(selected_tag);
            }

            /* Update cluster tag mapping for new insertions (complements initial cluster scan) */
            if (!dataset_prefilled) {
                char cluster_tag_buf[PLACEHOLDERS[CLUSTER_PLACEHOLDER_INDEX].len + 1];
                const char *cluster_tag_to_map = NULL;                
                if (config.cluster_mode) {                    
                    char *cluster_tag_pos = cmd + cluster_tag_indices[i];
                    /* Extract cluster tag using reusable function */
                    int64_t tag_len = PLACEHOLDERS[CLUSTER_PLACEHOLDER_INDEX].len;
                    memcpy(cluster_tag_buf, cluster_tag_pos, tag_len);
                    cluster_tag_buf[tag_len] = '\0'; /* Null-terminate */
                    cluster_tag_to_map = cluster_tag_buf;
                } 
                addClusterTagMapping(&cluster_tag_map, vector_id, cluster_tag_to_map);
            }

            /* Debug output for first few inserts */
            static int64_t debug_count = 0;
            if (debug_count < 5) {
                printf("DEBUG INSERT: dataset_idx=%lu, vector_id=%lu, key_str='%.*s', vec_size=%lu bytes\n",
                        dataset_idx, vector_id, (int)key_len, key, config.search.vector_dim * 4);
                // if (tag_count == 0) {
                //     printf("DEBUG INSERT: dataset_idx=%lu, vector_id=%lu, key_str='%s', vec_size=%lu bytes\n",
                //         dataset_idx, vector_id, key, config.search.vector_dim * 4);
                // } else {
                //     char *tag_payload_start = cmd + tag_indices[i] -6;
                //     // int64_t header_len = snprintf(NULL, 0, "%ld", config.search.payload_tag_len);
                //     // char *tag_value_start = tag_payload_start;
                //     // int64_t tag_value_len = 0;
                //     // while (tag_value_start[tag_value_len] != '\r' && tag_value_len < config.search.payload_tag_len) {
                //     //     tag_value_len++;
                //     // }
                //     sds tag_value = sdsnewlen(tag_payload_start, config.search.payload_tag_len);
                //     printf("DEBUG INSERT: dataset_idx=%lu, vector_id=%lu, tag_value='%s', vec_size=%lu bytes\n",
                //         dataset_idx, vector_id, tag_value, config.search.vector_dim * 4);
                //     sdsfree(tag_value);
                //     // printf("DEBUG CMD DUMP:START<\n%.*s\nDEBUG CMD DUMP - END>\n", 1600, cmd);
                // }
                debug_count++;
            }
        }
    }
    /* SEARCH: only vector replacement */
    if (vec_count > 0 && key_count == 0) {
        c->running_queries++;
        static __thread uint64_t rng_state = 0;
        if (rng_state == 0) {
            // Initialize with thread-specific seed
            rng_state = (uint64_t)thread_id * 0x9e3779b97f4a7c15ULL + (uint64_t)nstime();
        }
        
        for (size_t i = 0; i < vec_count; i++) {
            float *vec_write_pos = (float *)(cmd + vec_indices[i]);
            
            // xorshift64* - high quality PRNG
            rng_state ^= rng_state >> 12;
            rng_state ^= rng_state << 25;
            rng_state ^= rng_state >> 27;
            uint64_t query_idx = (rng_state * 0x2545F4914F6CDD1DULL) % c->dataset_query_capacity;

            /* Enqueue query index for recall tracking */
            if (c && c->dataset_query_indices) {
                c->dataset_query_indices[(c->dataset_query_tail++) % c->dataset_query_capacity] = query_idx;
            }

            datasetSetQueryVec((dataset_ctx_t*)config.dataset_ctx, query_idx, vec_write_pos);
        }
    }
    // /* SEARCH: only vector replacement */
    // if (vec_count > 0 && key_count == 0) {
    //     c->running_queries++;
    //     static __thread uint64_t next_query_idx = 0;
    //     if (next_query_idx == 0) {
    //         next_query_idx = thread_id + (1 << 10) - 1;
    //         next_query_idx *= 2654435761;
    //     }
    //     for (size_t i = 0; i < vec_count; i++) {
    //         float *vec_write_pos = (float *)(cmd + vec_indices[i]);
    //         uint64_t query_idx = next_query_idx % c->dataset_query_capacity;

    //         /* Enqueue query index for recall tracking */
    //         if (c && c->dataset_query_indices) {
    //             c->dataset_query_indices[(c->dataset_query_tail++) % c->dataset_query_capacity] = query_idx;
    //         }

    //         datasetSetQueryVec((dataset_ctx_t*)config.dataset_ctx, query_idx, vec_write_pos);
    //         next_query_idx++;
    //         next_query_idx *= 2654435761; // Knuth's multiplicative hash to reduce collisions in lower bits
    //     }
    // }

    /* DELETE: only key replacement */
    if (key_count > 0 && vec_count == 0) {
        for (size_t i = 0; i < key_count; i++) {
            char *key_write_pos = cmd + key_indices[i];
            char *key = key_write_pos - strlen(config.search.prefix) - 1;
            int64_t key_len = strlen(config.search.prefix) + PLACEHOLDERS[DATASET_KEY_PLACEHOLDER_INDEX].len + 1;
            if (config.cluster_mode) {
                key -= PLACEHOLDERS[CLUSTER_PLACEHOLDER_INDEX].len;
                key_len += PLACEHOLDERS[CLUSTER_PLACEHOLDER_INDEX].len;
            }
            uint64_t vector_id = 0;
            const char* cluster_tag = NULL;
            do {
                if (config.sequential_replacement) {
                    vector_id = atomic_fetch_add_explicit(vector_counter, 1, memory_order_relaxed);
                } else {
                    vector_id = random();
                }
                vector_id %= config.keyspacelen;  
                // if (config.cluster_mode) {
                cluster_tag = getClusterTagForVector(&cluster_tag_map, vector_id);
                if (cluster_tag == NULL && getClusterTagMapCount(&cluster_tag_map) > 0) {
                    // if we have some tags, but not for this vector, we need to retry
                    continue;
                } else if (getClusterTagMapCount(&cluster_tag_map) == 0) {
                    // if we have some tags, but not for this vector, we need to retry
                    break;
                }
                // }
            } while (!cluster_tag);
            if (!cluster_tag) {
                printf("No cluster tag found for vector ID %lu during DELETE\n", vector_id);
                assert(0);
            }
            encode_vector_key_fixed(key, key_len,
                                   NULL,
                                   cluster_tag, // cluster tag may be NULL if dataset is prefilled and no tags, but if tags exist, it must be provided and override existing tag. 
                                   // TODO need to handle case of cluster node mismatch
                                   vector_id);
        }
    }        
}

static void replacePlaceholders(client c, char *cmd_data, int64_t cmd_count) {
    static _Atomic uint64_t seq_key[PLACEHOLDER_NUM_OF] = {0};

    for (int64_t cmd_index = 0; cmd_index < cmd_count; cmd_index++) {
        char *cmd = cmd_data + cmd_index * placeholders.cmd_len;
        
        /* Handle __rand_int__ separately (multiple different values) */
        size_t *indices = placeholders.indices[0];
        _Atomic uint64_t *key_counter = &seq_key[0];
        for (size_t i = 0; i < placeholders.count[0]; i++) {
            replacePlaceholder(indices + i, 1, cmd, key_counter, placeholders.len[0]);
        }

        /* Handle other regular placeholders */
        for (size_t placeholder = 1; placeholder < PLACEHOLDER_NORMAL_NUM_OF; placeholder++) {
            indices = placeholders.indices[placeholder];
            size_t count = placeholders.count[placeholder];
            key_counter = &seq_key[placeholder];
            replacePlaceholder(indices, count, cmd, key_counter, placeholders.len[placeholder]);
        }
        if (placeholders.count[CLUSTER_PLACEHOLDER_INDEX] > 0) {
            indices = placeholders.indices[CLUSTER_PLACEHOLDER_INDEX];
            size_t count = placeholders.count[CLUSTER_PLACEHOLDER_INDEX];
            replacePlaceholderClusterTag(c, indices, count, cmd, 
                                   &seq_key[CLUSTER_PLACEHOLDER_INDEX]);
        }
        /* Handle vector placeholder */
        if (config.use_search && placeholders.count[VECTOR_PLACEHOLDER_INDEX] > 0) {
            indices = placeholders.indices[VECTOR_PLACEHOLDER_INDEX];
            size_t count = placeholders.count[VECTOR_PLACEHOLDER_INDEX];
            replacePlaceholderVector(indices, count, cmd, 
                                   &seq_key[VECTOR_PLACEHOLDER_INDEX]);
        }

        /* Handle dataset placeholders */
        if (config.use_dataset && (placeholders.count[DATASET_KEY_PLACEHOLDER_INDEX] > 0 ||
                                  placeholders.count[DATASET_VECTOR_PLACEHOLDER_INDEX] > 0)) {
            replacePlaceholderDataset(
                c,
                c->thread_id,
                placeholders.count[DATASET_KEY_PLACEHOLDER_INDEX],
                placeholders.indices[DATASET_KEY_PLACEHOLDER_INDEX],
                &seq_key[DATASET_KEY_PLACEHOLDER_INDEX],
                placeholders.count[DATASET_VECTOR_PLACEHOLDER_INDEX],
                placeholders.indices[DATASET_VECTOR_PLACEHOLDER_INDEX],
                &seq_key[DATASET_VECTOR_PLACEHOLDER_INDEX],
                placeholders.count[CLUSTER_PLACEHOLDER_INDEX],
                placeholders.indices[CLUSTER_PLACEHOLDER_INDEX],
                placeholders.count[DATASET_TAG_PLACEHOLDER_INDEX],
                placeholders.indices[DATASET_TAG_PLACEHOLDER_INDEX],
                &seq_key[DATASET_TAG_PLACEHOLDER_INDEX],
                cmd
            );
        }

        /* Verify RESP protocol structure integrity (excluding binary payloads)
         * Check that null bytes don't appear in RESP text sections.
         * Binary payloads (like vector data) can legitimately contain null bytes. */
        if (config.use_dataset && placeholders.count[DATASET_KEY_PLACEHOLDER_INDEX] > 0) {
            size_t key_start = placeholders.indices[DATASET_KEY_PLACEHOLDER_INDEX][0];
            /* Key starts after: prefix + cluster_tag + ':' */
            size_t cluster_tag_len = config.cluster_mode ? PLACEHOLDERS[CLUSTER_PLACEHOLDER_INDEX].len : 0;
            size_t prefix_len = strlen(config.search.prefix);
            size_t key_field_start = key_start - prefix_len - cluster_tag_len - 1;
            size_t key_field_len = prefix_len + cluster_tag_len + 1 + PLACEHOLDERS[DATASET_KEY_PLACEHOLDER_INDEX].len;
            
            /* Check key field for null bytes (should not have any) */
            int nulls_in_key = checkBufferForNulls(cmd + key_field_start, key_field_len, 
                                                    "key field", 0);
            if (nulls_in_key > 0) {
                fprintf(stderr, "ERROR: Found %d null bytes in RESP key field (positions [%zu, %zu))\n", 
                        nulls_in_key, key_field_start, key_field_start + key_field_len);
                fprintf(stderr, "This indicates a bug in key encoding - RESP keys must not contain null bytes\n");
                assert(0);
            }
        }
    }
}

static void releasePausedClient(client c) {
    if (c->thread_id >= 0) {
        benchmarkThread *thread = config.threads[c->thread_id % config.num_threads];
        listNode *ln = listSearchKey(thread->paused_clients, c);
        if (ln != NULL) {
            listDelNode(thread->paused_clients, ln);
        }
    } else {
        listNode *ln = listSearchKey(config.paused_clients, c);
        if (ln != NULL) {
            listDelNode(config.paused_clients, ln);
        }
    }
}

static void freeClient(client c) {
    aeEventLoop *el = CLIENT_GET_EVENTLOOP(c);
    listNode *ln;
    aeDeleteFileEvent(el, c->context->fd, AE_WRITABLE);
    aeDeleteFileEvent(el, c->context->fd, AE_READABLE);
    if (c->thread_id >= 0) {
        int64_t requests_finished = atomic_load_explicit(&config.requests_finished, memory_order_relaxed);
        if (requests_finished >= config.requests) {
            aeStop(el);
        }
    }
    valkeyFree(c->context);
    if (c->paused) releasePausedClient(c);
    sdsfree(c->obuf);
    zfree(c->stagptr);
    if (c->dataset_query_indices) zfree(c->dataset_query_indices);
    if (config.num_threads) pthread_mutex_lock(&(config.liveclients_mutex));
    config.liveclients--;
    list* l;
    if (c->thread_id >= 0) {
        l = config.threads[c->thread_id]->clients;
    } else {
        l = config.clients;
    }
    ln = listSearchKey(l, c);
    assert(ln != NULL);
    listDelNode(l, ln);
    zfree(c);
    if (config.num_threads) pthread_mutex_unlock(&(config.liveclients_mutex));
}

static void freeClientsList(list *clients) {
    listNode *ln = clients->head, *next;

    while (ln) {
        next = ln->next;
        freeClient(ln->value);
        ln = next;
    }
}

static void freeAllClients(void) {
    freeClientsList(config.clients);
}

static void resetClient(client c) {
    aeEventLoop *el = CLIENT_GET_EVENTLOOP(c);
    if (c->paused) releasePausedClient(c);
    aeDeleteFileEvent(el, c->context->fd, AE_WRITABLE);
    aeDeleteFileEvent(el, c->context->fd, AE_READABLE);
    if (config.ct == VALKEY_CONN_RDMA) {
        writeHandler(el, c->context->fd, c, 0); /* RDMA context always writable, but it can't be invoked by AE_WRITABLE */
    } else {
        aeCreateFileEvent(el, c->context->fd, AE_WRITABLE, writeHandler, c);
    }
    c->written = 0;
    c->pending = config.pipeline * c->seqlen;
    /* Reset query index queue for dataset */
    c->dataset_query_head = 0;
    c->dataset_query_tail = 0;
    
    c->running_queries = 0;
    c->latency = -1;
}

/* Helper function to find node index in selected_nodes array */
static int64_t findNodeIndex(clusterNode *node) {
    if (!node) return -1;
    for (int64_t i = 0; i < config.selected_node_count; i++) {
        if (config.selected_nodes[i] == node) return i;
    }
    return -1; /* Should never happen if node is valid */
}

static __thread int64_t num_sleepers = 0;
/* Acquires the specified number of tokens from the token bucket or calculates the wait time if tokens are not available.
 * This function implements a token bucket rate limiting algorithm to control access to a resource.
 *
 * The tokens parameter is the number of tokens to acquire.
 *
 * Returns the delay time in milliseconds that the caller should wait before proceeding, or 0 if tokens are immediately available.
 *
 * Token Bucket Algorithm Explanation:
 * - The token bucket algorithm allows a certain number of tokens to be accumulated over time, which can then be used to control the rate of requests.
 * - Due to the time event only allowing a delay of 1ms, a request for the next 1ms is issued.
 *
 * The function is thread-safe. */
static long long acquireTokenOrWait(int64_t tokens) {
    uint64_t time_per_token = config.time_per_token;
    uint64_t time_per_burst = config.time_per_burst;
    uint64_t new_time = 0;
    uint64_t now_epoch, next_epoch, min_time, delay_time;
    uint64_t last_time_ns, old_last_time_ns;

    while (1) {
        old_last_time_ns = atomic_load_explicit(&config.last_time_ns, memory_order_relaxed);
        last_time_ns = old_last_time_ns;
        now_epoch = nstime();

        // If the last_time_ns is 0, it means this is the first request, so we set it to now_epoch.
        if (last_time_ns == 0) {
            last_time_ns = now_epoch;
        }

        next_epoch = now_epoch + 1000000;
        min_time = next_epoch - time_per_burst;

        if (min_time > last_time_ns) { // if the last time is too old, reset it
            new_time = min_time + (time_per_token * tokens);
        } else {
            new_time = last_time_ns + (time_per_token * tokens);
        }

        delay_time = 0;
        if (new_time > next_epoch) { // if the new time is in the next epoch, we need to wait
            delay_time = new_time - now_epoch;
        } else {
            last_time_ns = new_time;
        }

        if (atomic_compare_exchange_weak_explicit(
                &config.last_time_ns,
                &old_last_time_ns,
                last_time_ns,
                memory_order_release,
                memory_order_relaxed)) {
            break;
        }
    }

    return delay_time / 1000000;
}

/* Quota-based node balancing: Check if a node has remaining quota.
 * 
 * New Strategy (Quota-based):
 * 1. Each node starts with a quota (e.g., 1000 requests via balance_quota_step)
 * 2. Each node tracks completed_requests_since_last_checked (node_request_counters)
 * 3. When ANY node exhausts its quota:
 *    a. Find min_completed = MIN(node_request_counters[i]) across all nodes
 *    b. If min_completed == 0: Sleep (wait for slowest node to make progress)
 *    c. Else:
 *       - Calculate quota_to_add = min_completed * (100 + tolerance_pct) / 100
 *       - Add quota_to_add to all nodes' remaining quota
 *       - Reset all node_request_counters to 0
 *       - First client adding quota wakes all sleeping clients
 * 4. This ensures:
 *    - Slowest node always completes its work before cycle ends
 *    - Fast nodes can't get more than initial_quota ahead of slowest
 *      (because all get same increment based on slowest's progress)
 *    - Tolerance allows for system jitter (e.g., 10% = fast can be 10% ahead)
 *    - Natural adaptation to actual throughput
 * 
 * Returns delay in milliseconds if node should be throttled, 0 otherwise.
 */
static long long checkNodeBalanceThrottle(int64_t thread_id, int64_t node_idx, int64_t tokens) {
    if (node_idx < 0 || node_idx >= config.selected_node_count) {
        return 0;
    }
    int64_t* node_quota_remaining;
    int64_t* node_request_counters;
    if (thread_id == -1) {
        /* Single-threaded mode: use global counters */
        node_quota_remaining = config.node_quota_remaining;
        node_request_counters = config.node_request_counters;
    } else {
        assert(config.num_threads > 0 && thread_id < config.num_threads);
        /* Multi-threaded mode: use per-thread counters */
        node_quota_remaining = config.threads[thread_id]->node_quota_remaining;
        node_request_counters = config.threads[thread_id]->node_request_counters;
    }
    
    /* Node has exhausted quota - check if we should start a new cycle
     * A new cycle starts when the slowest node has completed its quota */
    
    /* Find minimum requests completed in current cycle */
    int64_t min_completed = INT64_MAX;
    for (int64_t i = 0; i < config.selected_node_count; i++) {
        uint64_t completed = node_request_counters[i];
        if (completed < min_completed) {
            min_completed = completed;
        }
    }
    
    assert(min_completed != INT64_MAX);
    if (min_completed <= 0) {
        num_sleepers++;
        return 1; /* Slowest node hasn't made progress yet - throttle */
    }
    /* Calculate how many requests the slowest node needs to complete to finish its quota
     * Note: quota_remaining can be negative if we allowed burst */
    for (int64_t i = 0; i < config.selected_node_count; i++) {
        // compare and swap node_quota_remaining with node_quota_remaining + (min_completed * (100 + tolerance_pct) / 100)
        node_quota_remaining[i] += (min_completed * (100 + config.balance_tolerance_pct)) / 100;
        node_request_counters[i] = 0; /* Reset for next cycle */
    }
    
    return 0; /* New quota added - allow request */
}

/* Acquire tokens for a specific node or calculate wait time if node quota is exhausted.
 * This implements per-node rate limiting to ensure fair distribution across cluster nodes.
 * 
 * NOTE: This function does NOT increment statistics counters - that's done separately in writeHandler.
 * It uses dynamic balancing to throttle nodes that get too far ahead of the slowest node.
 * 
 * Returns the delay time in milliseconds if the node should be throttled,
//  * or 0 if the request can proceed immediately. */
// static long long acquireNodeTokenOrWait(int64_t thread_id, int64_t node_idx, int64_t tokens) {
//     /* Use dynamic balancing strategy: throttle nodes that are >10% ahead of slowest */
//     return checkNodeBalanceThrottle(thread_id, node_idx, tokens);
// }

/* Test function to simulate node balancing with different latencies.
 * 
 * This test simulates multiple nodes with different request latencies (in ms)
 * and verifies that the balancing algorithm allows throughput equal to:
 *   expected_rps = num_nodes / slowest_latency_ms * 1000
 * 
 * With 10% tolerance, each node should achieve approximately the same RPS as the slowest node.
 * 
 * Input: Array of latencies in milliseconds for each node
 * Example: [3000, 1000, 3000, 5000, 550, 100, 9000] means:
 *   - Slowest node has 9000ms latency = 0.111 rps
 *   - Expected balanced throughput = 7 nodes * 0.111 rps = 0.778 rps total
 *   - Each node should do ~0.111 rps (within 10% tolerance)
 */
static void testNodeBalancing(int64_t *latencies_ms, int64_t num_nodes, int64_t duration_sec) {
    printf("\n=== Node Balance Test ===\n");
    printf("Testing %ld nodes for %ld seconds\n", num_nodes, duration_sec);
    
    /* Find slowest node and calculate expected RPS */
    int64_t max_latency_ms = 0;
    for (int64_t i = 0; i < num_nodes; i++) {
        printf("  Node %ld: %ld ms latency\n", i, latencies_ms[i]);
        if (latencies_ms[i] > max_latency_ms) {
            max_latency_ms = latencies_ms[i];
        }
    }
    
    double slowest_node_rps = 1000.0 / max_latency_ms;
    double expected_total_rps = slowest_node_rps * num_nodes;
    
    printf("\nSlowest node: %ld ms = %.3f rps\n", max_latency_ms, slowest_node_rps);
    printf("Expected balanced total: %.3f rps (%.3f per node)\n", expected_total_rps, slowest_node_rps);
    printf("Expected tolerance: +/- 10%%\n\n");
    
    /* Save original config */
    int64_t orig_node_count = config.selected_node_count;
    int64_t orig_quota_step = config.balance_quota_step;
    int64_t orig_tolerance = config.balance_tolerance_pct;
    int64_t *orig_counters = config.node_request_counters;
    int64_t *orig_quota = config.node_quota_remaining;
    
    /* Setup test config with quota-based balancing */
    config.selected_node_count = num_nodes;
    config.balance_quota_step = 10;  /* Start with small quota for testing */
    config.balance_tolerance_pct = 10;
    config.node_request_counters = zcalloc(sizeof(int64_t) * num_nodes);
    config.node_quota_remaining = zcalloc(sizeof(int64_t) * num_nodes);
    
    printf("Using quota-based balancing: %ld requests per cycle\n", config.balance_quota_step);
    
    /* Initialize counters and quotas */
    for (int64_t i = 0; i < num_nodes; i++) {
        config.node_quota_remaining[i] = config.balance_quota_step;
    }
    
    /* Simulate requests for each node */
    uint64_t *total_requests = zcalloc(sizeof(uint64_t) * num_nodes);
    uint64_t *total_throttled_ms = zcalloc(sizeof(uint64_t) * num_nodes);
    
    uint64_t start_time_ns = nstime();
    uint64_t duration_ns = duration_sec * 1000000000ULL;
    uint64_t *next_available_ns = zcalloc(sizeof(uint64_t) * num_nodes);
    
    /* Initialize all nodes as available now */
    for (int64_t i = 0; i < num_nodes; i++) {
        next_available_ns[i] = start_time_ns;
    }
    
    printf("Simulating requests...\n");
    
    int debug_counter = 0;
    while (1) {
        uint64_t now_ns = nstime();
        if (now_ns - start_time_ns >= duration_ns) {
            break;
        }
        
        /* Try to send request from each node if it's available */
        for (int64_t node = 0; node < num_nodes; node++) {
            if (now_ns >= next_available_ns[node]) {
                /* Node is ready to send a request */
                
                /* Check if balancing would throttle this node (check BEFORE incrementing) */
                long long delay_ms = checkNodeBalanceThrottle(-1, node, 1);
                
                if (debug_counter < 50) {
                    int64_t quota = config.node_quota_remaining[node];
                    printf("[Debug %d] Node %ld: quota=%ld, delay=%lld ms\n",
                           debug_counter, node, quota, delay_ms);
                    debug_counter++;
                }
                
                if (delay_ms > 0) {
                    /* Throttled - don't increment, add delay */
                    total_throttled_ms[node] += delay_ms;
                    next_available_ns[node] = now_ns + (delay_ms * 1000000ULL);
                } else {
                    config.node_request_counters[node] += 1;
                    config.node_quota_remaining[node] -= 1;
                    total_requests[node]++;
                    
                    /* Node will be busy for its latency duration */
                    next_available_ns[node] = now_ns + (latencies_ms[node] * 1000000ULL);
                }
            }
        }
        
        /* Small sleep to avoid burning CPU (simulate event loop) */
        usleep(1000); /* 1 millisecond - check frequently to catch imbalance early */
    }
    
    uint64_t end_time_ns = nstime();
    double actual_duration_sec = (end_time_ns - start_time_ns) / 1000000000.0;
    
    printf("\n=== Results after %.2f seconds ===\n", actual_duration_sec);
    
    uint64_t total_all_requests = 0;
    uint64_t min_requests = UINT64_MAX;
    uint64_t max_requests = 0;
    
    for (int64_t i = 0; i < num_nodes; i++) {
        double node_rps = total_requests[i] / actual_duration_sec;
        double node_throttle_pct = (total_throttled_ms[i] * 100.0) / (actual_duration_sec * 1000.0);
        
        printf("Node %ld: %lu requests (%.3f rps) - throttled %.1f%% of time\n",
               i, total_requests[i], node_rps, node_throttle_pct);
        
        total_all_requests += total_requests[i];
        if (total_requests[i] < min_requests) min_requests = total_requests[i];
        if (total_requests[i] > max_requests) max_requests = total_requests[i];
    }
    
    double actual_total_rps = total_all_requests / actual_duration_sec;
    double imbalance_pct = min_requests > 0 ? 
        ((double)(max_requests - min_requests) / min_requests * 100.0) : 0;
    
    printf("\nTotal: %lu requests (%.3f rps)\n", total_all_requests, actual_total_rps);
    printf("Expected: %.3f rps\n", expected_total_rps);
    printf("Imbalance: %.1f%% (min=%lu, max=%lu)\n", imbalance_pct, min_requests, max_requests);
    
    /* Check if within tolerance */
    int passed = 1;
    if (imbalance_pct > 15.0) { /* Allow 15% due to simulation granularity */
        printf("❌ FAILED: Imbalance %.1f%% exceeds 15%% tolerance\n", imbalance_pct);
        passed = 0;
    } else {
        printf("✓ PASSED: Imbalance %.1f%% within tolerance\n", imbalance_pct);
    }
    
    /* Restore original config */
    config.selected_node_count = orig_node_count;
    config.balance_quota_step = orig_quota_step;
    config.balance_tolerance_pct = orig_tolerance;
    zfree(config.node_request_counters);
    zfree(config.node_quota_remaining);
    config.node_request_counters = orig_counters;
    config.node_quota_remaining = orig_quota;
    
    zfree(total_requests);
    zfree(total_throttled_ms);
    zfree(next_available_ns);
    
    printf("=========================\n\n");
    
    if (!passed) {
        exit(1);
    }
}

static void clientDone(client c) {
    int64_t requests_finished = atomic_load_explicit(&config.requests_finished, memory_order_relaxed);
    if (requests_finished >= config.requests) {
        
        freeClient(c);
        if (!config.num_threads && config.el) aeStop(config.el);
        return;
    }
    if (config.keepalive) {
        resetClient(c);
    } else {
        if (config.num_threads) pthread_mutex_lock(&(config.liveclients_mutex));
        config.liveclients--;
        createMissingClients("", 0, 1);
        config.liveclients++;
        if (config.num_threads) pthread_mutex_unlock(&(config.liveclients_mutex));
        freeClient(c);
    }
}

static void readHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    client c = privdata;
    void *reply = NULL;
    UNUSED(el);
    UNUSED(fd);
    UNUSED(mask);
    /* Calculate latency only for the first read event. This means that the
     * server already sent the reply and we need to parse it. Parsing overhead
     * is not part of the latency, so calculate it only once, here. */
    if (c->latency < 0) c->latency = ustime() - (c->start);

    if (valkeyBufferRead(c->context) != VALKEY_OK) {
        fprintf(stderr, "Error: %s\n", c->context->errstr);
        assert(0);
    } else {
        while (c->pending) {
            if (valkeyGetReply(c->context, &reply) != VALKEY_OK) {
                fprintf(stderr, "Error: %s\n", c->context->errstr);
                assert(0);
            }
            if (reply != NULL) {
                if (reply == (void *)VALKEY_REPLY_ERROR) {
                    fprintf(stderr, "Unexpected error reply, exiting...\n");
                    assert(0);
                }
                valkeyReply *r = reply;
                if (r->type == VALKEY_REPLY_ERROR) {
                    /* Try to update slots configuration if reply error is
                     * MOVED/ASK/CLUSTERDOWN and the key(s) used by the command
                     * contain(s) the slot hash tag.
                     * If the error is not topology-update related then we
                     * immediately exit to avoid false results. */
                    if (c->cluster_node && c->staglen) {
                        int64_t fetch_slots = 0, do_wait = 0;
                        if (!strncmp(r->str, "MOVED", 5) || !strncmp(r->str, "ASK", 3))
                            fetch_slots = 1;
                        else if (!strncmp(r->str, "CLUSTERDOWN", 11)) {
                            /* Usually the cluster is able to recover itself after
                             * a CLUSTERDOWN error, so try to sleep one second
                             * before requesting the new configuration. */
                            fetch_slots = 1;
                            do_wait = 1;
                            fprintf(stderr, "Error from server %s:%d: %s.\n", c->cluster_node->ip,
                                    c->cluster_node->port, r->str);
                        }
                        if (do_wait) sleep(1);
                        if (fetch_slots && !fetchClusterSlotsConfiguration(c)) {
                            fprintf(stderr, "Error from server %s:%d: %s\n", c->cluster_node->ip,
                                    c->cluster_node->port, r->str);
                            fflush(stderr);
                            assert(0);
                        }
                    } else {
                        if (c->cluster_node) {
                            fprintf(stderr, "Error from server %s:%d: %s\n", c->cluster_node->ip, c->cluster_node->port,
                                    r->str);
                        } else
                            fprintf(stderr, "Error from server: %s\n", r->str);
                        assert(0);
                    }
                }
                if (c->prefix_pending <= 0 && c->running_queries > 0) {
                    assert(c->dataset_query_head != c->dataset_query_tail);
                    uint64_t query_idx = c->dataset_query_indices[
                        (c->dataset_query_head++) % c->dataset_query_capacity
                    ];
                    processQueryResults(reply, query_idx);                    
                    c->running_queries--;
                }
                freeReplyObject(reply);
                /* This is an OK for prefix commands such as auth and select.*/
                if (c->prefix_pending > 0) {
                    c->prefix_pending--;
                    c->pending--;
                    /* Discard prefix commands on first response.*/
                    if (c->prefixlen > 0) {
                        size_t j;
                        sdsrange(c->obuf, c->prefixlen, -1);
                        /* Fix the pointers to the slot hash tags */
                        for (j = 0; j < c->staglen; j++) c->stagptr[j] -= c->prefixlen;
                        c->prefixlen = 0;
                    }
                    continue;
                }
                int64_t requests_finished = atomic_fetch_add_explicit(&config.requests_finished, 1, memory_order_relaxed);
                if (requests_finished < config.requests) {
                    if (config.num_threads == 0) {
                        hdr_record_value(config.latency_histogram, // Histogram to record to
                                         (long)c->latency <= CONFIG_LATENCY_HISTOGRAM_MAX_VALUE
                                             ? (long)c->latency
                                             : CONFIG_LATENCY_HISTOGRAM_MAX_VALUE); // Value to record
                        hdr_record_value(config.current_sec_latency_histogram,      // Histogram to record to
                                         (long)c->latency <= CONFIG_LATENCY_HISTOGRAM_INSTANT_MAX_VALUE
                                             ? (long)c->latency
                                             : CONFIG_LATENCY_HISTOGRAM_INSTANT_MAX_VALUE); // Value to record
                    } else {
                        hdr_record_value_atomic(config.latency_histogram, // Histogram to record to
                                                (long)c->latency <= CONFIG_LATENCY_HISTOGRAM_MAX_VALUE
                                                    ? (long)c->latency
                                                    : CONFIG_LATENCY_HISTOGRAM_MAX_VALUE); // Value to record
                        hdr_record_value_atomic(config.current_sec_latency_histogram,      // Histogram to record to
                                                (long)c->latency <= CONFIG_LATENCY_HISTOGRAM_INSTANT_MAX_VALUE
                                                    ? (long)c->latency
                                                    : CONFIG_LATENCY_HISTOGRAM_INSTANT_MAX_VALUE); // Value to record
                    }
                }
                c->pending--;
                if (c->pending == 0) {
                    clientDone(c);
                    break;
                }
            } else {
                break;
            }
        }
    }
}

/*
 * When a client is paused, the function is called by the event loop to
 * awaken the client.
 *
 * Return the number of milliseconds to wait before calling the function again.
 *
 * If the function returns AE_NOMORE, the event is removed.
 */
static long long awakenPausedClient(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    UNUSED(id);
    benchmarkThread *thread = (benchmarkThread *)clientData;

    list *paused_clients = NULL;
    if (thread == NULL) {
        paused_clients = config.paused_clients;
    } else {
        paused_clients = thread->paused_clients;
    }

    listIter li;
    listNode *ln;
    long long delay = 0;
    listRewind(paused_clients, &li);
    while ((ln = listNext(&li)) != NULL) {
        client c = ln->value;
        delay = acquireTokenOrWait(config.pipeline);
        if (delay) {
            break;
        }
        // When client acquires a token, try to write with `reuse`.
        c->paused = 0;
        c->reuse = 1;
        writeHandler(eventLoop, c->context->fd, c, AE_WRITABLE);
        listDelNode(paused_clients, ln);
    }

    // If there are no more paused clients, remove the event.
    if (delay == 0) {
        return AE_NOMORE;
    }
    return delay;
}

static void writeHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    client c = privdata;
    UNUSED(el);
    UNUSED(fd);
    UNUSED(mask);
    assert( config.pipeline > 0 );
    /* Priority 1: Check node balancing quota (more restrictive) 
     * Only enforce during actual benchmark phase.
     * We activate node balancing only when benchmark requests start being issued.
     * This excludes all setup phases: init, info fetch, backfill, prefill, etc. */
    if (c->written == 0 && config.balance_nodes && config.node_request_counters && c->cluster_node && c->thread_id != -1) {
        assert(c->thread_id < config.num_threads);
        int64_t node_idx = findNodeIndex(c->cluster_node);
        if (node_idx >= 0) {
            if (c->prefix_pending == 0 && config.threads[c->thread_id]->node_quota_remaining[node_idx] < config.pipeline) {
                // If quota is already exhausted, check if we should throttle
                if (checkNodeBalanceThrottle(c->thread_id, node_idx, config.pipeline)) {
                    // Throttle: simply return and try again later
                    return;
                }
            }
            config.threads[c->thread_id]->node_request_counters[node_idx] += config.pipeline;
            config.threads[c->thread_id]->node_quota_remaining[node_idx] -= config.pipeline;
            
        }
    }

    /* Priority 2: Check global RPS limit */
    if (config.rps > 0 && c->reuse == 0) {
        /* Acquire a token from the token bucket. */
        long long delay = acquireTokenOrWait(config.pipeline);

        if (delay) {
            int64_t thread_id = c->thread_id;
            int64_t paused_clients_count = 0;

            c->paused = 1;
            aeDeleteFileEvent(el, c->context->fd, AE_WRITABLE);

            benchmarkThread *thread = NULL;
            if (thread_id < 0) {
                paused_clients_count = listLength(config.paused_clients);
                listAddNodeTail(config.paused_clients, c);
            } else {
                thread = config.threads[thread_id % config.num_threads];
                paused_clients_count = listLength(thread->paused_clients);
                listAddNodeTail(thread->paused_clients, c);
            }
            if (paused_clients_count == 0) {
                /* Create a time event to awaken the client. */
                aeCreateTimeEvent(el, delay, awakenPausedClient, (void *)thread, NULL);
            }
            return;
        }
    }
    c->reuse = 0;

    /* Initialize request when nothing was written. */
    if (c->written == 0) {
        /* Enforce upper bound to number of requests. */
        int64_t requests_issued = atomic_fetch_add_explicit(&config.requests_issued,
                                                        config.pipeline * c->seqlen,
                                                        memory_order_relaxed);
        if (requests_issued >= config.requests) {
            return;
        }

        /* Really initialize: replace keys and set start time. */
        replacePlaceholders(c, c->obuf + c->prefixlen, config.pipeline);
        c->slots_last_update = atomic_load_explicit(&config.slots_last_update, memory_order_relaxed);
        c->start = ustime();
        c->latency = -1;
    }
    const ssize_t buflen = sdslen(c->obuf);
    const ssize_t writeLen = buflen - c->written;
    if (writeLen > 0) {
        void *ptr = c->obuf + c->written;
        while (1) {
            /* Optimistically try to write before checking if the file descriptor
             * is actually writable. At worst we get EAGAIN. */
            const ssize_t nwritten = cliWriteConn(c->context, ptr, writeLen);
            if (nwritten != writeLen) {
                if (nwritten == -1 && errno != EAGAIN) {
                    if (errno != EPIPE) fprintf(stderr, "Error writing to the server: %s\n", strerror(errno));
                    freeClient(c);
                    return;
                } else if (nwritten > 0) {
                    c->written += nwritten;
                    /* Ensure WRITABLE event is registered to complete the write */
                    if (config.ct != VALKEY_CONN_RDMA) {
                        aeCreateFileEvent(el, c->context->fd, AE_WRITABLE, writeHandler, c);
                    }
                    return;
                } else {
                    /* nwritten == -1 && errno == EAGAIN: would block, try again later */
                    /* Ensure WRITABLE event is registered for retry */
                    if (config.ct != VALKEY_CONN_RDMA) {
                        aeCreateFileEvent(el, c->context->fd, AE_WRITABLE, writeHandler, c);
                    }
                    return;
                }
            } else {
                aeDeleteFileEvent(el, c->context->fd, AE_WRITABLE);
                aeCreateFileEvent(el, c->context->fd, AE_READABLE, readHandler, c);
                return;
            }
        }
    }
}

/* Create a benchmark client, configured to send the command passed as 'cmd' of
 * 'len' bytes.
 *
 * The command is copied N times in the client output buffer (that is reused
 * again and again to send the request to the server) accordingly to the configured
 * pipeline size.
 *
 * Also an initial SELECT command is prepended in order to make sure the right
 * database is selected, if needed. The initial SELECT will be discarded as soon
 * as the first reply is received.
 *
 * To create a client from scratch, the 'from' pointer is set to NULL. If instead
 * we want to create a client using another client as reference, the 'from' pointer
 * points to the client to use as reference. In such a case the following
 * information is take from the 'from' client:
 *
 * 1) The command line to use.
 * 2) The offsets of the __rand_int__ elements inside the command line, used
 *    for arguments randomization.
 *
 * Even when cloning another client, prefix commands are applied if needed.*/
static client createClient(char *cmd, int64_t len, int64_t seqlen, client from, int64_t thread_id) {
    int64_t is_cluster_client = (config.cluster_mode && thread_id >= 0);
    client c = zcalloc(sizeof(struct _client));
    const char *ip = config.conn_info.hostip;
    int port = config.conn_info.hostport;
    struct timeval tv = {0};
    if (config.selected_node_count > 0) {
        int num_clients;
        /* If the user specified a list of nodes, use them in a round-robin
         * fashion. */
        int64_t node_idx = 0;
        if (thread_id >= 0) {
            benchmarkThread *thread = config.threads[thread_id];
            num_clients = listLength(thread->clients);
        } else {
            num_clients = config.liveclients;
        }
        node_idx = num_clients % config.selected_node_count;
        clusterNode *node = config.selected_nodes[node_idx];
        assert(node != NULL);
        ip = node->ip;
        port = node->port;
        c->cluster_node = node;
    }

    c->context = valkeyConnectWrapper(config.ct, ip, port, tv, 1, config.mptcp);
    if (c->context->err) {
        fprintf(stderr, "Could not connect to server at ");
        if (config.ct != VALKEY_CONN_UNIX || is_cluster_client)
            fprintf(stderr, "%s:%d: %s\n", ip, port, c->context->errstr);
        else
            fprintf(stderr, "%s: %s\n", ip, c->context->errstr);
        assert(0);
    }
    if (config.tls == 1) {
        const char *err = NULL;
        if (cliSecureConnection(c->context, config.sslconfig, &err) == VALKEY_ERR && err) {
            fprintf(stderr, "Could not negotiate a TLS connection: %s\n", err);
            assert(0);
        }
    }
    c->paused = 0;
    c->reuse = 0;
    c->thread_id = thread_id;

    /* Initialize query index queue for dataset recall tracking */
    c->dataset_query_capacity = config.dataset_num_queries;  /* 2x pipeline for safety */
    c->dataset_query_indices = zcalloc(sizeof(uint64_t) * c->dataset_query_capacity);
    c->dataset_query_head = 0;
    c->dataset_query_tail = 0;
    /* Suppress libvalkey cleanup of unused buffers for max speed. */
    c->context->reader->maxbuf = 0;

    /* Build the request buffer:
     * Queue N requests accordingly to the pipeline size, or simply clone
     * the example client buffer. */
    c->obuf = sdsempty();
    /* Prefix the request buffer with AUTH and/or SELECT commands, if applicable.
     * These commands are discarded after the first response, so if the client is
     * reused the commands will not be used again. */
    c->prefix_pending = 0;
    if (config.conn_info.auth) {
        char *buf = NULL;
        int64_t len;
        if (config.conn_info.user == NULL)
            len = valkeyFormatCommand(&buf, "AUTH %s", config.conn_info.auth);
        else
            len = valkeyFormatCommand(&buf, "AUTH %s %s", config.conn_info.user, config.conn_info.auth);
        c->obuf = sdscatlen(c->obuf, buf, len);
        zfree(buf);
        c->prefix_pending++;
    }

    if (config.enable_tracking) {
        char *buf = NULL;
        int64_t len = valkeyFormatCommand(&buf, "CLIENT TRACKING on");
        c->obuf = sdscatlen(c->obuf, buf, len);
        zfree(buf);
        c->prefix_pending++;
    }

    /* If a DB number different than zero is selected, prefix our request
     * buffer with the SELECT command, that will be discarded the first
     * time the replies are received, so if the client is reused the
     * SELECT command will not be used again. */
    if (config.conn_info.input_dbnum) {
        c->obuf = sdscatprintf(c->obuf, "*2\r\n$6\r\nSELECT\r\n$%ld\r\n%s\r\n", (int64_t)sdslen(config.input_dbnumstr),
                               config.input_dbnumstr);
        c->prefix_pending++;
    }

    if (config.resp3) {
        char *buf = NULL;
        int64_t len = valkeyFormatCommand(&buf, "HELLO 3");
        c->obuf = sdscatlen(c->obuf, buf, len);
        zfree(buf);
        c->prefix_pending++;
    }

    if (config.read_from_replica == FROM_REPLICA_ONLY || config.read_from_replica == FROM_ALL) {
        char *buf = NULL;
        int64_t len;
        len = valkeyFormatCommand(&buf, "READONLY");
        c->obuf = sdscatlen(c->obuf, buf, len);
        zfree(buf);
        c->prefix_pending++;
    }

    c->prefixlen = sdslen(c->obuf);
    /* Append the request itself. */
    if (from) {
        c->obuf = sdscatlen(c->obuf, from->obuf + from->prefixlen, sdslen(from->obuf) - from->prefixlen);
        seqlen = from->seqlen;
    } else {
        for (int64_t j = 0; j < config.pipeline; j++) c->obuf = sdscatlen(c->obuf, cmd, len);
    }

    c->written = 0;
    c->seqlen = seqlen;
    c->pending = config.pipeline * seqlen + c->prefix_pending;
    c->stagptr = NULL;
    c->staglen = 0;

    /* If cluster mode is enabled, set slot hashtags pointers. */
    if (config.cluster_mode) {
        if (from) {
            c->staglen = from->staglen;
            c->stagfree = 0;
            c->stagptr = zcalloc(sizeof(char *) * c->staglen);
            /* copy the offsets. */
            for (size_t j = 0; j < c->staglen; j++) {
                c->stagptr[j] = c->obuf + (from->stagptr[j] - from->obuf);
                /* Adjust for the different select prefix length. */
                c->stagptr[j] += c->prefixlen - from->prefixlen;
            }
        } else {
            char *p = c->obuf;

            c->staglen = 0;
            c->stagfree = RANDPTR_INITIAL_SIZE;
            c->stagptr = zcalloc(sizeof(char *) * c->stagfree);
            while ((p = strstr(p, "{clt}")) != NULL) {
                if (c->stagfree == 0) {
                    c->stagptr = zrealloc(c->stagptr, sizeof(char *) * c->staglen * 2);
                    c->stagfree += c->staglen;
                }
                c->stagptr[c->staglen++] = p;
                c->stagfree--;
                p += PLACEHOLDERS[CLUSTER_PLACEHOLDER_INDEX].len; /* 5 is strlen("{clt}"). */
            }
        }
    }
    aeEventLoop *el = NULL;
    if (thread_id < 0) {
        el = config.el;
        listAddNodeTail(config.clients, c);
    } else {
        assert(thread_id < config.num_threads);
        benchmarkThread *thread = config.threads[thread_id];
        el = thread->el;
        listAddNodeTail(thread->clients, c);
    }
    if (config.idlemode == 0) {
        if (config.ct == VALKEY_CONN_RDMA) {
            writeHandler(el, c->context->fd, c, 0);
        } else {
            aeCreateFileEvent(el, c->context->fd, AE_WRITABLE, writeHandler, c);
        }
    } else
        /* In idle mode, clients still need to register readHandler for catching errors */
        aeCreateFileEvent(el, c->context->fd, AE_READABLE, readHandler, c);

    config.liveclients++;

    c->slots_last_update = atomic_load_explicit(&config.slots_last_update, memory_order_relaxed);
    return c;
}

// Thread create missing clients, round robin on the selected nodes 
static void createMissingThreadClients(char *cmd, int64_t len, int64_t seqlen, benchmarkThread *thread, int64_t n_requested) {
    int64_t missing_clients = n_requested - listLength(thread->clients);
    int64_t n = 0;
    while (missing_clients > 0) {
        createClient(cmd, len, seqlen, NULL, thread->index);

        /* Listen backlog is quite limited on most systems */
        if (++n > 64) {
            usleep(50000);
            n = 0;
        }
        missing_clients--;
    }
}

static void createMissingClients(char *cmd, int64_t len, int64_t seqlen) {
    int64_t clients_per_thread = config.numclients;
    if (config.num_threads > 0) {
        clients_per_thread = (config.numclients + config.num_threads - 1) / config.num_threads;
    }
    for (int64_t thread_id = 0; thread_id < config.num_threads; thread_id++) {
        benchmarkThread *thread = config.threads[thread_id];
        createMissingThreadClients(cmd, len, seqlen, thread, clients_per_thread);
    }
}

static void showLatencyReport(void) {
    const float reqpersec = (float)config.requests_finished / ((float)config.totlatency / 1000.0f);
    const float p0 = ((float)hdr_min(config.latency_histogram)) / 1000.0f;
    const float p50 = hdr_value_at_percentile(config.latency_histogram, 50.0) / 1000.0f;
    const float p95 = hdr_value_at_percentile(config.latency_histogram, 95.0) / 1000.0f;
    const float p99 = hdr_value_at_percentile(config.latency_histogram, 99.0) / 1000.0f;
    const float p100 = ((float)hdr_max(config.latency_histogram)) / 1000.0f;
    const float avg = hdr_mean(config.latency_histogram) / 1000.0f;

    if (!config.quiet && !config.csv) {
        printf("%*s\r", (int)config.last_printed_bytes, " "); // ensure there is a clean line
        printf("====== %s ======\n", config.title);      
        printf("  %ld requests completed in %.2f seconds\n", config.requests_finished, (float)config.totlatency / 1000);
        printf("  %ld parallel clients\n", config.numclients);
        printf("  %ld bytes payload\n", config.datasize);
        printf("  keep alive: %ld\n", config.keepalive);
        const char *node_roles = NULL;
        if (config.read_from_replica == FROM_ALL) {
            node_roles = "cluster";
        } else if (config.read_from_replica == FROM_REPLICA_ONLY) {
            node_roles = "replica";
        } else {
            node_roles = "primary";
        }
        printf("  cluster mode: %s (%ld %s)\n", config.cluster_mode ? "yes" : "no", config.selected_node_count, node_roles);
        int64_t m;
        for (m = 0; m < config.selected_node_count; m++) {
            clusterNode *node = config.selected_nodes[m];
            serverConfig *cfg = node->server_config;
            if (cfg == NULL) continue;
            printf("  node [%ld] configuration:\n", m);
            printf("    save: %s\n", sdslen(cfg->save) ? cfg->save : "NONE");
            printf("    appendonly: %s\n", cfg->appendonly);
        }

        printf("  multi-thread: %s\n", (config.num_threads ? "yes" : "no"));
        if (config.num_threads) printf("  threads: %ld\n", config.num_threads);

        printf("\n");
        printf("Latency by percentile distribution:\n");
        struct hdr_iter iter;
        long long previous_cumulative_count = -1;
        const long long total_count = config.latency_histogram->total_count;
        hdr_iter_percentile_init(&iter, config.latency_histogram, 1);
        struct hdr_iter_percentiles *percentiles = &iter.specifics.percentiles;
        while (hdr_iter_next(&iter)) {
            const double value = iter.highest_equivalent_value / 1000.0f;
            const double percentile = percentiles->percentile;
            const long long cumulative_count = iter.cumulative_count;
            if (previous_cumulative_count != cumulative_count || cumulative_count == total_count) {
                printf("%3.3f%% <= %.3f milliseconds (cumulative count %lld)\n", percentile, value, cumulative_count);
            }
            previous_cumulative_count = cumulative_count;
        }
        printf("\n");
        printf("Cumulative distribution of latencies:\n");
        previous_cumulative_count = -1;
        hdr_iter_linear_init(&iter, config.latency_histogram, 100);
        while (hdr_iter_next(&iter)) {
            const double value = iter.highest_equivalent_value / 1000.0f;
            const long long cumulative_count = iter.cumulative_count;
            const double percentile = ((double)cumulative_count / (double)total_count) * 100.0;
            if (previous_cumulative_count != cumulative_count || cumulative_count == total_count) {
                printf("%3.3f%% <= %.3f milliseconds (cumulative count %lld)\n", percentile, value, cumulative_count);
            }
            /* After the 2 milliseconds latency to have percentages split
             * by decimals will just add a lot of noise to the output. */
            if (iter.highest_equivalent_value > 2000) {
                hdr_iter_linear_set_value_units_per_bucket(&iter, 1000);
            }
            previous_cumulative_count = cumulative_count;
        }
        printf("\n");
        printf("Summary:\n");
        printf("  throughput summary: %.2f requests per second\n", reqpersec);
        printf("  latency summary (msec):\n");
        printf("    %9s %9s %9s %9s %9s %9s\n", "avg", "min", "p50", "p95", "p99", "max");
        printf("    %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f\n", avg, p0, p50, p95, p99, p100);
        if (baseline_latency.measured) {
            printf("\n");
            printf("  baseline network latency (msec):\n");
            printf("    %9s %9s %9s %9s\n", "avg", "p50", "p95", "p99");
            printf("    %9.3f %9.3f %9.3f %9.3f\n",
                   baseline_latency.avg_latency_ms,
                   baseline_latency.p50_latency_ms,
                   baseline_latency.p95_latency_ms,
                   baseline_latency.p99_latency_ms);
            printf("  processing overhead (msec) = latency - baseline:\n");
            printf("    %9s %9s %9s %9s\n", "avg", "p50", "p95", "p99");
            printf("    %9.3f %9.3f %9.3f %9.3f\n",
                   avg - baseline_latency.avg_latency_ms,
                   p50 - baseline_latency.p50_latency_ms,
                   p95 - baseline_latency.p95_latency_ms,
                   p99 - baseline_latency.p99_latency_ms);
        }
        
    } else if (config.csv) {
        if (baseline_latency.measured) {
            printf("\"%s\",\"%.2f\",\"%.3f\",\"%.3f\",\"%.3f\",\"%.3f\",\"%.3f\",\"%.3f\",\"%.3f\",\"%.3f\",\"%.3f\",\"%.3f\"\n",
                   config.title, reqpersec, avg, p0, p50, p95, p99, p100,
                   baseline_latency.avg_latency_ms,
                   baseline_latency.p50_latency_ms,
                   baseline_latency.p95_latency_ms,
                   baseline_latency.p99_latency_ms);
        } else {
            printf("\"%s\",\"%.2f\",\"%.3f\",\"%.3f\",\"%.3f\",\"%.3f\",\"%.3f\",\"%.3f\"\n", config.title, reqpersec, avg,
                   p0, p50, p95, p99, p100);
        }
    } else {
        printf("%*s\r", (int)config.last_printed_bytes, " "); // ensure there is a clean line
        printf("%s: %.2f requests per second, p50=%.3f msec\n", config.title, reqpersec, p50);
    }
}

static void initBenchmarkThreads(void) {
    int64_t i;
    if (config.threads) freeBenchmarkThreads();
    config.threads = zcalloc(config.num_threads * sizeof(benchmarkThread *));
    for (i = 0; i < config.num_threads; i++) {
        benchmarkThread *thread = createBenchmarkThread(i);
        config.threads[i] = thread;
    }
}

static void startBenchmarkThreads(void) {
    int64_t i;
    for (i = 0; i < config.num_threads; i++) {
        benchmarkThread *t = config.threads[i];
        if (pthread_create(&(t->thread), NULL, execBenchmarkThread, t)) {
            fprintf(stderr, "FATAL: Failed to start thread %ld.\n", i);
            assert(0);
        }
    }
    for (i = 0; i < config.num_threads; i++) pthread_join(config.threads[i]->thread, NULL);
}
static clusterSnapshot* last_search_info = NULL;
static clusterSnapshot* last_ftinfo = NULL;
static clusterSnapshot* last_info_all = NULL;
static mstime_t snapshot_time = 0;
/* Benchmark a sequence of commands. The cmd is RESP encoded of length len and
 * seqlen is the number of commands included in cmd. */
static void benchmarkSequence(const char *title, char *cmd, int64_t len, int64_t seqlen) {
    config.title = title;
    config.requests_issued = 0;
    config.requests_finished = 0;
    config.previous_requests_finished = 0;
    config.last_printed_bytes = 0;
        /* Reset request counters */
    config.totlatency = 0;
    
    /* Reset recall statistics */
    initRecallStats();
    hdr_init(CONFIG_LATENCY_HISTOGRAM_MIN_VALUE,         // Minimum value
             CONFIG_LATENCY_HISTOGRAM_MAX_VALUE,         // Maximum value
             config.precision,                           // Number of significant figures
             &config.latency_histogram);                 // Pointer to initialise
    hdr_init(CONFIG_LATENCY_HISTOGRAM_MIN_VALUE,         // Minimum value
             CONFIG_LATENCY_HISTOGRAM_INSTANT_MAX_VALUE, // Maximum value
             config.precision,                           // Number of significant figures
             &config.current_sec_latency_histogram);     // Pointer to initialise

    initPlaceholders(cmd, len);
    if (config.num_threads) initBenchmarkThreads();
    getFullInfo(config.search.name, config.selected_node_count, config.selected_nodes, config.ct);
    long long search_memory = 0;
    long long search_reclaimable = 0;
    long long search_total_docs = 0;
    long long search_ingest_field_vector = 0;
    long long search_background_indexing_status = 0;
    long long after_search_memory = 0;
    long long after_search_reclaimable = 0;
    long long after_search_total_docs = 0;
    long long after_search_ingest_field_vector = 0;
    long long after_search_background_indexing_status = 0;
    
    if (config.rps > 0) {
        config.time_per_token = 1000000000 / config.rps;
        config.time_per_burst = config.time_per_token * config.rps;
        config.last_time_ns = 0;
    }

    /* Initialize node balancing if enabled
     * Skip during internal operations like baseline measurement */
    if (config.balance_nodes && !config.skip_latency_report) {
        if (config.selected_node_count == 0) {
            fprintf(stderr, "Error: --balance-nodes requires cluster mode with selected nodes\n");
            exit(1);
        }
        
        /* Allocate node request counters and quota arrays */
        config.node_request_counters = zcalloc(sizeof(int64_t) * config.selected_node_count);
        config.node_quota_remaining = zcalloc(sizeof(int64_t) * config.selected_node_count);
        
        /* Initialize each node with starting quota */
        for (int64_t i = 0; i < config.selected_node_count; i++) {
            config.node_request_counters[i] = 0;
            config.node_quota_remaining[i] = config.balance_quota_step;
        }
        
        if (!config.quiet) {
            printf("Node balancing enabled (quota-based):\n");
            printf("  Nodes: %ld\n", config.selected_node_count);
            printf("  Quota per cycle: %ld requests per node\n", config.balance_quota_step);
            printf("  Tolerance: %ld%%\n", config.balance_tolerance_pct);
            printf("  Total clients: %ld (%.1f per node)\n",
                   config.numclients,
                   (double)config.numclients / config.selected_node_count);
        }
    }

    createMissingClients(cmd, len, seqlen);
    
    config.start = mstime();    
    if (!config.num_threads)
        aeMain(config.el);
    else
        startBenchmarkThreads();
    
    config.totlatency = mstime() - config.start;
    if (config.use_search) {
        getFullInfo(config.search.name, config.selected_node_count, config.selected_nodes, config.ct);
        clusterSnapshot* after_search_info = NULL;
        clusterSnapshot* after_ftinfo = NULL;
        clusterSnapshot* after_info_all = NULL;
        after_search_info = getSearchInfo(config.selected_node_count, config.selected_nodes, config.ct,
                                         &after_search_memory, &after_search_reclaimable,
                                         &after_search_total_docs, &after_search_ingest_field_vector,
                                         &after_search_background_indexing_status);
        after_ftinfo = getFtInfoStatistics(config.search.name, config.selected_node_count, config.selected_nodes, config.ct);
        after_info_all = getInfoCluster(config.selected_node_count, config.selected_nodes, config.ct);
        if (last_info_all != NULL && last_ftinfo != NULL && last_search_info != NULL) {
            // Compare snapshots and print diffs
            compareInfoSnapshots(config.selected_node_count, config.selected_nodes, config.ct,
                             last_info_all, after_info_all, last_ftinfo, after_ftinfo, last_search_info, after_search_info);
        }
        freeClusterSnapshot(last_search_info);
        freeClusterSnapshot(last_ftinfo);
        freeClusterSnapshot(last_info_all);
        last_search_info = after_search_info;
        last_ftinfo = after_ftinfo;
        last_info_all = after_info_all;
        snapshot_time = mstime();
        printf_results("Search memory usage: before=%lld after=%lld (diff=%+lld), reclaimable: before=%lld after=%lld (diff=%+lld)\n",
               search_memory, after_search_memory, after_search_memory - search_memory,
               search_reclaimable, after_search_reclaimable, after_search_reclaimable - search_reclaimable);
        printf_results("Search total docs: before=%lld after=%lld (diff=%+lld)\n",
               search_total_docs, after_search_total_docs, after_search_total_docs - search_total_docs);
        printf_results("Search ingest field vector: before=%lld after=%lld (diff=%+lld)\n",
               search_ingest_field_vector, after_search_ingest_field_vector, after_search_ingest_field_vector - search_ingest_field_vector);
        printf_results("Search background indexing status: before=%lld after=%lld (diff=%+lld)\n",
               search_background_indexing_status, after_search_background_indexing_status, after_search_background_indexing_status - search_background_indexing_status);
    }
    
    if (!config.skip_latency_report) {
        showLatencyReport();
    }
    freeAllClients();
    /* Free the paused clients list (clients themselves are already freed) */
    if (config.paused_clients) {
        listRelease(config.paused_clients);
        config.paused_clients = listCreate();
    }
    if (config.threads) freeBenchmarkThreads();
    
    /* Only free histograms if we're not preserving them for later use */
    if (!config.preserve_histograms) {
        if (config.current_sec_latency_histogram) {
            hdr_close(config.current_sec_latency_histogram);
            config.current_sec_latency_histogram = NULL;
        }
        if (config.latency_histogram) {
            hdr_close(config.latency_histogram);
            config.latency_histogram = NULL;
        }
    }
    
    /* Cleanup node balancing resources */
    if (config.node_request_counters) {
        if (config.balance_nodes && !config.quiet) {
            printf("\nNode balancing statistics:\n");
            uint64_t total_requests = 0;
            for (int64_t i = 0; i < config.selected_node_count; i++) {
                uint64_t node_requests = atomic_load_explicit(&config.node_request_counters[i], memory_order_relaxed);
                total_requests += node_requests;
            }
            for (int64_t i = 0; i < config.selected_node_count; i++) {
                clusterNode *node = config.selected_nodes[i];
                uint64_t node_requests = atomic_load_explicit(&config.node_request_counters[i], memory_order_relaxed);
                double percentage = total_requests > 0 ? (100.0 * node_requests / total_requests) : 0.0;
                printf("  Node %ld (%s:%d): %lu requests (%.1f%%)\n",
                       i, node->ip, node->port, node_requests, percentage);
            }
            printf("  Total tracked: %lu requests\n", total_requests);
        }
        zfree(config.node_request_counters);
        config.node_request_counters = NULL;
    }
}

/* Benchmark a single RESP-encoded command of length len. */
static void benchmark(const char *title, char *cmd, int64_t len) {
    benchmarkSequence(title, cmd, len, 1);
}

/* Measure baseline network latency using minimal PING commands */
static void measureBaselineLatency(void) {
    if (baseline_latency.measured) {
        return;  /* Already measured */
    }
    
    /* Save current configuration */
    int64_t saved_clients = config.numclients;
    int64_t saved_threads = config.num_threads;
    long long saved_requests = config.requests;
    int64_t saved_quiet = config.quiet;
    int64_t saved_csv = config.csv;
    const char *saved_title = config.title;
    int64_t saved_skip_latency_report = config.skip_latency_report;
    
    /* Set to single-threaded, single-client for pure network measurement */
    config.numclients = 1;
    config.num_threads = 1;
    config.requests = 10000;
    config.quiet = 1;  /* Suppress all output during baseline measurement */
    config.csv = 0;    /* Don't output CSV for baseline */
    config.skip_latency_report = 1;  /* Don't show latency report for baseline */
    config.preserve_histograms = 1;  /* Don't free histograms so we can read them */
    
    /* Run PING_INLINE benchmark (minimal overhead) - silently */
    benchmark("BASELINE_LATENCY", "PING\r\n", 6);
    
    /* Collect baseline metrics from histogram (preserved by benchmarkSequence) */
    if (config.latency_histogram && config.latency_histogram->total_count > 0) {
        baseline_latency.avg_latency_ms = hdr_mean(config.latency_histogram) / 1000.0;
        baseline_latency.min_latency_ms = ((double)hdr_min(config.latency_histogram)) / 1000.0;
        baseline_latency.p50_latency_ms = hdr_value_at_percentile(config.latency_histogram, 50.0) / 1000.0;
        baseline_latency.p90_latency_ms = hdr_value_at_percentile(config.latency_histogram, 90.0) / 1000.0;
        baseline_latency.p95_latency_ms = hdr_value_at_percentile(config.latency_histogram, 95.0) / 1000.0;
        baseline_latency.p99_latency_ms = hdr_value_at_percentile(config.latency_histogram, 99.0) / 1000.0;
        baseline_latency.max_latency_ms = ((double)hdr_max(config.latency_histogram)) / 1000.0;
        baseline_latency.measured = 1;
    }
    
    /* Clean up the histogram created during baseline measurement */
    if (config.latency_histogram) {
        hdr_close(config.latency_histogram);
        config.latency_histogram = NULL;
    }
    if (config.current_sec_latency_histogram) {
        hdr_close(config.current_sec_latency_histogram);
        config.current_sec_latency_histogram = NULL;
    }
    
    /* Restore original configuration */
    config.numclients = saved_clients;
    config.num_threads = saved_threads;
    config.requests = saved_requests;
    config.quiet = saved_quiet;
    config.csv = saved_csv;
    config.title = saved_title;
    config.skip_latency_report = saved_skip_latency_report;
    config.preserve_histograms = 0;  /* Reset to default behavior */
    
    /* Mark as measured in config */
    config.baseline_measured = 1;
}


/* Thread functions. */
static benchmarkThread *createBenchmarkThread(int64_t index) {
    benchmarkThread *thread = zcalloc(sizeof(*thread));
    if (thread == NULL) return NULL;
    thread->index = index;
    thread->el = aeCreateEventLoop(1024 * 10);
    thread->paused_clients = listCreate();
    thread->clients = listCreate();
    /* Allocate node request counters and quota arrays */
    thread->node_request_counters = zcalloc(sizeof(int64_t) * config.selected_node_count);
    thread->node_quota_remaining = zcalloc(sizeof(int64_t) * config.selected_node_count);

    /* Initialize each node with starting quota */
    for (int64_t i = 0; i < config.selected_node_count; i++) {
        thread->node_quota_remaining[i] = config.balance_quota_step;
    }
    /* Note: Recall statistics are aggregated globally and shown in main output,
     * not per-thread, since recall is computed across all queries. */
    aeCreateTimeEvent(thread->el, 1, showThroughput, (void *)thread, NULL);
    return thread;
}



static void freeBenchmarkThread(benchmarkThread *thread) {
    if (thread->el) aeDeleteEventLoop(thread->el);
    // list merge
    freeClientsList(thread->clients);
    listRelease(thread->paused_clients);
    listRelease(thread->clients);

    zfree(thread->node_request_counters);
    zfree(thread->node_quota_remaining);
    zfree(thread);
}





static void freeBenchmarkThreads(void) {
    int64_t i = 0;
    for (; i < config.num_threads; i++) {
        benchmarkThread *thread = config.threads[i];
        if (thread) freeBenchmarkThread(thread);
    }
    zfree(config.threads);
    config.threads = NULL;
}

static void *execBenchmarkThread(void *ptr) {
    benchmarkThread *thread = (benchmarkThread *)ptr;
    aeMain(thread->el);
    return NULL;
}

/* Cluster helper functions. */

static clusterNode *createClusterNode(char *ip, int port) {
    clusterNode *node = zcalloc(sizeof(*node));
    if (!node) return NULL;
    node->ip = ip;
    node->port = port;
    node->name = NULL;
    node->flags = 0;
    node->replicate = NULL;
    node->replicas_count = 0;
    node->slots = zcalloc(CLUSTER_SLOTS * sizeof(int64_t));
    node->slots_count = 0;
    node->updated_slots = NULL;
    node->updated_slots_count = 0;
    node->server_config = NULL;
    return node;
}

static void freeClusterNode(clusterNode *node) {
    if (node->name) sdsfree(node->name);
    if (node->replicate) sdsfree(node->replicate);
    if (node->ctx) valkeyFree(node->ctx);
    /* If the node is not the reference node, that uses the address from
     * config.conn_info.hostip and config.conn_info.hostport, then the node ip has been
     * allocated by fetchClusterConfiguration, so it must be freed. */
    if (node->ip && strcmp(node->ip, config.conn_info.hostip) != 0) sdsfree(node->ip);
    if (node->server_config != NULL) freeServerConfig(node->server_config);
    zfree(node->slots);
    zfree(node);
}

static void freeClusterNodes(void) {
    int64_t i = 0;
    for (; i < config.cluster_node_count; i++) {
        clusterNode *n = config.cluster_nodes[i];
        if (n) freeClusterNode(n);
    }
    zfree(config.cluster_nodes);
    zfree(config.cluster_primary_nodes);
    config.cluster_nodes = NULL;
    config.cluster_primary_nodes = NULL;
}

static clusterNode **addClusterNode(clusterNode *node, int64_t is_primary) {
    int64_t selected =  isSelected(is_primary);
    node->is_replica = !is_primary;
    node->selected = selected;
    printf("Adding cluster node (%s) %s %s:%d\n", (selected? "selected": "not selected"), node->name, node->ip, node->port);
    if (!config.cluster_mode) {
        node->slots_count = CLUSTER_SLOTS;
        for (int64_t slot = 0; slot < CLUSTER_SLOTS; slot++) {
            node->slots[slot] = slot;
        }
    }
    // verify node ip + port is unique
    for (int64_t i = 0; i < config.cluster_node_count; i++) {
        clusterNode *n = config.cluster_nodes[i];
        if (strcmp(n->ip, node->ip) == 0 && n->port == node->port) {
            printf("Node %s:%d already exists, skipping name=%s, replicate=%s <==> n_name=%s, n_replicate=%s\n", node->ip, node->port, node->name, node->replicate, n->name, n->replicate);
            freeClusterNode(node);
            return config.cluster_nodes;
        }
    }
    int64_t count = config.cluster_node_count + 1;
    config.cluster_nodes = zrealloc(config.cluster_nodes, count * sizeof(clusterNode *));
    assert(config.cluster_nodes != NULL);
    node->ctx = getValkeyContext(config.ct, node->ip, node->port);
    config.cluster_nodes[config.cluster_node_count++] = node;
    
    if (node->replicate == NULL) {
        printf("Adding cluster primary node %s:%d\n", node->ip, node->port);
        config.cluster_primary_nodes = zrealloc(config.cluster_primary_nodes, (config.cluster_primary_node_count + 1) * sizeof(clusterNode *));
        config.cluster_primary_nodes[config.cluster_primary_node_count++] = node;
    }
    if (selected) {
        config.selected_nodes = zrealloc(config.selected_nodes, (config.selected_node_count + 1) * sizeof(clusterNode *));
        config.selected_nodes[config.selected_node_count++] = node;
    }
    return config.cluster_nodes;
}

int isElastiCacheEndpoint(const char *hostname) {
    /* Must contain ElastiCache domain markers */
    if (strstr(hostname, ".cache.amazonaws.com") == NULL) {
        return 0;
    }
    return 1;
}
/* CMD (Cluster Mode Disabled) configuration prototypes */
static int64_t fetchCMDNodesConfiguration(void);
static int64_t setupElastiCacheCMDNodes(void);
static sds constructElastiCacheReaderEndpoint(const char *hostname);
static int64_t setupOpenSourceCMDPrimary(valkeyReply *info_reply);
static int64_t setupOpenSourceCMDReplica(valkeyReply *info_reply);
// static serverConfig *getServerConfigSafe(enum valkeyConnectionType ct, const char *host, int port);

/**
 * Fetch nodes configuration for Cluster Mode Disabled (CMD) setup.
 * Uses INFO REPLICATION to discover primary and replica topology.
 * 
 * INFO REPLICATION format:
 * role:master
 * connected_slaves:1
 * slave0:ip=10.21.0.202,port=6379,state=online,offset=121817956112,lag=0,type=replica
 * 
 * For ElastiCache CMD: creates primary + reader endpoint (synthesized from hostname).
 * For open-source: creates primary + individual replica nodes from INFO REPLICATION.
 * 
 * Returns 1 on success, 0 on failure.
 */
static int64_t fetchCMDNodesConfiguration(void) {
    int64_t success = 1;
    valkeyContext *ctx = NULL;
    valkeyReply *reply = NULL;
    
    ctx = config.conn_ctx;
    if (ctx == NULL) {
        fprintf(stderr, "No existing connection context, creating new\n");
        ctx = getValkeyContext(config.ct, config.conn_info.hostip, config.conn_info.hostport);
        if (ctx == NULL) {
            fprintf(stderr, "ERROR: Failed to create connection context to %s:%d\n",
                    config.conn_info.hostip, config.conn_info.hostport);
            fflush(stderr);
            assert(0);
        }
    }

    /* Get replication info */
    reply = valkeyCommand(ctx, "INFO REPLICATION");
    if (reply == NULL || ctx->err || 
        (reply->type != VALKEY_REPLY_STRING && reply->type != VALKEY_REPLY_STATUS)) {
        fprintf(stderr, "ERROR: INFO REPLICATION failed: %s\n", 
                ctx->err ? ctx->errstr : "unexpected response type");
        success = 0;
        goto cleanup;
    }
    
    /* Parse INFO REPLICATION output */
    char *info = reply->str;
    char *role_line = strstr(info, "role:");
    if (!role_line) {
        fprintf(stderr, "ERROR: Could not find 'role:' in INFO REPLICATION output\n");
        success = 0;
        goto cleanup;
    }
    
    char role[32];
    sscanf(role_line, "role:%31s", role);
    
    if (strcmp(role, "master") == 0) {
        printf("Detected primary node, using INFO REPLICATION for replica discovery\n");
        success = setupOpenSourceCMDPrimary(reply);
    } else if (strcmp(role, "slave") == 0) {
        printf("Detected replica node, connecting to primary\n");
        success = setupOpenSourceCMDReplica(reply);
    } else {
        fprintf(stderr, "ERROR: Unknown role '%s' in INFO REPLICATION\n", role);
        success = 0;
    }

cleanup:
    if (reply) freeReplyObject(reply);
    
    if (!success && config.cluster_nodes) {
        freeClusterNodes();
    }
    
    return success;
}

/**
 * Setup nodes for ElastiCache CMD by synthesizing reader endpoint.
 * Pattern: <primary-host> → <primary-host with -ro inserted before .ng. or domain>
 * 
 * ElastiCache exposes replicas via DNS load-balanced reader endpoint, not individual IPs.
 */
static int64_t setupElastiCacheCMDNodes(void) {
    /* Add primary node */
    clusterNode *primary = createClusterNode((char *)config.conn_info.hostip, 
                                              config.conn_info.hostport);
    if (!primary) return 0;
    
    primary->name = sdsnew("primary");
    for (int64_t slot = 0; slot < CLUSTER_SLOTS; slot++) {
        primary->slots[primary->slots_count++] = slot;
    }

    if (!addClusterNode(primary, 1)) {
        freeClusterNode(primary);
        return 0;
    }
    
    /* Synthesize reader endpoint if ElastiCache pattern detected */
    sds reader_hostname = constructElastiCacheReaderEndpoint(config.conn_info.hostip);
    if (reader_hostname == NULL) {
        printf("No ElastiCache reader endpoint pattern detected in hostname %s, skipping\n",
                config.conn_info.hostip);
        /* Not an ElastiCache endpoint pattern - no reader to add */
        return 1;
    }
    
    /* Verify reader endpoint is reachable */
    valkeyContext *test_ctx = valkeyConnect(reader_hostname, config.conn_info.hostport);
    if (test_ctx == NULL || test_ctx->err) {
        fprintf(stderr, "WARNING: Synthesized reader endpoint %s:%d unreachable, skipping\n",
                reader_hostname, config.conn_info.hostport);
        sdsfree(reader_hostname);
        if (test_ctx) valkeyFree(test_ctx);
        assert(0);
        // return 1; /* Non-fatal - primary still usable */
    }
    valkeyFree(test_ctx);
    
    /* Add reader node */
    clusterNode *reader = createClusterNode(reader_hostname, config.conn_info.hostport);
    if (!reader) {
        fprintf(stderr, "ERROR: Failed to create reader node for %s:%d\n",
                reader_hostname, config.conn_info.hostport);
        sdsfree(reader_hostname);
        fflush(stderr);
        assert(0);
        // sdsfree(reader_hostname);
        // return 0;
    }
    
    reader->name = sdsnew("reader-endpoint");
    reader->flags = 1; /* Mark as replica endpoint */
    reader->replicate = sdsnew(primary->name);
    
    for (int64_t slot = 0; slot < CLUSTER_SLOTS; slot++) {
        reader->slots[reader->slots_count++] = slot;
    }

    if (!addClusterNode(reader, 0)) {
        freeClusterNode(reader);
        return 0;
    }
    
    primary->replicas_count = 1; /* Logical count - reader represents N replicas */
    return 1;
}

/**
 * Construct ElastiCache reader endpoint from primary hostname.
 * 
 * Patterns:
 *   CMD: xxx.ng.0001.region.cache.amazonaws.com → xxx-ro.ng.0001.region.cache.amazonaws.com
 *   CMD: xxx.ajfdds.ng.0001.region.cache.amazonaws.com → xxx-ro.ajfdds.ng.0001.region.cache.amazonaws.com
 * 
 * Returns allocated sds with reader hostname, or NULL if pattern not detected.
 */
static sds constructElastiCacheReaderEndpoint(const char *hostname) {
    /* Must contain ElastiCache domain markers */
    
    if (!isElastiCacheEndpoint(hostname)) {
        return NULL;
    }
    /* Already a -ro endpoint */
    if (strstr(hostname, "-ro.") != NULL) {
        return NULL;
    }
    
    /* Find the first dot - this is after the cluster name prefix */
    const char *first_dot = strchr(hostname, '.');
    if (first_dot == NULL) {
        return NULL;
    }
    
    /* Insert -ro before the first dot:
     * ec-search-ec-cmd.ajfdds.ng... → ec-search-ec-cmd-ro.ajfdds.ng... */
    size_t prefix_len = first_dot - hostname;
    sds reader = sdsnewlen(hostname, prefix_len);
    reader = sdscat(reader, "-ro");
    reader = sdscat(reader, first_dot);
    
    return reader;
}
/**
 * Setup nodes for open-source Valkey CMD when connected to primary.
 * Parses INFO REPLICATION response to enumerate individual replica endpoints.
 * 
 * INFO REPLICATION format:
 * slave0:ip=10.21.0.202,port=6379,state=online,offset=121817956112,lag=0,type=replica
 */
static int64_t setupOpenSourceCMDPrimary(valkeyReply *info_reply) {
    /* Add primary node */
    clusterNode *primary = createClusterNode((char *)config.conn_info.hostip, 
                                              config.conn_info.hostport);
    if (!primary) return 0;
    
    primary->name = sdsnew("primary");
    for (int64_t slot = 0; slot < CLUSTER_SLOTS; slot++) {
        primary->slots[primary->slots_count++] = slot;
    }
    
    if (!addClusterNode(primary, 1)) {
        freeClusterNode(primary);
        return 0;
    }
    
    /* Parse replicas from INFO REPLICATION: slave0:ip=X,port=Y,... */
    char *info = info_reply->str;
    char *line = info;
    int64_t replica_idx = 0;
    
    while (line) {
        /* Look for slave lines: "slave0:ip=..." */
        char *slave_line = strstr(line, "slave");
        if (!slave_line) break;
        
        /* Parse: slaveN:ip=X,port=Y,state=Z,... */
        char ip[256];
        int port = 0;
        char state[32];
        
        char *ip_start = strstr(slave_line, "ip=");
        char *port_start = strstr(slave_line, "port=");
        char *state_start = strstr(slave_line, "state=");
        
        if (ip_start && port_start) {
            ip_start += 3;  /* Skip "ip=" */
            char *ip_end = strchr(ip_start, ',');
            if (ip_end) {
                size_t ip_len = ip_end - ip_start;
                if (ip_len < sizeof(ip)) {
                    memcpy(ip, ip_start, ip_len);
                    ip[ip_len] = '\0';
                    
                    port_start += 5;  /* Skip "port=" */
                    port = atoi(port_start);
                    
                    /* Check if replica is online */
                    int64_t is_online = 1;
                    if (state_start) {
                        state_start += 6;  /* Skip "state=" */
                        char *state_end = strchr(state_start, ',');
                        if (state_end) {
                            size_t state_len = state_end - state_start;
                            if (state_len < sizeof(state)) {
                                memcpy(state, state_start, state_len);
                                state[state_len] = '\0';
                                is_online = (strcmp(state, "online") == 0);
                            }
                        }
                    }
                    
                    if (is_online && port > 0) {
                        clusterNode *replica = createClusterNode(sdsnew(ip), port);
                        if (!replica) return 0;
                        
                        replica->name = sdscatprintf(sdsempty(), "replica-%ld", replica_idx);
                        replica->flags = 1;
                        replica->replicate = sdsnew(primary->name);
                        
                        for (int64_t slot = 0; slot < CLUSTER_SLOTS; slot++) {
                            replica->slots[replica->slots_count++] = slot;
                        }
                        
                        printf("Found replica %ld: %s:%d (state=%s)\n", replica_idx, ip, port, state);
                        
                        if (!addClusterNode(replica, 0)) {
                            freeClusterNode(replica);
                            return 0;
                        }
                        
                        primary->replicas_count++;
                        replica_idx++;
                    }
                }
            }
        }
        
        /* Move to next line */
        line = strchr(slave_line, '\n');
        if (line) line++;
    }
    
    printf("Primary configured with %d replicas\n", primary->replicas_count);
    return 1;
}

/**
 * Setup nodes for open-source Valkey CMD when connected to replica.
 * Parses INFO REPLICATION to find primary, then adds current replica.
 * 
 * INFO REPLICATION format when connected to replica:
 * role:slave
 * master_host:172.31.36.220
 * master_port:6379
 */
static int64_t setupOpenSourceCMDReplica(valkeyReply *info_reply) {
    char *info = info_reply->str;
    
    /* Parse master host and port */
    char *master_host_line = strstr(info, "master_host:");
    char *master_port_line = strstr(info, "master_port:");
    
    if (!master_host_line || !master_port_line) {
        fprintf(stderr, "ERROR: Could not find master_host or master_port in INFO REPLICATION\n");
        return 0;
    }
    
    char primary_ip[256];
    int primary_port;
    
    sscanf(master_host_line, "master_host:%255s", primary_ip);
    sscanf(master_port_line, "master_port:%d", &primary_port);

    /* Add primary node */
    clusterNode *primary = createClusterNode(sdsnew(primary_ip), primary_port);
    if (!primary) return 0;
    
    primary->name = sdsnew("primary");
    for (int64_t slot = 0; slot < CLUSTER_SLOTS; slot++) {
        primary->slots[primary->slots_count++] = slot;
    }
    if (!addClusterNode(primary, 1)) {
        freeClusterNode(primary);
        return 0;
    }

    printf("Found primary: %s:%d\n", primary_ip, primary_port);

    /* Add current replica node */
    clusterNode *replica = createClusterNode((char *)config.conn_info.hostip,
                                              config.conn_info.hostport);
    if (!replica) return 0;
    
    replica->name = sdsnew("replica-0");
    replica->flags = 1;
    replica->replicate = sdsnew(primary->name);
    
    for (int64_t slot = 0; slot < CLUSTER_SLOTS; slot++) {
        replica->slots[replica->slots_count++] = slot;
    }
    if (!addClusterNode(replica, 0)) {
        freeClusterNode(replica);
        return 0;
    }
    
    primary->replicas_count = 1;
    printf("Configured replica node: %s:%d\n", config.conn_info.hostip, config.conn_info.hostport);
    return 1;
}

/* Fetch the cluster configuration by calling CLUSTER NODES and update
 * the internal representation of the cluster nodes accordingly. 
 * 
 * CLUSTER NODES format:
 * <id> <ip:port@cport> <flags> <master> <ping-sent> <pong-recv> <config-epoch> <link-state> <slot> <slot> ... <slot>
 * 
 * Examples:
 * - Master: f5f9bdad... 172.31.36.220:6379@1122 myself,master - 0 0 1 connected 0-16383
 * - Replica: ebdd1dc9... 172.31.20.20:6379@1122 slave f5f9bdad... 0 1761043963894 1 connected
 */
static int64_t fetchClusterConfiguration(void) {
    int64_t success = 1;
    valkeyContext *ctx = NULL;
    valkeyReply *reply = NULL;
    dict *nodes = NULL;
    const char *errmsg = "Failed to fetch cluster configuration";
    
    ctx = config.conn_ctx;
    if (ctx == NULL) {
        fprintf(stderr, "No existing connection context, creating new\n");
        ctx = getValkeyContext(config.ct, config.conn_info.hostip, config.conn_info.hostport);
        if (ctx == NULL) {
            assert(0);
        }
    }

    /* Try CLUSTER NODES first - gives us topology with explicit master/slave roles */
    reply = valkeyCommand(ctx, "CLUSTER NODES");
    if (reply == NULL || reply->type == VALKEY_REPLY_ERROR) {
        success = 0;
        if (reply) fprintf(stderr, "%s\nCLUSTER NODES ERROR: %s\n", errmsg, reply->str);
        goto cleanup;
    }
    
    if (reply->type != VALKEY_REPLY_STRING && reply->type != VALKEY_REPLY_STATUS) {
        fprintf(stderr, "%s\nUnexpected CLUSTER NODES response type: %d\n", errmsg, reply->type);
        success = 0;
        goto cleanup;
    }
    
    nodes = dictCreate(&dtype);
    
    /* Parse CLUSTER NODES line by line */
    char *line, *saveptr;
    char *nodes_str = sdsnew(reply->str);
    line = strtok_r(nodes_str, "\n", &saveptr);
    
    while (line != NULL) {
        printf("Parsing CLUSTER NODES line: %s\n", line);
        /* Parse: <id> <ip:port@cport> <flags> <master-id> <ping> <pong> <epoch> <state> <slots...> */
        char node_id[128], addr[256], flags[256], master_id[128];
        int64_t ping_sent, pong_recv, config_epoch;
        char link_state[32];
        
        int64_t parsed = sscanf(line, "%127s %255s %255s %127s %ld %ld %ld %31s",
                           node_id, addr, flags, master_id, 
                           &ping_sent, &pong_recv, &config_epoch, link_state);
        
        if (parsed < 8) {
            printf("Skipping malformed CLUSTER NODES line: %s\n", line);
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }
        
        /* Extract IP and port from ip:port@cport format */
        char *at_sign = strchr(addr, '@');
        if (at_sign) *at_sign = '\0';  /* Remove @cport */
        
        char *colon = strchr(addr, ':');
        if (!colon) {
            printf("Invalid address format in CLUSTER NODES: %s\n", addr);
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }
        
        *colon = '\0';
        char *ip_str = addr;
        int port = atoi(colon + 1);
        
        /* Determine if this is a master or replica from flags */
        int64_t is_master = (strstr(flags, "master") != NULL);
        int64_t is_slave = (strstr(flags, "slave") != NULL);
        
        if (!is_master && !is_slave) {
            printf("Skipping node with neither master nor slave flag: %s\n", node_id);
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }
        
        /* Check if node already exists */
        sds name = sdsnew(node_id);
        dictEntry *entry = dictFind(nodes, name);
        clusterNode *node = NULL;
        
        if (entry == NULL) {
            /* Create new node */
            sds ip = sdsnew(ip_str);
            /* Handle empty IP (means use the connection IP) */
            if (strlen(ip_str) == 0 || strcmp(ip_str, "") == 0) {
                sdsfree(ip);
                ip = sdsnew(config.conn_info.hostip);
            }
            
            node = createClusterNode(ip, port);
            if (node == NULL) {
                sdsfree(name);
                success = 0;
                goto cleanup;
            }
            
            node->name = name;
            
            /* Set replica relationship */
            if (is_slave && strcmp(master_id, "-") != 0) {
                node->replicate = sdsnew(master_id);
            }
            
            /* Parse slot ranges from remaining tokens - need to find them after the 8th field */
            char *slots_start = line;
            int64_t field_count = 0;
            /* Skip to the 9th field (after link_state) */
            while (*slots_start && field_count < 8) {
                if (*slots_start == ' ') {
                    field_count++;
                    while (*slots_start == ' ') slots_start++;  /* Skip multiple spaces */
                } else {
                    slots_start++;
                }
            }
            
            /* Now parse slot ranges from slots_start */
            if (*slots_start && is_master) {
                char *slots_str = sdsnew(slots_start);
                char *slot_token, *slot_saveptr;
                slot_token = strtok_r(slots_str, " ", &slot_saveptr);
                
                while (slot_token != NULL) {
                    /* Slot can be: "0-5460" or "5461" or "[0->-node_id]" (migrating) */
                    if (slot_token[0] == '[') {
                        /* Skip migrating/importing slots */
                        slot_token = strtok_r(NULL, " ", &slot_saveptr);
                        continue;
                    }
                    
                    char *dash = strchr(slot_token, '-');
                    if (dash) {
                        /* Slot range: start-end */
                        int slot_start = atoi(slot_token);
                        int slot_end = atoi(dash + 1);
                        for (int slot = slot_start; slot <= slot_end; slot++) {
                            node->slots[node->slots_count++] = slot;
                        }
                    } else {
                        /* Single slot */
                        int slot = atoi(slot_token);
                        node->slots[node->slots_count++] = slot;
                    }
                    
                    slot_token = strtok_r(NULL, " ", &slot_saveptr);
                }
                
                sdsfree(slots_str);
            }
            
            dictReplace(nodes, node->name, node);
            
            printf("Node %s (%s:%d) is %s, slots=%d\n", 
                   node_id, ip_str, port, 
                   is_master ? "master" : "slave", 
                   node->slots_count);

            if (!addClusterNode(node, is_master)) {
                success = 0;
                goto cleanup;
            }
        } else {
            sdsfree(name);
        }
        
        line = strtok_r(NULL, "\n", &saveptr);
    }
    
    sdsfree(nodes_str);
    
cleanup:
    if (!success) {
        if (config.cluster_nodes) freeClusterNodes();
    }
    if (reply) freeReplyObject(reply);
    if (nodes) dictRelease(nodes);
    /* Free the context if we created a new one */
    if (ctx != config.conn_ctx) {
        valkeyFree(ctx);
    }
    return success;
}

/* Request the current cluster slots configuration by calling CLUSTER SLOTS
 * and atomically update the slots after a successful reply. */
static int64_t fetchClusterSlotsConfiguration(client c) {
    UNUSED(c);
    int64_t success = 1, is_fetching_slots = 0, last_update = 0;
    size_t i, j;

    last_update = atomic_load_explicit(&config.slots_last_update, memory_order_relaxed);
    if (c->slots_last_update < last_update) {
        c->slots_last_update = last_update;
        return -1;
    }
    valkeyReply *reply = NULL;

    is_fetching_slots = atomic_fetch_add_explicit(&config.is_fetching_slots, 1, memory_order_relaxed);
    if (is_fetching_slots) return -1; // TODO: use other codes || errno ?
    atomic_store_explicit(&config.is_fetching_slots, 1, memory_order_relaxed);
    fprintf(stderr, "WARNING: Cluster slots configuration changed, fetching new one...\n");
    const char *errmsg = "Failed to update cluster slots configuration";

    /* printf("[%ld] fetchClusterSlotsConfiguration\n", c->thread_id); */
    dict *nodes = dictCreate(&dtype);
    // valkeyContext *ctx = NULL;
    for (i = 0; i < (size_t)config.cluster_node_count; i++) {
        clusterNode *node = config.cluster_nodes[i];
        assert(node->ip != NULL);
        assert(node->name != NULL);
        assert(node->port);
        /* Use first node as entry point to connect to. */
        if (node->ctx == NULL) {
            node->ctx = getValkeyContext(config.ct, node->ip, node->port);
            if (!node->ctx) {
                success = 0;
                goto cleanup;
            }
        }
        if (node->updated_slots != NULL) zfree(node->updated_slots);
        node->updated_slots = NULL;
        node->updated_slots_count = 0;
        dictReplace(nodes, node->name, node);
    }
    reply = valkeyCommand(config.cluster_nodes[0]->ctx, "CLUSTER SLOTS");
    if (reply == NULL || reply->type == VALKEY_REPLY_ERROR) {
        success = 0;
        if (reply) fprintf(stderr, "%s\nCLUSTER SLOTS ERROR: %s\n", errmsg, reply->str);
        goto cleanup;
    }
    assert(reply->type == VALKEY_REPLY_ARRAY);
    for (i = 0; i < reply->elements; i++) {
        valkeyReply *r = reply->element[i];
        assert(r->type == VALKEY_REPLY_ARRAY);
        assert(r->elements >= 3);
        int64_t from, to, slot;
        from = r->element[0]->integer;
        to = r->element[1]->integer;
        size_t start, end;
        if (config.read_from_replica == FROM_ALL) {
            start = 2;
            end = r->elements;
        } else if (config.read_from_replica == FROM_REPLICA_ONLY) {
            start = 3;
            end = r->elements;
        } else {
            start = 2;
            end = 3;
        }

        for (j = start; j < end; j++) {
            valkeyReply *nr = r->element[j];
            assert(nr->type == VALKEY_REPLY_ARRAY && nr->elements >= 3);
            assert(nr->element[2]->str != NULL);
            sds name = sdsnew(nr->element[2]->str);
            dictEntry *entry = dictFind(nodes, name);
            if (entry == NULL) {
                success = 0;
                fprintf(stderr,
                        "%s: could not find node with ID %s in current "
                        "configuration.\n",
                        errmsg, name);
                if (name) sdsfree(name);
                goto cleanup;
            }
            sdsfree(name);
            clusterNode *node = dictGetVal(entry);
            if (node->updated_slots == NULL) node->updated_slots = zcalloc(CLUSTER_SLOTS * sizeof(int64_t));
            for (slot = from; slot <= to; slot++) node->updated_slots[node->updated_slots_count++] = slot;
        }
    }
    updateClusterSlotsConfiguration();
cleanup:
    freeReplyObject(reply);
    // valkeyFree(ctx);
    dictRelease(nodes);
    atomic_store_explicit(&config.is_fetching_slots, 0, memory_order_relaxed);
    return success;
}

/* Atomically update the new slots configuration. */
static void updateClusterSlotsConfiguration(void) {
    if (!config.cluster_mode) return;
    pthread_mutex_lock(&config.is_updating_slots_mutex);
    atomic_store_explicit(&config.is_updating_slots, 1, memory_order_relaxed);

    int64_t i;
    for (i = 0; i < config.cluster_node_count; i++) {
        clusterNode *node = config.cluster_nodes[i];
        if (node->updated_slots != NULL) {
            int *oldslots = node->slots;
            node->slots = node->updated_slots;
            node->slots_count = node->updated_slots_count;
            node->updated_slots = NULL;
            node->updated_slots_count = 0;
            zfree(oldslots);
        }
    }
    atomic_store_explicit(&config.is_updating_slots, 0, memory_order_relaxed);
    atomic_fetch_add_explicit(&config.slots_last_update, 1, memory_order_relaxed);
    pthread_mutex_unlock(&config.is_updating_slots_mutex);
}

/* Generate random data for the benchmark. See #7196. */
static void genBenchmarkRandomData(char *data, int64_t count) {
    static uint32_t state = 1234;
    int64_t i = 0;

    while (count--) {
        state = (state * 1103515245 + 12345);
        data[i++] = '0' + ((state >> 16) & 63);
    }
}

/* Parse tag distributions from command line */
static void parseTagDistributions(const char *distributions_str) {
    sds str = sdsnew(distributions_str);
    int64_t count = 0;
    char *token;
    double cumulative = 0.0;
    
    /* First pass: count distributions */
    sds temp = sdsdup(str);
    char *saveptr;
    token = strtok_r(temp, ",", &saveptr);
    while (token) {
        count++;
        token = strtok_r(NULL, ",", &saveptr);
    }
    sdsfree(temp);
    
    /* Allocate array */
    config.search.curr_conf.tag_dists = zcalloc(sizeof(tagDistribution) * count);
    config.search.curr_conf.n_dists = count;
    
    /* Second pass: parse distributions */
    int64_t i = 0;
    token = strtok_r(str, ",", &saveptr);
    while (token) {
        char *colon = strchr(token, ':');
        if (!colon) {
            fprintf(stderr, "Invalid tag distribution format: %s\n", token);
            assert(0);
        }
        
        *colon = '\0';
        char *tag = token;
        double percentage = atof(colon + 1);
        
        cumulative += percentage;
        config.search.curr_conf.tag_dists[i].pattern = sdsnew(tag);
        config.search.curr_conf.tag_dists[i].percentage = percentage;
        config.search.curr_conf.tag_dists[i].cumulative = cumulative;
        
        i++;
        token = strtok_r(NULL, ",", &saveptr);
    }
    
    sdsfree(str);
    
    /* Note: Percentages are independent probabilities, they don't need to sum to 100% */
    /* Each percentage represents the probability that tag will be included */
}

/* Select tags based on independent probabilities - each tag has its own probability */
static sds selectTagByDistribution(void) {
    if (!config.search.curr_conf.tag_dists || config.search.curr_conf.n_dists == 0) {
        return NULL;
    }
    
    sds tags = sdsempty();
    int64_t first = 1;
    
    /* Each tag has an independent probability of being included */
    for (int64_t i = 0; i < config.search.curr_conf.n_dists; i++) {
        double random_percent = ((double)rand() / RAND_MAX) * 100.0;
        
        /* Include this tag if random falls within its percentage */
        if (random_percent <= config.search.curr_conf.tag_dists[i].percentage) {
            /* Process pattern with placeholders */
            sds tag = sdsdup(config.search.curr_conf.tag_dists[i].pattern);
            
            /* Replace __rand_int__ placeholder if present */
            if (strstr(tag, "__rand_int__")) {
                char rand_str[32];
                snprintf(rand_str, sizeof(rand_str), "%d", rand() % 1000000);
                char *pos = strstr(tag, "__rand_int__");
                if (pos) {
                    sds prefix = sdsnewlen(tag, pos - tag);
                    sds suffix = sdsnew(pos + strlen("__rand_int__"));
                    sdsfree(tag);
                    tag = sdscatprintf(prefix, "%s%s", rand_str, suffix);
                    sdsfree(suffix);
                }
            }
            
            /* Add tag to list with comma separator */
            if (!first) {
                tags = sdscat(tags, ",");
            }
            tags = sdscat(tags, tag);
            sdsfree(tag);
            first = 0;
        }
    }
    
    /* Return NULL if no tags selected, otherwise return tag list */
    if (sdslen(tags) == 0) {
        sdsfree(tags);
        return NULL;
    }
    
    return tags;
}

void setDefaultSearchConfig(void) {
    config.search.name = sdsnew("test_vector_index");
    config.search.prefix = sdsnew("vec:");
    config.search.vector_field = sdsnew("vector_field");
    config.search.vector_dim = 128; // Default vector dimension
    config.search.ef_construction = 256; // Default EF Construction
    config.search.ef_search = 256; // Default EF Search
    config.search.m = 16; // Default HNSW M parameter
    config.search.tag_field = NULL; // No tag field by default
    config.search.payload_tag_len = 128; // Default max tag length
    config.search.numeric_field = NULL; // No numeric field by default
    config.search.k = 10; // Default K for KNN queries
    config.search.curr_conf.tag_dists = NULL;
    config.search.curr_conf.n_dists = 0;
    config.search.curr_conf.tag_filter = NULL;
    config.search.metric = sdsnew("COSINE");
    config.search.algorithm = sdsnew("hnsw"); // Default algorithm
    config.search.nocontent = 0; // exclude content by default
    config.search.localonly = 0; // Default LOCALONLY option
}
/* Returns number of consumed options. */
int parseOptions(int argc, char **argv) {
    int64_t i;
    int64_t lastarg;
    int64_t exit_status = 1;
    char *tls_usage;
    char *rdma_usage;
    char *search_usage_part1;
    char *search_usage_part2;
    char *search_examples;
    for (i = 1; i < argc; i++) {
        lastarg = (i == (argc - 1));

        if (!strcmp(argv[i], "-c")) {
            if (lastarg) goto invalid;
            config.numclients = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
            sds version = cliVersion();
            printf("valkey-benchmark %s\n", version);
            sdsfree(version);
            exit(0);
        } else if (!strcmp(argv[i], "-n")) {
            if (lastarg) goto invalid;
            config.requests = strtoll(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "-k")) {
            if (lastarg) goto invalid;
            config.keepalive = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-h")) {
            if (lastarg) goto invalid;
            sdsfree(config.conn_info.hostip);
            config.conn_info.hostip = sdsnew(argv[++i]);
        } else if (!strcmp(argv[i], "-p")) {
            if (lastarg) goto invalid;
            config.conn_info.hostport = atoi(argv[++i]);
            if (config.conn_info.hostport < 0 || config.conn_info.hostport > 65535) {
                fprintf(stderr, "Invalid server port.\n");
                assert(0);
            }
        } else if (!strcmp(argv[i], "-s")) {
            if (lastarg) goto invalid;
            sdsfree(config.conn_info.hostip);
            config.conn_info.hostip = sdsnew(argv[++i]);
            config.ct = VALKEY_CONN_UNIX;
        } else if (!strcmp(argv[i], "-x")) {
            config.stdinarg = 1;
        } else if (!strcmp(argv[i], "-a")) {
            if (lastarg) goto invalid;
            config.conn_info.auth = sdsnew(argv[++i]);
        } else if (!strcmp(argv[i], "--user")) {
            if (lastarg) goto invalid;
            config.conn_info.user = sdsnew(argv[++i]);
        } else if (!strcmp(argv[i], "--rps")) {
            if (lastarg) goto invalid;
            config.rps = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--balance-nodes")) {
            config.balance_nodes = 1;
        } else if (!strcmp(argv[i], "--balance-quota-step")) {
            if (lastarg) goto invalid;
            config.balance_quota_step = atoi(argv[++i]);
            if (config.balance_quota_step <= 0) {
                fprintf(stderr, "Invalid balance quota step (must be > 0)\n");
                exit(1);
            }
        } else if (!strcmp(argv[i], "--balance-tolerance")) {
            if (lastarg) goto invalid;
            config.balance_tolerance_pct = atoi(argv[++i]);
            if (config.balance_tolerance_pct < 0 || config.balance_tolerance_pct > 100) {
                fprintf(stderr, "Invalid balance tolerance (must be 0-100)\n");
                exit(1);
            }
        } else if (!strcmp(argv[i], "--test-balance")) {
            /* Test node balancing algorithm with simulated latencies */
            printf("Running node balance test...\n");
            int64_t test_latencies[] = {3000, 1000, 3000, 5000, 550, 100, 9000};
            int64_t num_test_nodes = sizeof(test_latencies) / sizeof(test_latencies[0]);
            testNodeBalancing(test_latencies, num_test_nodes, 10); /* 10 second test */
            exit(0);
        } else if (!strcmp(argv[i], "-u") && !lastarg) {
            parseUri(argv[++i], "valkey-benchmark", &config.conn_info, &config.tls);
            if (config.conn_info.hostport < 0 || config.conn_info.hostport > 65535) {
                fprintf(stderr, "Invalid server port.\n");
                assert(0);
            }
            config.input_dbnumstr = sdsfromlonglong(config.conn_info.input_dbnum);
        } else if (!strcmp(argv[i], "-3")) {
            config.resp3 = 1;
        } else if (!strcmp(argv[i], "-d")) {
            if (lastarg) goto invalid;
            config.datasize = atoi(argv[++i]);
            if (config.datasize < 1) config.datasize = 1;
            if (config.datasize > 1024 * 1024 * 1024) config.datasize = 1024 * 1024 * 1024;
        } else if (!strcmp(argv[i], "-P")) {
            if (lastarg) goto invalid;
            config.pipeline = atoi(argv[++i]);
            if (config.pipeline <= 0) config.pipeline = 1;
        } else if (!strcmp(argv[i], "-r")) {
            if (lastarg) goto invalid;
            const char *next = argv[++i], *p = next;
            if (*p == '-') {
                p++;
                if (*p < '0' || *p > '9') goto invalid;
            }
            config.replace_placeholders = 1;
            config.keyspacelen = strtoll(next, NULL, 10);
            if (config.keyspacelen < 0) config.keyspacelen = 0;
        } else if (!strcmp(argv[i], "--sequential")) {
            config.sequential_replacement = 1;
        } else if (!strcmp(argv[i], "-q")) {
            config.quiet = 1;
        } else if (!strcmp(argv[i], "--csv")) {
            config.csv = 1;
        } else if (!strcmp(argv[i], "-l")) {
            config.loop = 1;
        } else if (!strcmp(argv[i], "-I")) {
            config.idlemode = 1;
        } else if (!strcmp(argv[i], "-e")) {
            fprintf(stderr, "WARNING: -e option has no effect. "
                            "We now immediately exit on error to avoid false results.\n");
        } else if (!strcmp(argv[i], "--seed")) {
            if (lastarg) goto invalid;
            int64_t rand_seed = atoi(argv[++i]);
            srandom(rand_seed);
            init_genrand64(rand_seed);
        } else if (!strcmp(argv[i], "-t")) {
            if (lastarg) goto invalid;
            /* We get the list of tests to run as a string in the form
             * get,set,lrange,...,test_N. Then we add a comma before and
             * after the string in order to make sure that searching
             * for ",testname," will always get a match if the test is
             * enabled. */
            config.tests = sdsnew(",");
            config.tests = sdscat(config.tests, (char *)argv[++i]);
            config.tests = sdscat(config.tests, ",");
            sdstolower(config.tests);
        } else if (!strcmp(argv[i], "--dbnum")) {
            if (lastarg) goto invalid;
            config.conn_info.input_dbnum = atoi(argv[++i]);
            config.input_dbnumstr = sdsfromlonglong(config.conn_info.input_dbnum);
        } else if (!strcmp(argv[i], "--precision")) {
            if (lastarg) goto invalid;
            config.precision = atoi(argv[++i]);
            if (config.precision < 0) config.precision = DEFAULT_LATENCY_PRECISION;
            if (config.precision > MAX_LATENCY_PRECISION) config.precision = MAX_LATENCY_PRECISION;
        } else if (!strcmp(argv[i], "--threads")) {
            if (lastarg) goto invalid;
            config.num_threads = atoi(argv[++i]);
            if (config.num_threads > MAX_THREADS) {
                fprintf(stderr, "WARNING: Too many threads, limiting threads to %d.\n", MAX_THREADS);
                config.num_threads = MAX_THREADS;
            } else if (config.num_threads < 0)
                config.num_threads = 0;        
        } else if (!strcmp(argv[i], "--rfr")) {
            if (argv[++i]) {
                if (!strcmp(argv[i], "all")) {
                    config.read_from_replica = FROM_ALL;
                } else if (!strcmp(argv[i], "yes")) {
                    config.read_from_replica = FROM_REPLICA_ONLY;
                } else if (!strcmp(argv[i], "no")) {
                    config.read_from_replica = FROM_PRIMARY_ONLY;
                } else {
                    goto invalid;
                }
            } else
                goto invalid;
        } else if (!strcmp(argv[i], "--enable-tracking")) {
            config.enable_tracking = 1;
        } else if (!strcmp(argv[i], "--clean")) {
            config.clean = 1;
        } else if (!strcmp(argv[i], "--search")) {
            // TODO: Is search is enabled and -t is not, do not run default tests
            config.use_search = 1;
        } else if (!strcmp(argv[i], "--nocontent")) {
            config.search.nocontent = 1;
        } else if (!strcmp(argv[i], "--localonly")) {
            config.search.localonly = 1;
        } else if (!strcmp(argv[i], "--dataset")) {
            if (lastarg) goto invalid;
            config.dataset_name = sdsnew(argv[++i]);
            config.use_dataset = 1;
        } else if (!strcmp(argv[i], "--dataset-path")) {
            if (lastarg) goto invalid;
            sdsfree(config.dataset_name);
            config.dataset_name = sdsnew(argv[++i]);
            config.use_dataset = 1;
        } else if (!strcmp(argv[i], "--filtered")) {
            config.use_filtered_search = 1;
        } else if (!strcmp(argv[i], "--optimize")) {
            config.optimize_enabled = 1;
        } else if (!strcmp(argv[i], "--optimize-objective")) {
            if (lastarg) goto invalid;
            if (config.optimize_objective) sdsfree(config.optimize_objective);
            config.optimize_objective = sdsnew(argv[++i]);
        } else if (!strcmp(argv[i], "--optimize-constraint")) {
            if (lastarg) goto invalid;
            config.optimize_constraints = realloc(config.optimize_constraints,
                                                 (config.num_optimize_constraints + 1) * sizeof(sds));
            config.optimize_constraints[config.num_optimize_constraints++] = sdsnew(argv[++i]);
        } else if (!strcmp(argv[i], "--optimize-csv")) {
            if (lastarg) goto invalid;
            if (config.optimize_csv_file) sdsfree(config.optimize_csv_file);
            config.optimize_csv_file = sdsnew(argv[++i]);
        } else if (!strcmp(argv[i], "--optimize-max-iterations")) {
            if (lastarg) goto invalid;
            config.optimize_max_iterations = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--optimize-min-requests")) {
            if (lastarg) goto invalid;
            config.optimize_min_requests = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--optimize-client-range")) {
            if (lastarg) goto invalid;
            if (config.optimize_client_range) sdsfree(config.optimize_client_range);
            config.optimize_client_range = sdsnew(argv[++i]);
        } else if (!strcmp(argv[i], "--optimize-thread-range")) {
            if (lastarg) goto invalid;
            if (config.optimize_thread_range) sdsfree(config.optimize_thread_range);
            config.optimize_thread_range = sdsnew(argv[++i]);
        } else if (!strcmp(argv[i], "--optimize-ef-search-range")) {
            if (lastarg) goto invalid;
            if (config.optimize_ef_search_range) sdsfree(config.optimize_ef_search_range);
            config.optimize_ef_search_range = sdsnew(argv[++i]);
        } else if (!strcmp(argv[i], "--optimize-pipeline-range")) {
            if (lastarg) goto invalid;
            if (config.optimize_pipeline_range) sdsfree(config.optimize_pipeline_range);
            config.optimize_pipeline_range = sdsnew(argv[++i]);
        } else if (!strcmp(argv[i], "--runtime-config")) {
            if (lastarg) goto invalid;
            if (config.runtime_config_file) sdsfree(config.runtime_config_file);
            config.runtime_config_file = sdsnew(argv[++i]);
        } else if (!strcmp(argv[i], "--restore-config")) {
            config.restore_runtime_config = 1;
        } else if (!strcmp(argv[i], "--search-print-results")) {
            config.print_search_results = 1;
        } else if (!strcmp(argv[i], "--search-prefix")) {
            if (lastarg) goto invalid;
            if (config.search.prefix) sdsfree(config.search.prefix);
            config.search.prefix = sdsnew(argv[++i]);
        } else if (!strcmp(argv[i], "--vector-field")) {
            if (lastarg) goto invalid;
            if (config.search.vector_field) sdsfree(config.search.vector_field);
            config.search.vector_field = sdsnew(argv[++i]);
        } else if (!strcmp(argv[i], "--search-name")) {
            if (lastarg) goto invalid;
            if (config.search.name) sdsfree(config.search.name);
            config.search.name = sdsnew(argv[++i]);
        } else if (!strcmp(argv[i], "--vector-dim")) {
            if (lastarg) goto invalid;
            config.search.vector_dim = atoi(argv[++i]);
            if (config.search.vector_dim <= VECTOR_NUM_RAND_DIM) {
                fprintf(stderr, "Invalid vector dimension: %ld\n", config.search.vector_dim);
                goto invalid;
            }
        } else if (!strcmp(argv[i], "--ef-search")) {
            if (lastarg) goto invalid;
            config.search.ef_search = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--ef-construction")) {
            if (lastarg) goto invalid;
            config.search.ef_construction = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--m")) {
            if (lastarg) goto invalid;
            config.search.m = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--tag-field")) {
            if (lastarg) goto invalid;
            if (config.search.tag_field) sdsfree(config.search.tag_field);
            config.search.tag_field = sdsnew(argv[++i]);
        } else if (!strcmp(argv[i], "--tag-filter")) {
            if (lastarg) goto invalid;
            if (config.search.curr_conf.tag_filter) sdsfree(config.search.curr_conf.tag_filter);
            config.search.curr_conf.tag_filter = sdsnew(argv[++i]);
        } else if (!strcmp(argv[i], "--search-tags")) {
            if (lastarg) goto invalid;
            parseTagDistributions(argv[++i]);
        } else if (!strcmp(argv[i], "--numeric-field")) {
            if (lastarg) goto invalid;
            if (config.search.numeric_field) sdsfree(config.search.numeric_field);
            config.search.numeric_field = sdsnew(argv[++i]);
        } else if (!strcmp(argv[i], "--search-alg")) {
            if (lastarg) goto invalid;
            if (strcmp(argv[i + 1], "hnsw") && strcmp(argv[i + 1], "flat")) {
                goto invalid;
            }
            if (config.search.algorithm) sdsfree(config.search.algorithm);
            config.search.algorithm = sdsnew(argv[++i]);
        } else if (!strcmp(argv[i], "--metric")) {            
            if (lastarg) goto invalid;
            if (strcmp(argv[i + 1], "L2") && strcmp(argv[i + 1], "IP") && strcmp(argv[i + 1], "COSINE")) {
                goto invalid;
            }
            if (config.search.metric) sdsfree(config.search.metric);
            config.search.metric = sdsnew(argv[++i]);
        } else if (!strcmp(argv[i], "--k")) {
            if (lastarg) goto invalid;
            config.search.k = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--num-functions")) {
            config.num_functions = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--num-keys-in-fcall")) {
            config.num_keys_in_fcall = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--save-config")) {
            config.save_config = 1;
        } else if (!strcmp(argv[i], "--no-save-config")) {
            config.no_save_config = 1;
        } else if (!strcmp(argv[i], "--no-baseline")) {
            config.no_baseline = 1;
        } else if (!strcmp(argv[i], "--baseline-latency")) {
            /* Explicitly enable (though it's default) - kept for backward compatibility */
            config.no_baseline = 0;
        } else if (!strcmp(argv[i], "--clear-config")) {
            if (config_persist_clear() == 0) {
                printf("Configuration cleared successfully.\n");
                exit(0);
            } else {
                fprintf(stderr, "Failed to clear configuration.\n");
                exit(1);
            }
        } else if (!strcmp(argv[i], "--show-config")) {
            persisted_config_t saved_config;
            memset(&saved_config, 0, sizeof(saved_config));
            if (config_persist_load(&saved_config) == 0) {
                config_persist_show(&saved_config, 0);
                config_persist_free(&saved_config);
            } else {
                printf("No saved configuration found.\n");
            }
            exit(0);
        } else if (!strcmp(argv[i], "--help")) {
            exit_status = 0;
            goto usage;
#ifdef USE_OPENSSL
        } else if (!strcmp(argv[i], "--tls")) {
            config.tls = 1;
        } else if (!strcmp(argv[i], "--sni")) {
            if (lastarg) goto invalid;
            config.sslconfig.sni = strdup(argv[++i]);
        } else if (!strcmp(argv[i], "--cacertdir")) {
            if (lastarg) goto invalid;
            config.sslconfig.cacertdir = strdup(argv[++i]);
        } else if (!strcmp(argv[i], "--cacert")) {
            if (lastarg) goto invalid;
            config.sslconfig.cacert = strdup(argv[++i]);
        } else if (!strcmp(argv[i], "--insecure")) {
            config.sslconfig.skip_cert_verify = 1;
        } else if (!strcmp(argv[i], "--cert")) {
            if (lastarg) goto invalid;
            config.sslconfig.cert = strdup(argv[++i]);
        } else if (!strcmp(argv[i], "--key")) {
            if (lastarg) goto invalid;
            config.sslconfig.key = strdup(argv[++i]);
        } else if (!strcmp(argv[i], "--tls-ciphers")) {
            if (lastarg) goto invalid;
            config.sslconfig.ciphers = strdup(argv[++i]);
#ifdef TLS1_3_VERSION
        } else if (!strcmp(argv[i], "--tls-ciphersuites")) {
            if (lastarg) goto invalid;
            config.sslconfig.ciphersuites = strdup(argv[++i]);
#endif
#endif
#ifdef USE_RDMA
        } else if (!strcmp(argv[i], "--rdma")) {
            if (valkeyInitiateRdma() != VALKEY_OK) {
                fprintf(stderr, "Failed to initialize RDMA support from libvalkey\n");
                assert(0);
            }
            config.ct = VALKEY_CONN_RDMA;
#endif
        } else if (!strcmp(argv[i], "--mptcp")) {
            config.mptcp = 1;
        } else if (!strcmp(argv[i], "--")) {
            /* End of options. */
            return i + 1;
        } else {
            /* Assume the user meant to provide an option when the arg starts
             * with a dash. We're done otherwise and should use the remainder
             * as the command and arguments for running the benchmark. */
            if (argv[i][0] == '-') goto invalid;
            return i;
        }
    }

    return i;

invalid:
    printf("Invalid option \"%s\" or option argument missing\n\n", argv[i]);

usage:
    tls_usage =
#ifdef USE_OPENSSL
        " --tls              Establish a secure TLS connection.\n"
        " --sni <host>       Server name indication for TLS.\n"
        " --cacert <file>    CA Certificate file to verify with.\n"
        " --cacertdir <dir>  Directory where trusted CA certificates are stored.\n"
        "                    If neither cacert nor cacertdir are specified, the default\n"
        "                    system-wide trusted root certs configuration will apply.\n"
        " --insecure         Allow insecure TLS connection by skipping cert validation.\n"
        " --cert <file>      Client certificate to authenticate with.\n"
        " --key <file>       Private key file to authenticate with.\n"
        " --tls-ciphers <list> Sets the list of preferred ciphers (TLSv1.2 and below)\n"
        "                    in order of preference from highest to lowest separated by colon (\":\").\n"
        "                    See the ciphers(1ssl) manpage for more information about the syntax of this string.\n"
#ifdef TLS1_3_VERSION
        " --tls-ciphersuites <list> Sets the list of preferred ciphersuites (TLSv1.3)\n"
        "                    in order of preference from highest to lowest separated by colon (\":\").\n"
        "                    See the ciphers(1ssl) manpage for more information about the syntax of this string,\n"
        "                    and specifically for TLSv1.3 ciphersuites.\n"
#endif
#endif
        "";

    rdma_usage =
#ifdef USE_RDMA
        " --rdma             Establish a RDMA connection.\n"
#endif
        "";

    search_examples = 
        " Search index tests:\n"
        "   $ valkey-benchmark --search  --search-name grocery_products --vector-dim 768 "
        "--tag-field \"category\" --search-tags 'fruits:100,vegetables:100,dairy:100,meat:52.2,fruitsppo:99,fruitsppod:99' -t vec-insert -n 100 -r 1000\n"
        " Query and filter vector data:\n"
        "   $ valkey-benchmark --search  --search-name grocery_products     --vector-dim 768     --tag-field \"category\"\n"
         "--search-tags 'fruits:5.7,vegetables:0.3,dairy:10.1,meat:52.2,fruitsppo:99,fruitsppod:99' --tag-filter 'fruits*'\n"
         "   -t vec-query  --search-print-results   -n 1 -r 10000000\n\n"
        " Vector insert with tag and numeric fields:\n"
        "   $ valkey-benchmark --search --search-name products --vector-dim 768 \\\n"
        "       --tag-field category --numeric-field price \\\n"
        "       --search-tags 'electronics:40,clothing:30,food:30' \\\n"
        "       -t vec-insert -n 10000\n\n";

    search_usage_part1 = 
         " --search           Enable search indexes for vec-insert, vec-query, vec-del, and\n"
        "                    vec-scan-q-verify tests. Creates a vector index when starting benchmarks.\n"
        "                    Available vector tests:\n"
        "                    - vec-insert: Insert vectors into the index\n"
        "                    - vec-query: Query vectors using KNN search\n"
        "                    - vec-del: Delete vectors from the index\n"
        "                    - vec-scan-q-verify: Query with vectors and verify self-recall\n"
        " --search-print-results Print the search results returned by FT.SEARCH queries.\n"
        " --ef-search <value> Set the EF_RUNTIME parameter for KNN queries. (default 200)\n"
        " --vector-dim <dim> Set the dimension of the vector index. Dim must be > 16. (default 128)\n"
        " --ef-construction <value> Set the EF_CONSTRUCTION parameter for KNN queries. (default 200)\n"
        " --m <value>        Set the HNSW M parameter for KNN queries. (default 16)\n"
        " --search-alg <name> Set the search algorithm to use for KNN queries. (default 'hnsw')\n"
        "                    Supported algorithms: 'hnsw', 'flat'.\n"
        " --metric <name>    Set the metric for KNN queries. (default 'L2')\n"
        "                    Supported metrics: 'L2', 'IP', 'COSINE'\n"
        " --k <value>       Set the number of nearest neighbors to return in KNN queries. (default 10)\n"
        " --search-name <name> Set the name of the search index to use for vec-query and vec-del tests.\n"
        "                    If not set, the default index name 'test_vector_index' is used.\n"
        " --search-prefix <prefix>\n"
        "                    Set the prefix for vector keys. (default 'vec:')\n"
        " --vector-field <name>\n"
        "                    Set the name for vector values. (default 'vector_field')\n"
        " --tag-field <name> Set the tag field name for the index.\n"
        "                    For multiple tag fields (future): use comma-separated names.\n"
        " --numeric-field <name>\n"
        "                    Set the numeric field name for the index.\n"
        "                    For multiple numeric fields (future): use comma-separated names.\n"
        " --tag-filter <pattern>\n"
        "                    Set tag filter pattern for vec-query operations (e.g., 'category_*').\n"
        " --search-tags <distribution>\n"
        "                    Comma-separated tag:percentage pairs for vec-insert operations.\n"
        "                    Example: 'fruits:8.5,vegetables:7.2,dairy:32.1,meat:52.2'\n"
        " --dataset <name>   Use a precomputed dataset for vector operations.\n"
        "                    Dataset must be in binary format (.bin extension).\n"
        " --dataset-path <path> Specify the full path to the dataset file.\n"
        " --filtered          Enable metadata filtering for vector search (requires dataset with metadata).\n"
        "\n";
    
    search_usage_part2 = 
        "Optimizer Options:\n"
        " --optimize         Enable adaptive load optimization. Automatically adjusts\n"
        "                    benchmark parameters to achieve optimization goals.\n"
        " --optimize-objective <spec>\n"
        "                    Set optimization objective. Format: 'maximize:metric' or 'minimize:metric'\n"
        "                    Available metrics: qps, avg_latency, p50_latency, p90_latency,\n"
        "                    p95_latency, p99_latency, max_latency, recall_avg, recall_min, recall_max\n"
        "                    Example: 'maximize:qps' or 'minimize:p99_latency'\n"
        " --optimize-constraint <spec>\n"
        "                    Add optimization constraint. Format: 'metric:op:value'\n"
        "                    Operators: 'lt' (<) or 'gt' (>)\n"
        "                    Example: 'p99_latency:lt:5.0' or 'recall_avg:gt:0.90'\n"
        "                    Can be specified multiple times for multiple constraints.\n"
        " --optimize-csv <file>\n"
        "                    Output optimization results to CSV file.\n"
        " --optimize-max-iterations <num>\n"
        "                    Maximum number of optimization iterations (default 50).\n"
        " --optimize-min-requests <num>\n"
        "                    Minimum requests per benchmark run during optimization (default 1000).\n"
        " --optimize-client-range <min:max>\n"
        "                    Range for number of parallel clients (connections) to test.\n"
        "                    Example: '20:400' (default: '1:1500')\n"
        " --optimize-thread-range <min:max>\n"
        "                    Range for number of threads to test.\n"
        "                    Example: '4:10' (default: '0:16')\n"
        " --optimize-ef-search-range <min:max>\n"
        "                    Range for ef_search parameter in HNSW queries.\n"
        "                    Example: '50:300' (default: '20:500')\n"
        " --optimize-pipeline-range <min:max>\n"
        "                    Range for pipeline size (number of commands per batch).\n"
        "                    Example: '10:100' (default: '1:1000')\n"
        "\n"
        "Runtime Configuration Options:\n"
        " --runtime-config <file>\n"
        "                    Apply server-side configurations from file before benchmark.\n"
        "                    Configurations are applied to all cluster nodes.\n"
        " --restore-config   Restore original server configurations after benchmark completes.\n";
    printf(
        "%s%s%s%s%s%s%s%s%s%s", /* Split to avoid strings longer than 4095 (-Woverlength-strings). */
        "Usage: valkey-benchmark [OPTIONS] [--] [COMMAND ARGS...]\n\n"
        "Simulates sending commands using multiple clients. The utility provides a\n"
        "default set of tests. You can run a subset of the tests using the -t option or\n"
        "supply one or more custom commands on the command line.\n\n"
        "To supply multiple commands on the command line, separate them with ';' as in\n"
        "`SET foo bar ';' GET foo`. You can also prefix a command in the sequence with\n"
        "a number N to repeat the command N times. In command arguments, the following\n"
        "placeholders are substituted:\n\n"
        " __rand_int__       Replaced with a zero-padded random integer in the range\n"
        "                    selected using the -r option. Multiple occurrences within the\n"
        "                    command will have different values.\n"
        "__rand_1st__        Like __rand_int__ but multiple occurrences will have the same\n"
        "                    value. __rand_2nd__ through __rand_9th__ are also available.\n"
        " __data__           Replaced with data of the size specified by the -d option.\n"
        " {clt}              Replaced with a tag that routes the command to each node in\n"
        "                    a cluster. Include this in key names when running in cluster\n"
        "                    mode.\n"
        "\n",
        "Options:\n"
        "\n"
        " -h <hostname>      Server hostname (default 127.0.0.1)\n"
        " -p <port>          Server port (default 6379)\n"
        " -s <socket>        Server socket (overrides host and port)\n"
        " -a <password>      Password for Valkey Auth\n"
        " --user <username>  Used to send ACL style 'AUTH username pass'. Needs -a.\n"
        " -u <uri>           Server URI on format valkey://user:password@host:port/dbnum\n"
        "                    User, password and dbnum are optional. For authentication\n"
        "                    without a username, use username 'default'. For TLS, use\n"
        "                    the scheme 'valkeys'.\n"
        " -c <clients>       Number of parallel connections (default 50).\n"
        "                    Note: If --cluster is used then number of clients has to be\n"
        "                    the same or higher than the number of nodes.\n"
        " -n <requests>      Total number of requests (default 100000)\n"
        " -d <size>          Data size of SET/GET value in bytes (default 3)\n"
        " --dbnum <db>       SELECT the specified db number (default 0)\n"
        " -3                 Start session in RESP3 protocol mode.\n"
        " --threads <num>    Enable multi-thread mode.\n"
        " --cluster          Enable cluster mode.\n"
        "                    If the command is supplied on the command line in cluster\n"
        "                    mode, the key must contain \"{clt}\". Otherwise, the\n"
        "                    command will not be sent to the right cluster node.\n"
        " --rfr <mode>       Enable read from replicas in cluster mode.\n"
        "                    This command must be used with the --cluster option.\n"
        "                    There are three modes for reading from replicas:\n"
        "                    'no' - sends read requests to primaries only (default) \n"
        "                    'yes' - sends read requests to replicas only.\n"
        "                    'all' - sends read requests to all nodes.\n"
        "                    Since write commands will not be accepted by replicas,\n"
        "                    it is recommended to enable read from replicas only for read\n"
        "                    command tests.\n"
        " --enable-tracking  Send CLIENT TRACKING on before starting benchmark.\n"
        " -k <boolean>       1=keep alive 0=reconnect (default 1)\n"
        " -r <keyspacelen>   Use random keys for SET/GET/INCR, random values for SADD,\n"
        "                    random members and scores for ZADD.\n"
        "                    Using this option the benchmark will replace the string\n"
        "                    __rand_int__ inside an argument with a random 12 digit\n"
        "                    number in the specified range from 0 to keyspacelen-1. The\n"
        "                    substitution changes every time a command is executed.\n"
        "                    Default tests use this to hit random keys in the specified\n"
        "                    range.\n"
        "                    Note: If -r is omitted, all commands in a benchmark will\n"
        "                    use the same key.\n"
        " --sequential       Modifies the -r argument to replace the string __rand_int__\n"
        "                    with 12 digit numbers sequentially instead of randomly.\n"
        "                    __rand_1st__ through __rand_9th__ are available with independent\n"
        "                    counters. Used to create expected number of elements with multiple\n"
        "                    replacements.\n"
        "                    example: ZADD myzset __rand_int__ element:__rand_1st__\n"
        " -P <numreq>        Pipeline <numreq> requests. That is, send multiple requests\n"
        "                    before waiting for the replies. Default 1 (no pipeline).\n"
        "                    When multiple commands are specified on the command line,\n"
        "                    then the full command sequence counts as one and -P controls\n"
        "                    the number of times the command sequence is sent in each\n"
        "                    pipeline.\n",
        " -q                 Quiet. Just show query/sec values\n"
        " --precision        Number of decimal places to display in latency output (default 0)\n"
        " --csv              Output in CSV format\n"
        " -l                 Loop. Run the tests forever\n"
        " -t <tests>         Only run the comma separated list of tests. The test\n"
        "                    names are the same as the ones produced as output.\n"
        "                    The -t option is ignored if a specific command is supplied\n"
        "                    on the command line.\n"
        " -I                 Idle mode. Just open N idle connections and wait.\n"
        " -x                 Read last argument from STDIN.\n"
        " --rps <requests>   Limit the total number of requests per second. Default 0 (no limit)\n"
        " --balance-nodes    Enable fair load distribution across cluster nodes using quota-based balancing.\n"
        "                    Each node gets a quota of requests per cycle. When the slowest node\n"
        "                    completes its quota, all nodes get refreshed quota and continue.\n"
        " --balance-quota-step <num> Number of requests each node can process per cycle.\n"
        "                    Default 1000. Only used with --balance-nodes.\n"
        " --balance-tolerance <pct> Tolerance percentage for node imbalance (0-100).\n"
        "                    Default 10. Only used with --balance-nodes.\n"
        " --test-balance     Run node balancing algorithm test and exit.\n"
        "                    Simulates nodes with different latencies to verify balancing.\n"
        " --seed <num>       Set the seed for random number generator. Default seed is based on time.\n"
        " --num-functions <num>\n"
        "                    Sets the number of functions present in the Lua lib that is\n"
        "                    loaded when running the 'function_load' test. (default 10).\n"
        " --num-keys-in-fcall <num>\n"
        "                    Sets the number of keys passed to FCALL command when running\n"
        "                    the 'fcall' test. (default 1)\n",
        search_usage_part1,
        search_usage_part2,
        tls_usage,
        rdma_usage,        
        " --mptcp            Enable an MPTCP connection.\n"
        "\n"
        "Configuration Persistence:\n"
        " --save-config      Save the current configuration after successful run.\n"
        " --no-save-config   Skip saving configuration for this run.\n"
        " --clear-config     Clear saved configuration and exit.\n"
        " --show-config      Display saved configuration and exit.\n"
        "\n"
        "Network Baseline Measurement:\n"
        " --no-baseline      Disable baseline network latency measurement (enabled by default).\n"
        "                    By default, baseline network latency is measured automatically\n"
        "                    using 10,000 PING operations (single client/thread). Results are\n"
        "                    shown in latency reports and included in CSV output.\n"
        "\n"
        " --help             Output this help and exit.\n"
        " --version          Output version and exit.\n\n"
        "Examples:\n\n"
        " Run the benchmark with the default configuration against 127.0.0.1:6379:\n"
        "   $ valkey-benchmark\n\n"
        " Use 20 parallel clients, for a total of 100k requests, against 192.168.1.1:\n"
        "   $ valkey-benchmark -h 192.168.1.1 -p 6379 -n 100000 -c 20\n\n"
        " Fill 127.0.0.1:6379 with about 1 million keys only using the SET test:\n"
        "   $ valkey-benchmark -t set -n 1000000 -r 100000000\n\n"
        " Benchmark 127.0.0.1:6379 for a few commands producing CSV output:\n"
        "   $ valkey-benchmark -t ping,set,get -n 100000 --csv\n\n"
        " Benchmark a specific command line:\n"
        "   $ valkey-benchmark -r 10000 -n 10000 eval 'return server.call(\"ping\")' 0\n\n"
        " Fill a list with 10000 random elements:\n"
        "   $ valkey-benchmark -r 10000 -n 10000 lpush mylist __rand_int__\n\n"
        " Benchmark a specific transaction:\n"
        "   $ valkey-benchmark -- multi ';' set key:__rand_int__ __data__ ';' \\\n"
        "                         incr counter ';' exec\n\n",
        search_examples,
        " For more information, see the Valkey documentation at https://valkey.io.\n");
    exit(exit_status);
}

long long showThroughput(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    UNUSED(eventLoop);
    UNUSED(id);
    benchmarkThread *thread = (benchmarkThread *)clientData;
    int64_t liveclients = atomic_load_explicit(&config.liveclients, memory_order_relaxed);
    int64_t requests_finished = atomic_load_explicit(&config.requests_finished, memory_order_relaxed);
    int64_t previous_requests_finished = atomic_load_explicit(&config.previous_requests_finished, memory_order_relaxed);
    long long current_tick = mstime();

    if (liveclients == 0 && requests_finished != config.requests) {
        fprintf(stderr, "All clients disconnected... aborting.\n");
        assert(0);
    }
    if (config.num_threads && requests_finished >= config.requests) {
        aeStop(eventLoop);
        return AE_NOMORE;
    }
    if (config.csv) return SHOW_THROUGHPUT_INTERVAL;
    if (config.quiet) return SHOW_THROUGHPUT_INTERVAL;
    /* only first thread output throughput */
    if (thread != NULL && thread->index != 0) {
        return SHOW_THROUGHPUT_INTERVAL;
    }
    if (config.idlemode == 1) {
        printf("clients: %ld\r", config.liveclients);
        fflush(stdout);
        return SHOW_THROUGHPUT_INTERVAL;
    }
    const float dt = (float)(current_tick - config.start) / 1000.0;
    const float rps = (float)requests_finished / dt;
    const float instantaneous_dt = (float)(current_tick - config.previous_tick) / 1000.0;
    const float instantaneous_rps = (float)(requests_finished - previous_requests_finished) / instantaneous_dt;
    config.previous_tick = current_tick;
    atomic_store_explicit(&config.previous_requests_finished, requests_finished, memory_order_relaxed);
    printf("%*s\r", (int)config.last_printed_bytes, " "); /* ensure there is a clean line */
    int64_t printed_bytes =
        printf("%s: rps=%.1f (overall: %.1f) avg_msec=%.3f (overall: %.3f)\r", config.title, instantaneous_rps, rps,
               hdr_mean(config.current_sec_latency_histogram) / 1000.0f, hdr_mean(config.latency_histogram) / 1000.0f);
    config.last_printed_bytes = printed_bytes;
    hdr_reset(config.current_sec_latency_histogram);
    fflush(stdout);
    return SHOW_THROUGHPUT_INTERVAL;
}

char *generateFunctionScript(uint32_t num_functions, int64_t with_keys) {
    /* 64K buffer to hold script code */
    const size_t buffer_len = 64 * 1024;
    char *buffer = zcalloc(buffer_len);
    memset(buffer, 0, buffer_len);

    int64_t written = snprintf(buffer, buffer_len, "#!lua name=benchlib\n");
    while (num_functions > 0 && (buffer_len - written) > 0) {
        assert(buffer_len - written > 0);
        int64_t n = 0;
        if (with_keys) {
            n = snprintf(buffer + written, buffer_len - written,
                         "local function foo%u(keys, args)\nreturn keys[0]\nend\n",
                         num_functions);
        } else {
            n = snprintf(buffer + written, buffer_len - written,
                         "local function foo%u()\nreturn 0\nend\n",
                         num_functions);
        }

        if (n < 0 || (size_t)n >= buffer_len - written) {
            break;
        }
        written += n;

        n = snprintf(buffer + written, buffer_len - written,
                     "server.register_function('foo%u', foo%u)\n",
                     num_functions,
                     num_functions);
        written += n;

        num_functions--;
    }

    return buffer;
}

/* Return true if the named test was selected using the -t command line
 * switch, or if all the tests are selected (no -t passed by user). */
int test_is_selected(const char *name) {
    char buf[256];
    int64_t l = strlen(name);

    if (config.tests == NULL) return 1;
    buf[0] = ',';
    memcpy(buf + 1, name, l);
    buf[l + 1] = ',';
    buf[l + 2] = '\0';
    return strstr(config.tests, buf) != NULL;
}

/* Wrapper for zcalloc to match libvalkey's calloc signature */
static void *zcalloc_wrapper(size_t nmemb, size_t size) {
    /* Overflow check */
    if (nmemb != 0 && size > SIZE_MAX / nmemb) {
        return NULL;
    }
    return zcalloc(nmemb * size);
}

int main(int argc, char **argv) {
    int64_t i;
    char *data, *cmd, *tag;
    int64_t len;
    memset(&config, 0, sizeof(config));
    config.cluster_mode = -1; /* Unknown until detected */

    /* Configure libvalkey to use jemalloc allocators.
     * This ensures valkeyFormatCommand() and other libvalkey functions
     * allocate memory using the same allocator (jemalloc) that we use
     * for zfree(), preventing allocator mismatch crashes. */
    valkeyAllocFuncs jemalloc_fns = {
        .mallocFn = zmalloc,
        .callocFn = zcalloc_wrapper,
        .reallocFn = zrealloc,
        .strdupFn = zstrdup,
        .freeFn = zfree,
    };
    valkeySetAllocators(&jemalloc_fns);

    srandom(time(NULL) ^ getpid());
    init_genrand64(ustime() ^ getpid());
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    config.ct = VALKEY_CONN_TCP;
    config.numclients = 50;
    config.requests = 100000;
    config.liveclients = 0;
    config.el = aeCreateEventLoop(1024 * 10);
    aeCreateTimeEvent(config.el, 1000, showThroughput, NULL, NULL);
    config.keepalive = 1;
    config.datasize = 3;
    config.pipeline = 1;
    config.replace_placeholders = 1;
    config.keyspacelen = 0;
    config.sequential_replacement = 0;
    config.quiet = 0;
    config.csv = 0;
    config.loop = 0;
    config.idlemode = 0;
    config.clients = listCreate();
    config.paused_clients = listCreate();
    config.conn_info.hostip = sdsnew("127.0.0.1");
    config.conn_info.hostport = 6379;
    config.use_search = 0;
    config.clean = 0;
    config.print_search_results = 0;
    config.search_debug = 1;
    config.tests = NULL;
    config.conn_info.input_dbnum = 0;
    config.stdinarg = 0;
    config.conn_info.auth = NULL;
    config.precision = DEFAULT_LATENCY_PRECISION;
    config.num_threads = 0;
    config.threads = NULL;
    config.cluster_mode = 0;
    config.rps = 0;
    config.balance_nodes = 0;
    config.balance_quota_step = 1000;  /* Default: 1000 requests per cycle */
    config.balance_tolerance_pct = 10;  /* Default: 10% tolerance */
    config.node_request_counters = NULL;
    config.node_quota_remaining = NULL;
    config.read_from_replica = FROM_PRIMARY_ONLY;
    config.cluster_node_count = 0;
    config.cluster_nodes = NULL;
    config.server_config = NULL;
    config.is_fetching_slots = 0;
    config.is_updating_slots = 0;
    config.slots_last_update = 0;
    config.enable_tracking = 0;
    config.num_functions = 10;
    config.num_keys_in_fcall = 1;
    config.resp3 = 0;
    resetPlaceholders();
    setDefaultSearchConfig();
    
    /* Initialize optimizer defaults */
    config.optimize_enabled = 0;
    config.optimizer = NULL;
    config.optimize_objective = NULL;
    config.optimize_constraints = NULL;
    config.num_optimize_constraints = 0;
    config.optimize_csv_file = NULL;
    config.optimize_max_iterations = 50;
    config.optimize_min_requests = 1000;

    /* Initialize config persistence */
    config_persist_init();
    config.save_config = 0;
    config.no_save_config = 0;

    /* Load saved configuration if exists */
    persisted_config_t saved_config;
    memset(&saved_config, 0, sizeof(saved_config));
    if (config_persist_load(&saved_config) == 0) {
        /* Apply saved configuration as defaults */
        if (saved_config.num_clients > 0) config.numclients = saved_config.num_clients;
        if (saved_config.num_threads > 0) config.num_threads = saved_config.num_threads;
        if (saved_config.pipeline > 0) config.pipeline = saved_config.pipeline;
        if (saved_config.requests > 0) config.requests = saved_config.requests;
        if (saved_config.keyspacelen > 0) config.keyspacelen = saved_config.keyspacelen;
        if (saved_config.dbnum > 0) config.conn_info.input_dbnum = saved_config.dbnum;
        if (saved_config.csv) config.csv = saved_config.csv;
        if (saved_config.loop) config.loop = saved_config.loop;
        if (saved_config.idlemode) config.idlemode = saved_config.idlemode;
        if (saved_config.keepalive > 0) config.keepalive = saved_config.keepalive;
        if (saved_config.precision > 0) config.precision = saved_config.precision;
        if (saved_config.resp3) config.resp3 = saved_config.resp3;

        /* Apply search parameters */
        if (saved_config.dataset) config.dataset_name = sdsnew(saved_config.dataset);
        if (saved_config.search_name) config.search.name = sdsnew(saved_config.search_name);
        if (saved_config.search_algorithm) config.search.algorithm = sdsnew(saved_config.search_algorithm);
        if (saved_config.search_prefix) config.search.prefix = sdsnew(saved_config.search_prefix);
        if (saved_config.vector_field) config.search.vector_field = sdsnew(saved_config.vector_field);
        if (saved_config.vector_dim > 0) config.search.vector_dim = saved_config.vector_dim;
        if (saved_config.tag_field) config.search.tag_field = sdsnew(saved_config.tag_field);
        if (saved_config.numeric_field) config.search.numeric_field = sdsnew(saved_config.numeric_field);
        if (saved_config.ef_search > 0) config.search.ef_search = saved_config.ef_search;
        if (saved_config.ef_construction > 0) config.search.ef_construction = saved_config.ef_construction;
        if (saved_config.m > 0) config.search.m = saved_config.m;
        if (saved_config.k > 0) config.search.k = saved_config.k;
        if (saved_config.metric) config.search.metric = sdsnew(saved_config.metric);
        // if (saved_config.nocontent) config.search.nocontent = saved_config.nocontent;
        if (saved_config.localonly) config.search.localonly = saved_config.localonly;
        if (saved_config.use_filtered_search) config.use_filtered_search = saved_config.use_filtered_search;

        /* Apply optimizer parameters */
        if (saved_config.optimize_objective) config.optimize_objective = sdsnew(saved_config.optimize_objective);
        if (saved_config.optimize_csv_file) config.optimize_csv_file = sdsnew(saved_config.optimize_csv_file);
        if (saved_config.optimize_max_iterations > 0) config.optimize_max_iterations = saved_config.optimize_max_iterations;
        if (saved_config.optimize_min_requests > 0) config.optimize_min_requests = saved_config.optimize_min_requests;

        /* Apply auth parameters */
        if (saved_config.auth) config.conn_info.auth = sdsnew(saved_config.auth);
        if (saved_config.user) config.conn_info.user = sdsnew(saved_config.user);

        /* Apply TLS parameters */
#ifdef USE_OPENSSL
        if (saved_config.tls_cert) config.sslconfig.cert = strdup(saved_config.tls_cert);
        if (saved_config.tls_key) config.sslconfig.key = strdup(saved_config.tls_key);
        if (saved_config.tls_cacert) config.sslconfig.cacert = strdup(saved_config.tls_cacert);
        if (saved_config.tls_cacertdir) config.sslconfig.cacertdir = strdup(saved_config.tls_cacertdir);
        if (saved_config.tls_skip_verify) config.sslconfig.skip_cert_verify = saved_config.tls_skip_verify;
        if (saved_config.sni) config.sslconfig.sni = strdup(saved_config.sni);
#endif
    }

    i = parseOptions(argc, argv);
    argc -= i;
    argv += i;

    /* Save configuration after successful parsing unless --no-save-config */
    if (!config.no_save_config && config.tests != NULL) {
        persisted_config_t config_to_save;
        memset(&config_to_save, 0, sizeof(config_to_save));

        /* Basic parameters */
        config_to_save.num_clients = config.numclients;
        config_to_save.num_threads = config.num_threads;
        config_to_save.pipeline = config.pipeline;
        config_to_save.requests = config.requests;
        config_to_save.keyspacelen = config.keyspacelen;
        config_to_save.dbnum = config.conn_info.input_dbnum;
        config_to_save.csv = config.csv;
        config_to_save.loop = config.loop;
        config_to_save.idlemode = config.idlemode;
        config_to_save.keepalive = config.keepalive;
        config_to_save.precision = config.precision;
        config_to_save.resp3 = config.resp3;

        /* Search parameters */
        if (config.dataset_name) config_to_save.dataset = config.dataset_name;
        if (config.search.name) config_to_save.search_name = config.search.name;
        if (config.search.algorithm) config_to_save.search_algorithm = config.search.algorithm;
        if (config.search.prefix) config_to_save.search_prefix = config.search.prefix;
        if (config.search.vector_field) config_to_save.vector_field = config.search.vector_field;
        config_to_save.vector_dim = config.search.vector_dim;
        if (config.search.tag_field) config_to_save.tag_field = config.search.tag_field;
        if (config.search.numeric_field) config_to_save.numeric_field = config.search.numeric_field;
        config_to_save.ef_search = config.search.ef_search;
        config_to_save.ef_construction = config.search.ef_construction;
        config_to_save.m = config.search.m;
        config_to_save.k = config.search.k;
        if (config.search.metric) config_to_save.metric = config.search.metric;
        config_to_save.nocontent = config.search.nocontent;
        config_to_save.localonly = config.search.localonly;
        config_to_save.use_filtered_search = config.use_filtered_search;

        /* Optimizer parameters */
        if (config.optimize_objective) config_to_save.optimize_objective = config.optimize_objective;
        if (config.optimize_csv_file) config_to_save.optimize_csv_file = config.optimize_csv_file;
        config_to_save.optimize_max_iterations = config.optimize_max_iterations;
        config_to_save.optimize_min_requests = config.optimize_min_requests;

        /* Auth parameters */
        if (config.conn_info.auth) config_to_save.auth = config.conn_info.auth;
        if (config.conn_info.user) config_to_save.user = config.conn_info.user;

        /* TLS parameters */
#ifdef USE_OPENSSL
        if (config.sslconfig.cert) config_to_save.tls_cert = config.sslconfig.cert;
        if (config.sslconfig.key) config_to_save.tls_key = config.sslconfig.key;
        if (config.sslconfig.cacert) config_to_save.tls_cacert = config.sslconfig.cacert;
        if (config.sslconfig.cacertdir) config_to_save.tls_cacertdir = config.sslconfig.cacertdir;
        config_to_save.tls_skip_verify = config.sslconfig.skip_cert_verify;
        if (config.sslconfig.sni) config_to_save.sni = config.sslconfig.sni;
#endif

        config_persist_save(&config_to_save);
    }

    /* Clean up the saved config */
    config_persist_free(&saved_config);

    tag = "";

#ifdef USE_OPENSSL
    if (config.tls) {
        cliSecureInit();
    }
#endif

    /* Initialize base vector */
    initBaseVector(config.search.vector_dim);
    
    if (config.mptcp && (config.ct != VALKEY_CONN_TCP)) {
        fprintf(stderr, "Options --mptcp is only supported by TCP\n");
        assert(0);
    }
    config.engine_type = getEngineType(config.conn_info.hostip, config.conn_info.hostport, config.ct);
    valkeyContext *ctx = getValkeyContext(config.ct, config.conn_info.hostip, config.conn_info.hostport);
    /* Detect cluster mode (CME vs CMD) */
    config.cluster_mode = isClusterModeEnabled(ctx) > 0; /* Unknown by default */
    valkeyFree(ctx);
    if (config.cluster_mode) {
        // We only include the slot placeholder {clt} if cluster mode is enabled
        tag = "{clt}";
        /* Fetch cluster configuration. */
        if (!fetchClusterConfiguration() || !config.cluster_nodes) {
            if (config.ct != VALKEY_CONN_UNIX) {
                fprintf(stderr,
                        "Failed to fetch cluster configuration from "
                        "%s:%d\n",
                        config.conn_info.hostip, config.conn_info.hostport);
            } else {
                fprintf(stderr,
                        "Failed to fetch cluster configuration from "
                        "%s\n",
                        config.conn_info.hostip);
            }
            assert(0);
        }
        if (config.selected_node_count == 0) {
            fprintf(stderr, "Invalid cluster: %ld node(s).\n", config.selected_node_count);
            assert(0);
        }       
    } else if (isElastiCacheEndpoint(config.conn_info.hostip)) {
        int64_t res = setupElastiCacheCMDNodes();
        if (!res) {
            fprintf(stderr,
                    "Failed to fetch cluster configuration from "
                    "%s:%d\n",
                    config.conn_info.hostip, config.conn_info.hostport);
            assert(0);
        }
    } else {
        int64_t res = fetchCMDNodesConfiguration();
        if (!res) {
            if (config.ct != VALKEY_CONN_UNIX) {
                fprintf(stderr,
                        "Failed to fetch cluster configuration from "
                        "%s:%d\n",
                        config.conn_info.hostip, config.conn_info.hostport);
            } else {
                fprintf(stderr,
                        "Failed to fetch cluster configuration from "
                        "%s\n",
                        config.conn_info.hostip);
            }
            assert(0);
        }
    }
    const char *node_roles = NULL;
    if (config.read_from_replica == FROM_ALL) {
        node_roles = "cluster";
    } else if (config.read_from_replica == FROM_REPLICA_ONLY) {
        node_roles = "replica";
    } else {
        node_roles = "primary";
    }
    printf("Cluster has %ld %s selected nodes:\n\n", config.selected_node_count, node_roles);
    i = 0;
    for (; i < config.selected_node_count; i++) {
        clusterNode *node = config.selected_nodes[i];
        if (!node) {
            fprintf(stderr, "Invalid cluster node #%ld\n", i);
            assert(0);
        }
        const char *node_type = (node->replicate == NULL ? "Primary" : "Replica");
        printf("Node %ld(%s): ", i, node_type);
        if (node->name) printf("%s ", node->name);
        printf("%s:%d\n", node->ip, node->port);
        safeGetServerConfig(config.ct, node->ip, node->port, node->server_config);
        if (node->server_config == NULL) {
            fprintf(stderr, "WARNING: Could not fetch node CONFIG %s:%d\n", node->ip, node->port);
        }
    }
    printf("\n");
    /* Automatically set thread number to node count if not specified
        * by the user. */
    if (config.num_threads == 0) config.num_threads = config.selected_node_count;
    if (config.num_threads > 0) {
        pthread_mutex_init(&(config.liveclients_mutex), NULL);
        pthread_mutex_init(&(config.is_updating_slots_mutex), NULL);
    }

    if (config.keepalive == 0) {
        fprintf(stderr, "WARNING: Keepalive disabled. You probably need "
                        "'echo 1 > /proc/sys/net/ipv4/tcp_tw_reuse' for Linux and "
                        "'sudo sysctl -w net.inet.tcp.msl=1000' for Mac OS X in order "
                        "to use a lot of clients/requests\n");
    }
    if (argc > 0 && config.tests != NULL) {
        fprintf(stderr, "WARNING: Option -t is ignored.\n");
    }

    if (config.idlemode) {
        printf("Creating %ld idle connections and waiting forever (Ctrl+C when done)\n", config.numclients);
        int64_t use_threads = (config.num_threads > 0);
        if (use_threads) {
            initBenchmarkThreads();
        }
        createMissingClients("", 0, 1);
        if (use_threads)
            startBenchmarkThreads();
        else
            aeMain(config.el);
        /* and will wait for every */
    }
    
    /* Run benchmark with command in the remainder of the arguments. */
    if (argc) {
        sds title = sdsnew(argv[0]);
        for (i = 1; i < argc; i++) {
            title = sdscatlen(title, " ", 1);
            title = sdscatlen(title, (char *)argv[i], strlen(argv[i]));
        }
        sds *sds_args = getSdsArrayFromArgv(argc, argv, 0);
        if (!sds_args) {
            fprintf(stderr, "Invalid quoted string\n");
            return 1;
        }
        if (config.stdinarg) {
            sds_args = sds_realloc(sds_args, (argc + 1) * sizeof(sds));
            sds_args[argc] = readArgFromStdin();
            argc++;
        }
        /* Setup argument length */
        size_t *argvlen = zcalloc(argc * sizeof(size_t));
        for (i = 0; i < argc; i++) argvlen[i] = sdslen(sds_args[i]);
        /* RESP-encode the command(s) given on the syntax
         *
         *     [N] command args [ ";" [N] command args [...] ]
         */
        int64_t start = 0;   /* Argument index where the current command starts. */
        int64_t repeat = 1;  /* Number of times to repeat the current command. */
        int64_t seq_len = 0; /* Total number of commands in the sequence. */
        sds cmd_seq = sdsempty();
        for (i = 0; i <= argc; i++) {
            if (i == start && sds_args[i][0] >= '1' && sds_args[i][0] <= '9') {
                /* Command prefixed by number means repeat command N times. */
                repeat = atoi(sds_args[i]);
                start++;
            } else if (i == argc || strcmp(";", sds_args[i]) == 0) {
                cmd = NULL;
                if (i == start) continue;
                /* End of command. RESP-encode and append to sequence. */
                len = valkeyFormatCommandArgv(&cmd, i - start,
                                              (const char **)sds_args + start,
                                              argvlen + start);
                for (int64_t j = 0; j < repeat; j++) {
                    cmd_seq = sdscatlen(cmd_seq, cmd, len);
                }
                seq_len += repeat;
                zfree(cmd);
                start = i + 1;
                repeat = 1;
            } else if (strstr(sds_args[i], "__data__")) {
                /* Replace data placeholders with data of length given by -d. */
                int num_parts;
                sds *parts = sdssplitlen(sds_args[i], sdslen(sds_args[i]),
                                         "__data__", strlen("__data__"),
                                         &num_parts);
                sds newarg = parts[0];
                parts[0] = NULL; /* prevent it from being freed below */
                for (int64_t j = 1; j < num_parts; j++) {
                    char data[config.datasize];
                    genBenchmarkRandomData(data, config.datasize);
                    newarg = sdscatlen(newarg, data, config.datasize);
                    newarg = sdscatlen(newarg, parts[j], sdslen(parts[j]));
                }
                sdsfreesplitres(parts, num_parts);
                sdsfree(sds_args[i]);
                sds_args[i] = newarg;
                argvlen[i] = sdslen(sds_args[i]);
            }
        }
        len = sdslen(cmd_seq);
        /* adjust the datasize to the parsed command */
        config.datasize = len;
        do {
            benchmarkSequence(title, cmd_seq, len, seq_len);
        } while (config.loop);
        sdsfree(cmd_seq);
        sdsfreesplitres(sds_args, argc);

        sdsfree(title);
        if (config.server_config != NULL) freeServerConfig(config.server_config);
        zfree(argvlen);
        return 0;
    }
    if (config.use_search) {
        /* Initialize dataset if enabled */
        if (config.use_dataset) {

            dataset_info_t info;
            config.dataset_ctx = dataset_init(config.dataset_name, &info);

            if (!config.dataset_ctx) {
                fprintf(stderr, "Failed to initialize dataset: %s\n", config.dataset_name);
                exit(1);
            }

            /* Store metadata */
            config.dataset_num_vectors = info.num_vectors;
            config.dataset_num_queries = info.num_queries;
            config.dataset_num_neighbors = info.num_neighbors;
            config.search.vector_dim = info.dim;
            config.search.k = info.num_neighbors < config.search.k ? info.num_neighbors : config.search.k;
            // verify datatype is FLOAT32
            if (strcmp(info.dtype, "FLOAT32") != 0) {
                fprintf(stderr, "ERROR: Unsupported dataset datatype: %s (only FLOAT32 supported)\n", info.dtype);
                exit(1);
            }
            // verify distance metric is supported
            if (strcmp(info.distance_metric, "L2") != 0 &&
                strcmp(info.distance_metric, "IP") != 0 &&
                strcmp(info.distance_metric, "COSINE") != 0) {
                fprintf(stderr, "ERROR: Unsupported dataset distance metric: %s (supported: L2, IP, COSINE)\n", info.distance_metric);
                exit(1);
            }
            /* Store distance metric */
            config.search.metric = sdsnewlen(info.distance_metric, strlen(info.distance_metric));



            /* Initialize recall tracking */
            initRecallStats();

            /* Initialize cluster tag mapping */
            initClusterTagMap(&cluster_tag_map, info.num_vectors * 2); /* initial capacity */


            /* Override vector dimension from dataset */
            if (config.search.vector_dim != (int64_t)info.dim) {
                fprintf(stderr, "WARNING: Overriding --vector-dim %ld with dataset dim %u\n",
                        config.search.vector_dim, info.dim);
                config.search.vector_dim = info.dim;
            }

            /* Initialize counters */
            atomic_store(&config.dataset_prefill_counter, 0);
            atomic_store(&config.dataset_query_counter, 0);

            printf("✓ Dataset loaded: %lu vectors, %lu queries, %u dims, %u neighbors\n",
                info.num_vectors, info.num_queries, info.dim, info.num_neighbors);
        }


        if (config.cluster_mode && config.cluster_primary_nodes && config.cluster_primary_node_count > 0) {
            valkeyContext *ctx = config.cluster_primary_nodes[0]->ctx;
            if (!ctx) {
                ctx = getValkeyContext(config.ct, 
                                      config.cluster_primary_nodes[0]->ip, 
                                      config.cluster_primary_nodes[0]->port);
            }
        } 
        
        /* Print cluster mode information */
        const char *cluster_mode_str = "Unknown";
        if (config.cluster_mode == 1) {
            cluster_mode_str = "CME (Cluster Mode Enabled)";
        } else if (config.cluster_mode == 0) {
            cluster_mode_str = "CMD (Cluster Mode Disabled)";
        }
        // createSearchHsetTemplate(&cmd);
        printf("Using search indexes for the benchmark. %s - %s\n", 
               config.engine_type == ENGINE_TYPE_MEMORYDB ? "MemoryDB" : config.engine_type == ENGINE_TYPE_ELASTICACHE_VALKEY ? "EC Valkey" : "OSS",
               cluster_mode_str);

        createDefaultSearchIndexes();
        sleep(2); /* wait a bit before checking index status */
        waitForIndexBackfillComplete(config.engine_type, config.selected_node_count, config.selected_nodes, config.ct, (const char**)&config.search.name, 1);

        long long search_memory = 0;
        long long search_reclaimable = 0;
        long long search_total_docs = 0;
        long long search_ingest_field_vector = 0;
        long long search_background_indexing_status = 0;
        last_search_info = getSearchInfo(config.selected_node_count, config.selected_nodes, config.ct,
                                        &search_memory, &search_reclaimable, &search_total_docs,
                                        &search_ingest_field_vector, &search_background_indexing_status);
        last_ftinfo = getFtInfoStatistics(config.search.name, config.selected_node_count, config.selected_nodes, config.ct);
        last_info_all = getInfoCluster(config.selected_node_count, config.selected_nodes, config.ct);
        if (config.use_dataset) {
            /* Build vector ID mappings by scanning cluster for pre-existing vectors */
            /* Note: New vectors inserted during benchmark will update the mapping in real-time */
            printf("Building vector ID to cluster tag mappings from existing cluster data...\n");
            int64_t scan_result = buildVectorIdMappings(config.cluster_mode,
                                                config.search.prefix,
                                                config.selected_nodes,
                                                config.selected_node_count,
                                                &cluster_tag_map, vectorKeyProcessor);
            if (scan_result != 0) {
                fprintf(stderr, "WARNING: Failed to build vector ID mappings, validation may be limited\n");
            } else {
                printf("Initial mapping built. New insertions will update mapping in real-time.\n");
            }        
        }
    }
    
    /* Apply runtime configuration if specified */
    if (config.runtime_config_file) {
        config.runtime_config_ctx = loadRuntimeConfig(config.runtime_config_file);
        if (config.runtime_config_ctx) {
            int64_t applied = applyRuntimeConfig(config.runtime_config_ctx,
                                            config.cluster_node_count,
                                            config.cluster_nodes,
                                            config.ct,
                                            !config.quiet);
            if (applied > 0 && !config.quiet) {
                printf("Successfully applied %ld runtime configuration settings\n", applied);
            } else if (applied < 0) {
                fprintf(stderr, "Warning: Failed to apply runtime configuration\n");
            }
        }
    }
    if (config.csv) {
        if (!config.no_baseline) {
            printf("\"test\",\"rps\",\"avg_latency_ms\",\"min_latency_ms\",\"p50_latency_ms\",\"p95_latency_ms\",\"p99_"
                   "latency_ms\",\"max_latency_ms\",\"baseline_avg_ms\",\"baseline_p50_ms\",\"baseline_p95_ms\",\"baseline_p99_ms\"\n");
        } else {
            printf("\"test\",\"rps\",\"avg_latency_ms\",\"min_latency_ms\",\"p50_latency_ms\",\"p95_latency_ms\",\"p99_"
                   "latency_ms\",\"max_latency_ms\"\n");
        }
    }

    /* Initialize optimizer if enabled */
    if (config.optimize_enabled) {
        if (!config.optimize_objective) {
            fprintf(stderr, "Error: --optimize requires --optimize-objective\n");
            fprintf(stderr, "Example: --optimize-objective 'maximize:qps'\n");
            exit(1);
        }
        
        config.optimizer = optimizer_create();
        if (!config.optimizer) {
            fprintf(stderr, "Failed to create optimizer\n");
            exit(1);
        }
        
        /* Add tunable parameters with domain-aware grouping
         * MIXED group: ef_search (affects recall AND latency/throughput tradeoff)
         * THROUGHPUT group: clients, threads, pipeline (affect QPS/latency, minimal recall impact)
         */
        
        /* Parse custom ranges or use defaults */
        int64_t client_min, client_max, thread_min, thread_max;
        int64_t ef_search_min, ef_search_max, pipeline_min, pipeline_max;
        
        parseOptimizeRange(config.optimize_client_range, &client_min, &client_max, 1, 1500);
        parseOptimizeRange(config.optimize_thread_range, &thread_min, &thread_max, 0, 16);
        parseOptimizeRange(config.optimize_ef_search_range, &ef_search_min, &ef_search_max, 20, 500);
        parseOptimizeRange(config.optimize_pipeline_range, &pipeline_min, &pipeline_max, 1, 1000);
        
        optimizer_add_param_grouped(config.optimizer, "clients", client_min, client_max, 5, config.numclients, PARAM_GROUP_THROUGHPUT);
        optimizer_add_param_grouped(config.optimizer, "threads", thread_min, thread_max, 1, config.num_threads, PARAM_GROUP_THROUGHPUT);
        // optimizer_add_param_grouped(config.optimizer, "pipeline", pipeline_min, pipeline_max, 1, config.pipeline, PARAM_GROUP_THROUGHPUT);
        optimizer_add_param_grouped(config.optimizer, "ef_search", ef_search_min, ef_search_max, 1, config.search.ef_search, PARAM_GROUP_MIXED);
        
        /* Add RPS as constraint-only parameter if specified */
        if (config.rps > 0) {
            optimizer_add_constraint_param(config.optimizer, "rps", config.rps);
            optimizer_add_constraint(config.optimizer, METRIC_QPS, CONSTRAINT_LESS_THAN, (double)config.rps);
        }
        
        /* Parse and add user-specified constraints */
        for (int64_t j = 0; j < config.num_optimize_constraints; j++) {
            parseOptimizerConstraint(config.optimizer, config.optimize_constraints[j]);
        }
        
        /* Set optimization objective */
        parseOptimizerObjective(config.optimizer, config.optimize_objective);
        
        /* Open CSV output file if specified */
        FILE *csv_file = NULL;
        if (config.optimize_csv_file) {
            csv_file = fopen(config.optimize_csv_file, "w");
            if (csv_file) {
                optimizer_print_csv_header(csv_file);
            } else {
                fprintf(stderr, "Warning: Could not open CSV file: %s\n", config.optimize_csv_file);
            }
        }
        
        printf("\n=== Starting Adaptive Load Optimization ===\n");
        printf("Objective: %s\n", config.optimize_objective);
        printf("Constraints: %ld specified\n", config.num_optimize_constraints);
        printf("Max iterations: %ld\n", config.optimize_max_iterations);
        printf("Min requests per run: %ld\n", config.optimize_min_requests);
        printf("User requested requests: %ld\n\n", config.requests);
        
        /* Save original config.requests to restore after optimization */
        int64_t original_requests = config.requests;
        
        status_t opt_status = STATUS_OK;
        int64_t iteration = 0;
        char *cmd_opt;
        int64_t len_opt;
        /* Run the benchmark (only vec-query for now) */
        assert(config.use_search && test_is_selected("vec-query"));
        size_t keyspacelen_before = config.keyspacelen;
        if (config.use_dataset) {
            config.keyspacelen = (int64_t)config.dataset_num_queries;
        }
        len_opt = createSearchCmdTemplate(&cmd_opt);
        
        config.keyspacelen = keyspacelen_before;
        // config.optimize_max_iterations = 10;
        /* Optimization loop */
        while (opt_status != STATUS_CONVERGED && iteration < config.optimize_max_iterations) {
            iteration++;
            
            /* Get current configuration from optimizer */
            int64_t opt_config[3];  /* clients, threads, ef_search (pipeline commented out) */
            optimizer_get_current_config(config.optimizer, opt_config, 3);
            
            /* Apply configuration */
            config.numclients = opt_config[0];     /* clients */
            config.num_threads = opt_config[1];    /* threads */
            // config.pipeline = opt_config[2];    /* pipeline (not yet enabled) */
            config.search.ef_search = opt_config[2];  /* ef_search */
            
            // /* Temporarily override requests with optimize_min_requests for faster iterations */
            // config.requests = config.optimize_min_requests;
            
            printf("\n--- Iteration %ld ---\n", iteration);
            printf("Config: clients=%ld threads=%ld pipeline=%ld ef_search=%ld requests=%ld\n",
                   config.numclients, config.num_threads, config.pipeline, config.search.ef_search, config.requests);
            
            
            benchmark("VEC-QUERY (optimizing)", cmd_opt, len_opt);
            
            /* Collect metrics */
            double metrics[METRIC_COUNT];
            collectOptimizerMetrics(metrics);
            
            /* Display current metrics */
            printf("QPS: %.1f | P99: %.3fms | Recall: %.4f\n",
                   metrics[METRIC_QPS], metrics[METRIC_P99_LATENCY], metrics[METRIC_RECALL_AVG]);
            
            /* Feed to optimizer */
            opt_status = optimizer_step(config.optimizer, metrics);
            
            /* Write to CSV if enabled */
            if (csv_file) {
                optimizer_print_csv_row(config.optimizer, csv_file);
                fflush(csv_file);
            }
            
            if (opt_status == STATUS_WAIT_STABILIZATION) {
                printf("Status: Waiting for stabilization...\n");
            } else if (opt_status == STATUS_NO_FEASIBLE) {
                printf("Status: No feasible solution found!\n");
                break;
            }
        }
        
        /* Restore original requests value */
        config.requests = original_requests;
        zfree(cmd_opt);
        /* Print final results */
        printf("\n=== Optimization Complete ===\n");
        optimizer_print_status(config.optimizer, stdout);
        
        const measurement_t *best = optimizer_get_best_solution(config.optimizer);
        if (best) {
            printf("\n=== Best Configuration Found ===\n");
            printf("QPS: %.1f\n", best->metrics[METRIC_QPS]);
            printf("Avg Latency: %.3fms\n", best->metrics[METRIC_AVG_LATENCY]);
            printf("P99 Latency: %.3fms\n", best->metrics[METRIC_P99_LATENCY]);
            printf("Recall Avg: %.4f\n", best->metrics[METRIC_RECALL_AVG]);
            printf("\nParameters:\n");
            for (int64_t j = 0; j < optimizer_get_param_count(config.optimizer); j++) {
                printf("  %s = %ld\n", optimizer_get_param_name(config.optimizer, j), 
                       best->param_values[j]);
            }
            
            /* Apply best configuration for final run if user requested full benchmark */
            if (original_requests > config.optimize_min_requests) {
                printf("\n=== Running Final Benchmark with Best Config ===\n");
                printf("Using %ld requests (user requested value)\n\n", original_requests);
                
                /* Apply best configuration */
                config.numclients = best->param_values[0];
                config.num_threads = best->param_values[1];
                // config.pipeline = best->param_values[2];
                config.search.ef_search = best->param_values[2];
                
                
                /* Run final benchmark with full request count */
                if (config.use_search && test_is_selected("vec-query")) {
                    size_t keyspacelen_before = config.keyspacelen;
                    if (config.use_dataset) {
                        config.keyspacelen = (int64_t)config.dataset_num_queries;
                    }
                    char *cmd_final;
                    int64_t len_final = createSearchCmdTemplate(&cmd_final);
                    benchmark("VEC-QUERY (final)", cmd_final, len_final);
                    zfree(cmd_final);
                    config.keyspacelen = keyspacelen_before;
                }
                
                /* Print final benchmark results */
                double final_metrics[METRIC_COUNT];
                collectOptimizerMetrics(final_metrics);
                printf("\n=== Final Benchmark Results ===\n");
                printf("QPS: %.1f\n", final_metrics[METRIC_QPS]);
                printf("Avg Latency: %.3fms\n", final_metrics[METRIC_AVG_LATENCY]);
                printf("P99 Latency: %.3fms\n", final_metrics[METRIC_P99_LATENCY]);
                printf("Recall Avg: %.4f\n", final_metrics[METRIC_RECALL_AVG]);
            }
        }
        
        if (csv_file) fclose(csv_file);
        optimizer_destroy(config.optimizer);
        
        /* Exit after optimization - don't run normal benchmarks */
        return 0;
    }
    
    /* Measure baseline network latency for non-optimizer runs (enabled by default unless --no-baseline) */
    if (!config.no_baseline && !config.baseline_measured) {
        measureBaselineLatency();
    }
    
    /* Run default benchmark suite. */
    data = zcalloc(config.datasize + 1);
    do {
        genBenchmarkRandomData(data, config.datasize);
        data[config.datasize] = '\0';

        if (test_is_selected("ping_inline") || test_is_selected("ping")) benchmark("PING_INLINE", "PING\r\n", 6);

        if (test_is_selected("ping_mbulk") || test_is_selected("ping")) {
            len = valkeyFormatCommand(&cmd, "PING");
            benchmark("PING_MBULK", cmd, len);
            zfree(cmd);
        }

        if (test_is_selected("set")) {
            len = valkeyFormatCommand(&cmd, "SET key%s:__rand_int__ %s", tag, data);
            benchmark("SET", cmd, len);
            zfree(cmd);
        }

        if (test_is_selected("get")) {
            len = valkeyFormatCommand(&cmd, "GET key%s:__rand_int__", tag);
            benchmark("GET", cmd, len);
            zfree(cmd);
        }

        if (test_is_selected("incr")) {
            len = valkeyFormatCommand(&cmd, "INCR counter%s:__rand_int__", tag);
            benchmark("INCR", cmd, len);
            zfree(cmd);
        }

        if (test_is_selected("lpush")) {
            len = valkeyFormatCommand(&cmd, "LPUSH mylist%s %s", tag, data);
            benchmark("LPUSH", cmd, len);
            zfree(cmd);
        }

        if (test_is_selected("rpush")) {
            len = valkeyFormatCommand(&cmd, "RPUSH mylist%s %s", tag, data);
            benchmark("RPUSH", cmd, len);
            zfree(cmd);
        }

        if (test_is_selected("lpop")) {
            len = valkeyFormatCommand(&cmd, "LPOP mylist%s", tag);
            benchmark("LPOP", cmd, len);
            zfree(cmd);
        }

        if (test_is_selected("rpop")) {
            len = valkeyFormatCommand(&cmd, "RPOP mylist%s", tag);
            benchmark("RPOP", cmd, len);
            zfree(cmd);
        }

        if (test_is_selected("sadd")) {
            len = valkeyFormatCommand(&cmd, "SADD myset%s element:__rand_int__", tag);
            benchmark("SADD", cmd, len);
            zfree(cmd);
        }

        if (test_is_selected("hset")) {
            len = valkeyFormatCommand(&cmd, "HSET myhash%s element:__rand_int__ %s", tag, data);
            benchmark("HSET", cmd, len);
            zfree(cmd);
        }
        if (config.use_search) {
            if (test_is_selected("vec-load")) {
                size_t prev_num_requests = config.requests;
                size_t prev_keyspacelen_before = config.keyspacelen;
                int64_t prev_sequential_replacement = config.sequential_replacement; /* force sequential keys for ground truth ingestion */
                config.requests = config.dataset_num_vectors;
                config.sequential_replacement = 1;

                if (config.use_dataset/* && config.cluster_mode*/) {
                    config.requests = config.dataset_num_vectors - getClusterTagMapCount(&cluster_tag_map);
                    config.keyspacelen = (int64_t)config.dataset_num_vectors;
                } 

                /* Ingest ground truth vectors from reserved range */
                len = createSearchHsetTemplate(&cmd);
                benchmark("VEC-LOAD", cmd, len);
                zfree(cmd);
                /* wait for index ingestion to complete*/
                // sds flat_index = sdsnew(config.search.name);
                // flat_index = sdscat(flat_index, "_flat");               
                // const char* index_names[2] = {config.search.name, flat_index};
                sleep(2); /* wait a bit before checking index status */
                waitForIndexBackfillComplete(config.engine_type, config.selected_node_count, config.selected_nodes, config.ct, (const char**)&config.search.name, 1);
                /* Index ingestion is done */
                config.sequential_replacement = prev_sequential_replacement; /* restore original setting */
                config.requests = prev_num_requests; /* restore original request count */
                config.keyspacelen = prev_keyspacelen_before;
            }
            
            if (test_is_selected("vec-insert")) {
                /* Use custom vector benchmark function */
                len = createSearchHsetTemplate(&cmd);
                benchmark("VEC-INSERT", cmd, len);
                zfree(cmd);
            }

            if (test_is_selected("vec-query")) {
                size_t keyspacelen_before = config.keyspacelen;
                if (config.use_dataset) {
                    // For dataset mode, the ground truth size is the number of vectors in the dataset
                    config.keyspacelen = (int64_t)config.dataset_num_queries;
                }
                /* Use custom vector benchmark function */
                len = createSearchCmdTemplate(&cmd);
                benchmark("VEC-QUERY", cmd, len);
                zfree(cmd);
                config.keyspacelen = keyspacelen_before; /* restore original keyspacelen */
            }

            if (test_is_selected("vec-del")) {
                sds key = getSearchKeyTemplate();
                len = valkeyFormatCommand(&cmd, "DEL %s", key);
                benchmark("VEC-DEL", cmd, len);
                sdsfree(key);
                zfree(cmd);
            }

            if (test_is_selected("vec-scan-q-verify")) {
                /* This test retrieves a vector from a known key and searches for it.
                 * We create a command sequence:
                 * 1. HGET <key> <vector_field> - Get the vector
                 * 2. FT.SEARCH <index> "*=>[KNN 10 @vector_field $QUERY_VECTOR]" ... - Search
                 * 
                 * For benchmarking, we use a simplified approach where we generate
                 * the query vector ourselves and verify self-search works.
                 * This is similar to vec-query but we ensure the queried vector exists. */
                
                /* Build a command that will query for vectors and verify they find themselves */
                sds key = getSearchKeyTemplate();
                
                /* Build a simple FT.SEARCH command (same as vec-query for now) */
                len = createSearchCmdTemplate(&cmd);
                
                /* TODO: Implement full scan-verify logic with command sequence:
                 * - First command: HGET to get vector
                 * - Second command: FT.SEARCH with that vector
                 * - Verify the original key appears in results
                 * This requires multi-command pipeline support. */
                
                benchmark("VEC-SCAN-Q-VERIFY", cmd, len);
                sdsfree(key);
                zfree(cmd);
            }
        }
        if (test_is_selected("spop")) {
            len = valkeyFormatCommand(&cmd, "SPOP myset%s", tag);
            benchmark("SPOP", cmd, len);
            zfree(cmd);
        }

        if (test_is_selected("zadd")) {
            char *score = "0";
            if (config.replace_placeholders) score = "__rand_int__";
            len = valkeyFormatCommand(&cmd, "ZADD myzset%s %s element:__rand_1st__", tag, score);
            benchmark("ZADD", cmd, len);
            zfree(cmd);
        }

        if (test_is_selected("zpopmin")) {
            len = valkeyFormatCommand(&cmd, "ZPOPMIN myzset%s", tag);
            benchmark("ZPOPMIN", cmd, len);
            zfree(cmd);
        }

        if (test_is_selected("lrange") || test_is_selected("lrange_100") || test_is_selected("lrange_300") ||
            test_is_selected("lrange_500") || test_is_selected("lrange_600")) {
            len = valkeyFormatCommand(&cmd, "LPUSH mylist%s %s", tag, data);
            benchmark("LPUSH (needed to benchmark LRANGE)", cmd, len);
            zfree(cmd);
        }

        if (test_is_selected("lrange") || test_is_selected("lrange_100")) {
            len = valkeyFormatCommand(&cmd, "LRANGE mylist%s 0 99", tag);
            benchmark("LRANGE_100 (first 100 elements)", cmd, len);
            zfree(cmd);
        }

        if (test_is_selected("lrange") || test_is_selected("lrange_300")) {
            len = valkeyFormatCommand(&cmd, "LRANGE mylist%s 0 299", tag);
            benchmark("LRANGE_300 (first 300 elements)", cmd, len);
            zfree(cmd);
        }

        if (test_is_selected("lrange") || test_is_selected("lrange_500")) {
            len = valkeyFormatCommand(&cmd, "LRANGE mylist%s 0 499", tag);
            benchmark("LRANGE_500 (first 500 elements)", cmd, len);
            zfree(cmd);
        }

        if (test_is_selected("lrange") || test_is_selected("lrange_600")) {
            len = valkeyFormatCommand(&cmd, "LRANGE mylist%s 0 599", tag);
            benchmark("LRANGE_600 (first 600 elements)", cmd, len);
            zfree(cmd);
        }

        if (test_is_selected("mset")) {
            const char *cmd_argv[21];
            cmd_argv[0] = "MSET";
            sds key_placeholder = sdscatprintf(sdsnew(""), "key%s:__rand_int__", tag);
            for (i = 1; i < 21; i += 2) {
                cmd_argv[i] = key_placeholder;
                cmd_argv[i + 1] = data;
            }
            len = valkeyFormatCommandArgv(&cmd, 21, cmd_argv, NULL);
            benchmark("MSET (10 keys)", cmd, len);
            zfree(cmd);
            sdsfree(key_placeholder);
        }

        if (test_is_selected("mget")) {
            const char *cmd_argv[11];
            cmd_argv[0] = "MGET";
            sds key_placeholder = sdscatprintf(sdsnew(""), "key%s:__rand_int__", tag);
            for (i = 1; i < 11; i++) {
                cmd_argv[i] = key_placeholder;
            }
            len = valkeyFormatCommandArgv(&cmd, 11, cmd_argv, NULL);
            benchmark("MGET (10 keys)", cmd, len);
            zfree(cmd);
            sdsfree(key_placeholder);
        }

        if (test_is_selected("xadd")) {
            len = valkeyFormatCommand(&cmd, "XADD mystream%s * myfield %s", tag, data);
            benchmark("XADD", cmd, len);
            zfree(cmd);
        }

        if (test_is_selected("function_load")) {
            char *script = generateFunctionScript(config.num_functions, 0);
            len = valkeyFormatCommand(&cmd, "function load replace %s", script);
            benchmark("FUNCTION LOAD", cmd, len);
            zfree(script);
            zfree(cmd);
        }

        if (test_is_selected("fcall")) {
            char *script = generateFunctionScript(1, config.num_keys_in_fcall > 0);

            valkeyContext* ctx = config.conn_ctx;
            if (ctx == NULL) {
                fprintf(stderr, "No existing connection context, creating new\n");
                ctx = getValkeyContext(config.ct, config.conn_info.hostip, config.conn_info.hostport);
                if (ctx == NULL) {
                    assert(0);
                }
            }

            assert(ctx != NULL && ctx->err == 0);
            void *reply = valkeyCommand(ctx, "FUNCTION LOAD REPLACE %s", script);

            assert(reply != NULL);
            freeReplyObject(reply);
            zfree(script);

            char **cmd_argv = zcalloc(sizeof(char *) * (config.num_keys_in_fcall + 3));
            int64_t ret = asprintf(&(cmd_argv[0]), "fcall");
            UNUSED(ret);
            ret = asprintf(&(cmd_argv[1]), "foo1");
            UNUSED(ret);
            ret = asprintf(&(cmd_argv[2]), "%ld", config.num_keys_in_fcall);
            UNUSED(ret);
            for (int64_t i = 0; i < config.num_keys_in_fcall; i++) {
                ret = asprintf(&(cmd_argv[3 + i]), "key%ld", i + 1);
                UNUSED(ret);
            }
            len = valkeyFormatCommandArgv(&cmd, config.num_keys_in_fcall + 3, (const char **)cmd_argv, NULL);
            for (int64_t i = 0; i < config.num_keys_in_fcall + 3; i++) {
                zfree(cmd_argv[i]);
            }
            zfree(cmd_argv);

            benchmark("FCALL", cmd, len);
            zfree(cmd);
        }

        if (!config.csv) printf("\n");
    } while (config.loop);

    zfree(data);
    freeCliConnInfo(config.conn_info);
    if (config.server_config != NULL) freeServerConfig(config.server_config);
    if (base_vector != NULL) zfree(base_vector);
    if (config.tests != NULL) sdsfree(config.tests);
    if (config.input_dbnumstr != NULL) sdsfree(config.input_dbnumstr);
    resetPlaceholders();
    
    /* Restore runtime configuration if requested */
    if (config.restore_runtime_config && config.runtime_config_ctx) {
        if (!config.quiet) {
            printf("\nRestoring original server configuration...\n");
        }
        int64_t restored = restoreRuntimeConfig(config.runtime_config_ctx,
                                           config.cluster_node_count,
                                           config.cluster_nodes,
                                           config.ct,
                                           !config.quiet);
        if (restored > 0 && !config.quiet) {
            printf("Successfully restored %ld configuration settings\n", restored);
        }
    }
    
    /* Free runtime configuration context */
    if (config.runtime_config_ctx) {
        freeRuntimeConfig(config.runtime_config_ctx);
        config.runtime_config_ctx = NULL;
    }
    
    /* Print dataset recall statistics if dataset mode was used */
    printDatasetRecallStats();

    /* Cleanup dataset context */
    if (config.dataset_ctx) {
        dataset_destroy((dataset_ctx_t*)config.dataset_ctx);
        config.dataset_ctx = NULL;
    }

    /* Cleanup cluster tag mapping if it was initialized */
    if (config.use_dataset) {
        cleanupClusterTagMap(&cluster_tag_map);
    }

    /* Cleanup snapshot info */
    if (last_search_info) {
        freeClusterSnapshot(last_search_info);
        last_search_info = NULL;
    }
    if (last_ftinfo) {
        freeClusterSnapshot(last_ftinfo);
        last_ftinfo = NULL;
    }
    if (last_info_all) {
        freeClusterSnapshot(last_info_all);
        last_info_all = NULL;
    }

    /* Cleanup cluster nodes */
    if (config.cluster_nodes) {
        freeClusterNodes();
    }
    
    /* Cleanup selected nodes array (nodes themselves are freed above) */
    if (config.selected_nodes) {
        zfree(config.selected_nodes);
        config.selected_nodes = NULL;
    }

    /* Cleanup lists and event loop */
    if (config.clients) {
        listRelease(config.clients);
        config.clients = NULL;
    }
    if (config.paused_clients) {
        listRelease(config.paused_clients);
        config.paused_clients = NULL;
    }
    if (config.el) {
        aeDeleteEventLoop(config.el);
        config.el = NULL;
    }

    /* Cleanup SSL config */
#ifdef USE_OPENSSL
    if (config.sslconfig.sni) free(config.sslconfig.sni);
    if (config.sslconfig.cacert) free(config.sslconfig.cacert);
    if (config.sslconfig.cacertdir) free(config.sslconfig.cacertdir);
    if (config.sslconfig.cert) free(config.sslconfig.cert);
    if (config.sslconfig.key) free(config.sslconfig.key);
    if (config.sslconfig.ciphers) free(config.sslconfig.ciphers);
#ifdef TLS1_3_VERSION
    if (config.sslconfig.ciphersuites) free(config.sslconfig.ciphersuites);
#endif
#endif

    return 0;
}
