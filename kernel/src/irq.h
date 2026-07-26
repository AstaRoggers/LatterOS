#ifndef IRQ_H
#define IRQ_H

#include "cpu_context.h"

#include <stdbool.h>
#include <stdint.h>

typedef void (*irq_handler_t)(void);

void irq_init(void);

void irq_set_lapic_enabled(
    bool enabled
);

void irq_set_ioapic_enabled(
    bool enabled
);

bool irq_ioapic_enabled(void);
const char *irq_controller_name(void);

void irq_register_handler(
    uint8_t irq,
    irq_handler_t handler
);

void irq_unregister_handler(
    uint8_t irq
);

void irq_enable(void);
void irq_disable(void);

cpu_context_t *interrupt_dispatch(
    uint64_t vector,
    cpu_context_t *context
);

#endif
