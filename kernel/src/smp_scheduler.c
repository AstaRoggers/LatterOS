#include "smp_scheduler.h"

#include "cpu_local.h"
#include "gdt.h"
#include "kstdio.h"
#include "lapic.h"
#include "smp.h"
#include "spinlock.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DEFAULT_BENCHMARK_ITERATIONS 5000000ULL
#define MAX_BENCHMARK_ITERATIONS     250000000ULL
#define INITIAL_RFLAGS               0x202ULL
#define SMP_THREAD_NONE              UINT32_MAX

typedef enum
{
    SMP_THREAD_FREE,
    SMP_THREAD_READY,
    SMP_THREAD_RUNNING,
    SMP_THREAD_EXITING,
    SMP_THREAD_COMPLETE
} smp_thread_state_t;

typedef struct
{
    uint64_t id;
    uint32_t slot;
    uint32_t current_cpu;
    uint32_t last_cpu;

    smp_thread_state_t state;
    bool queued;

    smp_job_function_t function;
    void *argument;
    cpu_context_t *context;

    uint64_t result;
    uint64_t scheduler_ticks;
    uint64_t switches;
    uint64_t voluntary_yields;
    uint64_t preemptions;
    uint64_t retired_sequence;
    uint32_t retired_cpu;

    uint8_t stack[SMP_KERNEL_THREAD_STACK_SIZE]
        __attribute__((aligned(16)));
} smp_kernel_thread_t;

typedef struct
{
    uint32_t entries[SMP_RUN_QUEUE_CAPACITY];
    uint32_t head;
    uint32_t tail;
    uint32_t count;

    uint32_t current_thread;
    cpu_context_t *idle_context;

    bool online;
    bool preemption_enabled;

    uint64_t running_thread_id;
    uint64_t last_thread_id;
    uint64_t last_result;

    uint64_t submitted_jobs;
    uint64_t completed_jobs;
    uint64_t rejected_jobs;
    uint64_t stolen_in;
    uint64_t stolen_out;
    uint64_t wake_failures;
    uint64_t context_switches;
    uint64_t timer_preemptions;
    uint64_t voluntary_yields;
    uint64_t idle_returns;
    uint64_t schedule_sequence;
} smp_run_queue_t;

typedef struct
{
    uint64_t iterations;
    uint32_t requested_cpu;
    uint32_t sequence;
} benchmark_argument_t;

extern void smp_thread_bootstrap(void);

static spinlock_t scheduler_lock;
static smp_run_queue_t run_queues[SMP_MAX_CPUS];
static smp_kernel_thread_t threads[
    SMP_KERNEL_THREAD_MAX_COUNT
];

static benchmark_argument_t benchmark_arguments[
    SMP_MAX_CPUS
][SMP_BENCHMARK_JOBS_PER_CPU];

static uint64_t next_thread_id;
static uint64_t total_completed;
static uint64_t total_stolen;
static bool initialized;

static void clear_bytes(
    void *pointer,
    size_t count
)
{
    uint8_t *bytes =
        (uint8_t *)pointer;

    for (
        size_t index = 0;
        index < count;
        index++
    )
    {
        bytes[index] = 0;
    }
}

static void disable_interrupts(void)
{
    __asm__ volatile(
        "cli"
        :
        :
        : "memory"
    );
}

static __attribute__((noreturn)) void halt_forever(void)
{
    for (;;)
    {
        __asm__ volatile("cli; hlt");
    }
}

static cpu_context_t *create_thread_context(
    smp_kernel_thread_t *thread
)
{
    if (thread == NULL)
    {
        return NULL;
    }

    uint8_t *stack_top =
        thread->stack +
        SMP_KERNEL_THREAD_STACK_SIZE;

    cpu_user_context_t *frame =
        (cpu_user_context_t *)(
            stack_top -
            sizeof(cpu_user_context_t)
        );

    clear_bytes(
        frame,
        sizeof(cpu_user_context_t)
    );

    frame->base.r12 =
        (uint64_t)thread->function;

    frame->base.r13 =
        (uint64_t)thread->argument;

    frame->base.r14 =
        thread->id;

    frame->base.rip =
        (uint64_t)smp_thread_bootstrap;

    frame->base.cs =
        GDT_KERNEL_CODE_SELECTOR;

    frame->base.rflags =
        INITIAL_RFLAGS;

    frame->rsp =
        (uint64_t)stack_top;

    frame->ss =
        GDT_KERNEL_DATA_SELECTOR;

    return &frame->base;
}

static bool cpu_is_worker_locked(
    uint32_t cpu_index
)
{
    if (
        cpu_index == 0 ||
        cpu_index >= smp_cpu_count() ||
        cpu_index >= SMP_MAX_CPUS ||
        !run_queues[cpu_index].online
    )
    {
        return false;
    }

    const smp_cpu_t *cpu =
        smp_cpu(cpu_index);

    if (cpu == NULL)
    {
        return false;
    }

    return (
        (smp_cpu_state_t)__atomic_load_n(
            &cpu->state,
            __ATOMIC_ACQUIRE
        ) == SMP_CPU_WORKER
    );
}

static uint32_t queue_load_locked(
    uint32_t cpu_index
)
{
    if (cpu_index >= SMP_MAX_CPUS)
    {
        return UINT32_MAX;
    }

    smp_run_queue_t *queue =
        &run_queues[cpu_index];

    uint32_t load = queue->count;

    if (queue->current_thread != SMP_THREAD_NONE)
    {
        load++;
    }

    return load;
}

static bool queue_push_locked(
    uint32_t cpu_index,
    uint32_t thread_slot
)
{
    if (
        cpu_index >= SMP_MAX_CPUS ||
        thread_slot >= SMP_KERNEL_THREAD_MAX_COUNT
    )
    {
        return false;
    }

    smp_run_queue_t *queue =
        &run_queues[cpu_index];

    smp_kernel_thread_t *thread =
        &threads[thread_slot];

    if (
        !queue->online ||
        queue->count >= SMP_RUN_QUEUE_CAPACITY ||
        thread->queued
    )
    {
        return false;
    }

    queue->entries[queue->tail] =
        thread_slot;

    queue->tail =
        (queue->tail + 1U) %
        SMP_RUN_QUEUE_CAPACITY;

    queue->count++;
    thread->queued = true;
    thread->current_cpu = cpu_index;

    return true;
}

static bool queue_pop_locked(
    uint32_t cpu_index,
    uint32_t *thread_slot
)
{
    if (
        cpu_index >= SMP_MAX_CPUS ||
        thread_slot == NULL
    )
    {
        return false;
    }

    smp_run_queue_t *queue =
        &run_queues[cpu_index];

    while (queue->count > 0)
    {
        uint32_t slot =
            queue->entries[queue->head];

        queue->head =
            (queue->head + 1U) %
            SMP_RUN_QUEUE_CAPACITY;

        queue->count--;

        if (slot >= SMP_KERNEL_THREAD_MAX_COUNT)
        {
            continue;
        }

        smp_kernel_thread_t *thread =
            &threads[slot];

        thread->queued = false;

        if (thread->state != SMP_THREAD_READY)
        {
            continue;
        }

        *thread_slot = slot;
        return true;
    }

    return false;
}

static bool steal_thread_locked(
    uint32_t thief_cpu,
    uint32_t *thread_slot
)
{
    if (
        thief_cpu == 0 ||
        thief_cpu >= SMP_MAX_CPUS ||
        thread_slot == NULL
    )
    {
        return false;
    }

    uint32_t victim_cpu = 0;
    uint32_t victim_depth = 1;

    for (
        uint32_t cpu_index = 1;
        cpu_index < smp_cpu_count();
        cpu_index++
    )
    {
        if (
            cpu_index == thief_cpu ||
            !run_queues[cpu_index].online
        )
        {
            continue;
        }

        if (run_queues[cpu_index].count > victim_depth)
        {
            victim_cpu = cpu_index;
            victim_depth =
                run_queues[cpu_index].count;
        }
    }

    if (victim_cpu == 0)
    {
        return false;
    }

    if (!queue_pop_locked(
            victim_cpu,
            thread_slot
        ))
    {
        return false;
    }

    smp_kernel_thread_t *thread =
        &threads[*thread_slot];

    thread->last_cpu = victim_cpu;
    thread->current_cpu = thief_cpu;

    run_queues[victim_cpu].stolen_out++;
    run_queues[thief_cpu].stolen_in++;
    total_stolen++;

    return true;
}

static bool thread_slot_active_locked(
    uint32_t thread_slot
)
{
    if (thread_slot >= SMP_KERNEL_THREAD_MAX_COUNT)
    {
        return true;
    }

    if (threads[thread_slot].queued)
    {
        return true;
    }

    for (
        uint32_t cpu_index = 1;
        cpu_index < smp_cpu_count();
        cpu_index++
    )
    {
        if (
            run_queues[cpu_index].current_thread ==
            thread_slot
        )
        {
            return true;
        }
    }

    return false;
}

static bool thread_slot_reapable_locked(
    uint32_t thread_slot
)
{
    if (thread_slot >= SMP_KERNEL_THREAD_MAX_COUNT)
    {
        return false;
    }

    smp_kernel_thread_t *thread =
        &threads[thread_slot];

    if (thread->state == SMP_THREAD_FREE)
    {
        return true;
    }

    if (
        thread->state != SMP_THREAD_COMPLETE ||
        thread_slot_active_locked(thread_slot) ||
        thread->retired_cpu >= SMP_MAX_CPUS
    )
    {
        return false;
    }

    /*
     * A later scheduler entry on the retiring CPU proves that the AP
     * has fully left the old thread's interrupt frame and stack.
     */
    return (
        run_queues[thread->retired_cpu].schedule_sequence >
        thread->retired_sequence
    );
}

static int32_t allocate_thread_slot_locked(void)
{
    for (
        uint32_t slot = 0;
        slot < SMP_KERNEL_THREAD_MAX_COUNT;
        slot++
    )
    {
        if (thread_slot_reapable_locked(slot))
        {
            return (int32_t)slot;
        }
    }

    return -1;
}

static void wake_cpu_locked(
    uint32_t cpu_index
)
{
    const smp_cpu_t *cpu =
        smp_cpu(cpu_index);

    if (cpu == NULL)
    {
        return;
    }

    if (!lapic_send_ipi(
            cpu->apic_id,
            SMP_SCHEDULER_IPI_VECTOR
        ))
    {
        run_queues[cpu_index].wake_failures++;
    }
}

static cpu_context_t *select_next_locked(
    uint32_t cpu_index,
    cpu_context_t *fallback
)
{
    smp_run_queue_t *queue =
        &run_queues[cpu_index];

    uint32_t next_slot = SMP_THREAD_NONE;

    bool found = queue_pop_locked(
        cpu_index,
        &next_slot
    );

    if (!found)
    {
        found = steal_thread_locked(
            cpu_index,
            &next_slot
        );
    }

    if (
        found &&
        next_slot < SMP_KERNEL_THREAD_MAX_COUNT
    )
    {
        smp_kernel_thread_t *next =
            &threads[next_slot];

        next->state = SMP_THREAD_RUNNING;
        next->current_cpu = cpu_index;
        next->switches++;

        queue->current_thread = next_slot;
        queue->running_thread_id = next->id;
        queue->context_switches++;

        cpu_local_set_current_kernel_thread(
            cpu_index,
            next->id
        );

        cpu_local_note_context_switch(
            cpu_index
        );

        return next->context != NULL ?
            next->context :
            fallback;
    }

    queue->current_thread = SMP_THREAD_NONE;
    queue->running_thread_id = 0;
    queue->idle_returns++;

    cpu_local_set_current_kernel_thread(
        cpu_index,
        0
    );

    return queue->idle_context != NULL ?
        queue->idle_context :
        fallback;
}

static cpu_context_t *schedule_context(
    cpu_context_t *context,
    bool timer_tick
)
{
    if (
        !initialized ||
        context == NULL
    )
    {
        return context;
    }

    uint32_t cpu_index =
        smp_current_cpu_index();

    if (
        cpu_index == 0 ||
        cpu_index >= SMP_MAX_CPUS
    )
    {
        return context;
    }

    uint64_t flags =
        spinlock_lock_irqsave(
            &scheduler_lock
        );

    smp_run_queue_t *queue =
        &run_queues[cpu_index];

    if (!queue->online)
    {
        spinlock_unlock_irqrestore(
            &scheduler_lock,
            flags
        );

        return context;
    }

    queue->schedule_sequence++;

    if (queue->current_thread == SMP_THREAD_NONE)
    {
        queue->idle_context = context;
    }
    else
    {
        uint32_t current_slot =
            queue->current_thread;

        if (current_slot < SMP_KERNEL_THREAD_MAX_COUNT)
        {
            smp_kernel_thread_t *current =
                &threads[current_slot];

            current->context = context;
            current->last_cpu = cpu_index;

            if (
                current->state ==
                SMP_THREAD_RUNNING
            )
            {
                if (timer_tick)
                {
                    current->scheduler_ticks++;
                    current->preemptions++;
                    queue->timer_preemptions++;

                    cpu_local_charge_scheduler_tick(
                        cpu_index
                    );
                }
                else
                {
                    current->voluntary_yields++;
                    queue->voluntary_yields++;
                }

                current->state = SMP_THREAD_READY;

                if (!queue_push_locked(
                        cpu_index,
                        current_slot
                    ))
                {
                    current->state =
                        SMP_THREAD_RUNNING;

                    spinlock_unlock_irqrestore(
                        &scheduler_lock,
                        flags
                    );

                    return context;
                }
            }
            else if (
                current->state ==
                SMP_THREAD_EXITING
            )
            {
                current->state =
                    SMP_THREAD_COMPLETE;

                current->retired_cpu = cpu_index;
                current->retired_sequence =
                    queue->schedule_sequence;

                queue->last_thread_id =
                    current->id;

                queue->last_result =
                    current->result;

                queue->completed_jobs++;
                total_completed++;
            }
        }

        queue->current_thread = SMP_THREAD_NONE;
        queue->running_thread_id = 0;
    }

    cpu_context_t *next =
        select_next_locked(
            cpu_index,
            context
        );

    spinlock_unlock_irqrestore(
        &scheduler_lock,
        flags
    );

    return next;
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
        (uint64_t)benchmark->requested_cpu ^
        ((uint64_t)benchmark->sequence << 32);

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

        if ((index & 0x3FFFULL) == 0)
        {
            smp_scheduler_yield();
        }
    }

    return value;
}

void smp_scheduler_init(void)
{
    clear_bytes(
        run_queues,
        sizeof(run_queues)
    );

    clear_bytes(
        threads,
        sizeof(threads)
    );

    clear_bytes(
        benchmark_arguments,
        sizeof(benchmark_arguments)
    );

    spinlock_init(&scheduler_lock);

    for (
        uint32_t cpu_index = 0;
        cpu_index < SMP_MAX_CPUS;
        cpu_index++
    )
    {
        run_queues[cpu_index].current_thread =
            SMP_THREAD_NONE;
    }

    for (
        uint32_t slot = 0;
        slot < SMP_KERNEL_THREAD_MAX_COUNT;
        slot++
    )
    {
        threads[slot].slot = slot;
        threads[slot].state = SMP_THREAD_FREE;
    }

    next_thread_id = 1;
    total_completed = 0;
    total_stolen = 0;
    initialized = true;
}

void smp_scheduler_cpu_online(
    uint32_t cpu_index
)
{
    if (
        !initialized ||
        cpu_index == 0 ||
        cpu_index >= SMP_MAX_CPUS
    )
    {
        return;
    }

    uint64_t flags =
        spinlock_lock_irqsave(
            &scheduler_lock
        );

    smp_run_queue_t *queue =
        &run_queues[cpu_index];

    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    queue->current_thread = SMP_THREAD_NONE;
    queue->idle_context = NULL;
    queue->running_thread_id = 0;
    queue->online = true;

    cpu_local_set_current_kernel_thread(
        cpu_index,
        0
    );

    spinlock_unlock_irqrestore(
        &scheduler_lock,
        flags
    );
}

void smp_scheduler_set_preemption(
    uint32_t cpu_index,
    bool enabled
)
{
    if (
        !initialized ||
        cpu_index >= SMP_MAX_CPUS
    )
    {
        return;
    }

    uint64_t flags =
        spinlock_lock_irqsave(
            &scheduler_lock
        );

    run_queues[cpu_index].preemption_enabled =
        enabled;

    spinlock_unlock_irqrestore(
        &scheduler_lock,
        flags
    );
}

void smp_scheduler_handle_ipi(void)
{
    /*
     * The IPI wakes an AP that is halted in its per-CPU idle path.
     * The AP either schedules directly from its local timer interrupt
     * or reaches the explicit yield trap immediately after HLT.
     */
}

cpu_context_t *smp_scheduler_handle_yield(
    cpu_context_t *context
)
{
    return schedule_context(
        context,
        false
    );
}

cpu_context_t *smp_scheduler_handle_timer(
    cpu_context_t *context
)
{
    return schedule_context(
        context,
        true
    );
}

void smp_scheduler_yield(void)
{
    __asm__ volatile(
        "int $0xF1"
        :
        :
        : "memory"
    );
}

__attribute__((noreturn))
void smp_scheduler_exit_current(
    uint64_t result
)
{
    disable_interrupts();
    spinlock_lock(&scheduler_lock);

    uint32_t cpu_index =
        smp_current_cpu_index();

    if (
        cpu_index > 0 &&
        cpu_index < SMP_MAX_CPUS
    )
    {
        smp_run_queue_t *queue =
            &run_queues[cpu_index];

        uint32_t current_slot =
            queue->current_thread;

        if (current_slot < SMP_KERNEL_THREAD_MAX_COUNT)
        {
            smp_kernel_thread_t *thread =
                &threads[current_slot];

            thread->result = result;
            thread->state = SMP_THREAD_EXITING;
            thread->queued = false;
        }
    }

    spinlock_unlock(&scheduler_lock);

    __asm__ volatile(
        "int $0xF1"
        :
        :
        : "memory"
    );

    halt_forever();
}

__attribute__((noreturn))
void smp_scheduler_ap_loop(
    uint32_t cpu_index
)
{
    if (
        cpu_index == 0 ||
        cpu_index >= SMP_MAX_CPUS
    )
    {
        halt_forever();
    }

    for (;;)
    {
        /*
         * This trap captures the AP idle context on first entry and
         * subsequently acts as the cooperative reschedule point.
         */
        __asm__ volatile(
            "int $0xF1"
            :
            :
            : "memory"
        );

        /*
         * The idle path is a real per-CPU scheduler context. An IPI or
         * local APIC timer interrupt resumes it when work is available.
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
        function == NULL
    )
    {
        return false;
    }

    uint64_t flags =
        spinlock_lock_irqsave(
            &scheduler_lock
        );

    if (!cpu_is_worker_locked(cpu_index))
    {
        spinlock_unlock_irqrestore(
            &scheduler_lock,
            flags
        );

        return false;
    }

    int32_t free_slot =
        allocate_thread_slot_locked();

    if (free_slot < 0)
    {
        run_queues[cpu_index].rejected_jobs++;

        spinlock_unlock_irqrestore(
            &scheduler_lock,
            flags
        );

        return false;
    }

    smp_kernel_thread_t *thread =
        &threads[free_slot];

    uint32_t preserved_slot =
        thread->slot;

    clear_bytes(
        thread,
        offsetof(
            smp_kernel_thread_t,
            stack
        )
    );

    thread->slot = preserved_slot;
    thread->id = next_thread_id++;
    thread->current_cpu = cpu_index;
    thread->last_cpu = cpu_index;
    thread->state = SMP_THREAD_READY;
    thread->function = function;
    thread->argument = argument;
    thread->context =
        create_thread_context(thread);

    if (
        thread->context == NULL ||
        !queue_push_locked(
            cpu_index,
            (uint32_t)free_slot
        )
    )
    {
        thread->state = SMP_THREAD_FREE;
        run_queues[cpu_index].rejected_jobs++;

        spinlock_unlock_irqrestore(
            &scheduler_lock,
            flags
        );

        return false;
    }

    run_queues[cpu_index].submitted_jobs++;

    if (job_id != NULL)
    {
        *job_id = thread->id;
    }

    wake_cpu_locked(cpu_index);

    spinlock_unlock_irqrestore(
        &scheduler_lock,
        flags
    );

    return true;
}

bool smp_scheduler_submit_any(
    smp_job_function_t function,
    void *argument,
    uint32_t *cpu_index,
    uint64_t *job_id
)
{
    if (
        !initialized ||
        function == NULL
    )
    {
        return false;
    }

    uint32_t selected = 0;

    uint64_t flags =
        spinlock_lock_irqsave(
            &scheduler_lock
        );

    uint32_t selected_load = UINT32_MAX;

    for (
        uint32_t index = 1;
        index < smp_cpu_count();
        index++
    )
    {
        if (!cpu_is_worker_locked(index))
        {
            continue;
        }

        uint32_t load =
            queue_load_locked(index);

        if (load < selected_load)
        {
            selected = index;
            selected_load = load;
        }
    }

    spinlock_unlock_irqrestore(
        &scheduler_lock,
        flags
    );

    if (selected == 0)
    {
        return false;
    }

    if (!smp_scheduler_submit(
            selected,
            function,
            argument,
            job_id
        ))
    {
        return false;
    }

    if (cpu_index != NULL)
    {
        *cpu_index = selected;
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

    if (iterations > MAX_BENCHMARK_ITERATIONS)
    {
        iterations =
            MAX_BENCHMARK_ITERATIONS;
    }

    uint32_t active_cpus = 0;

    for (
        uint32_t cpu_index = 1;
        cpu_index < smp_cpu_count();
        cpu_index++
    )
    {
        bool idle = false;

        uint64_t flags =
            spinlock_lock_irqsave(
                &scheduler_lock
            );

        if (cpu_is_worker_locked(cpu_index))
        {
            idle = (
                run_queues[cpu_index].count == 0 &&
                run_queues[cpu_index].current_thread ==
                    SMP_THREAD_NONE
            );
        }

        spinlock_unlock_irqrestore(
            &scheduler_lock,
            flags
        );

        if (!idle)
        {
            continue;
        }

        uint32_t submitted_on_cpu = 0;

        for (
            uint32_t sequence = 0;
            sequence < SMP_BENCHMARK_JOBS_PER_CPU;
            sequence++
        )
        {
            benchmark_argument_t *argument =
                &benchmark_arguments[
                    cpu_index
                ][sequence];

            argument->iterations = iterations;
            argument->requested_cpu = cpu_index;
            argument->sequence = sequence;

            if (smp_scheduler_submit(
                    cpu_index,
                    benchmark_job,
                    argument,
                    NULL
                ))
            {
                submitted_on_cpu++;
            }
        }

        if (submitted_on_cpu > 0)
        {
            active_cpus++;
        }
    }

    return active_cpus;
}

uint64_t smp_scheduler_completed_jobs(void)
{
    uint64_t flags =
        spinlock_lock_irqsave(
            &scheduler_lock
        );

    uint64_t completed = total_completed;

    spinlock_unlock_irqrestore(
        &scheduler_lock,
        flags
    );

    return completed;
}

uint32_t smp_scheduler_worker_count(void)
{
    if (!initialized)
    {
        return 0;
    }

    uint64_t flags =
        spinlock_lock_irqsave(
            &scheduler_lock
        );

    uint32_t count = 0;

    for (
        uint32_t index = 1;
        index < smp_cpu_count();
        index++
    )
    {
        if (cpu_is_worker_locked(index))
        {
            count++;
        }
    }

    spinlock_unlock_irqrestore(
        &scheduler_lock,
        flags
    );

    return count;
}

uint32_t smp_scheduler_queue_depth(
    uint32_t cpu_index
)
{
    if (
        !initialized ||
        cpu_index >= SMP_MAX_CPUS
    )
    {
        return 0;
    }

    uint64_t flags =
        spinlock_lock_irqsave(
            &scheduler_lock
        );

    uint32_t depth =
        run_queues[cpu_index].count;

    spinlock_unlock_irqrestore(
        &scheduler_lock,
        flags
    );

    return depth;
}

uint64_t smp_scheduler_current_thread_id(
    uint32_t cpu_index
)
{
    if (
        !initialized ||
        cpu_index >= SMP_MAX_CPUS
    )
    {
        return 0;
    }

    uint64_t flags =
        spinlock_lock_irqsave(
            &scheduler_lock
        );

    uint64_t id =
        run_queues[cpu_index].running_thread_id;

    spinlock_unlock_irqrestore(
        &scheduler_lock,
        flags
    );

    return id;
}

bool smp_scheduler_preemption_enabled(
    uint32_t cpu_index
)
{
    if (
        !initialized ||
        cpu_index >= SMP_MAX_CPUS
    )
    {
        return false;
    }

    uint64_t flags =
        spinlock_lock_irqsave(
            &scheduler_lock
        );

    bool enabled =
        run_queues[cpu_index].preemption_enabled;

    spinlock_unlock_irqrestore(
        &scheduler_lock,
        flags
    );

    return enabled;
}

const char *smp_job_state_name(
    smp_job_state_t state
)
{
    switch (state)
    {
        case SMP_JOB_RESERVED:
            return "reserved";

        case SMP_JOB_QUEUED:
            return "queued";

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
    if (!initialized)
    {
        kprintf(
            "SMP kernel-thread scheduler is not initialized\n"
        );
        return;
    }

    uint64_t flags =
        spinlock_lock_irqsave(
            &scheduler_lock
        );

    uint32_t workers = 0;

    for (
        uint32_t cpu_index = 1;
        cpu_index < smp_cpu_count();
        cpu_index++
    )
    {
        if (cpu_is_worker_locked(cpu_index))
        {
            workers++;
        }
    }

    kprintf(
        "SMP kernel threads: workers=%u queue=%u threads=%u stack=%u KiB timer=%u Hz completed=%llu stolen=%llu\n",
        (unsigned int)workers,
        (unsigned int)SMP_RUN_QUEUE_CAPACITY,
        (unsigned int)SMP_KERNEL_THREAD_MAX_COUNT,
        (unsigned int)(
            SMP_KERNEL_THREAD_STACK_SIZE / 1024U
        ),
        (unsigned int)SMP_SCHEDULER_TIMER_HZ,
        (unsigned long long)total_completed,
        (unsigned long long)total_stolen
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

        smp_run_queue_t *queue =
            &run_queues[cpu_index];

        kprintf(
            "CPU %u APIC=%u ready=%u current=%llu preempt=%s submitted=%llu completed=%llu switches=%llu timer=%llu yields=%llu steal-in=%llu steal-out=%llu rejected=%llu wake-fail=%llu last=%llu result=0x%llX\n",
            (unsigned int)cpu_index,
            (unsigned int)cpu->apic_id,
            (unsigned int)queue->count,
            (unsigned long long)
                queue->running_thread_id,
            queue->preemption_enabled ?
                "yes" : "no",
            (unsigned long long)
                queue->submitted_jobs,
            (unsigned long long)
                queue->completed_jobs,
            (unsigned long long)
                queue->context_switches,
            (unsigned long long)
                queue->timer_preemptions,
            (unsigned long long)
                queue->voluntary_yields,
            (unsigned long long)
                queue->stolen_in,
            (unsigned long long)
                queue->stolen_out,
            (unsigned long long)
                queue->rejected_jobs,
            (unsigned long long)
                queue->wake_failures,
            (unsigned long long)
                queue->last_thread_id,
            (unsigned long long)
                queue->last_result
        );
    }

    spinlock_unlock_irqrestore(
        &scheduler_lock,
        flags
    );
}
