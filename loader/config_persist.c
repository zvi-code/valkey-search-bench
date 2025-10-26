/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2024-present, Zvi Schneider
 * 
 * Persisted configuration handling for valkey-search-benchmark.
 * 
 * This file is part of valkey-search-benchmark and is licensed under the
 * BSD 3-Clause License. See the LICENSE file in the root directory.
 */
 #include "config_persist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <pwd.h>

#define CONFIG_FILE_NAME ".valkey-benchmark.conf"
#define GLOBAL_CONFIG_DIR ".valkey-benchmark"
#define CONFIG_VERSION "1.0"
#define MAX_LINE_LENGTH 1024

static char config_path[4096];

static int ensure_directory(const char *path) {
    struct stat st = {0};
    if (stat(path, &st) == -1) {
        if (mkdir(path, 0755) != 0) {
            return -1;
        }
    }
    return 0;
}

/* Sanitize session ID for safe filename usage */
static void sanitize_session_id(char *dest, size_t dest_size, const char *src) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dest_size - 1; i++) {
        char c = src[i];
        /* Replace special characters with underscores or hyphens */
        if ((c >= 'a' && c <= 'z') || 
            (c >= 'A' && c <= 'Z') || 
            (c >= '0' && c <= '9') || 
            c == '-' || c == '_') {
            dest[j++] = c;
        } else if (c == '%' || c == ' ' || c == '/' || c == '\\') {
            /* Skip or replace problematic characters */
            if (j > 0 && dest[j-1] != '-') {
                dest[j++] = '-';
            }
        }
    }
    dest[j] = '\0';
    
    /* Remove trailing dashes */
    while (j > 0 && dest[j-1] == '-') {
        dest[--j] = '\0';
    }
}

int config_persist_init(void) {
    config_path[0] = '\0';
    return 0;
}

char *config_persist_get_path(void) {
    if (config_path[0] != '\0') {
        return config_path;
    }

    char local_config[1024];
    char global_config[4096];
    char *session_id = getenv("VALKEY_BENCHMARK_SESSION");
    char auto_session_id[256] = {0};
    char sanitized_id[256] = {0};

    /* If no explicit session ID, try to auto-detect tmux session or terminal */
    if (!session_id) {
        /* Check for tmux session */
        char *tmux_pane = getenv("TMUX_PANE");
        if (tmux_pane) {
            /* Use tmux pane ID as session identifier - sanitize it */
            snprintf(auto_session_id, sizeof(auto_session_id), "tmux%s", tmux_pane);
            sanitize_session_id(sanitized_id, sizeof(sanitized_id), auto_session_id);
            session_id = sanitized_id;
        } else {
            /* Fall back to terminal session ID for unique identification */
            /* Try to get the controlling terminal's session ID */
            pid_t sid = getsid(0);
            if (sid > 0) {
                snprintf(auto_session_id, sizeof(auto_session_id), "term%d", sid);
                session_id = auto_session_id;
            } else {
                /* Last resort: use parent process ID */
                snprintf(auto_session_id, sizeof(auto_session_id), "term%d", getppid());
                session_id = auto_session_id;
            }
        }
    } else {
        /* Sanitize user-provided session ID too */
        sanitize_session_id(sanitized_id, sizeof(sanitized_id), session_id);
        session_id = sanitized_id;
    }

    /* Build config filenames based on session */
    if (session_id && session_id[0] != '\0') {
        snprintf(local_config, sizeof(local_config), ".valkey-benchmark-%s.conf", session_id);
    } else {
        snprintf(local_config, sizeof(local_config), "%s", CONFIG_FILE_NAME);
    }

    /* Priority 1: Working directory (current directory) */
    if (access(local_config, F_OK) == 0) {
        snprintf(config_path, sizeof(config_path), "%s", local_config);
        return config_path;
    }

    /* Priority 2: User global directory */
    char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) {
            home = pw->pw_dir;
        }
    }

    if (home) {
        char dir_path[4096];
        snprintf(dir_path, sizeof(dir_path), "%s/%s", home, GLOBAL_CONFIG_DIR);

        if (session_id) {
            snprintf(global_config, sizeof(global_config), "config-%s.conf", session_id);
        } else {
            snprintf(global_config, sizeof(global_config), "config.conf");
        }

        if (ensure_directory(dir_path) == 0) {
            int ret = snprintf(config_path, sizeof(config_path), "%s/%s", dir_path, global_config);
            if (ret >= sizeof(config_path)) {
                /* Path too long, fallback to local config */
                snprintf(config_path, sizeof(config_path), "%s", local_config);
            }
            return config_path;
        }
    }

    /* Fallback: Use local config in current directory */
    snprintf(config_path, sizeof(config_path), "%s", local_config);
    return config_path;
}

static char *strdup_safe(const char *s) {
    if (!s) return NULL;
    return strdup(s);
}

static void set_string_field(char **field, const char *value) {
    if (*field) {
        free(*field);
    }
    *field = strdup_safe(value);
}

int config_persist_load(persisted_config_t *config) {
    char *path = config_persist_get_path();
    FILE *fp = fopen(path, "r");
    if (!fp) {
        if (errno == ENOENT) {
            return 0;
        }
        return -1;
    }

    memset(config, 0, sizeof(persisted_config_t));

    char line[MAX_LINE_LENGTH];
    while (fgets(line, sizeof(line), fp)) {
        char key[256], value[768];

        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }

        if (sscanf(line, "%255s %767[^\n]", key, value) != 2) {
            continue;
        }

        if (strcmp(key, "version") == 0) {
            continue;
        } else if (strcmp(key, "last_updated") == 0) {
            config->last_updated = (time_t)atol(value);
        } else if (strcmp(key, "num_clients") == 0) {
            config->num_clients = atoi(value);
        } else if (strcmp(key, "num_threads") == 0) {
            config->num_threads = atoi(value);
        } else if (strcmp(key, "pipeline") == 0) {
            config->pipeline = atoi(value);
        } else if (strcmp(key, "requests") == 0) {
            config->requests = atoi(value);
        } else if (strcmp(key, "keyspacelen") == 0) {
            config->keyspacelen = atoi(value);
        } else if (strcmp(key, "dbnum") == 0) {
            config->dbnum = atoi(value);
        } else if (strcmp(key, "csv") == 0) {
            config->csv = atoi(value);
        } else if (strcmp(key, "loop") == 0) {
            config->loop = atoi(value);
        } else if (strcmp(key, "idlemode") == 0) {
            config->idlemode = atoi(value);
        } else if (strcmp(key, "keepalive") == 0) {
            config->keepalive = atoi(value);
        } else if (strcmp(key, "precision") == 0) {
            config->precision = atoi(value);
        } else if (strcmp(key, "resp3") == 0) {
            config->resp3 = atoi(value);
        } else if (strcmp(key, "dataset") == 0) {
            set_string_field(&config->dataset, value);
        } else if (strcmp(key, "search_name") == 0) {
            set_string_field(&config->search_name, value);
        } else if (strcmp(key, "search_algorithm") == 0) {
            set_string_field(&config->search_algorithm, value);
        } else if (strcmp(key, "search_prefix") == 0) {
            set_string_field(&config->search_prefix, value);
        } else if (strcmp(key, "vector_field") == 0) {
            set_string_field(&config->vector_field, value);
        } else if (strcmp(key, "vector_dim") == 0) {
            config->vector_dim = atoi(value);
        } else if (strcmp(key, "tag_field") == 0) {
            set_string_field(&config->tag_field, value);
        } else if (strcmp(key, "numeric_field") == 0) {
            set_string_field(&config->numeric_field, value);
        } else if (strcmp(key, "search_tags") == 0) {
            set_string_field(&config->search_tags, value);
        } else if (strcmp(key, "tag_filter") == 0) {
            set_string_field(&config->tag_filter, value);
        } else if (strcmp(key, "ef_search") == 0) {
            config->ef_search = atoi(value);
        } else if (strcmp(key, "ef_construction") == 0) {
            config->ef_construction = atoi(value);
        } else if (strcmp(key, "m") == 0) {
            config->m = atoi(value);
        } else if (strcmp(key, "k") == 0) {
            config->k = atoi(value);
        } else if (strcmp(key, "metric") == 0) {
            set_string_field(&config->metric, value);
        } else if (strcmp(key, "nocontent") == 0) {
            config->nocontent = atoi(value);
        } else if (strcmp(key, "localonly") == 0) {
            config->localonly = atoi(value);
        } else if (strcmp(key, "use_filtered_search") == 0) {
            config->use_filtered_search = atoi(value);
        } else if (strcmp(key, "optimize_objective") == 0) {
            set_string_field(&config->optimize_objective, value);
        } else if (strcmp(key, "optimize_csv_file") == 0) {
            set_string_field(&config->optimize_csv_file, value);
        } else if (strcmp(key, "optimize_max_iterations") == 0) {
            config->optimize_max_iterations = atoi(value);
        } else if (strcmp(key, "optimize_min_requests") == 0) {
            config->optimize_min_requests = atoi(value);
        } else if (strcmp(key, "optimize_client_range") == 0) {
            set_string_field(&config->optimize_client_range, value);
        } else if (strcmp(key, "optimize_thread_range") == 0) {
            set_string_field(&config->optimize_thread_range, value);
        } else if (strcmp(key, "optimize_ef_search_range") == 0) {
            set_string_field(&config->optimize_ef_search_range, value);
        } else if (strcmp(key, "optimize_pipeline_range") == 0) {
            set_string_field(&config->optimize_pipeline_range, value);
        } else if (strcmp(key, "tls_cert") == 0) {
            set_string_field(&config->tls_cert, value);
        } else if (strcmp(key, "tls_key") == 0) {
            set_string_field(&config->tls_key, value);
        } else if (strcmp(key, "tls_cacert") == 0) {
            set_string_field(&config->tls_cacert, value);
        } else if (strcmp(key, "tls_cacertdir") == 0) {
            set_string_field(&config->tls_cacertdir, value);
        } else if (strcmp(key, "tls_skip_verify") == 0) {
            config->tls_skip_verify = atoi(value);
        } else if (strcmp(key, "sni") == 0) {
            set_string_field(&config->sni, value);
        } else if (strcmp(key, "auth") == 0) {
            set_string_field(&config->auth, value);
        } else if (strcmp(key, "user") == 0) {
            set_string_field(&config->user, value);
        }
    }

    fclose(fp);
    return 0;
}

int config_persist_save(const persisted_config_t *config) {
    char *path = config_persist_get_path();
    FILE *fp = fopen(path, "w");
    if (!fp) {
        return -1;
    }

    fprintf(fp, "# Valkey Benchmark Configuration\n");
    fprintf(fp, "version %s\n", CONFIG_VERSION);
    fprintf(fp, "last_updated %ld\n", time(NULL));
    fprintf(fp, "\n");

    /* Basic benchmark parameters */
    if (config->num_clients > 0) {
        fprintf(fp, "num_clients %d\n", config->num_clients);
    }
    if (config->num_threads > 0) {
        fprintf(fp, "num_threads %d\n", config->num_threads);
    }
    if (config->pipeline > 0) {
        fprintf(fp, "pipeline %d\n", config->pipeline);
    }
    if (config->requests > 0) {
        fprintf(fp, "requests %d\n", config->requests);
    }
    if (config->keyspacelen > 0) {
        fprintf(fp, "keyspacelen %d\n", config->keyspacelen);
    }
    if (config->dbnum > 0) {
        fprintf(fp, "dbnum %d\n", config->dbnum);
    }
    if (config->csv) {
        fprintf(fp, "csv %d\n", config->csv);
    }
    if (config->loop) {
        fprintf(fp, "loop %d\n", config->loop);
    }
    if (config->idlemode) {
        fprintf(fp, "idlemode %d\n", config->idlemode);
    }
    if (config->keepalive > 0) {
        fprintf(fp, "keepalive %d\n", config->keepalive);
    }
    if (config->precision > 0) {
        fprintf(fp, "precision %d\n", config->precision);
    }
    if (config->resp3) {
        fprintf(fp, "resp3 %d\n", config->resp3);
    }

    /* Search parameters */
    if (config->dataset) {
        fprintf(fp, "dataset %s\n", config->dataset);
    }
    if (config->search_name) {
        fprintf(fp, "search_name %s\n", config->search_name);
    }
    if (config->search_algorithm) {
        fprintf(fp, "search_algorithm %s\n", config->search_algorithm);
    }
    if (config->search_prefix) {
        fprintf(fp, "search_prefix %s\n", config->search_prefix);
    }
    if (config->vector_field) {
        fprintf(fp, "vector_field %s\n", config->vector_field);
    }
    if (config->vector_dim > 0) {
        fprintf(fp, "vector_dim %d\n", config->vector_dim);
    }
    if (config->tag_field) {
        fprintf(fp, "tag_field %s\n", config->tag_field);
    }
    if (config->numeric_field) {
        fprintf(fp, "numeric_field %s\n", config->numeric_field);
    }
    if (config->search_tags) {
        fprintf(fp, "search_tags %s\n", config->search_tags);
    }
    if (config->tag_filter) {
        fprintf(fp, "tag_filter %s\n", config->tag_filter);
    }
    if (config->ef_search > 0) {
        fprintf(fp, "ef_search %d\n", config->ef_search);
    }
    if (config->ef_construction > 0) {
        fprintf(fp, "ef_construction %d\n", config->ef_construction);
    }
    if (config->m > 0) {
        fprintf(fp, "m %d\n", config->m);
    }
    if (config->k > 0) {
        fprintf(fp, "k %d\n", config->k);
    }
    if (config->metric) {
        fprintf(fp, "metric %s\n", config->metric);
    }
    if (config->nocontent) {
        fprintf(fp, "nocontent %d\n", config->nocontent);
    }
    if (config->localonly) {
        fprintf(fp, "localonly %d\n", config->localonly);
    }
    if (config->use_filtered_search) {
        fprintf(fp, "use_filtered_search %d\n", config->use_filtered_search);
    }

    /* Optimizer parameters */
    if (config->optimize_objective) {
        fprintf(fp, "optimize_objective %s\n", config->optimize_objective);
    }
    if (config->optimize_csv_file) {
        fprintf(fp, "optimize_csv_file %s\n", config->optimize_csv_file);
    }
    if (config->optimize_max_iterations > 0) {
        fprintf(fp, "optimize_max_iterations %d\n", config->optimize_max_iterations);
    }
    if (config->optimize_min_requests > 0) {
        fprintf(fp, "optimize_min_requests %d\n", config->optimize_min_requests);
    }
    if (config->optimize_client_range) {
        fprintf(fp, "optimize_client_range %s\n", config->optimize_client_range);
    }
    if (config->optimize_thread_range) {
        fprintf(fp, "optimize_thread_range %s\n", config->optimize_thread_range);
    }
    if (config->optimize_ef_search_range) {
        fprintf(fp, "optimize_ef_search_range %s\n", config->optimize_ef_search_range);
    }
    if (config->optimize_pipeline_range) {
        fprintf(fp, "optimize_pipeline_range %s\n", config->optimize_pipeline_range);
    }

    /* Authentication parameters */
    if (config->tls_cert) {
        fprintf(fp, "tls_cert %s\n", config->tls_cert);
    }
    if (config->tls_key) {
        fprintf(fp, "tls_key %s\n", config->tls_key);
    }
    if (config->tls_cacert) {
        fprintf(fp, "tls_cacert %s\n", config->tls_cacert);
    }
    if (config->tls_cacertdir) {
        fprintf(fp, "tls_cacertdir %s\n", config->tls_cacertdir);
    }
    if (config->tls_skip_verify) {
        fprintf(fp, "tls_skip_verify %d\n", config->tls_skip_verify);
    }
    if (config->sni) {
        fprintf(fp, "sni %s\n", config->sni);
    }
    if (config->auth) {
        fprintf(fp, "auth %s\n", config->auth);
    }
    if (config->user) {
        fprintf(fp, "user %s\n", config->user);
    }

    fclose(fp);
    return 0;
}

int config_persist_clear(void) {
    char *path = config_persist_get_path();
    if (unlink(path) != 0) {
        if (errno != ENOENT) {
            return -1;
        }
    }
    return 0;
}

void config_persist_show(const persisted_config_t *config, int verbose) {
    printf("Configuration File: %s\n", config_persist_get_path());

    if (!config) {
        printf("No configuration loaded.\n");
        return;
    }

    printf("\n=== Saved Configuration ===\n");

    if (config->last_updated > 0) {
        char time_str[64];
        struct tm *tm_info = localtime(&config->last_updated);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
        printf("Last Updated: %s\n", time_str);
    }

    /* Basic benchmark parameters */
    if (config->num_clients > 0) printf("num_clients: %d\n", config->num_clients);
    if (config->num_threads > 0) printf("num_threads: %d\n", config->num_threads);
    if (config->pipeline > 0) printf("pipeline: %d\n", config->pipeline);
    if (config->requests > 0) printf("requests: %d\n", config->requests);
    if (config->keyspacelen > 0) printf("keyspacelen: %d\n", config->keyspacelen);
    if (config->dbnum > 0) printf("dbnum: %d\n", config->dbnum);
    if (config->csv) printf("csv: %d\n", config->csv);
    if (config->loop) printf("loop: %d\n", config->loop);
    if (config->idlemode) printf("idlemode: %d\n", config->idlemode);
    if (config->keepalive > 0) printf("keepalive: %d\n", config->keepalive);
    if (config->precision > 0) printf("precision: %d\n", config->precision);
    if (config->resp3) printf("resp3: %d\n", config->resp3);

    /* Search parameters */
    if (config->dataset) printf("dataset: %s\n", config->dataset);
    if (config->search_name) printf("search_name: %s\n", config->search_name);
    if (config->search_algorithm) printf("search_algorithm: %s\n", config->search_algorithm);
    if (config->search_prefix) printf("search_prefix: %s\n", config->search_prefix);
    if (config->vector_field) printf("vector_field: %s\n", config->vector_field);
    if (config->vector_dim > 0) printf("vector_dim: %d\n", config->vector_dim);
    if (config->tag_field) printf("tag_field: %s\n", config->tag_field);
    if (config->numeric_field) printf("numeric_field: %s\n", config->numeric_field);
    if (config->search_tags) printf("search_tags: %s\n", config->search_tags);
    if (config->tag_filter) printf("tag_filter: %s\n", config->tag_filter);
    if (config->ef_search > 0) printf("ef_search: %d\n", config->ef_search);
    if (config->ef_construction > 0) printf("ef_construction: %d\n", config->ef_construction);
    if (config->m > 0) printf("m: %d\n", config->m);
    if (config->k > 0) printf("k: %d\n", config->k);
    if (config->metric) printf("metric: %s\n", config->metric);
    if (config->nocontent) printf("nocontent: %d\n", config->nocontent);
    if (config->localonly) printf("localonly: %d\n", config->localonly);
    if (config->use_filtered_search) printf("use_filtered_search: %d\n", config->use_filtered_search);

    /* Optimizer parameters */
    if (config->optimize_objective) printf("optimize_objective: %s\n", config->optimize_objective);
    if (config->optimize_csv_file) printf("optimize_csv_file: %s\n", config->optimize_csv_file);
    if (config->optimize_max_iterations > 0) printf("optimize_max_iterations: %d\n", config->optimize_max_iterations);
    if (config->optimize_min_requests > 0) printf("optimize_min_requests: %d\n", config->optimize_min_requests);
    if (config->optimize_client_range) printf("optimize_client_range: %s\n", config->optimize_client_range);
    if (config->optimize_thread_range) printf("optimize_thread_range: %s\n", config->optimize_thread_range);
    if (config->optimize_ef_search_range) printf("optimize_ef_search_range: %s\n", config->optimize_ef_search_range);
    if (config->optimize_pipeline_range) printf("optimize_pipeline_range: %s\n", config->optimize_pipeline_range);

    if (verbose) {
        if (config->tls_cert) printf("tls_cert: %s\n", config->tls_cert);
        if (config->tls_key) printf("tls_key: %s\n", config->tls_key);
        if (config->tls_cacert) printf("tls_cacert: %s\n", config->tls_cacert);
        if (config->tls_cacertdir) printf("tls_cacertdir: %s\n", config->tls_cacertdir);
        if (config->tls_skip_verify) printf("tls_skip_verify: %d\n", config->tls_skip_verify);
        if (config->sni) printf("sni: %s\n", config->sni);
        if (config->user) printf("user: %s\n", config->user);
    }

    printf("===========================\n");
}

void config_persist_free(persisted_config_t *config) {
    if (!config) return;

    /* Free all string fields */
    free(config->dataset);
    free(config->search_name);
    free(config->search_algorithm);
    free(config->search_prefix);
    free(config->vector_field);
    free(config->tag_field);
    free(config->numeric_field);
    free(config->search_tags);
    free(config->tag_filter);
    free(config->metric);
    free(config->optimize_objective);
    free(config->optimize_csv_file);
    free(config->auth);
    free(config->user);
    free(config->tls_cert);
    free(config->tls_key);
    free(config->tls_cacert);
    free(config->tls_cacertdir);
    free(config->sni);

    memset(config, 0, sizeof(persisted_config_t));
}

int config_persist_merge(persisted_config_t *base, const persisted_config_t *override) {
    if (!base || !override) return -1;

    /* Basic benchmark parameters */
    if (override->num_clients > 0) base->num_clients = override->num_clients;
    if (override->num_threads > 0) base->num_threads = override->num_threads;
    if (override->pipeline > 0) base->pipeline = override->pipeline;
    if (override->requests > 0) base->requests = override->requests;
    if (override->keyspacelen > 0) base->keyspacelen = override->keyspacelen;
    if (override->dbnum > 0) base->dbnum = override->dbnum;
    if (override->csv) base->csv = override->csv;
    if (override->loop) base->loop = override->loop;
    if (override->idlemode) base->idlemode = override->idlemode;
    if (override->keepalive > 0) base->keepalive = override->keepalive;
    if (override->precision > 0) base->precision = override->precision;
    if (override->resp3) base->resp3 = override->resp3;

    /* Search parameters */
    if (override->dataset) set_string_field(&base->dataset, override->dataset);
    if (override->search_name) set_string_field(&base->search_name, override->search_name);
    if (override->search_algorithm) set_string_field(&base->search_algorithm, override->search_algorithm);
    if (override->search_prefix) set_string_field(&base->search_prefix, override->search_prefix);
    if (override->vector_field) set_string_field(&base->vector_field, override->vector_field);
    if (override->vector_dim > 0) base->vector_dim = override->vector_dim;
    if (override->tag_field) set_string_field(&base->tag_field, override->tag_field);
    if (override->numeric_field) set_string_field(&base->numeric_field, override->numeric_field);
    if (override->search_tags) set_string_field(&base->search_tags, override->search_tags);
    if (override->tag_filter) set_string_field(&base->tag_filter, override->tag_filter);
    if (override->ef_search > 0) base->ef_search = override->ef_search;
    if (override->ef_construction > 0) base->ef_construction = override->ef_construction;
    if (override->m > 0) base->m = override->m;
    if (override->k > 0) base->k = override->k;
    if (override->metric) set_string_field(&base->metric, override->metric);
    if (override->nocontent) base->nocontent = override->nocontent;
    if (override->localonly) base->localonly = override->localonly;
    if (override->use_filtered_search) base->use_filtered_search = override->use_filtered_search;

    /* Optimizer parameters */
    if (override->optimize_objective) set_string_field(&base->optimize_objective, override->optimize_objective);
    if (override->optimize_csv_file) set_string_field(&base->optimize_csv_file, override->optimize_csv_file);
    if (override->optimize_max_iterations > 0) base->optimize_max_iterations = override->optimize_max_iterations;
    if (override->optimize_min_requests > 0) base->optimize_min_requests = override->optimize_min_requests;
    if (override->optimize_client_range) set_string_field(&base->optimize_client_range, override->optimize_client_range);
    if (override->optimize_thread_range) set_string_field(&base->optimize_thread_range, override->optimize_thread_range);
    if (override->optimize_ef_search_range) set_string_field(&base->optimize_ef_search_range, override->optimize_ef_search_range);
    if (override->optimize_pipeline_range) set_string_field(&base->optimize_pipeline_range, override->optimize_pipeline_range);

    /* Authentication parameters */
    if (override->auth) set_string_field(&base->auth, override->auth);
    if (override->user) set_string_field(&base->user, override->user);

    /* TLS parameters */
    if (override->tls_cert) set_string_field(&base->tls_cert, override->tls_cert);
    if (override->tls_key) set_string_field(&base->tls_key, override->tls_key);
    if (override->tls_cacert) set_string_field(&base->tls_cacert, override->tls_cacert);
    if (override->tls_cacertdir) set_string_field(&base->tls_cacertdir, override->tls_cacertdir);
    if (override->tls_skip_verify) base->tls_skip_verify = override->tls_skip_verify;
    if (override->sni) set_string_field(&base->sni, override->sni);

    return 0;
}