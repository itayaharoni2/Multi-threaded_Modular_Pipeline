#include "plugin_common.h"
#include <ctype.h>  // ok to use (By Piazza)
#include <string.h> // ok to use (By Piazza)
#include <stdlib.h> // ok to use (By Piazza)

// Small helper that detects the line <END>
static int is_end_line(const char *s)
{
    return s && strcmp(s, "<END>") == 0;
}

// uppercaser: Converts all alphabetic characters in the string to uppercase.
static const char *plugin_transform(const char *input_str)
{
    // if the input pointer is NULL return empty string so the pipeline keeps running
    if (!input_str)
        return strdup("");

    // if its <END>, just pass without transform
    if (is_end_line(input_str))
        return strdup(input_str); // pass the sentinel on, but DO NOT print

    size_t input_len = strlen(input_str);             // compute length
    char *output_str = (char *)malloc(input_len + 1); // allocate result
    // out of memory safe
    if (!output_str)
        return NULL;

    // loop characters
    for (size_t read_index = 0; read_index < input_len; ++read_index)
    {
        unsigned char c = (unsigned char)input_str[read_index]; // safety: Always cast to unsigned char before calling toupper
        output_str[read_index] = (char)toupper(c);              // uppercase conversion
    }
    output_str[input_len] = '\0'; // NUL terminate

    // pass heap string
    return output_str;
}

// init details
const char *plugin_init(int queue_size)
{
    return common_plugin_init(plugin_transform, "uppercaser", queue_size);
}
