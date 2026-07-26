#include "gdt.h"

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
 * Entries:
 * 0: null
 * 1: kernel code
 * 2: kernel data
 * 3: user data
 * 4: user code
 * 5-6: 64-bit TSS descriptor
 */
static uint64_t gdt_entries[7]
    __attribute__((aligned(16)));

static gdt_descriptor_t gdt_descriptor;
static tss_t tss;

static void clear_bytes(
    void *pointer,
    size_t count
)
{
    uint8_t *bytes = pointer;

    for (
        size_t index = 0;
        index < count;
        index++
    )
    {
        bytes[index] = 0;
    }
}

static void install_tss_descriptor(void)
{
    uint64_t base = (uint64_t)&tss;
    uint32_t limit =
        (uint32_t)(sizeof(tss) - 1);

    uint64_t low = 0;

    low |= (uint64_t)(limit & 0xFFFF);
    low |= (base & 0xFFFFFFULL) << 16;
    low |= 0x89ULL << 40;
    low |=
        (uint64_t)((limit >> 16) & 0x0F)
        << 48;
    low |=
        ((base >> 24) & 0xFFULL)
        << 56;

    gdt_entries[5] = low;
    gdt_entries[6] = base >> 32;
}

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
        "mov %[tss_selector], %%ax\n"
        "ltr %%ax\n"
        :
        : "m"(gdt_descriptor),
          [code_selector] "i"(GDT_KERNEL_CODE_SELECTOR),
          [data_selector] "i"(GDT_KERNEL_DATA_SELECTOR),
          [tss_selector] "i"(GDT_TSS_SELECTOR)
        : "rax", "memory"
    );
}

void gdt_init(void)
{
    clear_bytes(
        gdt_entries,
        sizeof(gdt_entries)
    );

    clear_bytes(
        &tss,
        sizeof(tss)
    );

    gdt_entries[0] =
        0x0000000000000000ULL;

    gdt_entries[1] =
        0x00AF9A000000FFFFULL;

    gdt_entries[2] =
        0x00CF92000000FFFFULL;

    gdt_entries[3] =
        0x00CFF2000000FFFFULL;

    gdt_entries[4] =
        0x00AFFA000000FFFFULL;

    tss.iomap_base =
        (uint16_t)sizeof(tss);

    install_tss_descriptor();

    gdt_descriptor.limit =
        sizeof(gdt_entries) - 1;

    gdt_descriptor.base =
        (uint64_t)&gdt_entries[0];

    gdt_load();
}

void gdt_set_kernel_stack(
    uint64_t stack_top
)
{
    tss.rsp0 = stack_top;
}
