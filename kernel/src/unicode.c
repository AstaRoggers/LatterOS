#include "unicode.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool decode_utf8(
    const uint8_t *input,
    uint32_t *codepoint,
    size_t *length
)
{
    if (
        input == NULL ||
        codepoint == NULL ||
        length == NULL
    )
    {
        return false;
    }

    uint8_t first = input[0];

    if (first < 0x80U)
    {
        *codepoint = first;
        *length = 1;
        return true;
    }

    if (
        first >= 0xC2U &&
        first <= 0xDFU
    )
    {
        uint8_t second = input[1];

        if ((second & 0xC0U) != 0x80U)
        {
            return false;
        }

        *codepoint =
            ((uint32_t)(first & 0x1FU) << 6) |
            (uint32_t)(second & 0x3FU);

        *length = 2;
        return true;
    }

    if (
        first >= 0xE0U &&
        first <= 0xEFU
    )
    {
        uint8_t second = input[1];
        uint8_t third = input[2];

        if (
            (second & 0xC0U) != 0x80U ||
            (third & 0xC0U) != 0x80U
        )
        {
            return false;
        }

        if (
            (first == 0xE0U && second < 0xA0U) ||
            (first == 0xEDU && second >= 0xA0U)
        )
        {
            return false;
        }

        *codepoint =
            ((uint32_t)(first & 0x0FU) << 12) |
            ((uint32_t)(second & 0x3FU) << 6) |
            (uint32_t)(third & 0x3FU);

        *length = 3;
        return true;
    }

    if (
        first >= 0xF0U &&
        first <= 0xF4U
    )
    {
        uint8_t second = input[1];
        uint8_t third = input[2];
        uint8_t fourth = input[3];

        if (
            (second & 0xC0U) != 0x80U ||
            (third & 0xC0U) != 0x80U ||
            (fourth & 0xC0U) != 0x80U
        )
        {
            return false;
        }

        if (
            (first == 0xF0U && second < 0x90U) ||
            (first == 0xF4U && second >= 0x90U)
        )
        {
            return false;
        }

        *codepoint =
            ((uint32_t)(first & 0x07U) << 18) |
            ((uint32_t)(second & 0x3FU) << 12) |
            ((uint32_t)(third & 0x3FU) << 6) |
            (uint32_t)(fourth & 0x3FU);

        *length = 4;
        return true;
    }

    return false;
}

static size_t encoded_utf8_length(uint32_t codepoint)
{
    if (codepoint <= 0x7FU)
    {
        return 1;
    }

    if (codepoint <= 0x7FFU)
    {
        return 2;
    }

    if (codepoint <= 0xFFFFU)
    {
        return 3;
    }

    if (codepoint <= 0x10FFFFU)
    {
        return 4;
    }

    return 0;
}

static bool encode_utf8(
    uint32_t codepoint,
    char *output,
    size_t capacity,
    size_t *written
)
{
    size_t length = encoded_utf8_length(codepoint);

    if (
        output == NULL ||
        written == NULL ||
        length == 0 ||
        capacity < length ||
        (codepoint >= 0xD800U &&
         codepoint <= 0xDFFFU)
    )
    {
        return false;
    }

    if (length == 1)
    {
        output[0] = (char)codepoint;
    }
    else if (length == 2)
    {
        output[0] =
            (char)(0xC0U | (codepoint >> 6));
        output[1] =
            (char)(0x80U | (codepoint & 0x3FU));
    }
    else if (length == 3)
    {
        output[0] =
            (char)(0xE0U | (codepoint >> 12));
        output[1] =
            (char)(
                0x80U |
                ((codepoint >> 6) & 0x3FU)
            );
        output[2] =
            (char)(0x80U | (codepoint & 0x3FU));
    }
    else
    {
        output[0] =
            (char)(0xF0U | (codepoint >> 18));
        output[1] =
            (char)(
                0x80U |
                ((codepoint >> 12) & 0x3FU)
            );
        output[2] =
            (char)(
                0x80U |
                ((codepoint >> 6) & 0x3FU)
            );
        output[3] =
            (char)(0x80U | (codepoint & 0x3FU));
    }

    *written = length;
    return true;
}

bool unicode_utf8_validate(const char *text)
{
    if (text == NULL)
    {
        return false;
    }

    const uint8_t *bytes =
        (const uint8_t *)text;

    size_t offset = 0;

    while (bytes[offset] != 0)
    {
        uint32_t codepoint;
        size_t length;

        if (
            !decode_utf8(
                bytes + offset,
                &codepoint,
                &length
            ) ||
            codepoint == 0 ||
            codepoint > 0x10FFFFU
        )
        {
            return false;
        }

        offset += length;
    }

    return true;
}

bool unicode_utf8_to_utf16(
    const char *text,
    uint16_t *output,
    size_t output_capacity,
    size_t *output_units
)
{
    if (
        text == NULL ||
        output == NULL ||
        output_capacity == 0
    )
    {
        return false;
    }

    const uint8_t *bytes =
        (const uint8_t *)text;

    size_t input_offset = 0;
    size_t units = 0;

    while (bytes[input_offset] != 0)
    {
        uint32_t codepoint;
        size_t length;

        if (
            !decode_utf8(
                bytes + input_offset,
                &codepoint,
                &length
            ) ||
            codepoint == 0 ||
            codepoint > 0x10FFFFU
        )
        {
            return false;
        }

        if (codepoint <= 0xFFFFU)
        {
            if (
                codepoint >= 0xD800U &&
                codepoint <= 0xDFFFU
            )
            {
                return false;
            }

            if (units >= output_capacity)
            {
                return false;
            }

            output[units++] = (uint16_t)codepoint;
        }
        else
        {
            if (units + 2 > output_capacity)
            {
                return false;
            }

            uint32_t value =
                codepoint - 0x10000U;

            output[units++] =
                (uint16_t)(
                    0xD800U |
                    (value >> 10)
                );

            output[units++] =
                (uint16_t)(
                    0xDC00U |
                    (value & 0x3FFU)
                );
        }

        input_offset += length;
    }

    if (output_units != NULL)
    {
        *output_units = units;
    }

    return true;
}

bool unicode_utf16_to_utf8(
    const uint16_t *input,
    size_t input_units,
    char *output,
    size_t output_capacity,
    size_t *output_bytes
)
{
    if (
        input == NULL ||
        output == NULL ||
        output_capacity == 0
    )
    {
        return false;
    }

    size_t input_index = 0;
    size_t output_index = 0;

    while (input_index < input_units)
    {
        uint32_t codepoint =
            input[input_index++];

        if (
            codepoint == 0 ||
            codepoint == 0xFFFFU
        )
        {
            break;
        }

        if (
            codepoint >= 0xD800U &&
            codepoint <= 0xDBFFU
        )
        {
            if (input_index >= input_units)
            {
                return false;
            }

            uint32_t low =
                input[input_index++];

            if (
                low < 0xDC00U ||
                low > 0xDFFFU
            )
            {
                return false;
            }

            codepoint =
                0x10000U +
                ((codepoint - 0xD800U) << 10) +
                (low - 0xDC00U);
        }
        else if (
            codepoint >= 0xDC00U &&
            codepoint <= 0xDFFFU
        )
        {
            return false;
        }

        size_t written;

        if (
            output_index >= output_capacity - 1 ||
            !encode_utf8(
                codepoint,
                output + output_index,
                output_capacity - output_index - 1,
                &written
            )
        )
        {
            return false;
        }

        output_index += written;
    }

    output[output_index] = '\0';

    if (output_bytes != NULL)
    {
        *output_bytes = output_index;
    }

    return true;
}
