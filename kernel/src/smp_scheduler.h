#ifndef SMP_SCHEDULER_H
#define SMP_SCHEDULER_H

#include <stdbool.h>
#include <stdint.h>

#define SMP_SCHEDULER_IPI_VECTOR 0xF0

typedef uint64_t (*smp_job_function_t)(
    void *argument
);

typedef enum
{
    SMP_JOB_IDLE,
    SMP_JOB_RESERVED,
    SMP_JOB_READY,
    SMP_JOB_RUNNING,
    SMP_JOB_COMPLETE
} smp_job_state_t;

void smp_scheduler_init(void);

void smp_scheduler_cpu_online(
    uint32_t cpu_index
);

__attribute__((noreturn))
void smp_scheduler_ap_loop(
    uint32_t cpu_index
);

void smp_scheduler_handle_ipi(void);

bool smp_scheduler_submit(
    uint32_t cpu_index,
    smp_job_function_t function,
    void *argument,
    uint64_t *job_id
);

uint32_t smp_scheduler_start_benchmark(
    uint64_t iterations
);

uint64_t smp_scheduler_completed_jobs(void);
uint32_t smp_scheduler_worker_count(void);

const char *smp_job_state_name(
    smp_job_state_t state
);

void smp_scheduler_print_status(void);

#endif
