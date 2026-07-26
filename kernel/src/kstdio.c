#include "kstdio.h"

#include "terminal.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KPRINTF_BUFFER_SIZE 512

#define LENGTH_DEFAULT 0
#define LENGTH_LONG    1
#define LENGTH_LLONG   2
#define LENGTH_SIZE    3

typedef struct
{
    char *buffer;
    size_t capacity;
    size_t length;
} format_output_t;

static void output_character(
    format_output_t *output,
    char character
)
{
    if (
        output->capacity > 0 &&
        output->length + 1 <
            output->capacity
    )
    {
        output->buffer[output->length] =
            character;
    }

    output->length++;
}

static void output_repeat(
    format_output_t *output,
    char character,
    size_t count
)
{
    for (
        size_t index = 0;
        index < count;
        index++
    )
    {
        output_character(
            output,
            character
        );
    }
}

static size_t text_length(const char *text)
{
    size_t length = 0;

    while (text[length] != '\0')
    {
        length++;
    }

    return length;
}

static void output_text(
    format_output_t *output,
    const char *text
)
{
    if (text == NULL)
    {
        text = "(null)";
    }

    for (
        size_t index = 0;
        text[index] != '\0';
        index++
    )
    {
        output_character(
            output,
            text[index]
        );
    }
}

static size_t unsigned_to_reversed(
    uint64_t value,
    uint32_t base,
    bool uppercase,
    char *reversed
)
{
    const char *digits =
        uppercase ?
        "0123456789ABCDEF" :
        "0123456789abcdef";

    size_t length = 0;

    do
    {
        reversed[length] =
            digits[value % base];

        value /= base;
        length++;
    }
    while (value != 0);

    return length;
}

static void output_unsigned(
    format_output_t *output,
    uint64_t value,
    uint32_t base,
    bool uppercase,
    size_t width,
    char padding,
    const char *prefix
)
{
    char reversed[65];

    size_t digits =
        unsigned_to_reversed(
            value,
            base,
            uppercase,
            reversed
        );

    size_t prefix_length =
        prefix == NULL ?
        0 :
        text_length(prefix);

    size_t total_length =
        prefix_length + digits;

    if (
        padding == ' ' &&
        width > total_length
    )
    {
        output_repeat(
            output,
            ' ',
            width - total_length
        );
    }

    if (prefix != NULL)
    {
        output_text(
            output,
            prefix
        );
    }

    if (
        padding == '0' &&
        width > total_length
    )
    {
        output_repeat(
            output,
            '0',
            width - total_length
        );
    }

    while (digits > 0)
    {
        digits--;

        output_character(
            output,
            reversed[digits]
        );
    }
}

static void output_signed(
    format_output_t *output,
    int64_t value,
    size_t width,
    char padding
)
{
    bool negative = value < 0;

    uint64_t magnitude =
        negative ?
        (uint64_t)(-(value + 1)) + 1ULL :
        (uint64_t)value;

    output_unsigned(
        output,
        magnitude,
        10,
        false,
        width,
        padding,
        negative ? "-" : NULL
    );
}

static uint64_t read_unsigned_argument(
    va_list *arguments,
    uint8_t length
)
{
    switch (length)
    {
        case LENGTH_LONG:
            return (uint64_t)va_arg(
                *arguments,
                unsigned long
            );

        case LENGTH_LLONG:
            return (uint64_t)va_arg(
                *arguments,
                unsigned long long
            );

        case LENGTH_SIZE:
            return (uint64_t)va_arg(
                *arguments,
                size_t
            );

        default:
            return (uint64_t)va_arg(
                *arguments,
                unsigned int
            );
    }
}

static int64_t read_signed_argument(
    va_list *arguments,
    uint8_t length
)
{
    switch (length)
    {
        case LENGTH_LONG:
            return (int64_t)va_arg(
                *arguments,
                long
            );

        case LENGTH_LLONG:
            return (int64_t)va_arg(
                *arguments,
                long long
            );

        case LENGTH_SIZE:
            return (int64_t)va_arg(
                *arguments,
                ptrdiff_t
            );

        default:
            return (int64_t)va_arg(
                *arguments,
                int
            );
    }
}

int kvsnprintf(
    char *buffer,
    size_t capacity,
    const char *format,
    va_list arguments
)
{
    if (
        format == NULL ||
        (capacity > 0 && buffer == NULL)
    )
    {
        return -1;
    }

    format_output_t output = {
        .buffer = buffer,
        .capacity = capacity,
        .length = 0
    };

    va_list values;
    va_copy(values, arguments);

    for (
        size_t index = 0;
        format[index] != '\0';
        index++
    )
    {
        if (format[index] != '%')
        {
            output_character(
                &output,
                format[index]
            );

            continue;
        }

        index++;

        if (format[index] == '%')
        {
            output_character(
                &output,
                '%'
            );

            continue;
        }

        char padding = ' ';

        if (format[index] == '0')
        {
            padding = '0';
            index++;
        }

        size_t width = 0;

        while (
            format[index] >= '0' &&
            format[index] <= '9'
        )
        {
            width =
                width * 10 +
                (size_t)(
                    format[index] - '0'
                );

            index++;
        }

        uint8_t length = LENGTH_DEFAULT;

        if (format[index] == 'l')
        {
            length = LENGTH_LONG;
            index++;

            if (format[index] == 'l')
            {
                length = LENGTH_LLONG;
                index++;
            }
        }
        else if (format[index] == 'z')
        {
            length = LENGTH_SIZE;
            index++;
        }

        char specifier = format[index];

        switch (specifier)
        {
            case 'c':
            {
                char character =
                    (char)va_arg(values, int);

                if (width > 1)
                {
                    output_repeat(
                        &output,
                        padding,
                        width - 1
                    );
                }

                output_character(
                    &output,
                    character
                );

                break;
            }

            case 's':
            {
                const char *text =
                    va_arg(values, const char *);

                if (text == NULL)
                {
                    text = "(null)";
                }

                size_t length_value =
                    text_length(text);

                if (width > length_value)
                {
                    output_repeat(
                        &output,
                        padding,
                        width - length_value
                    );
                }

                output_text(
                    &output,
                    text
                );

                break;
            }

            case 'd':
            case 'i':
                output_signed(
                    &output,
                    read_signed_argument(
                        &values,
                        length
                    ),
                    width,
                    padding
                );
                break;

            case 'u':
                output_unsigned(
                    &output,
                    read_unsigned_argument(
                        &values,
                        length
                    ),
                    10,
                    false,
                    width,
                    padding,
                    NULL
                );
                break;

            case 'x':
            case 'X':
                output_unsigned(
                    &output,
                    read_unsigned_argument(
                        &values,
                        length
                    ),
                    16,
                    specifier == 'X',
                    width,
                    padding,
                    NULL
                );
                break;

            case 'p':
                output_unsigned(
                    &output,
                    (uint64_t)(uintptr_t)
                        va_arg(values, void *),
                    16,
                    false,
                    sizeof(uintptr_t) * 2 + 2,
                    '0',
                    "0x"
                );
                break;

            case '\0':
                index--;
                break;

            default:
                output_character(
                    &output,
                    '%'
                );

                output_character(
                    &output,
                    specifier
                );
                break;
        }
    }

    va_end(values);

    if (capacity > 0)
    {
        size_t terminator =
            output.length < capacity ?
            output.length :
            capacity - 1;

        buffer[terminator] = '\0';
    }

    if (output.length > (size_t)INT32_MAX)
    {
        return INT32_MAX;
    }

    return (int)output.length;
}

int ksnprintf(
    char *buffer,
    size_t capacity,
    const char *format,
    ...
)
{
    va_list arguments;
    va_start(arguments, format);

    int result =
        kvsnprintf(
            buffer,
            capacity,
            format,
            arguments
        );

    va_end(arguments);
    return result;
}

int kprintf(
    const char *format,
    ...
)
{
    char buffer[KPRINTF_BUFFER_SIZE];

    va_list arguments;
    va_start(arguments, format);

    int result =
        kvsnprintf(
            buffer,
            sizeof(buffer),
            format,
            arguments
        );

    va_end(arguments);

    if (result >= 0)
    {
        terminal_write(buffer);
    }

    return result;
}
