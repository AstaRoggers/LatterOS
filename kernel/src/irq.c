#include "irq.h"

#include "lapic.h"
#include "panic.h"
#include "pic.h"
#include "process.h"
#include "syscall.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IRQ_COUNT        16
#define IRQ_VECTOR_BASE  32
#define IRQ_VECTOR_END   47
#define SYSCALL_VECTOR   128
#define SCHEDULER_VECTOR 129
#define SPURIOUS_VECTOR  255

static irq_handler_t irq_handlers[IRQ_COUNT];
static bool use_lapic;

void irq_init(void)
{
    use_lapic = false;

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

void irq_set_lapic_enabled(
    bool enabled
)
{
    use_lapic = enabled;
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

    if (irq >= 8)
    {
        pic_clear_mask(2);
    }

    pic_clear_mask(irq);
}

void irq_unregister_handler(
    uint8_t irq
)
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

cpu_context_t *interrupt_dispatch(
    uint64_t vector,
    cpu_context_t *context
)
{
    if (vector == SPURIOUS_VECTOR)
    {
        return context;
    }

    if (vector == SYSCALL_VECTOR)
    {
        return syscall_dispatch(context);
    }

    if (vector == SCHEDULER_VECTOR)
    {
        return process_schedule_now(
            context
        );
    }

    if (vector < IRQ_VECTOR_BASE)
    {
        kernel_panic(
            "Unhandled CPU exception",
            vector
        );

        return context;
    }

    if (vector <= IRQ_VECTOR_END)
    {
        uint8_t irq =
            (uint8_t)(
                vector -
                IRQ_VECTOR_BASE
            );

        irq_handler_t handler =
            irq_handlers[irq];

        if (handler != NULL)
        {
            handler();
        }

        pic_send_eoi(irq);

        if (use_lapic)
        {
            lapic_send_eoi();
        }

        if (irq == 0)
        {
            return process_schedule_on_timer(
                context
            );
        }

        return context;
    }

    kernel_panic(
        "Unhandled interrupt vector",
        vector
    );

    return context;
}
