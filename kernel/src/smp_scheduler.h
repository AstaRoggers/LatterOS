#ifndef SMP_SCHEDULER_H
#define SMP_SCHEDULER_H

#include "cpu_context.h"

#include <stdbool.h>
#include <stdint.h>

#define SMP_SCHEDULER_IPI_VECTOR   0xF0
#define SMP_SCHEDULER_YIELD_VECTOR 0xF1
#define SMP_SCHEDULER_TIMER_VECTOR 0xF2

#define SMP_SCHEDULER_TIMER_HZ       250U
#define SMP_RUN_QUEUE_CAPACITY       16U
#define SMP_BENCHMARK_JOBS_PER_CPU   4U
#define SMP_KERNEL_THREAD_MAX_COUNT  64U
#define SMP_KERNEL_THREAD_STACK_SIZE 16384U

typedef uint64_t (*smp_job_function_t)(
    void *argument
);

typedef enum
{
    SMP_JOB_IDLE,
    SMP_JOB_RESERVED,
    SMP_JOB_QUEUED,
    SMP_JOB_RUNNING,
    SMP_JOB_COMPLETE
} smp_job_state_t;

void smp_scheduler_init(void);

void smp_scheduler_cpu_online(
    uint32_t cpu_index
);

void smp_scheduler_set_preemption(
    uint32_t cpu_index,
    bool enabled
);

__attribute__((noreturn))
void smp_scheduler_ap_loop(
    uint32_t cpu_index
);

void smp_scheduler_handle_ipi(void);

cpu_context_t *smp_scheduler_handle_yield(
    cpu_context_t *context
);

cpu_context_t *smp_scheduler_handle_timer(
    cpu_context_t *context
);

void smp_scheduler_yield(void);

__attribute__((noreturn))
void smp_scheduler_exit_current(
    uint64_t result
);

bool smp_scheduler_submit(
    uint32_t cpu_index,
    smp_job_function_t function,
    void *argument,
    uint64_t *job_id
);

bool smp_scheduler_submit_any(
    smp_job_function_t function,
    void *argument,
    uint32_t *cpu_index,
    uint64_t *job_id
);

uint32_t smp_scheduler_start_benchmark(
    uint64_t iterations
);

uint64_t smp_scheduler_completed_jobs(void);
uint32_t smp_scheduler_worker_count(void);
uint32_t smp_scheduler_queue_depth(
    uint32_t cpu_index
);

uint64_t smp_scheduler_current_thread_id(
    uint32_t cpu_index
);

bool smp_scheduler_preemption_enabled(
    uint32_t cpu_index
);

const char *smp_job_state_name(
    smp_job_state_t state
);

void smp_scheduler_print_status(void);

#endif
