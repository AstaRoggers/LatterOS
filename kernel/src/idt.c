#include "idt.h"

#include <stdint.h>

#define KERNEL_CODE_SELECTOR 0x08
#define IDT_KERNEL_GATE      0x8E
#define IDT_USER_GATE        0xEE
#define SYSCALL_VECTOR       0x80

static idt_entry_t idt[IDT_ENTRIES]
    __attribute__((aligned(16)));

static idtr_t idtr;

extern void (*isr_stub_table[IDT_ENTRIES])(void);

void idt_set_gate(
    uint8_t vector,
    void (*handler)(void),
    uint8_t type_attributes
)
{
    uint64_t address = (uint64_t)handler;

    idt[vector].offset_low =
        (uint16_t)(address & 0xFFFF);

    idt[vector].selector =
        KERNEL_CODE_SELECTOR;

    idt[vector].ist = 0;

    idt[vector].type_attributes =
        type_attributes;

    idt[vector].offset_middle =
        (uint16_t)((address >> 16) & 0xFFFF);

    idt[vector].offset_high =
        (uint32_t)((address >> 32) & 0xFFFFFFFF);

    idt[vector].reserved = 0;
}

void idt_init(void)
{
    for (
        uint16_t vector = 0;
        vector < IDT_ENTRIES;
        vector++
    )
    {
        idt_set_gate(
            (uint8_t)vector,
            isr_stub_table[vector],
            IDT_KERNEL_GATE
        );
    }

    /* Allow ring-3 programs to enter the kernel through int 0x80. */
    idt_set_gate(
        SYSCALL_VECTOR,
        isr_stub_table[SYSCALL_VECTOR],
        IDT_USER_GATE
    );

    idtr.limit =
        (uint16_t)(sizeof(idt) - 1);

    idtr.base =
        (uint64_t)&idt[0];

    idt_load();
}

void idt_load(void)
{
    __asm__ volatile(
        "lidt %0"
        :
        : "m"(idtr)
        : "memory"
    );
}
