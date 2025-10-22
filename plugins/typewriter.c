#include "plugin_common.h"
#include <stdio.h>  // ok to use (By Piazza)
#include <string.h> // ok to use (By Piazza)
#include <stdlib.h> // ok to use (By Piazza)
#include <unistd.h> // ok to use (By Piazza)

// Small helper that detects the line <END>
static int is_end_line(const char *s)
{
    return s && strcmp(s, "<END>") == 0;
}

// typewriter: Simulates a typewriter effect by printing each character with a 100ms delay (you can use the usleep function).
static const char *plugin_transform(const char *input_str)
{
    // if the input pointer is NULL return empty string so the pipeline keeps running
    if (!input_str)
        return strdup("");

    // if its <END>, just pass without printing
    if (is_end_line(input_str))
        return strdup(input_str);

    // empty inputs produce no partial tag
    if (*input_str)
    {
        // puts the tag
        fputs("[typewriter] ", stdout);
        fflush(stdout);

        // // iterate each char
        for (const char *p = input_str; *p; ++p)
        {
            fputc(*p, stdout); // print char
            fflush(stdout);    // print every char with delay
            usleep(100000);    // 100-ms delay
        }

        fputc('\n', stdout); // add newline
        fflush(stdout);      // print instantly
    }

    return strdup(input_str); // pass-through
}

// init details
const char *plugin_init(int queue_size)
{
    return common_plugin_init(plugin_transform, "typewriter", queue_size);
}
