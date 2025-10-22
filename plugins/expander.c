#include "plugin_common.h"
#include <string.h> // ok to use (By Piazza)
#include <stdlib.h> // ok to use (By Piazza)

// Small helper that detects the line <END>
static int is_end_line(const char *s)
{
  return s && strcmp(s, "<END>") == 0;
}

// expander: Inserts a single white space between each character in the string.
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

  // if empty do nothing
  if (input_len == 0)
    return strdup("");

  // put space after each character, exept the last one (len = 2n-1)
  size_t output_len = (input_len * 2) - 1;

  // +1 for '\0'
  char *output_str = (char *)malloc(output_len + 1);

  // if allocation fails, return NULL to signal error
  if (!output_str)
    return NULL;

  // write index
  size_t write_index = 0;
  // goes over chars
  for (size_t read_index = 0; read_index < input_len; ++read_index)
  {
    // copy char
    output_str[write_index++] = input_str[read_index];
    // space between chars
    if (read_index + 1 < input_len)
      output_str[write_index++] = ' ';
  }
  output_str[output_len] = '\0'; // NUL terminate

  // heap string (freed by common layer)
  return output_str;
}

// init details
const char *plugin_init(int queue_size)
{
  return common_plugin_init(plugin_transform, "expander", queue_size);
}