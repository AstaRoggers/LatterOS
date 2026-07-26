#ifndef SYSCALL_H
#define SYSCALL_H

#include "cpu_context.h"

#include <stdint.h>

#define SYSCALL_CONSOLE_WRITE 1ULL
#define SYSCALL_EXIT          2ULL
#define SYSCALL_GETPID        3ULL
#define SYSCALL_FILE_OPEN     4ULL
#define SYSCALL_FILE_READ     5ULL
#define SYSCALL_FILE_WRITE    6ULL
#define SYSCALL_FILE_CLOSE    7ULL
#define SYSCALL_PROCESS_SPAWN 8ULL
#define SYSCALL_PROCESS_KILL  9ULL
#define SYSCALL_PROCESS_YIELD 10ULL

#define USER_OPEN_READ     0x01U
#define USER_OPEN_WRITE    0x02U
#define USER_OPEN_CREATE   0x04U
#define USER_OPEN_TRUNCATE 0x08U

cpu_context_t *syscall_dispatch(
    cpu_context_t *context
);

#endif
