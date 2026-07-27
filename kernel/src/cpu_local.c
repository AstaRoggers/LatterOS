#include "cpu_local.h"

#include <stddef.h>
#include <stdint.h>

#define IA32_GS_BASE        0xC0000101U
#define IA32_KERNEL_GS_BASE 0xC0000102U

static cpu_local_t cpu_locals[CPU_LOCAL_MAX_CPUS]
    __attribute__((aligned(64)));

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

static void write_msr(
    uint32_t msr,
    uint64_t value
)
{
    uint32_t low =
        (uint32_t)(value & 0xFFFFFFFFULL);

    uint32_t high =
        (uint32_t)(value >> 32);

    __asm__ volatile(
        "wrmsr"
        :
        : "c"(msr),
          "a"(low),
          "d"(high)
        : "memory"
    );
}

void cpu_local_bootstrap(void)
{
    clear_bytes(
        cpu_locals,
        sizeof(cpu_locals)
    );

    for (
        uint32_t index = 0;
        index < CPU_LOCAL_MAX_CPUS;
        index++
    )
    {
        cpu_locals[index].self =
            &cpu_locals[index];

        cpu_locals[index].index = index;
        cpu_locals[index].current_process_slot =
            CPU_LOCAL_NO_PROCESS;
    }

    (void)cpu_local_configure(
        0,
        0,
        0,
        true
    );
}

bool cpu_local_configure(
    uint32_t index,
    uint32_t processor_id,
    uint32_t apic_id,
    bool bsp
)
{
    if (index >= CPU_LOCAL_MAX_CPUS)
    {
        return false;
    }

    cpu_local_t *local =
        &cpu_locals[index];

    local->self = local;
    local->index = index;
    local->processor_id = processor_id;
    local->apic_id = apic_id;
    local->bsp = bsp;
    local->configured = true;

    if (bsp)
    {
        local->current_process_slot = 0;
        local->current_pid = 0;
    }
    else if (
        local->current_process_slot == 0 &&
        local->current_pid == 0
    )
    {
        local->current_process_slot =
            CPU_LOCAL_NO_PROCESS;
    }

    return true;
}

bool cpu_local_bind(uint32_t index)
{
    if (index >= CPU_LOCAL_MAX_CPUS)
    {
        return false;
    }

    cpu_local_t *local =
        &cpu_locals[index];

    if (!local->configured)
    {
        return false;
    }

    uint64_t address =
        (uint64_t)local;

    write_msr(
        IA32_GS_BASE,
        address
    );

    write_msr(
        IA32_KERNEL_GS_BASE,
        address
    );

    __atomic_thread_fence(
        __ATOMIC_SEQ_CST
    );

    local->bound = true;

    return true;
}

cpu_local_t *cpu_local_current(void)
{
    cpu_local_t *local;

    __asm__ volatile(
        "movq %%gs:0, %0"
        : "=r"(local)
        :
        : "memory"
    );

    if (
        local == NULL ||
        local->self != local ||
        local->index >= CPU_LOCAL_MAX_CPUS
    )
    {
        return NULL;
    }

    return local;
}

cpu_local_t *cpu_local_by_index(uint32_t index)
{
    if (index >= CPU_LOCAL_MAX_CPUS)
    {
        return NULL;
    }

    return &cpu_locals[index];
}

const cpu_local_t *cpu_local_get(uint32_t index)
{
    return cpu_local_by_index(index);
}

void cpu_local_set_descriptor_state(
    uint32_t index,
    uint64_t tss_address,
    uint64_t kernel_stack_top,
    uint64_t interrupt_stack_top
)
{
    cpu_local_t *local =
        cpu_local_by_index(index);

    if (local == NULL)
    {
        return;
    }

    local->tss_address = tss_address;
    local->kernel_stack_top =
        kernel_stack_top;

    local->interrupt_stack_top =
        interrupt_stack_top;

    local->descriptor_ready = true;
}

void cpu_local_set_current_process(
    uint32_t index,
    uint32_t process_slot,
    uint64_t pid
)
{
    cpu_local_t *local =
        cpu_local_by_index(index);

    if (local == NULL)
    {
        return;
    }

    local->current_process_slot =
        process_slot;

    local->current_pid = pid;
}

void cpu_local_charge_scheduler_tick(uint32_t index)
{
    cpu_local_t *local =
        cpu_local_by_index(index);

    if (local == NULL)
    {
        return;
    }

    __atomic_add_fetch(
        &local->scheduler_ticks,
        1,
        __ATOMIC_RELAXED
    );
}

void cpu_local_note_context_switch(uint32_t index)
{
    cpu_local_t *local =
        cpu_local_by_index(index);

    if (local == NULL)
    {
        return;
    }

    __atomic_add_fetch(
        &local->context_switches,
        1,
        __ATOMIC_RELAXED
    );
}
