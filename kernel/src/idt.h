#ifndef IDT_H
#define IDT_H

#include <stdint.h>

#define IDT_ENTRIES 256

typedef struct __attribute__((packed))
{
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attributes;
    uint16_t offset_middle;
    uint32_t offset_high;
    uint32_t reserved;
} idt_entry_t;

typedef struct __attribute__((packed))
{
    uint16_t limit;
    uint64_t base;
} idtr_t;

void idt_set_gate(
    uint8_t vector,
    void (*handler)(void),
    uint8_t type_attributes
);

void idt_init(void);

#endif
