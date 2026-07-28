#ifndef PLATFORM_DETECT_H
#define PLATFORM_DETECT_H

#include <stdbool.h>

typedef enum
{
    PLATFORM_HYPERVISOR_NONE,
    PLATFORM_HYPERVISOR_VIRTUALBOX,
    PLATFORM_HYPERVISOR_KVM,
    PLATFORM_HYPERVISOR_HYPERV,
    PLATFORM_HYPERVISOR_VMWARE,
    PLATFORM_HYPERVISOR_XEN,
    PLATFORM_HYPERVISOR_TCG,
    PLATFORM_HYPERVISOR_UNKNOWN
} platform_hypervisor_t;

void platform_detect_init(void);
bool platform_hypervisor_present(void);
platform_hypervisor_t platform_hypervisor(void);
const char *platform_hypervisor_name(void);
const char *platform_hypervisor_vendor(void);
bool platform_is_virtualbox(void);

#endif
