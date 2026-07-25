#include "irq.h"

#include "panic.h"
#include "pic.h"

#include <stddef.h>
#include <stdint.h>

#define IRQ_COUNT        16
#define IRQ_VECTOR_BASE  32
#define IRQ_VECTOR_END   (IRQ_VECTOR_BASE + IRQ_COUNT - 1)

static irq_handler_t irq_handlers[IRQ_COUNT];

void irq_init(void)
{
    for (
        uint8_t irq = 0;
        irq < IRQ_COUNT;
        irq++
    )
    {
        irq_handlers[irq] = NULL;
        pic_set_mask(irq);
    }
}

void irq_register_handler(
    uint8_t irq,
    irq_handler_t handler
)
{
    if (
        irq >= IRQ_COUNT ||
        handler == NULL
    )
    {
        return;
    }

    irq_handlers[irq] = handler;

    /*
     * Slave PIC interrupts travel through master IRQ 2.
     */
    if (irq >= 8)
    {
        pic_clear_mask(2);
    }

    pic_clear_mask(irq);
}

void irq_unregister_handler(uint8_t irq)
{
    if (irq >= IRQ_COUNT)
    {
        return;
    }

    pic_set_mask(irq);
    irq_handlers[irq] = NULL;
}

void irq_enable(void)
{
    __asm__ volatile(
        "sti"
        :
        :
        : "memory"
    );
}

void irq_disable(void)
{
    __asm__ volatile(
        "cli"
        :
        :
        : "memory"
    );
}

void interrupt_dispatch(uint64_t vector)
{
    if (vector < IRQ_VECTOR_BASE)
    {
        kernel_panic(
            "Unhandled CPU exception",
            vector
        );
    }

    if (vector <= IRQ_VECTOR_END)
    {
        uint8_t irq =
            (uint8_t)(vector - IRQ_VECTOR_BASE);

        irq_handler_t handler =
            irq_handlers[irq];

        if (handler != NULL)
        {
            handler();
        }

        pic_send_eoi(irq);
        return;
    }

    kernel_panic(
        "Unhandled interrupt vector",
        vector
    );
}
