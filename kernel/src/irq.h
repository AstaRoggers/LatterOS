#ifndef IRQ_H
#define IRQ_H

#include <stdint.h>

typedef void (*irq_handler_t)(void);

void irq_init(void);

void irq_register_handler(
    uint8_t irq,
    irq_handler_t handler
);

void irq_unregister_handler(uint8_t irq);

void irq_enable(void);
void irq_disable(void);

/*
 * Called by interrupt_stubs.S.
 * Do not call this function directly.
 */
void interrupt_dispatch(uint64_t vector);

#endif
