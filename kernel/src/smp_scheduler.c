#include "smp_scheduler.h"

#include "kstdio.h"
#include "lapic.h"
#include "smp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DEFAULT_BENCHMARK_ITERATIONS 5000000ULL
#define MAX_BENCHMARK_ITERATIONS     250000000ULL

typedef struct
{
    volatile uint32_t state;
    smp_job_function_t function;
    void *argument;
    uint64_t job_id;
    uint64_t last_result;
    uint64_t completed_jobs;
    uint64_t submitted_jobs;
} smp_mailbox_t;

typedef struct
{
    uint64_t iterations;
    uint32_t cpu_index;
} benchmark_argument_t;

static smp_mailbox_t mailboxes[SMP_MAX_CPUS];
static benchmark_argument_t benchmark_arguments[
    SMP_MAX_CPUS
];

static volatile uint64_t next_job_id;
static volatile uint64_t total_completed;
static bool initialized;

static void clear_bytes(
    void *pointer,
    size_t count
)
{
    uint8_t *bytes = pointer;

    for (
        size_t index = 0;
        index < count;
        index++
    )
    {
        bytes[index] = 0;
    }
}

static uint32_t state_load(
    const volatile uint32_t *state
)
{
    return __atomic_load_n(
        state,
        __ATOMIC_ACQUIRE
    );
}

static void state_store(
    volatile uint32_t *state,
    smp_job_state_t value
)
{
    __atomic_store_n(
        state,
        (uint32_t)value,
        __ATOMIC_RELEASE
    );
}

static bool reserve_mailbox(
    smp_mailbox_t *mailbox
)
{
    uint32_t expected = SMP_JOB_IDLE;

    if (
        __atomic_compare_exchange_n(
            &mailbox->state,
            &expected,
            SMP_JOB_RESERVED,
            false,
            __ATOMIC_ACQ_REL,
            __ATOMIC_ACQUIRE
        )
    )
    {
        return true;
    }

    expected = SMP_JOB_COMPLETE;

    return __atomic_compare_exchange_n(
        &mailbox->state,
        &expected,
        SMP_JOB_RESERVED,
        false,
        __ATOMIC_ACQ_REL,
        __ATOMIC_ACQUIRE
    );
}

static uint64_t benchmark_job(
    void *argument
)
{
    benchmark_argument_t *benchmark =
        (benchmark_argument_t *)argument;

    uint64_t iterations =
        benchmark->iterations;

    uint64_t value =
        0x9E3779B97F4A7C15ULL ^
        (uint64_t)benchmark->cpu_index;

    for (
        uint64_t index = 0;
        index < iterations;
        index++
    )
    {
        value ^= value >> 12;
        value ^= value << 25;
        value ^= value >> 27;
        value *= 0x2545F4914F6CDD1DULL;
        value += index + 1;

        if ((index & 0xFFFFULL) == 0)
        {
            __asm__ volatile("pause");
        }
    }

    return value;
}

void smp_scheduler_init(void)
{
    clear_bytes(
        mailboxes,
        sizeof(mailboxes)
    );

    clear_bytes(
        benchmark_arguments,
        sizeof(benchmark_arguments)
    );

    next_job_id = 1;
    total_completed = 0;
    initialized = true;
}

void smp_scheduler_cpu_online(
    uint32_t cpu_index
)
{
    if (
        !initialized ||
        cpu_index >= SMP_MAX_CPUS
    )
    {
        return;
    }

    state_store(
        &mailboxes[cpu_index].state,
        SMP_JOB_IDLE
    );
}

void smp_scheduler_handle_ipi(void)
{
    /*
     * The IPI exists only to wake a halted application processor.
     * The AP loop consumes the mailbox after the interrupt returns.
     */
}

__attribute__((noreturn))
void smp_scheduler_ap_loop(
    uint32_t cpu_index
)
{
    if (cpu_index >= SMP_MAX_CPUS)
    {
        for (;;)
        {
            __asm__ volatile("cli; hlt");
        }
    }

    smp_mailbox_t *mailbox =
        &mailboxes[cpu_index];

    for (;;)
    {
        __asm__ volatile(
            "cli"
            :
            :
            : "memory"
        );

        uint32_t state =
            state_load(&mailbox->state);

        if (state == SMP_JOB_READY)
        {
            uint32_t expected =
                SMP_JOB_READY;

            if (
                __atomic_compare_exchange_n(
                    &mailbox->state,
                    &expected,
                    SMP_JOB_RUNNING,
                    false,
                    __ATOMIC_ACQ_REL,
                    __ATOMIC_ACQUIRE
                )
            )
            {
                smp_job_function_t function =
                    mailbox->function;

                void *argument =
                    mailbox->argument;

                __asm__ volatile(
                    "sti"
                    :
                    :
                    : "memory"
                );

                uint64_t result = 0;

                if (function != NULL)
                {
                    result = function(argument);
                }

                mailbox->last_result = result;

                __atomic_add_fetch(
                    &mailbox->completed_jobs,
                    1,
                    __ATOMIC_RELAXED
                );

                __atomic_add_fetch(
                    &total_completed,
                    1,
                    __ATOMIC_RELAXED
                );

                state_store(
                    &mailbox->state,
                    SMP_JOB_COMPLETE
                );

                continue;
            }
        }

        /*
         * STI; HLT is used as one atomic wait sequence. If a wake IPI
         * is already pending, it becomes visible immediately after HLT
         * and the CPU resumes at the next instruction.
         */
        __asm__ volatile(
            "sti\n"
            "hlt"
            :
            :
            : "memory"
        );
    }
}

bool smp_scheduler_submit(
    uint32_t cpu_index,
    smp_job_function_t function,
    void *argument,
    uint64_t *job_id
)
{
    if (
        !initialized ||
        function == NULL ||
        cpu_index == 0 ||
        cpu_index >= smp_cpu_count() ||
        cpu_index >= SMP_MAX_CPUS
    )
    {
        return false;
    }

    const smp_cpu_t *cpu =
        smp_cpu(cpu_index);

    if (
        cpu == NULL ||
        (smp_cpu_state_t)__atomic_load_n(
            &cpu->state,
            __ATOMIC_ACQUIRE
        ) != SMP_CPU_WORKER
    )
    {
        return false;
    }

    smp_mailbox_t *mailbox =
        &mailboxes[cpu_index];

    if (!reserve_mailbox(mailbox))
    {
        return false;
    }

    uint64_t identifier =
        __atomic_fetch_add(
            &next_job_id,
            1,
            __ATOMIC_RELAXED
        );

    mailbox->function = function;
    mailbox->argument = argument;
    mailbox->job_id = identifier;
    mailbox->submitted_jobs++;

    state_store(
        &mailbox->state,
        SMP_JOB_READY
    );

    if (
        !lapic_send_ipi(
            cpu->apic_id,
            SMP_SCHEDULER_IPI_VECTOR
        )
    )
    {
        state_store(
            &mailbox->state,
            SMP_JOB_IDLE
        );

        return false;
    }

    if (job_id != NULL)
    {
        *job_id = identifier;
    }

    return true;
}

uint32_t smp_scheduler_start_benchmark(
    uint64_t iterations
)
{
    if (iterations == 0)
    {
        iterations =
            DEFAULT_BENCHMARK_ITERATIONS;
    }

    if (
        iterations >
        MAX_BENCHMARK_ITERATIONS
    )
    {
        iterations =
            MAX_BENCHMARK_ITERATIONS;
    }

    uint32_t submitted = 0;

    for (
        uint32_t cpu_index = 1;
        cpu_index < smp_cpu_count();
        cpu_index++
    )
    {
        benchmark_arguments[cpu_index]
            .iterations = iterations;

        benchmark_arguments[cpu_index]
            .cpu_index = cpu_index;

        if (
            smp_scheduler_submit(
                cpu_index,
                benchmark_job,
                &benchmark_arguments[cpu_index],
                NULL
            )
        )
        {
            submitted++;
        }
    }

    return submitted;
}

uint64_t smp_scheduler_completed_jobs(void)
{
    return __atomic_load_n(
        &total_completed,
        __ATOMIC_RELAXED
    );
}

uint32_t smp_scheduler_worker_count(void)
{
    uint32_t count = 0;

    for (
        uint32_t index = 1;
        index < smp_cpu_count();
        index++
    )
    {
        const smp_cpu_t *cpu =
            smp_cpu(index);

        if (
            cpu != NULL &&
            (smp_cpu_state_t)__atomic_load_n(
                &cpu->state,
                __ATOMIC_ACQUIRE
            ) == SMP_CPU_WORKER
        )
        {
            count++;
        }
    }

    return count;
}

const char *smp_job_state_name(
    smp_job_state_t state
)
{
    switch (state)
    {
        case SMP_JOB_RESERVED:
            return "reserved";

        case SMP_JOB_READY:
            return "ready";

        case SMP_JOB_RUNNING:
            return "running";

        case SMP_JOB_COMPLETE:
            return "complete";

        default:
            return "idle";
    }
}

void smp_scheduler_print_status(void)
{
    kprintf(
        "SMP work scheduler: workers=%u completed=%llu\n",
        (unsigned int)smp_scheduler_worker_count(),
        (unsigned long long)
            smp_scheduler_completed_jobs()
    );

    for (
        uint32_t cpu_index = 1;
        cpu_index < smp_cpu_count();
        cpu_index++
    )
    {
        const smp_cpu_t *cpu =
            smp_cpu(cpu_index);

        if (cpu == NULL)
        {
            continue;
        }

        const smp_mailbox_t *mailbox =
            &mailboxes[cpu_index];

        smp_job_state_t state =
            (smp_job_state_t)state_load(
                &mailbox->state
            );

        kprintf(
            "CPU %u APIC=%u job=%llu state=%s submitted=%llu completed=%llu result=0x%llX\n",
            (unsigned int)cpu_index,
            (unsigned int)cpu->apic_id,
            (unsigned long long)mailbox->job_id,
            smp_job_state_name(state),
            (unsigned long long)
                mailbox->submitted_jobs,
            (unsigned long long)
                mailbox->completed_jobs,
            (unsigned long long)
                mailbox->last_result
        );
    }
}
