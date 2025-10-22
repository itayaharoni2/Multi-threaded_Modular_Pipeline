#include "monitor.h"
#include <errno.h> // ok by Piazza

/* Helper: convert pthread error code to errno and return -1 on fail */
static int fail_with_errno(int return_code)
{
    if (return_code != 0)
        errno = return_code;
    return -1;
}

/* Initialize a monitor. Returns 0 on success, -1 on failure (errno set). */
int monitor_init(monitor_t *m)
{
    // makes sure the pointer points to available memory
    if (!m)
    {
        errno = EINVAL;
        return -1;
    }

    // init the monitors mutex and makes sure it worked, if it failed return -1
    int return_code = pthread_mutex_init(&m->mutex, NULL);
    if (return_code != 0)
        return fail_with_errno(return_code);

    // init the monitors cond var, if it failed release the mutex and return -1
    return_code = pthread_cond_init(&m->condition, NULL);
    if (return_code != 0)
    {
        int saved = return_code;
        (void)pthread_mutex_destroy(&m->mutex);
        return fail_with_errno(saved);
    }

    m->signaled = 0; // start cleared
    return 0;        // init worked
}

/* Destroy a monitor and free its resources*/
void monitor_destroy(monitor_t *m)
{
    if (!m)
        return;                                // if the pointer is empty return NULL
    (void)pthread_cond_destroy(&m->condition); // destroy the condition variable
    (void)pthread_mutex_destroy(&m->mutex);    // destroy the mutex
} // if its on the heap we need to free more resources i need to remember that

/* Signal a monitor (sets the monitor state)*/
void monitor_signal(monitor_t *m)
{
    if (!m)
        return;                                  // if the pointer is empty return NULL
    (void)pthread_mutex_lock(&m->mutex);         // we lock so we have safe access
    m->signaled = 1;                             // manual-reset: stays signaled
    (void)pthread_cond_broadcast(&m->condition); // wake all current waiters
    (void)pthread_mutex_unlock(&m->mutex);       // release the inner lock
}

/* Reset a monitor (clears the monitor state) */
void monitor_reset(monitor_t *m)
{
    if (!m)
        return;                            // if the pointer is empty return NULL
    (void)pthread_mutex_lock(&m->mutex);   // lock for safe access
    m->signaled = 0;                       // resets the flag
    (void)pthread_mutex_unlock(&m->mutex); // finished the action so realease the lock
}

/* wait for a monitor to be signaled (infinite wait), 0 on success -1 or failure*/
int monitor_wait(monitor_t *m)
{
    // return error if the pointer is not good
    if (!m)
    {
        errno = EINVAL;
        return -1;
    }

    // lock before taking action
    int return_code = pthread_mutex_lock(&m->mutex);
    if (return_code != 0)
        return fail_with_errno(return_code);

    // loop to make sure we wake up only when needed
    while (!m->signaled)
    {
        return_code = pthread_cond_wait(&m->condition, &m->mutex);
        if (return_code != 0)
        {
            (void)pthread_mutex_unlock(&m->mutex);
            return fail_with_errno(return_code);
        }
    }

    // after the action release the lock
    return_code = pthread_mutex_unlock(&m->mutex);
    if (return_code != 0)
        return fail_with_errno(return_code);
    return 0; // return 0 for success
}
