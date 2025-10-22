#include "plugin_common.h"
#include <string.h> // ok to use (By Piazza)
#include <stdlib.h> // ok to use (By Piazza)

// Small helper that detects the line <END>
static int is_end_line(const char *s)
{
    return s && strcmp(s, "<END>") == 0;
}

// rotator: Moves every character in the string one position to the right. The last character wraps around to the front.
static const char *plugin_transform(const char *input_str)
{
    // if the input pointer is NULL return empty string so the pipeline keeps running
    if (!input_str)
        return strdup("");

    // if its <END>, just pass without transform
    if (is_end_line(input_str))
        return strdup(input_str);

    // computre length
    size_t input_len = strlen(input_str);
    // nothing to rotate
    if (input_len <= 1)
        return strdup(input_str);

    // Allocate space for the result
    char *output_str = (char *)malloc(input_len + 1);
    // out of memory safe
    if (!output_str)
        return NULL;

    output_str[0] = input_str[input_len - 1];         // last moves to first
    memcpy(output_str + 1, input_str, input_len - 1); // shift original letters right
    output_str[input_len] = '\0';                     // NUL terminate

    return output_str; // heap string
}

// init details
const char *plugin_init(int queue_size)
{
    return common_plugin_init(plugin_transform, "rotator", queue_size);
}
