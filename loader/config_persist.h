#ifndef CONFIG_PERSIST_H
#define CONFIG_PERSIST_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

typedef struct {
    /* Basic benchmark parameters */
    int num_clients;
    int num_threads;
    int pipeline;
    int requests;
    int keyspacelen;
    int dbnum;
    int csv;
    int loop;
    int idlemode;
    int keepalive;
    int precision;
    int cluster_mode;
    int resp3;

    /* Search parameters */
    char *dataset;
    char *search_name;
    char *search_algorithm;
    char *search_prefix;
    char *vector_field;
    int vector_dim;
    char *tag_field;
    char *numeric_field;
    char *search_tags;
    char *tag_filter;
    int ef_search;
    int ef_construction;
    int m;
    int k;
    char *metric;
    int nocontent;
    int localonly;
    int use_filtered_search;

    /* Optimizer parameters */
    int optimize_enabled;
    char *optimize_objective;
    char *optimize_csv_file;
    int optimize_max_iterations;
    int optimize_min_requests;

    /* Authentication parameters */
    char *auth;
    char *user;

    /* TLS parameters */
    char *tls_cert;
    char *tls_key;
    char *tls_cacert;
    char *tls_cacertdir;
    int tls_skip_verify;
    char *sni;

    time_t last_updated;
} persisted_config_t;

typedef struct {
    bool save_config;
    bool no_save_config;
    bool clear_config;
    bool show_config;
} config_control_flags_t;

int config_persist_init(void);
char *config_persist_get_path(void);
int config_persist_load(persisted_config_t *config);
int config_persist_save(const persisted_config_t *config);
int config_persist_clear(void);
void config_persist_show(const persisted_config_t *config, int verbose);
void config_persist_free(persisted_config_t *config);
int config_persist_merge(persisted_config_t *base, const persisted_config_t *override);

#endif