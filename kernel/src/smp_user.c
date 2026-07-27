#include "smp_user.h"

#include "cpu_local.h"
#include "hhdm.h"
#include "kstdio.h"
#include "page_allocator.h"
#include "paging.h"
#include "smp.h"
#include "smp_scheduler.h"
#include "spinlock.h"
#include "syscall.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SMP_USER_CODE_ADDRESS  0x0000005000000000ULL
#define SMP_USER_STACK_ADDRESS 0x0000005000200000ULL
#define SMP_USER_PAGE_SIZE     4096ULL
#define SMP_USER_DEFAULT_WORK  5000000ULL
#define SMP_USER_MAX_WORK      250000000ULL
#define SMP_USER_WRITE_LIMIT   1024ULL
#define SMP_USER_WRITE_CHUNK   120ULL
#define SMP_USER_SYSCALL_ERROR UINT64_MAX
#define SMP_USER_PID_BASE      0x100000ULL

typedef enum
{
    SMP_USER_FREE,
    SMP_USER_STARTING,
    SMP_USER_RUNNING,
    SMP_USER_COMPLETE
} smp_user_state_t;

typedef struct
{
    uint32_t slot;
    volatile uint32_t state;

    uint64_t pid;
    uint64_t thread_id;
    uint32_t requested_cpu;
    uint32_t last_cpu;

    uint64_t page_table_root;
    void *code_page;
    void *stack_page;

    uint64_t iterations;
    int64_t exit_status;

    uint64_t fault_vector;
    uint64_t fault_error;
    uint64_t fault_address;
} smp_user_process_t;

extern const uint8_t smp_user_payload_start[];
extern const uint8_t smp_user_payload_end[];

static spinlock_t user_lock;
static smp_user_process_t processes[
    SMP_USER_MAX_PROCESSES
];

static uint64_t next_pid;
static uint64_t total_started;
static uint64_t total_completed;
static uint64_t total_faulted;
static uint64_t total_syscalls;
static uint64_t total_yields;
static uint64_t total_migrations;
static uint64_t total_console_bytes;
static uint64_t total_console_checksum;
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
    smp_user_state_t value
)
{
    __atomic_store_n(
        state,
        (uint32_t)value,
        __ATOMIC_RELEASE
    );
}

static void release_resources_locked(
    smp_user_process_t *process
)
{
    if (process == NULL)
    {
        return;
    }

    if (
        process->page_table_root != 0 &&
        process->page_table_root !=
            paging_kernel_root()
    )
    {
        paging_destroy_user_space(
            process->page_table_root
        );
    }

    if (process->code_page != NULL)
    {
        free_page(process->code_page);
    }

    if (process->stack_page != NULL)
    {
        free_page(process->stack_page);
    }

    uint32_t slot = process->slot;

    clear_bytes(
        process,
        sizeof(*process)
    );

    process->slot = slot;
    state_store(
        &process->state,
        SMP_USER_FREE
    );
}

static void reap_completed(void)
{
    if (!initialized)
    {
        return;
    }

    uint64_t flags =
        spinlock_lock_irqsave(
            &user_lock
        );

    for (
        uint32_t slot = 0;
        slot < SMP_USER_MAX_PROCESSES;
        slot++
    )
    {
        smp_user_process_t *process =
            &processes[slot];

        if (
            state_load(&process->state) ==
            SMP_USER_COMPLETE
        )
        {
            release_resources_locked(
                process
            );
        }
    }

    spinlock_unlock_irqrestore(
        &user_lock,
        flags
    );
}

static int32_t reserve_process_slot(
    uint32_t cpu_index,
    uint64_t iterations
)
{
    uint64_t flags =
        spinlock_lock_irqsave(
            &user_lock
        );

    int32_t result = -1;

    for (
        uint32_t slot = 0;
        slot < SMP_USER_MAX_PROCESSES;
        slot++
    )
    {
        smp_user_process_t *process =
            &processes[slot];

        if (
            state_load(&process->state) !=
            SMP_USER_FREE
        )
        {
            continue;
        }

        clear_bytes(
            process,
            sizeof(*process)
        );

        process->slot = slot;
        process->pid = next_pid++;
        process->requested_cpu = cpu_index;
        process->last_cpu = cpu_index;
        process->iterations = iterations;

        state_store(
            &process->state,
            SMP_USER_STARTING
        );

        result = (int32_t)slot;
        break;
    }

    spinlock_unlock_irqrestore(
        &user_lock,
        flags
    );

    return result;
}

static smp_user_process_t *current_process(void)
{
    cpu_local_t *local =
        cpu_local_current();

    if (
        local == NULL ||
        local->current_process_slot >=
            SMP_USER_MAX_PROCESSES ||
        local->current_pid < SMP_USER_PID_BASE
    )
    {
        return NULL;
    }

    smp_user_process_t *process =
        &processes[
            local->current_process_slot
        ];

    if (
        state_load(&process->state) ==
            SMP_USER_FREE ||
        process->pid != local->current_pid
    )
    {
        return NULL;
    }

    return process;
}

static bool user_range_valid(
    const smp_user_process_t *process,
    uint64_t address,
    size_t size,
    bool writable
)
{
    if (
        process == NULL ||
        process->page_table_root == 0
    )
    {
        return false;
    }

    return paging_user_range_valid(
        process->page_table_root,
        address,
        size,
        writable
    );
}

static void completion_callback(
    void *owner,
    int64_t status,
    uint32_t cpu_index
)
{
    smp_user_process_t *process =
        (smp_user_process_t *)owner;

    if (process == NULL)
    {
        return;
    }

    process->exit_status = status;
    process->last_cpu = cpu_index;

    if (cpu_index != process->requested_cpu)
    {
        __atomic_add_fetch(
            &total_migrations,
            1,
            __ATOMIC_RELAXED
        );
    }

    if (process->fault_vector != 0)
    {
        __atomic_add_fetch(
            &total_faulted,
            1,
            __ATOMIC_RELAXED
        );
    }
    else
    {
        __atomic_add_fetch(
            &total_completed,
            1,
            __ATOMIC_RELAXED
        );
    }

    state_store(
        &process->state,
        SMP_USER_COMPLETE
    );
}

static bool spawn_on_cpu(
    uint32_t cpu_index,
    uint64_t iterations
)
{
    int32_t slot = reserve_process_slot(
        cpu_index,
        iterations
    );

    if (slot < 0)
    {
        return false;
    }

    smp_user_process_t *process =
        &processes[slot];

    size_t payload_size =
        (size_t)(
            smp_user_payload_end -
            smp_user_payload_start
        );

    if (
        payload_size == 0 ||
        payload_size > SMP_USER_PAGE_SIZE
    )
    {
        uint64_t flags =
            spinlock_lock_irqsave(
                &user_lock
            );
        release_resources_locked(process);
        spinlock_unlock_irqrestore(
            &user_lock,
            flags
        );
        return false;
    }

    process->code_page = alloc_page();
    process->stack_page = alloc_page();

    if (
        process->code_page == NULL ||
        process->stack_page == NULL
    )
    {
        uint64_t flags =
            spinlock_lock_irqsave(
                &user_lock
            );
        release_resources_locked(process);
        spinlock_unlock_irqrestore(
            &user_lock,
            flags
        );
        return false;
    }

    uint8_t *code =
        (uint8_t *)physical_to_virtual(
            (uint64_t)process->code_page
        );

    uint8_t *stack =
        (uint8_t *)physical_to_virtual(
            (uint64_t)process->stack_page
        );

    clear_bytes(
        code,
        SMP_USER_PAGE_SIZE
    );

    clear_bytes(
        stack,
        SMP_USER_PAGE_SIZE
    );

    for (
        size_t index = 0;
        index < payload_size;
        index++
    )
    {
        code[index] =
            smp_user_payload_start[index];
    }

    process->page_table_root =
        paging_create_user_space();

    if (
        process->page_table_root == 0 ||
        !paging_map_user_page_in(
            process->page_table_root,
            SMP_USER_CODE_ADDRESS,
            (uint64_t)process->code_page,
            false,
            true
        ) ||
        !paging_map_user_page_in(
            process->page_table_root,
            SMP_USER_STACK_ADDRESS,
            (uint64_t)process->stack_page,
            true,
            false
        )
    )
    {
        uint64_t flags =
            spinlock_lock_irqsave(
                &user_lock
            );
        release_resources_locked(process);
        spinlock_unlock_irqrestore(
            &user_lock,
            flags
        );
        return false;
    }

    state_store(
        &process->state,
        SMP_USER_RUNNING
    );

    uint64_t thread_id = 0;

    if (!smp_scheduler_submit_user(
            cpu_index,
            SMP_USER_CODE_ADDRESS,
            SMP_USER_STACK_ADDRESS +
                SMP_USER_PAGE_SIZE - 16ULL,
            iterations,
            process->page_table_root,
            process->slot,
            process->pid,
            process,
            completion_callback,
            &thread_id
        ))
    {
        uint64_t flags =
            spinlock_lock_irqsave(
                &user_lock
            );
        release_resources_locked(process);
        spinlock_unlock_irqrestore(
            &user_lock,
            flags
        );
        return false;
    }

    process->thread_id = thread_id;

    __atomic_add_fetch(
        &total_started,
        1,
        __ATOMIC_RELAXED
    );

    return true;
}

static uint64_t console_write(
    smp_user_process_t *process,
    uint64_t address,
    uint64_t length
)
{
    if (length == 0)
    {
        return 0;
    }

    if (
        length > SMP_USER_WRITE_LIMIT ||
        !user_range_valid(
            process,
            address,
            (size_t)length,
            false
        )
    )
    {
        return SMP_USER_SYSCALL_ERROR;
    }

    const uint8_t *source =
        (const uint8_t *)address;

    uint64_t checksum =
        0xCBF29CE484222325ULL;

    for (
        uint64_t index = 0;
        index < length;
        index++
    )
    {
        checksum ^= source[index];
        checksum *= 0x100000001B3ULL;
    }

    __atomic_add_fetch(
        &total_console_bytes,
        length,
        __ATOMIC_RELAXED
    );

    __atomic_xor_fetch(
        &total_console_checksum,
        checksum,
        __ATOMIC_RELAXED
    );

    return length;
}

void smp_user_init(void)
{
    clear_bytes(
        processes,
        sizeof(processes)
    );

    spinlock_init(&user_lock);

    for (
        uint32_t slot = 0;
        slot < SMP_USER_MAX_PROCESSES;
        slot++
    )
    {
        processes[slot].slot = slot;
        state_store(
            &processes[slot].state,
            SMP_USER_FREE
        );
    }

    next_pid = SMP_USER_PID_BASE;
    total_started = 0;
    total_completed = 0;
    total_faulted = 0;
    total_syscalls = 0;
    total_yields = 0;
    total_migrations = 0;
    total_console_bytes = 0;
    total_console_checksum = 0;
    initialized = true;
}

uint32_t smp_user_start_benchmark(
    uint64_t iterations
)
{
    if (!initialized)
    {
        return 0;
    }

    if (iterations == 0)
    {
        iterations = SMP_USER_DEFAULT_WORK;
    }

    if (iterations > SMP_USER_MAX_WORK)
    {
        iterations = SMP_USER_MAX_WORK;
    }

    reap_completed();

    uint32_t started = 0;

    for (
        uint32_t cpu_index = 1;
        cpu_index < smp_cpu_count();
        cpu_index++
    )
    {
        if (spawn_on_cpu(
                cpu_index,
                iterations
            ))
        {
            started++;
        }
    }

    return started;
}

bool smp_user_is_current(void)
{
    return current_process() != NULL;
}

cpu_context_t *smp_user_syscall_dispatch(
    cpu_context_t *context
)
{
    smp_user_process_t *process =
        current_process();

    if (
        context == NULL ||
        process == NULL ||
        (context->cs & 0x3ULL) != 0x3ULL
    )
    {
        if (context != NULL)
        {
            context->rax =
                SMP_USER_SYSCALL_ERROR;
        }
        return context;
    }

    __atomic_add_fetch(
        &total_syscalls,
        1,
        __ATOMIC_RELAXED
    );

    switch (context->rax)
    {
        case SYSCALL_CONSOLE_WRITE:
            context->rax = console_write(
                process,
                context->rdi,
                context->rsi
            );
            return context;

        case SYSCALL_GETPID:
            context->rax = process->pid;
            return context;

        case SYSCALL_PROCESS_YIELD:
            context->rax = 0;
            __atomic_add_fetch(
                &total_yields,
                1,
                __ATOMIC_RELAXED
            );
            return smp_scheduler_handle_yield(
                context
            );

        case SYSCALL_PROCESS_KILL:
            if (context->rdi != process->pid)
            {
                context->rax =
                    SMP_USER_SYSCALL_ERROR;
                return context;
            }

            return smp_scheduler_exit_current_user(
                context,
                -9
            );

        case SYSCALL_EXIT:
            return smp_scheduler_exit_current_user(
                context,
                (int64_t)context->rdi
            );

        default:
            context->rax =
                SMP_USER_SYSCALL_ERROR;
            return context;
    }
}

cpu_context_t *smp_user_fault_from_exception(
    cpu_context_t *context,
    uint64_t vector,
    uint64_t error_code,
    uint64_t fault_address
)
{
    smp_user_process_t *process =
        current_process();

    if (
        process == NULL ||
        context == NULL
    )
    {
        return context;
    }

    process->fault_vector = vector;
    process->fault_error = error_code;
    process->fault_address = fault_address;

    return smp_scheduler_exit_current_user(
        context,
        -(int64_t)(256ULL + vector)
    );
}

void smp_user_print_status(void)
{
    if (!initialized)
    {
        kprintf(
            "SMP user-process support is not initialized\n"
        );
        return;
    }

    reap_completed();

    uint64_t flags =
        spinlock_lock_irqsave(
            &user_lock
        );

    uint32_t active = 0;

    for (
        uint32_t slot = 0;
        slot < SMP_USER_MAX_PROCESSES;
        slot++
    )
    {
        if (
            state_load(&processes[slot].state) !=
            SMP_USER_FREE
        )
        {
            active++;
        }
    }

    kprintf(
        "SMP ring-3 users: started=%llu completed=%llu faulted=%llu active=%u syscalls=%llu yields=%llu migrated=%llu console-bytes=%llu checksum=0x%llX\n",
        (unsigned long long)__atomic_load_n(
            &total_started,
            __ATOMIC_RELAXED
        ),
        (unsigned long long)__atomic_load_n(
            &total_completed,
            __ATOMIC_RELAXED
        ),
        (unsigned long long)__atomic_load_n(
            &total_faulted,
            __ATOMIC_RELAXED
        ),
        (unsigned int)active,
        (unsigned long long)__atomic_load_n(
            &total_syscalls,
            __ATOMIC_RELAXED
        ),
        (unsigned long long)__atomic_load_n(
            &total_yields,
            __ATOMIC_RELAXED
        ),
        (unsigned long long)__atomic_load_n(
            &total_migrations,
            __ATOMIC_RELAXED
        ),
        (unsigned long long)__atomic_load_n(
            &total_console_bytes,
            __ATOMIC_RELAXED
        ),
        (unsigned long long)__atomic_load_n(
            &total_console_checksum,
            __ATOMIC_RELAXED
        )
    );

    for (
        uint32_t slot = 0;
        slot < SMP_USER_MAX_PROCESSES;
        slot++
    )
    {
        smp_user_process_t *process =
            &processes[slot];

        if (
            state_load(&process->state) ==
            SMP_USER_FREE
        )
        {
            continue;
        }

        kprintf(
            "  pid=%llu thread=%llu requested-cpu=%u last-cpu=%u state=%s iterations=%llu\n",
            (unsigned long long)process->pid,
            (unsigned long long)process->thread_id,
            (unsigned int)process->requested_cpu,
            (unsigned int)process->last_cpu,
            state_load(&process->state) ==
                SMP_USER_COMPLETE ?
                "complete" :
                "running",
            (unsigned long long)process->iterations
        );
    }

    spinlock_unlock_irqrestore(
        &user_lock,
        flags
    );
}
