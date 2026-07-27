#include "lapic.h"

#include "timer.h"

#include <stdbool.h>
#include <stdint.h>

#define CPUID_FEATURES_LEAF 1U
#define CPUID_EDX_APIC      (1U << 9)
#define CPUID_ECX_X2APIC    (1U << 21)

#define IA32_APIC_BASE_MSR  0x1BU
#define APIC_BASE_ENABLE    (1ULL << 11)
#define APIC_BASE_X2APIC    (1ULL << 10)

#define X2APIC_ID_MSR          0x802U
#define X2APIC_TPR_MSR         0x808U
#define X2APIC_EOI_MSR         0x80BU
#define X2APIC_SVR_MSR         0x80FU
#define X2APIC_ICR_MSR         0x830U
#define X2APIC_LVT_TIMER_MSR   0x832U
#define X2APIC_LVT_LINT0_MSR   0x835U
#define X2APIC_LVT_LINT1_MSR   0x836U
#define X2APIC_LVT_ERROR_MSR   0x837U
#define X2APIC_TIMER_INITIAL   0x838U
#define X2APIC_TIMER_CURRENT   0x839U
#define X2APIC_TIMER_DIVIDE    0x83EU

#define APIC_SOFTWARE_ENABLE   (1ULL << 8)
#define APIC_LVT_MASKED        (1ULL << 16)
#define APIC_TIMER_PERIODIC    (1ULL << 17)
#define APIC_DELIVERY_EXTINT   (7ULL << 8)
#define APIC_SPURIOUS_VECTOR   0xFFULL
#define APIC_TIMER_DIVIDE_16   0x3ULL
#define APIC_TIMER_SAFE_VECTOR 0x20ULL

#define LAPIC_CALIBRATION_TICKS 20ULL
#define LAPIC_CALIBRATION_SPINS 500000000ULL

static bool enabled;
static bool timer_calibrated;
static uint64_t timer_base_frequency;

static void cpuid_features(
    uint32_t *ecx,
    uint32_t *edx
)
{
    uint32_t eax = CPUID_FEATURES_LEAF;
    uint32_t ebx;
    uint32_t feature_ecx = 0;
    uint32_t feature_edx;

    __asm__ volatile(
        "cpuid"
        : "+a"(eax),
          "=b"(ebx),
          "+c"(feature_ecx),
          "=d"(feature_edx)
    );

    (void)ebx;

    *ecx = feature_ecx;
    *edx = feature_edx;
}

static uint64_t read_msr(uint32_t msr)
{
    uint32_t low;
    uint32_t high;

    __asm__ volatile(
        "rdmsr"
        : "=a"(low),
          "=d"(high)
        : "c"(msr)
    );

    return
        ((uint64_t)high << 32) |
        (uint64_t)low;
}

static void write_msr(
    uint32_t msr,
    uint64_t value
)
{
    uint32_t low =
        (uint32_t)value;

    uint32_t high =
        (uint32_t)(value >> 32);

    __asm__ volatile(
        "wrmsr"
        :
        : "c"(msr),
          "a"(low),
          "d"(high)
        : "memory"
    );
}

static bool lapic_enable_current_cpu(void)
{
    uint32_t feature_ecx;
    uint32_t feature_edx;

    cpuid_features(
        &feature_ecx,
        &feature_edx
    );

    if (
        !(feature_edx & CPUID_EDX_APIC) ||
        !(feature_ecx & CPUID_ECX_X2APIC)
    )
    {
        return false;
    }

    uint64_t apic_base =
        read_msr(IA32_APIC_BASE_MSR);

    apic_base |=
        APIC_BASE_ENABLE |
        APIC_BASE_X2APIC;

    write_msr(
        IA32_APIC_BASE_MSR,
        apic_base
    );

    write_msr(
        X2APIC_TPR_MSR,
        0
    );

    write_msr(
        X2APIC_LVT_TIMER_MSR,
        APIC_LVT_MASKED |
        APIC_TIMER_SAFE_VECTOR
    );

    write_msr(
        X2APIC_TIMER_INITIAL,
        0
    );

    write_msr(
        X2APIC_LVT_LINT1_MSR,
        APIC_LVT_MASKED
    );

    write_msr(
        X2APIC_LVT_ERROR_MSR,
        APIC_LVT_MASKED
    );

    uint64_t spurious =
        read_msr(X2APIC_SVR_MSR);

    spurious &= ~0xFFULL;

    spurious |=
        APIC_SOFTWARE_ENABLE |
        APIC_SPURIOUS_VECTOR;

    write_msr(
        X2APIC_SVR_MSR,
        spurious
    );

    enabled = true;
    return true;
}

bool lapic_init(void)
{
    timer_calibrated = false;
    timer_base_frequency = 0;

    return lapic_enable_current_cpu();
}

bool lapic_init_secondary(void)
{
    if (!lapic_enable_current_cpu())
    {
        return false;
    }

    /*
     * Secondary processors do not receive the legacy PIC ExtINT
     * input. External hardware IRQs remain targeted at the BSP.
     */
    lapic_set_legacy_pic(false);

    return true;
}

bool lapic_is_enabled(void)
{
    return enabled;
}

uint32_t lapic_id(void)
{
    if (!enabled)
    {
        return 0;
    }

    return (uint32_t)read_msr(
        X2APIC_ID_MSR
    );
}

void lapic_set_legacy_pic(
    bool legacy_enabled
)
{
    if (!enabled)
    {
        return;
    }

    write_msr(
        X2APIC_LVT_LINT0_MSR,
        legacy_enabled ?
            APIC_DELIVERY_EXTINT :
            APIC_LVT_MASKED
    );
}

void lapic_send_eoi(void)
{
    if (!enabled)
    {
        return;
    }

    write_msr(
        X2APIC_EOI_MSR,
        0
    );
}

bool lapic_send_ipi(
    uint32_t destination_apic_id,
    uint8_t vector
)
{
    if (
        !enabled ||
        vector < 0x20
    )
    {
        return false;
    }

    uint64_t command =
        ((uint64_t)destination_apic_id << 32) |
        (uint64_t)vector;

    write_msr(
        X2APIC_ICR_MSR,
        command
    );

    return true;
}

bool lapic_timer_calibrate(void)
{
    if (
        !enabled ||
        timer_frequency() == 0
    )
    {
        return false;
    }

    write_msr(
        X2APIC_TIMER_DIVIDE,
        APIC_TIMER_DIVIDE_16
    );

    write_msr(
        X2APIC_LVT_TIMER_MSR,
        APIC_LVT_MASKED |
        APIC_TIMER_SAFE_VECTOR
    );

    write_msr(
        X2APIC_TIMER_INITIAL,
        UINT32_MAX
    );

    uint64_t first_tick = timer_ticks();
    uint64_t spins = 0;

    while (
        timer_ticks() == first_tick &&
        spins < LAPIC_CALIBRATION_SPINS
    )
    {
        __asm__ volatile("pause");
        spins++;
    }

    if (spins >= LAPIC_CALIBRATION_SPINS)
    {
        lapic_timer_stop();
        return false;
    }

    uint64_t start_tick = timer_ticks();
    uint32_t start_count =
        (uint32_t)read_msr(
            X2APIC_TIMER_CURRENT
        );

    uint64_t target_tick =
        start_tick +
        LAPIC_CALIBRATION_TICKS;

    spins = 0;

    while (
        timer_ticks() < target_tick &&
        spins < LAPIC_CALIBRATION_SPINS
    )
    {
        __asm__ volatile("pause");
        spins++;
    }

    uint64_t end_tick = timer_ticks();
    uint32_t end_count =
        (uint32_t)read_msr(
            X2APIC_TIMER_CURRENT
        );

    lapic_timer_stop();

    if (
        spins >= LAPIC_CALIBRATION_SPINS ||
        end_tick <= start_tick
    )
    {
        return false;
    }

    uint64_t elapsed_counts;

    if (start_count >= end_count)
    {
        elapsed_counts =
            (uint64_t)start_count -
            (uint64_t)end_count;
    }
    else
    {
        elapsed_counts =
            (uint64_t)start_count +
            ((uint64_t)UINT32_MAX -
                (uint64_t)end_count) +
            1ULL;
    }

    uint64_t elapsed_ticks =
        end_tick - start_tick;

    if (
        elapsed_counts == 0 ||
        elapsed_ticks == 0
    )
    {
        return false;
    }

    timer_base_frequency =
        (elapsed_counts *
            (uint64_t)timer_frequency()) /
        elapsed_ticks;

    timer_calibrated =
        timer_base_frequency >= 1000ULL;

    return timer_calibrated;
}

bool lapic_timer_start_periodic(
    uint8_t vector,
    uint32_t frequency
)
{
    if (
        !enabled ||
        !timer_calibrated ||
        vector < 0x20 ||
        frequency == 0
    )
    {
        return false;
    }

    uint64_t initial_count =
        timer_base_frequency /
        (uint64_t)frequency;

    if (initial_count == 0)
    {
        initial_count = 1;
    }

    if (initial_count > UINT32_MAX)
    {
        initial_count = UINT32_MAX;
    }

    write_msr(
        X2APIC_TIMER_DIVIDE,
        APIC_TIMER_DIVIDE_16
    );

    write_msr(
        X2APIC_LVT_TIMER_MSR,
        (uint64_t)vector |
        APIC_TIMER_PERIODIC
    );

    write_msr(
        X2APIC_TIMER_INITIAL,
        initial_count
    );

    return true;
}

void lapic_timer_stop(void)
{
    if (!enabled)
    {
        return;
    }

    write_msr(
        X2APIC_LVT_TIMER_MSR,
        APIC_LVT_MASKED |
        APIC_TIMER_SAFE_VECTOR
    );

    write_msr(
        X2APIC_TIMER_INITIAL,
        0
    );
}

bool lapic_timer_is_calibrated(void)
{
    return timer_calibrated;
}

uint64_t lapic_timer_base_frequency(void)
{
    return timer_base_frequency;
}
