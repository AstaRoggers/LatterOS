#ifndef KSTRING_H
#define KSTRING_H

#include <stddef.h>

size_t kstrlen(const char *text);

int kstrcmp(
    const char *first,
    const char *second
);

int kstrncmp(
    const char *first,
    const char *second,
    size_t count
);

char *kstrcpy(
    char *destination,
    const char *source
);

char *kstrncpy(
    char *destination,
    const char *source,
    size_t count
);

char *kstrcat(
    char *destination,
    const char *source
);

const char *kstrchr(
    const char *text,
    int character
);

#endif
