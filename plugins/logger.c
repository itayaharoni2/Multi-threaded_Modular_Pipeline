#include "plugin_common.h"
#include <stdio.h>  // ok to use (By Piazza)
#include <string.h> // ok to use (By Piazza)
#include <stdlib.h> // ok to use (By Piazza)

// Small helper that detects the line <END>
static int is_end_line(const char *s)
{
    return s && strcmp(s, "<END>") == 0;
}

// logger: Logs all strings that pass through to standard output
static const char *plugin_transform(const char *input_str)
{
    // if the input pointer is NULL return empty string so the pipeline keeps running
    if (!input_str)
        return strdup("");

    // if its <END>, just pass without printing
    if (is_end_line(input_str))
        return strdup(input_str);

    // Print the string to stdout
    fputs("[logger] ", stdout); // puts the tag like in the pdf
    fputs(input_str, stdout);   // write the content
    fputc('\n', stdout);        // newline so logs are separated
    fflush(stdout);             // flush so it appears immediately

    // return a heap-allocated copy for the pipeline
    // common layer will free this after forwarding
    return strdup(input_str);
}

// init details
const char *plugin_init(int queue_size)
{
    return common_plugin_init(plugin_transform, "logger", queue_size);
}
