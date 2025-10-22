#include "plugin_common.h"
#include <stdio.h>   // ok to use (by Piazza)
#include <stdlib.h>  // ok to use (by Piazza)
#include <string.h>  // ok to use (by Piazza)
#include <pthread.h> // ok to use (by Piazza)

// static plugin context used by the plugin .so
// one global state per plugin shared object
static plugin_context_t global_plugin_context = {
    .name = NULL,             // name used in logs
    .queue = NULL,            // pointer to its input queue
    .consumer_thread = 0,     // thread that consumes from the queue
    .next_place_work = NULL,  // function pointer to next stages place_work
    .process_function = NULL, // plugins transform function
    .initialized = 0,
    .finished = 0};

// Dont submit log_error and log_info
/*
// diagnostic logging for errors (stderr)
void log_error(plugin_context_t *context, const char *message)
{
    const char *plugin_name = (context && context->name) ? context->name : "UNKNOWN";
    fprintf(stderr, "[ERROR][%s] - %s\n", plugin_name, message ? message : "(null)");
}

// diagnostic logging for info (stderr)
void log_info(plugin_context_t *context, const char *message)
{
    const char *plugin_name = (context && context->name) ? context->name : "UNKNOWN";
    fprintf(stderr, "[INFO][%s] - %s\n", plugin_name, message ? message : "(null)");
}
*/

// expose plugin name for loader (required by the SDK)
const char *plugin_get_name(void)
{
    return global_plugin_context.name ? global_plugin_context.name : "";
}

// helper: propagate <END> downstream if were chained
static void forward_end_if_attached(plugin_context_t *plugin_ctx)
{
    if (plugin_ctx->next_place_work)
    {
        const char *err = plugin_ctx->next_place_work("<END>");
        if (err)
            log_error(plugin_ctx, err);
    }
}

// thread entry: consume, transform, forward
void *plugin_consumer_thread(void *arg)
{
    plugin_context_t *plugin_ctx = (plugin_context_t *)arg;
    if (!plugin_ctx || !plugin_ctx->queue || !plugin_ctx->process_function)
    {
        // defensive: mark finished on bad setup
        if (plugin_ctx)
            plugin_ctx->finished = 1;
        return NULL;
    }

    // main consumer loop
    for (;;)
    {
        // blocking get
        char *in = consumer_producer_get(plugin_ctx->queue);

        if (!in)
        {
            log_error(plugin_ctx, "get() failed");
            break; // exit the loop gracefully
        }

        // shutdown marker
        if (strcmp(in, "<END>") == 0)
        {
            forward_end_if_attached(plugin_ctx); // pass END to next stage if exists
            free(in);                            // done with the input copy
            break;                               // exit loop
        }

        // We always free in and we free out only if (out != in) to avoid double free
        const char *out = plugin_ctx->process_function(in);
        if (!out)
        {
            log_error(plugin_ctx, "process_function returned NULL");
            free(in);
            continue;
        }

        if (plugin_ctx->next_place_work)
        {
            const char *err = plugin_ctx->next_place_work(out);
            if (err)
                log_error(plugin_ctx, err);
        }

        if (out != in)
        {
            free((char *)out);
        }
        free(in);
    }

    plugin_ctx->finished = 1;                             // mark thread done
    consumer_producer_signal_finished(plugin_ctx->queue); // wake up waiters on finished flag
    return NULL;                                          // return NULL on success
}

// shared init called by each plugin’s plugin_init()
const char *common_plugin_init(const char *(*process_function)(const char *), const char *name, int queue_size)
{
    // error messages
    if (global_plugin_context.initialized)
        return "plugin already initialized";
    if (!process_function)
        return "process_function is NULL";
    if (queue_size < 1)
        return "queue_size must be > 0";

    // allocate queue object
    global_plugin_context.queue = (consumer_producer_t *)malloc(sizeof(*global_plugin_context.queue));
    if (!global_plugin_context.queue)
        return "failed to allocate queue";

    // init queue internals
    const char *queue_init_error = consumer_producer_init(global_plugin_context.queue, queue_size);
    if (queue_init_error)
    {
        free(global_plugin_context.queue);
        global_plugin_context.queue = NULL;
        return queue_init_error;
    }

    global_plugin_context.name = name ? name : "plugin";       // store name for logs
    global_plugin_context.process_function = process_function; // store transform function
    global_plugin_context.next_place_work = NULL;              // not attached yet
    global_plugin_context.finished = 0;                        // consumer not finished

    // spawn consumer thread
    int return_code = pthread_create(&global_plugin_context.consumer_thread, NULL, plugin_consumer_thread, &global_plugin_context);
    // clean up on thread creation fail
    if (return_code != 0)
    {
        consumer_producer_destroy(global_plugin_context.queue);
        free(global_plugin_context.queue);
        global_plugin_context.queue = NULL;
        global_plugin_context.process_function = NULL;
        return "failed to create consumer thread";
    }

    global_plugin_context.initialized = 1; // init succeeded
    // log_info(&global_plugin_context, "initialized"); // removed because of submission
    return NULL; // success
}

// destroy queue and join consumer
const char *plugin_fini(void)
{
    if (!global_plugin_context.initialized)
        return "plugin not initialized";

    // Always join if we created the thread
    if (global_plugin_context.consumer_thread)
    {
        int return_code = pthread_join(global_plugin_context.consumer_thread, NULL);
        if (return_code != 0)
            return "pthread_join failed in plugin_fini";
        global_plugin_context.consumer_thread = 0;
    }

    consumer_producer_destroy(global_plugin_context.queue); // tear down queue internals
    free(global_plugin_context.queue);                      // free queue object
    global_plugin_context.queue = NULL;

    // clear pointers/flags
    global_plugin_context.process_function = NULL;
    global_plugin_context.next_place_work = NULL;
    global_plugin_context.initialized = 0;
    return NULL; // success
}

// enqueue work for this plugin
const char *plugin_place_work(const char *str)
{
    if (!global_plugin_context.initialized)
        return "plugin not initialized";
    if (!global_plugin_context.queue)
        return "queue not available";

    // put() returns NULL on success
    return consumer_producer_put(global_plugin_context.queue, str);
}

// set/clear forwarding function to next plugin’s put()
void plugin_attach(const char *(*next_place_work)(const char *))
{
    global_plugin_context.next_place_work = next_place_work;
    log_info(&global_plugin_context, next_place_work ? "attached to next plugin" : "detached from next plugin");
}

// wait on queue’s finished monitor
const char *plugin_wait_finished(void)
{
    if (!global_plugin_context.initialized)
        return "plugin not initialized";

    int return_code = consumer_producer_wait_finished(global_plugin_context.queue);
    if (return_code != 0)
        return "wait_finished failed";

    return NULL; // success
}