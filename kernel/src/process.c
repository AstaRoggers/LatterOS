#include "process.h"

#include "cpu_local.h"
#include "executable.h"
#include "gdt.h"
#include "hhdm.h"
#include "irq.h"
#include "klog.h"
#include "page_allocator.h"
#include "paging.h"
#include "physical_memory.h"
#include "security.h"
#include "smp.h"
#include "smp_scheduler.h"
#include "spinlock.h"
#include "terminal.h"
#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INITIAL_RFLAGS       0x202ULL
#define SCHEDULER_VECTOR     0x81

#define USER_BASE_ADDRESS    0x0000004000000000ULL
#define USER_REGION_STRIDE   0x0000000000400000ULL
#define USER_CODE_OFFSET     0x0000000000000000ULL
#define USER_STACK_OFFSET    0x0000000000200000ULL

#define KERNEL_STACK_REGION_BASE \
    0xFFFFFE0000000000ULL
#define KERNEL_STACK_STRIDE \
    (PAGE_SIZE * 3ULL)
#define KERNEL_STACK_GUARD_OFFSET 0ULL
#define KERNEL_STACK_PAGE_OFFSET  PAGE_SIZE

extern void process_task_bootstrap(void);

static process_t processes[PROCESS_MAX_COUNT];
static volatile uint64_t demo_counters[
    PROCESS_MAX_COUNT
];

static spinlock_t process_lock;
static uint32_t bsp_current_index;
static uint64_t next_pid;
static uint32_t demo_thread_number;
static bool initialized;

static void clear_bytes(
    void *pointer,
    size_t count
)
{
    uint8_t *bytes = (uint8_t *)pointer;

    for (
        size_t index = 0;
        index < count;
        index++
    )
    {
        bytes[index] = 0;
    }
}

static void clear_process(process_t *process)
{
    clear_bytes(
        process,
        sizeof(*process)
    );

    process->state = PROCESS_UNUSED;
    process->mode = PROCESS_KERNEL;
    process->assigned_cpu = PROCESS_CPU_NONE;
    process->running_cpu = PROCESS_CPU_NONE;
    process->retired_cpu = PROCESS_CPU_NONE;
}

static void copy_name(
    char *destination,
    const char *source
)
{
    uint32_t index = 0;

    while (
        source != NULL &&
        source[index] != '\0' &&
        index < PROCESS_NAME_LENGTH - 1
    )
    {
        destination[index] = source[index];
        index++;
    }

    destination[index] = '\0';
}

static const char *path_name(
    const char *path
)
{
    const char *name = path;

    for (
        uint32_t index = 0;
        path[index] != '\0';
        index++
    )
    {
        if (path[index] == '/')
        {
            name = &path[index + 1];
        }
    }

    return name;
}

static uint64_t kernel_guard_for_slot(
    uint32_t slot
)
{
    return
        KERNEL_STACK_REGION_BASE +
        (uint64_t)slot *
            KERNEL_STACK_STRIDE +
        KERNEL_STACK_GUARD_OFFSET;
}

static uint64_t kernel_stack_for_slot(
    uint32_t slot
)
{
    return
        KERNEL_STACK_REGION_BASE +
        (uint64_t)slot *
            KERNEL_STACK_STRIDE +
        KERNEL_STACK_PAGE_OFFSET;
}

static uint32_t current_cpu_index(void)
{
    if (!initialized)
    {
        return 0;
    }

    uint32_t cpu_index =
        smp_current_cpu_index();

    if (cpu_index >= CPU_LOCAL_MAX_CPUS)
    {
        return 0;
    }

    return cpu_index;
}

static uint32_t current_slot_unlocked(void)
{
    uint32_t cpu_index = current_cpu_index();

    if (cpu_index == 0)
    {
        return bsp_current_index;
    }

    cpu_local_t *local =
        cpu_local_current();

    if (
        local == NULL ||
        local->current_process_slot >=
            PROCESS_MAX_COUNT
    )
    {
        return 0;
    }

    uint32_t slot =
        local->current_process_slot;

    if (
        processes[slot].state ==
            PROCESS_UNUSED ||
        processes[slot].pid !=
            local->current_pid
    )
    {
        return 0;
    }

    return slot;
}

static bool process_can_reap_locked(
    uint32_t slot,
    uint32_t excluded_slot
)
{
    if (
        slot == 0 ||
        slot == excluded_slot ||
        slot >= PROCESS_MAX_COUNT
    )
    {
        return false;
    }

    process_t *process =
        &processes[slot];

    if (
        process->state != PROCESS_TERMINATED ||
        process->scheduler_enqueued ||
        process->running_cpu != PROCESS_CPU_NONE
    )
    {
        return false;
    }

    if (
        !process->smp_managed ||
        process->retired_cpu == PROCESS_CPU_NONE
    )
    {
        return true;
    }

    return (
        smp_scheduler_cpu_sequence(
            process->retired_cpu
        ) > process->retired_sequence
    );
}

static void destroy_process_slot_locked(
    uint32_t slot
)
{
    if (
        slot == 0 ||
        slot >= PROCESS_MAX_COUNT
    )
    {
        return;
    }

    process_t *process =
        &processes[slot];

    if (process->mode == PROCESS_USER)
    {
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

        if (process->user_code_page != NULL)
        {
            free_page(
                process->user_code_page
            );
        }

        if (process->user_stack_page != NULL)
        {
            free_page(
                process->user_stack_page
            );
        }
    }

    if (process->kernel_stack_virtual != 0)
    {
        (void)paging_unmap_page_in(
            paging_kernel_root(),
            process->kernel_stack_virtual
        );
    }

    if (process->kernel_stack_page != NULL)
    {
        free_page(
            process->kernel_stack_page
        );
    }

    clear_process(process);
}

static void reap_terminated_locked(
    uint32_t excluded_slot
)
{
    for (
        uint32_t slot = 1;
        slot < PROCESS_MAX_COUNT;
        slot++
    )
    {
        if (process_can_reap_locked(
                slot,
                excluded_slot
            ))
        {
            destroy_process_slot_locked(slot);
        }
    }
}

static int32_t find_free_slot_locked(void)
{
    reap_terminated_locked(
        current_slot_unlocked()
    );

    for (
        uint32_t slot = 1;
        slot < PROCESS_MAX_COUNT;
        slot++
    )
    {
        if (
            processes[slot].state ==
                PROCESS_UNUSED
        )
        {
            return (int32_t)slot;
        }
    }

    return -1;
}

static cpu_context_t *create_kernel_context(
    uint64_t stack_virtual,
    kernel_thread_entry_t entry,
    void *argument
)
{
    uint8_t *stack_top =
        (uint8_t *)stack_virtual + PAGE_SIZE;

    cpu_user_context_t *frame =
        (cpu_user_context_t *)(
            stack_top -
            sizeof(cpu_user_context_t)
        );

    clear_bytes(
        frame,
        sizeof(*frame)
    );

    frame->base.r12 = (uint64_t)entry;
    frame->base.r13 = (uint64_t)argument;
    frame->base.rip =
        (uint64_t)process_task_bootstrap;
    frame->base.cs =
        GDT_KERNEL_CODE_SELECTOR;
    frame->base.rflags = INITIAL_RFLAGS;
    frame->rsp = (uint64_t)stack_top;
    frame->ss = GDT_KERNEL_DATA_SELECTOR;

    return &frame->base;
}

static cpu_context_t *create_user_context(
    uint64_t kernel_stack_virtual,
    uint64_t code_virtual,
    uint64_t stack_virtual
)
{
    uint8_t *kernel_stack_top =
        (uint8_t *)kernel_stack_virtual +
        PAGE_SIZE;

    cpu_user_context_t *context =
        (cpu_user_context_t *)(
            kernel_stack_top -
            sizeof(cpu_user_context_t)
        );

    clear_bytes(
        context,
        sizeof(*context)
    );

    context->base.rip = code_virtual;
    context->base.cs = GDT_USER_CODE_SELECTOR;
    context->base.rflags = INITIAL_RFLAGS;
    context->rsp =
        stack_virtual + PAGE_SIZE - 16;
    context->ss = GDT_USER_DATA_SELECTOR;

    return &context->base;
}

static void select_process(
    const process_t *process
)
{
    uint64_t root = paging_kernel_root();

    if (
        process != NULL &&
        process->page_table_root != 0
    )
    {
        root = process->page_table_root;
    }

    paging_activate(root);

    if (
        process != NULL &&
        process->kernel_stack_top != 0
    )
    {
        gdt_set_kernel_stack(
            process->kernel_stack_top
        );
    }
}

static uint32_t find_next_bsp_ready_locked(void)
{
    for (
        uint32_t offset = 1;
        offset <= PROCESS_MAX_COUNT;
        offset++
    )
    {
        uint32_t slot =
            (bsp_current_index + offset) %
            PROCESS_MAX_COUNT;

        process_t *process =
            &processes[slot];

        if (
            process->state == PROCESS_READY &&
            !process->scheduler_enqueued &&
            (
                process->assigned_cpu == 0 ||
                process->assigned_cpu ==
                    PROCESS_CPU_NONE
            )
        )
        {
            return slot;
        }
    }

    return bsp_current_index;
}

static cpu_context_t *schedule_bsp(
    cpu_context_t *context,
    bool charge_tick
)
{
    if (
        !initialized ||
        context == NULL
    )
    {
        return context;
    }

    spinlock_lock(&process_lock);

    reap_terminated_locked(
        bsp_current_index
    );

    process_t *current =
        &processes[bsp_current_index];

    current->context = context;

    if (charge_tick)
    {
        current->cpu_ticks++;
    }

    if (current->state == PROCESS_RUNNING)
    {
        current->state = PROCESS_READY;
    }

    uint32_t next_slot =
        find_next_bsp_ready_locked();

    process_t *next =
        &processes[next_slot];

    if (
        next->state != PROCESS_READY ||
        next->context == NULL
    )
    {
        if (
            current->state !=
                PROCESS_TERMINATED
        )
        {
            current->state = PROCESS_RUNNING;
            current->assigned_cpu = 0;
            current->running_cpu = 0;
            select_process(current);
            spinlock_unlock(&process_lock);
            return context;
        }

        process_t *kernel = &processes[0];
        kernel->state = PROCESS_RUNNING;
        kernel->assigned_cpu = 0;
        kernel->running_cpu = 0;
        bsp_current_index = 0;
        select_process(kernel);

        cpu_context_t *result =
            kernel->context != NULL ?
            kernel->context :
            context;

        spinlock_unlock(&process_lock);
        return result;
    }

    current->running_cpu = PROCESS_CPU_NONE;

    next->state = PROCESS_RUNNING;
    next->assigned_cpu = 0;
    next->running_cpu = 0;
    next->switches++;
    bsp_current_index = next_slot;

    select_process(next);

    cpu_context_t *result = next->context;
    spinlock_unlock(&process_lock);
    return result;
}

static void process_smp_completion(
    void *owner,
    int64_t status,
    uint32_t cpu_index
)
{
    process_t *process =
        (process_t *)owner;

    if (process == NULL)
    {
        return;
    }

    uint64_t retired_sequence =
        smp_scheduler_cpu_sequence(
            cpu_index
        );

    uint64_t flags =
        spinlock_lock_irqsave(
            &process_lock
        );

    if (
        process->state != PROCESS_UNUSED
    )
    {
        process->exit_status = status;
        process->state = PROCESS_TERMINATED;
        process->scheduler_enqueued = false;
        process->running_cpu = PROCESS_CPU_NONE;
        process->assigned_cpu = cpu_index;
        process->retired_cpu = cpu_index;
        process->retired_sequence =
            retired_sequence;
        process->switches++;
    }

    spinlock_unlock_irqrestore(
        &process_lock,
        flags
    );
}

static void dispatch_user_process(
    uint32_t slot
)
{
    if (slot >= PROCESS_MAX_COUNT)
    {
        return;
    }

    process_t *process =
        &processes[slot];

    uint64_t prepare_flags =
        spinlock_lock_irqsave(
            &process_lock
        );

    if (
        process->state == PROCESS_UNUSED ||
        process->mode != PROCESS_USER ||
        process->context == NULL
    )
    {
        spinlock_unlock_irqrestore(
            &process_lock,
            prepare_flags
        );
        return;
    }

    process->smp_managed = true;
    process->scheduler_enqueued = true;
    process->state = PROCESS_RUNNING;
    process->assigned_cpu = PROCESS_CPU_NONE;
    process->running_cpu = PROCESS_CPU_NONE;
    process->retired_cpu = PROCESS_CPU_NONE;
    process->retired_sequence = 0;

    spinlock_unlock_irqrestore(
        &process_lock,
        prepare_flags
    );

    uint32_t cpu_index = PROCESS_CPU_NONE;
    uint64_t thread_id = 0;

    bool submitted =
        smp_scheduler_submit_process_any(
            process->context,
            process->page_table_root,
            process->kernel_stack_top,
            slot,
            process->pid,
            process,
            process_smp_completion,
            &cpu_index,
            &thread_id
        );

    uint64_t flags =
        spinlock_lock_irqsave(
            &process_lock
        );

    if (
        process->state == PROCESS_UNUSED ||
        process->pid == 0
    )
    {
        spinlock_unlock_irqrestore(
            &process_lock,
            flags
        );
        return;
    }

    if (submitted)
    {
        if (process->scheduler_enqueued)
        {
            process->assigned_cpu = cpu_index;
            process->running_cpu = cpu_index;
            process->scheduler_thread_id =
                thread_id;
        }
    }
    else
    {
        process->smp_managed = false;
        process->scheduler_enqueued = false;
        process->state = PROCESS_READY;
        process->assigned_cpu = 0;
        process->running_cpu = PROCESS_CPU_NONE;
        process->scheduler_thread_id = 0;
    }

    spinlock_unlock_irqrestore(
        &process_lock,
        flags
    );
}

static void demo_thread(void *argument)
{
    volatile uint64_t *counter =
        (volatile uint64_t *)argument;

    for (;;)
    {
        (*counter)++;
        __asm__ volatile(
            "pause"
            :
            :
            : "memory"
        );
    }
}

static void uint64_to_string(
    uint64_t value,
    char *buffer
)
{
    char temporary[21];
    uint32_t length = 0;

    if (value == 0)
    {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }

    while (value > 0)
    {
        temporary[length++] =
            (char)('0' + value % 10);
        value /= 10;
    }

    for (
        uint32_t index = 0;
        index < length;
        index++
    )
    {
        buffer[index] =
            temporary[length - index - 1];
    }

    buffer[length] = '\0';
}

static const char *state_name(
    process_state_t state
)
{
    switch (state)
    {
        case PROCESS_READY:
            return "READY";

        case PROCESS_RUNNING:
            return "RUNNING";

        case PROCESS_TERMINATED:
            return "TERMINATED";

        default:
            return "UNUSED";
    }
}

static const char *mode_name(
    process_mode_t mode
)
{
    return mode == PROCESS_USER ?
        "USER" :
        "KERNEL";
}

void process_init(void)
{
    irq_disable();
    spinlock_init(&process_lock);

    for (
        uint32_t slot = 0;
        slot < PROCESS_MAX_COUNT;
        slot++
    )
    {
        clear_process(&processes[slot]);
        demo_counters[slot] = 0;
    }

    process_t *kernel = &processes[0];

    kernel->pid = 0;
    kernel->parent_pid = 0;
    kernel->page_table_root =
        paging_kernel_root();
    copy_name(kernel->name, "kernel");
    kernel->state = PROCESS_RUNNING;
    kernel->mode = PROCESS_KERNEL;
    kernel->uid = SECURITY_UID_ROOT;
    kernel->gid = SECURITY_GID_ROOT;
    kernel->capabilities = SECURITY_CAP_ALL;
    kernel->switches = 1;
    kernel->assigned_cpu = 0;
    kernel->running_cpu = 0;

    bsp_current_index = 0;
    next_pid = 1;
    demo_thread_number = 0;
    initialized = true;
}

bool process_create_kernel_thread(
    const char *name,
    kernel_thread_entry_t entry,
    void *argument
)
{
    if (
        !initialized ||
        entry == NULL
    )
    {
        return false;
    }

    uint64_t flags =
        spinlock_lock_irqsave(
            &process_lock
        );

    int32_t slot = find_free_slot_locked();

    if (slot < 0)
    {
        spinlock_unlock_irqrestore(
            &process_lock,
            flags
        );
        return false;
    }

    void *kernel_stack_page = alloc_page();

    if (kernel_stack_page == NULL)
    {
        spinlock_unlock_irqrestore(
            &process_lock,
            flags
        );
        return false;
    }

    uint64_t kernel_guard =
        kernel_guard_for_slot(
            (uint32_t)slot
        );

    uint64_t kernel_stack_virtual =
        kernel_stack_for_slot(
            (uint32_t)slot
        );

    (void)paging_unmap_page_in(
        paging_kernel_root(),
        kernel_guard
    );

    (void)paging_unmap_page_in(
        paging_kernel_root(),
        kernel_stack_virtual
    );

    if (!paging_map_kernel_page(
            kernel_stack_virtual,
            (uint64_t)kernel_stack_page,
            true
        ))
    {
        free_page(kernel_stack_page);
        spinlock_unlock_irqrestore(
            &process_lock,
            flags
        );
        return false;
    }

    process_t *process =
        &processes[slot];

    clear_process(process);
    process->pid = next_pid++;
    process->parent_pid = 0;
    process->page_table_root =
        paging_kernel_root();
    copy_name(process->name, name);
    process->state = PROCESS_READY;
    process->mode = PROCESS_KERNEL;
    process->uid = SECURITY_UID_ROOT;
    process->gid = SECURITY_GID_ROOT;
    process->capabilities = SECURITY_CAP_ALL;
    process->kernel_stack_page =
        kernel_stack_page;
    process->kernel_stack_virtual =
        kernel_stack_virtual;
    process->kernel_stack_guard =
        kernel_guard;
    process->kernel_stack_top =
        kernel_stack_virtual + PAGE_SIZE;
    process->context =
        create_kernel_context(
            kernel_stack_virtual,
            entry,
            argument
        );
    process->assigned_cpu = 0;

    spinlock_unlock_irqrestore(
        &process_lock,
        flags
    );

    return true;
}

bool process_spawn_demo_thread(void)
{
    if (
        demo_thread_number >=
        PROCESS_MAX_COUNT - 1
    )
    {
        return false;
    }

    char name[PROCESS_NAME_LENGTH] =
        "worker-";

    uint32_t number =
        demo_thread_number + 1;
    uint32_t position = 7;

    if (number >= 10)
    {
        name[position++] =
            (char)('0' + number / 10);
    }

    name[position++] =
        (char)('0' + number % 10);
    name[position] = '\0';

    bool created =
        process_create_kernel_thread(
            name,
            demo_thread,
            (void *)&demo_counters[
                demo_thread_number
            ]
        );

    if (created)
    {
        demo_thread_number++;
    }

    return created;
}

uint64_t process_create_user_program(
    const char *path
)
{
    if (
        !initialized ||
        path == NULL
    )
    {
        return 0;
    }

    vfs_node_t *file = vfs_open(path);

    if (
        file == NULL ||
        file->type != VFS_NODE_FILE ||
        !vfs_check_access(
            file,
            VFS_ACCESS_EXECUTE
        )
    )
    {
        return 0;
    }

    uint64_t flags =
        spinlock_lock_irqsave(
            &process_lock
        );

    int32_t slot = find_free_slot_locked();

    if (slot < 0)
    {
        spinlock_unlock_irqrestore(
            &process_lock,
            flags
        );
        return 0;
    }

    const process_t *parent =
        process_current();

    uint64_t parent_pid =
        parent != NULL ? parent->pid : 0;
    uint32_t uid;
    uint32_t gid;
    uint64_t capabilities;

    if (
        parent != NULL &&
        parent->mode == PROCESS_USER
    )
    {
        uid = parent->uid;
        gid = parent->gid;
        capabilities = parent->capabilities;
    }
    else
    {
        uid = security_session_uid();
        gid = security_session_gid();
        capabilities =
            security_session_capabilities();
    }

    void *kernel_stack_page = alloc_page();
    void *user_code_page = alloc_page();
    void *user_stack_page = alloc_page();

    if (
        kernel_stack_page == NULL ||
        user_code_page == NULL ||
        user_stack_page == NULL
    )
    {
        if (kernel_stack_page != NULL)
        {
            free_page(kernel_stack_page);
        }
        if (user_code_page != NULL)
        {
            free_page(user_code_page);
        }
        if (user_stack_page != NULL)
        {
            free_page(user_stack_page);
        }

        spinlock_unlock_irqrestore(
            &process_lock,
            flags
        );
        return 0;
    }

    uint64_t region_base =
        USER_BASE_ADDRESS +
        (uint64_t)slot *
            USER_REGION_STRIDE;

    uint64_t code_virtual =
        region_base + USER_CODE_OFFSET;
    uint64_t stack_guard =
        region_base + USER_STACK_OFFSET;
    uint64_t stack_virtual =
        stack_guard + PAGE_SIZE;

    clear_bytes(
        physical_to_virtual(
            (uint64_t)user_code_page
        ),
        PAGE_SIZE
    );

    clear_bytes(
        physical_to_virtual(
            (uint64_t)user_stack_page
        ),
        PAGE_SIZE
    );

    uint64_t kernel_guard =
        kernel_guard_for_slot(
            (uint32_t)slot
        );
    uint64_t kernel_stack_virtual =
        kernel_stack_for_slot(
            (uint32_t)slot
        );

    (void)paging_unmap_page_in(
        paging_kernel_root(),
        kernel_guard
    );

    (void)paging_unmap_page_in(
        paging_kernel_root(),
        kernel_stack_virtual
    );

    bool kernel_stack_mapped =
        paging_map_kernel_page(
            kernel_stack_virtual,
            (uint64_t)kernel_stack_page,
            true
        );

    size_t image_size = 0;
    uint32_t entry_offset = 0;
    uint64_t user_root = 0;
    bool loaded = false;

    if (kernel_stack_mapped)
    {
        loaded = executable_load(
            file,
            physical_to_virtual(
                (uint64_t)user_code_page
            ),
            PAGE_SIZE,
            &image_size,
            &entry_offset
        );
    }

    if (loaded)
    {
        user_root =
            paging_create_user_space();
    }

    bool mapped = false;

    if (user_root != 0)
    {
        mapped =
            paging_map_user_page_in(
                user_root,
                code_virtual,
                (uint64_t)user_code_page,
                false,
                true
            ) &&
            paging_map_user_page_in(
                user_root,
                stack_virtual,
                (uint64_t)user_stack_page,
                true,
                false
            );
    }

    if (!mapped)
    {
        if (
            user_root != 0 &&
            user_root != paging_current_root()
        )
        {
            paging_destroy_user_space(
                user_root
            );
        }

        if (kernel_stack_mapped)
        {
            (void)paging_unmap_page_in(
                paging_kernel_root(),
                kernel_stack_virtual
            );
        }

        free_page(kernel_stack_page);
        free_page(user_code_page);
        free_page(user_stack_page);

        spinlock_unlock_irqrestore(
            &process_lock,
            flags
        );
        return 0;
    }

    process_t *process =
        &processes[slot];

    clear_process(process);
    process->pid = next_pid++;
    process->parent_pid = parent_pid;
    copy_name(
        process->name,
        path_name(path)
    );
    process->state = PROCESS_READY;
    process->mode = PROCESS_USER;
    process->uid = uid;
    process->gid = gid;
    process->capabilities = capabilities;
    process->page_table_root = user_root;
    process->kernel_stack_page =
        kernel_stack_page;
    process->kernel_stack_virtual =
        kernel_stack_virtual;
    process->kernel_stack_guard =
        kernel_guard;
    process->kernel_stack_top =
        kernel_stack_virtual + PAGE_SIZE;
    process->user_code_page =
        user_code_page;
    process->user_stack_page =
        user_stack_page;
    process->user_code_virtual =
        code_virtual;
    process->user_stack_virtual =
        stack_virtual;
    process->user_stack_guard =
        stack_guard;
    process->user_code_size = image_size;
    process->context =
        create_user_context(
            kernel_stack_virtual,
            code_virtual + entry_offset,
            stack_virtual
        );

    uint64_t pid = process->pid;

    spinlock_unlock_irqrestore(
        &process_lock,
        flags
    );

    dispatch_user_process(
        (uint32_t)slot
    );

    return pid;
}

bool process_user_may_signal(uint64_t pid)
{
    const process_t *current =
        process_current();

    if (
        current == NULL ||
        current->mode != PROCESS_USER ||
        pid == 0
    )
    {
        return false;
    }

    if (pid == current->pid)
    {
        return true;
    }

    uint64_t flags =
        spinlock_lock_irqsave(
            &process_lock
        );

    bool allowed = false;

    for (
        uint32_t slot = 1;
        slot < PROCESS_MAX_COUNT;
        slot++
    )
    {
        const process_t *target =
            &processes[slot];

        if (
            target->state == PROCESS_UNUSED ||
            target->pid != pid
        )
        {
            continue;
        }

        allowed =
            (current->capabilities &
                SECURITY_CAP_PROCESS_ADMIN) != 0 ||
            (
                target->uid == current->uid &&
                target->parent_pid ==
                    current->pid
            );
        break;
    }

    spinlock_unlock_irqrestore(
        &process_lock,
        flags
    );

    return allowed;
}

bool process_terminate(uint64_t pid)
{
    if (
        !initialized ||
        pid == 0
    )
    {
        return false;
    }

    uint64_t flags =
        spinlock_lock_irqsave(
            &process_lock
        );

    uint32_t target_slot = PROCESS_MAX_COUNT;
    bool scheduler_enqueued = false;

    for (
        uint32_t slot = 1;
        slot < PROCESS_MAX_COUNT;
        slot++
    )
    {
        process_t *process =
            &processes[slot];

        if (
            process->state == PROCESS_UNUSED ||
            process->pid != pid
        )
        {
            continue;
        }

        target_slot = slot;
        scheduler_enqueued =
            process->scheduler_enqueued;
        break;
    }

    if (target_slot >= PROCESS_MAX_COUNT)
    {
        spinlock_unlock_irqrestore(
            &process_lock,
            flags
        );
        return false;
    }

    if (scheduler_enqueued)
    {
        processes[target_slot]
            .terminate_requested = true;

        spinlock_unlock_irqrestore(
            &process_lock,
            flags
        );

        return smp_scheduler_request_process_stop(
            target_slot,
            pid,
            -9
        );
    }

    if (target_slot == bsp_current_index)
    {
        spinlock_unlock_irqrestore(
            &process_lock,
            flags
        );
        return false;
    }

    processes[target_slot].state =
        PROCESS_TERMINATED;
    processes[target_slot].running_cpu =
        PROCESS_CPU_NONE;
    destroy_process_slot_locked(
        target_slot
    );

    spinlock_unlock_irqrestore(
        &process_lock,
        flags
    );

    return true;
}

cpu_context_t *process_schedule_on_timer(
    cpu_context_t *context
)
{
    if (current_cpu_index() != 0)
    {
        return smp_scheduler_handle_timer(
            context
        );
    }

    return schedule_bsp(
        context,
        true
    );
}

cpu_context_t *process_schedule_now(
    cpu_context_t *context
)
{
    if (current_cpu_index() != 0)
    {
        return smp_scheduler_handle_yield(
            context
        );
    }

    return schedule_bsp(
        context,
        false
    );
}

cpu_context_t *process_exit_from_syscall(
    cpu_context_t *context,
    int64_t status
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
        current_cpu_index();
    uint32_t slot =
        current_slot_unlocked();

    if (slot == 0 || slot >= PROCESS_MAX_COUNT)
    {
        return context;
    }

    uint64_t flags =
        spinlock_lock_irqsave(
            &process_lock
        );

    process_t *current =
        &processes[slot];

    current->context = context;
    current->exit_status = status;
    current->state = PROCESS_TERMINATED;
    current->running_cpu = PROCESS_CPU_NONE;

    spinlock_unlock_irqrestore(
        &process_lock,
        flags
    );

    if (cpu_index != 0)
    {
        return smp_scheduler_exit_current_user(
            context,
            status
        );
    }

    return schedule_bsp(
        context,
        false
    );
}

cpu_context_t *process_fault_from_exception(
    cpu_context_t *context,
    uint64_t vector,
    uint64_t error_code,
    uint64_t fault_address
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
        current_cpu_index();
    uint32_t slot =
        current_slot_unlocked();

    if (slot == 0 || slot >= PROCESS_MAX_COUNT)
    {
        return context;
    }

    process_t *current =
        &processes[slot];

    if (current->mode != PROCESS_USER)
    {
        return context;
    }

    klogf(
        KLOG_WARNING,
        "process",
        "terminated user pid=%llu name=%s cpu=%u exception=%llu error=0x%llx address=0x%llx",
        (unsigned long long)current->pid,
        current->name,
        (unsigned int)cpu_index,
        (unsigned long long)vector,
        (unsigned long long)error_code,
        (unsigned long long)fault_address
    );

    int64_t status =
        -(int64_t)(256 + vector);

    uint64_t flags =
        spinlock_lock_irqsave(
            &process_lock
        );

    current->context = context;
    current->exit_status = status;
    current->state = PROCESS_TERMINATED;
    current->running_cpu = PROCESS_CPU_NONE;

    spinlock_unlock_irqrestore(
        &process_lock,
        flags
    );

    if (cpu_index != 0)
    {
        return smp_scheduler_exit_current_user(
            context,
            status
        );
    }

    return schedule_bsp(
        context,
        false
    );
}

void process_exit_current(void)
{
    irq_disable();

    if (current_cpu_index() != 0)
    {
        smp_scheduler_exit_current(0);
    }

    uint32_t slot = bsp_current_index;

    spinlock_lock(&process_lock);

    if (slot != 0)
    {
        processes[slot].state =
            PROCESS_TERMINATED;
        processes[slot].running_cpu =
            PROCESS_CPU_NONE;
    }

    spinlock_unlock(&process_lock);

    __asm__ volatile(
        "int $0x81"
        :
        :
        : "memory"
    );

    for (;;)
    {
        __asm__ volatile("hlt");
    }
}

bool process_user_range_readable(
    uint64_t address,
    size_t size
)
{
    const process_t *process =
        process_current();

    return (
        process != NULL &&
        process->mode == PROCESS_USER &&
        paging_user_range_valid(
            process->page_table_root,
            address,
            size,
            false
        )
    );
}

bool process_user_range_writable(
    uint64_t address,
    size_t size
)
{
    const process_t *process =
        process_current();

    return (
        process != NULL &&
        process->mode == PROCESS_USER &&
        paging_user_range_valid(
            process->page_table_root,
            address,
            size,
            true
        )
    );
}

bool process_user_range_valid(
    uint64_t address,
    size_t size
)
{
    return process_user_range_readable(
        address,
        size
    );
}

uint32_t process_count(void)
{
    if (!initialized)
    {
        return 0;
    }

    uint64_t flags =
        spinlock_lock_irqsave(
            &process_lock
        );

    uint32_t count = 0;

    for (
        uint32_t slot = 0;
        slot < PROCESS_MAX_COUNT;
        slot++
    )
    {
        if (
            processes[slot].state !=
                PROCESS_UNUSED
        )
        {
            count++;
        }
    }

    spinlock_unlock_irqrestore(
        &process_lock,
        flags
    );

    return count;
}

const process_t *process_get(
    uint32_t index
)
{
    if (!initialized)
    {
        return NULL;
    }

    uint32_t current = 0;

    for (
        uint32_t slot = 0;
        slot < PROCESS_MAX_COUNT;
        slot++
    )
    {
        if (
            processes[slot].state ==
                PROCESS_UNUSED
        )
        {
            continue;
        }

        if (current == index)
        {
            return &processes[slot];
        }

        current++;
    }

    return NULL;
}

const process_t *process_current(void)
{
    if (!initialized)
    {
        return NULL;
    }

    uint32_t slot =
        current_slot_unlocked();

    if (slot >= PROCESS_MAX_COUNT)
    {
        return NULL;
    }

    return &processes[slot];
}

void process_print_all(void)
{
    terminal_write_line(
        "PID  CPU  MODE    STATE       TICKS  SWITCHES  NAME"
    );

    uint64_t flags =
        spinlock_lock_irqsave(
            &process_lock
        );

    for (
        uint32_t slot = 0;
        slot < PROCESS_MAX_COUNT;
        slot++
    )
    {
        const process_t *process =
            &processes[slot];

        if (
            process->state ==
                PROCESS_UNUSED
        )
        {
            continue;
        }

        char number[21];

        uint64_to_string(
            process->pid,
            number
        );
        terminal_write(number);
        terminal_write("    ");

        if (
            process->assigned_cpu ==
                PROCESS_CPU_NONE
        )
        {
            terminal_write("-    ");
        }
        else
        {
            uint64_to_string(
                process->assigned_cpu,
                number
            );
            terminal_write(number);
            terminal_write("    ");
        }

        terminal_write(
            mode_name(process->mode)
        );
        terminal_write("    ");
        terminal_write(
            state_name(process->state)
        );
        terminal_write("    ");

        uint64_to_string(
            process->cpu_ticks,
            number
        );
        terminal_write(number);
        terminal_write("      ");

        uint64_to_string(
            process->switches,
            number
        );
        terminal_write(number);
        terminal_write("         ");
        terminal_write_line(process->name);
    }

    spinlock_unlock_irqrestore(
        &process_lock,
        flags
    );
}

uint32_t process_guarded_stack_count(void)
{
    uint64_t flags =
        spinlock_lock_irqsave(
            &process_lock
        );

    uint32_t count = 0;

    for (
        uint32_t slot = 1;
        slot < PROCESS_MAX_COUNT;
        slot++
    )
    {
        const process_t *process =
            &processes[slot];

        if (
            process->state != PROCESS_UNUSED &&
            process->kernel_stack_virtual != 0
        )
        {
            count++;
        }
    }

    spinlock_unlock_irqrestore(
        &process_lock,
        flags
    );

    return count;
}

bool process_guard_pages_validate(void)
{
    uint64_t flags =
        spinlock_lock_irqsave(
            &process_lock
        );

    bool valid = true;

    for (
        uint32_t slot = 1;
        slot < PROCESS_MAX_COUNT;
        slot++
    )
    {
        const process_t *process =
            &processes[slot];

        if (process->state == PROCESS_UNUSED)
        {
            continue;
        }

        if (
            process->kernel_stack_virtual == 0 ||
            process->kernel_stack_guard == 0 ||
            paging_is_mapped(
                process->kernel_stack_guard
            ) ||
            !paging_is_mapped(
                process->kernel_stack_virtual
            )
        )
        {
            valid = false;
            break;
        }

        if (
            process->mode == PROCESS_USER &&
            (
                process->user_stack_guard == 0 ||
                paging_is_mapped_in(
                    process->page_table_root,
                    process->user_stack_guard
                ) ||
                !paging_is_mapped_in(
                    process->page_table_root,
                    process->user_stack_virtual
                )
            )
        )
        {
            valid = false;
            break;
        }
    }

    spinlock_unlock_irqrestore(
        &process_lock,
        flags
    );

    return valid;
}
