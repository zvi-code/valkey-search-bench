/*
 * load_optimizer.c - Adaptive Load Optimizer with Domain-Aware Phased Optimization
 * 
 * This module implements a gradient descent optimizer for automatically tuning
 * benchmark parameters (clients, threads, pipeline depth, ef_search) to maximize
 * or minimize objective metrics while satisfying constraints.
 * 
 * KEY ALGORITHMIC IMPROVEMENTS FOR FAST CONVERGENCE:
 * 
 * 1. ADAPTIVE STEP SIZING (Large → Small)
 *    - Starts with 4x base step size for rapid exploration of parameter space
 *    - Automatically reduces step size as optimization converges
 *    - Rationale: Large initial steps find boundaries quickly, small final steps
 *      enable precise tuning without oscillation
 *    - Implementation: step_size_multiplier starts at 4.0, decays by 0.9 or 0.85
 * 
 * 2. GRADIENT-PRIORITIZED UPDATES
 *    - Parameters are sorted by |∂f/∂θᵢ| (gradient magnitude)
 *    - High-impact parameters are adjusted first in each iteration
 *    - Rationale: Changing parameters with biggest impact provides fastest
 *      improvement per iteration
 *    - Implementation: apply_gradient_update() sorts params before applying changes
 * 
 * 3. EARLY ABORT FOR HOPELESS CONFIGURATIONS
 *    - Detects catastrophically poor performance (>5-10x worse than baseline)
 *    - Terminates benchmark run early instead of waiting for completion
 *    - Automatically resets to best known feasible configuration
 *    - Rationale: Avoid wasting time on parameter combinations that clearly
 *      cannot meet constraints (e.g., ef_search=1000 causing latency explosion)
 *    - Implementation: should_abort_early() checks degradation ratios, tracks
 *      poor_config_streak, returns STATUS_ERROR to signal early termination
 * 
 * 4. DOMAIN-AWARE PHASED OPTIMIZATION
 *    - Parameters grouped by what they affect: RECALL vs THROUGHPUT
 *    - Optimization phases: RECALL → THROUGHPUT → HILL_CLIMB → REFINEMENT
 *    - Each phase only adjusts its group's parameters
 *    - Rationale: ef_search affects recall but not QPS; clients/threads affect
 *      QPS but not recall. Optimizing independently converges faster than
 *      adjusting all 4 parameters simultaneously
 *    - Implementation: param_group_t enum, get_active_param_group(), phase transitions
 * 
 * OPTIMIZATION ALGORITHM:
 *   - Gradient estimation via finite differences on measurement history
 *   - Projected gradient descent with bound constraints
 *   - Exponential learning rate decay (α(t) = α₀ × β^t)
 *   - Constraint satisfaction via penalty projection
 * 
 * CONVERGENCE CRITERIA:
 *   - Objective improvement < 2% for 5 consecutive iterations
 *   - Learning rate decayed below minimum threshold
 *   - All parameters at bounds with no improvement possible
 */

#include "load_optimizer.h"

/* ============================================================================
 * Constants and Utilities
 * ============================================================================ */

static const char* metric_names[METRIC_COUNT] = {
    [METRIC_QPS] = "QPS",
    [METRIC_AVG_LATENCY] = "avg_latency_ms",
    [METRIC_P50_LATENCY] = "p50_latency_ms",
    [METRIC_P90_LATENCY] = "p90_latency_ms",
    [METRIC_P95_LATENCY] = "p95_latency_ms",
    [METRIC_P99_LATENCY] = "p99_latency_ms",
    [METRIC_MAX_LATENCY] = "max_latency_ms",
    [METRIC_MIN_LATENCY] = "min_latency_ms",
    [METRIC_RECALL_AVG] = "recall_avg",
    [METRIC_RECALL_MIN] = "recall_min",
    [METRIC_RECALL_MAX] = "recall_max",
    [METRIC_RECALL_PERFECT_PCT] = "recall_perfect_pct",
    [METRIC_RECALL_ZERO_PCT] = "recall_zero_pct",
};

static const char* phase_names[] = {
    [PHASE_INIT] = "INIT",
    [PHASE_FEASIBILITY] = "FEASIBILITY",
    [PHASE_RECALL] = "RECALL_OPT",
    [PHASE_THROUGHPUT] = "THROUGHPUT_OPT",
    [PHASE_HILL_CLIMB] = "HILL_CLIMB",
    [PHASE_REFINEMENT] = "REFINEMENT",
    [PHASE_CONVERGED] = "CONVERGED"
};

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(x, lo, hi) (MAX((lo), MIN((x), (hi))))

/* Forward declarations */
static int estimate_grid_points(const param_t *p);
static void init_grid_search_for_throughput(optimizer_t *opt);
static int update_grid_search(optimizer_t *opt, double current_score, int constraints_satisfied);

/* ============================================================================
 * Optimizer Creation and Configuration
 * ============================================================================ */

optimizer_t* optimizer_create(void) {
    optimizer_t *opt = calloc(1, sizeof(optimizer_t));
    if (!opt) return NULL;
    
    /* Algorithm hyperparameters */
    opt->initial_learning_rate = 0.2;
    opt->learning_rate = opt->initial_learning_rate;
    opt->learning_decay = 0.75;  /* Exponential decay factor */
    opt->step_size_multiplier = 10.0;  /* Start with 10x step size, reduce as we converge */
    opt->stabilization_window = 1;  /* Wait this many iterations after param change */
    opt->convergence_threshold = 0.01;  /* 2% relative change */
    opt->required_stable_iterations = 4;
    
    /* Early termination detection */
    opt->poor_config_streak = 0;
    opt->baseline_objective = -DBL_MAX;
    opt->early_abort_enabled = true;
    
    /* Binary search for RECALL phase */
    opt->binary_search_active = 0;
    opt->binary_search_param_idx = -1;
    opt->binary_search_low = 0;
    opt->binary_search_high = 0;
    opt->binary_search_last_feasible = -1;
    
    /* Grid search for THROUGHPUT phase */
    opt->grid_search_active = 0;
    opt->grid_search_param_idx = -1;
    opt->grid_search_phase = 0;
    opt->grid_search_tested_count = 0;
    opt->grid_search_best_value = -1;
    opt->grid_search_best_score = -DBL_MAX;
    
    /* Initialize history buffer */
    opt->history_capacity = 200;
    opt->history = calloc(opt->history_capacity, sizeof(measurement_t));
    if (!opt->history) {
        free(opt);
        return NULL;
    }
    
    opt->phase = PHASE_INIT;
    opt->last_objective_value = -DBL_MAX;
    
    return opt;
}

void optimizer_destroy(optimizer_t *opt) {
    if (!opt) return;
    free(opt->params);
    free(opt->constraints);
    free(opt->history);
    free(opt);
}

bool optimizer_add_param(optimizer_t *opt, const char *name,
                         int min_val, int max_val, int step_size, int initial_val) {
    return optimizer_add_param_grouped(opt, name, min_val, max_val, step_size, initial_val, PARAM_GROUP_MIXED);
}

bool optimizer_add_param_grouped(optimizer_t *opt, const char *name,
                                 int min_val, int max_val, int step_size, int initial_val,
                                 param_group_t group) {
    assert(opt && name);
    assert(min_val <= max_val && step_size > 0);
    
    param_t *new_params = realloc(opt->params, (opt->num_params + 1) * sizeof(param_t));
    if (!new_params) return false;
    
    opt->params = new_params;
    param_t *p = &opt->params[opt->num_params];
    
    p->name = name;
    p->min_val = min_val;
    p->max_val = max_val;
    p->step_size = step_size;
    p->adaptive_step_size = step_size * 4;  /* Start with 4x larger steps for exploration */
    p->current_val = CLAMP(initial_val, min_val, max_val);
    p->initial_val = p->current_val;
    p->gradient = 0.0;
    p->gradient_magnitude = 0.0;
    p->last_update_iter = 0;
    p->is_tunable = true;  /* This is a tunable parameter */
    p->locked = false;     /* Not locked initially */
    p->group = group;      /* Set parameter group */
    
    opt->num_params++;
    return true;
}

bool optimizer_add_constraint_param(optimizer_t *opt, const char *name, int current_val) {
    assert(opt && name);
    
    param_t *new_params = realloc(opt->params, (opt->num_params + 1) * sizeof(param_t));
    if (!new_params) return false;
    
    opt->params = new_params;
    param_t *p = &opt->params[opt->num_params];
    
    p->name = name;
    p->min_val = current_val;
    p->max_val = current_val;
    p->step_size = 0;
    p->current_val = current_val;
    p->initial_val = current_val;
    p->gradient = 0.0;
    p->last_update_iter = 0;
    p->is_tunable = false;  /* This is constraint-only, not tunable */
    p->locked = false;     /* Constraint-only params are not locked */
    p->group = PARAM_GROUP_MIXED;  /* Group doesn't matter for constraint-only params */
    
    opt->num_params++;
    return true;
}

bool optimizer_add_constraint(optimizer_t *opt, metric_t metric,
                              constraint_type_t type, double threshold) {
    assert(opt && metric < METRIC_COUNT);
    
    constraint_t *new_constraints = realloc(opt->constraints,
                                           (opt->num_constraints + 1) * sizeof(constraint_t));
    if (!new_constraints) return false;
    
    opt->constraints = new_constraints;
    constraint_t *c = &opt->constraints[opt->num_constraints];
    
    c->metric = metric;
    c->type = type;
    c->threshold = threshold;
    c->violation = 0.0;
    
    opt->num_constraints++;
    return true;
}

void optimizer_set_objective(optimizer_t *opt, metric_t metric, objective_type_t type) {
    assert(opt && metric < METRIC_COUNT);
    opt->objective.metric = metric;
    opt->objective.type = type;
}

/* ============================================================================
 * Constraint and Objective Evaluation
 * ============================================================================ */

/*
 * Evaluate all constraints and compute violation metrics
 * Returns: true if all constraints satisfied
 * 
 * Violation metric for constraint g(x) ≤ 0:
 *   violation = max(0, g(x))
 */
static bool evaluate_constraints(optimizer_t *opt, measurement_t *m) {
    bool all_satisfied = true;
    
    for (int i = 0; i < opt->num_constraints; i++) {
        constraint_t *c = &opt->constraints[i];
        double value = m->metrics[c->metric];
        
        switch (c->type) {
            case CONSTRAINT_LESS_THAN:
                c->violation = MAX(0.0, value - c->threshold);
                if (value >= c->threshold) all_satisfied = false;
                break;
                
            case CONSTRAINT_GREATER_THAN:
                c->violation = MAX(0.0, c->threshold - value);
                if (value <= c->threshold) all_satisfied = false;
                break;
        }
    }
    
    m->constraints_satisfied = all_satisfied;
    return all_satisfied;
}

/*
 * Compute objective score normalized for maximization
 * Transforms minimization objectives: score = -metric
 */
static double compute_objective_score(const optimizer_t *opt, const measurement_t *m) {
    double value = m->metrics[opt->objective.metric];
    
    if (opt->objective.type == OBJECTIVE_MINIMIZE) {
        return -value;
    }
    return value;
}

/* ============================================================================
 * Measurement Recording
 * ============================================================================ */

/*
 * Record new measurement and update best solutions
 * Maintains sliding window of history for gradient estimation
 */
static void record_measurement(optimizer_t *opt, const double metrics[METRIC_COUNT]) {
    /* Expand history if needed */
    if (opt->history_size >= opt->history_capacity) {
        int new_cap = opt->history_capacity * 2;
        measurement_t *new_hist = realloc(opt->history, new_cap * sizeof(measurement_t));
        if (new_hist) {
            opt->history = new_hist;
            opt->history_capacity = new_cap;
        } else {
            /* Shift history to make room (circular buffer) */
            memmove(opt->history, opt->history + 1,
                   (opt->history_size - 1) * sizeof(measurement_t));
            opt->history_size--;
        }
    }
    
    measurement_t *m = &opt->history[opt->history_size++];
    memcpy(m->metrics, metrics, sizeof(m->metrics));
    
    /* Snapshot current parameters */
    m->num_params = opt->num_params;
    for (int i = 0; i < opt->num_params; i++) {
        m->param_values[i] = opt->params[i].current_val;
    }
    
    m->iteration = opt->total_iterations;
    m->timestamp = time(NULL);
    
    /* Evaluate constraints and objective */
    evaluate_constraints(opt, m);
    m->objective_score = compute_objective_score(opt, m);
    
    /* Update best solutions */
    if (m->objective_score > opt->best_ever.objective_score) {
        opt->best_ever = *m;
    }
    
    if (m->constraints_satisfied) {
        if (!opt->has_feasible_solution ||
            m->objective_score > opt->best_feasible.objective_score) {
            opt->best_feasible = *m;
            opt->has_feasible_solution = true;
        }
    }
}

/* ============================================================================
 * Gradient Estimation
 * ============================================================================ */

/*
 * Estimate gradient using recent history via finite differences
 * 
 * For parameter θᵢ:
 *   ∂f/∂θᵢ ≈ (f(θ + δeᵢ) - f(θ - δeᵢ)) / (2δ)
 * 
 * Uses weighted average of recent changes where parameter i was modified
 */
static void estimate_gradients(optimizer_t *opt) {
    if (opt->history_size < 2) return;
    
    for (int i = 0; i < opt->num_params; i++) {
        param_t *p = &opt->params[i];
        
        double gradient_sum = 0.0;
        int count = 0;
        
        /* Look back in history for changes in this parameter */
        for (int h = opt->history_size - 1; h > 0 && count < 5; h--) {
            const measurement_t *curr = &opt->history[h];
            const measurement_t *prev = &opt->history[h - 1];
            
            int delta = curr->param_values[i] - prev->param_values[i];
            if (delta != 0) {
                /* Only consider if constraints were satisfied in both */
                if (curr->constraints_satisfied && prev->constraints_satisfied) {
                    double obj_delta = curr->objective_score - prev->objective_score;
                    gradient_sum += obj_delta / delta;
                    count++;
                }
            }
        }
        
        if (count > 0) {
            p->gradient = gradient_sum / count;
            p->gradient_magnitude = fabs(p->gradient);  /* Track magnitude for prioritization */
        } else {
            /* No recent changes - use small random perturbation signal */
            p->gradient = ((double)rand() / RAND_MAX - 0.5) * 0.1;
            p->gradient_magnitude = fabs(p->gradient);
        }
    }
}

/* ============================================================================
 * Parameter Update and Projection
 * ============================================================================ */

/*
 * Determine which parameter group should be active based on current phase
 */
static param_group_t get_active_param_group(optimization_phase_t phase) {
    switch (phase) {
        case PHASE_RECALL:
            return PARAM_GROUP_RECALL;
        case PHASE_THROUGHPUT:
            return PARAM_GROUP_THROUGHPUT;
        case PHASE_HILL_CLIMB:
        case PHASE_REFINEMENT:
            return PARAM_GROUP_MIXED;  /* Optimize all parameters together */
        default:
            return PARAM_GROUP_MIXED;
    }
}

/*
 * Apply gradient-based update with projection to bounds
 * 
 * Update rule: θᵢ(t+1) = clip(θᵢ(t) + α(t) × ∇f(θᵢ) × adaptive_step, [min, max])
 * 
 * Improvements:
 * 1. Uses adaptive step sizes (start large, reduce as converging)
 * 2. Prioritizes parameters with largest gradient magnitude (biggest impact)
 * 3. Phase-aware filtering (RECALL/THROUGHPUT/MIXED)
 * 
 * Returns: true if any parameter changed
 */
static bool apply_gradient_update(optimizer_t *opt) {
    bool changed = false;
    param_group_t active_group = get_active_param_group(opt->phase);
    
    /* Update adaptive step size multiplier based on phase */
    if (opt->phase == PHASE_RECALL || opt->phase == PHASE_THROUGHPUT) {
        /* Early phases: use larger steps for faster exploration */
        opt->step_size_multiplier = MAX(1.0, opt->step_size_multiplier * 0.9);
    } else {
        /* Later phases: smaller steps for refinement */
        opt->step_size_multiplier = MAX(0.5, opt->step_size_multiplier * 0.85);
    }
    
    /* Sort parameters by gradient magnitude to prioritize high-impact changes */
    typedef struct { int idx; double magnitude; } param_priority_t;
    param_priority_t priorities[16];  /* Max params */
    int priority_count = 0;
    
    for (int i = 0; i < opt->num_params; i++) {
        param_t *p = &opt->params[i];
        
        /* Skip constraint-only parameters */
        if (!p->is_tunable) continue;
        
        /* Skip locked parameters (optimized in previous phase) */
        if (p->locked) continue;
        
        /* Phase-aware filtering: only update parameters in active group */
        if (active_group != PARAM_GROUP_MIXED && p->group != active_group && p->group != PARAM_GROUP_MIXED) {
            continue;
        }
        
        priorities[priority_count].idx = i;
        priorities[priority_count].magnitude = p->gradient_magnitude;
        priority_count++;
    }
    
    /* Sort by magnitude (descending) - simple bubble sort for small arrays */
    for (int i = 0; i < priority_count - 1; i++) {
        for (int j = i + 1; j < priority_count; j++) {
            if (priorities[j].magnitude > priorities[i].magnitude) {
                param_priority_t temp = priorities[i];
                priorities[i] = priorities[j];
                priorities[j] = temp;
            }
        }
    }
    
    /* Apply updates starting with highest-impact parameters */
    for (int p_idx = 0; p_idx < priority_count; p_idx++) {
        int i = priorities[p_idx].idx;
        param_t *p = &opt->params[i];
        
        /* Update adaptive step size for this parameter */
        p->adaptive_step_size = (int)(p->step_size * opt->step_size_multiplier);
        p->adaptive_step_size = MAX(p->step_size, p->adaptive_step_size);  /* Never go below base step */
        
        /* Compute step: α × gradient × adaptive_step_size */
        double raw_step = opt->learning_rate * p->gradient * p->adaptive_step_size;
        int step = (int)round(raw_step);
        
        /* Must be at least one step if gradient is non-zero */
        if (fabs(raw_step) > 0.1 && step == 0) {
            step = (raw_step > 0) ? 1 : -1;
        }
        
        if (step != 0) {
            int new_val = p->current_val + step;
            new_val = CLAMP(new_val, p->min_val, p->max_val);
            
            if (new_val != p->current_val) {
                p->current_val = new_val;
                p->last_update_iter = opt->total_iterations;
                changed = true;
            }
        }
    }
    
    return changed;
}

/*
 * Binary search back to constraint boundary if violated
 * Projects current configuration onto feasible region
 */
static void project_to_feasible(optimizer_t *opt) {
    if (!opt->has_feasible_solution) return;
    
    /* Check if current config violates constraints */
    bool violated = false;
    for (int i = 0; i < opt->num_constraints; i++) {
        if (opt->constraints[i].violation > 0) {
            violated = true;
            break;
        }
    }
    
    if (violated) {
        /* Scale back towards best feasible solution */
        for (int i = 0; i < opt->num_params; i++) {
            if (!opt->params[i].is_tunable) continue;  /* Skip constraint-only params */
            
            int current = opt->params[i].current_val;
            int feasible = opt->best_feasible.param_values[i];
            
            /* Move halfway back */
            opt->params[i].current_val = (current + feasible) / 2;
        }
    }
}

/*
 * Coordinate descent: optimize one parameter at a time
 * Used in refinement phase for fine-grained tuning
 * 
 * In phased optimization, only iterates through parameters in the active group
 */
static bool coordinate_descent_step(optimizer_t *opt) {
    param_group_t active_group = get_active_param_group(opt->phase);
    
    /* Find next tunable parameter in active group */
    int attempts = 0;
    int idx = -1;
    
    while (attempts < opt->num_params) {
        int candidate_idx = opt->coordinate_index % opt->num_params;
        opt->coordinate_index++;
        
        param_t *candidate = &opt->params[candidate_idx];
        
        /* Check if parameter is tunable, not locked, and in active group */
        if (candidate->is_tunable && !candidate->locked) {
            if (active_group == PARAM_GROUP_MIXED || 
                candidate->group == active_group || 
                candidate->group == PARAM_GROUP_MIXED) {
                idx = candidate_idx;
                break;
            }
        }
        attempts++;
    }
    
    if (idx < 0) {
        /* No tunable parameters found in active group */
        return false;
    }
    
    param_t *p = &opt->params[idx];
    
    /* Try increasing */
    int new_val_up = MIN(p->current_val + p->step_size, p->max_val);
    /* Try decreasing */
    int new_val_down = MAX(p->current_val - p->step_size, p->min_val);
    
    /* Choose direction that's likely to improve (use gradient hint) */
    int new_val;
    if (p->gradient > 0 && new_val_up != p->current_val) {
        new_val = new_val_up;
    } else if (p->gradient < 0 && new_val_down != p->current_val) {
        new_val = new_val_down;
    } else {
        /* No clear direction, skip to next parameter */
        return false;
    }
    
    if (new_val != p->current_val) {
        p->current_val = new_val;
        return true;
    }
    
    return false;
}

/* ============================================================================
 * Phase Management and Convergence
 * ============================================================================ */

/*
 * Check convergence criteria:
 * 1. Objective improvement < threshold for N consecutive iterations
 * 2. Learning rate decayed below minimum
 * 3. All parameters at bounds
 */
static bool check_convergence(optimizer_t *opt) {
    if (opt->history_size < opt->required_stable_iterations) {
        return false;
    }
    
    /* Check relative improvement */
    double current = opt->best_feasible.objective_score;
    double delta = fabs(current - opt->last_objective_value);
    double relative_change = (fabs(opt->last_objective_value) > 1e-9) ?
                            delta / fabs(opt->last_objective_value) : delta;
    
    if (relative_change < opt->convergence_threshold) {
        opt->consecutive_stable++;
    } else {
        opt->consecutive_stable = 0;
        opt->last_objective_value = current;
    }
    
    if (opt->consecutive_stable >= opt->required_stable_iterations) {
        return true;
    }
    
    /* Check if learning rate too small */
    if (opt->learning_rate < 0.01) {
        return true;
    }
    
    return false;
}

/*
 * Update learning rate with exponential decay
 * α(t) = α₀ × β^t, where β < 1
 */
static void update_learning_rate(optimizer_t *opt, bool improvement) {
    if (improvement) {
        /* Boost learning rate slightly on success */
        opt->learning_rate = MIN(opt->learning_rate * 1.1, opt->initial_learning_rate);
    } else {
        /* Decay learning rate */
        opt->learning_rate *= opt->learning_decay;
    }
}

/*
 * Check if recall-related constraints are satisfied
 * Used to determine when to transition from RECALL to THROUGHPUT phase
 */
__attribute__((unused))
static bool recall_constraints_satisfied(optimizer_t *opt) {
    if (!opt->has_feasible_solution) return false;
    
    /* Check constraints related to recall metrics */
    for (int i = 0; i < opt->num_constraints; i++) {
        constraint_t *c = &opt->constraints[i];
        
        /* Check if this is a recall-related metric */
        if (c->metric == METRIC_RECALL_AVG || 
            c->metric == METRIC_RECALL_MIN || 
            c->metric == METRIC_RECALL_MAX || 
            c->metric == METRIC_RECALL_PERFECT_PCT || 
            c->metric == METRIC_RECALL_ZERO_PCT) {
            
            if (c->violation > 0) {
                return false;  /* Recall constraint violated */
            }
        }
    }
    
    return true;  /* All recall constraints satisfied */
}

/*
 * Transition between optimization phases
 * 
 * Phase flow for domain-aware optimization:
 *   INIT → FEASIBILITY → RECALL → THROUGHPUT → HILL_CLIMB → REFINEMENT → CONVERGED
 * 
 * RECALL phase: Optimize ef_search to meet recall constraints
 * THROUGHPUT phase: Optimize clients/threads/pipeline for QPS/latency
 * HILL_CLIMB: Optimize all parameters together for final tuning
 */
static void update_phase(optimizer_t *opt) {
    switch (opt->phase) {
        case PHASE_INIT:
            if (opt->history_size >= 1) {
                if (opt->has_feasible_solution) {
                    /* Check if we have recall constraints */
                    bool has_recall_constraints = false;
                    for (int i = 0; i < opt->num_constraints; i++) {
                        metric_t m = opt->constraints[i].metric;
                        if (m == METRIC_RECALL_AVG || m == METRIC_RECALL_MIN || 
                            m == METRIC_RECALL_MAX || m == METRIC_RECALL_PERFECT_PCT || 
                            m == METRIC_RECALL_ZERO_PCT) {
                            has_recall_constraints = true;
                            break;
                        }
                    }
                    
                    /* Start with RECALL phase if we have recall constraints */
                    if (has_recall_constraints) {
                        opt->phase = PHASE_RECALL;
                    } else {
                        opt->phase = PHASE_THROUGHPUT;
                    }
                } else {
                    opt->phase = PHASE_FEASIBILITY;
                }
            }
            break;
            
        case PHASE_FEASIBILITY:
            if (opt->has_feasible_solution) {
                /* Same logic as PHASE_INIT for next phase selection */
                bool has_recall_constraints = false;
                for (int i = 0; i < opt->num_constraints; i++) {
                    metric_t m = opt->constraints[i].metric;
                    if (m == METRIC_RECALL_AVG || m == METRIC_RECALL_MIN || 
                        m == METRIC_RECALL_MAX || m == METRIC_RECALL_PERFECT_PCT || 
                        m == METRIC_RECALL_ZERO_PCT) {
                        has_recall_constraints = true;
                        break;
                    }
                }
                
                if (has_recall_constraints) {
                    opt->phase = PHASE_RECALL;
                } else {
                    opt->phase = PHASE_THROUGHPUT;
                }
                opt->learning_rate = opt->initial_learning_rate;
            } else if (opt->total_iterations > 50) {
                /* Give up after many attempts */
                opt->phase = PHASE_CONVERGED;
            }
            break;
        
        case PHASE_RECALL:
            /* Move to THROUGHPUT phase once binary search completes */
            if (!opt->binary_search_active) {
                /* Binary search finished - lock RECALL/MIXED parameters */
                for (int i = 0; i < opt->num_params; i++) {
                    param_t *p = &opt->params[i];
                    if (p->is_tunable && 
                        (p->group == PARAM_GROUP_RECALL || p->group == PARAM_GROUP_MIXED)) {
                        p->locked = true;
                        printf("[Phase Transition] Locking %s=%d (RECALL phase complete)\n",
                               p->name, p->current_val);
                    }
                }
                
                /* Reset step size multiplier for THROUGHPUT phase - use smaller steps */
                opt->step_size_multiplier = 2.0;  /* Start with 2x steps instead of 10x */
                printf("[Phase Transition] Reset step_size_multiplier to 2.0 for THROUGHPUT phase\n");
                
                /* Binary search finished */
                if (opt->binary_search_last_feasible >= 0) {
                    /* Found optimal value, move to throughput optimization */
                    opt->phase = PHASE_THROUGHPUT;
                    opt->learning_rate = opt->initial_learning_rate * 0.5;  /* Lower learning rate */
                } else {
                    /* No feasible solution found in binary search range */
                    /* Try throughput phase anyway */
                    opt->phase = PHASE_THROUGHPUT;
                    opt->learning_rate = opt->initial_learning_rate * 0.5;
                }
            }
            break;
        
        case PHASE_THROUGHPUT:
            /* Move to HILL_CLIMB once throughput optimization converges */
            if (check_convergence(opt) || opt->learning_rate < 0.1) {
                opt->phase = PHASE_HILL_CLIMB;
                opt->learning_rate = opt->initial_learning_rate * 0.5;  /* Lower LR for fine-tuning */
            }
            break;
            
        case PHASE_HILL_CLIMB:
            if (opt->learning_rate < 0.1) {
                opt->phase = PHASE_REFINEMENT;
                opt->coordinate_index = 0;
            }
            if (check_convergence(opt)) {
                opt->phase = PHASE_CONVERGED;
            }
            break;
            
        case PHASE_REFINEMENT:
            opt->refinement_cycles++;
            if (opt->refinement_cycles > opt->num_params * 3 ||
                check_convergence(opt)) {
                opt->phase = PHASE_CONVERGED;
            }
            break;
            
        case PHASE_CONVERGED:
            /* Terminal state */
            break;
    }
}

/* ============================================================================
 * Binary Search for RECALL Phase (Minimal ef_search)
 * ============================================================================ */

/*
 * Initialize binary search for finding minimal parameter value that satisfies recall constraints.
 * Used specifically for ef_search in RECALL phase.
 * 
 * Strategy: Binary search to find lowest ef_search where recall >= threshold
 * Example: If constraint is recall >= 0.95:
 *   - Test ef_search=10 → recall=0.68 (fail) → low=10
 *   - Test ef_search=500 → recall=0.98 (pass) → high=500, last_feasible=500
 *   - Test ef_search=255 → recall=0.96 (pass) → high=255, last_feasible=255
 *   - Test ef_search=132 → recall=0.94 (fail) → low=132
 *   - Test ef_search=193 → recall=0.95 (pass) → high=193, last_feasible=193
 *   - Continue until low+1 >= high → return last_feasible=193
 */
static void init_binary_search_for_recall(optimizer_t *opt) {
    /* Find ef_search parameter (or any MIXED/RECALL group param) */
    int param_idx = -1;
    for (int i = 0; i < opt->num_params; i++) {
        if (opt->params[i].is_tunable && 
            (opt->params[i].group == PARAM_GROUP_MIXED || opt->params[i].group == PARAM_GROUP_RECALL)) {
            param_idx = i;
            break;
        }
    }
    
    if (param_idx < 0) {
        /* No recall parameter to optimize */
        opt->binary_search_active = 0;
        return;
    }
    
    param_t *p = &opt->params[param_idx];
    opt->binary_search_active = 1;
    opt->binary_search_param_idx = param_idx;
    opt->binary_search_low = p->min_val;
    opt->binary_search_high = p->max_val;
    opt->binary_search_last_feasible = -1;
    
    /* Start with midpoint */
    int mid = (p->min_val + p->max_val) / 2;
    /* Round to nearest step_size multiple */
    mid = ((mid + p->step_size / 2) / p->step_size) * p->step_size;
    p->current_val = CLAMP(mid, p->min_val, p->max_val);
    
    printf("[Binary Search] Starting search for %s in range [%d, %d], initial value=%d\n",
           p->name, p->min_val, p->max_val, p->current_val);
}

/*
 * Update binary search based on whether current configuration satisfies recall constraints.
 * Returns 1 if search should continue, 0 if converged.
 */
static int update_binary_search(optimizer_t *opt, int constraints_satisfied) {
    if (!opt->binary_search_active) return 0;
    
    param_t *p = &opt->params[opt->binary_search_param_idx];
    int current = p->current_val;
    
    if (constraints_satisfied) {
        /* Current value works - try lower */
        opt->binary_search_high = current;
        opt->binary_search_last_feasible = current;
        printf("[Binary Search] %s=%d PASSED recall constraint, searching lower [%d, %d]\n",
               p->name, current, opt->binary_search_low, opt->binary_search_high);
    } else {
        /* Current value fails - try higher */
        opt->binary_search_low = current;
        printf("[Binary Search] %s=%d FAILED recall constraint, searching higher [%d, %d]\n",
               p->name, current, opt->binary_search_low, opt->binary_search_high);
    }
    
    /* Check convergence: search range <= step_size */
    if (opt->binary_search_high - opt->binary_search_low <= p->step_size) {
        if (opt->binary_search_last_feasible >= 0) {
            p->current_val = opt->binary_search_last_feasible;
            printf("[Binary Search] CONVERGED: Optimal %s=%d (minimal value satisfying constraints)\n",
                   p->name, p->current_val);
        } else {
            printf("[Binary Search] FAILED: No feasible value found in range\n");
        }
        opt->binary_search_active = 0;
        return 0;
    }
    
    /* Calculate next midpoint */
    int mid = (opt->binary_search_low + opt->binary_search_high) / 2;
    /* Round to nearest step_size multiple */
    mid = ((mid + p->step_size / 2) / p->step_size) * p->step_size;
    p->current_val = CLAMP(mid, p->min_val, p->max_val);
    
    printf("[Binary Search] Next test: %s=%d\n", p->name, p->current_val);
    return 1;
}

/* ============================================================================
 * Grid Search for THROUGHPUT Phase - Exhaustive Parameter Exploration
 * ============================================================================ */

/*
 * Initialize grid search for a THROUGHPUT parameter
 * Strategy: Multi-scale search (coarse → fine)
 * 
 * Coarse phase: Test exponentially-spaced values across full range
 *   Example for clients [10, 1000]:
 *     10, 20, 40, 80, 160, 320, 640, 1000
 * 
 * Fine phase: After finding best coarse value, test smaller increments around it
 *   Example if best=160:
 *     140, 150, 160, 170, 180
 */
static void init_grid_search_for_throughput(optimizer_t *opt) {
    /* Find next untested THROUGHPUT parameter */
    int param_idx = -1;
    for (int i = 0; i < opt->num_params; i++) {
        param_t *p = &opt->params[i];
        if (p->is_tunable && !p->locked && p->group == PARAM_GROUP_THROUGHPUT) {
            /* TODO: Track which params were already grid searched to avoid re-testing */
            /* For now, search first available param */
            param_idx = i;
            break;
        }
    }
    
    if (param_idx < 0) {
        /* No more parameters to grid search */
        opt->grid_search_active = 0;
        return;
    }
    
    param_t *p = &opt->params[param_idx];
    opt->grid_search_active = 1;
    opt->grid_search_param_idx = param_idx;
    opt->grid_search_phase = 0;  /* Start with coarse search */
    opt->grid_search_tested_count = 0;
    opt->grid_search_best_value = p->current_val;
    opt->grid_search_best_score = opt->has_feasible_solution ? 
                                  opt->best_feasible.objective_score : -DBL_MAX;
    
    /* Start coarse search at minimum value */
    p->current_val = p->min_val;
    
    printf("[Grid Search] Starting COARSE search for %s in range [%d, %d]\n",
           p->name, p->min_val, p->max_val);
    printf("[Grid Search] Testing value %d/%d: %s=%d\n", 
           opt->grid_search_tested_count + 1, 
           estimate_grid_points(p), p->name, p->current_val);
}

/*
 * Estimate number of points in grid for a parameter
 */
static int estimate_grid_points(const param_t *p) {
    int range = p->max_val - p->min_val;
    if (range <= 0) return 1;
    
    /* Coarse: log2(range/step) points */
    int coarse = 0;
    for (int val = p->min_val; val < p->max_val; val = val * 2 + p->step_size) {
        coarse++;
    }
    coarse++; /* Include max_val */
    
    /* Fine: ±2 steps around best (5 points) */
    int fine = 5;
    
    return coarse + fine;
}

/*
 * Update grid search: move to next test value
 * Returns 1 if search continues, 0 if complete
 */
static int update_grid_search(optimizer_t *opt, double current_score, int constraints_satisfied) {
    if (!opt->grid_search_active) return 0;
    
    param_t *p = &opt->params[opt->grid_search_param_idx];
    int current_val = p->current_val;
    
    opt->grid_search_tested_count++;
    
    /* Update best if this configuration is better */
    if (constraints_satisfied && current_score > opt->grid_search_best_score) {
        opt->grid_search_best_value = current_val;
        opt->grid_search_best_score = current_score;
        printf("[Grid Search] New best: %s=%d, score=%.2f\n", 
               p->name, current_val, current_score);
    }
    
    if (opt->grid_search_phase == 0) {
        /* COARSE phase: exponential steps */
        int next_val;
        if (current_val == p->min_val) {
            /* First step: go to min + step_size or double */
            next_val = MIN(p->min_val + p->step_size, p->min_val * 2);
        } else {
            /* Exponential: val * 2 (or val + step_size if too small) */
            next_val = MAX(current_val * 2, current_val + p->step_size);
        }
        
        /* Round to step_size multiple */
        next_val = ((next_val + p->step_size / 2) / p->step_size) * p->step_size;
        next_val = MIN(next_val, p->max_val);
        
        if (next_val > p->max_val || next_val <= current_val) {
            /* Coarse phase complete - transition to fine phase */
            opt->grid_search_phase = 1;
            opt->grid_search_tested_count = 0;
            
            printf("[Grid Search] COARSE phase complete. Best: %s=%d\n", 
                   p->name, opt->grid_search_best_value);
            printf("[Grid Search] Starting FINE search around best value\n");
            
            /* Start fine search at best_value - 2*step */
            int fine_start = MAX(opt->grid_search_best_value - 2 * p->step_size, p->min_val);
            p->current_val = fine_start;
            
            printf("[Grid Search] Testing value 1/5: %s=%d\n", p->name, p->current_val);
            return 1;
        }
        
        p->current_val = next_val;
        printf("[Grid Search] Testing value %d: %s=%d\n", 
               opt->grid_search_tested_count + 1, p->name, p->current_val);
        return 1;
        
    } else {
        /* FINE phase: linear steps around best value */
        /* Range: [best - 2*step, best + 2*step] in increments of step_size */
        int fine_end = MIN(opt->grid_search_best_value + 2 * p->step_size, p->max_val);
        int next_val = current_val + p->step_size;
        
        if (next_val > fine_end || opt->grid_search_tested_count >= 5) {
            /* Fine phase complete */
            p->current_val = opt->grid_search_best_value;
            printf("[Grid Search] CONVERGED: Optimal %s=%d (score=%.2f)\n",
                   p->name, opt->grid_search_best_value, opt->grid_search_best_score);
            opt->grid_search_active = 0;
            return 0;
        }
        
        p->current_val = next_val;
        printf("[Grid Search] Testing value %d/5: %s=%d\n", 
               opt->grid_search_tested_count + 1, p->name, p->current_val);
        return 1;
    }
}

/* ============================================================================
 * Main Optimization Step
 * ============================================================================ */

/*
 * Detect if current configuration is hopelessly poor and should be aborted early
 * 
 * Criteria for early termination:
 * 1. Objective score is extremely poor (>10x worse than baseline)
 * 2. Performance degraded significantly (>5x worse than best feasible)
 * 3. Multiple consecutive iterations without improvement
 * 
 * Returns: true if this config should be aborted
 */
static bool should_abort_early(optimizer_t *opt, const measurement_t *m) {
    if (!opt->early_abort_enabled) return false;
    if (opt->phase == PHASE_INIT || opt->phase == PHASE_FEASIBILITY) return false;
    if (!opt->has_feasible_solution) return false;
    
    /* Update baseline from best feasible solution */
    if (opt->baseline_objective == -DBL_MAX) {
        opt->baseline_objective = opt->best_feasible.objective_score;
    }
    
    /* Check if performance is catastrophically poor */
    bool catastrophic = false;
    
    /* Criterion 1: Objective score >10x worse than baseline */
    if (opt->baseline_objective > 0) {
        double degradation_ratio = opt->baseline_objective / (m->objective_score + 1e-9);
        if (degradation_ratio > 10.0) {
            catastrophic = true;
        }
    }
    
    /* Criterion 2: Performance >5x worse than best feasible for critical metrics */
    if (opt->best_feasible.metrics[METRIC_QPS] > 0) {
        double qps_degradation = opt->best_feasible.metrics[METRIC_QPS] / (m->metrics[METRIC_QPS] + 1.0);
        double latency_degradation = m->metrics[METRIC_AVG_LATENCY] / (opt->best_feasible.metrics[METRIC_AVG_LATENCY] + 1e-6);
        
        if (qps_degradation > 5.0 || latency_degradation > 5.0) {
            catastrophic = true;
        }
    }
    
    /* Track streak of poor performance */
    if (catastrophic) {
        opt->poor_config_streak++;
    } else {
        opt->poor_config_streak = 0;
    }
    
    /* Abort if consistently poor for 3+ iterations */
    if (opt->poor_config_streak >= 3) {
        return true;
    }
    
    return false;
}

/*
 * Single optimization iteration
 * 
 * Returns:
 *   STATUS_WAIT_STABILIZATION - waiting for system to stabilize
 *   STATUS_OK - parameter update applied
 *   STATUS_CONVERGED - optimization complete
 *   STATUS_NO_FEASIBLE - cannot satisfy constraints
 *   STATUS_ERROR - early abort due to hopeless configuration
 */
status_t optimizer_step(optimizer_t *opt, const double metrics[METRIC_COUNT]) {
    assert(opt && metrics);
    
    /* Record measurement */
    record_measurement(opt, metrics);
    opt->total_iterations++;
    opt->iterations_since_change++;
    
    /* Check for early termination of hopeless configurations */
    if (opt->history_size > 0) {
        const measurement_t *current = &opt->history[opt->history_size - 1];
        if (should_abort_early(opt, current)) {
            /* Reset to best feasible and signal abort */
            if (opt->has_feasible_solution) {
                for (int i = 0; i < opt->num_params; i++) {
                    opt->params[i].current_val = opt->best_feasible.param_values[i];
                }
            }
            opt->poor_config_streak = 0;  /* Reset streak after recovery */
            return STATUS_ERROR;  /* Signal caller to skip this iteration */
        }
    }
    
    /* Wait for stabilization after parameter changes */
    if (opt->iterations_since_change < opt->stabilization_window) {
        return STATUS_WAIT_STABILIZATION;
    }
    
    /* Update optimization phase */
    update_phase(opt);
    
    if (opt->phase == PHASE_CONVERGED) {
        return STATUS_CONVERGED;
    }
    
    /* Check feasibility */
    if (!opt->has_feasible_solution && opt->phase != PHASE_FEASIBILITY) {
        return STATUS_NO_FEASIBLE;
    }
    
    bool changed = false;
    
    switch (opt->phase) {
        case PHASE_INIT:
            /* Just collecting initial measurement */
            break;
            
        case PHASE_FEASIBILITY: {
            /* Exponential search: double resources until feasible */
            for (int i = 0; i < opt->num_params; i++) {
                param_t *p = &opt->params[i];
                int new_val = MIN(p->current_val * 2, p->max_val);
                if (new_val > p->current_val) {
                    p->current_val = new_val;
                    changed = true;
                }
            }
            break;
        }
        
        case PHASE_RECALL: {
            /* Binary search for minimal ef_search satisfying recall constraints */
            if (!opt->binary_search_active) {
                /* Initialize binary search on first RECALL iteration */
                init_binary_search_for_recall(opt);
                changed = true;
            } else {
                /* Update binary search based on latest measurement */
                const measurement_t *current = &opt->history[opt->history_size - 1];
                int continue_search = update_binary_search(opt, current->constraints_satisfied);
                changed = continue_search;
                
                if (!continue_search) {
                    /* Binary search complete - transition to next phase */
                    printf("[Binary Search] Complete, transitioning out of RECALL phase\n");
                }
            }
            break;
        }
        
        case PHASE_THROUGHPUT: {
            /* Grid search for exhaustive parameter exploration */
            if (!opt->grid_search_active) {
                /* Initialize grid search for next parameter */
                init_grid_search_for_throughput(opt);
                if (opt->grid_search_active) {
                    changed = true;
                } else {
                    /* No more parameters to grid search - move to next phase */
                    printf("[Grid Search] All THROUGHPUT parameters explored\n");
                    opt->phase = PHASE_HILL_CLIMB;
                    opt->learning_rate = opt->initial_learning_rate * 0.5;
                    changed = false;
                }
            } else {
                /* Continue grid search */
                const measurement_t *current = &opt->history[opt->history_size - 1];
                int continue_search = update_grid_search(opt, current->objective_score, 
                                                         current->constraints_satisfied);
                changed = continue_search;
                
                if (!continue_search && !opt->grid_search_active) {
                    /* This parameter's grid search complete, start next parameter */
                    init_grid_search_for_throughput(opt);
                    changed = (opt->grid_search_active == 1);
                }
            }
            break;
        }
        
        case PHASE_HILL_CLIMB: {
            /* Gradient-based optimization for final tuning */
            estimate_gradients(opt);
            changed = apply_gradient_update(opt);
            
            if (changed) {
                project_to_feasible(opt);
            }
            
            /* Update learning rate based on improvement */
            bool improvement = (opt->history_size >= 2 &&
                               opt->history[opt->history_size - 1].objective_score >
                               opt->history[opt->history_size - 2].objective_score);
            update_learning_rate(opt, improvement);
            break;
        }
            
        case PHASE_REFINEMENT:
            /* Fine-grained coordinate descent */
            changed = coordinate_descent_step(opt);
            break;
            
        case PHASE_CONVERGED:
            return STATUS_CONVERGED;
    }
    
    if (changed) {
        opt->iterations_since_change = 0;
    }
    
    return STATUS_OK;
}

/* ============================================================================
 * Status and Reporting
 * ============================================================================ */

void optimizer_print_status(const optimizer_t *opt, FILE *out) {
    fprintf(out, "\n=== Optimization Status ===\n");
    fprintf(out, "Phase: %s\n", phase_names[opt->phase]);
    fprintf(out, "Iteration: %d\n", opt->total_iterations);
    fprintf(out, "Learning rate: %.4f\n", opt->learning_rate);
    fprintf(out, "Has feasible: %s\n", opt->has_feasible_solution ? "YES" : "NO");
    
    fprintf(out, "\nCurrent Parameters:\n");
    for (int i = 0; i < opt->num_params; i++) {
        const param_t *p = &opt->params[i];
        const char *tunable_mark = p->is_tunable ? "" : " [FIXED]";
        fprintf(out, "  %s = %d [%d, %d] (gradient: %.3f)%s\n",
                p->name, p->current_val, p->min_val, p->max_val, p->gradient, tunable_mark);
    }
    
    if (opt->history_size > 0) {
        const measurement_t *latest = &opt->history[opt->history_size - 1];
        fprintf(out, "\nLatest Metrics:\n");
        for (int i = 0; i < METRIC_COUNT; i++) {
            fprintf(out, "  %s: %.3f\n", metric_names[i], latest->metrics[i]);
        }
        fprintf(out, "  Objective score: %.3f\n", latest->objective_score);
        fprintf(out, "  Constraints: %s\n",
                latest->constraints_satisfied ? "SATISFIED" : "VIOLATED");
    }
    
    if (opt->has_feasible_solution) {
        fprintf(out, "\nBest Feasible Solution:\n");
        fprintf(out, "  Objective: %.3f\n", opt->best_feasible.objective_score);
        fprintf(out, "  Parameters: ");
        for (int i = 0; i < opt->best_feasible.num_params; i++) {
            fprintf(out, "%s=%d ", opt->params[i].name,
                    opt->best_feasible.param_values[i]);
        }
        fprintf(out, "\n");
    }
    
    fprintf(out, "\n");
}

void optimizer_get_current_config(const optimizer_t *opt, int *values, int max_params) {
    int n = MIN(opt->num_params, max_params);
    for (int i = 0; i < n; i++) {
        values[i] = opt->params[i].current_val;
    }
}

const measurement_t* optimizer_get_best_solution(const optimizer_t *opt) {
    return opt->has_feasible_solution ? &opt->best_feasible : NULL;
}

int optimizer_get_param_count(const optimizer_t *opt) {
    return opt->num_params;
}

const char* optimizer_get_param_name(const optimizer_t *opt, int idx) {
    if (idx < 0 || idx >= opt->num_params) return NULL;
    return opt->params[idx].name;
}

void optimizer_reset_to_current_config(optimizer_t *opt) {
    /* Reset all tunable parameters to their current optimized values
     * This is called before each new benchmark run */
    for (int i = 0; i < opt->num_params; i++) {
        opt->params[i].initial_val = opt->params[i].current_val;
    }
}

void optimizer_print_csv_header(FILE *out) {
    fprintf(out, "iteration,phase,qps,avg_lat,p50_lat,p90_lat,p95_lat,p99_lat,max_lat,");
    fprintf(out, "recall_avg,recall_min,recall_max,recall_perfect_pct,recall_zero_pct,");
    fprintf(out, "objective,constraints_ok,learning_rate\n");
}

void optimizer_print_csv_row(const optimizer_t *opt, FILE *out) {
    if (opt->history_size == 0) return;
    
    const measurement_t *m = &opt->history[opt->history_size - 1];
    
    fprintf(out, "%d,%s,", m->iteration, phase_names[opt->phase]);
    fprintf(out, "%.1f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,",
            m->metrics[METRIC_QPS],
            m->metrics[METRIC_AVG_LATENCY],
            m->metrics[METRIC_P50_LATENCY],
            m->metrics[METRIC_P90_LATENCY],
            m->metrics[METRIC_P95_LATENCY],
            m->metrics[METRIC_P99_LATENCY],
            m->metrics[METRIC_MAX_LATENCY]);
    fprintf(out, "%.4f,%.4f,%.4f,%.2f,%.2f,",
            m->metrics[METRIC_RECALL_AVG],
            m->metrics[METRIC_RECALL_MIN],
            m->metrics[METRIC_RECALL_MAX],
            m->metrics[METRIC_RECALL_PERFECT_PCT],
            m->metrics[METRIC_RECALL_ZERO_PCT]);
    fprintf(out, "%.3f,%s,%.4f\n",
            m->objective_score,
            m->constraints_satisfied ? "yes" : "no",
            opt->learning_rate);
}

/* ============================================================================
 * Example Usage
 * ============================================================================ */

#ifdef OPTIMIZER_EXAMPLE

/*
 * Simulated benchmark function for testing
 * Models: QPS increases with resources but with diminishing returns
 *         Latency increases with load
 */
static void simulate_benchmark(int clients, int threads, int io_threads,
                               int reader_threads, int writer_threads,
                               double *metrics) {
    /* Simulate QPS: parallel resources × efficiency */
    double parallelism = clients * threads;
    double io_efficiency = 1.0 + log(io_threads + 1) * 0.3;
    double search_efficiency = 1.0 + log(reader_threads + writer_threads + 1) * 0.2;
    
    metrics[METRIC_QPS] = parallelism * io_efficiency * search_efficiency * 1000.0;
    metrics[METRIC_QPS] /= (1.0 + parallelism / 10000.0);  /* Saturation */
    
    /* Simulate latency: increases with load */
    double load_factor = parallelism / (io_threads * 10.0);
    metrics[METRIC_AVG_LATENCY] = 0.5 + load_factor * 0.3;
    metrics[METRIC_P99_LATENCY] = metrics[METRIC_AVG_LATENCY] * 2.5;
    
    /* Add noise */
    for (int i = 0; i < METRIC_COUNT; i++) {
        double noise = ((double)rand() / RAND_MAX - 0.5) * 0.1;
        metrics[i] *= (1.0 + noise);
    }
}

int main(void) {
    srand(time(NULL));
    
    optimizer_t *opt = optimizer_create();
    if (!opt) {
        fprintf(stderr, "Failed to create optimizer\n");
        return 1;
    }
    
    /* Configure parameters */
    optimizer_add_param(opt, "clients", 1, 1000, 10, 50);
    optimizer_add_param(opt, "threads", 1, 32, 1, 4);
    optimizer_add_param(opt, "io_threads", 1, 16, 1, 4);
    optimizer_add_param(opt, "reader_threads", 1, 32, 2, 8);
    optimizer_add_param(opt, "writer_threads", 1, 16, 1, 4);
    
    /* Add constraints: p99 latency < 1.0ms */
    optimizer_add_constraint(opt, METRIC_P99_LATENCY, CONSTRAINT_LESS_THAN, 1.0);
    
    /* Objective: maximize QPS */
    optimizer_set_objective(opt, METRIC_QPS, OBJECTIVE_MAXIMIZE);
    
    printf("Starting optimization...\n");
    printf("Goal: Maximize QPS subject to p99_latency < 1.0ms\n\n");
    
    status_t status;
    int config[5];
    double metrics[METRIC_COUNT] = {0};
    
    do {
        /* Get current configuration */
        optimizer_get_current_config(opt, config, 5);
        
        /* Run simulated benchmark */
        simulate_benchmark(config[0], config[1], config[2], config[3], config[4], metrics);
        
        /* Feed results to optimizer */
        status = optimizer_step(opt, metrics);
        
        /* Print status every 5 iterations */
        if (opt->total_iterations % 5 == 0) {
            optimizer_print_status(opt, stdout);
        }
        
        if (status == STATUS_WAIT_STABILIZATION) {
            printf("Iteration %d: Waiting for stabilization...\n", opt->total_iterations);
        }
        
    } while (status != STATUS_CONVERGED && opt->total_iterations < 100);
    
    printf("\n=== FINAL RESULTS ===\n");
    optimizer_print_status(opt, stdout);
    
    const measurement_t *best = optimizer_get_best_solution(opt);
    if (best) {
        printf("\nOptimal Configuration Found:\n");
        printf("  QPS: %.0f\n", best->metrics[METRIC_QPS]);
        printf("  Avg Latency: %.3fms\n", best->metrics[METRIC_AVG_LATENCY]);
        printf("  P99 Latency: %.3fms\n", best->metrics[METRIC_P99_LATENCY]);
    } else {
        printf("\nNo feasible solution found!\n");
    }
    
    optimizer_destroy(opt);
    return 0;
}

#endif /* OPTIMIZER_EXAMPLE */