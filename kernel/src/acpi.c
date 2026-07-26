#include "acpi.h"

#include "hhdm.h"

#include <stddef.h>
#include <stdint.h>

#define ACPI_RSDP_SIGNATURE "RSD PTR "
#define ACPI_MADT_SIGNATURE "APIC"

#define MADT_ENTRY_LOCAL_APIC             0
#define MADT_ENTRY_IOAPIC                 1
#define MADT_ENTRY_INTERRUPT_OVERRIDE     2
#define MADT_ENTRY_LOCAL_APIC_OVERRIDE    5
#define MADT_ENTRY_LOCAL_X2APIC           9

#define MADT_CPU_ENABLED                  0x00000001U
#define MADT_CPU_ONLINE_CAPABLE           0x00000002U

#define ACPI_POLARITY_MASK                0x0003U
#define ACPI_POLARITY_ACTIVE_LOW          0x0003U
#define ACPI_TRIGGER_MASK                 0x000CU
#define ACPI_TRIGGER_LEVEL                0x000CU

typedef struct __attribute__((packed))
{
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
} acpi_rsdp_v1_t;

typedef struct __attribute__((packed))
{
    acpi_rsdp_v1_t v1;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} acpi_rsdp_v2_t;

typedef struct __attribute__((packed))
{
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} acpi_sdt_header_t;

typedef struct __attribute__((packed))
{
    acpi_sdt_header_t header;
    uint32_t local_apic_address;
    uint32_t flags;
    uint8_t entries[];
} acpi_madt_t;

typedef struct __attribute__((packed))
{
    uint8_t type;
    uint8_t length;
} madt_entry_header_t;

typedef struct __attribute__((packed))
{
    madt_entry_header_t header;
    uint8_t acpi_processor_id;
    uint8_t apic_id;
    uint32_t flags;
} madt_local_apic_t;

typedef struct __attribute__((packed))
{
    madt_entry_header_t header;
    uint8_t ioapic_id;
    uint8_t reserved;
    uint32_t address;
    uint32_t gsi_base;
} madt_ioapic_t;

typedef struct __attribute__((packed))
{
    madt_entry_header_t header;
    uint8_t bus;
    uint8_t source_irq;
    uint32_t gsi;
    uint16_t flags;
} madt_interrupt_override_t;

typedef struct __attribute__((packed))
{
    madt_entry_header_t header;
    uint16_t reserved;
    uint64_t address;
} madt_local_apic_override_t;

typedef struct __attribute__((packed))
{
    madt_entry_header_t header;
    uint16_t reserved;
    uint32_t x2apic_id;
    uint32_t flags;
    uint32_t acpi_uid;
} madt_local_x2apic_t;

static bool available;
static bool using_xsdt;
static uint64_t local_apic_address;
static char oem_text[7];

static acpi_cpu_info_t cpus[ACPI_MAX_CPUS];
static uint32_t cpu_count;

static acpi_ioapic_info_t ioapics[ACPI_MAX_IOAPICS];
static uint32_t ioapic_count;

static acpi_iso_info_t overrides[ACPI_MAX_ISO_OVERRIDES];
static uint32_t override_count;

static bool bytes_equal(
    const char *first,
    const char *second,
    size_t length
)
{
    for (size_t index = 0; index < length; index++)
    {
        if (first[index] != second[index])
        {
            return false;
        }
    }

    return true;
}

static bool checksum_valid(
    const void *address,
    size_t length
)
{
    const uint8_t *bytes =
        (const uint8_t *)address;

    uint8_t checksum = 0;

    for (size_t index = 0; index < length; index++)
    {
        checksum =
            (uint8_t)(checksum + bytes[index]);
    }

    return checksum == 0;
}

static void copy_oem_id(const char source[6])
{
    for (uint32_t index = 0; index < 6; index++)
    {
        oem_text[index] = source[index];
    }

    oem_text[6] = '\0';
}

static const acpi_sdt_header_t *map_table(
    uint64_t physical_address
)
{
    if (physical_address == 0)
    {
        return NULL;
    }

    return (const acpi_sdt_header_t *)
        physical_to_virtual(physical_address);
}

static bool table_valid(
    const acpi_sdt_header_t *table
)
{
    if (
        table == NULL ||
        table->length < sizeof(acpi_sdt_header_t)
    )
    {
        return false;
    }

    return checksum_valid(
        table,
        table->length
    );
}

static void add_cpu(
    uint32_t apic_id,
    uint32_t acpi_uid,
    uint32_t flags,
    bool x2apic
)
{
    if (cpu_count >= ACPI_MAX_CPUS)
    {
        return;
    }

    cpus[cpu_count].apic_id = apic_id;
    cpus[cpu_count].acpi_uid = acpi_uid;
    cpus[cpu_count].enabled =
        (flags & (
            MADT_CPU_ENABLED |
            MADT_CPU_ONLINE_CAPABLE
        )) != 0;
    cpus[cpu_count].x2apic = x2apic;

    cpu_count++;
}

static void parse_madt(
    const acpi_madt_t *madt
)
{
    local_apic_address =
        madt->local_apic_address;

    const uint8_t *cursor = madt->entries;
    const uint8_t *end =
        (const uint8_t *)madt +
        madt->header.length;

    while (
        cursor + sizeof(madt_entry_header_t) <= end
    )
    {
        const madt_entry_header_t *header =
            (const madt_entry_header_t *)cursor;

        if (
            header->length <
                sizeof(madt_entry_header_t) ||
            cursor + header->length > end
        )
        {
            break;
        }

        switch (header->type)
        {
            case MADT_ENTRY_LOCAL_APIC:
            {
                if (
                    header->length >=
                    sizeof(madt_local_apic_t)
                )
                {
                    const madt_local_apic_t *entry =
                        (const madt_local_apic_t *)cursor;

                    add_cpu(
                        entry->apic_id,
                        entry->acpi_processor_id,
                        entry->flags,
                        false
                    );
                }

                break;
            }

            case MADT_ENTRY_IOAPIC:
            {
                if (
                    header->length >= sizeof(madt_ioapic_t) &&
                    ioapic_count < ACPI_MAX_IOAPICS
                )
                {
                    const madt_ioapic_t *entry =
                        (const madt_ioapic_t *)cursor;

                    ioapics[ioapic_count].id =
                        entry->ioapic_id;
                    ioapics[ioapic_count].address =
                        entry->address;
                    ioapics[ioapic_count].gsi_base =
                        entry->gsi_base;

                    ioapic_count++;
                }

                break;
            }

            case MADT_ENTRY_INTERRUPT_OVERRIDE:
            {
                if (
                    header->length >=
                        sizeof(madt_interrupt_override_t) &&
                    override_count <
                        ACPI_MAX_ISO_OVERRIDES
                )
                {
                    const madt_interrupt_override_t *entry =
                        (const madt_interrupt_override_t *)cursor;

                    if (entry->bus == 0)
                    {
                        overrides[override_count].source_irq =
                            entry->source_irq;
                        overrides[override_count].gsi =
                            entry->gsi;
                        overrides[override_count].active_low =
                            (entry->flags & ACPI_POLARITY_MASK) ==
                            ACPI_POLARITY_ACTIVE_LOW;
                        overrides[override_count].level_triggered =
                            (entry->flags & ACPI_TRIGGER_MASK) ==
                            ACPI_TRIGGER_LEVEL;

                        override_count++;
                    }
                }

                break;
            }

            case MADT_ENTRY_LOCAL_APIC_OVERRIDE:
            {
                if (
                    header->length >=
                    sizeof(madt_local_apic_override_t)
                )
                {
                    const madt_local_apic_override_t *entry =
                        (const madt_local_apic_override_t *)cursor;

                    local_apic_address = entry->address;
                }

                break;
            }

            case MADT_ENTRY_LOCAL_X2APIC:
            {
                if (
                    header->length >=
                    sizeof(madt_local_x2apic_t)
                )
                {
                    const madt_local_x2apic_t *entry =
                        (const madt_local_x2apic_t *)cursor;

                    add_cpu(
                        entry->x2apic_id,
                        entry->acpi_uid,
                        entry->flags,
                        true
                    );
                }

                break;
            }

            default:
                break;
        }

        cursor += header->length;
    }
}

static const acpi_sdt_header_t *find_table(
    const acpi_sdt_header_t *root,
    const char signature[4]
)
{
    if (!table_valid(root))
    {
        return NULL;
    }

    size_t entry_size =
        using_xsdt ? sizeof(uint64_t) : sizeof(uint32_t);

    size_t entry_count =
        (root->length - sizeof(acpi_sdt_header_t)) /
        entry_size;

    const uint8_t *entries =
        (const uint8_t *)root +
        sizeof(acpi_sdt_header_t);

    for (size_t index = 0; index < entry_count; index++)
    {
        uint64_t physical_address;

        if (using_xsdt)
        {
            const uint64_t *entry =
                (const uint64_t *)(
                    entries + index * sizeof(uint64_t)
                );

            physical_address = *entry;
        }
        else
        {
            const uint32_t *entry =
                (const uint32_t *)(
                    entries + index * sizeof(uint32_t)
                );

            physical_address = *entry;
        }

        const acpi_sdt_header_t *table =
            map_table(physical_address);

        if (
            table_valid(table) &&
            bytes_equal(
                table->signature,
                signature,
                4
            )
        )
        {
            return table;
        }
    }

    return NULL;
}

bool acpi_init(void *rsdp_address)
{
    available = false;
    using_xsdt = false;
    local_apic_address = 0;
    cpu_count = 0;
    ioapic_count = 0;
    override_count = 0;

    for (uint32_t index = 0; index < 7; index++)
    {
        oem_text[index] = '\0';
    }

    if (rsdp_address == NULL)
    {
        return false;
    }

    const acpi_rsdp_v1_t *rsdp_v1 =
        (const acpi_rsdp_v1_t *)rsdp_address;

    if (
        !bytes_equal(
            rsdp_v1->signature,
            ACPI_RSDP_SIGNATURE,
            8
        ) ||
        !checksum_valid(
            rsdp_v1,
            sizeof(acpi_rsdp_v1_t)
        )
    )
    {
        return false;
    }

    copy_oem_id(rsdp_v1->oem_id);

    uint64_t root_physical =
        rsdp_v1->rsdt_address;

    if (rsdp_v1->revision >= 2)
    {
        const acpi_rsdp_v2_t *rsdp_v2 =
            (const acpi_rsdp_v2_t *)rsdp_address;

        if (
            rsdp_v2->length >= sizeof(acpi_rsdp_v2_t) &&
            checksum_valid(
                rsdp_v2,
                rsdp_v2->length
            ) &&
            rsdp_v2->xsdt_address != 0
        )
        {
            using_xsdt = true;
            root_physical = rsdp_v2->xsdt_address;
        }
    }

    const acpi_sdt_header_t *root =
        map_table(root_physical);

    if (!table_valid(root))
    {
        return false;
    }

    const acpi_sdt_header_t *madt_header =
        find_table(
            root,
            ACPI_MADT_SIGNATURE
        );

    if (
        madt_header == NULL ||
        madt_header->length < sizeof(acpi_madt_t)
    )
    {
        return false;
    }

    parse_madt(
        (const acpi_madt_t *)madt_header
    );

    available = ioapic_count > 0;
    return available;
}

bool acpi_is_available(void)
{
    return available;
}

const char *acpi_root_table_name(void)
{
    return using_xsdt ? "XSDT" : "RSDT";
}

const char *acpi_oem_id(void)
{
    return oem_text;
}

uint64_t acpi_local_apic_address(void)
{
    return local_apic_address;
}

uint32_t acpi_cpu_count(void)
{
    return cpu_count;
}

const acpi_cpu_info_t *acpi_cpu(uint32_t index)
{
    if (index >= cpu_count)
    {
        return NULL;
    }

    return &cpus[index];
}

uint32_t acpi_ioapic_count(void)
{
    return ioapic_count;
}

const acpi_ioapic_info_t *acpi_ioapic(uint32_t index)
{
    if (index >= ioapic_count)
    {
        return NULL;
    }

    return &ioapics[index];
}

uint32_t acpi_iso_count(void)
{
    return override_count;
}

const acpi_iso_info_t *acpi_iso(uint32_t index)
{
    if (index >= override_count)
    {
        return NULL;
    }

    return &overrides[index];
}

bool acpi_resolve_isa_irq(
    uint8_t irq,
    uint32_t *gsi,
    bool *active_low,
    bool *level_triggered
)
{
    if (
        gsi == NULL ||
        active_low == NULL ||
        level_triggered == NULL
    )
    {
        return false;
    }

    *gsi = irq;
    *active_low = false;
    *level_triggered = false;

    for (
        uint32_t index = 0;
        index < override_count;
        index++
    )
    {
        if (overrides[index].source_irq == irq)
        {
            *gsi = overrides[index].gsi;
            *active_low = overrides[index].active_low;
            *level_triggered =
                overrides[index].level_triggered;

            break;
        }
    }

    return true;
}
