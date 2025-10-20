/**
 * Progress Bar Utility
 *
 * Provides wget-style progress bar functionality for long-running operations.
 * Thread-safe and designed for minimal terminal output overhead.
 *
 * Example usage:
 *   progressBar bar;
 *   initProgressBar(&bar, 1000000, 0, "Processing vectors");
 *   
 *   for (int i = 0; i < 1000000; i++) {
 *       // do work
 *       updateProgressBar(&bar, i + 1, 0);
 *   }
 *   
 *   finishProgressBar(&bar);
 */

#ifndef PROGRESS_BAR_H
#define PROGRESS_BAR_H

#include <stdint.h>
#include <pthread.h>
#include <sys/time.h>

/* Progress bar state */
typedef struct progressBar {
    uint64_t total;              /* Total items to process */
    uint64_t current;            /* Current items processed */
    uint64_t rate_total;         /* Total for secondary rate metric (optional) */
    uint64_t rate_current;       /* Current value for secondary rate metric */
    uint64_t last_rate_current;  /* Last displayed rate_current value */
    char *label;                 /* Progress label/description */
    struct timeval start_time;   /* Start time for rate calculation */
    struct timeval last_update;  /* Last update time for throttling */
    int bar_width;               /* Width of the progress bar */
    int last_percent;            /* Last displayed percentage */
    uint64_t last_current;       /* Last displayed current value */
    pthread_mutex_t mutex;       /* Thread safety */
    int update_interval_ms;      /* Minimum milliseconds between updates */
    int enabled;                 /* Whether progress bar is enabled */
    int finished;                /* Whether progress bar is finished */
} progressBar;

/**
 * Initialize a progress bar
 * @param bar Pointer to progressBar structure
 * @param total Total number of items to process (for percentage calculation)
 * @param rate_total Total for secondary rate metric (0 to disable)
 * @param label Description label for the progress bar
 */
void initProgressBar(progressBar *bar, uint64_t total, uint64_t rate_total, const char *label);

/**
 * Update progress bar with new count
 * @param bar Pointer to progressBar structure
 * @param current Current number of items processed
 * @param rate_current Current value for secondary rate metric
 */
void updateProgressBar(progressBar *bar, uint64_t current, uint64_t rate_current);

/**
 * Force update progress bar (ignoring time throttle)
 * @param bar Pointer to progressBar structure
 * @param current Current number of items processed
 * @param rate_current Current value for secondary rate metric
 */
void forceUpdateProgressBar(progressBar *bar, uint64_t current, uint64_t rate_current);

/**
 * Finish and cleanup progress bar
 * @param bar Pointer to progressBar structure
 */
void finishProgressBar(progressBar *bar);

/**
 * Thread-safe increment of progress bar (atomic add)
 * @param bar Pointer to progressBar structure
 * @param increment Amount to increment by
 * @param rate_increment Amount to increment rate counter by
 */
void incrementProgressBar(progressBar *bar, uint64_t increment, uint64_t rate_increment);

/**
 * Set custom update interval (default is 100ms)
 * @param bar Pointer to progressBar structure
 * @param interval_ms Milliseconds between updates
 */
void setProgressBarUpdateInterval(progressBar *bar, int interval_ms);

#endif /* PROGRESS_BAR_H */
