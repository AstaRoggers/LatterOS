#include "kstring.h"

#include <stddef.h>

size_t kstrlen(const char *text)
{
    size_t length = 0;

    while (text[length] != '\0')
    {
        length++;
    }

    return length;
}

int kstrcmp(
    const char *first,
    const char *second
)
{
    size_t index = 0;

    while (
        first[index] != '\0' &&
        first[index] == second[index]
    )
    {
        index++;
    }

    return
        (unsigned char)first[index] -
        (unsigned char)second[index];
}

int kstrncmp(
    const char *first,
    const char *second,
    size_t count
)
{
    for (
        size_t index = 0;
        index < count;
        index++
    )
    {
        unsigned char left =
            (unsigned char)first[index];

        unsigned char right =
            (unsigned char)second[index];

        if (left != right)
        {
            return (int)left - (int)right;
        }

        if (left == '\0')
        {
            return 0;
        }
    }

    return 0;
}

char *kstrcpy(
    char *destination,
    const char *source
)
{
    size_t index = 0;

    do
    {
        destination[index] = source[index];
        index++;
    }
    while (source[index - 1] != '\0');

    return destination;
}

char *kstrncpy(
    char *destination,
    const char *source,
    size_t count
)
{
    size_t index = 0;

    while (
        index < count &&
        source[index] != '\0'
    )
    {
        destination[index] = source[index];
        index++;
    }

    while (index < count)
    {
        destination[index] = '\0';
        index++;
    }

    return destination;
}

char *kstrcat(
    char *destination,
    const char *source
)
{
    size_t destination_length =
        kstrlen(destination);

    kstrcpy(
        &destination[destination_length],
        source
    );

    return destination;
}

const char *kstrchr(
    const char *text,
    int character
)
{
    char target = (char)character;

    for (size_t index = 0;; index++)
    {
        if (text[index] == target)
        {
            return &text[index];
        }

        if (text[index] == '\0')
        {
            return NULL;
        }
    }
}
