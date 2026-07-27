#include "irq.h"

#include "ioapic.h"
#include "lapic.h"
#include "panic.h"
#include "pic.h"
#include "process.h"
#include "syscall.h"
#include "smp_scheduler.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IRQ_COUNT        16
#define IRQ_VECTOR_BASE  32
#define IRQ_VECTOR_END   47
#define SYSCALL_VECTOR   128
#define SCHEDULER_VECTOR 129
#define SMP_IPI_VECTOR   SMP_SCHEDULER_IPI_VECTOR
#define SMP_YIELD_VECTOR SMP_SCHEDULER_YIELD_VECTOR
#define SMP_TIMER_VECTOR SMP_SCHEDULER_TIMER_VECTOR
#define SPURIOUS_VECTOR  255

static irq_handler_t irq_handlers[IRQ_COUNT];
static bool use_lapic;
static bool use_ioapic;

void irq_init(void)
{
    use_lapic = false;
    use_ioapic = false;

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

void irq_set_ioapic_enabled(
    bool enabled
)
{
    use_ioapic =
        enabled && ioapic_is_ready();
}

bool irq_ioapic_enabled(void)
{
    return use_ioapic;
}

const char *irq_controller_name(void)
{
    return use_ioapic ?
        "I/O APIC" :
        "8259 PIC";
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

    if (use_ioapic)
    {
        (void)ioapic_route_isa_irq(
            irq,
            (uint8_t)(IRQ_VECTOR_BASE + irq),
            false
        );

        return;
    }

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

    if (use_ioapic)
    {
        (void)ioapic_mask_isa_irq(irq);
    }
    else
    {
        pic_set_mask(irq);
    }

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

    if (vector == SMP_IPI_VECTOR)
    {
        smp_scheduler_handle_ipi();

        if (use_lapic)
        {
            lapic_send_eoi();
        }

        return context;
    }

    if (vector == SMP_YIELD_VECTOR)
    {
        return smp_scheduler_handle_yield(
            context
        );
    }

    if (vector == SMP_TIMER_VECTOR)
    {
        cpu_context_t *next =
            smp_scheduler_handle_timer(
                context
            );

        if (use_lapic)
        {
            lapic_send_eoi();
        }

        return next;
    }

    if (vector < IRQ_VECTOR_BASE)
    {
        bool from_user = (
            context != NULL &&
            (context->cs & 0x3ULL) == 0x3ULL
        );

        if (from_user)
        {
            uint64_t fault_address = 0;

            if (vector == 14)
            {
                __asm__ volatile(
                    "mov %%cr2, %0"
                    : "=r"(fault_address)
                );
            }

            return process_fault_from_exception(
                context,
                vector,
                context->error_code,
                fault_address
            );
        }

        kernel_panic_context(
            "Unhandled CPU exception",
            context
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

        if (use_ioapic)
        {
            if (use_lapic)
            {
                lapic_send_eoi();
            }
        }
        else
        {
            pic_send_eoi(irq);

            if (use_lapic)
            {
                lapic_send_eoi();
            }
        }

        if (irq == 0)
        {
            return process_schedule_on_timer(
                context
            );
        }

        return context;
    }

    kernel_panic_context(
        "Unhandled interrupt vector",
        context
    );

    return context;
}
