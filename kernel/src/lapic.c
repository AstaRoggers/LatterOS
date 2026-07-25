#include "lapic.h"

#include <stdbool.h>
#include <stdint.h>

#define CPUID_FEATURES_LEAF 1U
#define CPUID_EDX_APIC      (1U << 9)
#define CPUID_ECX_X2APIC    (1U << 21)

#define IA32_APIC_BASE_MSR  0x1BU
#define APIC_BASE_ENABLE    (1ULL << 11)
#define APIC_BASE_X2APIC    (1ULL << 10)

#define X2APIC_TPR_MSR       0x808U
#define X2APIC_EOI_MSR       0x80BU
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

bool lapic_init(void)
{
    enabled = false;

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
        X2APIC_LVT_LINT0_MSR,
        APIC_DELIVERY_EXTINT
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

bool lapic_is_enabled(void)
{
    return enabled;
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
