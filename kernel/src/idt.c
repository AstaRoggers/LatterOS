#include "idt.h"

#include <stdint.h>

#define KERNEL_CODE_SELECTOR 0x08
#define IDT_INTERRUPT_GATE   0x8E

static idt_entry_t idt[IDT_ENTRIES]
    __attribute__((aligned(16)));

static idtr_t idtr;

extern void (*isr_stub_table[IDT_ENTRIES])(void);

void idt_set_gate(
    uint8_t vector,
    void (*handler)(void)
)
{
    uint64_t address = (uint64_t)handler;

    idt[vector].offset_low =
        (uint16_t)(address & 0xFFFF);

    idt[vector].selector =
        KERNEL_CODE_SELECTOR;

    idt[vector].ist = 0;

    idt[vector].type_attributes =
        IDT_INTERRUPT_GATE;

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
            isr_stub_table[vector]
        );
    }

    idtr.limit =
        (uint16_t)(sizeof(idt) - 1);

    idtr.base =
        (uint64_t)&idt[0];

    __asm__ volatile(
        "lidt %0"
        :
        : "m"(idtr)
        : "memory"
    );
}
