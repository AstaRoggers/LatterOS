#include "kstdlib.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool character_is_space(char character)
{
    return (
        character == ' ' ||
        character == '\t' ||
        character == '\n' ||
        character == '\r' ||
        character == '\f' ||
        character == '\v'
    );
}

static bool parse_unsigned_magnitude(
    const char *text,
    uint64_t maximum,
    uint64_t *value,
    size_t *end_index
)
{
    size_t index = 0;
    uint64_t result = 0;
    bool found_digit = false;

    while (character_is_space(text[index]))
    {
        index++;
    }

    while (
        text[index] >= '0' &&
        text[index] <= '9'
    )
    {
        uint64_t digit =
            (uint64_t)(text[index] - '0');

        if (
            result >
            (maximum - digit) / 10
        )
        {
            return false;
        }

        result = result * 10 + digit;
        found_digit = true;
        index++;
    }

    if (!found_digit)
    {
        return false;
    }

    while (character_is_space(text[index]))
    {
        index++;
    }

    if (text[index] != '\0')
    {
        return false;
    }

    *value = result;

    if (end_index != NULL)
    {
        *end_index = index;
    }

    return true;
}

bool kparse_u64(
    const char *text,
    uint64_t *value
)
{
    if (
        text == NULL ||
        value == NULL
    )
    {
        return false;
    }

    size_t index = 0;

    while (character_is_space(text[index]))
    {
        index++;
    }

    if (text[index] == '+')
    {
        index++;
    }
    else if (text[index] == '-')
    {
        return false;
    }

    return parse_unsigned_magnitude(
        &text[index],
        UINT64_MAX,
        value,
        NULL
    );
}

bool kparse_i64(
    const char *text,
    int64_t *value
)
{
    if (
        text == NULL ||
        value == NULL
    )
    {
        return false;
    }

    size_t index = 0;

    while (character_is_space(text[index]))
    {
        index++;
    }

    bool negative = false;

    if (text[index] == '-')
    {
        negative = true;
        index++;
    }
    else if (text[index] == '+')
    {
        index++;
    }

    uint64_t maximum =
        negative ?
        ((uint64_t)INT64_MAX + 1ULL) :
        (uint64_t)INT64_MAX;

    uint64_t magnitude;

    if (
        !parse_unsigned_magnitude(
            &text[index],
            maximum,
            &magnitude,
            NULL
        )
    )
    {
        return false;
    }

    if (negative)
    {
        if (
            magnitude ==
            (uint64_t)INT64_MAX + 1ULL
        )
        {
            *value = INT64_MIN;
        }
        else
        {
            *value = -(int64_t)magnitude;
        }
    }
    else
    {
        *value = (int64_t)magnitude;
    }

    return true;
}

int64_t katoi(const char *text)
{
    int64_t value = 0;

    (void)kparse_i64(
        text,
        &value
    );

    return value;
}

char *kutoa(
    uint64_t value,
    char *buffer,
    uint32_t base
)
{
    static const char digits[] =
        "0123456789abcdef";

    if (
        buffer == NULL ||
        base < 2 ||
        base > 16
    )
    {
        return NULL;
    }

    char reversed[65];
    size_t length = 0;

    do
    {
        reversed[length] =
            digits[value % base];

        value /= base;
        length++;
    }
    while (value != 0);

    for (
        size_t index = 0;
        index < length;
        index++
    )
    {
        buffer[index] =
            reversed[length - index - 1];
    }

    buffer[length] = '\0';
    return buffer;
}

char *kitoa(
    int64_t value,
    char *buffer,
    uint32_t base
)
{
    if (buffer == NULL)
    {
        return NULL;
    }

    if (
        base == 10 &&
        value < 0
    )
    {
        uint64_t magnitude =
            (uint64_t)(-(value + 1)) + 1ULL;

        buffer[0] = '-';

        if (
            kutoa(
                magnitude,
                &buffer[1],
                base
            ) == NULL
        )
        {
            return NULL;
        }

        return buffer;
    }

    return kutoa(
        (uint64_t)value,
        buffer,
        base
    );
}
