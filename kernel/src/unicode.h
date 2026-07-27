#ifndef UNICODE_H
#define UNICODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool unicode_utf8_validate(const char *text);

bool unicode_utf8_to_utf16(
    const char *text,
    uint16_t *output,
    size_t output_capacity,
    size_t *output_units
);

bool unicode_utf16_to_utf8(
    const uint16_t *input,
    size_t input_units,
    char *output,
    size_t output_capacity,
    size_t *output_bytes
);

#endif
