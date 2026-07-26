#ifndef STACKTRACE_H
#define STACKTRACE_H

#include "cpu_context.h"

#include <stddef.h>
#include <stdint.h>

#define STACKTRACE_MAX_FRAMES 16

size_t stacktrace_collect(
    uint64_t instruction_pointer,
    uint64_t frame_pointer,
    uint64_t stack_pointer,
    uint64_t *frames,
    size_t capacity
);

size_t stacktrace_collect_context(
    const cpu_context_t *context,
    uint64_t *frames,
    size_t capacity
);

size_t stacktrace_collect_current(
    uint64_t *frames,
    size_t capacity
);

void stacktrace_print_current(void);

#endif
