#include "syscall.h"

#include "process.h"
#include "terminal.h"

#include <stddef.h>
#include <stdint.h>

#define SYSCALL_WRITE_LIMIT 1024
#define SYSCALL_WRITE_CHUNK 120

static uint64_t syscall_write(
    uint64_t user_address,
    uint64_t length
)
{
    if (
        length == 0 ||
        length > SYSCALL_WRITE_LIMIT ||
        !process_user_range_valid(
            user_address,
            (size_t)length
        )
    )
    {
        return 0;
    }

    const char *source =
        (const char *)user_address;

    uint64_t offset = 0;

    while (offset < length)
    {
        uint64_t remaining =
            length - offset;

        uint64_t count =
            remaining < SYSCALL_WRITE_CHUNK ?
            remaining :
            SYSCALL_WRITE_CHUNK;

        char buffer[
            SYSCALL_WRITE_CHUNK + 1
        ];

        for (
            uint64_t index = 0;
            index < count;
            index++
        )
        {
            buffer[index] =
                source[offset + index];
        }

        buffer[count] = '\0';
        terminal_write(buffer);
        offset += count;
    }

    return length;
}

cpu_context_t *syscall_dispatch(
    cpu_context_t *context
)
{
    if (context == NULL)
    {
        return NULL;
    }

    switch (context->rax)
    {
        case SYSCALL_WRITE:
            context->rax =
                syscall_write(
                    context->rdi,
                    context->rsi
                );

            return context;

        case SYSCALL_EXIT:
            return process_exit_from_syscall(
                context,
                (int64_t)context->rdi
            );

        case SYSCALL_GETPID:
        {
            const process_t *process =
                process_current();

            context->rax =
                process != NULL ?
                process->pid :
                0;

            return context;
        }

        default:
            context->rax = UINT64_MAX;
            return context;
    }
}
