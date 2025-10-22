#ifndef MONITOR_H
#define MONITOR_H

#include <pthread.h>

/**
 * Monitor structure that can remember its state (manual-reset)
 * This solves the race condition where signals sent before waiting are lost
 */
typedef struct
{
    pthread_mutex_t mutex;    /* Mutex for thread safety */
    pthread_cond_t condition; /* Condition variable */
    int signaled;             /* Flag to remember if monitor was signaled */
} monitor_t;

/**
 * Initialize a monitor
 * @param monitor Pointer to monitor structure
 * @return 0 on success, -1 on failure
 */
int monitor_init(monitor_t *monitor);

/**
 * Destroy a monitor and free its resources
 * @param monitor Pointer to monitor structure
 */
void monitor_destroy(monitor_t *monitor);

/**
 * Signal a monitor (sets the monitor state)
 * @param monitor Pointer to monitor structure
 */
void monitor_signal(monitor_t *monitor);

/**
 * Reset a monitor (clears the monitor state)
 * @param monitor Pointer to monitor structure
 */
void monitor_reset(monitor_t *monitor);

/**
 * Wait for a monitor to be signaled (infinite wait)
 * @param monitor Pointer to monitor structure
 * @return 0 on success, -1 on error
 */
int monitor_wait(monitor_t *monitor);

// prevents doubling
#endif /* MONITOR_H */
