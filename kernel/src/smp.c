#include "smp.h"

#include "cpu_local.h"
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
static struct limine_mp_info *startup_information[
    SMP_MAX_CPUS
];

static uint32_t discovered_count;
static uint32_t bsp_id;
static bool available;
static bool x2apic_mode;

static void clear_bytes(
    void *pointer,
    size_t count
)
{
    uint8_t *bytes =
        (uint8_t *)pointer;

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

static bool configure_slot(
    uint32_t index,
    struct limine_mp_info *information,
    bool bsp
)
{
    if (
        index >= SMP_MAX_CPUS ||
        information == NULL
    )
    {
        return false;
    }

    cpu_states[index].index = index;
    cpu_states[index].processor_id =
        information->processor_id;

    cpu_states[index].apic_id =
        information->lapic_id;

    cpu_states[index].bsp = bsp;
    startup_information[index] = information;

    (void)cpu_local_configure(
        index,
        information->processor_id,
        information->lapic_id,
        bsp
    );

    state_store(
        &cpu_states[index].state,
        bsp ?
            SMP_CPU_ONLINE :
            SMP_CPU_STARTING
    );

    return true;
}

static __attribute__((noreturn)) void halt_cpu(void)
{
    for (;;)
    {
        __asm__ volatile("cli; hlt");
    }
}

static __attribute__((noreturn)) void smp_ap_entry(
    struct limine_mp_info *information
)
{
    if (information == NULL)
    {
        halt_cpu();
    }

    uint32_t index =
        (uint32_t)information->extra_argument;

    if (index >= discovered_count)
    {
        halt_cpu();
    }

    /*
     * Every AP receives its own descriptor table, TSS, ring-transition
     * stack, emergency stack, and GS-backed CPU-local data before it
     * enables interrupts or enters the worker loop.
     */
    if (!gdt_load_secondary(index))
    {
        state_store(
            &cpu_states[index].state,
            SMP_CPU_OFFLINE
        );

        halt_cpu();
    }

    idt_load();

    if (!lapic_init_secondary())
    {
        state_store(
            &cpu_states[index].state,
            SMP_CPU_OFFLINE
        );

        halt_cpu();
    }

    uint32_t actual_apic_id =
        lapic_id();

    cpu_states[index].apic_id =
        actual_apic_id;

    (void)cpu_local_configure(
        index,
        information->processor_id,
        actual_apic_id,
        false
    );

    smp_scheduler_cpu_online(index);

    bool preemption_ready =
        lapic_timer_start_periodic(
            SMP_SCHEDULER_TIMER_VECTOR,
            SMP_SCHEDULER_TIMER_HZ
        );

    smp_scheduler_set_preemption(
        index,
        preemption_ready
    );

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

    clear_bytes(
        startup_information,
        sizeof(startup_information)
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

    bsp_id = response->bsp_lapic_id;

    x2apic_mode =
        (response->flags &
            LIMINE_MP_RESPONSE_X86_64_X2APIC) != 0;

    struct limine_mp_info *bsp_information =
        NULL;

    for (
        uint64_t source_index = 0;
        source_index < response->cpu_count;
        source_index++
    )
    {
        struct limine_mp_info *information =
            response->cpus[source_index];

        if (
            information != NULL &&
            information->lapic_id == bsp_id
        )
        {
            bsp_information = information;
            break;
        }
    }

    if (bsp_information == NULL)
    {
        return false;
    }

    /* Keep the BSP at logical CPU index zero for the existing worker API. */
    if (!configure_slot(0, bsp_information, true))
    {
        return false;
    }

    discovered_count = 1;

    for (
        uint64_t source_index = 0;
        source_index < response->cpu_count &&
            discovered_count < SMP_MAX_CPUS;
        source_index++
    )
    {
        struct limine_mp_info *information =
            response->cpus[source_index];

        if (
            information == NULL ||
            information == bsp_information
        )
        {
            continue;
        }

        if (
            configure_slot(
                discovered_count,
                information,
                false
            )
        )
        {
            discovered_count++;
        }
    }

    /* Publish the BSP's actual APIC identity in its CPU-local block. */
    cpu_states[0].apic_id = lapic_id();

    (void)cpu_local_configure(
        0,
        bsp_information->processor_id,
        cpu_states[0].apic_id,
        true
    );

    available = true;

    /*
     * Calibrate the x2APIC timer against the already-running 1000 Hz
     * PIT before APs are released. Every AP then programs the same
     * calibrated timer rate for local preemptive scheduling.
     */
    (void)lapic_timer_calibrate();

    /* Start APs only after every logical slot is fully configured. */
    for (
        uint32_t index = 1;
        index < discovered_count;
        index++
    )
    {
        struct limine_mp_info *information =
            startup_information[index];

        information->extra_argument = index;

        __atomic_thread_fence(
            __ATOMIC_RELEASE
        );

        information->goto_address =
            smp_ap_entry;
    }

    /*
     * Give APs time to load their private GDT/TSS and publish worker
     * state. A failed AP remains visible as starting/offline without
     * blocking the BSP forever.
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

uint32_t smp_current_cpu_index(void)
{
    cpu_local_t *local =
        cpu_local_current();

    if (
        local != NULL &&
        local->index < discovered_count
    )
    {
        return local->index;
    }

    smp_cpu_t *cpu =
        find_by_apic_id(lapic_id());

    return cpu != NULL ?
        cpu->index :
        0;
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
    uint32_t index =
        smp_current_cpu_index();

    if (index < discovered_count)
    {
        return &cpu_states[index];
    }

    return NULL;
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
            return "online/scheduler";

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

        const cpu_local_t *local =
            cpu_local_get(index);

        smp_cpu_state_t state =
            (smp_cpu_state_t)state_load(
                &cpu->state
            );

        const char *task_name = "none";

        if (local != NULL)
        {
            if (
                local->current_process_slot == 0 &&
                local->current_pid == 0
            )
            {
                task_name = "kernel";
            }
            else if (
                local->current_kernel_thread_id != 0
            )
            {
                task_name = "kthread";
            }
            else if (
                local->current_process_slot ==
                CPU_LOCAL_NO_PROCESS &&
                state == SMP_CPU_WORKER
            )
            {
                task_name = "AP-idle";
            }
            else if (
                local->current_process_slot !=
                CPU_LOCAL_NO_PROCESS
            )
            {
                task_name = "process";
            }
        }

        kprintf(
            "CPU %u: processor=%u APIC=%u %s %s local=%s GDT/TSS=%s task=%s\n",
            (unsigned int)index,
            (unsigned int)cpu->processor_id,
            (unsigned int)cpu->apic_id,
            cpu->bsp ? "BSP" : "AP",
            smp_cpu_state_name(state),
            local != NULL && local->bound ?
                "bound" : "unbound",
            gdt_cpu_ready(index) ?
                "ready" : "missing",
            task_name
        );

        kprintf(
            "       TSS=0x%llX RSP0=0x%llX IST1=0x%llX pid=%llu kthread=%llu ticks=%llu switches=%llu preempt=%s\n",
            (unsigned long long)
                gdt_tss_address(index),
            (unsigned long long)
                gdt_kernel_stack_top(index),
            (unsigned long long)
                gdt_interrupt_stack_top(index),
            (unsigned long long)(
                local != NULL ?
                    local->current_pid : 0
            ),
            (unsigned long long)(
                local != NULL ?
                    local->current_kernel_thread_id : 0
            ),
            (unsigned long long)(
                local != NULL ?
                    local->scheduler_ticks : 0
            ),
            (unsigned long long)(
                local != NULL ?
                    local->context_switches : 0
            ),
            smp_scheduler_preemption_enabled(index) ?
                "yes" : "no"
        );
    }

    kprintf(
        "Scheduler: BSP process scheduler + AP stackful kernel threads; user processes remain BSP-only\n"
    );

    kprintf(
        "AP timer: calibrated=%s base=%llu Hz target=%u Hz\n",
        lapic_timer_is_calibrated() ? "yes" : "no",
        (unsigned long long)
            lapic_timer_base_frequency(),
        (unsigned int)SMP_SCHEDULER_TIMER_HZ
    );
}
