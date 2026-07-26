#ifndef KSTDIO_H
#define KSTDIO_H

#include <stdarg.h>
#include <stddef.h>

int kvsnprintf(
    char *buffer,
    size_t capacity,
    const char *format,
    va_list arguments
);

int ksnprintf(
    char *buffer,
    size_t capacity,
    const char *format,
    ...
);

int kprintf(
    const char *format,
    ...
);

#endif
