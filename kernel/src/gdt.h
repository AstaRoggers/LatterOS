#ifndef GDT_H
#define GDT_H

#include <stdbool.h>
#include <stdint.h>

#define GDT_KERNEL_CODE_SELECTOR 0x08
#define GDT_KERNEL_DATA_SELECTOR 0x10
#define GDT_USER_DATA_SELECTOR   0x1B
#define GDT_USER_CODE_SELECTOR   0x23
#define GDT_TSS_SELECTOR         0x28

#define GDT_CPU_KERNEL_STACK_SIZE    16384U
#define GDT_CPU_INTERRUPT_STACK_SIZE 16384U

void gdt_init(void);

bool gdt_load_secondary(
    uint32_t cpu_index
);

void gdt_set_kernel_stack(
    uint64_t stack_top
);

bool gdt_cpu_ready(uint32_t cpu_index);
uint64_t gdt_tss_address(uint32_t cpu_index);
uint64_t gdt_kernel_stack_top(uint32_t cpu_index);
uint64_t gdt_interrupt_stack_top(uint32_t cpu_index);

#endif
