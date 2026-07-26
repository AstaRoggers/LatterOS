#ifndef PANIC_H
#define PANIC_H

#include "cpu_context.h"

#include <stdint.h>

void kernel_panic(
    const char *message,
    uint64_t exception_number
);

void kernel_panic_context(
    const char *message,
    const cpu_context_t *context
);

#endif
