#ifndef SMP_USER_H
#define SMP_USER_H

#include "cpu_context.h"

#include <stdbool.h>
#include <stdint.h>

#define SMP_USER_MAX_PROCESSES 16U

void smp_user_init(void);

uint32_t smp_user_start_benchmark(
    uint64_t iterations
);

bool smp_user_is_current(void);

cpu_context_t *smp_user_syscall_dispatch(
    cpu_context_t *context
);

cpu_context_t *smp_user_fault_from_exception(
    cpu_context_t *context,
    uint64_t vector,
    uint64_t error_code,
    uint64_t fault_address
);

void smp_user_print_status(void);

#endif
