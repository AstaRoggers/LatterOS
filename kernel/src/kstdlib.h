#ifndef KSTDLIB_H
#define KSTDLIB_H

#include <stdbool.h>
#include <stdint.h>

bool kparse_u64(
    const char *text,
    uint64_t *value
);

bool kparse_i64(
    const char *text,
    int64_t *value
);

int64_t katoi(const char *text);

char *kutoa(
    uint64_t value,
    char *buffer,
    uint32_t base
);

char *kitoa(
    int64_t value,
    char *buffer,
    uint32_t base
);

#endif
