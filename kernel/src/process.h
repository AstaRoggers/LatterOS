#ifndef PROCESS_H
#define PROCESS_H

#include "cpu_context.h"

#include <stdbool.h>
#include <stdint.h>

#define PROCESS_NAME_LENGTH 32
#define PROCESS_MAX_COUNT   16

typedef enum
{
    PROCESS_UNUSED,
    PROCESS_READY,
    PROCESS_RUNNING,
    PROCESS_TERMINATED
} process_state_t;

typedef void (*kernel_thread_entry_t)(void *argument);

typedef struct
{
    uint64_t pid;
    char name[PROCESS_NAME_LENGTH];
    process_state_t state;

    cpu_context_t *context;
    void *stack_page;

    uint64_t cpu_ticks;
    uint64_t switches;
} process_t;

void process_init(void);

bool process_create_kernel_thread(
    const char *name,
    kernel_thread_entry_t entry,
    void *argument
);

bool process_spawn_demo_thread(void);

cpu_context_t *process_schedule_on_timer(
    cpu_context_t *context
);

cpu_context_t *process_schedule_now(
    cpu_context_t *context
);

void process_exit_current(void);

uint32_t process_count(void);
const process_t *process_get(uint32_t index);
const process_t *process_current(void);

void process_print_all(void);

#endif
