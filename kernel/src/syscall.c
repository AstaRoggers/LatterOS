#include "syscall.h"

#include "process.h"
#include "terminal.h"
#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SYSCALL_TRANSFER_LIMIT 1024
#define SYSCALL_WRITE_CHUNK    120
#define USER_FD_BASE           3
#define SYSCALL_ERROR          UINT64_MAX

static process_t *current_user_process(void)
{
    const process_t *current =
        process_current();

    if (
        current == NULL ||
        current->mode != PROCESS_USER
    )
    {
        return NULL;
    }

    return (process_t *)(uintptr_t)current;
}

static bool copy_user_string(
    uint64_t user_address,
    char *buffer,
    size_t capacity
)
{
    if (
        user_address == 0 ||
        buffer == NULL ||
        capacity < 2
    )
    {
        return false;
    }

    for (
        size_t index = 0;
        index < capacity - 1;
        index++
    )
    {
        uint64_t address =
            user_address + index;

        if (
            address < user_address ||
            !process_user_range_valid(
                address,
                1
            )
        )
        {
            return false;
        }

        char character =
            *(const char *)address;

        buffer[index] = character;

        if (character == '\0')
        {
            return index > 0;
        }
    }

    buffer[capacity - 1] = '\0';
    return false;
}

static uint64_t syscall_console_write(
    uint64_t user_address,
    uint64_t length
)
{
    if (length == 0)
    {
        return 0;
    }

    if (
        length > SYSCALL_TRANSFER_LIMIT ||
        !process_user_range_valid(
            user_address,
            (size_t)length
        )
    )
    {
        return SYSCALL_ERROR;
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

static process_file_t *file_from_fd(
    process_t *process,
    uint64_t descriptor
)
{
    if (
        process == NULL ||
        descriptor < USER_FD_BASE
    )
    {
        return NULL;
    }

    uint64_t index =
        descriptor - USER_FD_BASE;

    if (
        index >= PROCESS_MAX_FILES ||
        !process->files[index].used
    )
    {
        return NULL;
    }

    return &process->files[index];
}

static uint64_t syscall_file_open(
    uint64_t user_path,
    uint64_t raw_flags
)
{
    process_t *process =
        current_user_process();

    uint32_t flags =
        (uint32_t)raw_flags;

    if (
        process == NULL ||
        (flags & (USER_OPEN_READ | USER_OPEN_WRITE)) == 0 ||
        (flags & ~(USER_OPEN_READ |
                   USER_OPEN_WRITE |
                   USER_OPEN_CREATE |
                   USER_OPEN_TRUNCATE)) != 0
    )
    {
        return SYSCALL_ERROR;
    }

    char path[VFS_PATH_MAX];

    if (
        !copy_user_string(
            user_path,
            path,
            sizeof(path)
        )
    )
    {
        return SYSCALL_ERROR;
    }

    vfs_node_t *node =
        vfs_open(path);

    if (
        node == NULL &&
        (flags & USER_OPEN_CREATE) != 0
    )
    {
        if (!vfs_create_file(path))
        {
            return SYSCALL_ERROR;
        }

        node = vfs_open(path);
    }

    if (
        node == NULL ||
        node->type != VFS_NODE_FILE
    )
    {
        return SYSCALL_ERROR;
    }

    if (
        (flags & USER_OPEN_TRUNCATE) != 0
    )
    {
        if (
            (flags & USER_OPEN_WRITE) == 0 ||
            node->operations == NULL ||
            node->operations->truncate == NULL ||
            !node->operations->truncate(node)
        )
        {
            return SYSCALL_ERROR;
        }
    }

    for (
        uint64_t index = 0;
        index < PROCESS_MAX_FILES;
        index++
    )
    {
        process_file_t *file =
            &process->files[index];

        if (file->used)
        {
            continue;
        }

        file->used = true;
        file->node = node;
        file->offset = 0;
        file->flags = flags;

        return USER_FD_BASE + index;
    }

    return SYSCALL_ERROR;
}

static uint64_t syscall_file_read(
    uint64_t descriptor,
    uint64_t user_buffer,
    uint64_t count
)
{
    if (count == 0)
    {
        return 0;
    }

    if (
        count > SYSCALL_TRANSFER_LIMIT ||
        !process_user_range_valid(
            user_buffer,
            (size_t)count
        )
    )
    {
        return SYSCALL_ERROR;
    }

    process_t *process =
        current_user_process();

    process_file_t *file =
        file_from_fd(
            process,
            descriptor
        );

    if (
        file == NULL ||
        (file->flags & USER_OPEN_READ) == 0
    )
    {
        return SYSCALL_ERROR;
    }

    size_t transferred =
        vfs_read(
            file->node,
            file->offset,
            (void *)user_buffer,
            (size_t)count
        );

    file->offset += transferred;
    return transferred;
}

static uint64_t syscall_file_write(
    uint64_t descriptor,
    uint64_t user_buffer,
    uint64_t count
)
{
    if (count == 0)
    {
        return 0;
    }

    if (
        count > SYSCALL_TRANSFER_LIMIT ||
        !process_user_range_valid(
            user_buffer,
            (size_t)count
        )
    )
    {
        return SYSCALL_ERROR;
    }

    process_t *process =
        current_user_process();

    process_file_t *file =
        file_from_fd(
            process,
            descriptor
        );

    if (
        file == NULL ||
        (file->flags & USER_OPEN_WRITE) == 0
    )
    {
        return SYSCALL_ERROR;
    }

    size_t transferred =
        vfs_write(
            file->node,
            file->offset,
            (const void *)user_buffer,
            (size_t)count
        );

    file->offset += transferred;
    return transferred;
}

static uint64_t syscall_file_close(
    uint64_t descriptor
)
{
    process_t *process =
        current_user_process();

    process_file_t *file =
        file_from_fd(
            process,
            descriptor
        );

    if (file == NULL)
    {
        return SYSCALL_ERROR;
    }

    file->used = false;
    file->node = NULL;
    file->offset = 0;
    file->flags = 0;

    return 0;
}

static uint64_t syscall_process_spawn(
    uint64_t user_path
)
{
    char path[VFS_PATH_MAX];

    if (
        !copy_user_string(
            user_path,
            path,
            sizeof(path)
        )
    )
    {
        return SYSCALL_ERROR;
    }

    uint64_t pid =
        process_create_user_program(path);

    return pid == 0 ?
        SYSCALL_ERROR :
        pid;
}

static uint64_t syscall_process_kill(
    uint64_t pid
)
{
    return process_terminate(pid) ?
        0 :
        SYSCALL_ERROR;
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
        case SYSCALL_CONSOLE_WRITE:
            context->rax =
                syscall_console_write(
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
                SYSCALL_ERROR;

            return context;
        }

        case SYSCALL_FILE_OPEN:
            context->rax =
                syscall_file_open(
                    context->rdi,
                    context->rsi
                );

            return context;

        case SYSCALL_FILE_READ:
            context->rax =
                syscall_file_read(
                    context->rdi,
                    context->rsi,
                    context->rdx
                );

            return context;

        case SYSCALL_FILE_WRITE:
            context->rax =
                syscall_file_write(
                    context->rdi,
                    context->rsi,
                    context->rdx
                );

            return context;

        case SYSCALL_FILE_CLOSE:
            context->rax =
                syscall_file_close(
                    context->rdi
                );

            return context;

        case SYSCALL_PROCESS_SPAWN:
            context->rax =
                syscall_process_spawn(
                    context->rdi
                );

            return context;

        case SYSCALL_PROCESS_KILL:
            context->rax =
                syscall_process_kill(
                    context->rdi
                );

            return context;

        case SYSCALL_PROCESS_YIELD:
            context->rax = 0;

            return process_schedule_now(
                context
            );

        default:
            context->rax = SYSCALL_ERROR;
            return context;
    }
}
