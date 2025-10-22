#define _GNU_SOURCE // allowed as said in the Q&A video

#include <dlfcn.h>  // ok to use (Piazza)
#include <errno.h>  // ok to use (Piazza)
#include <stdio.h>  // ok to use (Piazza)
#include <stdlib.h> // ok to use (Piazza)
#include <stdarg.h>
#include <string.h> // ok to use (Piazza)
#include <unistd.h> // ok to use (Piazza)

#include "plugins/plugin_sdk.h" // the contract

// Bonus - Sets a compile-time flag that checks if we can use dlmopen
#if defined(__GLIBC__)
#include <link.h> // used by dlmopen
#define DLMOPEN_SUPPORTED 1
#else
#define DLMOPEN_SUPPORTED 0
#endif

// Bonus - Allow turning choosing dlmopen / dlopen at runtime
#define DLMOPEN_ENV_VAR "ANALYZER_DLMOPEN"
static int use_dlmopen(void)
{
#if !DLMOPEN_SUPPORTED
    return 0; // Not available, shouldnt happen on ubuntu
#else
    const char *val = getenv(DLMOPEN_ENV_VAR);
    return (val == NULL) || (strcmp(val, "0") != 0);
#endif
}

// Step 1 - Holds parsed Command-Line info to keep clean main function
typedef struct
{
    int queue_size;
    int selected_plugin_count;
} pipeline_configuration_t;

// Step 2 - function pointer typedefs matching plugin_sdk.h so dlsym casts are type-safe
typedef const char *(*plugin_init_func_t)(int queue_size);
typedef const char *(*plugin_fini_func_t)(void);
typedef const char *(*plugin_place_work_func_t)(const char *str);
typedef void (*plugin_attach_func_t)(const char *(*next_place_work)(const char *));
typedef const char *(*plugin_wait_finished_func_t)(void);

// Step 2 - Copied from instructions (Stores the plugins .so)
typedef struct
{
    plugin_init_func_t init;
    plugin_fini_func_t fini;
    plugin_place_work_func_t place_work;
    plugin_attach_func_t attach;
    plugin_wait_finished_func_t wait_finished;
    char *name;
    void *handle;
} plugin_handle_t;

// -------------------------------------------- Helpers --------------------------------------------------------

static const char *g_prog = NULL;

static void print_error_and_exit(int exit_code, int print_usage, const char *prefix_line, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void usage_help_message(const char *prog);

// Helper to handle errors
static void print_error_and_exit(int exit_code, int print_usage, const char *prefix_line, const char *fmt, ...)
{
    // If theres an error message, print it first
    if (prefix_line && *prefix_line)
        fputs(prefix_line, stderr);

    va_list ap;                // Declares a variable to iterate over the variadic arguments
    va_start(ap, fmt);         // Initializes ap to read the arguments that come after fmt
    vfprintf(stderr, fmt, ap); // Prints formatted error message to stderr using the argument list
    va_end(ap);                // Cleans up the variadic argument list
    fputc('\n', stderr);       // Ensures the error ends with a newline

    // If print_usage is 1, print the big usage text to stdout
    if (print_usage && g_prog)
        usage_help_message(g_prog);

    // exit with the requested exit code
    exit(exit_code);
}

// Step 1 Helper function - used when args are missing
// this usage message printed to stdout
static void usage_help_message(const char *prog)
{
    fprintf(stdout,
            "Usage: %s <queue_size> <plugin1> <plugin2> ... <pluginN>\n"
            "\n"
            "Arguments:\n"
            "  queue_size  Maximum number of items in each plugin's queue\n"
            "  plugin1..N  Names of plugins to load (without .so extension)\n"
            "\n"
            "Available plugins:\n"
            "  logger      - Logs all strings that pass through\n"
            "  typewriter  - Simulates typewriter effect with delays\n"
            "  uppercaser  - Converts strings to uppercase\n"
            "  rotator     - Move every character to the right. Last character moves to the beginning.\n"
            "  flipper     - Reverses the order of characters\n"
            "  expander    - Expands each character with spaces\n"
            "\n"
            "Example:\n"
            "  %s 20 uppercaser rotator logger\n"
            "echo 'hello' | %s 20 uppercaser rotator logger\n"
            "echo '<END>' | %s 20 uppercaser rotator logger\n",
            prog, prog, prog, prog);
}

// -------------------------------------------- Main Application Steps --------------------------------------------------------

// Step 1 - parse, allocate names[] and plugins[]
static void parse_command_line(int argc, char **argv, pipeline_configuration_t *cfg, plugin_handle_t **plugins_out, char ***names_out)
{
    // we need at least 3 arguments: name of program, queue size, at least one plugin
    if (argc < 3)
    {
        // print error to stderr, print the usage message, exit with code 1
        print_error_and_exit(1, 1, NULL, "Missing arguments");
    }

    // Parsing queue size
    char *endptr_after_number = NULL;                                                 // catches the numbers part
    long queue_size_value = strtol(argv[1], &endptr_after_number, 10);                // takes the second argument (queue size)
    if (!endptr_after_number || *endptr_after_number != '\0' || queue_size_value < 1) // checks: parsing works, 0 < queue_size
    {
        // print error to stderr, print the usage message, exit with code 1
        print_error_and_exit(1, 1, NULL, "invalid queue size (must be greater than 0): '%s'", argv[1]);
    }
    cfg->queue_size = (int)queue_size_value; // puts the number into queue_size if its good

    // Parsing the plugins
    cfg->selected_plugin_count = argc - 2; // Everything after argv[1] (queue size) is treated as a plugin name, counts them
    if (cfg->selected_plugin_count <= 0)   // Double-checks theres at least one plugin - not supposed to ever be true
    {
        // print error to stderr, print the usage message, exit with code 1
        print_error_and_exit(1, 1, NULL, "No plugins specified");
    }

    // Allocates arrays to hold per-plugin state (stages) and the plugin names (names)
    // use calloc and error out if memory allocation fails.
    plugin_handle_t *plugins = calloc((size_t)cfg->selected_plugin_count, sizeof(plugin_handle_t));
    if (!plugins)
        // prints error to stderr, exit with code 1
        print_error_and_exit(1, 0, NULL, "calloc plugins failed");

    char **names = calloc((size_t)cfg->selected_plugin_count, sizeof(char *));
    if (!names)
        // prints error to stderr, exit with code 1
        print_error_and_exit(1, 0, NULL, "calloc names failed");

    // fill names from argv (just point to argv memory)
    for (int i = 0; i < cfg->selected_plugin_count; ++i)
    {
        names[i] = argv[i + 2];
    }

    // returns the newly allocated arrays
    *plugins_out = plugins;
    *names_out = names;
}

// Step 2 - loads the plugin
static void load_plugin(plugin_handle_t *ph, const char *plugin_name)
{
    // Supports explicit paths - only for testing (annoying bugs)
    char so_path[512];
    if (strchr(plugin_name, '/'))
    {
        // treat as explicit path
        snprintf(so_path, sizeof so_path, "%s", plugin_name);
    }
    else
    {
        // load from ./output/<name>.so
        snprintf(so_path, sizeof so_path, "./output/%s.so", plugin_name);
    }

    // Save an owned copy of the plugin name
    ph->name = strdup(plugin_name);
    if (!ph->name)
        // print error to stderr, print usage message, exit with code 1
        print_error_and_exit(1, 1, "Step 2: Load Plugin Shared Objects failed\n", "out of memory for plugin name '%s'", plugin_name);

// Load with RTLD_NOW | RTLD_LOCAL, report dlerror on failure
// Load each instance in its own namespace
#if DLMOPEN_SUPPORTED
    if (use_dlmopen())
    {
        ph->handle = dlmopen(LM_ID_NEWLM, so_path, RTLD_NOW | RTLD_LOCAL);
        if (!ph->handle)
            // print error to stderr, print usage to stdout, exit with code 1
            print_error_and_exit(1, 1, "Step 2: Load Plugin Shared Objects failed\n", "dlmopen('%s') error: %s", so_path, dlerror());
    }
    else
#endif
    {
        ph->handle = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
        if (!ph->handle)
            // print error to stderr, print usage to stdout, exit with code 1
            print_error_and_exit(1, 1, "Step 2: Load Plugin Shared Objects failed\n", "dlopen('%s') error: %s", so_path, dlerror());
    }

    // Resolve each symbol with dlerror() checks after each dlsym
    dlerror();
    *(void **)(&ph->init) = dlsym(ph->handle, "plugin_init");
    const char *e = dlerror();
    if (e || !ph->init)
        print_error_and_exit(1, 1,
                             "Step 2: Load Plugin Shared Objects failed\n",
                             "missing required symbol '%s' in '%s': %s",
                             "plugin_init", so_path, e ? e : "(null)");

    dlerror();
    *(void **)(&ph->place_work) = dlsym(ph->handle, "plugin_place_work");
    e = dlerror();
    if (e || !ph->place_work)
        print_error_and_exit(1, 1,
                             "Step 2: Load Plugin Shared Objects failed\n",
                             "missing required symbol '%s' in '%s': %s",
                             "plugin_place_work", so_path, e ? e : "(null)");

    dlerror();
    *(void **)(&ph->attach) = dlsym(ph->handle, "plugin_attach");
    e = dlerror();
    if (e || !ph->attach)
        print_error_and_exit(1, 1,
                             "Step 2: Load Plugin Shared Objects failed\n",
                             "missing required symbol '%s' in '%s': %s",
                             "plugin_attach", so_path, e ? e : "(null)");

    dlerror();
    *(void **)(&ph->wait_finished) = dlsym(ph->handle, "plugin_wait_finished");
    e = dlerror();
    if (e || !ph->wait_finished)
        print_error_and_exit(1, 1,
                             "Step 2: Load Plugin Shared Objects failed\n",
                             "missing required symbol '%s' in '%s': %s",
                             "plugin_wait_finished", so_path, e ? e : "(null)");

    dlerror();
    *(void **)(&ph->fini) = dlsym(ph->handle, "plugin_fini");
    e = dlerror();
    if (e || !ph->fini)
        print_error_and_exit(1, 1,
                             "Step 2: Load Plugin Shared Objects failed\n",
                             "missing required symbol '%s' in '%s': %s",
                             "plugin_fini", so_path, e ? e : "(null)");
}

// Step 2: Loads the plugin (dont need to check uniqe)
static void step2_load_or_exit(const pipeline_configuration_t *cfg, char **names, plugin_handle_t *plugins)
{
    // load the plugins
    for (int i = 0; i < cfg->selected_plugin_count; ++i)
    {
        load_plugin(&plugins[i], names[i]);
    }
}

// Step 3 - Call init(queue_size) for each plugin.
// On failure: clean up already loaded plugins, print to stderr, and exit with code 2.
static void init_plugin(plugin_handle_t *p, int plugins_count, int queue_size)
{
    for (int i = 0; i < plugins_count; ++i)
    {
        const char *err = p[i].init(queue_size);
        // init returns NULL on success,
        if (err)
        {
            // Preserve the failing plugin name before we free it
            const char *name_ptr = p[i].name ? p[i].name : "(unknown)";
            char failing_name[256];
            snprintf(failing_name, sizeof failing_name, "%s", name_ptr);

            // Prints context to stderr
            fprintf(stderr, "Initialize Plugins failed\n");
            fprintf(stderr, "plugin_init(%s) error: %s\n", failing_name, err);

            // Roll back already-initialized/loaded plugins, including the failing one
            for (int j = i; j >= 0; --j)
            {
                if (p[j].fini)
                {
                    const char *ferr = p[j].fini();
                    // fini returns NULL on success, error message on failure
                    if (ferr)
                        fprintf(stderr, "plugin_fini(%s) error during rollback: %s\n", p[j].name ? p[j].name : "(null)", ferr);
                }
                if (p[j].handle)
                    dlclose(p[j].handle);

                free(p[j].name);
                p[j].name = NULL;
                p[j].handle = NULL;
            }

            // Ensure anything after i that we may have partially touched is also closed/freed
            for (int j = i + 1; j < plugins_count; ++j)
            {
                if (p[j].handle)
                    dlclose(p[j].handle);
                free(p[j].name);
                p[j].name = NULL;
                p[j].handle = NULL;
            }

            // exit with code 2, Summarize the primary failure
            print_error_and_exit(2, /*print_usage=*/0, /*prefix=*/NULL, "plugin_init(%s) error: %s", failing_name, err);
        }
    }
}

// Step 4 - attach the plugin to the next one in the pipeline
static void wire_plugins(plugin_handle_t *p, int n)
{
    // Basic check
    if (!p || n <= 0)
        // error to stderr, exit with code 1
        print_error_and_exit(1, 0, NULL, "wire_plugins: invalid pipeline (n=%d)", n);

    // Loops over all plugins except the last one. attach them to the next one
    for (int i = 0; i + 1 < n; ++i)
    {
        // Check required function pointers exist
        if (!p[i].attach)
            // print error to stderr, exit with code 1
            print_error_and_exit(1, 0, NULL, "wire_plugins: plugin %d ('%s') missing attach",
                                 i, p[i].name ? p[i].name : "(null)");

        if (!p[i + 1].place_work)
            // print error to stderr, exit with code 1
            print_error_and_exit(1, 0, NULL, "wire_plugins: plugin %d ('%s') missing place_work",
                                 i + 1, p[i + 1].name ? p[i + 1].name : "(null)");

        // Do the wiring
        p[i].attach(p[i + 1].place_work);
    }
}

// Step 5 - read inputs, strip newline, send to first plugin
// If the line is exactly "<END>", send it and break the loop
static void feed_input(plugin_handle_t *first_plugin)
{
    enum
    {
        MAX_LINE_LEN = 1024 // max length is 1024 characters
    };
    char line_buffer[MAX_LINE_LEN + 1]; // room for the terminating '\0'

    // basic check (Step 4 already checks this, but to be sure)
    if (!first_plugin || !first_plugin->place_work)
        // print to stderr, exit with code 1
        print_error_and_exit(1, 0, NULL, "feed_input: invalid pipeline entry");

    // Use fgets() to read lines up to 1024 characters
    while (fgets(line_buffer, sizeof line_buffer, stdin))
    {
        size_t line_len = strlen(line_buffer);
        int had_trailing_newline = 0;

        // Removes the trailing newline so plugins dont see \n
        if (line_len && line_buffer[line_len - 1] == '\n')
        {
            line_buffer[--line_len] = '\0';
            had_trailing_newline = 1;
        }

        // Just in case - If buffer filled exactly and no '\n' was read, swallow a lone newline next
        if (!had_trailing_newline && line_len == (sizeof line_buffer - 1))
        {
            int next_char = fgetc(stdin);
            if (next_char != '\n' && next_char != EOF)
                ungetc(next_char, stdin);
        }

        const char *err;

        // check if the line is exactly "<END>"
        if (strcmp(line_buffer, "<END>") == 0)
        {
            err = first_plugin->place_work("<END>"); // send it into the pipeline
            if (err)
                // error to stderr, exit code 1
                print_error_and_exit(1, 0, NULL, "place_work('<END>') error: %s", err);
            break; // Stop reading more lines
        }

        // if the line is normal, send it to the pipeline
        err = first_plugin->place_work(line_buffer);
        if (err)
            // returns null on success
            print_error_and_exit(1, 0, NULL, "place_work error: %s", err);
    }

    // If input ended with an error, show it
    if (ferror(stdin))
        print_error_and_exit(1, 0, NULL, "stdin read error");
}

// Step 6 + 7 - Wait for plugins to finish and cleanup
static void teardown(plugin_handle_t *p, int n)
{
    // loop that waits for all the plugins to finish
    for (int i = 0; i < n; ++i)
    {
        // just to make sure
        if (!p[i].wait_finished)
            print_error_and_exit(1, 0, NULL, "plugin %d ('%s') missing wait_finished",
                                 i, p[i].name ? p[i].name : "(null)");

        const char *werr = p[i].wait_finished();
        // If there was an error string, abort the program with a clear message that includes the plugin’s name
        if (werr)
            print_error_and_exit(1, 0, NULL, "plugin_wait_finished(%s) error: %s",
                                 p[i].name ? p[i].name : "(null)", werr);
    }

    // reverse loop to cleanup the piplelines safely
    for (int i = n - 1; i >= 0; --i)
    {
        // just to make sure
        if (!p[i].fini)
            print_error_and_exit(1, 0, NULL, "plugin %d ('%s') missing fini",
                                 i, p[i].name ? p[i].name : "(null)");

        const char *ferr = p[i].fini(); // call each plugin's fini
        // If the plugin’s finalization reports an error, abort with details
        if (ferr)
            print_error_and_exit(1, 0, NULL, "plugin_fini(%s) error: %s",
                                 p[i].name ? p[i].name : "(null)", ferr);

        // Unload the shared object only after the plugin finalized
        if (p[i].handle)
            dlclose(p[i].handle);

        free(p[i].name); // Free the heap-allocated copy of the plugin’s name
    }
}

// ---------------------------------------------- Main --------------------------------------------------
int main(int argc, char **argv)
{
    g_prog = argv[0];

    // Step 1: parse and alloc
    pipeline_configuration_t cfg = {0};
    plugin_handle_t *plugins = NULL;
    char **names = NULL;
    parse_command_line(argc, argv, &cfg, &plugins, &names);

    // Step 2: Load Plugins Shared Objects
    step2_load_or_exit(&cfg, names, plugins);

    // Step 3: Initialize Plugins
    init_plugin(plugins, cfg.selected_plugin_count, cfg.queue_size);

    // Step 4: Attach Plugins Together
    wire_plugins(plugins, cfg.selected_plugin_count);

    // Step 5: Read Input from STDIN
    feed_input(&plugins[0]);

    // Steps 6 + 7: Wait for Plugins to Finish and Cleanup
    teardown(plugins, cfg.selected_plugin_count);

    // free top-level arrays allocated in main
    free(names);
    free(plugins);

    // Step 8: Finalize
    printf("Pipeline shutdown complete\n");
    return 0; // exit with code 0
}