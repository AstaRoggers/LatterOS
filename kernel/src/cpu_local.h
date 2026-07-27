#ifndef CPU_LOCAL_H
#define CPU_LOCAL_H

#include <stdbool.h>
#include <stdint.h>

#define CPU_LOCAL_MAX_CPUS 64
#define CPU_LOCAL_NO_PROCESS UINT32_MAX

typedef struct cpu_local
{
    struct cpu_local *self;

    uint32_t index;
    uint32_t processor_id;
    uint32_t apic_id;

    bool bsp;
    bool configured;
    bool bound;
    bool descriptor_ready;

    uint64_t tss_address;
    uint64_t kernel_stack_top;
    uint64_t interrupt_stack_top;

    uint32_t current_process_slot;
    uint32_t reserved0;
    uint64_t current_pid;
    uint64_t current_kernel_thread_id;

    uint64_t scheduler_ticks;
    uint64_t context_switches;
} cpu_local_t;

void cpu_local_bootstrap(void);

bool cpu_local_configure(
    uint32_t index,
    uint32_t processor_id,
    uint32_t apic_id,
    bool bsp
);

bool cpu_local_bind(uint32_t index);

cpu_local_t *cpu_local_current(void);
cpu_local_t *cpu_local_by_index(uint32_t index);
const cpu_local_t *cpu_local_get(uint32_t index);

void cpu_local_set_descriptor_state(
    uint32_t index,
    uint64_t tss_address,
    uint64_t kernel_stack_top,
    uint64_t interrupt_stack_top
);

void cpu_local_set_current_process(
    uint32_t index,
    uint32_t process_slot,
    uint64_t pid
);

void cpu_local_set_current_kernel_thread(
    uint32_t index,
    uint64_t thread_id
);

void cpu_local_charge_scheduler_tick(uint32_t index);
void cpu_local_note_context_switch(uint32_t index);

#endif
