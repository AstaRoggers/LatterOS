#ifndef ACPI_H
#define ACPI_H

#include <stdbool.h>
#include <stdint.h>

#define ACPI_MAX_CPUS          64
#define ACPI_MAX_IOAPICS        8
#define ACPI_MAX_ISO_OVERRIDES 16

typedef struct
{
    uint32_t apic_id;
    uint32_t acpi_uid;
    bool enabled;
    bool x2apic;
} acpi_cpu_info_t;

typedef struct
{
    uint8_t id;
    uint32_t address;
    uint32_t gsi_base;
} acpi_ioapic_info_t;

typedef struct
{
    uint8_t source_irq;
    uint32_t gsi;
    bool active_low;
    bool level_triggered;
} acpi_iso_info_t;

bool acpi_init(void *rsdp_address);
bool acpi_is_available(void);

const char *acpi_root_table_name(void);
const char *acpi_oem_id(void);

uint64_t acpi_local_apic_address(void);

uint32_t acpi_cpu_count(void);
const acpi_cpu_info_t *acpi_cpu(uint32_t index);

uint32_t acpi_ioapic_count(void);
const acpi_ioapic_info_t *acpi_ioapic(uint32_t index);

uint32_t acpi_iso_count(void);
const acpi_iso_info_t *acpi_iso(uint32_t index);

bool acpi_resolve_isa_irq(
    uint8_t irq,
    uint32_t *gsi,
    bool *active_low,
    bool *level_triggered
);

#endif
