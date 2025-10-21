/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2024-present, Zvi Schneider
 * 
 * Progress Bar Implementation
 *
 * Provides wget-style progress bars with automatic rate calculation and
 * time-based throttling to avoid terminal spam.
 *
 * This file is part of valkey-search-benchmark and is licensed under the
 * BSD 3-Clause License. See the LICENSE file in the root directory.
 */

#include "utils.h"
#include "zmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

/* ANSI color codes */
#define COLOR_GREEN "\033[32m"
#define COLOR_BLUE "\033[34m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_RESET "\033[0m"
#define COLOR_BOLD "\033[1m"

/* Progress bar characters */
#define BAR_COMPLETE "="
#define BAR_CURRENT ">"
#define BAR_INCOMPLETE " "

/* Default settings */
#define DEFAULT_BAR_WIDTH 50
#define DEFAULT_UPDATE_INTERVAL_MS 100
#define MIN_UPDATE_INTERVAL_MS 50

// /* Get terminal width */
// static int getTerminalWidth(void) {
//     struct winsize w;
//     if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) {
//         return w.ws_col;
//     }
//     return 80; /* Default fallback */
// }

/* Calculate time difference in milliseconds */
static long timeDiffMs(struct timeval *start, struct timeval *end) {
    return (end->tv_sec - start->tv_sec) * 1000 +
           (end->tv_usec - start->tv_usec) / 1000;
}

/* Format large numbers with K/M/G suffixes */
static void formatNumber(char *buf, size_t buf_size, uint64_t num) {
    if (num >= 1000000000) {
        snprintf(buf, buf_size, "%.1fG", num / 1000000000.0);
    } else if (num >= 1000000) {
        snprintf(buf, buf_size, "%.1fM", num / 1000000.0);
    } else if (num >= 1000) {
        snprintf(buf, buf_size, "%.1fK", num / 1000.0);
    } else {
        snprintf(buf, buf_size, "%lu", (unsigned long)num);
    }
}

/* Format rate (items per second) */
static void formatRate(char *buf, size_t buf_size, double rate) {
    if (rate >= 1000000.0) {
        snprintf(buf, buf_size, "%.1fM/s", rate / 1000000.0);
    } else if (rate >= 1000.0) {
        snprintf(buf, buf_size, "%.1fK/s", rate / 1000.0);
    } else {
        snprintf(buf, buf_size, "%.0f/s", rate);
    }
}

/* Format time in seconds */
static void formatTime(char *buf, size_t buf_size, long seconds) {
    if (seconds >= 3600) {
        snprintf(buf, buf_size, "%ldh%ldm", seconds / 3600, (seconds % 3600) / 60);
    } else if (seconds >= 60) {
        snprintf(buf, buf_size, "%ldm%lds", seconds / 60, seconds % 60);
    } else {
        snprintf(buf, buf_size, "%lds", seconds);
    }
}

void initProgressBar(progressBar *bar, uint64_t total, uint64_t rate_total, const char *label) {
    if (!bar) return;
    
    memset(bar, 0, sizeof(progressBar));
    bar->total = total;
    bar->rate_total = rate_total;
    bar->label = label ? zstrdup(label) : NULL;
    bar->bar_width = DEFAULT_BAR_WIDTH;
    bar->update_interval_ms = DEFAULT_UPDATE_INTERVAL_MS;
    bar->enabled = 1; /* Force enable - always show progress */
    bar->last_percent = -1;
    
    gettimeofday(&bar->start_time, NULL);
    bar->last_update = bar->start_time;
    
    pthread_mutex_init(&bar->mutex, NULL);
    
    /* Don't print initial label - let the caller print context */
}

/* Internal render function (assumes lock is held) */
static void renderProgressBar(progressBar *bar, int force_update) {
    if (!bar->enabled || bar->finished) return;
    
    struct timeval now;
    gettimeofday(&now, NULL);
    
    /* Throttle updates unless forced */
    if (!force_update) {
        long ms_since_update = timeDiffMs(&bar->last_update, &now);
        if (ms_since_update < bar->update_interval_ms) {
            return;
        }
    }
    
    bar->last_update = now;
    
    /* Calculate percentage */
    int percent = bar->total > 0 ? (int)((bar->current * 100) / bar->total) : 0;
    if (percent > 100) percent = 100;
    
    /* Skip update if percentage and count haven't changed significantly */
    if (!force_update && percent == bar->last_percent && 
        bar->current == bar->last_current &&
        bar->rate_current == bar->last_rate_current) {
        return;
    }
    
    bar->last_percent = percent;
    bar->last_current = bar->current;
    bar->last_rate_current = bar->rate_current;
    
    /* Calculate rate and time */
    long elapsed_ms = timeDiffMs(&bar->start_time, &now);
    double elapsed_sec = elapsed_ms / 1000.0;
    double rate = (elapsed_sec > 0.1) ? (bar->current / elapsed_sec) : 0.0;
    
    /* Calculate secondary rate if enabled */
    double secondary_rate = 0.0;
    if (bar->rate_total > 0 && elapsed_sec > 0.1) {
        secondary_rate = bar->rate_current / elapsed_sec;
    }
    
    /* Estimate remaining time */
    long eta_sec = 0;
    if (rate > 0 && bar->total > bar->current) {
        eta_sec = (long)((bar->total - bar->current) / rate);
    }
    
    /* Build progress bar */
    int filled = (bar->bar_width * percent) / 100;
    
    /* Format numbers */
    char current_str[32], total_str[32], rate_str[32], eta_str[32];
    char rate_current_str[32], secondary_rate_str[32];
    formatNumber(current_str, sizeof(current_str), bar->current);
    formatNumber(total_str, sizeof(total_str), bar->total);
    formatRate(rate_str, sizeof(rate_str), rate);
    formatTime(eta_str, sizeof(eta_str), eta_sec);
    
    if (bar->rate_total > 0) {
        formatNumber(rate_current_str, sizeof(rate_current_str), bar->rate_current);
        formatRate(secondary_rate_str, sizeof(secondary_rate_str), secondary_rate);
    }
    
    /* Print progress bar in wget style */
    printf("\r%s%3d%%%s [", COLOR_GREEN, percent, COLOR_RESET);
    
    /* Draw bar */
    int i;
    for (i = 0; i < bar->bar_width; i++) {
        if (i < filled - 1) {
            printf("%s", BAR_COMPLETE);
        } else if (i == filled - 1 && filled < bar->bar_width) {
            printf("%s", BAR_CURRENT);
        } else {
            printf("%s", BAR_INCOMPLETE);
        }
    }
    
    printf("] %s%s/%s%s %s%s%s",
           COLOR_BOLD, current_str, total_str, COLOR_RESET,
           COLOR_BLUE, rate_str, COLOR_RESET);
    
    /* Show secondary metric if enabled */
    if (bar->rate_total > 0) {
        printf(" | %s%s%s %s%s%s",
               COLOR_BOLD, rate_current_str, COLOR_RESET,
               COLOR_BLUE, secondary_rate_str, COLOR_RESET);
    }
    
    if (eta_sec > 0 && bar->current < bar->total) {
        printf(" ETA: %s", eta_str);
    }
    
    /* Always clear to end of line to avoid artifacts */
    printf("\033[K");  /* ANSI escape: clear from cursor to end of line */
    
    fflush(stdout);
}

void updateProgressBar(progressBar *bar, uint64_t current, uint64_t rate_current) {
    if (!bar || !bar->enabled) return;
    
    pthread_mutex_lock(&bar->mutex);
    bar->current = current;
    bar->rate_current = rate_current;
    renderProgressBar(bar, 0);
    pthread_mutex_unlock(&bar->mutex);
}

void forceUpdateProgressBar(progressBar *bar, uint64_t current, uint64_t rate_current) {
    if (!bar || !bar->enabled) return;
    
    pthread_mutex_lock(&bar->mutex);
    bar->current = current;
    bar->rate_current = rate_current;
    renderProgressBar(bar, 1);
    pthread_mutex_unlock(&bar->mutex);
}

void incrementProgressBar(progressBar *bar, uint64_t increment, uint64_t rate_increment) {
    if (!bar || !bar->enabled) return;
    
    pthread_mutex_lock(&bar->mutex);
    bar->current += increment;
    bar->rate_current += rate_increment;
    renderProgressBar(bar, 0);
    pthread_mutex_unlock(&bar->mutex);
}

void finishProgressBar(progressBar *bar) {
    if (!bar) return;
    
    pthread_mutex_lock(&bar->mutex);
    
    if (bar->enabled && !bar->finished) {
        /* Force final update to show 100% */
        bar->current = bar->total;
        renderProgressBar(bar, 1);
        
        /* Move to next line but keep the progress bar visible */
        printf("\n");
        fflush(stdout);
        
        /* Reset ANSI attributes for subsequent output */
        printf("\033[0m");
        fflush(stdout);
    }
    
    bar->finished = 1;
    pthread_mutex_unlock(&bar->mutex);
    
    /* Cleanup */
    if (bar->label) {
        zfree(bar->label);
        bar->label = NULL;
    }
    pthread_mutex_destroy(&bar->mutex);
}

void setProgressBarUpdateInterval(progressBar *bar, int interval_ms) {
    if (!bar) return;
    
    pthread_mutex_lock(&bar->mutex);
    bar->update_interval_ms = (interval_ms < MIN_UPDATE_INTERVAL_MS) ? 
                              MIN_UPDATE_INTERVAL_MS : interval_ms;
    pthread_mutex_unlock(&bar->mutex);
}