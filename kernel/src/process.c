#include "process.h"

#include "hhdm.h"
#include "irq.h"
#include "page_allocator.h"
#include "physical_memory.h"
#include "terminal.h"

#include <stddef.h>
#include <stdint.h>

#define KERNEL_CODE_SELECTOR 0x08ULL
#define INITIAL_RFLAGS       0x202ULL
#define SCHEDULER_VECTOR     0x80

extern void process_task_bootstrap(void);

static process_t processes[PROCESS_MAX_COUNT];
static uint32_t current_index;
static uint64_t next_pid;
static uint32_t demo_thread_number;
static bool initialized;

static void clear_process(process_t *process)
{
    process->pid = 0;

    for (
        uint32_t index = 0;
        index < PROCESS_NAME_LENGTH;
        index++
    )
    {
        process->name[index] = '\0';
    }

    process->state = PROCESS_UNUSED;
    process->context = NULL;
    process->stack_page = NULL;
    process->cpu_ticks = 0;
    process->switches = 0;
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

static int32_t find_free_slot(void)
{
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

static cpu_context_t *create_initial_context(
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

    uint64_t *words =
        (uint64_t *)context;

    for (
        uint32_t index = 0;
        index <
            sizeof(cpu_context_t) /
            sizeof(uint64_t);
        index++
    )
    {
        words[index] = 0;
    }

    context->r12 =
        (uint64_t)entry;

    context->r13 =
        (uint64_t)argument;

    context->vector = 0;
    context->error_code = 0;

    context->rip =
        (uint64_t)process_task_bootstrap;

    context->cs =
        KERNEL_CODE_SELECTOR;

    context->rflags =
        INITIAL_RFLAGS;

    return context;
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
        current->state =
            PROCESS_RUNNING;

        return context;
    }

    next->state = PROCESS_RUNNING;
    next->switches++;
    current_index = next_index;

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

static volatile uint64_t demo_counters[
    PROCESS_MAX_COUNT
];

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

    kernel->state =
        PROCESS_RUNNING;

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

    void *stack_page =
        alloc_page();

    if (stack_page == NULL)
    {
        return false;
    }

    process_t *process =
        &processes[slot];

    clear_process(process);

    process->pid = next_pid++;
    copy_name(process->name, name);
    process->state = PROCESS_READY;
    process->stack_page = stack_page;

    process->context =
        create_initial_context(
            stack_page,
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

void process_exit_current(void)
{
    irq_disable();

    processes[current_index].state =
        PROCESS_TERMINATED;

    __asm__ volatile(
        "int $0x80"
        :
        :
        : "memory"
    );

    for (;;)
    {
        __asm__ volatile("hlt");
    }
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
        "PID  STATE       TICKS  SWITCHES  NAME"
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
