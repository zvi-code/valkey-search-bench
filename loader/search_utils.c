/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2024-present, Zvi Schneider
 * 
 * Valkey Benchmark Utilities
 *
 * Utility functions for the Valkey vector search benchmark tool.
 *
 * This file is part of valkey-search-benchmark and is licensed under the
 * BSD 3-Clause License. See the LICENSE file in the root directory.
 */

#include "search_utils.h"
#include "utils.h"
#include <ctype.h>
#include <valkey/valkey.h>
#include <math.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include "zmalloc.h"
#include "util.h"
#include <time.h>
#include <assert.h>
#include <unistd.h>


/* Forward declaration - getValkeyContext is defined in valkey-benchmark.c */
valkeyContext *getValkeyContext(enum valkeyConnectionType ct, const char *ip_or_path, int port);

static int search_debug = 1;
#define UNUSED(V) ((void)V)

/* Exact field name matcher */
static int exact_field_matcher(const char *line, const char *prefix) {
    if (!line || !prefix) return 0;
    
    /* Find the colon separator */
    const char *colon = strchr(line, ':');
    if (!colon) return 0;
    
    size_t key_len = colon - line;
    size_t prefix_len = strlen(prefix);
    
    return (key_len == prefix_len && strncmp(line, prefix, prefix_len) == 0);
}

/* Prefix matcher for fields with common prefixes */
static int prefix_field_matcher(const char *line, const char *prefix) {
    if (!line || !prefix) return 0;
    return strncmp(line, prefix, strlen(prefix)) == 0;
}

static long long parse_generic(const char *value, ParseConfig parse_config) {
    if (!value) return 0;
    if (parse_config.strategy == PARSE_CMDSTATS) {
        /* Example: cmdstat_get:calls=100,usec=2000,usec_per_call=20.00,rejected=0,failed=0 */
        const char *p = strstr(value, parse_config.key);
        if (!p) return 0;
        p += strlen(parse_config.key);
        if (*p == '=') p++;
        return (long long)atoll(p); 
    }
    /* Common: skip whitespace */
    while (*value == ' ' || *value == '\t') value++;
    
    /* Common: handle quoted values */
    if (*value == '"') {
        value++;
        char *end_quote = strchr(value, '"');
        if (end_quote) {
            char buf[64];
            size_t len = end_quote - value;
            if (len < sizeof(buf)) {
                memcpy(buf, value, len);
                buf[len] = '\0';
                value = buf;
            }
        }
    }
    
    switch (parse_config.strategy) {
        case PARSE_INTEGER:
            return atoll(value);
            
        case PARSE_MEMORY: {
            char *end;
            long long val = strtoll(value, &end, 10);
            if (end && *end) {
                switch (toupper(*end)) {
                    case 'K': return val * 1024;
                    case 'M': return val * 1024 * 1024;
                    case 'G': return val * 1024 * 1024 * 1024;
                }
            }
            return val;
        }
        
        case PARSE_FLOAT_FIXED:
            return (long long)(atof(value) * 1000.0);
            
        case PARSE_PERCENTILE: {
            const char *p = strstr(value, parse_config.key);
            if (!p) return 0;
            p += strlen(parse_config.key);
            if (*p == '=') p++;
            return (long long)(atof(p) * 1000);
        }
        default:
            return 0;
    }
    
    return 0;
}


typedef union {
    long long simple_value;
    struct { long long sum; int count; } avg_state;
    struct { long long min_val; long long max_val; int initialized; } minmax_state;
} AggregateState;

static void aggregate_generic(void **opaque, long long value, int node_idx, 
                              int is_last_node, AggregationType type) {
    UNUSED(node_idx);
    
    if (!*opaque) {
        *opaque = zcalloc(sizeof(AggregateState));
    }
    
    AggregateState *state = (AggregateState *)*opaque;
    
    switch (type) {
        case AGG_SUM:
            state->simple_value += value;
            break;
            
        case AGG_AVERAGE:
            state->avg_state.sum += value;
            state->avg_state.count++;
            if (is_last_node && state->avg_state.count > 0) {
                long long avg = state->avg_state.sum / state->avg_state.count;
                state->avg_state.sum = avg;
                state->avg_state.count = 1;
            }
            break;
            
        case AGG_MAX:
            if (value > state->simple_value) {
                state->simple_value = value;
            }
            break;
            
        case AGG_MINMAX:
            if (!state->minmax_state.initialized) {
                state->minmax_state.min_val = value;
                state->minmax_state.max_val = value;
                state->minmax_state.initialized = 1;
            } else {
                if (value < state->minmax_state.min_val) state->minmax_state.min_val = value;
                if (value > state->minmax_state.max_val) state->minmax_state.max_val = value;
            }
            break;
    }
    
    UNUSED(is_last_node);
}

/* Format large number with M/G suffix and comma separators */
static void format_large_number(char *buf, size_t buf_size, long long value) {
    if (value >= 1000000000LL) {
        /* Billions */
        double val = (double)value / 1000000000.0;
        if (val >= 100.0) {
            snprintf(buf, buf_size, "%.0fG", val);
        } else if (val >= 10.0) {
            snprintf(buf, buf_size, "%.1fG", val);
        } else {
            snprintf(buf, buf_size, "%.2fG", val);
        }
    } else if (value >= 1000000LL) {
        /* Millions */
        double val = (double)value / 1000000.0;
        if (val >= 100.0) {
            snprintf(buf, buf_size, "%.0fM", val);
        } else if (val >= 10.0) {
            snprintf(buf, buf_size, "%.1fM", val);
        } else {
            snprintf(buf, buf_size, "%.2fM", val);
        }
    } else if (value >= 10000LL) {
        /* Thousands with comma separator */
        if (value >= 1000000LL) {
            snprintf(buf, buf_size, "%lld,%03lld,%03lld", 
                    value / 1000000, (value / 1000) % 1000, value % 1000);
        } else {
            snprintf(buf, buf_size, "%lld,%03lld", value / 1000, value % 1000);
        }
    } else {
        /* Small numbers - no formatting needed */
        snprintf(buf, buf_size, "%lld", value);
    }
}

/* Format rate with appropriate precision */
static void format_rate(char *buf, size_t buf_size, double rate) {
    if (rate >= 1000000.0) {
        /* Millions */
        double val = rate / 1000000.0;
        if (val >= 100.0) {
            snprintf(buf, buf_size, "%.0fM", val);
        } else if (val >= 10.0) {
            snprintf(buf, buf_size, "%.1fM", val);
        } else {
            snprintf(buf, buf_size, "%.2fM", val);
        }
    } else if (rate >= 10000.0) {
        /* Thousands */
        double val = rate / 1000.0;
        if (val >= 100.0) {
            snprintf(buf, buf_size, "%.0fK", val);
        } else if (val >= 10.0) {
            snprintf(buf, buf_size, "%.1fK", val);
        } else {
            snprintf(buf, buf_size, "%.2fK", val);
        }
    } else if (rate >= 10.0) {
        snprintf(buf, buf_size, "%.0f", rate);
    } else if (rate >= 1.0) {
        snprintf(buf, buf_size, "%.1f", rate);
    } else if (rate > 0.0) {
        snprintf(buf, buf_size, "%.2f", rate);
    } else {
        snprintf(buf, buf_size, "0");
    }
}



/* Fix buffer size for minmax display */
static void display_formatted(const char *field_name, void *opaque, 
                              int node_count, DisplayFormat format) {
    UNUSED(node_count);
    if (!opaque) return;
    
    char buf[256];  /* Increased from 64 to 256 for minmax range */
    
    switch (format) {
        case DISPLAY_INTEGER: {
            long long value = *(long long *)opaque;
            format_large_number(buf, sizeof(buf), value);
            break;
        }
        
        case DISPLAY_MEMORY_MB: {
            long long bytes = *(long long *)opaque;
            double mb = (double)bytes / (1024.0 * 1024.0);
            if (mb == 0.0) {
                snprintf(buf, sizeof(buf), "0 MB");
            } else if (mb >= 1000.0) {
                double gb = mb / 1024.0;
                snprintf(buf, sizeof(buf), gb >= 10.0 ? "%.0f GB" : "%.2f GB", gb);
            } else if (mb >= 10.0) {
                snprintf(buf, sizeof(buf), "%.0f MB", mb);
            } else {
                snprintf(buf, sizeof(buf), mb >= 1.0 ? "%.1f MB" : "%.2f MB", mb);
            }
            break;
        }
        
        case DISPLAY_MEMORY_HUMAN: {
            long long bytes = *(long long *)opaque;
            const char *units[] = {"B", "KB", "MB", "GB", "TB"};
            int unit_idx = 0;
            double size = (double)bytes;
            
            while (size >= 1024.0 && unit_idx < 4) {
                size /= 1024.0;
                unit_idx++;
            }
            
            if (size == 0.0) {
                snprintf(buf, sizeof(buf), "0 %s", units[unit_idx]);
            } else if (size >= 100.0) {
                snprintf(buf, sizeof(buf), "%.0f %s", size, units[unit_idx]);
            } else {
                snprintf(buf, sizeof(buf), size >= 10.0 ? "%.1f %s" : "%.2f %s", 
                        size, units[unit_idx]);
            }
            break;
        }
        
        case DISPLAY_PERCENTAGE: {
            long long fixed_val = *(long long *)opaque;
            double pct = (double)fixed_val / 1000.0;
            if (pct == 0.0) {
                snprintf(buf, sizeof(buf), "0%%");
            } else if (fabs(pct) >= 10.0) {
                snprintf(buf, sizeof(buf), "%.0f%%", pct);
            } else {
                snprintf(buf, sizeof(buf), fabs(pct) >= 1.0 ? "%.1f%%" : "%.3f%%", pct);
            }
            break;
        }
        
        case DISPLAY_FLOAT: {
            long long fixed_val = *(long long *)opaque;
            double val = (double)fixed_val / 1000.0;
            if (val == 0.0) {
                snprintf(buf, sizeof(buf), "0");
            } else if (fabs(val) >= 1000.0) {
                format_rate(buf, sizeof(buf), val);
            } else if (fabs(val) >= 100.0) {
                snprintf(buf, sizeof(buf), "%.0f", val);
            } else {
                snprintf(buf, sizeof(buf), fabs(val) >= 10.0 ? "%.1f" : "%.3f", val);
            }
            break;
        }
        
        case DISPLAY_LATENCY_USEC: {
            long long usec = *(long long *)opaque;
            double ms = (double)usec / 1000.0;
            if (ms == 0.0) {
                snprintf(buf, sizeof(buf), "0 us");
            } else if (ms >= 10.0) {
                snprintf(buf, sizeof(buf), "%.0f us", ms);
            } else {
                snprintf(buf, sizeof(buf), ms >= 1.0 ? "%.1f us" : "%.2f us", ms);
            }
            break;
        }
        
        case DISPLAY_MINMAX: {
            AggregateState *state = (AggregateState *)opaque;
            if (state->minmax_state.initialized) {
                char min_buf[64], max_buf[64];
                format_large_number(min_buf, sizeof(min_buf), state->minmax_state.min_val);
                format_large_number(max_buf, sizeof(max_buf), state->minmax_state.max_val);
                
                if (state->minmax_state.min_val == state->minmax_state.max_val) {
                    snprintf(buf, sizeof(buf), "%s", min_buf);
                } else {
                    /* Larger buffer prevents truncation warning */
                    snprintf(buf, sizeof(buf), "%s - %s", min_buf, max_buf);
                }
            } else {
                buf[0] = '\0';
            }
            break;
        }
    }
    
    if (field_name && strlen(field_name) > 0) {
        printf("%s: %s", field_name, buf);
    } else {
        printf("%s", buf);
    }
}


static void diff_generic(const char *field_name, fieldSnapshot *old, 
                        fieldSnapshot *new_snap, long long time_delta_ms, 
                        int node_idx, DiffType type) {
    UNUSED(field_name);
    
    if (!old || !new_snap || !old->valid || !new_snap->valid || time_delta_ms <= 0) {
        return;
    }
    
    double interval_sec = (double)time_delta_ms / 1000.0;
    
    if (node_idx < 0) {
        /* Cluster-wide calculation */
        long long delta = new_snap->value - old->value;
        
        switch (type) {
            case DIFF_RATE_COUNT: {
                double rate = (double)delta / interval_sec;
                char rate_str[64], delta_str[64];
                format_rate(rate_str, sizeof(rate_str), rate);
                format_large_number(delta_str, sizeof(delta_str), delta);
                printf("%s/sec (Δ %s)", rate_str, delta_str);
                break;
            }
            
            case DIFF_RATE_MICROSEC: {
                /* Convert microseconds to seconds: divide by 1,000,000 */
                double rate = ((double)delta / 1000000.0) / interval_sec;
                char rate_str[64], delta_str[64];
                format_rate(rate_str, sizeof(rate_str), rate);
                format_large_number(delta_str, sizeof(delta_str), delta);
                printf("%s/sec (Δ %s)", rate_str, delta_str);
                break;
            }
            
            case DIFF_MEMORY_GROWTH: {
                double mb_per_sec = (double)(delta / (1024 * 1024)) / interval_sec;
                long long delta_mb = delta / (1024 * 1024);
                char rate_str[64], delta_str[64];
                format_rate(rate_str, sizeof(rate_str), mb_per_sec);
                format_large_number(delta_str, sizeof(delta_str), delta_mb);
                printf("%s MB/sec (Δ %s MB)", rate_str, delta_str);
                break;
            }
            
            case DIFF_PERCENTAGE_CHANGE: {
                if (old->value == 0) {
                    printf("N/A (initial value was 0)");
                } else {
                    double pct_change = ((double)delta / (double)old->value) * 100.0;
                    char old_str[64], new_str[64];
                    format_large_number(old_str, sizeof(old_str), old->value);
                    format_large_number(new_str, sizeof(new_str), new_snap->value);
                    
                    if (pct_change == 0.0) {
                        printf("0%% change (from %s to %s)", old_str, new_str);
                    } else if (fabs(pct_change) >= 10.0) {
                        printf("%.0f%% change (from %s to %s)", pct_change, old_str, new_str);
                    } else {
                        printf(fabs(pct_change) >= 1.0 ? "%.1f%% change (from %s to %s)" : 
                               "%.2f%% change (from %s to %s)", pct_change, old_str, new_str);
                    }
                }
                break;
            }
            case DIFF_NONE:
                /* No diff calculation */
                break;
        }
    } else {
        /* Per-node calculation */
        if (old->per_node_values && new_snap->per_node_values &&
            node_idx < old->node_count && node_idx < new_snap->node_count) {
            
            long long node_delta = new_snap->per_node_values[node_idx] - 
                                   old->per_node_values[node_idx];
            
            switch (type) {
                case DIFF_RATE_COUNT: {
                    double node_rate = (double)node_delta / interval_sec;
                    char rate_str[64], delta_str[64];
                    format_rate(rate_str, sizeof(rate_str), node_rate);
                    format_large_number(delta_str, sizeof(delta_str), node_delta);
                    printf("%s/sec (Δ %s)", rate_str, delta_str);
                    break;
                }
                
                case DIFF_RATE_MICROSEC: {
                    double node_rate = ((double)node_delta / 1000000.0) / interval_sec;
                    char rate_str[64], delta_str[64];
                    format_rate(rate_str, sizeof(rate_str), node_rate);
                    format_large_number(delta_str, sizeof(delta_str), node_delta);
                    printf("%s/sec (Δ %s)", rate_str, delta_str);
                    break;
                }
                
                case DIFF_MEMORY_GROWTH: {
                    double node_mb_per_sec = (double)(node_delta / (1024 * 1024)) / interval_sec;
                    long long node_delta_mb = node_delta / (1024 * 1024);
                    char rate_str[64], delta_str[64];
                    format_rate(rate_str, sizeof(rate_str), node_mb_per_sec);
                    format_large_number(delta_str, sizeof(delta_str), node_delta_mb);
                    printf("%s MB/sec (Δ %s MB)", rate_str, delta_str);
                    break;
                }
                
                case DIFF_PERCENTAGE_CHANGE:
                case DIFF_NONE: 
                    /* Not typically used for per-node */
                    break;
            }
        }
    }
}

typedef struct {
    int metric_width;
    int aggregate_width;
    int node_width;
    int num_nodes;
    sds *node_headers;
} TableLayout;

typedef struct {
    const char *title;
    const char *command;
    int num_fields;
    long long timestamp_ms;
} TableHeader;

/* Box drawing characters as string constants */
static const char *BOX_TOP_LEFT = "╔";
static const char *BOX_TOP_RIGHT = "╗";
static const char *BOX_BOTTOM_LEFT = "╚";
static const char *BOX_BOTTOM_RIGHT = "╝";
static const char *BOX_VERTICAL_LEFT = "╠";
static const char *BOX_VERTICAL_RIGHT = "╣";
static const char *BOX_HORIZONTAL = "═";
static const char *BOX_VERTICAL = "║";
static const char *BOX_HORIZONTAL_THIN = "─";


static int shouldSkipZeroRow(fieldSnapshot *field, infoFieldType *field_type, 
                             int num_nodes, int is_debug) {
    if (!is_debug && !field->valid) return 1;
    
    /* Check aggregate value */
    int all_zero = (field->value == 0);
    
    /* Check per-node values if tracked */
    if (all_zero && field_type->track_per_node && field->per_node_values) {
        for (int i = 0; i < num_nodes; i++) {
            if (field->per_node_values[i] != 0) {
                all_zero = 0;
                break;
            }
        }
    }
    
    return all_zero;
}

static int shouldSkipZeroDeltaRow(fieldSnapshot *old_field, fieldSnapshot *new_field,
                                  infoFieldType *field_type, int num_nodes, 
                                  int is_debug, int is_memory_diff) {
    if (!is_debug && (!old_field->valid || !new_field->valid)) return 1;
    
    long long delta = new_field->value - old_field->value;
    
    /* For memory diffs, check MB-level changes */
    int all_zero;
    if (is_memory_diff) {
        long long delta_mb = delta / (1024 * 1024);
        all_zero = (delta_mb == 0);
    } else {
        all_zero = (delta == 0);
    }
    
    /* Check per-node deltas */
    if (all_zero && field_type->track_per_node && 
        old_field->per_node_values && new_field->per_node_values) {
        for (int i = 0; i < num_nodes; i++) {
            long long node_delta = new_field->per_node_values[i] - 
                                   old_field->per_node_values[i];
            
            int node_zero;
            if (is_memory_diff) {
                long long node_delta_mb = node_delta / (1024 * 1024);
                node_zero = (node_delta_mb == 0);
            } else {
                node_zero = (node_delta == 0);
            }
            
            if (!node_zero) {
                all_zero = 0;
                break;
            }
        }
    }
    
    return all_zero;
}

static TableLayout calculateTableLayout(clusterSnapshot *snapshot, int num_fields, 
                                        infoFieldType *fields, int is_debug) {
    TableLayout layout = {0};
    layout.num_nodes = snapshot->num_nodes;
    layout.aggregate_width = 26;  /* Fixed width for aggregate values */
    layout.node_width = 26;       /* Fixed width for per-node values */
    layout.metric_width = 30;     /* Minimum width */
    
    /* Calculate maximum field name width (only for rows that will be printed) */
    for (int i = 0; i < num_fields; i++) {
        if (shouldSkipZeroRow(&snapshot->fields[i], &fields[i], 
                             snapshot->num_nodes, is_debug)) {
            continue;
        }
        
        int len;
        if (!fields[i].parse_config.key) {
            len = strlen(fields[i].prefix_match);
        } else {
            len = strlen(fields[i].prefix_match) + 1 + strlen(fields[i].parse_config.key);
        }
        
        if (len > layout.metric_width) {
            layout.metric_width = len;
        }
    }
    
    layout.metric_width += 2;  /* Add padding */
    
    /* Prepare node headers (truncated to fit column width) */
    layout.node_headers = zcalloc(snapshot->num_nodes * sizeof(sds));
    for (int i = 0; i < snapshot->num_nodes; i++) {
        char truncated[20];
        snprintf(truncated, sizeof(truncated), "%.17s", snapshot->node_identifiers[i]);
        layout.node_headers[i] = sdsnew(truncated);
    }
    
    return layout;
}

static void freeTableLayout(TableLayout *layout) {
    if (layout->node_headers) {
        for (int i = 0; i < layout->num_nodes; i++) {
            sdsfree(layout->node_headers[i]);
        }
        zfree(layout->node_headers);
    }
}

static int calculateTotalWidth(TableLayout *layout, int has_per_node_data) {
    int width = 2 + layout->metric_width + 2;           /* "  METRIC  " */
    width += layout->aggregate_width + 2;               /* "  AGGREGATE" */
    if (has_per_node_data) {
        width += layout->num_nodes * (layout->node_width + 2);  /* "  NODE" per node */
    }
    return width;
}

static void printTableBorder(int width, const char *left, const char *middle, const char *right) {
    printf("%s", left);
    for (int i = 0; i < width; i++) printf("%s", middle);
    printf("%s\n", right);
}

static void printCenteredTitle(const char *title, int total_width) {
    int title_len = strlen(title);
    int left_padding = (total_width - title_len) / 2;
    int right_padding = total_width - title_len - left_padding;
    
    printf("%s", BOX_VERTICAL);
    for (int i = 0; i < left_padding; i++) printf(" ");
    printf("%s", title);
    for (int i = 0; i < right_padding; i++) printf(" ");
    printf("%s\n", BOX_VERTICAL);
}

static void printTableHeader(TableHeader header, int total_width) {
    printTableBorder(total_width, BOX_TOP_LEFT, BOX_HORIZONTAL, BOX_TOP_RIGHT);
    printCenteredTitle(header.title, total_width);
    printTableBorder(total_width, BOX_VERTICAL_LEFT, BOX_HORIZONTAL, BOX_VERTICAL_RIGHT);
    
    /* Format timestamp */
    time_t now = header.timestamp_ms / 1000;
    struct tm *tm_info = localtime(&now);
    char time_buffer[26];
    strftime(time_buffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);
    
    printf("%s Time: %s | Command: %-30s | Fields: %-4d", 
           BOX_VERTICAL, time_buffer, header.command, header.num_fields);
    int info_len = 7 + 19 + 11 + 30 + 10 + 4;
    for (int i = info_len; i < total_width; i++) printf(" ");
    printf("%s\n", BOX_VERTICAL);
    
    printTableBorder(total_width, BOX_BOTTOM_LEFT, BOX_HORIZONTAL, BOX_BOTTOM_RIGHT);
}

static void printSnapshotColumnHeaders(TableLayout *layout, int has_per_node_data) {
    printf("\n");
    printf("  %-*s  %*s", layout->metric_width, "METRIC", 
           layout->aggregate_width, "AGGREGATE");
    
    if (has_per_node_data) {
        for (int i = 0; i < layout->num_nodes; i++) {
            printf("  %*s", layout->node_width, layout->node_headers[i]);
        }
    }
    printf("\n");
    
    /* Print separator line */
    printf("  ");
    for (int i = 0; i < layout->metric_width; i++) printf("%s", BOX_HORIZONTAL_THIN);
    printf("  ");
    for (int i = 0; i < layout->aggregate_width; i++) printf("%s", BOX_HORIZONTAL_THIN);
    
    if (has_per_node_data) {
        for (int i = 0; i < layout->num_nodes; i++) {
            printf("  ");
            for (int j = 0; j < layout->node_width; j++) printf("%s", BOX_HORIZONTAL_THIN);
        }
    }
    printf("\n");
}

static void printDiffColumnHeaders(TableLayout *layout, int has_per_node_data) {
    printf("\n");
    printf("  %-*s  %*s", layout->metric_width, "METRIC", 
           layout->aggregate_width, "CLUSTER DELTA");
    
    if (has_per_node_data) {
        for (int i = 0; i < layout->num_nodes; i++) {
            printf("  %*s", layout->node_width, layout->node_headers[i]);
        }
    }
    printf("\n");
    
    /* Print separator line */
    printf("  ");
    for (int i = 0; i < layout->metric_width; i++) printf("%s", BOX_HORIZONTAL_THIN);
    printf("  ");
    for (int i = 0; i < layout->aggregate_width; i++) printf("%s", BOX_HORIZONTAL_THIN);
    
    if (has_per_node_data) {
        for (int i = 0; i < layout->num_nodes; i++) {
            printf("  ");
            for (int j = 0; j < layout->node_width; j++) printf("%s", BOX_HORIZONTAL_THIN);
        }
    }
    printf("\n");
}

/* Capture display output to buffer for alignment */
static void captureDisplayOutput(char *buffer, size_t buf_size, 
                                 DisplayFormat disp, const char *field_name,
                                 void *opaque, int node_count) {
    buffer[0] = '\0';
    FILE *temp_stream = fmemopen(buffer, buf_size - 1, "w");
    if (!temp_stream) return;
    
    FILE *orig_stdout = stdout;
    stdout = temp_stream;
    display_formatted(field_name, opaque, node_count, disp);
    stdout = orig_stdout;
    fclose(temp_stream);
    
    /* Remove trailing newlines */
    int len = strlen(buffer);
    while (len > 0 && (buffer[len-1] == '\n' || buffer[len-1] == '\r')) {
        buffer[--len] = '\0';
    }
}

static void printSnapshotRow(clusterSnapshot *snapshot, infoFieldType *field,
                             fieldSnapshot *field_snap, void *opaque,
                             TableLayout *layout, int has_per_node_data,
                             int field_node_count) {
    /* Build display name */
    sds display_name = sdsempty();
    if (!field->parse_config.key) {
        display_name = sdscatprintf(display_name, "%s", field->prefix_match);
    } else {
        display_name = sdscatprintf(display_name, "%s.%s", 
                                   field->prefix_match, field->parse_config.key);
    }
    
    /* Print metric name with left alignment */
    printf("  %-*s  ", layout->metric_width, display_name);
    
    /* Capture and print aggregate value with right alignment */
    char aggregate_buffer[256];
    captureDisplayOutput(aggregate_buffer, sizeof(aggregate_buffer),
                        field->display_format, "", opaque, field_node_count);
    printf("%*s", layout->aggregate_width, aggregate_buffer);
    
    /* Print per-node values if tracked */
    if (field->track_per_node && field_snap->per_node_values) {
        for (int node_idx = 0; node_idx < snapshot->num_nodes; node_idx++) {
            char node_buffer[256];
            captureDisplayOutput(node_buffer, sizeof(node_buffer),
                               field->display_format, "",
                               &field_snap->per_node_values[node_idx],
                               field_node_count);
            printf("  %*s", layout->node_width, node_buffer);
        }
    } else if (has_per_node_data) {
        /* Fill empty columns for fields without per-node data */
        for (int node_idx = 0; node_idx < snapshot->num_nodes; node_idx++) {
            printf("  %*s", layout->node_width, "");
        }
    }
    
    printf("\n");
    sdsfree(display_name);
}

static void captureDiffOutput(char *buffer, size_t buf_size,
                              DiffType diff_type, const char *field_name,
                              fieldSnapshot *old, fieldSnapshot *new_snap,
                              long long time_delta_ms, int node_idx) {
    buffer[0] = '\0';
    FILE *temp_stream = fmemopen(buffer, buf_size - 1, "w");
    if (!temp_stream) return;
    
    FILE *orig_stdout = stdout;
    stdout = temp_stream;
    diff_generic(field_name, old, new_snap, time_delta_ms, node_idx, diff_type);
    stdout = orig_stdout;
    fclose(temp_stream);
    
    /* Remove trailing newlines */
    int len = strlen(buffer);
    while (len > 0 && (buffer[len-1] == '\n' || buffer[len-1] == '\r')) {
        buffer[--len] = '\0';
    }
}

static void printDiffRow(clusterSnapshot *old, clusterSnapshot *new_snap,
                        infoFieldType *field, int field_idx,
                        TableLayout *layout, int has_per_node_data,
                        long long time_delta_ms) {
    fieldSnapshot *old_field = &old->fields[field_idx];
    fieldSnapshot *new_field = &new_snap->fields[field_idx];
    
    /* Build display name */
    sds display_name = sdsempty();
    if (!field->parse_config.key) {
        display_name = sdscatprintf(display_name, "%s", field->prefix_match);
    } else {
        display_name = sdscatprintf(display_name, "%s.%s", 
                                   field->prefix_match, field->parse_config.key);
    }
    
    /* Print metric name with left alignment */
    printf("  %-*s  ", layout->metric_width, display_name);
    
    /* Capture and print cluster delta with right alignment */
    char delta_buffer[512];
    if (field->diff_type != DIFF_NONE) {
        captureDiffOutput(delta_buffer, sizeof(delta_buffer),
                         field->diff_type, old_field->field_name,
                         old_field, new_field, time_delta_ms, -1);
    } else {
        long long delta = new_field->value - old_field->value;
        snprintf(delta_buffer, sizeof(delta_buffer), "Δ %lld", delta);
    }
    printf("%*s", layout->aggregate_width, 
           delta_buffer[0] ? delta_buffer : "N/A");
    
    /* Print per-node diffs if available */
    if (field->track_per_node && old_field->per_node_values && 
        new_field->per_node_values) {
        for (int node_idx = 0; node_idx < old->num_nodes && 
             node_idx < new_snap->num_nodes; node_idx++) {
            
            if (sdscmp(old->node_identifiers[node_idx], 
                      new_snap->node_identifiers[node_idx]) == 0) {
                
                char node_buffer[512];
                if (field->diff_type != DIFF_NONE) {
                    captureDiffOutput(node_buffer, sizeof(node_buffer),
                                    field->diff_type, old_field->field_name,
                                    old_field, new_field, time_delta_ms, node_idx);
                } else {
                    node_buffer[0] = '\0';
                }
                printf("  %*s", layout->node_width, node_buffer);
            } else {
                printf("  %*s", layout->node_width, "");
            }
        }
    } else if (has_per_node_data) {
        /* Fill empty columns */
        for (int node_idx = 0; node_idx < old->num_nodes && 
             node_idx < new_snap->num_nodes; node_idx++) {
            printf("  %*s", layout->node_width, "");
        }
    }
    
    printf("\n");
    sdsfree(display_name);
}

EngineType getEngineType(const char *ip_or_path, int port, enum valkeyConnectionType ct) {
    /* check info for : os:Amazon MemoryDB */
    valkeyContext *ctx = getValkeyContext(ct, ip_or_path, port);
    if (!ctx) return ENGINE_TYPE_UNKNOWN; /* Error obtaining context */
    // send command to node
    valkeyReply *reply = valkeyCommand(ctx, "INFO");
    if (!reply) {
        printf("Error: No response from node %s while checking index status.\n", ip_or_path);
        return ENGINE_TYPE_UNKNOWN;
    }
    // get the string value of line starts with os:
    char *os_line = strstr(reply->str, "os:");
    EngineType result;
    if (os_line) {
        if (strstr(os_line, "Amazon MemoryDB") != NULL) {
            result = ENGINE_TYPE_MEMORYDB; /* It's a MemoryDB node */
        } else if (strstr(os_line, "Amazon ElastiCache") != NULL) {
            result = ENGINE_TYPE_ELASTICACHE_VALKEY; /* It's a Redis node */
        } else {
            result = ENGINE_TYPE_OSS_VALKEY; /* Unknown engine */
        }
    } else {
        printf("Error: 'os' field not found in INFO output on node %s.\n", ip_or_path);
        result = ENGINE_TYPE_UNKNOWN; /* 'os' field not found */
    }
    
    freeReplyObject(reply);
    valkeyFree(ctx);
    return result;
}

/* Check if the server is running in Cluster Mode Enabled (CME) 
 * Returns: 1 if cluster_enabled:1, 0 if cluster_enabled:0, -1 on error
 */
int isClusterModeEnabled(valkeyContext *ctx) {
    if (!ctx) return -1;
    
    valkeyReply *reply = valkeyCommand(ctx, "INFO CLUSTER");
    if (!reply) {
        fprintf(stderr, "Error: No response from server while checking cluster mode.\n");
        return -1;
    }
    
    int cluster_enabled = -1;
    
    if (reply->type == VALKEY_REPLY_STRING || reply->type == VALKEY_REPLY_STATUS) {
        /* Look for cluster_enabled field in INFO output */
        char *cluster_line = strstr(reply->str, "cluster_enabled:");
        if (cluster_line) {
            /* Parse the value after cluster_enabled: */
            cluster_line += strlen("cluster_enabled:");
            cluster_enabled = atoi(cluster_line);
        }
    }
    
    freeReplyObject(reply);
    
    if (cluster_enabled == -1) {
        /* If cluster_enabled field not found, try INFO SERVER */
        reply = valkeyCommand(ctx, "INFO SERVER");
        if (reply && (reply->type == VALKEY_REPLY_STRING || reply->type == VALKEY_REPLY_STATUS)) {
            char *cluster_line = strstr(reply->str, "cluster_enabled:");
            if (cluster_line) {
                cluster_line += strlen("cluster_enabled:");
                cluster_enabled = atoi(cluster_line);
            }
        }
        if (reply) freeReplyObject(reply);
    }
    printf("Cluster Mode Enabled: %d\n", cluster_enabled);
    return cluster_enabled;
}



/* Convert FT.INFO RESP array response to line-based format
ec-search-zvi-ec-1shard-no-tls.ajfdds.clustercfg.euw1devo.cache.amazonaws.com:6379> ft.info vindex_4dim
 1) index_name
 2) vindex_4dim
 3) index_options
 4) (empty array)
 5) index_definition
 6) 1) key_type
    2) HASH
    3) prefixes
    4) 1) vkey4dim:
    5) default_score
    6) "1"
 7) attributes
 8) 1)  1) identifier
        2) embed
        3) attribute
        4) embed
        5) type
        6) VECTOR
        7) algorithm
        8) HNSW
        9) data_type
       10) FLOAT32
       11) dim
       12) (integer) 4
       13) distance_metric
       14) L2
       15) M
       16) (integer) 16
       17) ef_construction
       18) (integer) 200
       19) ef_runtime
       20) (integer) 32
       21) capacity
       22) (integer) 5294080
       23) size
       24) "5288102"
 9) num_docs
10) (integer) 5288102
11) num_terms
12) (integer) 0
13) num_records
14) (integer) 5288102
15) hash_indexing_failures
16) "0"
17) gc_stats
18)  1) bytes_collected
     2) "0"
     3) total_ms_run
     4) "0"
     5) total_cycles
     6) "0"
     7) average_cycle_time_ms
     8) "nan"
     9) last_run_time_ms
    10) "0"
    11) gc_numeric_trees_missed
    12) "0"
    13) gc_blocks_denied
    14) "0"
19) cursor_stats
20) 1) global_idle
    2) (integer) 0
    3) global_total
    4) (integer) 0
    5) index_capacity
    6) (integer) 0
    7) index_total
    8) (integer) 0
21) dialect_stats
22) 1) dialect_1
    2) (integer) 0
    3) dialect_2
    4) (integer) 0
    5) dialect_3
    6) (integer) 0
    7) dialect_4
    8) (integer) 0
23) Index Errors
24) 1) indexing failures
    2) (integer) 0
    3) last indexing error
    4) N/A
    5) last indexing error key
    6) "N/A"
    7) background indexing status
    8) OK
25) backfill_in_progress
26) "0"
27) backfill_complete_percent
28) "1.000000"
29) mutation_queue_size
30) "0"
31) recent_mutations_queue_delay
32) "0 sec"
33) state
34) ready
35) pending_slots_count
36) "0"
ec-search-zvi-ec-1shard-no-tls.ajfdds.clustercfg.euw1devo.cache.amazonaws.com:6379> 
*/
static sds convertFtInfoToLines(valkeyReply *reply, const char *prefix) {
    if (!reply ) return NULL;
    // printf("Converting FT.INFO response to lines with prefix: %s\n", prefix ? prefix : "(none)");
    sds lines = sdsempty();
    if (reply->type != VALKEY_REPLY_ARRAY) {
        if (reply->type == VALKEY_REPLY_STRING || reply->type == VALKEY_REPLY_STATUS) {
            lines = sdscatprintf(lines, "%s:%s\n", prefix ? prefix : "", reply->str);
        } else if (reply->type == VALKEY_REPLY_INTEGER) {
            lines = sdscatprintf(lines, "%s:%lld\n", prefix ? prefix : "", reply->integer);
        } else {
            printf("Unexpected FT.INFO reply type: %d\n", reply->type);
        }
        return lines;
    }
    for (size_t i = 0; i < reply->elements; i++) {                
        valkeyReply *element_reply = reply->element[i];
        if (!element_reply) continue;
        if (element_reply->type == VALKEY_REPLY_ARRAY) {
            sds attr_lines = convertFtInfoToLines(element_reply, prefix);
            if (attr_lines) {
                lines = sdscatsds(lines, attr_lines);
                sdsfree(attr_lines);
            }
            continue;
        }
        if (i == reply->elements -1) {
            if (element_reply->type == VALKEY_REPLY_STRING || element_reply->type == VALKEY_REPLY_STATUS) {
                lines = sdscatprintf(lines, "%s:%s\n", prefix ? prefix : "", element_reply->str);
            } else if (element_reply->type == VALKEY_REPLY_INTEGER) {
                lines = sdscatprintf(lines, "%s:%lld\n", prefix ? prefix : "", element_reply->integer);
            }        
            break; /* No value for the last key */
        }
        char *key_name = element_reply->str;
        
        /* Build full key with prefix if provided */
        sds full_key = prefix ? sdscatprintf(sdsempty(), "%s.%s", prefix, key_name) 
                              : sdsnew(key_name);
        i++;
        valkeyReply *value = reply->element[i];
        if (value->type == VALKEY_REPLY_STRING || value->type == VALKEY_REPLY_STATUS) {
            lines = sdscatprintf(lines, "%s:%s\n", full_key, value->str);
        } else if (value->type == VALKEY_REPLY_INTEGER) {
            lines = sdscatprintf(lines, "%s:%lld\n", full_key, value->integer);
        } else if (value->type == VALKEY_REPLY_ARRAY) {
            /* Handle nested arrays */
            /* Attributes array contains one or more attribute definitions */
            sds attr_lines = convertFtInfoToLines(value, full_key);
            if (attr_lines) {
                lines = sdscatsds(lines, attr_lines);
                sdsfree(attr_lines);
            }
        } else {
            printf("Skipping unsupported value type %d for key %s\n", value->type, full_key);
        }
        
        sdsfree(full_key);
    }
    
    return lines;
}



/* 
ec-search-zvi-memdb-no-tls-0001-001.ajfdds.0001.memorydb-devo.eu-west-1.amazonaws.com:6379> ft.info gist-960-1M-960-100
 1) index_name
 2) "gist-960-1M-960-100"
 3) creation_timestamp
 4) (integer) 1760486768231299
 5) key_type
 6) HASH
 7) key_prefixes
 8) 1) "zvec_gist:"
 9) fields
10) 1)  1) identifier
        2) vector_field
        3) field_name
        4) vector_field
        5) type
        6) VECTOR
        7) option
        8)
        9) vector_params
       10)  1) algorithm
            2) HNSW
            3) data_type
            4) FLOAT32
            5) dimension
            6) (integer) 960
            7) distance_metric
            8) L2
            9) initial_capacity
           10) (integer) 1000
           11) current_capacity
           12) (integer) 1735134
           13) maximum_edges
           14) (integer) 12
           15) ef_construction
           16) (integer) 400
           17) ef_runtime
           18) (integer) 200
           19) epsilon
           20) "0.01"
11) space_usage
12) (integer) 3063809792
13) fulltext_space_usage
14) (integer) 38524385
15) vector_space_usage
16) (integer) 3025285407
17) num_docs
18) (integer) 1000000
19) num_indexed_vectors
20) (integer) 1000000
21) current_lag
22) (integer) 0
23) index_status
24) AVAILABLE
25) index_degradation_percentage
26) (integer) 41
ec-search-zvi-memdb-no-tls-0001-001.ajfdds.0001.memorydb-devo.eu-west-1.amazonaws.com:6379>
*/
static sds convertMemDBFtInfoToLines(valkeyReply *reply, const char *prefix) {
    if (!reply) return NULL;

    sds lines = sdsempty();
    
    /* Handle non-array types */
    if (reply->type != VALKEY_REPLY_ARRAY) {
        if (reply->type == VALKEY_REPLY_STRING || reply->type == VALKEY_REPLY_STATUS) {
            lines = sdscatprintf(lines, "%s:%s\n", prefix ? prefix : "", reply->str);
        } else if (reply->type == VALKEY_REPLY_INTEGER) {
            lines = sdscatprintf(lines, "%s:%lld\n", prefix ? prefix : "", reply->integer);
        }
        return lines;
    }
    
    /* Process array elements as key-value pairs */
    for (size_t i = 0; i < reply->elements; i += 2) {
        if (i + 1 >= reply->elements) break; /* Ensure we have both key and value */
        
        valkeyReply *key_elem = reply->element[i];
        valkeyReply *val_elem = reply->element[i + 1];
        
        if (!key_elem || !val_elem) continue;
        
        /* Key should be a string */
        if (key_elem->type != VALKEY_REPLY_STRING && key_elem->type != VALKEY_REPLY_STATUS) {
            continue;
        }
        
        char *key_name = key_elem->str;
        
        /* Build full key with prefix */
        sds full_key = prefix ? sdscatprintf(sdsempty(), "%s.%s", prefix, key_name) 
                              : sdsnew(key_name);
        
        /* Handle different value types */
        if (val_elem->type == VALKEY_REPLY_STRING || val_elem->type == VALKEY_REPLY_STATUS) {
            /* Simple string value */
            lines = sdscatprintf(lines, "%s:%s\n", full_key, val_elem->str);
        } else if (val_elem->type == VALKEY_REPLY_INTEGER) {
            /* Integer value */
            lines = sdscatprintf(lines, "%s:%lld\n", full_key, val_elem->integer);
        } else if (val_elem->type == VALKEY_REPLY_ARRAY) {
            /* Nested array - check if it's a list or nested key-value pairs */
            if (val_elem->elements > 0 && val_elem->element[0]) {
                valkeyReply *first = val_elem->element[0];
                
                /* If first element is an array, it's likely a complex structure (like fields) */
                if (first->type == VALKEY_REPLY_ARRAY) {
                    /* Process each sub-array with the current key as prefix */
                    for (size_t j = 0; j < val_elem->elements; j++) {
                        sds nested_lines = convertMemDBFtInfoToLines(val_elem->element[j], full_key);
                        if (nested_lines) {
                            lines = sdscatsds(lines, nested_lines);
                            sdsfree(nested_lines);
                        }
                    }
                } else if ((first->type == VALKEY_REPLY_STRING || first->type == VALKEY_REPLY_STATUS) &&
                           val_elem->elements % 2 == 0) {
                    /* Even number of string elements suggests key-value pairs */
                    sds nested_lines = convertMemDBFtInfoToLines(val_elem, full_key);
                    if (nested_lines) {
                        lines = sdscatsds(lines, nested_lines);
                        sdsfree(nested_lines);
                    }
                } else {
                    /* It's a simple list - just concat the first value */
                    if (first->type == VALKEY_REPLY_STRING || first->type == VALKEY_REPLY_STATUS) {
                        lines = sdscatprintf(lines, "%s:%s\n", full_key, first->str);
                    } else if (first->type == VALKEY_REPLY_INTEGER) {
                        lines = sdscatprintf(lines, "%s:%lld\n", full_key, first->integer);
                    }
                }
            }
        }
        
        sdsfree(full_key);
    }
    
    return lines;
}

int getNodeProgressEC(clusterNode *node, enum valkeyConnectionType ct, const char *index_name, long long int* node_docs, int* progress_percent) {
    *node_docs = 0;
    int in_progress = 0;
    *progress_percent = 100;
    valkeyContext *ctx = node->ctx ? node->ctx : getValkeyContext(ct, node->ip, node->port);
    assert(ctx);

    // send command to node
    valkeyReply *reply = valkeyCommand(ctx, "FT.INFO %b", index_name, strlen(index_name));
    assert(reply);
    // printf("FT.INFO response from node %s:\n", node->name);
    // fflush(stdout);
    // sleep(1); // give some time for the user to read the message
    sds info_lines = convertFtInfoToLines(reply, NULL);
    freeReplyObject(reply);
    if (!info_lines) {
        printf("Error: Unable to parse FT.INFO response from node %s.\n", node->name);
        assert(0);
    }

    /* Check for backfill_in_progress line */
    char *line = strstr(info_lines, "backfill_in_progress:");
    if (line) {
        sscanf(line, "backfill_in_progress:%d", &in_progress);
    }
    // get backfill_complete_percent line
    line = strstr(info_lines, "backfill_complete_percent:");
    double backfill_complete_percent = 0.0;
    if (line) {
        sscanf(line, "backfill_complete_percent:%lf", &backfill_complete_percent);
    }
    // set in progress percent by backfill_complete_percent*100 as integer
    *progress_percent = (int)(backfill_complete_percent * 100);
    // get num_docs line
    line = strstr(info_lines, "num_docs:");
    long long num_docs = 0;
    if (line) {
        sscanf(line, "num_docs:%lld", &num_docs);
    }

    /* Update progress metrics */
    *node_docs += num_docs;
    sdsfree(info_lines);
    return in_progress;
}

int getNodeProgressMemoryDB(clusterNode *node, enum valkeyConnectionType ct, const char *index_name, long long int* node_docs, int* progress_percent) {
    *node_docs = 0;
    int in_progress = 0;
    *progress_percent = 100;
    valkeyContext *ctx = node->ctx ? node->ctx : getValkeyContext(ct, node->ip, node->port);
    assert(ctx);

    // send command to node
    valkeyReply *reply = valkeyCommand(ctx, "FT.INFO %b", index_name, strlen(index_name));
    assert(reply);
    // sleep(1); // give some time for the user to read the message
    sds info_lines = convertMemDBFtInfoToLines(reply, NULL);
    freeReplyObject(reply);
    assert(info_lines);
    valkeyReply *search_info_reply = valkeyCommand(ctx, "INFO SEARCH");
    assert(search_info_reply);

    sds search_info_lines = convertMemDBFtInfoToLines(search_info_reply, NULL);
    assert(search_info_lines);
    freeReplyObject(search_info_reply);
    if (strlen(info_lines) == 0) {
        if (info_lines) sdsfree(info_lines);
        // if node is replica, it might be lagging, so just return in progress.
        if (node->is_replica) {
            *progress_percent = 0;
            return 1;
        }
        fprintf(stderr, "Error: Empty FT.INFO response from node %s.\n", node->name);
        fflush(stderr);
        assert(0);        
    }
    if (strstr(info_lines, "index_degradation_percentage:") == NULL) {
        fprintf(stderr, "Error: FT.INFO response from node %s does not contain expected fields. GOT:\n%s\n", node->name, info_lines);
        fflush(stderr);
        sdsfree(info_lines);
        assert(0);
    }
    char *search_current_backfill_num_str = strstr(search_info_lines, "search_num_active_backfills:");
    if (!search_current_backfill_num_str) {
        fprintf(stderr, "Error: Missing search_num_active_backfills in INFO SEARCH response from node %s.\n%s\n", node->name, search_info_lines);
        fflush(stderr);
        sdsfree(info_lines);
        sdsfree(search_info_lines);
        assert(0);
    }
    // // verify memorydb 
    // assert(strstr(info_lines, "index_degradation_percentage:") != NULL);
    char *search_current_backfill_progress_percentage_str = strstr(search_info_lines, "search_current_backfill_progress_percentage:");
    if (!search_current_backfill_progress_percentage_str) {
        int active_backfills = 0;
        int res = sscanf(search_current_backfill_num_str, "search_num_active_backfills:%d", &active_backfills);
        if (res != 1) {
            fprintf(stderr, "Error: Unable to parse search_num_active_backfills from node %s.\nsearch_num_active_backfills: %s\n=======\n Info lines: %s\n", node->name, search_current_backfill_num_str, search_info_lines);
            fflush(stderr);
            sdsfree(info_lines);
            assert(0);
        }
        if (active_backfills > 0) {
            fprintf(stderr, "Error: Missing search_current_backfill_progress_percentage in INFO SEARCH response from node %s.\n%s\n", node->name, search_info_lines);
            fflush(stderr);
            sdsfree(info_lines);
            sdsfree(search_info_lines);
            assert(0);
        }
    }
    // assert(search_current_backfill_progress_percentage_str != NULL);
    char *status_str = strstr(info_lines, "index_status:");
    char *degradation_str = strstr(info_lines, "index_degradation_percentage:");
    char *num_docs_str = strstr(info_lines, "num_indexed_vectors:");
    if (!status_str || !degradation_str || !num_docs_str) {
        fprintf(stderr, "Error: Missing expected fields in FT.INFO response from node %s.\n", node->name);
        fflush(stderr);
        sdsfree(info_lines);
        assert(0);
    }
    char status[64];
    int degradation = 0;
    if (sscanf(status_str, "index_status:%63s", status) != 1) {
        fprintf(stderr, "Error: Unable to parse index_status from node %s.\n", node->name);
        fflush(stderr);
        sdsfree(info_lines);
        assert(0);
    }
    if (sscanf(degradation_str, "index_degradation_percentage:%d", &degradation) != 1) {
        fprintf(stderr, "Error: Unable to parse index_degradation_percentage from node %s.\n", node->name);
        fflush(stderr);
        sdsfree(info_lines);
        assert(0);
    }
    if (sscanf(num_docs_str, "num_indexed_vectors:%lld", node_docs) != 1) {
        fprintf(stderr, "Error: Unable to parse num_indexed_vectors from node %s.\n", node->name);
        fflush(stderr);
        sdsfree(info_lines);
        assert(0);
    }
    
    // check status
    if (strcmp(status, "BACKFILLING") == 0 || strstr(status, "QUEUED") != NULL || degradation > 0) {
        *progress_percent = 0;
        if (degradation > 0) {
            *progress_percent = 100 - degradation; // 100% if not degraded, 0% if fully degraded
        }
        if (strcmp(status, "BACKFILLING") == 0) {
            in_progress = 1;
            int res = sscanf(search_current_backfill_progress_percentage_str, "search_current_backfill_progress_percentage:%d", progress_percent);
            if (res != 1) {
                fprintf(stderr, "Error: Unable to parse search_current_backfill_progress_percentage from node %s.\nsearch_current_backfill_progress_percentage_str: %s\n=======\n Info lines: %s\n", node->name, search_current_backfill_progress_percentage_str, search_info_lines);
                fflush(stderr);
                sdsfree(info_lines);
                assert(0);
            }
        } 
    } else {
        assert(strcmp(status, "AVAILABLE") == 0);
    }
    sdsfree(info_lines);
    sdsfree(search_info_lines);
        
    return in_progress;
// error:
//     /* Move to next line but keep the progress bar visible */
//     printf("\n");
//     fflush(stdout);
    
//     /* Reset ANSI attributes for subsequent output */
//     printf("\033[0m");
//     fflush(stdout);

}

void waitForIndexBackfillComplete(EngineType engine_type, int cluster_node_count, clusterNode **cluster_nodes,
                                        enum valkeyConnectionType ct, const char **index_names, int num_indexes) {
    if (!index_names) return;
    printf("%s: Waiting for index%s: ", 
           engine_type == ENGINE_TYPE_MEMORYDB ? "MemoryDB" : "ValkeySearch",
           num_indexes > 1 ? "es" : "");
    for (int i = 0; i < num_indexes; i++) {
        if (i == 0) {
            printf("'%s'", index_names[i]);
        } else {
            printf(", '%s'", index_names[i]);
        }
    }
    printf(" backfill to complete on all nodes...\n");
    fflush(stdout);
    // wait for 2 seconds before checking progress
    sleep(2);
    long long int total_docs = 0;
    int backfill_in_progress_nodes = 0;
    int global_backfill_complete_percent = 0;
    
    /* Initialize progress bar with 100 as total (percentage), and track total_docs as secondary metric */
    progressBar progress;
    initProgressBar(&progress, 100, 0, "Backfill");  /* 0 for rate_total means we'll set it dynamically */
    
    /* Force initial display at 0% - this shows immediately */
    forceUpdateProgressBar(&progress, 0, 0);

    do {
        total_docs = 0;
        backfill_in_progress_nodes = 0;
        global_backfill_complete_percent = 0;
        // go over all nodes and check FT.INFO
        // check these metrics:
        // 23) index_status
        // 24) AVAILABLE | BACKFILLING
        // 25) index_degradation_percentage
        // 26) (integer) 41
        // if index_status is AVAILABLE on all nodes, and index_degradation_percentage is 0 on all nodes, we are done
        // if index_status is BACKFILLING on any node, we are not done
        // if not, continue to next nodes to collect global progress, print global progress and number of nodes still backfilling and number of docs
        for (int i = 0; i < cluster_node_count; i++) {
            clusterNode *node = cluster_nodes[i];
            int progress_percent = 0;
            long long int node_docs = 0;
            assert(node);
            for (int j = 0; j < num_indexes; j++) {
                const char *index_name = index_names[j];
                int in_progress;
                if (engine_type == ENGINE_TYPE_MEMORYDB) {
                    in_progress = getNodeProgressMemoryDB(node, ct, index_name, &node_docs, &progress_percent);
                } else {
                    in_progress = getNodeProgressEC(node, ct, index_name, &node_docs, &progress_percent);
                }
                if (in_progress) {
                    backfill_in_progress_nodes++;
                }                                     
                global_backfill_complete_percent += progress_percent;
                total_docs += node_docs;
            }

        }

        // Update progress bar - force update to ensure it shows even if percentage didn't change
        // Show percentage progress and total documents as secondary metric
        global_backfill_complete_percent /= cluster_node_count * num_indexes;
        
        /* Enable secondary rate metric display after first iteration */
        if (progress.rate_total == 0 && total_docs > 0) {
            progress.rate_total = 1; /* Non-zero enables display */
        }
        
        forceUpdateProgressBar(&progress, (uint64_t)global_backfill_complete_percent, (uint64_t)total_docs);
        
        // wait for 1 second before next check
        sleep(1);
    } while (backfill_in_progress_nodes > 0);
    forceUpdateProgressBar(&progress, 100, (uint64_t)total_docs);
    finishProgressBar(&progress);
    printf("%d Index%s backfill process has completed on all nodes. Total docs indexed: %lld\n", 
           num_indexes, num_indexes > 1 ? "es" : "", total_docs);
}

/* Create snapshot from current cluster state 
# search_index_stats
search_used_memory_bytes:4171347520
search_used_memory_human:3.88G
search_number_of_indexes:4
search_num_fulltext_indexes:0
search_num_vector_indexes:4
search_num_hash_indexes:4
search_num_json_indexes:0
search_num_available_indexes:4
search_index_validation_failures:0
search_total_indexed_keys:2971718
search_total_indexed_vectors:2971718
search_total_indexed_hash_keys:2971718
search_total_indexed_json_documents:0
search_total_index_size:3533826366
search_total_fulltext_index_size:112647672
search_total_vector_index_size:3421178694
search_max_index_degradation_percentage:41
search_max_index_lag_ms:0

# search_ingestion
search_background_indexing_status:NO_ACTIVITY

# search_backfill
search_num_active_backfills:0
search_backfills_paused:no

# search_query
search_num_active_queries:0

*/

/* Create snapshot from current cluster state 
# Modules

# search_coordinator

# search_core-management
search_cores_received:0
search_cores_requested:0

# search_global_ingestion
search_ingest_field_numeric:0
search_ingest_field_tag:0
search_ingest_field_vector:1000323
search_ingest_hash_blocked:0
search_ingest_hash_keys:2167580
search_ingest_json_blocked:0
search_ingest_json_keys:0
search_ingest_last_batch_size:0
search_ingest_total_batches:0
search_ingest_total_failures:0

# search_global_metrics
search_bounds_check_errors:0
search_hnsw_edges_marked_deleted:14112
search_hnsw_nodes_marked_deleted:882
search_interned_strings_memory:35397554
search_keys_bytes:20997522
search_num_flat_indexes:0
search_num_flat_nodes:0
search_num_hnsw_edges:16012224
search_num_hnsw_indexes:1
search_num_hnsw_nodes:1000764
search_num_interned_strings:1899884
search_num_numeric_indexes:0
search_num_numeric_records:0
search_num_tag_indexes:0
search_num_tags:0
search_tags_bytes:0
search_vectors_bytes:14400032
search_vectors_marked_deleted:1
search_vectors_memory_marked_deleted:16

# search_hnswlib
search_hnsw_add_exceptions_count:0
search_hnsw_create_exceptions_count:0
search_hnsw_modify_exceptions_count:441
search_hnsw_remove_exceptions_count:0
search_hnsw_search_exceptions_count:0

# search_index_metering
search_module_background_mspus:0

# search_index_stats
search_number_of_active_indexes:1
search_number_of_active_indexes_indexing:0
search_number_of_active_indexes_running_queries:0
search_number_of_attributes:1
search_number_of_indexes:1
search_total_active_write_threads:16
search_total_indexed_documents:1000323
search_total_indexing_time:0

# search_indexing
search_background_indexing_status:NO_ACTIVITY

# search_latency
search_hnsw_vector_index_search_latency_usec:p50=25.343,p99=44.031,p99.9=63.231

# search_memory
search_index_reclaimable_memory:16
search_search_extra_counter_00:0
search_used_memory:856192040
search_used_memory_bytes:856192040
search_used_memory_human:816.53MiB

# search_metering
search_ft_search_ecpus:0
search_prefix_write_error_cnt:0
search_suffix_write_error_cnt:0

# search_query
search_failure_requests_count:0
search_hybrid_requests_count:0
search_inline_filtering_requests_count:0
search_query_prefiltering_requests_cnt:0
search_result_record_dropped_count:0
search_successful_requests_count:3127457307

# search_rdb
search_rdb_load_failure_cnt:0
search_rdb_load_success_cnt:0
search_rdb_save_failure_cnt:0
search_rdb_save_success_cnt:0

# search_string_interning
search_string_interning_memory_bytes:1899884
search_string_interning_memory_human:1.81MiB
search_string_interning_store_size:1899885

# search_test-counters
search_test-counter-ForceCancels:0
search_test-counter-gRPCCancels:0

# search_thread-pool
search_query_queue_size:0
search_read_cpu_time_sec:126989.62995999999
search_reader_resumed_cnt:0
search_used_read_cpu:4.2168464057504291
search_used_write_cpu:0.00065063934711687642
search_worker_pool_suspend_cnt:0
search_write_cpu_time_sec:462.99530899999996
search_writer_queue_size:0
search_writer_resumed_cnt:0
search_writer_suspension_expired_cnt:0

# search_time_slice_mutex
search_time_slice_deletes:0
search_time_slice_queries:3127457309
search_time_slice_read_periods:3127457309
search_time_slice_read_time:85366119063
search_time_slice_upserts:1114643
search_time_slice_write_periods:2167580
search_time_slice_write_time:2890865782

# search_timeouts
search_cancel-timeouts:0

# search_vector_externing
search_vector_externing_deferred_entry_cnt:0
search_vector_externing_entry_count:0
search_vector_externing_generated_value_cnt:0
search_vector_externing_hash_extern_errors:0
search_vector_externing_lru_promote_cnt:0
search_vector_externing_num_lru_entries:0
search_network_bytes_out:0
search_network_bytes_in:0

*/

clusterSnapshot* createClusterSnapshot(const char *command, 
                                        int num_fields, infoFieldType *fields, 
                                        int cluster_node_count, clusterNode **cluster_nodes,
                                        enum valkeyConnectionType ct, int print_info) {
    clusterSnapshot *snapshot = zcalloc(sizeof(clusterSnapshot));
    snapshot->timestamp_ms = ustime() / 1000;
    snapshot->num_fields = num_fields;
    snapshot->fields = zcalloc(num_fields * sizeof(fieldSnapshot));
    snapshot->num_nodes = cluster_node_count;
    snapshot->node_identifiers = zcalloc(snapshot->num_nodes * sizeof(sds));
    
    /* Initialize fields */
    for (int i = 0; i < num_fields; i++) {
        snapshot->fields[i].field_name = fields[i].prefix_match;
        snapshot->fields[i].valid = 0;
        snapshot->fields[i].per_node_values = NULL;
        snapshot->fields[i].node_count = 0;
        
        if (fields[i].track_per_node) {
            snapshot->fields[i].per_node_values = zcalloc(snapshot->num_nodes * sizeof(long long));
            snapshot->fields[i].node_count = snapshot->num_nodes;
        }
    }
    
    /* Temporary storage for aggregation */
    void **field_opaques = zcalloc(num_fields * sizeof(void *));
    int *field_node_counts = zcalloc(num_fields * sizeof(int));

    /* Detect command type */
    int is_ftinfo = (strncasecmp(command, "FT.INFO", 7) == 0);
    
    /* Query each node and aggregate (unchanged logic) */
    for (int node_idx = 0; node_idx < cluster_node_count; node_idx++) {
        clusterNode *node = cluster_nodes[node_idx];
        assert(node != NULL);
        int is_replica = node->replicate == NULL ? 0 : 1;
        
        /* Store node identifier */
        if (6379 == node->port) {
            snapshot->node_identifiers[node_idx] = sdscatprintf(sdsempty(), 
                                                        "%s(%c)", node->ip, is_replica ? 'R' : 'P');
        } else {
            snapshot->node_identifiers[node_idx] = sdscatprintf(sdsempty(), 
                                                        "%s:%d(%c)", node->ip, node->port, 
                                                        is_replica ? 'R' : 'P');
        }
        
        valkeyContext *ctx = getValkeyContext(ct, node->ip, node->port);
        assert(ctx != NULL);
        
        valkeyReply *reply = valkeyCommand(ctx, command);
        assert(reply != NULL);
        
        sds lines = NULL;
        
        if (is_ftinfo && reply->type == VALKEY_REPLY_ARRAY) {
            lines = convertFtInfoToLines(reply, NULL);
        } else if (reply->type == VALKEY_REPLY_STRING || reply->type == VALKEY_REPLY_STATUS) {
            lines = sdsnew(reply->str);
        }
        
        if (lines) {
            char *lines_copy = sdsdup(lines);
            char *saveptr;
            char *line = strtok_r(lines_copy, "\n", &saveptr);
            
            while (line != NULL) {
                if (*line && *line != '#') {
                    for (int field_idx = 0; field_idx < num_fields; field_idx++) {
                        infoFieldType *field = &fields[field_idx];
                        
                        if (field->match(line, field->prefix_match)) {
                            char *colon = strchr(line, ':');
                            if (colon) {
                                char *value = colon + 1;
                                long long parsed_value = parse_generic(value, field->parse_config);
                                
                                /* Store per-node value if tracking */
                                if (field->track_per_node && 
                                    snapshot->fields[field_idx].per_node_values) {
                                    snapshot->fields[field_idx].per_node_values[node_idx] = parsed_value;
                                }
                                
                                /* Aggregate */
                                int is_last_node = (node_idx == cluster_node_count - 1);
                                if ((field->nodes_to_aggregate == FROM_ALL) || 
                                    (field->nodes_to_aggregate == FROM_PRIMARY_ONLY && !is_replica) || 
                                    (field->nodes_to_aggregate == FROM_REPLICA_ONLY && is_replica)) {
                                    aggregate_generic(&field_opaques[field_idx], parsed_value, 
                                              node_idx, is_last_node, field->aggregation_type);
                                    field_node_counts[field_idx]++;
                                }
                                
                                snapshot->fields[field_idx].valid = 1;
                            }
                            if (field->is_last) break;
                        }
                    }
                }
            line = strtok_r(NULL, "\n", &saveptr);
        }
        
        sdsfree(lines_copy);
        sdsfree(lines);
    }        freeReplyObject(reply);
        valkeyFree(ctx);
    }
    
    /* Store aggregated values */
    for (int field_idx = 0; field_idx < num_fields; field_idx++) {
        if (field_opaques[field_idx] && snapshot->fields[field_idx].valid) {
            AggregateState *state = (AggregateState *)field_opaques[field_idx];
            
            if (fields[field_idx].aggregation_type == AGG_AVERAGE) {
                snapshot->fields[field_idx].value = state->avg_state.count > 0 ? 
                    state->avg_state.sum / state->avg_state.count : 0;
            } else if (fields[field_idx].aggregation_type == AGG_MINMAX) {
                snapshot->fields[field_idx].value = state->minmax_state.max_val;
            } else {
                snapshot->fields[field_idx].value = state->simple_value;
            }
        }
    }
    
    if (print_info) {
        /* Pre-check: count rows that will be printed */
        int rows_to_print = 0;
        for (int field_idx = 0; field_idx < num_fields; field_idx++) {
            if (!shouldSkipZeroRow(&snapshot->fields[field_idx], &fields[field_idx],
                                  snapshot->num_nodes, search_debug)) {
                rows_to_print++;
            }
        }
        
        if (rows_to_print == 0) {
            /* Clean up and return without printing */
            for (int i = 0; i < num_fields; i++) {
                if (field_opaques[i]) zfree(field_opaques[i]);
            }
            zfree(field_opaques);
            zfree(field_node_counts);
            return snapshot;
        }
        
        /* Calculate layout */
        TableLayout layout = calculateTableLayout(snapshot, num_fields, fields, 
                                                  search_debug);
        
        /* Check if we have per-node data */
        int has_per_node_data = 0;
        for (int i = 0; i < num_fields; i++) {
            if (fields[i].track_per_node && snapshot->fields[i].per_node_values) {
                has_per_node_data = 1;
                break;
            }
        }
        
        int total_width = calculateTotalWidth(&layout, has_per_node_data);
        
        /* Print table header */
        TableHeader header = {
            .title = "AGGREGATE STATS",
            .command = command,
            .num_fields = num_fields,
            .timestamp_ms = snapshot->timestamp_ms
        };
        printTableHeader(header, total_width);
        
        /* Print column headers */
        if (has_per_node_data) {
            printSnapshotColumnHeaders(&layout, has_per_node_data);
        } else {
            printf("\n");
        }
        
        /* Print data rows */
        for (int field_idx = 0; field_idx < num_fields; field_idx++) {
            if (shouldSkipZeroRow(&snapshot->fields[field_idx], &fields[field_idx],
                                 snapshot->num_nodes, search_debug)) {
                if (field_opaques[field_idx]) {
                    zfree(field_opaques[field_idx]);
                    field_opaques[field_idx] = NULL;
                }
                continue;
            }
            
            printSnapshotRow(snapshot, &fields[field_idx], &snapshot->fields[field_idx],
                           field_opaques[field_idx], &layout, has_per_node_data,
                           field_node_counts[field_idx]);
            
            if (field_opaques[field_idx]) {
                zfree(field_opaques[field_idx]);
            }
        }
        
        printf("\n");
        freeTableLayout(&layout);
    } else {
        /* Not printing - just clean up opaques */
        for (int i = 0; i < num_fields; i++) {
            if (field_opaques[i]) zfree(field_opaques[i]);
        }
    }
    
    zfree(field_opaques);
    zfree(field_node_counts);
    return snapshot;
}

/* Free snapshot */
void freeClusterSnapshot(clusterSnapshot *snapshot) {
    if (!snapshot) return;
    
    for (int i = 0; i < snapshot->num_fields; i++) {
        // sdsfree(snapshot->fields[i].field_name);
        if (snapshot->fields[i].per_node_values) {
            zfree(snapshot->fields[i].per_node_values);
        }
    }
    
    for (int i = 0; i < snapshot->num_nodes; i++) {
        if (snapshot->node_identifiers[i]) {
            sdsfree(snapshot->node_identifiers[i]);
        }
    }
    
    zfree(snapshot->fields);
    zfree(snapshot->node_identifiers);
    zfree(snapshot);
}


void compareClusterSnapshots(clusterSnapshot *old, clusterSnapshot *new_snap,
                            int num_fields, infoFieldType *fields) {
    if (!old || !new_snap) {
        fprintf(stderr, "Cannot compare: missing snapshot\n");
        return;
    }
    
    long long time_delta_ms = new_snap->timestamp_ms - old->timestamp_ms;
    if (time_delta_ms <= 0) {
        fprintf(stderr, "Invalid time delta: %lld ms\n", time_delta_ms);
        return;
    }
    
    /* Pre-check: count rows that will be printed */
    int rows_to_print = 0;
    for (int field_idx = 0; field_idx < num_fields; field_idx++) {
        if ((fields[field_idx].diff_type == DIFF_NONE) || field_idx >= old->num_fields || 
            field_idx >= new_snap->num_fields) {
            continue;
        }
        
        fieldSnapshot *old_field = &old->fields[field_idx];
        fieldSnapshot *new_field = &new_snap->fields[field_idx];
        
        if (!old_field->valid || !new_field->valid) continue;
        if (strcmp(old_field->field_name, new_field->field_name) != 0) continue;

        int is_memory_diff = (fields[field_idx].diff_type == DIFF_MEMORY_GROWTH);
        if (!shouldSkipZeroDeltaRow(old_field, new_field, &fields[field_idx],
                                   old->num_nodes, search_debug, is_memory_diff)) {
            rows_to_print++;
        }
    }
    
    if (rows_to_print == 0) return;
    
    /* Format timestamps */
    time_t old_time_sec = old->timestamp_ms / 1000;
    time_t new_time_sec = new_snap->timestamp_ms / 1000;
    int old_ms = old->timestamp_ms % 1000;
    int new_ms = new_snap->timestamp_ms % 1000;
    
    struct tm old_tm_copy, new_tm_copy;
    struct tm *tm_ptr;
    
    tm_ptr = localtime(&old_time_sec);
    old_tm_copy = *tm_ptr;
    
    tm_ptr = localtime(&new_time_sec);
    new_tm_copy = *tm_ptr;
    
    char old_time_str[32], new_time_str[32], temp_buf[26];
    
    strftime(temp_buf, 26, "%H:%M:%S", &old_tm_copy);
    snprintf(old_time_str, sizeof(old_time_str), "%s.%03d", temp_buf, old_ms);
    
    strftime(temp_buf, 26, "%H:%M:%S", &new_tm_copy);
    snprintf(new_time_str, sizeof(new_time_str), "%s.%03d", temp_buf, new_ms);
    
    /* Calculate layout */
    TableLayout layout = calculateTableLayout(old, num_fields, fields, search_debug);
    
    /* Check for per-node data */
    int has_per_node_data = 0;
    for (int i = 0; i < num_fields; i++) {
        if ((fields[i].diff_type == DIFF_NONE) || i >= old->num_fields || i >= new_snap->num_fields) continue;
        fieldSnapshot *old_field = &old->fields[i];
        if (fields[i].track_per_node && old_field->per_node_values) {
            has_per_node_data = 1;
            break;
        }
    }
    
    int total_width = calculateTotalWidth(&layout, has_per_node_data);
    
    /* Print header with string constants */
    printf("\n");
    printTableBorder(total_width, BOX_TOP_LEFT, BOX_HORIZONTAL, BOX_TOP_RIGHT);
    printCenteredTitle("CLUSTER STATISTICS DELTA", total_width);
    printTableBorder(total_width, BOX_VERTICAL_LEFT, BOX_HORIZONTAL, BOX_VERTICAL_RIGHT);
    
    /* Print time interval info */
    char info_line[256];
    snprintf(info_line, sizeof(info_line), "Time Interval: %.2f sec | From: %s to %s", 
             (double)time_delta_ms / 1000.0, old_time_str, new_time_str);
    int info_len = strlen(info_line);
    
    printf("%s %s", BOX_VERTICAL, info_line);
    for (int i = info_len + 1; i < total_width; i++) printf(" ");
    printf("%s\n", BOX_VERTICAL);
    
    printTableBorder(total_width, BOX_BOTTOM_LEFT, BOX_HORIZONTAL, BOX_BOTTOM_RIGHT);
    
    /* Print column headers */
    if (has_per_node_data) {
        printDiffColumnHeaders(&layout, has_per_node_data);
    } else {
        printf("\n");
    }
    
    /* Print diff rows */
    for (int field_idx = 0; field_idx < num_fields; field_idx++) {
        if ((fields[field_idx].diff_type == DIFF_NONE) || field_idx >= old->num_fields || 
            field_idx >= new_snap->num_fields) {
            continue;
        }
        
        fieldSnapshot *old_field = &old->fields[field_idx];
        fieldSnapshot *new_field = &new_snap->fields[field_idx];
        
        if (!old_field->valid || !new_field->valid) continue;
        if (strcmp(old_field->field_name, new_field->field_name) != 0) continue;
        
        int is_memory_diff = (fields[field_idx].diff_type == DIFF_MEMORY_GROWTH);
        if (shouldSkipZeroDeltaRow(old_field, new_field, &fields[field_idx],
                                  old->num_nodes, search_debug, is_memory_diff)) {
            continue;
        }
        
        printDiffRow(old, new_snap, &fields[field_idx], field_idx,
                    &layout, has_per_node_data, time_delta_ms);
    }
    
    printf("\n");
    freeTableLayout(&layout);
}

/* INFO SEARCH fields with temporal tracking - supports EC CME, EC CMD, and MemoryDB */
infoFieldType search_info_fields[] = {
    /* Request rates - Common to all systems */
    {"search_successful_requests_count", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_INTEGER, DIFF_RATE_COUNT, 1, FROM_ALL, 1},
    {"search_failure_requests_count", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_INTEGER, DIFF_RATE_COUNT, 1, FROM_ALL, 1},
    {"search_hybrid_requests_count", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_INTEGER, DIFF_RATE_COUNT, 1, FROM_ALL, 1},

    /* Memory growth - Common to all systems */
    {"search_used_memory_bytes", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_MEMORY_MB, DIFF_MEMORY_GROWTH, 1, FROM_PRIMARY_ONLY, 1},
    {"search_index_reclaimable_memory", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_MEMORY_MB, DIFF_MEMORY_GROWTH, 1, FROM_PRIMARY_ONLY, 1},
    
    /* EC CMD/CME specific - ingestion */
    {"search_ingest_field_vector", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_INTEGER, DIFF_RATE_COUNT, 1, FROM_PRIMARY_ONLY, 1},
    
    /* EC CMD/CME specific - indexing rates */
    {"search_total_indexed_documents", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_INTEGER, DIFF_RATE_COUNT, 1, FROM_ALL, 1},
    {"search_total_active_write_threads", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_MINMAX, DISPLAY_MINMAX, DIFF_NONE, 1, FROM_ALL, 1},
    
    /* MemoryDB specific - indexing stats */
    {"search_total_indexed_keys", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_INTEGER, DIFF_RATE_COUNT, 1, FROM_PRIMARY_ONLY, 1},
    {"search_total_indexed_vectors", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_INTEGER, DIFF_RATE_COUNT, 1, FROM_PRIMARY_ONLY, 1},
    {"search_total_indexed_hash_keys", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_INTEGER, DIFF_RATE_COUNT, 1, FROM_PRIMARY_ONLY, 1},
    {"search_total_index_size", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_MEMORY_HUMAN, DIFF_MEMORY_GROWTH, 1, FROM_PRIMARY_ONLY, 1},
    {"search_total_vector_index_size", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_MEMORY_HUMAN, DIFF_MEMORY_GROWTH, 1, FROM_PRIMARY_ONLY, 1},
    {"search_max_index_degradation_percentage", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_MAX, DISPLAY_INTEGER, DIFF_NONE, 1, FROM_PRIMARY_ONLY, 1},
    {"search_max_index_lag_ms", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_MAX, DISPLAY_INTEGER, DIFF_NONE, 1, FROM_PRIMARY_ONLY, 1},
    
    /* EC CMD/CME specific - CPU usage */
    {"search_read_cpu_time_sec", exact_field_matcher, {PARSE_FLOAT_FIXED, NULL},
     AGG_SUM, DISPLAY_FLOAT, DIFF_RATE_COUNT, 1, FROM_ALL, 1},
    {"search_write_cpu_time_sec", exact_field_matcher, {PARSE_FLOAT_FIXED, NULL},
     AGG_SUM, DISPLAY_FLOAT, DIFF_RATE_COUNT, 1, FROM_PRIMARY_ONLY, 1},
    {"search_used_read_cpu", exact_field_matcher, {PARSE_FLOAT_FIXED, NULL},
     AGG_AVERAGE, DISPLAY_PERCENTAGE, DIFF_NONE, 1, FROM_ALL, 1},
    {"search_used_write_cpu", exact_field_matcher, {PARSE_FLOAT_FIXED, NULL},
     AGG_AVERAGE, DISPLAY_PERCENTAGE, DIFF_NONE, 1, FROM_PRIMARY_ONLY, 1},

    /* EC CMD/CME specific - Queue sizes */
    {"search_query_queue_size", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_AVERAGE, DISPLAY_INTEGER, DIFF_NONE, 1, FROM_ALL, 1},
    {"search_writer_queue_size", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_AVERAGE, DISPLAY_INTEGER, DIFF_NONE, 1, FROM_PRIMARY_ONLY, 1},

    /* Latencies - Common to EC CMD/CME, may not exist in MemoryDB */
    {"search_hnsw_vector_index_search_latency_usec", exact_field_matcher, {PARSE_PERCENTILE, "p50"},
     AGG_MAX, DISPLAY_LATENCY_USEC, DIFF_NONE, 1, FROM_ALL, 1},
    {"search_hnsw_vector_index_search_latency_usec", exact_field_matcher, {PARSE_PERCENTILE, "p99"},
     AGG_MAX, DISPLAY_LATENCY_USEC, DIFF_NONE, 1, FROM_ALL, 1},
    {"search_hnsw_vector_index_search_latency_usec", exact_field_matcher, {PARSE_PERCENTILE, "p99.9"},
     AGG_MAX, DISPLAY_LATENCY_USEC, DIFF_NONE, 1, FROM_ALL, 1},
    {"search_flat_vector_index_search_latency_usec", exact_field_matcher, {PARSE_PERCENTILE, "p99"},
     AGG_MAX, DISPLAY_LATENCY_USEC, DIFF_NONE, 1, FROM_ALL, 1},
    
    /* EC CME specific - coordinator latencies */
    {"search_coordinator_server_search_index_partition_success_latency_usec", exact_field_matcher, {PARSE_PERCENTILE, "p99"},
     AGG_MAX, DISPLAY_LATENCY_USEC, DIFF_NONE, 1, FROM_ALL, 1},
    {"search_coordinator_client_search_index_partition_success_latency_usec", exact_field_matcher, {PARSE_PERCENTILE, "p99"},
     AGG_MAX, DISPLAY_LATENCY_USEC, DIFF_NONE, 1, FROM_ALL, 1},
    
    /* Error rates - EC CMD/CME specific */
    {"search_hnsw_add_exceptions_count", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_INTEGER, DIFF_RATE_COUNT, 1, FROM_ALL, 1},
    {"search_bounds_check_errors", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_INTEGER, DIFF_RATE_COUNT, 1, FROM_ALL, 1},
    
    /* Counters - EC CMD/CME specific */
    {"search_num_hnsw_edges", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_INTEGER, DIFF_NONE, 1, FROM_PRIMARY_ONLY, 1},
    {"search_num_flat_nodes", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_INTEGER, DIFF_NONE, 1, FROM_PRIMARY_ONLY, 1},
    {"search_num_vector_indexes", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_INTEGER, DIFF_NONE, 1, FROM_PRIMARY_ONLY, 1},
    {"search_num_hnsw_indexes", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_INTEGER, DIFF_NONE, 1, FROM_PRIMARY_ONLY, 1},
    {"search_num_flat_indexes", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_INTEGER, DIFF_NONE, 1, FROM_PRIMARY_ONLY, 1},
    {"search_number_of_indexes", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_INTEGER, DIFF_NONE, 1, FROM_PRIMARY_ONLY, 1},
    {"search_num_available_indexes", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_INTEGER, DIFF_NONE, 1, FROM_PRIMARY_ONLY, 1},
    {"search_vectors_marked_deleted", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_INTEGER, DIFF_NONE, 1, FROM_PRIMARY_ONLY, 1},
    {"search_num_hnsw_nodes", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_INTEGER, DIFF_PERCENTAGE_CHANGE, 1, FROM_PRIMARY_ONLY, 1},
    
    /* Status - MemoryDB specific */
    {"search_background_indexing_status", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_INTEGER, DIFF_NONE, 0, FROM_PRIMARY_ONLY, 1},
    {"search_num_active_backfills", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_INTEGER, DIFF_NONE, 1, FROM_PRIMARY_ONLY, 1},
    {"search_current_backfill_progress_percentage", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_AVERAGE, DISPLAY_INTEGER, DIFF_NONE, 1, FROM_PRIMARY_ONLY, 1},
    {"search_num_active_queries", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_INTEGER, DIFF_NONE, 1, FROM_ALL, 1},
    
    /* Memory stats - Common to EC CMD/CME */
    {"search_vectors_memory_marked_deleted", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_MEMORY_HUMAN, DIFF_NONE, 1, FROM_PRIMARY_ONLY, 1},
    {"search_vectors_bytes", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_MEMORY_HUMAN, DIFF_NONE, 1, FROM_PRIMARY_ONLY, 1},
    {"search_interned_strings_memory", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_MEMORY_HUMAN, DIFF_NONE, 1, FROM_PRIMARY_ONLY, 1},
    
    /* Network stats - EC CME specific */
    {"search_network_bytes_in", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_MEMORY_HUMAN, DIFF_RATE_COUNT, 1, FROM_ALL, 1},
    {"search_network_bytes_out", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_MEMORY_HUMAN, DIFF_RATE_COUNT, 1, FROM_ALL, 1},
    {"search_coordinator_bytes_in", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_MEMORY_HUMAN, DIFF_RATE_COUNT, 1, FROM_ALL, 1},
    {"search_coordinator_bytes_out", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_MEMORY_HUMAN, DIFF_RATE_COUNT, 1, FROM_ALL, 1}
};



/* FT.INFO fields with temporal tracking - supports EC and MemoryDB */
infoFieldType ftinfo_fields[] = {
    /* EC format fields */
    {"num_docs", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_INTEGER, DIFF_RATE_COUNT, 1, FROM_PRIMARY_ONLY, 1},
    {"hash_indexing_failures", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_INTEGER, DIFF_RATE_COUNT, 1, FROM_ALL, 1},
    {"mutation_queue_size", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_AVERAGE, DISPLAY_INTEGER, DIFF_NONE, 1, FROM_PRIMARY_ONLY, 1},
    {"num_records", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_INTEGER, DIFF_NONE, 1, FROM_PRIMARY_ONLY, 1},
    {"attributes.dim", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_MINMAX, DISPLAY_MINMAX, DIFF_NONE, 0, FROM_PRIMARY_ONLY, 1},
    {"attributes.M", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_MINMAX, DISPLAY_MINMAX, DIFF_NONE, 0, FROM_PRIMARY_ONLY, 1},
    {"attributes.capacity", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_INTEGER, DIFF_NONE, 0, FROM_PRIMARY_ONLY, 1},
    {"attributes.size", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_INTEGER, DIFF_NONE, 0, FROM_PRIMARY_ONLY, 1},
    
    /* MemoryDB format fields */
    {"index_name", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_INTEGER, DIFF_NONE, 0, FROM_PRIMARY_ONLY, 1},
    {"num_indexed_vectors", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_INTEGER, DIFF_RATE_COUNT, 1, FROM_PRIMARY_ONLY, 1},
    {"space_usage", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_MEMORY_HUMAN, DIFF_MEMORY_GROWTH, 1, FROM_PRIMARY_ONLY, 1},
    {"vector_space_usage", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_MEMORY_HUMAN, DIFF_MEMORY_GROWTH, 1, FROM_PRIMARY_ONLY, 1},
    {"fulltext_space_usage", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_MEMORY_HUMAN, DIFF_MEMORY_GROWTH, 1, FROM_PRIMARY_ONLY, 1},
    {"current_lag", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_MAX, DISPLAY_INTEGER, DIFF_NONE, 1, FROM_PRIMARY_ONLY, 1},
    {"index_status", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_INTEGER, DIFF_NONE, 0, FROM_PRIMARY_ONLY, 1},
    {"index_degradation_percentage", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_MAX, DISPLAY_INTEGER, DIFF_NONE, 1, FROM_PRIMARY_ONLY, 1},
    {"fields.vector_params.dimension", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_MINMAX, DISPLAY_MINMAX, DIFF_NONE, 0, FROM_PRIMARY_ONLY, 1},
    {"fields.vector_params.maximum_edges", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_MINMAX, DISPLAY_MINMAX, DIFF_NONE, 0, FROM_PRIMARY_ONLY, 1},
    {"fields.vector_params.current_capacity", exact_field_matcher, {PARSE_INTEGER, NULL},
     AGG_SUM, DISPLAY_INTEGER, DIFF_NONE, 0, FROM_PRIMARY_ONLY, 1}
};


infoFieldType info_fields[] = {
    /* Memory */
    {"used_memory", exact_field_matcher, {PARSE_MEMORY, NULL},
     AGG_SUM, DISPLAY_MEMORY_MB, DIFF_NONE, 1, FROM_PRIMARY_ONLY, 1},
    
    /* FT.SEARCH command stats */
    {"cmdstat_FT.SEARCH", prefix_field_matcher, {PARSE_CMDSTATS, "calls"},
     AGG_SUM, DISPLAY_INTEGER, DIFF_RATE_COUNT, 1, FROM_ALL, 0},
    {"cmdstat_FT.SEARCH", prefix_field_matcher, {PARSE_CMDSTATS, "usec"},
     AGG_SUM, DISPLAY_INTEGER, DIFF_RATE_MICROSEC, 1, FROM_ALL, 0},
    {"cmdstat_FT.SEARCH", prefix_field_matcher, {PARSE_CMDSTATS, "usec_per_call"},
     AGG_AVERAGE, DISPLAY_INTEGER, DIFF_NONE, 1, FROM_ALL, 0},
    {"cmdstat_FT.SEARCH", prefix_field_matcher, {PARSE_CMDSTATS, "rejected"},
     AGG_SUM, DISPLAY_INTEGER, DIFF_RATE_COUNT, 1, FROM_ALL, 0},
    {"cmdstat_FT.SEARCH", prefix_field_matcher, {PARSE_CMDSTATS, "failed"},
     AGG_SUM, DISPLAY_INTEGER, DIFF_RATE_COUNT, 1, FROM_ALL, 1},
    
    /* HSET command stats */
    {"cmdstat_hset", prefix_field_matcher, {PARSE_CMDSTATS, "calls"},
     AGG_SUM, DISPLAY_INTEGER, DIFF_RATE_COUNT, 1, FROM_PRIMARY_ONLY, 0},
    {"cmdstat_hset", prefix_field_matcher, {PARSE_CMDSTATS, "usec"},
     AGG_SUM, DISPLAY_INTEGER, DIFF_RATE_MICROSEC, 1, FROM_PRIMARY_ONLY, 0},
    {"cmdstat_hset", prefix_field_matcher, {PARSE_CMDSTATS, "usec_per_call"},
     AGG_AVERAGE, DISPLAY_INTEGER, DIFF_NONE, 1, FROM_PRIMARY_ONLY, 0},
    {"cmdstat_hset", prefix_field_matcher, {PARSE_CMDSTATS, "rejected"},
     AGG_SUM, DISPLAY_INTEGER, DIFF_RATE_COUNT, 1, FROM_PRIMARY_ONLY, 0},
    {"cmdstat_hset", prefix_field_matcher, {PARSE_CMDSTATS, "failed"},
     AGG_SUM, DISPLAY_INTEGER, DIFF_RATE_COUNT, 1, FROM_PRIMARY_ONLY, 1}
};

/*

(const char *command, 
                                        int num_fields, infoFieldType *fields, 
                                        int cluster_node_count, clusterNode **cluster_nodes,
                                        valkeyConnectionType ct, int print_info)
*/
// getMemoryInfoClusterGeneric();
void getFullInfo(const char *index_name, 
                int cluster_node_count, clusterNode **cluster_nodes,
                enum valkeyConnectionType ct) {
    // long long search_memory = 0;
    // long long search_reclaimable = 0;
    // long long search_total_docs = 0;
    // long long search_ingest_field_vector = 0;
    // printf("\n------>\n");
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "FT.INFO %s", index_name);
    int ftinfo_num_fields = sizeof(ftinfo_fields) / sizeof(ftinfo_fields[0]);
    clusterSnapshot* ftinfo_snapshot = createClusterSnapshot(cmd, ftinfo_num_fields, ftinfo_fields, 
                                                              cluster_node_count, cluster_nodes, ct, 0);

    int search_num_fields = sizeof(search_info_fields) / sizeof(search_info_fields[0]);
    clusterSnapshot* search_info_snapshot = createClusterSnapshot("INFO SEARCH", search_num_fields, search_info_fields, 
                                                                   cluster_node_count, cluster_nodes, ct, 0);

    int info_num_fields = sizeof(info_fields) / sizeof(info_fields[0]);
    clusterSnapshot* info_snapshot = createClusterSnapshot("INFO ALL", info_num_fields, info_fields, 
                                                            cluster_node_count, cluster_nodes, ct, 0);

    freeClusterSnapshot(ftinfo_snapshot);
    freeClusterSnapshot(search_info_snapshot);
    freeClusterSnapshot(info_snapshot);
    // printf("------>\n");
}

clusterSnapshot* getSearchInfo(int cluster_node_count, clusterNode **cluster_nodes,
                                enum valkeyConnectionType ct,
                                long long *search_memory, long long *search_reclaimable, 
                                long long *search_total_docs, long long *search_ingest_field_vector, 
                                long long *search_background_indexing_status) {
    int num_fields = sizeof(search_info_fields) / sizeof(search_info_fields[0]);
    clusterSnapshot* info_snapshot = createClusterSnapshot("INFO SEARCH", num_fields, search_info_fields, 
                                                            cluster_node_count, cluster_nodes, ct, 0);
    /* Initialize aggregated values */
    *search_memory = 0;
    *search_reclaimable = 0;
    *search_total_docs = 0;
    *search_ingest_field_vector = 0;
    *search_background_indexing_status = 0;
    assert(info_snapshot);    
    for (int i = 0; i < info_snapshot->num_fields; i++) {
        if (info_snapshot->fields[i].valid) {
            if (strcmp(info_snapshot->fields[i].field_name, "search_used_memory_bytes") == 0) {
                *search_memory = info_snapshot->fields[i].value;
            } else if (strcmp(info_snapshot->fields[i].field_name, "search_index_reclaimable_memory") == 0) {
                *search_reclaimable = info_snapshot->fields[i].value;
            } else if (strcmp(info_snapshot->fields[i].field_name, "search_total_indexed_documents") == 0) {
                *search_total_docs = info_snapshot->fields[i].value;
            } else if (strcmp(info_snapshot->fields[i].field_name, "search_ingest_field_vector") == 0) {
                *search_ingest_field_vector = info_snapshot->fields[i].value;
            } else if (strcmp(info_snapshot->fields[i].field_name, "search_background_indexing_status") == 0) {
                *search_background_indexing_status = info_snapshot->fields[i].value;
            }
            // printf("> %s:%lld\n", info_snapshot->fields[i].field_name, info_snapshot->fields[i].value);
        }

    }
    return info_snapshot;
}

clusterSnapshot* getInfoCluster(int cluster_node_count, clusterNode **cluster_nodes,
                                enum valkeyConnectionType ct) {
    int num_fields = sizeof(info_fields) / sizeof(info_fields[0]);
    clusterSnapshot* info_snapshot = createClusterSnapshot("INFO ALL", num_fields, info_fields, 
                                                            cluster_node_count, cluster_nodes, ct, 0);
    // print the values in the snapshot
    assert(info_snapshot); 
    return info_snapshot;
}

/* Example 4: Get current FT.INFO statistics */
clusterSnapshot* getFtInfoStatistics(const char *index_name,
                                      int cluster_node_count, clusterNode **cluster_nodes,
                                      enum valkeyConnectionType ct) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "FT.INFO %s", index_name);
    int num_fields = sizeof(ftinfo_fields) / sizeof(ftinfo_fields[0]);
    // getAggregatedClusterStats(cmd, num_fields, ftinfo_fields);
    clusterSnapshot* info_snapshot = createClusterSnapshot(cmd, num_fields, ftinfo_fields, 
                                                            cluster_node_count, cluster_nodes, ct, 0);
    assert(info_snapshot); 
    // for (int i = 0; i < info_snapshot->num_fields; i++) {
    //     if (info_snapshot->fields[i].valid) {
    //         printf("> %s:%lld", info_snapshot->fields[i].field_name, info_snapshot->fields[i].value);
    //     }
    // }
    return info_snapshot;
}

void* compareInfoSnapshots(int cluster_node_count, clusterNode **cluster_nodes,
                            enum valkeyConnectionType ct, 
                            clusterSnapshot *old_infoall, clusterSnapshot *new_snap_infoall, 
                            clusterSnapshot *old_ftinfo, clusterSnapshot *new_snap_ftinfo, 
                            clusterSnapshot *old_infosearch, clusterSnapshot *new_snap_infosearch) {
    int ftinfo_num_fields = sizeof(ftinfo_fields) / sizeof(ftinfo_fields[0]);
    int search_num_fields = sizeof(search_info_fields) / sizeof(search_info_fields[0]);
    int info_num_fields = sizeof(info_fields) / sizeof(info_fields[0]);
    compareClusterSnapshots(old_infoall, new_snap_infoall, info_num_fields, info_fields);
    compareClusterSnapshots(old_ftinfo, new_snap_ftinfo, ftinfo_num_fields, ftinfo_fields);
    compareClusterSnapshots(old_infosearch, new_snap_infosearch, search_num_fields, search_info_fields);
    return NULL;
}

/* ============================================================================
 * Runtime Configuration Management
 * 
 * Allows applying server-side configurations before running benchmarks.
 * Configurations are read from a simple .conf file with key=value pairs.
 * ============================================================================ */

#define RUNTIME_CONFIG_INITIAL_CAPACITY 16
#define MAX_CONFIG_LINE_LENGTH 4096

/* Load runtime configuration from file */
runtimeConfigContext* loadRuntimeConfig(const char *config_file) {
    if (!config_file) {
        return NULL;
    }

    FILE *fp = fopen(config_file, "r");
    if (!fp) {
        fprintf(stderr, "Warning: Could not open runtime config file: %s\n", config_file);
        return NULL;
    }

    runtimeConfigContext *ctx = zmalloc(sizeof(runtimeConfigContext));
    ctx->entries = zmalloc(sizeof(runtimeConfigEntry) * RUNTIME_CONFIG_INITIAL_CAPACITY);
    ctx->num_entries = 0;
    ctx->capacity = RUNTIME_CONFIG_INITIAL_CAPACITY;
    ctx->applied = 0;

    char line[MAX_CONFIG_LINE_LENGTH];
    int line_num = 0;

    while (fgets(line, sizeof(line), fp)) {
        line_num++;
        
        /* Remove newline */
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') {
            line[len-1] = '\0';
            len--;
        }
        if (len > 0 && line[len-1] == '\r') {
            line[len-1] = '\0';
            len--;
        }

        /* Skip empty lines and comments */
        char *p = line;
        while (*p && (*p == ' ' || *p == '\t')) p++;
        if (*p == '\0' || *p == '#') {
            continue;
        }

        /* Find the separator (space or =) */
        char *key_start = p;
        char *sep = p;
        while (*sep && *sep != ' ' && *sep != '\t' && *sep != '=') sep++;
        if (*sep == '\0') {
            fprintf(stderr, "Warning: Invalid config line %d (no value): %s\n", line_num, line);
            continue;
        }

        /* Extract key */
        size_t key_len = sep - key_start;
        char *key = zmalloc(key_len + 1);
        memcpy(key, key_start, key_len);
        key[key_len] = '\0';

        /* Skip separator(s) */
        while (*sep && (*sep == ' ' || *sep == '\t' || *sep == '=')) sep++;
        if (*sep == '\0') {
            fprintf(stderr, "Warning: Invalid config line %d (empty value): %s\n", line_num, line);
            zfree(key);
            continue;
        }

        /* Extract value (rest of line, trim trailing whitespace) */
        char *value_start = sep;
        char *value_end = line + len - 1;
        while (value_end > value_start && (*value_end == ' ' || *value_end == '\t')) {
            value_end--;
        }
        size_t value_len = value_end - value_start + 1;
        char *value = zmalloc(value_len + 1);
        memcpy(value, value_start, value_len);
        value[value_len] = '\0';

        /* Expand capacity if needed */
        if (ctx->num_entries >= ctx->capacity) {
            ctx->capacity *= 2;
            ctx->entries = zrealloc(ctx->entries, sizeof(runtimeConfigEntry) * ctx->capacity);
        }

        /* Store entry */
        ctx->entries[ctx->num_entries].key = key;
        ctx->entries[ctx->num_entries].value = value;
        ctx->entries[ctx->num_entries].original_value = NULL;
        ctx->num_entries++;
    }

    fclose(fp);

    if (ctx->num_entries == 0) {
        fprintf(stderr, "Warning: No valid configuration entries found in %s\n", config_file);
        freeRuntimeConfig(ctx);
        return NULL;
    }

    return ctx;
}

/* Apply runtime configuration to a single node */
static int applyConfigToNode(runtimeConfigContext *ctx, valkeyContext *node_ctx, 
                              const char *node_name, int verbose) {
    int success_count = 0;
    int error_count = 0;

    for (int i = 0; i < ctx->num_entries; i++) {
        runtimeConfigEntry *entry = &ctx->entries[i];
        
        /* Get current value first (for restoration later) */
        if (!entry->original_value) {
            valkeyReply *get_reply = valkeyCommand(node_ctx, "CONFIG GET %s", entry->key);
            if (get_reply && get_reply->type == VALKEY_REPLY_ARRAY && get_reply->elements == 2) {
                /* CONFIG GET returns [key, value] */
                if (get_reply->element[1]->type == VALKEY_REPLY_STRING) {
                    entry->original_value = sdsnew(get_reply->element[1]->str);
                }
            }
            if (get_reply) freeReplyObject(get_reply);
        }

        /* Apply new configuration */
        valkeyReply *reply = valkeyCommand(node_ctx, "CONFIG SET %s %s", 
                                          entry->key, entry->value);
        
        if (!reply) {
            fprintf(stderr, "Error: Failed to set config %s on %s: Connection error\n",
                    entry->key, node_name);
            error_count++;
            continue;
        }

        if (reply->type == VALKEY_REPLY_ERROR) {
            fprintf(stderr, "Error: Failed to set config %s=%s on %s: %s\n",
                    entry->key, entry->value, node_name, reply->str);
            error_count++;
        } else {
            if (verbose) {
                printf("✓ Set %s = %s on %s", entry->key, entry->value, node_name);
                if (entry->original_value) {
                    printf(" (was: %s)", entry->original_value);
                }
                printf("\n");
            }
            success_count++;
        }

        freeReplyObject(reply);
    }

    if (error_count > 0) {
        fprintf(stderr, "Warning: %d/%d configurations failed on %s\n", 
                error_count, ctx->num_entries, node_name);
    }

    return success_count;
}

/* Apply runtime configuration to server(s) */
int applyRuntimeConfig(runtimeConfigContext *ctx, 
                       int cluster_node_count, 
                       clusterNode **cluster_nodes,
                       enum valkeyConnectionType ct,
                       int verbose) {
    if (!ctx || ctx->num_entries == 0) {
        return 0;
    }

    if (verbose) {
        printf("\n=== Applying Runtime Configuration ===\n");
        printf("Configuration entries: %d\n", ctx->num_entries);
    }

    int total_applied = 0;

    if (cluster_node_count > 0 && cluster_nodes) {
        /* Apply to all cluster nodes */
        for (int i = 0; i < cluster_node_count; i++) {
            if (!cluster_nodes[i] || !cluster_nodes[i]->ctx) {
                continue;
            }
            
            char node_name[256];
            snprintf(node_name, sizeof(node_name), "%s:%d", 
                     cluster_nodes[i]->ip, cluster_nodes[i]->port);
            
            int applied = applyConfigToNode(ctx, cluster_nodes[i]->ctx, node_name, verbose);
            total_applied += applied;
        }
    } else {
        /* Single node mode - would need to be passed separately */
        fprintf(stderr, "Warning: No cluster nodes provided for config application\n");
        return -1;
    }

    ctx->applied = 1;

    if (verbose) {
        printf("Total configurations applied: %d\n", total_applied);
        printf("=====================================\n\n");
    }

    return total_applied;
}

/* Restore configuration on a single node */
static int restoreConfigOnNode(runtimeConfigContext *ctx, valkeyContext *node_ctx,
                                const char *node_name, int verbose) {
    int success_count = 0;
    int error_count = 0;

    for (int i = 0; i < ctx->num_entries; i++) {
        runtimeConfigEntry *entry = &ctx->entries[i];
        
        if (!entry->original_value) {
            /* No original value saved, skip restoration */
            continue;
        }

        valkeyReply *reply = valkeyCommand(node_ctx, "CONFIG SET %s %s",
                                          entry->key, entry->original_value);
        
        if (!reply) {
            fprintf(stderr, "Error: Failed to restore config %s on %s: Connection error\n",
                    entry->key, node_name);
            error_count++;
            continue;
        }

        if (reply->type == VALKEY_REPLY_ERROR) {
            fprintf(stderr, "Error: Failed to restore config %s=%s on %s: %s\n",
                    entry->key, entry->original_value, node_name, reply->str);
            error_count++;
        } else {
            if (verbose) {
                printf("✓ Restored %s = %s on %s\n", 
                       entry->key, entry->original_value, node_name);
            }
            success_count++;
        }

        freeReplyObject(reply);
    }

    return success_count;
}

/* Restore original configuration */
int restoreRuntimeConfig(runtimeConfigContext *ctx,
                         int cluster_node_count,
                         clusterNode **cluster_nodes,
                         enum valkeyConnectionType ct,
                         int verbose) {
    if (!ctx || !ctx->applied) {
        return 0;
    }

    if (verbose) {
        printf("\n=== Restoring Original Configuration ===\n");
    }

    int total_restored = 0;

    if (cluster_node_count > 0 && cluster_nodes) {
        for (int i = 0; i < cluster_node_count; i++) {
            if (!cluster_nodes[i] || !cluster_nodes[i]->ctx) {
                continue;
            }
            
            char node_name[256];
            snprintf(node_name, sizeof(node_name), "%s:%d",
                     cluster_nodes[i]->ip, cluster_nodes[i]->port);
            
            int restored = restoreConfigOnNode(ctx, cluster_nodes[i]->ctx, node_name, verbose);
            total_restored += restored;
        }
    }

    ctx->applied = 0;

    if (verbose) {
        printf("Total configurations restored: %d\n", total_restored);
        printf("========================================\n\n");
    }

    return total_restored;
}

/* Free runtime configuration context */
void freeRuntimeConfig(runtimeConfigContext *ctx) {
    if (!ctx) {
        return;
    }

    for (int i = 0; i < ctx->num_entries; i++) {
        if (ctx->entries[i].key) {
            zfree(ctx->entries[i].key);
        }
        if (ctx->entries[i].value) {
            zfree(ctx->entries[i].value);
        }
        if (ctx->entries[i].original_value) {
            sdsfree(ctx->entries[i].original_value);
        }
    }

    if (ctx->entries) {
        zfree(ctx->entries);
    }

    zfree(ctx);
}