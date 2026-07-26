#ifndef IOAPIC_H
#define IOAPIC_H

#include <stdbool.h>
#include <stdint.h>

bool ioapic_init(uint32_t destination_apic_id);
bool ioapic_is_ready(void);

uint32_t ioapic_controller_count(void);
uint32_t ioapic_redirection_count(void);

bool ioapic_route_isa_irq(
    uint8_t irq,
    uint8_t vector,
    bool masked
);

bool ioapic_mask_isa_irq(uint8_t irq);

#endif
