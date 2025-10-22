#include "plugin_common.h"
#include <string.h> // ok to use (By Piazza)
#include <stdlib.h> // ok to use (By Piazza)

// Small helper that detects the line <END>
static int is_end_line(const char *s)
{
    return s && strcmp(s, "<END>") == 0;
}

// flipper: Reverses the order of characters in the string
static const char *plugin_transform(const char *input_str)
{
    // if the input pointer is NULL return empty string so the pipeline keeps running
    if (!input_str)
        return strdup("");

    // if its <END>, just pass without transform
    if (is_end_line(input_str))
        return strdup(input_str);

    // compute length
    size_t input_len = strlen(input_str);
    char *output_str = (char *)malloc(input_len + 1); // allocate result
    // out of memort safe
    if (!output_str)
        return NULL;

    // reverse copy
    for (size_t read_index = 0; read_index < input_len; ++read_index)
    {
        output_str[read_index] = input_str[input_len - 1 - read_index];
    }
    output_str[input_len] = '\0'; // NUL terminate

    // return the reversed string
    return output_str;
}

// init details
const char *plugin_init(int queue_size)
{
    return common_plugin_init(plugin_transform, "flipper", queue_size);
}
