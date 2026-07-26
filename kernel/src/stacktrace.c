#include "stacktrace.h"

#include "kstdio.h"
#include "physical_memory.h"
#include "process.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STACK_ALIGNMENT 8ULL
#define MAX_FRAME_DISTANCE (PAGE_SIZE * 2ULL)

static bool canonical_address(uint64_t address)
{
    uint64_t upper = address >> 48;
    uint64_t sign = (address >> 47) & 1ULL;

    return sign ?
        upper == 0xFFFFULL :
        upper == 0;
}

static void stack_bounds(
    uint64_t stack_pointer,
    uint64_t *lower,
    uint64_t *upper
)
{
    const process_t *process =
        process_current();

    if (
        process != NULL &&
        process->kernel_stack_virtual != 0 &&
        stack_pointer >=
            process->kernel_stack_virtual &&
        stack_pointer <
            process->kernel_stack_top
    )
    {
        *lower =
            process->kernel_stack_virtual;

        *upper =
            process->kernel_stack_top;

        return;
    }

    uint64_t page =
        stack_pointer &
        ~(uint64_t)(PAGE_SIZE - 1);

    *lower = page;
    *upper = page + PAGE_SIZE;
}

size_t stacktrace_collect(
    uint64_t instruction_pointer,
    uint64_t frame_pointer,
    uint64_t stack_pointer,
    uint64_t *frames,
    size_t capacity
)
{
    if (
        frames == NULL ||
        capacity == 0
    )
    {
        return 0;
    }

    uint64_t lower;
    uint64_t upper;

    stack_bounds(
        stack_pointer,
        &lower,
        &upper
    );

    size_t count = 0;

    if (canonical_address(instruction_pointer))
    {
        frames[count++] = instruction_pointer;
    }

    uint64_t current = frame_pointer;

    while (count < capacity)
    {
        if (
            current < lower ||
            current > upper - 16 ||
            (current & (STACK_ALIGNMENT - 1)) != 0
        )
        {
            break;
        }

        const uint64_t *frame =
            (const uint64_t *)current;

        uint64_t previous = frame[0];
        uint64_t return_address = frame[1];

        if (!canonical_address(return_address))
        {
            break;
        }

        frames[count++] = return_address;

        if (
            previous <= current ||
            previous - current >
                MAX_FRAME_DISTANCE
        )
        {
            break;
        }

        current = previous;
    }

    return count;
}

size_t stacktrace_collect_context(
    const cpu_context_t *context,
    uint64_t *frames,
    size_t capacity
)
{
    if (context == NULL)
    {
        return 0;
    }

    uint64_t stack_pointer =
        (uint64_t)context +
        sizeof(cpu_context_t);

    if ((context->cs & 3ULL) == 3ULL)
    {
        const cpu_user_context_t *user =
            (const cpu_user_context_t *)context;

        stack_pointer = user->rsp;
    }

    return stacktrace_collect(
        context->rip,
        context->rbp,
        stack_pointer,
        frames,
        capacity
    );
}

__attribute__((noinline))
size_t stacktrace_collect_current(
    uint64_t *frames,
    size_t capacity
)
{
    uint64_t frame_pointer;
    uint64_t stack_pointer;

    __asm__ volatile(
        "movq %%rbp, %0\n"
        "movq %%rsp, %1"
        : "=r"(frame_pointer),
          "=r"(stack_pointer)
    );

    uint64_t instruction_pointer =
        (uint64_t)__builtin_return_address(0);

    return stacktrace_collect(
        instruction_pointer,
        frame_pointer,
        stack_pointer,
        frames,
        capacity
    );
}

void stacktrace_print_current(void)
{
    uint64_t frames[STACKTRACE_MAX_FRAMES];

    size_t count =
        stacktrace_collect_current(
            frames,
            STACKTRACE_MAX_FRAMES
        );

    kprintf(
        "Stack trace: %zu frame%s\n",
        count,
        count == 1 ? "" : "s"
    );

    for (
        size_t index = 0;
        index < count;
        index++
    )
    {
        kprintf(
            "  #%zu 0x%016llX\n",
            index,
            (unsigned long long)frames[index]
        );
    }
}
