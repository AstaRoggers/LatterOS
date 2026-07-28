#include "installer.h"

#include "block_device.h"
#include "fat_fs.h"
#include "installer_payload.h"
#include "smp_scheduler.h"
#include "timer.h"
#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INSTALLER_REQUIRED_SECTOR_SIZE 512U
#define INSTALLER_MINIMUM_MIB 384ULL
#define INSTALLER_ALIGNMENT_SECTORS 2048ULL
#define INSTALLER_ESP_MIB 96ULL
#define INSTALLER_ESP_SECTORS \
    (INSTALLER_ESP_MIB * 1024ULL * 1024ULL / 512ULL)

#define GPT_ENTRY_COUNT 128U
#define GPT_ENTRY_SIZE 128U
#define GPT_ENTRY_BYTES (GPT_ENTRY_COUNT * GPT_ENTRY_SIZE)
#define GPT_ENTRY_SECTORS (GPT_ENTRY_BYTES / 512U)
#define GPT_HEADER_SIZE 92U

#define FAT_RESERVED_SECTORS 32U
#define FAT_COUNT 2U
#define FAT_ROOT_CLUSTER 2U
#define FAT_MARKER_CLUSTER 3U
#define FAT_SECTORS_PER_CLUSTER 1U
#define FAT32_EOC 0x0FFFFFFFU

#define ZERO_BATCH_SECTORS 128U
#define INSTALLER_COPY_CHUNK 32768U
#define INSTALLER_PARTITION_WRITE_SECTORS 128U
#define INSTALLER_PARTITION_CACHE_SECTORS 384U
#define INSTALLER_PARTITION_PREFIX "LatterOS install partition"

#define INSTALLER_ESP_MOUNT "/media/install-efi"
#define INSTALLER_SYSTEM_MOUNT "/media/install-system"

typedef struct
{
    bool valid;
    bool dirty;
    uint64_t lba;
    uint64_t access_sequence;
    uint8_t data[INSTALLER_REQUIRED_SECTOR_SIZE];
} installer_partition_cache_entry_t;

typedef struct
{
    const block_device_t *parent;
    uint64_t first_lba;
    uint64_t sector_count;

    uint64_t cache_sequence;
    uint32_t cache_used;
    installer_partition_cache_entry_t cache[
        INSTALLER_PARTITION_CACHE_SECTORS
    ];
    uint8_t write_buffer[
        INSTALLER_PARTITION_WRITE_SECTORS *
        INSTALLER_REQUIRED_SECTOR_SIZE
    ];
} installer_partition_context_t;


typedef enum
{
    INSTALLER_ASYNC_IDLE,
    INSTALLER_ASYNC_QUEUED,
    INSTALLER_ASYNC_RUNNING,
    INSTALLER_ASYNC_COMPLETE
} installer_async_state_t;

typedef enum
{
    INSTALLER_PREVIOUS_MOUNT_NONE,
    INSTALLER_PREVIOUS_MOUNT_USB,
    INSTALLER_PREVIOUS_MOUNT_SATA,
    INSTALLER_PREVIOUS_MOUNT_NVME,
    INSTALLER_PREVIOUS_MOUNT_OTHER
} installer_previous_mount_t;

static uint32_t target_devices[INSTALLER_MAX_TARGETS];
static uint32_t target_count;
static char last_error[INSTALLER_MESSAGE_CAPACITY];

static volatile uint32_t async_state;
static uint32_t async_target_index;
static uint32_t async_worker_cpu;
static uint64_t async_job_id;
static volatile bool async_worker_context;
static installer_report_t async_report;
static volatile uint32_t progress_sequence;
static uint32_t progress_percentage;
static char progress_message[INSTALLER_MESSAGE_CAPACITY];

static uint8_t sector_buffer[INSTALLER_REQUIRED_SECTOR_SIZE];
static uint8_t verify_buffer[INSTALLER_REQUIRED_SECTOR_SIZE];
static uint8_t zero_buffer[
    INSTALLER_REQUIRED_SECTOR_SIZE * ZERO_BATCH_SECTORS
];
static uint8_t gpt_entries[GPT_ENTRY_BYTES];
static uint8_t payload_verify_buffer[INSTALLER_COPY_CHUNK];

static installer_partition_context_t efi_partition_context;
static installer_partition_context_t system_partition_context;

static const char efi_partition_name[] =
    "LatterOS install partition EFI";
static const char system_partition_name[] =
    "LatterOS install partition system";

static const char installed_limine_configuration[] =
    "timeout: 3\n"
    "default_entry: 1\n"
    "interface_resolution: 1280x800\n"
    "interface_branding: LatterOS\n"
    "\n"
    "/LatterOS\n"
    "    protocol: limine\n"
    "    path: boot():/boot/kernel\n"
    "    cmdline: source=installed mode=normal\n"
    "    module_path: boot():/boot/kernel\n"
    "    module_string: latteros-installer-kernel\n"
    "    module_path: boot():/EFI/BOOT/BOOTX64.EFI\n"
    "    module_string: latteros-installer-bootx64\n"
    "    resolution: 1280x800x32\n"
    "\n"
    "/LatterOS Safe Mode\n"
    "    protocol: limine\n"
    "    path: boot():/boot/kernel\n"
    "    cmdline: source=installed mode=safe\n"
    "    module_path: boot():/boot/kernel\n"
    "    module_string: latteros-installer-kernel\n"
    "    module_path: boot():/EFI/BOOT/BOOTX64.EFI\n"
    "    module_string: latteros-installer-bootx64\n"
    "    resolution: 1280x800x32\n"
    "\n"
    "/LatterOS Recovery\n"
    "    protocol: limine\n"
    "    path: boot():/boot/kernel\n"
    "    cmdline: source=installed mode=recovery\n"
    "    module_path: boot():/boot/kernel\n"
    "    module_string: latteros-installer-kernel\n"
    "    module_path: boot():/EFI/BOOT/BOOTX64.EFI\n"
    "    module_string: latteros-installer-bootx64\n"
    "    resolution: 1280x800x32\n"
    "\n"
    "/LatterOS Hardware Test\n"
    "    protocol: limine\n"
    "    path: boot():/boot/kernel\n"
    "    cmdline: source=installed mode=hardware\n"
    "    module_path: boot():/boot/kernel\n"
    "    module_string: latteros-installer-kernel\n"
    "    module_path: boot():/EFI/BOOT/BOOTX64.EFI\n"
    "    module_string: latteros-installer-bootx64\n"
    "    resolution: 1280x800x32\n"
    "\n"
    "/LatterOS Compatibility Mode\n"
    "    protocol: limine\n"
    "    path: boot():/boot/kernel\n"
    "    cmdline: source=installed mode=compatibility\n"
    "    module_path: boot():/boot/kernel\n"
    "    module_string: latteros-installer-kernel\n"
    "    module_path: boot():/EFI/BOOT/BOOTX64.EFI\n"
    "    module_string: latteros-installer-bootx64\n"
    "    resolution: 1280x800x32\n"
    "\n"
    "/LatterOS VirtualBox Mode\n"
    "    protocol: limine\n"
    "    path: boot():/boot/kernel\n"
    "    cmdline: source=installed mode=virtualbox\n"
    "    module_path: boot():/boot/kernel\n"
    "    module_string: latteros-installer-kernel\n"
    "    module_path: boot():/EFI/BOOT/BOOTX64.EFI\n"
    "    module_string: latteros-installer-bootx64\n"
    "    resolution: 1280x800x32\n";

static const char installation_marker[] =
    "LatterOS 0.20.3-rc1 installation complete.\r\n"
    "UEFI fallback loader: /EFI/BOOT/BOOTX64.EFI\r\n"
    "Kernel: /boot/kernel\r\n"
    "Configuration: /boot/limine/limine.conf\r\n";

static const char installation_version[] =
    "name=LatterOS\r\n"
    "version=0.20.3-rc1\r\n"
    "milestone=20D\r\n"
    "channel=rc\r\n"
    "boot=Limine UEFI\r\n"
    "architecture=x86_64\r\n"
    "package-format=LPKGv1\r\n";

static void clear_bytes(void *pointer, size_t count)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0; index < count; index++)
    {
        bytes[index] = 0;
    }
}

static void copy_bytes(
    void *destination,
    const void *source,
    size_t count
)
{
    uint8_t *output = destination;
    const uint8_t *input = source;

    for (size_t index = 0; index < count; index++)
    {
        output[index] = input[index];
    }
}

static size_t string_length(const char *text)
{
    size_t length = 0;

    if (text == NULL)
    {
        return 0;
    }

    while (text[length] != '\0')
    {
        length++;
    }

    return length;
}

static bool strings_equal(
    const char *first,
    const char *second
)
{
    if (first == NULL || second == NULL)
    {
        return false;
    }

    size_t index = 0;

    while (
        first[index] != '\0' &&
        second[index] != '\0'
    )
    {
        if (first[index] != second[index])
        {
            return false;
        }

        index++;
    }

    return first[index] == second[index];
}

static bool strings_contain(
    const char *text,
    const char *needle
)
{
    if (text == NULL || needle == NULL || needle[0] == '\0')
    {
        return false;
    }

    size_t text_length = string_length(text);
    size_t needle_length = string_length(needle);

    if (needle_length > text_length)
    {
        return false;
    }

    for (
        size_t start = 0;
        start + needle_length <= text_length;
        start++
    )
    {
        bool matched = true;

        for (size_t index = 0; index < needle_length; index++)
        {
            char first = text[start + index];
            char second = needle[index];

            if (first >= 'A' && first <= 'Z')
            {
                first = (char)(first - 'A' + 'a');
            }

            if (second >= 'A' && second <= 'Z')
            {
                second = (char)(second - 'A' + 'a');
            }

            if (first != second)
            {
                matched = false;
                break;
            }
        }

        if (matched)
        {
            return true;
        }
    }

    return false;
}

static void installer_cooperate(void)
{
    if (__atomic_load_n(
            &async_worker_context,
            __ATOMIC_ACQUIRE
        ))
    {
        smp_scheduler_yield();
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
        while (
            source[index] != '\0' &&
            index + 1U < capacity
        )
        {
            destination[index] = source[index];
            index++;
        }
    }

    destination[index] = '\0';
}

static void update_progress(
    uint32_t percentage,
    const char *message
)
{
    if (percentage > 100U)
    {
        percentage = 100U;
    }

    (void)__atomic_add_fetch(
        &progress_sequence,
        1U,
        __ATOMIC_ACQ_REL
    );

    progress_percentage = percentage;
    copy_text(
        progress_message,
        sizeof(progress_message),
        message
    );

    (void)__atomic_add_fetch(
        &progress_sequence,
        1U,
        __ATOMIC_RELEASE
    );
}

static void set_error(const char *message)
{
    copy_text(last_error, sizeof(last_error), message);
}

static void set_report(
    installer_report_t *report,
    bool success,
    const char *message
)
{
    if (report == NULL)
    {
        return;
    }

    report->success = success;
    copy_text(report->message, sizeof(report->message), message);
}

static void write_u16(uint8_t *buffer, size_t offset, uint16_t value)
{
    buffer[offset] = (uint8_t)(value & 0xFFU);
    buffer[offset + 1U] = (uint8_t)(value >> 8);
}

static void write_u32(uint8_t *buffer, size_t offset, uint32_t value)
{
    buffer[offset] = (uint8_t)(value & 0xFFU);
    buffer[offset + 1U] = (uint8_t)((value >> 8) & 0xFFU);
    buffer[offset + 2U] = (uint8_t)((value >> 16) & 0xFFU);
    buffer[offset + 3U] = (uint8_t)((value >> 24) & 0xFFU);
}

static void write_u64(uint8_t *buffer, size_t offset, uint64_t value)
{
    write_u32(buffer, offset, (uint32_t)value);
    write_u32(buffer, offset + 4U, (uint32_t)(value >> 32));
}

static uint32_t read_u32(const uint8_t *buffer, size_t offset)
{
    return
        (uint32_t)buffer[offset] |
        ((uint32_t)buffer[offset + 1U] << 8) |
        ((uint32_t)buffer[offset + 2U] << 16) |
        ((uint32_t)buffer[offset + 3U] << 24);
}

static uint32_t crc32_bytes(const void *data, size_t count)
{
    const uint8_t *bytes = data;
    uint32_t crc = 0xFFFFFFFFU;

    for (size_t index = 0; index < count; index++)
    {
        crc ^= bytes[index];

        for (uint8_t bit = 0; bit < 8U; bit++)
        {
            uint32_t mask =
                (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }

    return ~crc;
}

static uint64_t align_up(uint64_t value, uint64_t alignment)
{
    return
        (value + alignment - 1ULL) /
        alignment * alignment;
}

static bool valid_raw_target(const block_device_t *device)
{
    if (
        device == NULL ||
        !device->online ||
        !device->writable ||
        device->removable ||
        device->sector_size != INSTALLER_REQUIRED_SECTOR_SIZE ||
        device->sector_count <
            INSTALLER_MINIMUM_MIB * 1024ULL * 1024ULL /
                INSTALLER_REQUIRED_SECTOR_SIZE ||
        device->read == NULL ||
        device->write == NULL
    )
    {
        return false;
    }

    if (strings_contain(device->name, "partition"))
    {
        return false;
    }

    return true;
}

void installer_refresh(void)
{
    if (installer_running())
    {
        set_error("Installation is already in progress");
        return;
    }

    target_count = 0;

    if (!installer_payload_available())
    {
        set_error(
            "Installer payload missing: boot the installation ISO"
        );
        return;
    }

    uint32_t count = block_device_count();

    for (
        uint32_t index = 0;
        index < count && target_count < INSTALLER_MAX_TARGETS;
        index++
    )
    {
        const block_device_t *device = block_device_get(index);

        if (!valid_raw_target(device))
        {
            continue;
        }

        target_devices[target_count++] = index;
    }

    if (target_count == 0)
    {
        set_error(
            "No writable non-removable 512-byte disk of at least 384 MiB"
        );
    }
    else
    {
        set_error("Ready");
    }
}

void installer_init(void)
{
    clear_bytes(target_devices, sizeof(target_devices));
    clear_bytes(zero_buffer, sizeof(zero_buffer));
    clear_bytes(&async_report, sizeof(async_report));
    async_target_index = UINT32_MAX;
    progress_sequence = 0;
    progress_percentage = 0;
    copy_text(
        progress_message,
        sizeof(progress_message),
        "Idle"
    );
    __atomic_store_n(
        &async_state,
        INSTALLER_ASYNC_IDLE,
        __ATOMIC_RELEASE
    );
    set_error("Not scanned");
    installer_refresh();
}

uint32_t installer_target_count(void)
{
    return target_count;
}

bool installer_target_get(
    uint32_t target_index,
    installer_target_info_t *information
)
{
    if (
        information == NULL ||
        target_index >= target_count
    )
    {
        return false;
    }

    uint32_t device_index = target_devices[target_index];
    const block_device_t *device = block_device_get(device_index);

    if (!valid_raw_target(device))
    {
        return false;
    }

    information->target_index = target_index;
    information->device_index = device_index;
    information->sector_size = device->sector_size;
    information->sector_count = device->sector_count;
    information->capacity_mib =
        device->sector_count * device->sector_size /
        (1024ULL * 1024ULL);

    copy_text(
        information->name,
        sizeof(information->name),
        device->name != NULL ? device->name : "Unnamed disk"
    );

    return true;
}

static void make_guid(
    uint8_t guid[16],
    uint64_t seed,
    uint32_t discriminator
)
{
    uint64_t first =
        seed ^
        (0x9E3779B97F4A7C15ULL *
            (uint64_t)(discriminator + 1U));

    uint64_t second =
        (seed << 17) ^
        (seed >> 11) ^
        0xD1B54A32D192ED03ULL ^
        discriminator;

    for (uint32_t index = 0; index < 8U; index++)
    {
        guid[index] = (uint8_t)(first >> (index * 8U));
        guid[index + 8U] =
            (uint8_t)(second >> (index * 8U));
    }

    guid[7] = (uint8_t)((guid[7] & 0x0FU) | 0x40U);
    guid[8] = (uint8_t)((guid[8] & 0x3FU) | 0x80U);
}

static void copy_guid(uint8_t *destination, const uint8_t source[16])
{
    for (uint32_t index = 0; index < 16U; index++)
    {
        destination[index] = source[index];
    }
}

static void write_partition_name(
    uint8_t *entry,
    const char *name
)
{
    for (uint32_t index = 0; index < 36U; index++)
    {
        uint16_t character = 0;

        if (name != NULL && name[index] != '\0')
        {
            character = (uint8_t)name[index];
        }

        write_u16(entry, 56U + index * 2U, character);

        if (character == 0)
        {
            break;
        }
    }
}

static void build_partition_entry(
    uint8_t *entry,
    const uint8_t type_guid[16],
    const uint8_t unique_guid[16],
    uint64_t first_lba,
    uint64_t last_lba,
    const char *name
)
{
    clear_bytes(entry, GPT_ENTRY_SIZE);
    copy_guid(entry, type_guid);
    copy_guid(entry + 16U, unique_guid);
    write_u64(entry, 32U, first_lba);
    write_u64(entry, 40U, last_lba);
    write_u64(entry, 48U, 0);
    write_partition_name(entry, name);
}

static bool write_sectors_batched(
    const block_device_t *device,
    uint64_t first_lba,
    uint32_t sector_count,
    const uint8_t *buffer
)
{
    uint32_t remaining = sector_count;
    uint64_t lba = first_lba;
    const uint8_t *source = buffer;

    while (remaining != 0)
    {
        uint32_t batch = remaining > ZERO_BATCH_SECTORS ?
            ZERO_BATCH_SECTORS : remaining;

        if (!block_device_write(device, lba, batch, source))
        {
            return false;
        }

        lba += batch;
        source += (size_t)batch * INSTALLER_REQUIRED_SECTOR_SIZE;
        remaining -= batch;
        installer_cooperate();
    }

    return true;
}

static bool write_zero_range(
    const block_device_t *device,
    uint64_t first_lba,
    uint32_t sector_count
)
{
    uint32_t remaining = sector_count;
    uint64_t lba = first_lba;

    while (remaining != 0)
    {
        uint32_t batch = remaining > ZERO_BATCH_SECTORS ?
            ZERO_BATCH_SECTORS : remaining;

        if (!block_device_write(
            device,
            lba,
            batch,
            zero_buffer
        ))
        {
            return false;
        }

        lba += batch;
        remaining -= batch;
        installer_cooperate();
    }

    return true;
}

static uint32_t fat_size_for_partition(
    uint32_t total_sectors,
    uint32_t *cluster_count
)
{
    uint32_t fat_sectors = 1U;

    for (uint32_t iteration = 0; iteration < 32U; iteration++)
    {
        if (
            total_sectors <=
                FAT_RESERVED_SECTORS + FAT_COUNT * fat_sectors
        )
        {
            return 0;
        }

        uint32_t data_sectors =
            total_sectors - FAT_RESERVED_SECTORS -
            FAT_COUNT * fat_sectors;

        uint32_t clusters =
            data_sectors / FAT_SECTORS_PER_CLUSTER;

        uint32_t needed =
            (uint32_t)(
                ((uint64_t)(clusters + 2U) * 4ULL + 511ULL) /
                512ULL
            );

        if (needed == fat_sectors)
        {
            *cluster_count = clusters;
            return fat_sectors;
        }

        fat_sectors = needed;
    }

    return 0;
}

static void write_label(
    uint8_t *destination,
    const char *label
)
{
    for (uint32_t index = 0; index < 11U; index++)
    {
        char character = ' ';

        if (label != NULL && label[index] != '\0')
        {
            character = label[index];
        }

        destination[index] = (uint8_t)character;
    }
}

static void build_fat32_boot_sector(
    uint8_t *boot,
    uint64_t partition_start,
    uint32_t total_sectors,
    uint32_t fat_sectors,
    const char *label,
    uint32_t serial
)
{
    clear_bytes(boot, 512U);
    boot[0] = 0xEBU;
    boot[1] = 0x58U;
    boot[2] = 0x90U;

    static const char oem[8] = {
        'L','A','T','T','E','R','O','S'
    };

    for (uint32_t index = 0; index < 8U; index++)
    {
        boot[3U + index] = (uint8_t)oem[index];
    }

    write_u16(boot, 11U, 512U);
    boot[13] = FAT_SECTORS_PER_CLUSTER;
    write_u16(boot, 14U, FAT_RESERVED_SECTORS);
    boot[16] = FAT_COUNT;
    write_u16(boot, 17U, 0);
    write_u16(boot, 19U, 0);
    boot[21] = 0xF8U;
    write_u16(boot, 22U, 0);
    write_u16(boot, 24U, 63U);
    write_u16(boot, 26U, 255U);
    write_u32(boot, 28U, (uint32_t)partition_start);
    write_u32(boot, 32U, total_sectors);
    write_u32(boot, 36U, fat_sectors);
    write_u16(boot, 40U, 0);
    write_u16(boot, 42U, 0);
    write_u32(boot, 44U, FAT_ROOT_CLUSTER);
    write_u16(boot, 48U, 1U);
    write_u16(boot, 50U, 6U);
    boot[64] = 0x80U;
    boot[66] = 0x29U;
    write_u32(boot, 67U, serial);
    write_label(boot + 71U, label);

    static const char type[8] = {
        'F','A','T','3','2',' ',' ',' '
    };

    for (uint32_t index = 0; index < 8U; index++)
    {
        boot[82U + index] = (uint8_t)type[index];
    }

    boot[510] = 0x55U;
    boot[511] = 0xAAU;
}

static void build_fsinfo(
    uint8_t *information,
    uint32_t free_clusters,
    uint32_t next_free
)
{
    clear_bytes(information, 512U);
    write_u32(information, 0U, 0x41615252U);
    write_u32(information, 484U, 0x61417272U);
    write_u32(information, 488U, free_clusters);
    write_u32(information, 492U, next_free);
    write_u32(information, 508U, 0xAA550000U);
}

static void write_fat_entry(
    uint8_t *fat_sector,
    uint32_t cluster,
    uint32_t value
)
{
    write_u32(
        fat_sector,
        (size_t)cluster * 4U,
        value
    );
}

static bool write_marker_file(
    const block_device_t *device,
    uint64_t partition_start,
    uint32_t fat_sectors,
    const char *marker_text
)
{
    size_t marker_length = string_length(marker_text);

    if (marker_length > 512U)
    {
        marker_length = 512U;
    }

    uint64_t first_data =
        partition_start + FAT_RESERVED_SECTORS +
        (uint64_t)FAT_COUNT * fat_sectors;

    uint64_t root_lba = first_data;
    uint64_t marker_lba = first_data + 1ULL;

    clear_bytes(sector_buffer, sizeof(sector_buffer));

    static const char short_name[11] = {
        'I','N','S','T','A','L','L',' ','T','X','T'
    };

    for (uint32_t index = 0; index < 11U; index++)
    {
        sector_buffer[index] = (uint8_t)short_name[index];
    }

    sector_buffer[11] = 0x20U;
    write_u16(
        sector_buffer,
        20U,
        (uint16_t)(FAT_MARKER_CLUSTER >> 16)
    );
    write_u16(
        sector_buffer,
        26U,
        (uint16_t)FAT_MARKER_CLUSTER
    );
    write_u32(
        sector_buffer,
        28U,
        (uint32_t)marker_length
    );

    if (!block_device_write(
        device,
        root_lba,
        1U,
        sector_buffer
    ))
    {
        return false;
    }

    clear_bytes(sector_buffer, sizeof(sector_buffer));

    for (size_t index = 0; index < marker_length; index++)
    {
        sector_buffer[index] = (uint8_t)marker_text[index];
    }

    return block_device_write(
        device,
        marker_lba,
        1U,
        sector_buffer
    );
}

static bool format_fat32(
    const block_device_t *device,
    uint64_t partition_start,
    uint64_t partition_sector_count,
    const char *label,
    uint32_t serial,
    const char *marker_text
)
{
    if (
        partition_sector_count > UINT32_MAX ||
        partition_sector_count < 131072ULL
    )
    {
        return false;
    }

    uint32_t total_sectors =
        (uint32_t)partition_sector_count;
    uint32_t cluster_count = 0;
    uint32_t fat_sectors = fat_size_for_partition(
        total_sectors,
        &cluster_count
    );

    if (
        fat_sectors == 0 ||
        cluster_count < 65525U
    )
    {
        return false;
    }

    if (!write_zero_range(
        device,
        partition_start,
        FAT_RESERVED_SECTORS
    ))
    {
        return false;
    }

    build_fat32_boot_sector(
        sector_buffer,
        partition_start,
        total_sectors,
        fat_sectors,
        label,
        serial
    );

    if (
        !block_device_write(
            device,
            partition_start,
            1U,
            sector_buffer
        ) ||
        !block_device_write(
            device,
            partition_start + 6ULL,
            1U,
            sector_buffer
        )
    )
    {
        return false;
    }

    bool marker = marker_text != NULL;
    uint32_t allocated_clusters = marker ? 2U : 1U;

    build_fsinfo(
        sector_buffer,
        cluster_count - allocated_clusters,
        marker ? 4U : 3U
    );

    if (
        !block_device_write(
            device,
            partition_start + 1ULL,
            1U,
            sector_buffer
        ) ||
        !block_device_write(
            device,
            partition_start + 7ULL,
            1U,
            sector_buffer
        )
    )
    {
        return false;
    }

    uint64_t first_fat =
        partition_start + FAT_RESERVED_SECTORS;

    if (!write_zero_range(
        device,
        first_fat,
        fat_sectors * FAT_COUNT
    ))
    {
        return false;
    }

    clear_bytes(sector_buffer, sizeof(sector_buffer));
    write_fat_entry(sector_buffer, 0U, 0x0FFFFFF8U);
    write_fat_entry(sector_buffer, 1U, FAT32_EOC);
    write_fat_entry(sector_buffer, FAT_ROOT_CLUSTER, FAT32_EOC);

    if (marker)
    {
        write_fat_entry(
            sector_buffer,
            FAT_MARKER_CLUSTER,
            FAT32_EOC
        );
    }

    for (uint32_t fat = 0; fat < FAT_COUNT; fat++)
    {
        uint64_t fat_lba =
            first_fat + (uint64_t)fat * fat_sectors;

        if (!block_device_write(
            device,
            fat_lba,
            1U,
            sector_buffer
        ))
        {
            return false;
        }
    }

    uint64_t first_data =
        first_fat + (uint64_t)FAT_COUNT * fat_sectors;

    if (!write_zero_range(
        device,
        first_data,
        marker ? 2U : 1U
    ))
    {
        return false;
    }

    if (
        marker &&
        !write_marker_file(
            device,
            partition_start,
            fat_sectors,
            marker_text
        )
    )
    {
        return false;
    }

    return true;
}

static void build_gpt_header(
    uint8_t *header,
    uint64_t current_lba,
    uint64_t backup_lba,
    uint64_t first_usable,
    uint64_t last_usable,
    const uint8_t disk_guid[16],
    uint64_t entries_lba,
    uint32_t entries_crc
)
{
    clear_bytes(header, 512U);

    static const char signature[8] = {
        'E','F','I',' ','P','A','R','T'
    };

    for (uint32_t index = 0; index < 8U; index++)
    {
        header[index] = (uint8_t)signature[index];
    }

    write_u32(header, 8U, 0x00010000U);
    write_u32(header, 12U, GPT_HEADER_SIZE);
    write_u32(header, 16U, 0);
    write_u32(header, 20U, 0);
    write_u64(header, 24U, current_lba);
    write_u64(header, 32U, backup_lba);
    write_u64(header, 40U, first_usable);
    write_u64(header, 48U, last_usable);
    copy_guid(header + 56U, disk_guid);
    write_u64(header, 72U, entries_lba);
    write_u32(header, 80U, GPT_ENTRY_COUNT);
    write_u32(header, 84U, GPT_ENTRY_SIZE);
    write_u32(header, 88U, entries_crc);

    uint32_t header_crc =
        crc32_bytes(header, GPT_HEADER_SIZE);
    write_u32(header, 16U, header_crc);
}

static bool write_gpt(
    const block_device_t *device,
    installer_report_t *report
)
{
    uint64_t last_lba = device->sector_count - 1ULL;
    uint64_t backup_entries_lba =
        last_lba - GPT_ENTRY_SECTORS;
    uint64_t first_usable =
        align_up(2ULL + GPT_ENTRY_SECTORS,
            INSTALLER_ALIGNMENT_SECTORS);
    uint64_t last_usable = backup_entries_lba - 1ULL;

    uint64_t efi_first = first_usable;
    uint64_t efi_last =
        efi_first + INSTALLER_ESP_SECTORS - 1ULL;
    uint64_t system_first = align_up(
        efi_last + 1ULL,
        INSTALLER_ALIGNMENT_SECTORS
    );

    if (
        system_first >= last_usable ||
        last_usable - system_first + 1ULL < 262144ULL
    )
    {
        set_error("Disk is too small for EFI and system partitions");
        return false;
    }

    uint64_t system_last = last_usable;

    if (report != NULL)
    {
        report->efi_first_lba = efi_first;
        report->efi_last_lba = efi_last;
        report->system_first_lba = system_first;
        report->system_last_lba = system_last;
    }

    update_progress(10U, "Writing GPT partition table");

    clear_bytes(sector_buffer, sizeof(sector_buffer));
    sector_buffer[446U + 4U] = 0xEEU;
    write_u32(sector_buffer, 446U + 8U, 1U);

    uint64_t protective_sectors = device->sector_count - 1ULL;
    write_u32(
        sector_buffer,
        446U + 12U,
        protective_sectors > UINT32_MAX ?
            UINT32_MAX : (uint32_t)protective_sectors
    );

    sector_buffer[510] = 0x55U;
    sector_buffer[511] = 0xAAU;

    if (!block_device_write(device, 0, 1U, sector_buffer))
    {
        set_error("Unable to write protective MBR");
        return false;
    }

    static const uint8_t esp_type_guid[16] = {
        0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11,
        0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B
    };

    static const uint8_t system_type_guid[16] = {
        0xAF,0x3D,0xC6,0x0F,0x83,0x84,0x72,0x47,
        0x8E,0x79,0x3D,0x69,0xD8,0x47,0x7D,0xE4
    };

    uint64_t seed =
        device->sector_count ^
        timer_ticks() ^
        0x4C61747465724F53ULL;

    uint8_t disk_guid[16];
    uint8_t esp_guid[16];
    uint8_t system_guid[16];
    make_guid(disk_guid, seed, 0U);
    make_guid(esp_guid, seed, 1U);
    make_guid(system_guid, seed, 2U);

    clear_bytes(gpt_entries, sizeof(gpt_entries));

    build_partition_entry(
        gpt_entries,
        esp_type_guid,
        esp_guid,
        efi_first,
        efi_last,
        "LatterOS EFI"
    );

    build_partition_entry(
        gpt_entries + GPT_ENTRY_SIZE,
        system_type_guid,
        system_guid,
        system_first,
        system_last,
        "LatterOS System"
    );

    uint32_t entries_crc =
        crc32_bytes(gpt_entries, sizeof(gpt_entries));

    if (!write_sectors_batched(
        device,
        2ULL,
        GPT_ENTRY_SECTORS,
        gpt_entries
    ))
    {
        set_error("Unable to write primary GPT entries");
        return false;
    }

    if (!write_sectors_batched(
        device,
        backup_entries_lba,
        GPT_ENTRY_SECTORS,
        gpt_entries
    ))
    {
        set_error("Unable to write backup GPT entries");
        return false;
    }

    build_gpt_header(
        sector_buffer,
        1ULL,
        last_lba,
        first_usable,
        last_usable,
        disk_guid,
        2ULL,
        entries_crc
    );

    if (!block_device_write(device, 1ULL, 1U, sector_buffer))
    {
        set_error("Unable to write primary GPT header");
        return false;
    }

    build_gpt_header(
        sector_buffer,
        last_lba,
        1ULL,
        first_usable,
        last_usable,
        disk_guid,
        backup_entries_lba,
        entries_crc
    );

    if (!block_device_write(
        device,
        last_lba,
        1U,
        sector_buffer
    ))
    {
        set_error("Unable to write backup GPT header");
        return false;
    }

    static const char marker[] =
        "LatterOS installation volume.\r\n"
        "Milestone 20D installs VirtualBox-ready release-candidate boot payloads.\r\n";

    uint64_t efi_sectors = efi_last - efi_first + 1ULL;
    uint64_t system_sectors = system_last - system_first + 1ULL;

    update_progress(20U, "Formatting EFI system partition");

    if (!format_fat32(
        device,
        efi_first,
        efi_sectors,
        "LATTERESP",
        (uint32_t)seed,
        NULL
    ))
    {
        set_error("Unable to format EFI partition as FAT32");
        return false;
    }

    update_progress(35U, "Formatting LatterOS system partition");

    if (!format_fat32(
        device,
        system_first,
        system_sectors,
        "LATTEROS",
        (uint32_t)(seed >> 32),
        marker
    ))
    {
        set_error("Unable to format system partition as FAT32");
        return false;
    }

    update_progress(48U, "Synchronizing formatted disk");

    if (!block_device_sync(device))
    {
        set_error("Disk flush failed after formatting");
        return false;
    }

    return true;
}


static installer_partition_cache_entry_t *find_cached_sector(
    installer_partition_context_t *context,
    uint64_t lba
)
{
    if (context == NULL)
    {
        return NULL;
    }

    for (
        uint32_t index = 0;
        index < INSTALLER_PARTITION_CACHE_SECTORS;
        index++
    )
    {
        installer_partition_cache_entry_t *entry =
            &context->cache[index];

        if (entry->valid && entry->lba == lba)
        {
            entry->access_sequence = ++context->cache_sequence;
            return entry;
        }
    }

    return NULL;
}

static void clear_partition_cache(
    installer_partition_context_t *context
)
{
    if (context == NULL)
    {
        return;
    }

    clear_bytes(context->cache, sizeof(context->cache));
    context->cache_sequence = 0;
    context->cache_used = 0;
}

static installer_partition_cache_entry_t *minimum_dirty_sector(
    installer_partition_context_t *context
)
{
    installer_partition_cache_entry_t *selected = NULL;

    if (context == NULL)
    {
        return NULL;
    }

    for (
        uint32_t index = 0;
        index < INSTALLER_PARTITION_CACHE_SECTORS;
        index++
    )
    {
        installer_partition_cache_entry_t *entry =
            &context->cache[index];

        if (
            entry->valid &&
            entry->dirty &&
            (
                selected == NULL ||
                entry->lba < selected->lba
            )
        )
        {
            selected = entry;
        }
    }

    return selected;
}

static bool flush_partition_context(
    installer_partition_context_t *context
)
{
    if (context == NULL)
    {
        return false;
    }

    if (context->cache_used == 0)
    {
        return true;
    }

    if (context->parent == NULL)
    {
        return false;
    }

    for (;;)
    {
        installer_partition_cache_entry_t *first =
            minimum_dirty_sector(context);

        if (first == NULL)
        {
            break;
        }

        uint64_t first_lba = first->lba;
        uint32_t count = 0;

        while (count < INSTALLER_PARTITION_WRITE_SECTORS)
        {
            installer_partition_cache_entry_t *entry =
                find_cached_sector(
                    context,
                    first_lba + count
                );

            if (
                entry == NULL ||
                !entry->dirty
            )
            {
                break;
            }

            copy_bytes(
                context->write_buffer +
                    (size_t)count *
                        INSTALLER_REQUIRED_SECTOR_SIZE,
                entry->data,
                INSTALLER_REQUIRED_SECTOR_SIZE
            );
            count++;
        }

        if (
            count == 0 ||
            !block_device_write(
                context->parent,
                context->first_lba + first_lba,
                count,
                context->write_buffer
            )
        )
        {
            return false;
        }

        for (uint32_t offset = 0; offset < count; offset++)
        {
            installer_partition_cache_entry_t *entry =
                find_cached_sector(
                    context,
                    first_lba + offset
                );

            if (entry != NULL)
            {
                entry->valid = false;
                entry->dirty = false;
                entry->lba = 0;
                entry->access_sequence = 0;

                if (context->cache_used > 0)
                {
                    context->cache_used--;
                }
            }
        }

        installer_cooperate();
    }

    /* Clean cache entries are not needed after an installer flush. */
    clear_partition_cache(context);
    return true;
}

static installer_partition_cache_entry_t *allocate_cached_sector(
    installer_partition_context_t *context,
    uint64_t lba
)
{
    installer_partition_cache_entry_t *entry =
        find_cached_sector(context, lba);

    if (entry != NULL)
    {
        return entry;
    }

    installer_partition_cache_entry_t *oldest_clean = NULL;

    for (
        uint32_t index = 0;
        index < INSTALLER_PARTITION_CACHE_SECTORS;
        index++
    )
    {
        installer_partition_cache_entry_t *candidate =
            &context->cache[index];

        if (!candidate->valid)
        {
            entry = candidate;
            break;
        }

        if (
            !candidate->dirty &&
            (
                oldest_clean == NULL ||
                candidate->access_sequence <
                    oldest_clean->access_sequence
            )
        )
        {
            oldest_clean = candidate;
        }
    }

    if (entry == NULL && oldest_clean != NULL)
    {
        entry = oldest_clean;
    }

    if (entry == NULL)
    {
        if (!flush_partition_context(context))
        {
            return NULL;
        }

        entry = &context->cache[0];
    }

    bool was_valid = entry->valid;
    entry->valid = true;
    entry->dirty = false;
    entry->lba = lba;
    entry->access_sequence = ++context->cache_sequence;

    if (!was_valid)
    {
        context->cache_used++;
    }

    return entry;
}

static bool partition_read_callback(
    void *context_pointer,
    uint64_t lba,
    uint32_t sector_count,
    void *buffer
)
{
    installer_partition_context_t *context = context_pointer;

    if (
        context == NULL ||
        context->parent == NULL ||
        buffer == NULL ||
        sector_count == 0 ||
        lba >= context->sector_count ||
        sector_count > context->sector_count - lba
    )
    {
        return false;
    }

    uint8_t *output = buffer;

    for (uint32_t sector = 0; sector < sector_count; sector++)
    {
        uint64_t current_lba = lba + sector;
        installer_partition_cache_entry_t *entry =
            find_cached_sector(context, current_lba);

        if (entry != NULL)
        {
            copy_bytes(
                output +
                    (size_t)sector *
                        INSTALLER_REQUIRED_SECTOR_SIZE,
                entry->data,
                INSTALLER_REQUIRED_SECTOR_SIZE
            );
            continue;
        }

        entry = allocate_cached_sector(
            context,
            current_lba
        );

        if (entry == NULL)
        {
            return false;
        }

        if (!block_device_read(
                context->parent,
                context->first_lba + current_lba,
                1U,
                entry->data
            ))
        {
            entry->valid = false;
            entry->dirty = false;
            entry->lba = 0;
            entry->access_sequence = 0;

            if (context->cache_used > 0)
            {
                context->cache_used--;
            }

            return false;
        }

        copy_bytes(
            output +
                (size_t)sector *
                    INSTALLER_REQUIRED_SECTOR_SIZE,
            entry->data,
            INSTALLER_REQUIRED_SECTOR_SIZE
        );
    }

    return true;
}

static bool partition_write_callback(
    void *context_pointer,
    uint64_t lba,
    uint32_t sector_count,
    const void *buffer
)
{
    installer_partition_context_t *context = context_pointer;

    if (
        context == NULL ||
        context->parent == NULL ||
        buffer == NULL ||
        sector_count == 0 ||
        lba >= context->sector_count ||
        sector_count > context->sector_count - lba
    )
    {
        return false;
    }

    const uint8_t *input = buffer;

    for (uint32_t sector = 0; sector < sector_count; sector++)
    {
        installer_partition_cache_entry_t *entry =
            allocate_cached_sector(
                context,
                lba + sector
            );

        if (entry == NULL)
        {
            return false;
        }

        copy_bytes(
            entry->data,
            input +
                (size_t)sector *
                    INSTALLER_REQUIRED_SECTOR_SIZE,
            INSTALLER_REQUIRED_SECTOR_SIZE
        );
        entry->dirty = true;
        entry->access_sequence = ++context->cache_sequence;
    }

    return true;
}

static bool register_install_partition(
    installer_partition_context_t *context,
    const block_device_t *parent,
    uint64_t first_lba,
    uint64_t last_lba,
    const char *name,
    uint32_t *device_index
)
{
    if (
        context == NULL ||
        parent == NULL ||
        name == NULL ||
        device_index == NULL ||
        first_lba > last_lba ||
        last_lba >= parent->sector_count
    )
    {
        return false;
    }

    context->parent = parent;
    context->first_lba = first_lba;
    context->sector_count = last_lba - first_lba + 1ULL;
    clear_partition_cache(context);

    block_device_t partition_device = {
        .name = name,
        .sector_size = parent->sector_size,
        .sector_count = context->sector_count,
        .writable = true,
        .online = true,
        .removable = false,
        .io_references = 0,
        .context = context,
        .read = partition_read_callback,
        .write = partition_write_callback
    };

    if (!block_device_register(&partition_device))
    {
        return false;
    }

    for (uint32_t index = 0; index < block_device_count(); index++)
    {
        const block_device_t *registered = block_device_get(index);

        if (
            registered != NULL &&
            registered->context == context
        )
        {
            *device_index = index;
            return true;
        }
    }

    return false;
}

static bool ensure_directory(const char *path)
{
    vfs_node_t *existing = vfs_open(path);

    if (existing != NULL)
    {
        return existing->type == VFS_NODE_DIRECTORY;
    }

    return vfs_make_directory(path);
}

static bool write_payload_file_progress(
    const char *path,
    const uint8_t *data,
    size_t size,
    uint32_t progress_start,
    uint32_t progress_end,
    const char *progress_text
)
{
    if (
        path == NULL ||
        data == NULL ||
        size == 0
    )
    {
        return false;
    }

    vfs_node_t *node = vfs_open(path);

    if (node == NULL)
    {
        if (!vfs_create_file(path))
        {
            return false;
        }

        node = vfs_open(path);
    }

    if (
        node == NULL ||
        node->type != VFS_NODE_FILE ||
        !vfs_truncate(node)
    )
    {
        return false;
    }

    size_t offset = 0;

    while (offset < size)
    {
        size_t remaining = size - offset;
        size_t count = remaining > INSTALLER_COPY_CHUNK ?
            INSTALLER_COPY_CHUNK : remaining;

        if (
            vfs_write(
                node,
                offset,
                data + offset,
                count
            ) != count
        )
        {
            return false;
        }

        offset += count;

        if (
            progress_text != NULL &&
            progress_end >= progress_start
        )
        {
            uint64_t span =
                (uint64_t)progress_end - progress_start;
            uint32_t percentage = progress_start +
                (uint32_t)(span * offset / size);
            update_progress(percentage, progress_text);
        }

        installer_cooperate();
    }

    return node->size == size;
}

static bool write_payload_file(
    const char *path,
    const uint8_t *data,
    size_t size
)
{
    return write_payload_file_progress(
        path,
        data,
        size,
        0U,
        0U,
        NULL
    );
}

static bool write_text_file(
    const char *path,
    const char *text
)
{
    return write_payload_file(
        path,
        (const uint8_t *)text,
        string_length(text)
    );
}

static bool verify_payload_file(
    const char *path,
    const uint8_t *expected,
    size_t expected_size
)
{
    vfs_node_t *node = vfs_open(path);

    if (
        node == NULL ||
        node->type != VFS_NODE_FILE ||
        node->size != expected_size
    )
    {
        return false;
    }

    size_t offset = 0;

    while (offset < expected_size)
    {
        size_t remaining = expected_size - offset;
        size_t count = remaining > sizeof(payload_verify_buffer) ?
            sizeof(payload_verify_buffer) : remaining;

        if (
            vfs_read(
                node,
                offset,
                payload_verify_buffer,
                count
            ) != count
        )
        {
            return false;
        }

        for (size_t index = 0; index < count; index++)
        {
            if (
                payload_verify_buffer[index] !=
                expected[offset + index]
            )
            {
                return false;
            }
        }

        offset += count;
    }

    return true;
}

static bool verify_text_file(
    const char *path,
    const char *text
)
{
    return verify_payload_file(
        path,
        (const uint8_t *)text,
        string_length(text)
    );
}

static bool unmount_install_volume(
    installer_partition_context_t *context,
    bool operation_success
)
{
    bool unmounted = true;

    if (fat_fs_mounted())
    {
        unmounted = fat_fs_unmount();
    }

    bool flushed = flush_partition_context(context);
    return operation_success && unmounted && flushed;
}

static bool install_efi_volume(
    uint32_t device_index,
    const uint8_t *kernel,
    size_t kernel_size,
    const uint8_t *bootx64,
    size_t bootx64_size
)
{
    update_progress(69U, "Mounting EFI installation volume");

    if (!fat_fs_mount_device_index(
        device_index,
        INSTALLER_ESP_MOUNT
    ))
    {
        set_error("Unable to mount the new EFI FAT32 partition");
        return false;
    }

    update_progress(70U, "Creating EFI boot directories");

    bool success =
        ensure_directory(INSTALLER_ESP_MOUNT "/EFI") &&
        ensure_directory(INSTALLER_ESP_MOUNT "/EFI/BOOT") &&
        ensure_directory(INSTALLER_ESP_MOUNT "/boot") &&
        ensure_directory(INSTALLER_ESP_MOUNT "/boot/limine");

    if (!success)
    {
        set_error("Unable to create EFI installation directories");
        return unmount_install_volume(&efi_partition_context, false);
    }

    success = write_payload_file_progress(
        INSTALLER_ESP_MOUNT "/EFI/BOOT/BOOTX64.EFI",
        bootx64,
        bootx64_size,
        71U,
        73U,
        "Copying Limine UEFI loader"
    );

    if (success)
    {
        update_progress(74U, "Writing installed boot configuration");
        success = write_text_file(
            INSTALLER_ESP_MOUNT "/EFI/BOOT/limine.conf",
            installed_limine_configuration
        );
    }

    if (success)
    {
        success = write_payload_file_progress(
            INSTALLER_ESP_MOUNT "/boot/kernel",
            kernel,
            kernel_size,
            75U,
            81U,
            "Copying LatterOS kernel to EFI volume"
        );
    }

    if (success)
    {
        update_progress(82U, "Writing EFI installation metadata");
        success =
            write_text_file(
                INSTALLER_ESP_MOUNT "/boot/limine/limine.conf",
                installed_limine_configuration
            ) &&
            write_text_file(
                INSTALLER_ESP_MOUNT "/INSTALL.TXT",
                installation_marker
            );
    }

    if (!success)
    {
        set_error("Unable to copy UEFI boot files to the EFI partition");
        return unmount_install_volume(&efi_partition_context, false);
    }

    update_progress(83U, "Verifying EFI boot payload");

    success =
        verify_payload_file(
            INSTALLER_ESP_MOUNT "/EFI/BOOT/BOOTX64.EFI",
            bootx64,
            bootx64_size
        ) &&
        verify_payload_file(
            INSTALLER_ESP_MOUNT "/boot/kernel",
            kernel,
            kernel_size
        ) &&
        verify_text_file(
            INSTALLER_ESP_MOUNT "/boot/limine/limine.conf",
            installed_limine_configuration
        );

    if (!success)
    {
        set_error("EFI payload read-back verification failed");
        return unmount_install_volume(&efi_partition_context, false);
    }

    update_progress(84U, "Flushing EFI installation volume");

    if (!unmount_install_volume(&efi_partition_context, true))
    {
        set_error("Unable to synchronize and unmount the EFI partition");
        return false;
    }

    return true;
}

static bool install_system_volume(
    uint32_t device_index,
    const uint8_t *kernel,
    size_t kernel_size
)
{
    update_progress(85U, "Mounting LatterOS system volume");

    if (!fat_fs_mount_device_index(
        device_index,
        INSTALLER_SYSTEM_MOUNT
    ))
    {
        set_error("Unable to mount the new LatterOS system partition");
        return false;
    }

    update_progress(86U, "Creating LatterOS system directories");

    bool success =
        ensure_directory(INSTALLER_SYSTEM_MOUNT "/boot") &&
        ensure_directory(INSTALLER_SYSTEM_MOUNT "/boot/limine") &&
        ensure_directory(INSTALLER_SYSTEM_MOUNT "/System");

    if (!success)
    {
        set_error("Unable to create LatterOS system directories");
        return unmount_install_volume(&system_partition_context, false);
    }

    success = write_payload_file_progress(
        INSTALLER_SYSTEM_MOUNT "/boot/kernel",
        kernel,
        kernel_size,
        87U,
        93U,
        "Copying LatterOS kernel to system volume"
    );

    if (success)
    {
        update_progress(94U, "Writing LatterOS system metadata");
        success =
            write_text_file(
                INSTALLER_SYSTEM_MOUNT "/boot/limine/limine.conf",
                installed_limine_configuration
            ) &&
            write_text_file(
                INSTALLER_SYSTEM_MOUNT "/System/INSTALL.TXT",
                installation_marker
            ) &&
            write_text_file(
                INSTALLER_SYSTEM_MOUNT "/System/VERSION.TXT",
                installation_version
            );
    }

    if (!success)
    {
        set_error("Unable to copy the LatterOS system payload");
        return unmount_install_volume(&system_partition_context, false);
    }

    update_progress(95U, "Verifying LatterOS system payload");

    success =
        verify_payload_file(
            INSTALLER_SYSTEM_MOUNT "/boot/kernel",
            kernel,
            kernel_size
        ) &&
        verify_text_file(
            INSTALLER_SYSTEM_MOUNT "/boot/limine/limine.conf",
            installed_limine_configuration
        ) &&
        verify_text_file(
            INSTALLER_SYSTEM_MOUNT "/System/INSTALL.TXT",
            installation_marker
        );

    if (!success)
    {
        set_error("System payload read-back verification failed");
        return unmount_install_volume(&system_partition_context, false);
    }

    update_progress(96U, "Flushing LatterOS system volume");

    if (!unmount_install_volume(&system_partition_context, true))
    {
        set_error("Unable to synchronize and unmount the system partition");
        return false;
    }

    return true;
}

static bool suspend_existing_fat_mount(
    installer_previous_mount_t *previous
)
{
    if (previous == NULL)
    {
        return false;
    }

    *previous = INSTALLER_PREVIOUS_MOUNT_NONE;

    if (!fat_fs_mounted())
    {
        return true;
    }

    const char *path = fat_fs_mount_path();

    if (strings_equal(path, "/media/usb"))
    {
        *previous = INSTALLER_PREVIOUS_MOUNT_USB;
    }
    else if (strings_equal(path, "/media/sata"))
    {
        *previous = INSTALLER_PREVIOUS_MOUNT_SATA;
    }
    else if (strings_equal(path, "/media/nvme"))
    {
        *previous = INSTALLER_PREVIOUS_MOUNT_NVME;
    }
    else
    {
        *previous = INSTALLER_PREVIOUS_MOUNT_OTHER;
    }

    if (!fat_fs_unmount())
    {
        set_error("Unable to suspend the currently mounted FAT volume");
        return false;
    }

    return true;
}

static void restore_previous_fat_mount(
    installer_previous_mount_t previous
)
{
    if (fat_fs_mounted())
    {
        return;
    }

    switch (previous)
    {
        case INSTALLER_PREVIOUS_MOUNT_USB:
            (void)fat_fs_mount_first_usb();
            break;

        case INSTALLER_PREVIOUS_MOUNT_SATA:
            (void)fat_fs_mount_first_sata();
            break;

        case INSTALLER_PREVIOUS_MOUNT_NVME:
            (void)fat_fs_mount_first_nvme();
            break;

        case INSTALLER_PREVIOUS_MOUNT_NONE:
        case INSTALLER_PREVIOUS_MOUNT_OTHER:
        default:
            break;
    }
}

static bool install_boot_payload(
    const block_device_t *device,
    const installer_report_t *report
)
{
    const uint8_t *kernel = NULL;
    const uint8_t *bootx64 = NULL;
    size_t kernel_size = 0;
    size_t bootx64_size = 0;

    if (
        !installer_payload_get(
            INSTALLER_PAYLOAD_KERNEL,
            &kernel,
            &kernel_size
        ) ||
        !installer_payload_get(
            INSTALLER_PAYLOAD_BOOTX64,
            &bootx64,
            &bootx64_size
        )
    )
    {
        set_error("Installer payload unavailable: boot from the LatterOS ISO");
        return false;
    }

    update_progress(60U, "Preparing boot payload installation");

    installer_previous_mount_t previous_mount;

    if (!suspend_existing_fat_mount(&previous_mount))
    {
        return false;
    }

    (void)block_device_unregister_prefix(
        INSTALLER_PARTITION_PREFIX
    );

    uint32_t efi_device_index = UINT32_MAX;
    uint32_t system_device_index = UINT32_MAX;

    bool registered =
        register_install_partition(
            &efi_partition_context,
            device,
            report->efi_first_lba,
            report->efi_last_lba,
            efi_partition_name,
            &efi_device_index
        ) &&
        register_install_partition(
            &system_partition_context,
            device,
            report->system_first_lba,
            report->system_last_lba,
            system_partition_name,
            &system_device_index
        );

    if (!registered)
    {
        set_error("Unable to register temporary installation partitions");
        (void)block_device_unregister_prefix(
            INSTALLER_PARTITION_PREFIX
        );
        restore_previous_fat_mount(previous_mount);
        return false;
    }

    update_progress(68U, "Installing Limine UEFI and kernel");

    bool success = install_efi_volume(
        efi_device_index,
        kernel,
        kernel_size,
        bootx64,
        bootx64_size
    );

    if (success)
    {
        update_progress(84U, "Installing LatterOS system payload");
        success = install_system_volume(
            system_device_index,
            kernel,
            kernel_size
        );
    }

    if (fat_fs_mounted())
    {
        (void)fat_fs_unmount();
    }

    if (!flush_partition_context(&efi_partition_context))
    {
        success = false;
        set_error("Unable to flush the EFI installation buffer");
    }

    if (!flush_partition_context(&system_partition_context))
    {
        success = false;
        set_error("Unable to flush the system installation buffer");
    }

    (void)block_device_unregister_prefix(
        INSTALLER_PARTITION_PREFIX
    );

    if (success)
    {
        update_progress(96U, "Finalizing and synchronizing installation");
    }

    if (success && !block_device_sync(device))
    {
        set_error("Final installation-disk synchronization failed");
        success = false;
    }

    restore_previous_fat_mount(previous_mount);
    return success;
}

static bool verify_gpt_and_fat(
    const block_device_t *device,
    const installer_report_t *report
)
{
    update_progress(52U, "Verifying GPT and FAT32 volumes");
    if (
        !block_device_read(device, 0, 1U, verify_buffer) ||
        verify_buffer[510] != 0x55U ||
        verify_buffer[511] != 0xAAU
    )
    {
        set_error("Protective MBR verification failed");
        return false;
    }

    if (!block_device_read(device, 1, 1U, verify_buffer))
    {
        set_error("Primary GPT header cannot be read back");
        return false;
    }

    static const char signature[8] = {
        'E','F','I',' ','P','A','R','T'
    };

    for (uint32_t index = 0; index < 8U; index++)
    {
        if (verify_buffer[index] != (uint8_t)signature[index])
        {
            set_error("Primary GPT signature verification failed");
            return false;
        }
    }

    uint32_t stored_header_crc = read_u32(verify_buffer, 16U);
    write_u32(verify_buffer, 16U, 0);

    if (
        crc32_bytes(verify_buffer, GPT_HEADER_SIZE) !=
        stored_header_crc
    )
    {
        set_error("Primary GPT CRC verification failed");
        return false;
    }

    if (
        !block_device_read(
            device,
            report->efi_first_lba,
            1U,
            verify_buffer
        ) ||
        verify_buffer[510] != 0x55U ||
        verify_buffer[511] != 0xAAU
    )
    {
        set_error("EFI FAT32 boot-sector verification failed");
        return false;
    }

    if (
        !block_device_read(
            device,
            report->system_first_lba,
            1U,
            verify_buffer
        ) ||
        verify_buffer[510] != 0x55U ||
        verify_buffer[511] != 0xAAU
    )
    {
        set_error("System FAT32 boot-sector verification failed");
        return false;
    }

    return true;
}

bool installer_prepare_target(
    uint32_t target_index,
    installer_report_t *report
)
{
    update_progress(2U, "Validating installation target");

    if (report != NULL)
    {
        clear_bytes(report, sizeof(*report));
    }

    if (!installer_payload_available())
    {
        set_error(
            "Installer payload missing: boot from the LatterOS installation ISO"
        );
        set_report(report, false, last_error);
        return false;
    }

    if (target_index >= target_count)
    {
        set_error("Select a valid installation target");
        set_report(report, false, last_error);
        return false;
    }

    uint32_t device_index = target_devices[target_index];
    const block_device_t *device = block_device_get(device_index);

    if (!valid_raw_target(device))
    {
        set_error("Selected disk is no longer available or safe");
        set_report(report, false, last_error);
        return false;
    }

    if (!write_gpt(device, report))
    {
        set_report(report, false, last_error);
        return false;
    }

    if (!verify_gpt_and_fat(device, report))
    {
        set_report(report, false, last_error);
        return false;
    }

    if (!install_boot_payload(device, report))
    {
        set_report(report, false, last_error);
        return false;
    }

    set_error(
        "Installation complete: Limine UEFI + kernel payload verified"
    );
    update_progress(100U, last_error);
    set_report(report, true, last_error);
    return true;
}

static uint64_t installer_worker(void *argument)
{
    (void)argument;

    __atomic_store_n(
        &async_worker_context,
        true,
        __ATOMIC_RELEASE
    );

    __atomic_store_n(
        &async_state,
        INSTALLER_ASYNC_RUNNING,
        __ATOMIC_RELEASE
    );

    installer_report_t report;
    clear_bytes(&report, sizeof(report));
    bool success = installer_prepare_target(
        async_target_index,
        &report
    );

    async_report = report;

    __atomic_store_n(
        &async_worker_context,
        false,
        __ATOMIC_RELEASE
    );

    __atomic_store_n(
        &async_state,
        INSTALLER_ASYNC_COMPLETE,
        __ATOMIC_RELEASE
    );

    return success ? 1ULL : 0ULL;
}

bool installer_start_target(uint32_t target_index)
{
    if (
        target_index >= target_count ||
        !installer_payload_available()
    )
    {
        set_error("Select a valid installation target");
        return false;
    }

    uint32_t expected = INSTALLER_ASYNC_IDLE;

    if (!__atomic_compare_exchange_n(
        &async_state,
        &expected,
        INSTALLER_ASYNC_QUEUED,
        false,
        __ATOMIC_ACQ_REL,
        __ATOMIC_ACQUIRE
    ))
    {
        set_error("Installation is already in progress");
        return false;
    }

    async_target_index = target_index;
    async_worker_cpu = 0;
    async_job_id = 0;
    __atomic_store_n(
        &async_worker_context,
        false,
        __ATOMIC_RELEASE
    );
    clear_bytes(&async_report, sizeof(async_report));
    update_progress(1U, "Queuing installer on an application processor");

    uint32_t selected_cpu = 0;
    uint64_t selected_job = 0;

    if (!smp_scheduler_submit_any(
            installer_worker,
            NULL,
            &selected_cpu,
            &selected_job
        ))
    {
        __atomic_store_n(
            &async_state,
            INSTALLER_ASYNC_IDLE,
            __ATOMIC_RELEASE
        );
        set_error(
            "Unable to start installer on an application processor"
        );
        update_progress(0U, last_error);
        return false;
    }

    async_worker_cpu = selected_cpu;
    async_job_id = selected_job;
    set_error("Installation queued on an application processor");
    return true;
}

bool installer_running(void)
{
    uint32_t state = __atomic_load_n(
        &async_state,
        __ATOMIC_ACQUIRE
    );

    return
        state == INSTALLER_ASYNC_QUEUED ||
        state == INSTALLER_ASYNC_RUNNING;
}

bool installer_progress(
    uint32_t *percentage,
    char *message,
    size_t message_capacity
)
{
    if (percentage == NULL || message == NULL || message_capacity == 0)
    {
        return false;
    }

    for (uint32_t attempt = 0; attempt < 16U; attempt++)
    {
        uint32_t before = __atomic_load_n(
            &progress_sequence,
            __ATOMIC_ACQUIRE
        );

        if ((before & 1U) != 0)
        {
            __asm__ volatile("pause");
            continue;
        }

        uint32_t current_percentage = progress_percentage;
        copy_text(
            message,
            message_capacity,
            progress_message
        );

        uint32_t after = __atomic_load_n(
            &progress_sequence,
            __ATOMIC_ACQUIRE
        );

        if (before == after && (after & 1U) == 0)
        {
            *percentage = current_percentage;
            return true;
        }
    }

    return false;
}

bool installer_take_completion(installer_report_t *report)
{
    uint32_t state = __atomic_load_n(
        &async_state,
        __ATOMIC_ACQUIRE
    );

    if (state != INSTALLER_ASYNC_COMPLETE)
    {
        return false;
    }

    if (report != NULL)
    {
        *report = async_report;
    }

    __atomic_store_n(
        &async_state,
        INSTALLER_ASYNC_IDLE,
        __ATOMIC_RELEASE
    );
    async_target_index = UINT32_MAX;
    return true;
}

const char *installer_last_error(void)
{
    return last_error;
}
