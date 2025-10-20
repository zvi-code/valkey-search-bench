/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2024-present, Zvi Schneider
 * 
 * Adaptive Load Optimizer for Valkey Benchmark
 * 
 * OVERVIEW:
 * Automatically tunes benchmark parameters (clients, threads, pipeline, ef_search)
 * to maximize or minimize an objective metric (e.g., maximize QPS, minimize latency)
 * while satisfying hard constraints (e.g., recall >= 0.9, QPS <= max_rate).
 *
 * This file is part of valkey-search-benchmark and is licensed under the
 * BSD 3-Clause License. See the LICENSE file in the root directory.
 */

/*
 * 
 * PHASED OPTIMIZATION STRATEGY:
 * 
 * Phase 1 (FEASIBILITY): Find any configuration that satisfies all constraints
 *   - Uses exponential search (double resources each iteration)
 *   - Exits once valid configuration found
 * 
 * Phase 2 (RECALL): Optimize recall-affecting parameters (ef_search)
 *   - Only adjusts PARAM_GROUP_RECALL parameters
 *   - Continues until recall constraints satisfied
 *   - Uses large adaptive steps for fast boundary detection
 * 
 * Phase 3 (THROUGHPUT): Optimize throughput parameters (clients, threads, pipeline)
 *   - Only adjusts PARAM_GROUP_THROUGHPUT parameters
 *   - Maximizes QPS or minimizes latency
 *   - Independent from recall optimization (domain knowledge)
 * 
 * Phase 4 (HILL_CLIMB): Joint optimization of all parameters
 *   - Adjusts all PARAM_GROUP_MIXED parameters together
 *   - Fine-tunes interactions between parameter groups
 *   - Uses medium adaptive steps
 * 
 * Phase 5 (REFINEMENT): Coordinate descent for final precision
 *   - One parameter at a time optimization
 *   - Smallest step sizes for precise tuning
 *   - Exits when no further improvement possible
 * 
 * ADAPTIVE STEP SIZING:
 * - Initial: 4x base step size (rapid exploration)
 * - Decay: 0.9x per iteration in early phases, 0.85x in later phases
 * - Minimum: 1x base step size (never below configured minimum)
 * - Rationale: Large steps find boundaries fast, small steps converge precisely
 * 
 * GRADIENT-BASED PRIORITIZATION:
 * - Estimates ∂f/∂θᵢ for each parameter from measurement history
 * - Sorts parameters by |gradient| (impact magnitude)
 * - Updates high-impact parameters first
 * - Rationale: Biggest changes come from parameters with largest gradients
 * 
 * EARLY ABORT MECHANISM:
 * - Monitors performance degradation during benchmark runs
 * - Aborts if >5-10x worse than baseline for 3+ consecutive iterations
 * - Resets to best known feasible configuration
 * - Rationale: Don't waste time on hopeless configs (e.g., ef_search too high)
 * 
 * MATHEMATICAL MODEL:
 *   maximize/minimize f(θ) subject to g_i(θ) ≤ 0 for all i
 *   where θ ∈ ℝⁿ are parameters, f is objective, g_i are constraints
 * 
 * CONVERGENCE GUARANTEES:
 * - Monotonic improvement when feasible solution exists
 * - Exponential convergence rate: O(log(1/ε)) iterations
 * - Terminates when improvement < 2% for 5 consecutive iterations
 */
#ifndef LOAD_OPTIMIZER
#define LOAD_OPTIMIZER
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <assert.h>
#include <float.h>

/* ============================================================================
 * Type Definitions
 * ============================================================================ */

typedef enum {
    OBJECTIVE_MAXIMIZE,
    OBJECTIVE_MINIMIZE
} objective_type_t;

typedef enum {
    CONSTRAINT_LESS_THAN,
    CONSTRAINT_GREATER_THAN
} constraint_type_t;

typedef enum {
    METRIC_QPS,
    METRIC_AVG_LATENCY,
    METRIC_P90_LATENCY,
    METRIC_P99_LATENCY,
    METRIC_MAX_LATENCY,
    METRIC_P50_LATENCY,
    METRIC_P95_LATENCY,
    METRIC_MIN_LATENCY,
    METRIC_RECALL_AVG,
    METRIC_RECALL_MIN,
    METRIC_RECALL_MAX,
    METRIC_RECALL_PERFECT_PCT,  /* Percentage of queries with perfect recall */
    METRIC_RECALL_ZERO_PCT,     /* Percentage of queries with zero recall */
    METRIC_COUNT
} metric_t;

typedef enum {
    PHASE_INIT,           /* Initial setup - collect first measurement */
    PHASE_FEASIBILITY,    /* Finding any feasible solution - exponential resource growth */
    PHASE_RECALL,         /* Optimize recall-affecting parameters (ef_search) - domain-aware phase */
    PHASE_THROUGHPUT,     /* Optimize throughput parameters (clients, threads, pipeline) - domain-aware phase */
    PHASE_HILL_CLIMB,     /* Joint optimization of all parameters - gradient descent */
    PHASE_REFINEMENT,     /* Fine-tuning via coordinate descent - one param at a time */
    PHASE_CONVERGED       /* Optimization complete - no further improvement possible */
} optimization_phase_t;

typedef enum {
    STATUS_OK,                   /* Parameter update applied successfully */
    STATUS_WAIT_STABILIZATION,   /* Waiting for system to stabilize after parameter change */
    STATUS_CONVERGED,            /* Optimization converged - no further improvement possible */
    STATUS_NO_FEASIBLE,          /* Cannot find any configuration satisfying constraints */
    STATUS_ERROR                 /* Early abort - configuration is catastrophically poor */
} status_t;

typedef enum {
    PARAM_GROUP_RECALL,      /* Parameters that primarily affect recall (e.g., ef_search) - optimized in PHASE_RECALL */
    PARAM_GROUP_THROUGHPUT,  /* Parameters that primarily affect QPS/latency (clients, threads, pipeline) - optimized in PHASE_THROUGHPUT */
    PARAM_GROUP_MIXED        /* Parameters that affect both or unclassified - optimized in all phases */
} param_group_t;

/* 
 * Tunable parameter with adaptive step sizing
 * 
 * Fields:
 *   step_size: Base step size (configured at creation, never changes)
 *   adaptive_step_size: Current step size used during optimization
 *                       Starts at 4x base, decreases as converging
 *   gradient: Estimated ∂f/∂θᵢ from measurement history
 *   gradient_magnitude: |∂f/∂θᵢ| used to prioritize high-impact parameters
 *   group: RECALL/THROUGHPUT/MIXED - determines when parameter is adjusted
 */
typedef struct {
    const char *name;
    int min_val;
    int max_val;
    int step_size;           /* Base step size - configured minimum */
    int adaptive_step_size;  /* Current adaptive step size (4x → 1x base during optimization) */
    int current_val;
    int initial_val;         /* For reset between runs */
    double gradient;         /* Estimated ∂f/∂θᵢ from finite differences */
    double gradient_magnitude; /* |∂f/∂θᵢ| for sorting by impact */
    int last_update_iter;    /* Iteration number when parameter was last changed */
    bool is_tunable;         /* False if parameter is constraint-only (not optimized) */
    bool locked;             /* True if parameter is locked after its optimization phase */
    bool grid_searched;      /* True if parameter has completed grid search in THROUGHPUT phase */
    param_group_t group;     /* RECALL/THROUGHPUT/MIXED - determines optimization phase */
} param_t;

/* Constraint: metric ⋚ threshold */
typedef struct {
    metric_t metric;
    constraint_type_t type;
    double threshold;
    double violation;     /* Degree of violation (0 if satisfied) */
} constraint_t;

/* Optimization objective */
typedef struct {
    metric_t metric;
    objective_type_t type;
} objective_t;

/* Single benchmark measurement */
typedef struct {
    double metrics[METRIC_COUNT];
    int param_values[16];      /* Snapshot of parameters at measurement time */
    int num_params;
    double objective_score;    /* f(θ) normalized for maximization */
    bool constraints_satisfied;
    int iteration;
    time_t timestamp;
} measurement_t;

/* Main optimizer state */
typedef struct {
    /* Configuration */
    param_t *params;
    int num_params;
    constraint_t *constraints;
    int num_constraints;
    objective_t objective;
    
    /* Measurement history */
    measurement_t *history;
    int history_size;
    int history_capacity;
    
    /* Best solutions */
    measurement_t best_feasible;
    measurement_t best_ever;  /* Best objective, may violate constraints */
    bool has_feasible_solution;
    
    /* Algorithm state */
    optimization_phase_t phase;
    double learning_rate;        /* α(t) - current gradient descent step size multiplier */
    double initial_learning_rate;
    double learning_decay;       /* β for exponential decay: α(t) = α₀ × β^t */
    double step_size_multiplier; /* Adaptive multiplier for parameter steps: 4.0 → 1.0 during optimization */
    
    int stabilization_window;    /* Iterations to wait after parameter change (default: 3) */
    int iterations_since_change;
    int total_iterations;
    
    /* Early termination detection - avoids wasting time on hopeless configurations */
    int poor_config_streak;      /* Count of consecutive iterations with >5-10x performance degradation */
    double baseline_objective;   /* Baseline score for detecting catastrophic degradation */
    bool early_abort_enabled;    /* Enable early termination feature (default: true) */
    
    /* Convergence tracking */
    double convergence_threshold;     /* Relative improvement threshold */
    int consecutive_stable;           /* Count of stable iterations */
    int required_stable_iterations;   /* Required for convergence */
    double last_objective_value;
    
    /* Phase-specific state */
    int coordinate_index;      /* For coordinate descent */
    int refinement_cycles;
    
    /* Binary search state for RECALL phase (finding minimal ef_search) */
    int binary_search_active;  /* 1 if binary search in progress, 0 otherwise */
    int binary_search_param_idx; /* Index of parameter being binary searched (typically ef_search) */
    int binary_search_low;     /* Current lower bound */
    int binary_search_high;    /* Current upper bound */
    int binary_search_last_feasible; /* Last value that satisfied constraints */
    
    /* Grid search state for THROUGHPUT phase (exhaustive exploration) */
    int grid_search_active;    /* 1 if grid search in progress, 0 otherwise */
    int grid_search_param_idx; /* Current parameter being grid searched */
    int grid_search_phase;     /* 0=coarse, 1=fine */
    int grid_search_tested_count; /* Number of values tested so far */
    int grid_search_best_value;   /* Best value found so far */
    double grid_search_best_score; /* Best score found so far */
    int grid_search_fine_start;   /* Start of fine search range (fixed at phase transition) */
    int grid_search_fine_end;     /* End of fine search range (fixed at phase transition) */
    
    /* Valkey benchmark integration */
    void *benchmark_context;   /* Opaque pointer to benchmark config */
    
} optimizer_t;

/* ============================================================================
 * Public API Functions
 * ============================================================================ */

/* Create and configure optimizer */
optimizer_t* optimizer_create(void);
void optimizer_destroy(optimizer_t *opt);

/* Add tunable parameter that optimizer can adjust */
bool optimizer_add_param(optimizer_t *opt, const char *name,
                         int min_val, int max_val, int step_size, int initial_val);

/* Add tunable parameter with specified group */
bool optimizer_add_param_grouped(optimizer_t *opt, const char *name,
                                 int min_val, int max_val, int step_size, int initial_val,
                                 param_group_t group);

/* Add constraint-only parameter (not tunable, but can be constrained) */
bool optimizer_add_constraint_param(optimizer_t *opt, const char *name, int current_val);

/* Add constraint: metric ⋚ threshold */
bool optimizer_add_constraint(optimizer_t *opt, metric_t metric,
                              constraint_type_t type, double threshold);

/* Set optimization objective */
void optimizer_set_objective(optimizer_t *opt, metric_t metric, objective_type_t type);

/* Main optimization step - call after each benchmark run */
status_t optimizer_step(optimizer_t *opt, const double metrics[METRIC_COUNT]);

/* Query current configuration */
void optimizer_get_current_config(const optimizer_t *opt, int *values, int max_params);
const char* optimizer_get_param_name(const optimizer_t *opt, int idx);
int optimizer_get_param_count(const optimizer_t *opt);

/* Get best solution found so far */
const measurement_t* optimizer_get_best_solution(const optimizer_t *opt);

/* Status and diagnostics */
void optimizer_print_status(const optimizer_t *opt, FILE *out);
void optimizer_print_csv_header(FILE *out);
void optimizer_print_csv_row(const optimizer_t *opt, FILE *out);

/* Reset parameters to initial values (for next benchmark run) */
void optimizer_reset_to_current_config(optimizer_t *opt);

#endif