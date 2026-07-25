#ifndef PANIC_H
#define PANIC_H

#include <stdint.h>

void kernel_panic(
    const char *message,
    uint64_t exception_number
);

#endif