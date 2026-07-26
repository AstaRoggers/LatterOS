#include "lapic.h"

#include <stdbool.h>
#include <stdint.h>

#define CPUID_FEATURES_LEAF 1U
#define CPUID_EDX_APIC      (1U << 9)
#define CPUID_ECX_X2APIC    (1U << 21)

#define IA32_APIC_BASE_MSR  0x1BU
#define APIC_BASE_ENABLE    (1ULL << 11)
#define APIC_BASE_X2APIC    (1ULL << 10)

#define X2APIC_ID_MSR        0x802U
#define X2APIC_TPR_MSR       0x808U
#define X2APIC_EOI_MSR       0x80BU
#define X2APIC_ICR_MSR       0x830U
#define X2APIC_SVR_MSR       0x80FU
#define X2APIC_LVT_TIMER_MSR 0x832U
#define X2APIC_LVT_LINT0_MSR 0x835U
#define X2APIC_LVT_LINT1_MSR 0x836U
#define X2APIC_LVT_ERROR_MSR 0x837U

#define APIC_SOFTWARE_ENABLE (1ULL << 8)
#define APIC_LVT_MASKED       (1ULL << 16)
#define APIC_DELIVERY_EXTINT  (7ULL << 8)
#define APIC_SPURIOUS_VECTOR  0xFFULL

static bool enabled;

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
        APIC_LVT_MASKED
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
     * input. External hardware IRQs remain targeted at the BSP
     * until the SMP scheduler and per-CPU interrupt balancing land.
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
