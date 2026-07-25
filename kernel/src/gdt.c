#include "gdt.h"

#include <stdint.h>

#define GDT_KERNEL_CODE_SELECTOR 0x08
#define GDT_KERNEL_DATA_SELECTOR 0x10

typedef struct
{
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdt_descriptor_t;

static uint64_t gdt_entries[3];

static gdt_descriptor_t gdt_descriptor;

static void gdt_load(void)
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
        :
        : [descriptor] "m"(gdt_descriptor),
          [code_selector] "i"(GDT_KERNEL_CODE_SELECTOR),
          [data_selector] "i"(GDT_KERNEL_DATA_SELECTOR)
        : "rax", "memory"
    );
}

void gdt_init(void)
{
    gdt_entries[0] = 0x0000000000000000ULL;

    gdt_entries[1] = 0x00AF9A000000FFFFULL;

    gdt_entries[2] = 0x00AF92000000FFFFULL;

    gdt_descriptor.limit =
        sizeof(gdt_entries) - 1;

    gdt_descriptor.base =
        (uint64_t)&gdt_entries[0];

    gdt_load();
}