#ifndef SYSCALL_H
#define SYSCALL_H

#include "cpu_context.h"

#include <stdint.h>

#define SYSCALL_WRITE  1ULL
#define SYSCALL_EXIT   2ULL
#define SYSCALL_GETPID 3ULL

cpu_context_t *syscall_dispatch(
    cpu_context_t *context
);

#endif
