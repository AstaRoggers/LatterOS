#include "process.h"

#include "gdt.h"
#include "hhdm.h"
#include "irq.h"
#include "page_allocator.h"
#include "paging.h"
#include "physical_memory.h"
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

extern void process_task_bootstrap(void);

static process_t processes[PROCESS_MAX_COUNT];
static uint32_t current_index;
static uint64_t next_pid;
static uint32_t demo_thread_number;
static bool initialized;

static volatile uint64_t demo_counters[
    PROCESS_MAX_COUNT
];

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

static void clear_process(process_t *process)
{
    clear_bytes(
        process,
        sizeof(process_t)
    );

    process->state = PROCESS_UNUSED;
    process->mode = PROCESS_KERNEL;
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

static void destroy_process_slot(
    uint32_t slot
)
{
    process_t *process =
        &processes[slot];

    if (
        process->mode == PROCESS_USER
    )
    {
        if (process->user_code_virtual != 0)
        {
            paging_unmap_page(
                process->user_code_virtual
            );
        }

        if (process->user_stack_virtual != 0)
        {
            paging_unmap_page(
                process->user_stack_virtual
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

    if (process->kernel_stack_page != NULL)
    {
        free_page(
            process->kernel_stack_page
        );
    }

    clear_process(process);
}

static void reap_terminated(
    uint32_t excluded_slot
)
{
    for (
        uint32_t slot = 1;
        slot < PROCESS_MAX_COUNT;
        slot++
    )
    {
        if (
            slot != excluded_slot &&
            processes[slot].state ==
                PROCESS_TERMINATED
        )
        {
            destroy_process_slot(slot);
        }
    }
}

static int32_t find_free_slot(void)
{
    reap_terminated(current_index);

    for (
        uint32_t index = 1;
        index < PROCESS_MAX_COUNT;
        index++
    )
    {
        if (
            processes[index].state ==
            PROCESS_UNUSED
        )
        {
            return (int32_t)index;
        }
    }

    return -1;
}

static cpu_context_t *create_kernel_context(
    void *physical_stack_page,
    kernel_thread_entry_t entry,
    void *argument
)
{
    uint8_t *stack_base =
        physical_to_virtual(
            (uint64_t)physical_stack_page
        );

    uint8_t *stack_top =
        stack_base + PAGE_SIZE;

    cpu_context_t *context =
        (cpu_context_t *)(
            stack_top -
            sizeof(cpu_context_t)
        );

    clear_bytes(
        context,
        sizeof(cpu_context_t)
    );

    context->r12 =
        (uint64_t)entry;

    context->r13 =
        (uint64_t)argument;

    context->rip =
        (uint64_t)process_task_bootstrap;

    context->cs =
        GDT_KERNEL_CODE_SELECTOR;

    context->rflags =
        INITIAL_RFLAGS;

    return context;
}

static cpu_context_t *create_user_context(
    void *physical_kernel_stack,
    uint64_t code_virtual,
    uint64_t stack_virtual
)
{
    uint8_t *kernel_stack_base =
        physical_to_virtual(
            (uint64_t)physical_kernel_stack
        );

    uint8_t *kernel_stack_top =
        kernel_stack_base + PAGE_SIZE;

    cpu_user_context_t *context =
        (cpu_user_context_t *)(
            kernel_stack_top -
            sizeof(cpu_user_context_t)
        );

    clear_bytes(
        context,
        sizeof(cpu_user_context_t)
    );

    context->base.rip =
        code_virtual;

    context->base.cs =
        GDT_USER_CODE_SELECTOR;

    context->base.rflags =
        INITIAL_RFLAGS;

    context->rsp =
        stack_virtual + PAGE_SIZE - 16;

    context->ss =
        GDT_USER_DATA_SELECTOR;

    return &context->base;
}

static uint32_t find_next_ready(void)
{
    for (
        uint32_t offset = 1;
        offset <= PROCESS_MAX_COUNT;
        offset++
    )
    {
        uint32_t index =
            (current_index + offset) %
            PROCESS_MAX_COUNT;

        if (
            processes[index].state ==
            PROCESS_READY
        )
        {
            return index;
        }
    }

    return current_index;
}

static void select_kernel_stack(
    const process_t *process
)
{
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

static cpu_context_t *schedule(
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

    reap_terminated(current_index);

    process_t *current =
        &processes[current_index];

    current->context = context;

    if (charge_tick)
    {
        current->cpu_ticks++;
    }

    if (
        current->state ==
        PROCESS_RUNNING
    )
    {
        current->state =
            PROCESS_READY;
    }

    uint32_t next_index =
        find_next_ready();

    process_t *next =
        &processes[next_index];

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
            current->state =
                PROCESS_RUNNING;

            select_kernel_stack(current);
            return context;
        }

        /* The kernel process is always expected to remain runnable. */
        process_t *kernel =
            &processes[0];

        kernel->state =
            PROCESS_RUNNING;

        current_index = 0;
        select_kernel_stack(kernel);

        return kernel->context != NULL ?
            kernel->context :
            context;
    }

    next->state = PROCESS_RUNNING;
    next->switches++;
    current_index = next_index;

    select_kernel_stack(next);

    return next->context;
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
        temporary[length] =
            (char)(
                '0' +
                value % 10
            );

        length++;
        value /= 10;
    }

    for (
        uint32_t index = 0;
        index < length;
        index++
    )
    {
        buffer[index] =
            temporary[
                length - index - 1
            ];
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

    for (
        uint32_t index = 0;
        index < PROCESS_MAX_COUNT;
        index++
    )
    {
        clear_process(
            &processes[index]
        );

        demo_counters[index] = 0;
    }

    process_t *kernel =
        &processes[0];

    kernel->pid = 0;
    copy_name(
        kernel->name,
        "kernel"
    );

    kernel->state = PROCESS_RUNNING;
    kernel->mode = PROCESS_KERNEL;
    kernel->switches = 1;

    current_index = 0;
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

    irq_disable();

    int32_t slot = find_free_slot();

    if (slot < 0)
    {
        return false;
    }

    void *kernel_stack_page =
        alloc_page();

    if (kernel_stack_page == NULL)
    {
        return false;
    }

    process_t *process =
        &processes[slot];

    clear_process(process);

    process->pid = next_pid++;
    copy_name(process->name, name);
    process->state = PROCESS_READY;
    process->mode = PROCESS_KERNEL;
    process->kernel_stack_page =
        kernel_stack_page;

    process->kernel_stack_top =
        (uint64_t)physical_to_virtual(
            (uint64_t)kernel_stack_page
        ) + PAGE_SIZE;

    process->context =
        create_kernel_context(
            kernel_stack_page,
            entry,
            argument
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
            (char)(
                '0' +
                number / 10
            );
    }

    name[position++] =
        (char)(
            '0' +
            number % 10
        );

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

    vfs_node_t *file =
        vfs_open(path);

    if (
        file == NULL ||
        file->type != VFS_NODE_FILE ||
        file->size == 0 ||
        file->size > PAGE_SIZE
    )
    {
        return 0;
    }

    irq_disable();

    int32_t slot = find_free_slot();

    if (slot < 0)
    {
        return 0;
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

        return 0;
    }

    uint64_t region_base =
        USER_BASE_ADDRESS +
        (uint64_t)slot *
            USER_REGION_STRIDE;

    uint64_t code_virtual =
        region_base +
        USER_CODE_OFFSET;

    uint64_t stack_virtual =
        region_base +
        USER_STACK_OFFSET;

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

    if (
        vfs_read(
            file,
            0,
            physical_to_virtual(
                (uint64_t)user_code_page
            ),
            file->size
        ) != file->size
    )
    {
        free_page(kernel_stack_page);
        free_page(user_code_page);
        free_page(user_stack_page);
        return 0;
    }

    if (
        !paging_map_user_page(
            code_virtual,
            (uint64_t)user_code_page,
            true
        )
    )
    {
        free_page(kernel_stack_page);
        free_page(user_code_page);
        free_page(user_stack_page);
        return 0;
    }

    if (
        !paging_map_user_page(
            stack_virtual,
            (uint64_t)user_stack_page,
            true
        )
    )
    {
        paging_unmap_page(code_virtual);
        free_page(kernel_stack_page);
        free_page(user_code_page);
        free_page(user_stack_page);
        return 0;
    }

    process_t *process =
        &processes[slot];

    clear_process(process);

    process->pid = next_pid++;
    copy_name(
        process->name,
        path_name(path)
    );

    process->state = PROCESS_READY;
    process->mode = PROCESS_USER;
    process->kernel_stack_page =
        kernel_stack_page;

    process->kernel_stack_top =
        (uint64_t)physical_to_virtual(
            (uint64_t)kernel_stack_page
        ) + PAGE_SIZE;

    process->user_code_page =
        user_code_page;

    process->user_stack_page =
        user_stack_page;

    process->user_code_virtual =
        code_virtual;

    process->user_stack_virtual =
        stack_virtual;

    process->user_code_size =
        file->size;

    process->context =
        create_user_context(
            kernel_stack_page,
            code_virtual,
            stack_virtual
        );

    return process->pid;
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

    irq_disable();

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

        if (slot == current_index)
        {
            return false;
        }

        process->state =
            PROCESS_TERMINATED;

        destroy_process_slot(slot);
        return true;
    }

    return false;
}

cpu_context_t *process_schedule_on_timer(
    cpu_context_t *context
)
{
    return schedule(
        context,
        true
    );
}

cpu_context_t *process_schedule_now(
    cpu_context_t *context
)
{
    return schedule(
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
        current_index == 0
    )
    {
        return context;
    }

    process_t *current =
        &processes[current_index];

    current->context = context;
    current->exit_status = status;
    current->state = PROCESS_TERMINATED;

    return schedule(
        context,
        false
    );
}

void process_exit_current(void)
{
    irq_disable();

    if (current_index != 0)
    {
        processes[current_index].state =
            PROCESS_TERMINATED;
    }

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

bool process_user_range_valid(
    uint64_t address,
    size_t size
)
{
    const process_t *process =
        process_current();

    if (
        process == NULL ||
        process->mode != PROCESS_USER ||
        size == 0 ||
        address > UINT64_MAX - size
    )
    {
        return false;
    }

    uint64_t end = address + size;

    uint64_t code_start =
        process->user_code_virtual;

    uint64_t code_end =
        code_start + PAGE_SIZE;

    uint64_t stack_start =
        process->user_stack_virtual;

    uint64_t stack_end =
        stack_start + PAGE_SIZE;

    bool in_code =
        address >= code_start &&
        end <= code_end;

    bool in_stack =
        address >= stack_start &&
        end <= stack_end;

    return in_code || in_stack;
}

uint32_t process_count(void)
{
    uint32_t count = 0;

    for (
        uint32_t index = 0;
        index < PROCESS_MAX_COUNT;
        index++
    )
    {
        if (
            processes[index].state !=
            PROCESS_UNUSED
        )
        {
            count++;
        }
    }

    return count;
}

const process_t *process_get(
    uint32_t index
)
{
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

    return &processes[current_index];
}

void process_print_all(void)
{
    terminal_write_line(
        "PID  MODE    STATE       TICKS  SWITCHES  NAME"
    );

    for (
        uint32_t index = 0;
        index < process_count();
        index++
    )
    {
        const process_t *process =
            process_get(index);

        if (process == NULL)
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

        terminal_write_line(
            process->name
        );
    }
}
