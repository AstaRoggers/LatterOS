#include "hardware_compat.h"

#include "acpi.h"
#include "ahci.h"
#include "block_device.h"
#include "display.h"
#include "network.h"
#include "nvme.h"
#include "pci.h"
#include "physical_memory.h"
#include "smp.h"
#include "usb.h"
#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HARDWARE_REPORT_PATH \
    "/home/user/Documents/Hardware Compatibility Report.txt"
#define HARDWARE_REPORT_CAPACITY 24576U
#define HARDWARE_MINIMUM_MEMORY_BYTES (128ULL * 1024ULL * 1024ULL)
#define HARDWARE_RECOMMENDED_MEMORY_BYTES (256ULL * 1024ULL * 1024ULL)

static hardware_compat_snapshot_t current_snapshot;
static bool initialized;
static char summary_text[128];
static char last_message[128];
static char report_buffer[HARDWARE_REPORT_CAPACITY];

static void clear_bytes(void *pointer, size_t count)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0; index < count; index++)
    {
        bytes[index] = 0;
    }
}

static void copy_text(
    char *destination,
    size_t capacity,
    const char *source
)
{
    if (destination == NULL || capacity == 0)
    {
        return;
    }

    size_t index = 0;

    if (source != NULL)
    {
        while (source[index] != '\0' && index + 1U < capacity)
        {
            destination[index] = source[index];
            index++;
        }
    }

    destination[index] = '\0';
}

static void append_character(
    char *buffer,
    size_t capacity,
    size_t *position,
    char character
)
{
    if (
        buffer == NULL ||
        position == NULL ||
        *position + 1U >= capacity
    )
    {
        return;
    }

    buffer[*position] = character;
    (*position)++;
    buffer[*position] = '\0';
}

static void append_text(
    char *buffer,
    size_t capacity,
    size_t *position,
    const char *text
)
{
    if (text == NULL)
    {
        return;
    }

    for (size_t index = 0; text[index] != '\0'; index++)
    {
        append_character(buffer, capacity, position, text[index]);
    }
}

static void append_unsigned(
    char *buffer,
    size_t capacity,
    size_t *position,
    uint64_t value
)
{
    char reverse[24];
    uint32_t count = 0;

    do
    {
        reverse[count++] = (char)('0' + value % 10ULL);
        value /= 10ULL;
    }
    while (value != 0 && count < sizeof(reverse));

    while (count > 0)
    {
        append_character(
            buffer,
            capacity,
            position,
            reverse[--count]
        );
    }
}

static void append_hexadecimal(
    char *buffer,
    size_t capacity,
    size_t *position,
    uint64_t value,
    uint32_t digits
)
{
    static const char hexadecimal[] = "0123456789ABCDEF";

    append_text(buffer, capacity, position, "0x");

    for (uint32_t index = 0; index < digits; index++)
    {
        uint32_t shift = (digits - index - 1U) * 4U;
        append_character(
            buffer,
            capacity,
            position,
            hexadecimal[(value >> shift) & 0x0FULL]
        );
    }
}

static void append_boolean(
    char *buffer,
    size_t capacity,
    size_t *position,
    bool value
)
{
    append_text(
        buffer,
        capacity,
        position,
        value ? "yes" : "no"
    );
}

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

static void trim_brand(char brand[49])
{
    uint32_t input = 0;
    uint32_t output = 0;
    bool previous_space = true;

    while (brand[input] != '\0')
    {
        char character = brand[input++];
        bool space = character == ' ' || character == '\t';

        if (space)
        {
            if (!previous_space && output + 1U < 49U)
            {
                brand[output++] = ' ';
            }
        }
        else if (output + 1U < 49U)
        {
            brand[output++] = character;
        }

        previous_space = space;
    }

    while (output > 0 && brand[output - 1U] == ' ')
    {
        output--;
    }

    brand[output] = '\0';
}

static void collect_cpu(hardware_compat_snapshot_t *snapshot)
{
    uint32_t maximum_basic;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;

    cpuid(0U, 0U, &maximum_basic, &ebx, &ecx, &edx);
    store_u32(&snapshot->cpu_vendor[0], ebx);
    store_u32(&snapshot->cpu_vendor[4], edx);
    store_u32(&snapshot->cpu_vendor[8], ecx);
    snapshot->cpu_vendor[12] = '\0';

    uint32_t maximum_extended;
    cpuid(0x80000000U, 0U, &maximum_extended, NULL, NULL, NULL);

    if (maximum_basic >= 1U)
    {
        uint32_t eax;
        cpuid(1U, 0U, &eax, &ebx, &ecx, &edx);

        uint32_t base_family = (eax >> 8) & 0xFU;
        uint32_t base_model = (eax >> 4) & 0xFU;
        uint32_t extended_family = (eax >> 20) & 0xFFU;
        uint32_t extended_model = (eax >> 16) & 0xFU;

        snapshot->cpu_family = base_family;

        if (base_family == 0xFU)
        {
            snapshot->cpu_family += extended_family;
        }

        snapshot->cpu_model = base_model;

        if (base_family == 0x6U || base_family == 0xFU)
        {
            snapshot->cpu_model += extended_model << 4;
        }

        snapshot->cpu_stepping = eax & 0xFU;
        snapshot->tsc = (edx & (1U << 4)) != 0;
        snapshot->apic = (edx & (1U << 9)) != 0;
        snapshot->sse2 = (edx & (1U << 26)) != 0;
        snapshot->x2apic = (ecx & (1U << 21)) != 0;
    }

    if (maximum_extended >= 0x80000001U)
    {
        cpuid(0x80000001U, 0U, NULL, NULL, NULL, &edx);
        snapshot->nx = (edx & (1U << 20)) != 0;
        snapshot->long_mode = (edx & (1U << 29)) != 0;
    }

    if (maximum_extended >= 0x80000007U)
    {
        cpuid(0x80000007U, 0U, NULL, NULL, NULL, &edx);
        snapshot->invariant_tsc = (edx & (1U << 8)) != 0;
    }

    if (maximum_extended >= 0x80000004U)
    {
        for (uint32_t index = 0; index < 3U; index++)
        {
            uint32_t eax;
            cpuid(
                0x80000002U + index,
                0U,
                &eax,
                &ebx,
                &ecx,
                &edx
            );

            char *destination =
                &snapshot->cpu_brand[index * 16U];
            store_u32(destination + 0, eax);
            store_u32(destination + 4, ebx);
            store_u32(destination + 8, ecx);
            store_u32(destination + 12, edx);
        }

        snapshot->cpu_brand[48] = '\0';
        trim_brand(snapshot->cpu_brand);
    }

    if (snapshot->cpu_brand[0] == '\0')
    {
        copy_text(
            snapshot->cpu_brand,
            sizeof(snapshot->cpu_brand),
            snapshot->cpu_vendor
        );
    }
}

static const char *pci_class_name(uint8_t class_code)
{
    switch (class_code)
    {
        case 0x01U:
            return "Mass storage";

        case 0x02U:
            return "Network";

        case 0x03U:
            return "Display";

        case 0x04U:
            return "Multimedia";

        case 0x06U:
            return "Bridge";

        case 0x0CU:
            return "Serial bus";

        default:
            return "Other";
    }
}

static void collect_pci(hardware_compat_snapshot_t *snapshot)
{
    snapshot->pci_devices = pci_device_count();

    for (uint32_t index = 0; index < snapshot->pci_devices; index++)
    {
        const pci_device_t *device = pci_get_device(index);

        if (device == NULL)
        {
            continue;
        }

        switch (device->class_code)
        {
            case 0x01U:
                snapshot->pci_storage++;
                break;

            case 0x02U:
                snapshot->pci_network++;
                break;

            case 0x03U:
                snapshot->pci_display++;
                break;

            case 0x06U:
                snapshot->pci_bridges++;
                break;

            case 0x0CU:
                if (device->subclass == 0x03U)
                {
                    snapshot->pci_usb++;
                }
                break;

            default:
                break;
        }
    }
}

static void collect_storage(hardware_compat_snapshot_t *snapshot)
{
    uint32_t slots = block_device_count();

    for (uint32_t index = 0; index < slots; index++)
    {
        const block_device_t *device = block_device_get(index);

        if (device == NULL)
        {
            continue;
        }

        snapshot->block_devices++;

        if (device->writable)
        {
            snapshot->writable_block_devices++;
        }

        if (device->removable)
        {
            snapshot->removable_block_devices++;
        }
    }

    snapshot->ahci_devices = ahci_device_count();
    snapshot->nvme_namespaces = nvme_namespace_count();
}

static void evaluate_status(hardware_compat_snapshot_t *snapshot)
{
    snapshot->warning_count = 0;
    snapshot->failure_count = 0;

    if (!snapshot->long_mode)
    {
        snapshot->failure_count++;
    }

    if (!snapshot->apic)
    {
        snapshot->failure_count++;
    }

    if (snapshot->total_memory_bytes < HARDWARE_MINIMUM_MEMORY_BYTES)
    {
        snapshot->failure_count++;
    }

    if (snapshot->block_devices == 0)
    {
        snapshot->failure_count++;
    }

    if (!snapshot->acpi_available)
    {
        snapshot->warning_count++;
    }

    if (snapshot->acpi_available && snapshot->acpi_ioapics == 0)
    {
        snapshot->warning_count++;
    }

    if (snapshot->usable_memory_bytes < HARDWARE_RECOMMENDED_MEMORY_BYTES)
    {
        snapshot->warning_count++;
    }

    if (
        snapshot->detected_cpus > 1U &&
        snapshot->online_cpus < snapshot->detected_cpus
    )
    {
        snapshot->warning_count++;
    }

    if (!snapshot->invariant_tsc)
    {
        snapshot->warning_count++;
    }

    if (snapshot->failure_count != 0)
    {
        snapshot->status = HARDWARE_COMPAT_UNSUPPORTED;
        copy_text(
            summary_text,
            sizeof(summary_text),
            "Critical compatibility requirements are missing"
        );
    }
    else if (snapshot->warning_count != 0)
    {
        snapshot->status = HARDWARE_COMPAT_WARNING;
        copy_text(
            summary_text,
            sizeof(summary_text),
            "Booted successfully with compatibility warnings"
        );
    }
    else
    {
        snapshot->status = HARDWARE_COMPAT_READY;
        copy_text(
            summary_text,
            sizeof(summary_text),
            "Core hardware compatibility checks passed"
        );
    }
}

void hardware_compat_refresh(void)
{
    uint32_t next_generation = current_snapshot.generation + 1U;
    clear_bytes(&current_snapshot, sizeof(current_snapshot));
    current_snapshot.generation = next_generation;

    collect_cpu(&current_snapshot);

    current_snapshot.detected_cpus = smp_cpu_count();
    current_snapshot.online_cpus = smp_online_count();
    current_snapshot.total_memory_bytes = physical_memory_total_bytes();
    current_snapshot.usable_memory_bytes = physical_memory_usable_bytes();

    current_snapshot.acpi_available = acpi_is_available();
    copy_text(
        current_snapshot.acpi_root,
        sizeof(current_snapshot.acpi_root),
        current_snapshot.acpi_available ? acpi_root_table_name() : "none"
    );
    copy_text(
        current_snapshot.acpi_oem,
        sizeof(current_snapshot.acpi_oem),
        current_snapshot.acpi_available ? acpi_oem_id() : "none"
    );
    current_snapshot.acpi_cpus = acpi_cpu_count();
    current_snapshot.acpi_ioapics = acpi_ioapic_count();
    current_snapshot.acpi_overrides = acpi_iso_count();

    collect_pci(&current_snapshot);
    collect_storage(&current_snapshot);

    current_snapshot.usb_ready = usb_is_ready();
    current_snapshot.usb_devices = usb_device_count();
    current_snapshot.usb_keyboard = usb_keyboard_ready();
    current_snapshot.usb_mouse = usb_mouse_ready();

    current_snapshot.network_ready = network_is_ready();
    current_snapshot.network_link = network_link_up();

    copy_text(
        current_snapshot.display_backend,
        sizeof(current_snapshot.display_backend),
        display_backend_name()
    );

    display_stats_t display_statistics;
    clear_bytes(&display_statistics, sizeof(display_statistics));
    display_get_stats(&display_statistics);
    current_snapshot.display_width = display_statistics.width;
    current_snapshot.display_height = display_statistics.height;
    current_snapshot.display_refresh_hz = display_statistics.refresh_hz;

    evaluate_status(&current_snapshot);
    copy_text(last_message, sizeof(last_message), "Hardware profile refreshed");
}

void hardware_compat_init(void)
{
    if (initialized)
    {
        return;
    }

    initialized = true;
    clear_bytes(&current_snapshot, sizeof(current_snapshot));
    copy_text(summary_text, sizeof(summary_text), "Not evaluated");
    copy_text(last_message, sizeof(last_message), "Hardware profile not yet collected");
    hardware_compat_refresh();
}

const hardware_compat_snapshot_t *hardware_compat_snapshot(void)
{
    hardware_compat_init();
    return &current_snapshot;
}

hardware_compat_status_t hardware_compat_status(void)
{
    return hardware_compat_snapshot()->status;
}

const char *hardware_compat_status_name(void)
{
    switch (hardware_compat_status())
    {
        case HARDWARE_COMPAT_READY:
            return "READY";

        case HARDWARE_COMPAT_WARNING:
            return "WARNING";

        case HARDWARE_COMPAT_UNSUPPORTED:
            return "UNSUPPORTED";

        case HARDWARE_COMPAT_UNKNOWN:
        default:
            return "UNKNOWN";
    }
}

const char *hardware_compat_summary(void)
{
    hardware_compat_init();
    return summary_text;
}

const char *hardware_compat_last_message(void)
{
    hardware_compat_init();
    return last_message;
}

bool hardware_compat_run_quick_test(void)
{
    hardware_compat_refresh();

    bool passed =
        current_snapshot.failure_count == 0 &&
        current_snapshot.pci_devices > 0 &&
        current_snapshot.online_cpus > 0;

    copy_text(
        last_message,
        sizeof(last_message),
        passed ?
            "Quick compatibility test passed" :
            "Quick compatibility test found a critical issue"
    );

    return passed;
}

static void append_report_header(size_t *position)
{
    append_text(report_buffer, sizeof(report_buffer), position, "LatterOS Hardware Compatibility Report\n");
    append_text(report_buffer, sizeof(report_buffer), position, "Milestone 20A\n\n");
    append_text(report_buffer, sizeof(report_buffer), position, "Status: ");
    append_text(report_buffer, sizeof(report_buffer), position, hardware_compat_status_name());
    append_text(report_buffer, sizeof(report_buffer), position, "\nSummary: ");
    append_text(report_buffer, sizeof(report_buffer), position, hardware_compat_summary());
    append_text(report_buffer, sizeof(report_buffer), position, "\nWarnings: ");
    append_unsigned(report_buffer, sizeof(report_buffer), position, current_snapshot.warning_count);
    append_text(report_buffer, sizeof(report_buffer), position, "\nFailures: ");
    append_unsigned(report_buffer, sizeof(report_buffer), position, current_snapshot.failure_count);
    append_text(report_buffer, sizeof(report_buffer), position, "\n\n");
}

static void append_report_cpu(size_t *position)
{
    append_text(report_buffer, sizeof(report_buffer), position, "CPU\n---\nVendor: ");
    append_text(report_buffer, sizeof(report_buffer), position, current_snapshot.cpu_vendor);
    append_text(report_buffer, sizeof(report_buffer), position, "\nModel: ");
    append_text(report_buffer, sizeof(report_buffer), position, current_snapshot.cpu_brand);
    append_text(report_buffer, sizeof(report_buffer), position, "\nFamily/model/stepping: ");
    append_unsigned(report_buffer, sizeof(report_buffer), position, current_snapshot.cpu_family);
    append_character(report_buffer, sizeof(report_buffer), position, '/');
    append_unsigned(report_buffer, sizeof(report_buffer), position, current_snapshot.cpu_model);
    append_character(report_buffer, sizeof(report_buffer), position, '/');
    append_unsigned(report_buffer, sizeof(report_buffer), position, current_snapshot.cpu_stepping);
    append_text(report_buffer, sizeof(report_buffer), position, "\nDetected CPUs: ");
    append_unsigned(report_buffer, sizeof(report_buffer), position, current_snapshot.detected_cpus);
    append_text(report_buffer, sizeof(report_buffer), position, "\nOnline CPUs: ");
    append_unsigned(report_buffer, sizeof(report_buffer), position, current_snapshot.online_cpus);
    append_text(report_buffer, sizeof(report_buffer), position, "\nLong mode: ");
    append_boolean(report_buffer, sizeof(report_buffer), position, current_snapshot.long_mode);
    append_text(report_buffer, sizeof(report_buffer), position, "\nNX: ");
    append_boolean(report_buffer, sizeof(report_buffer), position, current_snapshot.nx);
    append_text(report_buffer, sizeof(report_buffer), position, "\nAPIC: ");
    append_boolean(report_buffer, sizeof(report_buffer), position, current_snapshot.apic);
    append_text(report_buffer, sizeof(report_buffer), position, "\nx2APIC: ");
    append_boolean(report_buffer, sizeof(report_buffer), position, current_snapshot.x2apic);
    append_text(report_buffer, sizeof(report_buffer), position, "\nTSC: ");
    append_boolean(report_buffer, sizeof(report_buffer), position, current_snapshot.tsc);
    append_text(report_buffer, sizeof(report_buffer), position, "\nInvariant TSC: ");
    append_boolean(report_buffer, sizeof(report_buffer), position, current_snapshot.invariant_tsc);
    append_text(report_buffer, sizeof(report_buffer), position, "\nSSE2: ");
    append_boolean(report_buffer, sizeof(report_buffer), position, current_snapshot.sse2);
    append_text(report_buffer, sizeof(report_buffer), position, "\n\n");
}

static void append_report_platform(size_t *position)
{
    append_text(report_buffer, sizeof(report_buffer), position, "Platform\n--------\nTotal memory MiB: ");
    append_unsigned(report_buffer, sizeof(report_buffer), position, current_snapshot.total_memory_bytes / (1024ULL * 1024ULL));
    append_text(report_buffer, sizeof(report_buffer), position, "\nUsable memory MiB: ");
    append_unsigned(report_buffer, sizeof(report_buffer), position, current_snapshot.usable_memory_bytes / (1024ULL * 1024ULL));
    append_text(report_buffer, sizeof(report_buffer), position, "\nACPI: ");
    append_boolean(report_buffer, sizeof(report_buffer), position, current_snapshot.acpi_available);
    append_text(report_buffer, sizeof(report_buffer), position, "\nACPI root: ");
    append_text(report_buffer, sizeof(report_buffer), position, current_snapshot.acpi_root);
    append_text(report_buffer, sizeof(report_buffer), position, "\nACPI OEM: ");
    append_text(report_buffer, sizeof(report_buffer), position, current_snapshot.acpi_oem);
    append_text(report_buffer, sizeof(report_buffer), position, "\nACPI CPUs: ");
    append_unsigned(report_buffer, sizeof(report_buffer), position, current_snapshot.acpi_cpus);
    append_text(report_buffer, sizeof(report_buffer), position, "\nI/O APICs: ");
    append_unsigned(report_buffer, sizeof(report_buffer), position, current_snapshot.acpi_ioapics);
    append_text(report_buffer, sizeof(report_buffer), position, "\nIRQ overrides: ");
    append_unsigned(report_buffer, sizeof(report_buffer), position, current_snapshot.acpi_overrides);
    append_text(report_buffer, sizeof(report_buffer), position, "\n\n");
}

static void append_report_devices(size_t *position)
{
    append_text(report_buffer, sizeof(report_buffer), position, "Devices\n-------\nPCI devices: ");
    append_unsigned(report_buffer, sizeof(report_buffer), position, current_snapshot.pci_devices);
    append_text(report_buffer, sizeof(report_buffer), position, "\nPCI storage/network/display/USB/bridges: ");
    append_unsigned(report_buffer, sizeof(report_buffer), position, current_snapshot.pci_storage);
    append_character(report_buffer, sizeof(report_buffer), position, '/');
    append_unsigned(report_buffer, sizeof(report_buffer), position, current_snapshot.pci_network);
    append_character(report_buffer, sizeof(report_buffer), position, '/');
    append_unsigned(report_buffer, sizeof(report_buffer), position, current_snapshot.pci_display);
    append_character(report_buffer, sizeof(report_buffer), position, '/');
    append_unsigned(report_buffer, sizeof(report_buffer), position, current_snapshot.pci_usb);
    append_character(report_buffer, sizeof(report_buffer), position, '/');
    append_unsigned(report_buffer, sizeof(report_buffer), position, current_snapshot.pci_bridges);
    append_text(report_buffer, sizeof(report_buffer), position, "\nOnline block devices: ");
    append_unsigned(report_buffer, sizeof(report_buffer), position, current_snapshot.block_devices);
    append_text(report_buffer, sizeof(report_buffer), position, "\nWritable block devices: ");
    append_unsigned(report_buffer, sizeof(report_buffer), position, current_snapshot.writable_block_devices);
    append_text(report_buffer, sizeof(report_buffer), position, "\nRemovable block devices: ");
    append_unsigned(report_buffer, sizeof(report_buffer), position, current_snapshot.removable_block_devices);
    append_text(report_buffer, sizeof(report_buffer), position, "\nAHCI devices: ");
    append_unsigned(report_buffer, sizeof(report_buffer), position, current_snapshot.ahci_devices);
    append_text(report_buffer, sizeof(report_buffer), position, "\nNVMe namespaces: ");
    append_unsigned(report_buffer, sizeof(report_buffer), position, current_snapshot.nvme_namespaces);
    append_text(report_buffer, sizeof(report_buffer), position, "\nUSB controller ready: ");
    append_boolean(report_buffer, sizeof(report_buffer), position, current_snapshot.usb_ready);
    append_text(report_buffer, sizeof(report_buffer), position, "\nUSB devices: ");
    append_unsigned(report_buffer, sizeof(report_buffer), position, current_snapshot.usb_devices);
    append_text(report_buffer, sizeof(report_buffer), position, "\nUSB keyboard/mouse: ");
    append_boolean(report_buffer, sizeof(report_buffer), position, current_snapshot.usb_keyboard);
    append_character(report_buffer, sizeof(report_buffer), position, '/');
    append_boolean(report_buffer, sizeof(report_buffer), position, current_snapshot.usb_mouse);
    append_text(report_buffer, sizeof(report_buffer), position, "\nNetwork ready/link: ");
    append_boolean(report_buffer, sizeof(report_buffer), position, current_snapshot.network_ready);
    append_character(report_buffer, sizeof(report_buffer), position, '/');
    append_boolean(report_buffer, sizeof(report_buffer), position, current_snapshot.network_link);
    append_text(report_buffer, sizeof(report_buffer), position, "\nDisplay: ");
    append_text(report_buffer, sizeof(report_buffer), position, current_snapshot.display_backend);
    append_character(report_buffer, sizeof(report_buffer), position, ' ');
    append_unsigned(report_buffer, sizeof(report_buffer), position, current_snapshot.display_width);
    append_character(report_buffer, sizeof(report_buffer), position, 'x');
    append_unsigned(report_buffer, sizeof(report_buffer), position, current_snapshot.display_height);
    append_character(report_buffer, sizeof(report_buffer), position, '@');
    append_unsigned(report_buffer, sizeof(report_buffer), position, current_snapshot.display_refresh_hz);
    append_text(report_buffer, sizeof(report_buffer), position, "Hz\n\n");
}

static void append_report_pci_inventory(size_t *position)
{
    append_text(report_buffer, sizeof(report_buffer), position, "PCI inventory\n-------------\n");

    uint32_t count = current_snapshot.pci_devices;
    uint32_t visible = count > 64U ? 64U : count;

    for (uint32_t index = 0; index < visible; index++)
    {
        const pci_device_t *device = pci_get_device(index);

        if (device == NULL)
        {
            continue;
        }

        append_unsigned(report_buffer, sizeof(report_buffer), position, index);
        append_text(report_buffer, sizeof(report_buffer), position, ": ");
        append_unsigned(report_buffer, sizeof(report_buffer), position, device->bus);
        append_character(report_buffer, sizeof(report_buffer), position, ':');
        append_unsigned(report_buffer, sizeof(report_buffer), position, device->device);
        append_character(report_buffer, sizeof(report_buffer), position, '.');
        append_unsigned(report_buffer, sizeof(report_buffer), position, device->function);
        append_text(report_buffer, sizeof(report_buffer), position, " vendor=");
        append_hexadecimal(report_buffer, sizeof(report_buffer), position, device->vendor_id, 4U);
        append_text(report_buffer, sizeof(report_buffer), position, " device=");
        append_hexadecimal(report_buffer, sizeof(report_buffer), position, device->device_id, 4U);
        append_text(report_buffer, sizeof(report_buffer), position, " class=");
        append_hexadecimal(report_buffer, sizeof(report_buffer), position, device->class_code, 2U);
        append_character(report_buffer, sizeof(report_buffer), position, '/');
        append_hexadecimal(report_buffer, sizeof(report_buffer), position, device->subclass, 2U);
        append_character(report_buffer, sizeof(report_buffer), position, ' ');
        append_text(report_buffer, sizeof(report_buffer), position, pci_class_name(device->class_code));
        append_character(report_buffer, sizeof(report_buffer), position, '\n');
    }

    if (count > visible)
    {
        append_text(report_buffer, sizeof(report_buffer), position, "Additional PCI devices omitted from this report.\n");
    }
}

bool hardware_compat_write_report(void)
{
    hardware_compat_refresh();
    clear_bytes(report_buffer, sizeof(report_buffer));
    size_t position = 0;

    append_report_header(&position);
    append_report_cpu(&position);
    append_report_platform(&position);
    append_report_devices(&position);
    append_report_pci_inventory(&position);

    bool saved = vfs_write_text(HARDWARE_REPORT_PATH, report_buffer);

    copy_text(
        last_message,
        sizeof(last_message),
        saved ?
            "Hardware report saved in Documents" :
            "Unable to save hardware report"
    );

    return saved;
}

const char *hardware_compat_report_path(void)
{
    return HARDWARE_REPORT_PATH;
}
