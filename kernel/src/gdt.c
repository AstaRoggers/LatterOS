#include "gdt.h"

#include "cpu_local.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdt_descriptor_t;

typedef struct
{
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed)) tss_t;

/*
 * Entries per CPU:
 * 0: null
 * 1: kernel code
 * 2: kernel data
 * 3: user data
 * 4: user code
 * 5-6: 64-bit TSS descriptor
 */
static uint64_t gdt_entries[CPU_LOCAL_MAX_CPUS][7]
    __attribute__((aligned(16)));

static gdt_descriptor_t gdt_descriptors[
    CPU_LOCAL_MAX_CPUS
];

static tss_t tss_entries[CPU_LOCAL_MAX_CPUS]
    __attribute__((aligned(16)));

static uint8_t kernel_transition_stacks[
    CPU_LOCAL_MAX_CPUS
][GDT_CPU_KERNEL_STACK_SIZE]
    __attribute__((aligned(16)));

static uint8_t interrupt_emergency_stacks[
    CPU_LOCAL_MAX_CPUS
][GDT_CPU_INTERRUPT_STACK_SIZE]
    __attribute__((aligned(16)));

static bool prepared[CPU_LOCAL_MAX_CPUS];

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

static void install_tss_descriptor(
    uint32_t cpu_index
)
{
    uint64_t base =
        (uint64_t)&tss_entries[cpu_index];

    uint32_t limit =
        (uint32_t)(sizeof(tss_t) - 1);

    uint64_t low = 0;

    low |= (uint64_t)(limit & 0xFFFFU);
    low |= (base & 0xFFFFFFULL) << 16;
    low |= 0x89ULL << 40;

    low |=
        (uint64_t)((limit >> 16) & 0x0FU)
        << 48;

    low |=
        ((base >> 24) & 0xFFULL)
        << 56;

    gdt_entries[cpu_index][5] = low;
    gdt_entries[cpu_index][6] = base >> 32;
}

static bool prepare_cpu(uint32_t cpu_index)
{
    if (cpu_index >= CPU_LOCAL_MAX_CPUS)
    {
        return false;
    }

    if (prepared[cpu_index])
    {
        return true;
    }

    clear_bytes(
        gdt_entries[cpu_index],
        sizeof(gdt_entries[cpu_index])
    );

    clear_bytes(
        &tss_entries[cpu_index],
        sizeof(tss_entries[cpu_index])
    );

    gdt_entries[cpu_index][0] =
        0x0000000000000000ULL;

    gdt_entries[cpu_index][1] =
        0x00AF9A000000FFFFULL;

    gdt_entries[cpu_index][2] =
        0x00CF92000000FFFFULL;

    gdt_entries[cpu_index][3] =
        0x00CFF2000000FFFFULL;

    gdt_entries[cpu_index][4] =
        0x00AFFA000000FFFFULL;

    uint64_t kernel_stack_top =
        (uint64_t)&kernel_transition_stacks[
            cpu_index
        ][GDT_CPU_KERNEL_STACK_SIZE];

    uint64_t interrupt_stack_top =
        (uint64_t)&interrupt_emergency_stacks[
            cpu_index
        ][GDT_CPU_INTERRUPT_STACK_SIZE];

    tss_entries[cpu_index].rsp0 =
        kernel_stack_top;

    /*
     * IST1 is reserved now so later exception-gate work can enable
     * a per-CPU emergency stack without changing the TSS layout.
     */
    tss_entries[cpu_index].ist1 =
        interrupt_stack_top;

    tss_entries[cpu_index].iomap_base =
        (uint16_t)sizeof(tss_t);

    install_tss_descriptor(cpu_index);

    gdt_descriptors[cpu_index].limit =
        (uint16_t)(
            sizeof(gdt_entries[cpu_index]) - 1
        );

    gdt_descriptors[cpu_index].base =
        (uint64_t)&gdt_entries[cpu_index][0];

    prepared[cpu_index] = true;

    cpu_local_set_descriptor_state(
        cpu_index,
        (uint64_t)&tss_entries[cpu_index],
        kernel_stack_top,
        interrupt_stack_top
    );

    return true;
}

static void load_cpu(uint32_t cpu_index)
{
    __asm__ volatile(
        "lgdt %0\n"

        "mov %[data_selector], %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%ss\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"

        "pushq %[code_selector]\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"

        "mov %[tss_selector], %%ax\n"
        "ltr %%ax\n"
        :
        : "m"(gdt_descriptors[cpu_index]),
          [code_selector] "i"(GDT_KERNEL_CODE_SELECTOR),
          [data_selector] "i"(GDT_KERNEL_DATA_SELECTOR),
          [tss_selector] "i"(GDT_TSS_SELECTOR)
        : "rax", "memory"
    );
}

void gdt_init(void)
{
    cpu_local_bootstrap();

    if (!prepare_cpu(0))
    {
        return;
    }

    load_cpu(0);
    (void)cpu_local_bind(0);
}

bool gdt_load_secondary(uint32_t cpu_index)
{
    if (!prepare_cpu(cpu_index))
    {
        return false;
    }

    load_cpu(cpu_index);

    return cpu_local_bind(cpu_index);
}

void gdt_set_kernel_stack(
    uint64_t stack_top
)
{
    if (stack_top == 0)
    {
        return;
    }

    cpu_local_t *local =
        cpu_local_current();

    if (
        local == NULL ||
        local->index >= CPU_LOCAL_MAX_CPUS ||
        !prepared[local->index]
    )
    {
        return;
    }

    tss_entries[local->index].rsp0 =
        stack_top;

    local->kernel_stack_top =
        stack_top;
}

bool gdt_cpu_ready(uint32_t cpu_index)
{
    if (cpu_index >= CPU_LOCAL_MAX_CPUS)
    {
        return false;
    }

    return prepared[cpu_index];
}

uint64_t gdt_tss_address(uint32_t cpu_index)
{
    if (!gdt_cpu_ready(cpu_index))
    {
        return 0;
    }

    return (uint64_t)&tss_entries[cpu_index];
}

uint64_t gdt_kernel_stack_top(uint32_t cpu_index)
{
    if (!gdt_cpu_ready(cpu_index))
    {
        return 0;
    }

    return tss_entries[cpu_index].rsp0;
}

uint64_t gdt_interrupt_stack_top(uint32_t cpu_index)
{
    if (!gdt_cpu_ready(cpu_index))
    {
        return 0;
    }

    return tss_entries[cpu_index].ist1;
}
