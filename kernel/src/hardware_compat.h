#ifndef HARDWARE_COMPAT_H
#define HARDWARE_COMPAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum
{
    HARDWARE_COMPAT_UNKNOWN,
    HARDWARE_COMPAT_READY,
    HARDWARE_COMPAT_WARNING,
    HARDWARE_COMPAT_UNSUPPORTED
} hardware_compat_status_t;

typedef struct
{
    hardware_compat_status_t status;
    uint32_t generation;

    char cpu_vendor[13];
    char cpu_brand[49];
    uint32_t cpu_family;
    uint32_t cpu_model;
    uint32_t cpu_stepping;

    bool long_mode;
    bool nx;
    bool apic;
    bool x2apic;
    bool tsc;
    bool invariant_tsc;
    bool sse2;

    bool hypervisor_present;
    bool virtualbox;
    char hypervisor_name[32];
    char hypervisor_vendor[16];

    uint32_t detected_cpus;
    uint32_t online_cpus;
    uint64_t total_memory_bytes;
    uint64_t usable_memory_bytes;

    bool acpi_available;
    char acpi_root[8];
    char acpi_oem[8];
    uint32_t acpi_cpus;
    uint32_t acpi_ioapics;
    uint32_t acpi_overrides;

    uint32_t pci_devices;
    uint32_t pci_storage;
    uint32_t pci_network;
    uint32_t pci_display;
    uint32_t pci_usb;
    uint32_t pci_bridges;

    uint32_t block_devices;
    uint32_t writable_block_devices;
    uint32_t removable_block_devices;
    uint32_t ahci_devices;
    uint32_t nvme_namespaces;

    bool usb_ready;
    uint32_t usb_devices;
    bool usb_keyboard;
    bool usb_mouse;

    bool network_ready;
    bool network_link;
    char display_backend[32];
    uint32_t display_width;
    uint32_t display_height;
    uint32_t display_refresh_hz;

    uint32_t warning_count;
    uint32_t failure_count;
} hardware_compat_snapshot_t;

void hardware_compat_init(void);
void hardware_compat_refresh(void);
const hardware_compat_snapshot_t *hardware_compat_snapshot(void);

hardware_compat_status_t hardware_compat_status(void);
const char *hardware_compat_status_name(void);
const char *hardware_compat_summary(void);
const char *hardware_compat_last_message(void);

bool hardware_compat_run_quick_test(void);
bool hardware_compat_write_report(void);
const char *hardware_compat_report_path(void);

#endif
