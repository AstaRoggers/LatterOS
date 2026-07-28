#include "platform_detect.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool initialized;
static bool hypervisor_present;
static platform_hypervisor_t detected_hypervisor;
static char hypervisor_vendor[13];

static void cpuid(
    uint32_t leaf,
    uint32_t subleaf,
    uint32_t *eax,
    uint32_t *ebx,
    uint32_t *ecx,
    uint32_t *edx
)
{
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;

    __asm__ volatile(
        "cpuid"
        : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
        : "a"(leaf), "c"(subleaf)
        : "memory"
    );

    if (eax != NULL)
    {
        *eax = a;
    }

    if (ebx != NULL)
    {
        *ebx = b;
    }

    if (ecx != NULL)
    {
        *ecx = c;
    }

    if (edx != NULL)
    {
        *edx = d;
    }
}

static void store_u32(char *destination, uint32_t value)
{
    destination[0] = (char)(value & 0xFFU);
    destination[1] = (char)((value >> 8) & 0xFFU);
    destination[2] = (char)((value >> 16) & 0xFFU);
    destination[3] = (char)((value >> 24) & 0xFFU);
}

static bool text_equal(const char *first, const char *second)
{
    if (first == NULL || second == NULL)
    {
        return false;
    }

    size_t index = 0;

    while (first[index] != '\0' && second[index] != '\0')
    {
        if (first[index] != second[index])
        {
            return false;
        }

        index++;
    }

    return first[index] == second[index];
}

static platform_hypervisor_t classify_vendor(const char *vendor)
{
    if (text_equal(vendor, "VBoxVBoxVBox"))
    {
        return PLATFORM_HYPERVISOR_VIRTUALBOX;
    }

    if (text_equal(vendor, "KVMKVMKVM"))
    {
        return PLATFORM_HYPERVISOR_KVM;
    }

    if (text_equal(vendor, "Microsoft Hv"))
    {
        return PLATFORM_HYPERVISOR_HYPERV;
    }

    if (text_equal(vendor, "VMwareVMware"))
    {
        return PLATFORM_HYPERVISOR_VMWARE;
    }

    if (text_equal(vendor, "XenVMMXenVMM"))
    {
        return PLATFORM_HYPERVISOR_XEN;
    }

    if (text_equal(vendor, "TCGTCGTCGTCG"))
    {
        return PLATFORM_HYPERVISOR_TCG;
    }

    return PLATFORM_HYPERVISOR_UNKNOWN;
}

void platform_detect_init(void)
{
    if (initialized)
    {
        return;
    }

    initialized = true;
    hypervisor_present = false;
    detected_hypervisor = PLATFORM_HYPERVISOR_NONE;

    for (uint32_t index = 0; index < sizeof(hypervisor_vendor); index++)
    {
        hypervisor_vendor[index] = '\0';
    }

    uint32_t maximum_basic;
    uint32_t ecx;
    cpuid(0U, 0U, &maximum_basic, NULL, NULL, NULL);

    if (maximum_basic < 1U)
    {
        return;
    }

    cpuid(1U, 0U, NULL, NULL, &ecx, NULL);
    hypervisor_present = (ecx & (1U << 31)) != 0U;

    if (!hypervisor_present)
    {
        return;
    }

    uint32_t maximum_hypervisor;
    uint32_t ebx;
    uint32_t edx;

    cpuid(
        0x40000000U,
        0U,
        &maximum_hypervisor,
        &ebx,
        &ecx,
        &edx
    );

    (void)maximum_hypervisor;
    store_u32(&hypervisor_vendor[0], ebx);
    store_u32(&hypervisor_vendor[4], ecx);
    store_u32(&hypervisor_vendor[8], edx);
    hypervisor_vendor[12] = '\0';
    detected_hypervisor = classify_vendor(hypervisor_vendor);
}

bool platform_hypervisor_present(void)
{
    platform_detect_init();
    return hypervisor_present;
}

platform_hypervisor_t platform_hypervisor(void)
{
    platform_detect_init();
    return detected_hypervisor;
}

const char *platform_hypervisor_name(void)
{
    switch (platform_hypervisor())
    {
        case PLATFORM_HYPERVISOR_VIRTUALBOX:
            return "Oracle VirtualBox";

        case PLATFORM_HYPERVISOR_KVM:
            return "KVM";

        case PLATFORM_HYPERVISOR_HYPERV:
            return "Microsoft Hyper-V";

        case PLATFORM_HYPERVISOR_VMWARE:
            return "VMware";

        case PLATFORM_HYPERVISOR_XEN:
            return "Xen";

        case PLATFORM_HYPERVISOR_TCG:
            return "QEMU TCG";

        case PLATFORM_HYPERVISOR_UNKNOWN:
            return "Unknown hypervisor";

        case PLATFORM_HYPERVISOR_NONE:
        default:
            return "Bare metal";
    }
}

const char *platform_hypervisor_vendor(void)
{
    platform_detect_init();
    return hypervisor_present ? hypervisor_vendor : "none";
}

bool platform_is_virtualbox(void)
{
    return platform_hypervisor() == PLATFORM_HYPERVISOR_VIRTUALBOX;
}
