#include "smp.h"

#include "gdt.h"
#include "idt.h"
#include "irq.h"
#include "kstdio.h"
#include "lapic.h"
#include "smp_scheduler.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SMP_STARTUP_SPIN_LIMIT 100000000ULL

static smp_cpu_t cpu_states[SMP_MAX_CPUS];
static uint32_t discovered_count;
static uint32_t bsp_id;
static bool available;
static bool x2apic_mode;

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

static uint32_t state_load(
    const volatile uint32_t *state
)
{
    return __atomic_load_n(
        state,
        __ATOMIC_ACQUIRE
    );
}

static void state_store(
    volatile uint32_t *state,
    smp_cpu_state_t value
)
{
    __atomic_store_n(
        state,
        (uint32_t)value,
        __ATOMIC_RELEASE
    );
}

static smp_cpu_t *find_by_apic_id(
    uint32_t apic_id
)
{
    for (
        uint32_t index = 0;
        index < discovered_count;
        index++
    )
    {
        if (cpu_states[index].apic_id == apic_id)
        {
            return &cpu_states[index];
        }
    }

    return NULL;
}

static __attribute__((noreturn)) void smp_ap_entry(
    struct limine_mp_info *information
)
{
    uint32_t index =
        (uint32_t)information->extra_argument;

    if (index >= discovered_count)
    {
        for (;;)
        {
            __asm__ volatile("cli; hlt");
        }
    }

    /*
     * Limine enters the AP in 64-bit mode with a private stack. Load
     * the shared LatterOS descriptor tables, initialize this CPU's
     * local APIC, publish per-CPU state, and enter the AP work loop.
     */
    gdt_load_secondary();
    idt_load();

    if (!lapic_init_secondary())
    {
        state_store(
            &cpu_states[index].state,
            SMP_CPU_OFFLINE
        );

        for (;;)
        {
            __asm__ volatile("cli; hlt");
        }
    }

    cpu_states[index].apic_id =
        lapic_id();

    smp_scheduler_cpu_online(index);

    state_store(
        &cpu_states[index].state,
        SMP_CPU_WORKER
    );

    irq_enable();
    smp_scheduler_ap_loop(index);
}

bool smp_init(
    struct limine_mp_response *response
)
{
    clear_bytes(
        cpu_states,
        sizeof(cpu_states)
    );

    discovered_count = 0;
    bsp_id = 0;
    available = false;
    x2apic_mode = false;

    if (
        response == NULL ||
        response->cpus == NULL ||
        response->cpu_count == 0
    )
    {
        return false;
    }

    uint64_t reported_count =
        response->cpu_count;

    if (reported_count > SMP_MAX_CPUS)
    {
        reported_count = SMP_MAX_CPUS;
    }

    discovered_count =
        (uint32_t)reported_count;

    bsp_id = response->bsp_lapic_id;

    x2apic_mode =
        (response->flags &
            LIMINE_MP_RESPONSE_X86_64_X2APIC) != 0;

    for (
        uint32_t index = 0;
        index < discovered_count;
        index++
    )
    {
        struct limine_mp_info *information =
            response->cpus[index];

        cpu_states[index].index = index;

        if (information == NULL)
        {
            state_store(
                &cpu_states[index].state,
                SMP_CPU_OFFLINE
            );
            continue;
        }

        cpu_states[index].processor_id =
            information->processor_id;

        cpu_states[index].apic_id =
            information->lapic_id;

        cpu_states[index].bsp =
            information->lapic_id == bsp_id;

        if (cpu_states[index].bsp)
        {
            state_store(
                &cpu_states[index].state,
                SMP_CPU_ONLINE
            );
            continue;
        }

        state_store(
            &cpu_states[index].state,
            SMP_CPU_STARTING
        );

        information->extra_argument = index;

        __atomic_thread_fence(
            __ATOMIC_RELEASE
        );

        information->goto_address =
            smp_ap_entry;
    }

    available = true;

    /*
     * Give the application processors time to publish that they
     * reached their parked state. Failure to reach it does not stop
     * the BSP; offline CPUs remain visible in the diagnostic command.
     */
    for (
        uint64_t spin = 0;
        spin < SMP_STARTUP_SPIN_LIMIT;
        spin++
    )
    {
        if (smp_online_count() >= discovered_count)
        {
            break;
        }

        __asm__ volatile("pause");
    }

    return true;
}

bool smp_is_available(void)
{
    return available;
}

bool smp_uses_x2apic(void)
{
    return x2apic_mode;
}

uint32_t smp_cpu_count(void)
{
    return discovered_count;
}

uint32_t smp_online_count(void)
{
    uint32_t count = 0;

    for (
        uint32_t index = 0;
        index < discovered_count;
        index++
    )
    {
        uint32_t state =
            state_load(&cpu_states[index].state);

        if (
            state == SMP_CPU_ONLINE ||
            state == SMP_CPU_PARKED ||
            state == SMP_CPU_WORKER
        )
        {
            count++;
        }
    }

    return count;
}

uint32_t smp_bsp_apic_id(void)
{
    return bsp_id;
}

const smp_cpu_t *smp_cpu(uint32_t index)
{
    if (index >= discovered_count)
    {
        return NULL;
    }

    return &cpu_states[index];
}

const smp_cpu_t *smp_current_cpu(void)
{
    return find_by_apic_id(
        lapic_id()
    );
}

const char *smp_cpu_state_name(
    smp_cpu_state_t state
)
{
    switch (state)
    {
        case SMP_CPU_STARTING:
            return "starting";

        case SMP_CPU_ONLINE:
            return "online";

        case SMP_CPU_PARKED:
            return "online/parked";

        case SMP_CPU_WORKER:
            return "online/worker";

        default:
            return "offline";
    }
}

void smp_print_status(void)
{
    if (!available)
    {
        kprintf(
            "SMP: Limine MP response unavailable\n"
        );
        return;
    }

    kprintf(
        "SMP: discovered=%u online=%u BSP-APIC=%u mode=%s\n",
        (unsigned int)discovered_count,
        (unsigned int)smp_online_count(),
        (unsigned int)bsp_id,
        x2apic_mode ? "x2APIC" : "xAPIC"
    );

    for (
        uint32_t index = 0;
        index < discovered_count;
        index++
    )
    {
        const smp_cpu_t *cpu =
            &cpu_states[index];

        smp_cpu_state_t state =
            (smp_cpu_state_t)state_load(
                &cpu->state
            );

        kprintf(
            "CPU %u: processor=%u APIC=%u %s %s\n",
            (unsigned int)index,
            (unsigned int)cpu->processor_id,
            (unsigned int)cpu->apic_id,
            cpu->bsp ? "BSP" : "AP",
            smp_cpu_state_name(state)
        );
    }

    kprintf(
        "Scheduler: BSP process scheduler + AP work queues\n"
    );
}
