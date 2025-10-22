#include "consumer_producer.h"
#include "monitor.h"
#include <stdlib.h>  // ok by Piazza - calloc malloc etc
#include <string.h>  // ok by Piazza - memset memcpy
#include <pthread.h> // ok by PDF - mutex

// --------------------------------------------- Internal synchronization -------------------------------------------------
typedef struct cp_lock_entry
{
    const void *queue_key;            // consumer_producer_t* as key
    pthread_mutex_t queue_mutex;      // per-queue mutex
    struct cp_lock_entry *next_entry; // singly-linked list
} cp_lock_entry_t;

// Globals
static cp_lock_entry_t *g_queue_lock_list_head = NULL;                      // list head
static pthread_mutex_t g_queue_lock_list_mutex = PTHREAD_MUTEX_INITIALIZER; // protects the list

// Ensure there is a dedicated mutex for a given queue object, returns a pointer to that mutex
// Create one if missing
static pthread_mutex_t *cp_register_lock(const void *queue_key)
{
    pthread_mutex_lock(&g_queue_lock_list_mutex); // lock the global list while we search/modify

    // iterate existing entries
    for (cp_lock_entry_t *entry = g_queue_lock_list_head; entry; entry = entry->next_entry)
    {
        // if we found an existing lock for this queue
        if (entry->queue_key == queue_key)
        {
            pthread_mutex_unlock(&g_queue_lock_list_mutex); // release global list lock
            return &entry->queue_mutex;                     // return its mutex (the lock)
        }
    }

    // else - allocate a new list node
    cp_lock_entry_t *entry = (cp_lock_entry_t *)calloc(1, sizeof(*entry));

    // if theres an error
    if (!entry)
    {
        pthread_mutex_unlock(&g_queue_lock_list_mutex); // release global list lock
        return NULL;                                    // signal failure
    }

    entry->queue_key = queue_key;                  // bind node to this queue key
    pthread_mutex_init(&entry->queue_mutex, NULL); // init the per-queue mutex with default attrs
    entry->next_entry = g_queue_lock_list_head;    // push-front into the singly-linked list
    g_queue_lock_list_head = entry;                // update list head

    pthread_mutex_unlock(&g_queue_lock_list_mutex); // release global list lock
    return &entry->queue_mutex;                     // return the new mutex
}

// Remove and destroy the mutex entry for a specific queue
static void cp_destroy_lock(const void *queue_key)
{
    pthread_mutex_lock(&g_queue_lock_list_mutex); // lock the global list while we work

    cp_lock_entry_t **link_to_current_entry_ptr = &g_queue_lock_list_head; // pointer-to-pointer
    while (*link_to_current_entry_ptr)                                     // walk the list
    {
        if ((*link_to_current_entry_ptr)->queue_key == queue_key) // found the node for this queue
        {
            cp_lock_entry_t *entry_to_remove = *link_to_current_entry_ptr; // keep a handle to free
            *link_to_current_entry_ptr = entry_to_remove->next_entry;      // unlink from list
            pthread_mutex_destroy(&entry_to_remove->queue_mutex);          // destroy the per-queue mutex
            free(entry_to_remove);                                         // free node
            break;                                                         // done
        }
        link_to_current_entry_ptr = &(*link_to_current_entry_ptr)->next_entry; // if we didnt find - continue searching
    }
    pthread_mutex_unlock(&g_queue_lock_list_mutex); // release global list lock
}

// Look up the per-queue mutex for a given queue object.
// returns NULL if not found
static pthread_mutex_t *cp_get_lock(const void *queue_key)
{
    pthread_mutex_lock(&g_queue_lock_list_mutex); // lock the global list while we work

    for (cp_lock_entry_t *entry = g_queue_lock_list_head; entry; entry = entry->next_entry) // iterate entries
    {
        if (entry->queue_key == queue_key) // found it
        {
            pthread_mutex_unlock(&g_queue_lock_list_mutex); // release global list lock
            return &entry->queue_mutex;                     // return its mutex
        }
    }
    pthread_mutex_unlock(&g_queue_lock_list_mutex); // release global list lock
    return NULL;                                    // if not found (shouldn't happen after init)
}

// -------------------------------------- API implementation -------------------------------------------------------

// Initialize a ring-buffer queue with monitors and a per-queue mutex
// return NULL on success or error message on failure
const char *consumer_producer_init(consumer_producer_t *q, int capacity)
{
    // Makes sure the capacity is ok and the pointer is ok
    if (!q || capacity < 1)
        return "invalid args";

    // Zero the struct so all integers start at 0 and pointers at NULL
    memset(q, 0, sizeof(*q));

    // Allocate the ring buffer; holds pointers to strings + handles error
    q->items = (char **)calloc((size_t)capacity, sizeof(char *));
    if (!q->items)
        return "calloc failed";

    q->capacity = capacity; // set capacity
    q->count = 0;           // empty in the start
    q->head = 0;            // read index
    q->tail = 0;            // write index

    // Initialize monitors: not full, not empty, and finished + checks
    // not full - signals when space becomes available
    if (monitor_init(&q->not_full_monitor) != 0)
    {
        free(q->items);
        q->items = NULL;
        return "monitor_init(not_full) failed";
    }
    // not empty - signal when an item becomes available
    if (monitor_init(&q->not_empty_monitor) != 0)
    {
        monitor_destroy(&q->not_full_monitor);
        free(q->items);
        q->items = NULL;
        return "monitor_init(not_empty) failed";
    }
    // finished - signal when producers are finished
    if (monitor_init(&q->finished_monitor) != 0)
    {
        monitor_destroy(&q->not_empty_monitor);
        monitor_destroy(&q->not_full_monitor);
        free(q->items);
        q->items = NULL;
        return "monitor_init(finished) failed";
    }

    // Create per-queue mutex
    pthread_mutex_t *queue_lock = cp_register_lock(q);
    if (!queue_lock)
    {
        // destroy monitors if lock allocation fails
        monitor_destroy(&q->finished_monitor);
        monitor_destroy(&q->not_empty_monitor);
        monitor_destroy(&q->not_full_monitor);
        free(q->items);
        q->items = NULL;
        return "lock allocation failed";
    }

    return NULL; // success
}

// Destroy a consumer-producer queue and free its resources
void consumer_producer_destroy(consumer_producer_t *q)
{
    // finish
    if (!q)
        return;

    // get the per-queue mutex
    pthread_mutex_t *queue_lock = cp_get_lock(q);

    if (queue_lock)
        pthread_mutex_lock(queue_lock); // guard against concurrent put/get

    // free any remaining items defensively
    // if ring exists
    if (q->items)
    {
        // free and remaining owned strings
        for (int i = 0; i < q->count; ++i)
        {
            int idx = (q->head + i) % q->capacity;
            free(q->items[idx]);
        }
        free(q->items);
        q->items = NULL; // null for safety
    }

    if (queue_lock)
        pthread_mutex_unlock(queue_lock); // release per-queue mutex

    // destroy monitors after no one uses the queue anymore
    monitor_destroy(&q->not_full_monitor);
    monitor_destroy(&q->not_empty_monitor);
    monitor_destroy(&q->finished_monitor);

    // remove and destroy the per-queue lock
    cp_destroy_lock(q);
}

// add an item to the queue (producer), blocks if full. Null for success, error message for failure
// Copies the contents of the string internally (deep copy). The caller retains ownership
const char *consumer_producer_put(consumer_producer_t *q, const char *item)
{
    // validate inputs
    if (!q || !item)
        return "invalid args";

    pthread_mutex_t *queue_lock = cp_get_lock(q); // get per-queue mutex
    // for safety - should not happen after init
    if (!queue_lock)
        return "queue lock missing";

    pthread_mutex_lock(queue_lock); // begin critical section
    // full - must wait
    while (q->count == q->capacity)
    {
        monitor_reset(&q->not_full_monitor);
        pthread_mutex_unlock(queue_lock); // drop mutex before waiting

        // wait until space signaled
        int w = monitor_wait(&q->not_full_monitor);
        if (w != 0)
            return "wait(not_full) failed";

        pthread_mutex_lock(queue_lock); // reacquire and recheck loop condition
    }

    size_t L = strlen(item) + 1;           // compute bytes to copy (include '\0')
    q->items[q->tail] = (char *)malloc(L); // allocate storage for the copy

    // handle out of memory errors
    if (!q->items[q->tail])
    {
        pthread_mutex_unlock(queue_lock); // leave critical section
        return "out of memory";           // signal failure
    }

    memcpy(q->items[q->tail], item, L);    // copy string into ring slot
    q->tail = (q->tail + 1) % q->capacity; // advance tail (wrap around)
    q->count++;                            // increment count

    monitor_signal(&q->not_empty_monitor); // Wake a potential getter
    pthread_mutex_unlock(queue_lock);      // end critical section

    return NULL; // success
}

// Remove an item from the queue (consumer) and returns it, blocks if empty.
// string item if good, wait if empty
// Returns a newly heap-allocated string that the caller must free
char *consumer_producer_get(consumer_producer_t *q)
{
    // invalid queue
    if (!q)
        return NULL;

    // get per-queue mutex
    pthread_mutex_t *queue_lock = cp_get_lock(q);

    // shouldn't happen after init
    if (!queue_lock)
        return NULL;

    for (;;) // block until non-empty loop
    {
        pthread_mutex_lock(queue_lock); // begin critical section
        if (q->count > 0)               // any item available?
        {
            char *s = q->items[q->head];           // take pointer to front item (ownership to caller)
            q->head = (q->head + 1) % q->capacity; // advance head
            q->count--;                            // decrement count
            monitor_signal(&q->not_full_monitor);  // wake a producer waiting for space
            pthread_mutex_unlock(queue_lock);      // leave critical section
            return s;                              // return the dequeued string
        }
        monitor_reset(&q->not_empty_monitor);
        pthread_mutex_unlock(queue_lock); // empty - leave critical section

        // wait until someone enqueues
        int w = monitor_wait(&q->not_empty_monitor);
        if (w != 0)
            return NULL; // <-- propagate failure
    }
}

// Notify anyone waiting for finished that production is done
void consumer_producer_signal_finished(consumer_producer_t *q)
{
    // if NULL do nothing
    if (!q)
        return;

    // once signaled, all waiters will see finished
    monitor_signal(&q->finished_monitor);
}

// Block until finished is signaled. return 0 on success, -1 if q is null
int consumer_producer_wait_finished(consumer_producer_t *q)
{
    // validate
    if (!q)
        return -1;

    // block until finished is signaled
    int w = monitor_wait(&q->finished_monitor);
    return (w == 0) ? 0 : -1; // 0 on success, -1 on failure
}
