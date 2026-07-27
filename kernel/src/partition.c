#include "partition.h"

#include "kstdio.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MBR_PARTITION_TABLE_OFFSET 446U
#define MBR_SIGNATURE_OFFSET       510U
#define MBR_SIGNATURE_LOW          0x55U
#define MBR_SIGNATURE_HIGH         0xAAU
#define MBR_PARTITION_COUNT        4U
#define MBR_TYPE_PROTECTIVE_GPT    0xEEU
#define LATTEROS_FS_PARTITION_START 2048U

#define GPT_HEADER_LBA              1ULL
#define GPT_MIN_HEADER_SIZE         92U
#define GPT_MAX_ENTRY_SIZE          512U
#define GPT_MAX_ENTRY_COUNT         256U
#define GPT_SIGNATURE               0x5452415020494645ULL


typedef struct __attribute__((packed))
{
    uint8_t status;
    uint8_t first_chs[3];
    uint8_t type;
    uint8_t last_chs[3];
    uint32_t first_lba;
    uint32_t sector_count;
} mbr_partition_entry_t;

typedef struct __attribute__((packed))
{
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t disk_guid[16];
    uint64_t entries_lba;
    uint32_t entry_count;
    uint32_t entry_size;
    uint32_t entries_crc32;
} gpt_header_t;

typedef struct __attribute__((packed))
{
    uint8_t type_guid[16];
    uint8_t unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name_utf16[36];
} gpt_entry_t;

static partition_t partitions[PARTITION_MAX_COUNT];
static uint32_t detected_count;
static uint32_t raw_device_count;
static bool initialized;
static uint8_t sector_buffer[512];
static uint8_t second_sector_buffer[512];

static void clear_bytes(void *pointer, uint32_t count)
{
    uint8_t *bytes = pointer;

    for (uint32_t index = 0; index < count; index++)
    {
        bytes[index] = 0;
    }
}

static bool string_starts_with(
    const char *text,
    const char *prefix
)
{
    if (text == NULL || prefix == NULL)
    {
        return false;
    }

    uint32_t index = 0;

    while (prefix[index] != '\0')
    {
        if (text[index] != prefix[index])
        {
            return false;
        }

        index++;
    }

    return true;
}

static void append_uint(
    char *text,
    uint32_t capacity,
    uint32_t *length,
    uint32_t value
)
{
    char reverse[11];
    uint32_t count = 0;

    if (value == 0)
    {
        if (*length + 1U < capacity)
        {
            text[(*length)++] = '0';
        }

        return;
    }

    while (value > 0 && count < sizeof(reverse))
    {
        reverse[count++] =
            (char)('0' + value % 10U);
        value /= 10U;
    }

    while (count > 0 && *length + 1U < capacity)
    {
        text[(*length)++] = reverse[--count];
    }
}

static void copy_label(
    char *destination,
    uint32_t capacity,
    const uint16_t *source,
    uint32_t source_count
)
{
    uint32_t length = 0;

    while (
        length < source_count &&
        length + 1U < capacity
    )
    {
        uint16_t value = source[length];

        if (value == 0)
        {
            break;
        }

        destination[length] =
            value >= 32U && value <= 126U ?
                (char)value : '?';

        length++;
    }

    while (
        length > 0 &&
        destination[length - 1U] == ' '
    )
    {
        length--;
    }

    destination[length] = '\0';
}

static const char *device_partition_prefix(
    const block_device_t *device
)
{
    if (device == NULL)
    {
        return "Disk partition ";
    }

    if (string_starts_with(device->name, "AHCI SATA"))
    {
        return "AHCI partition ";
    }

    if (string_starts_with(device->name, "NVMe namespace"))
    {
        return "NVMe partition ";
    }

    if (string_starts_with(device->name, "ATA"))
    {
        return "ATA partition ";
    }

    return "Disk partition ";
}

static void build_partition_name(partition_t *partition)
{
    const char *prefix =
        device_partition_prefix(partition->device);

    uint32_t length = 0;

    while (
        prefix[length] != '\0' &&
        length + 1U < sizeof(partition->name)
    )
    {
        partition->name[length] = prefix[length];
        length++;
    }

    append_uint(
        partition->name,
        sizeof(partition->name),
        &length,
        partition->number
    );

    partition->name[length] = '\0';
}

static bool guid_is_zero(const uint8_t *guid)
{
    for (uint32_t index = 0; index < 16U; index++)
    {
        if (guid[index] != 0)
        {
            return false;
        }
    }

    return true;
}

static uint32_t crc32_update(
    uint32_t crc,
    const uint8_t *data,
    uint32_t length
)
{
    for (uint32_t index = 0; index < length; index++)
    {
        crc ^= data[index];

        for (uint8_t bit = 0; bit < 8U; bit++)
        {
            uint32_t mask =
                (uint32_t)-(int32_t)(crc & 1U);

            crc = (crc >> 1) ^
                (0xEDB88320U & mask);
        }
    }

    return crc;
}

static uint32_t crc32(
    const uint8_t *data,
    uint32_t length
)
{
    return ~crc32_update(0xFFFFFFFFU, data, length);
}

static bool partition_read_callback(
    void *context,
    uint64_t lba,
    uint32_t sector_count,
    void *buffer
)
{
    return partition_read(
        (const partition_t *)context,
        lba,
        sector_count,
        buffer
    );
}

static bool partition_write_callback(
    void *context,
    uint64_t lba,
    uint32_t sector_count,
    const void *buffer
)
{
    return partition_write(
        (const partition_t *)context,
        lba,
        sector_count,
        buffer
    );
}

static bool partition_already_known(
    const block_device_t *device,
    uint64_t start_lba,
    uint64_t sector_count
)
{
    for (uint32_t index = 0; index < detected_count; index++)
    {
        if (
            partitions[index].device == device &&
            partitions[index].start_lba == start_lba &&
            partitions[index].sector_count == sector_count
        )
        {
            return true;
        }
    }

    return false;
}

static bool append_partition(
    const block_device_t *device,
    uint32_t source_device_index,
    uint32_t number,
    partition_scheme_t scheme,
    uint8_t type,
    const uint8_t *type_guid,
    const uint8_t *unique_guid,
    const char *label,
    bool bootable,
    uint64_t start_lba,
    uint64_t sector_count
)
{
    if (
        device == NULL ||
        detected_count >= PARTITION_MAX_COUNT ||
        start_lba >= device->sector_count ||
        sector_count == 0 ||
        sector_count > device->sector_count - start_lba ||
        partition_already_known(
            device,
            start_lba,
            sector_count
        )
    )
    {
        return false;
    }

    partition_t *partition =
        &partitions[detected_count];

    clear_bytes(partition, sizeof(*partition));
    partition->device = device;
    partition->source_device_index = source_device_index;
    partition->number = number;
    partition->scheme = scheme;
    partition->type = type;
    partition->bootable = bootable;
    partition->start_lba = start_lba;
    partition->sector_count = sector_count;

    if (type_guid != NULL)
    {
        for (uint32_t index = 0; index < 16U; index++)
        {
            partition->type_guid[index] = type_guid[index];
        }
    }

    if (unique_guid != NULL)
    {
        for (uint32_t index = 0; index < 16U; index++)
        {
            partition->unique_guid[index] = unique_guid[index];
        }
    }

    if (label != NULL)
    {
        uint32_t index = 0;

        while (
            label[index] != '\0' &&
            index + 1U < sizeof(partition->label)
        )
        {
            partition->label[index] = label[index];
            index++;
        }

        partition->label[index] = '\0';
    }

    build_partition_name(partition);

    partition->block_device.name = partition->name;
    partition->block_device.sector_size = device->sector_size;
    partition->block_device.sector_count = sector_count;
    partition->block_device.writable = device->writable;
    partition->block_device.context = partition;
    partition->block_device.read = partition_read_callback;
    partition->block_device.write =
        device->writable ?
            partition_write_callback :
            NULL;

    if (!block_device_register(&partition->block_device))
    {
        clear_bytes(partition, sizeof(*partition));
        return false;
    }

    detected_count++;
    return true;
}

static bool read_gpt_entry(
    const block_device_t *device,
    const gpt_header_t *header,
    uint32_t index,
    uint8_t *entry_buffer
)
{
    uint64_t byte_offset =
        (uint64_t)index * header->entry_size;

    uint64_t lba =
        header->entries_lba +
        byte_offset / device->sector_size;

    uint32_t sector_offset =
        (uint32_t)(byte_offset % device->sector_size);

    if (!block_device_read(
        device,
        lba,
        1,
        sector_buffer
    ))
    {
        return false;
    }

    uint32_t first_count =
        device->sector_size - sector_offset;

    if (first_count > header->entry_size)
    {
        first_count = header->entry_size;
    }

    for (uint32_t byte = 0; byte < first_count; byte++)
    {
        entry_buffer[byte] =
            sector_buffer[sector_offset + byte];
    }

    if (first_count == header->entry_size)
    {
        return true;
    }

    if (!block_device_read(
        device,
        lba + 1U,
        1,
        second_sector_buffer
    ))
    {
        return false;
    }

    for (
        uint32_t byte = first_count;
        byte < header->entry_size;
        byte++
    )
    {
        entry_buffer[byte] =
            second_sector_buffer[byte - first_count];
    }

    return true;
}

static bool validate_gpt_entries_crc(
    const block_device_t *device,
    const gpt_header_t *header
)
{
    uint64_t total_bytes =
        (uint64_t)header->entry_count *
        header->entry_size;

    uint64_t byte_position = 0;
    uint32_t crc = 0xFFFFFFFFU;

    while (byte_position < total_bytes)
    {
        uint64_t lba =
            header->entries_lba +
            byte_position / device->sector_size;

        if (!block_device_read(
            device,
            lba,
            1,
            sector_buffer
        ))
        {
            return false;
        }

        uint32_t count = device->sector_size;

        if (total_bytes - byte_position < count)
        {
            count = (uint32_t)(total_bytes - byte_position);
        }

        crc = crc32_update(crc, sector_buffer, count);
        byte_position += count;
    }

    return ~crc == header->entries_crc32;
}

static bool scan_gpt_device(
    const block_device_t *device,
    uint32_t source_device_index
)
{
    if (
        device == NULL ||
        device->sector_size != 512U ||
        !block_device_read(
            device,
            GPT_HEADER_LBA,
            1,
            sector_buffer
        )
    )
    {
        return false;
    }

    gpt_header_t header =
        *(const gpt_header_t *)sector_buffer;

    if (
        header.signature != GPT_SIGNATURE ||
        header.header_size < GPT_MIN_HEADER_SIZE ||
        header.header_size > device->sector_size ||
        header.current_lba != GPT_HEADER_LBA ||
        header.first_usable_lba > header.last_usable_lba ||
        header.last_usable_lba >= device->sector_count ||
        header.entries_lba >= device->sector_count ||
        header.entry_count == 0 ||
        header.entry_count > GPT_MAX_ENTRY_COUNT ||
        header.entry_size < sizeof(gpt_entry_t) ||
        header.entry_size > GPT_MAX_ENTRY_SIZE
    )
    {
        return false;
    }

    uint8_t header_copy[512];

    for (uint32_t index = 0; index < device->sector_size; index++)
    {
        header_copy[index] = sector_buffer[index];
    }

    header_copy[16] = 0;
    header_copy[17] = 0;
    header_copy[18] = 0;
    header_copy[19] = 0;

    if (
        crc32(header_copy, header.header_size) !=
        header.header_crc32 ||
        !validate_gpt_entries_crc(device, &header)
    )
    {
        return false;
    }

    uint8_t entry_buffer[GPT_MAX_ENTRY_SIZE];
    bool found = false;

    for (
        uint32_t index = 0;
        index < header.entry_count &&
        detected_count < PARTITION_MAX_COUNT;
        index++
    )
    {
        if (!read_gpt_entry(
            device,
            &header,
            index,
            entry_buffer
        ))
        {
            break;
        }

        const gpt_entry_t *entry =
            (const gpt_entry_t *)entry_buffer;

        if (guid_is_zero(entry->type_guid))
        {
            continue;
        }

        if (
            entry->first_lba < header.first_usable_lba ||
            entry->last_lba > header.last_usable_lba ||
            entry->first_lba > entry->last_lba
        )
        {
            continue;
        }

        char label[48];
        copy_label(
            label,
            sizeof(label),
            entry->name_utf16,
            36U
        );

        bool bootable =
            (entry->attributes & (1ULL << 2)) != 0;

        if (append_partition(
            device,
            source_device_index,
            index + 1U,
            PARTITION_SCHEME_GPT,
            0,
            entry->type_guid,
            entry->unique_guid,
            label,
            bootable,
            entry->first_lba,
            entry->last_lba - entry->first_lba + 1U
        ))
        {
            found = true;
        }
    }

    return found;
}

static void scan_mbr_device(
    const block_device_t *device,
    uint32_t source_device_index
)
{
    if (
        device == NULL ||
        device->sector_size != 512U ||
        !block_device_read(
            device,
            0,
            1,
            sector_buffer
        )
    )
    {
        return;
    }

    if (
        sector_buffer[MBR_SIGNATURE_OFFSET] !=
            MBR_SIGNATURE_LOW ||
        sector_buffer[MBR_SIGNATURE_OFFSET + 1U] !=
            MBR_SIGNATURE_HIGH
    )
    {
        return;
    }

    const mbr_partition_entry_t *entries =
        (const mbr_partition_entry_t *)(
            &sector_buffer[MBR_PARTITION_TABLE_OFFSET]
        );

    bool protective_gpt = false;

    for (
        uint32_t index = 0;
        index < MBR_PARTITION_COUNT;
        index++
    )
    {
        if (entries[index].type == MBR_TYPE_PROTECTIVE_GPT)
        {
            protective_gpt = true;
            break;
        }
    }

    if (protective_gpt && scan_gpt_device(
        device,
        source_device_index
    ))
    {
        return;
    }

    for (
        uint32_t index = 0;
        index < MBR_PARTITION_COUNT;
        index++
    )
    {
        const mbr_partition_entry_t *entry =
            &entries[index];

        if (
            entry->type == 0 ||
            entry->type == MBR_TYPE_PROTECTIVE_GPT ||
            entry->sector_count == 0
        )
        {
            continue;
        }

        (void)append_partition(
            device,
            source_device_index,
            index + 1U,
            PARTITION_SCHEME_MBR,
            entry->type,
            NULL,
            NULL,
            NULL,
            entry->status == 0x80U,
            entry->first_lba,
            entry->sector_count
        );
    }
}

void partition_init(void)
{
    if (initialized)
    {
        return;
    }

    initialized = true;
    detected_count = 0;
    raw_device_count = block_device_count();

    for (
        uint32_t index = 0;
        index < raw_device_count;
        index++
    )
    {
        scan_mbr_device(
            block_device_get(index),
            index
        );
    }
}

uint32_t partition_count(void)
{
    return detected_count;
}

const partition_t *partition_get(uint32_t index)
{
    if (index >= detected_count)
    {
        return NULL;
    }

    return &partitions[index];
}

const partition_t *partition_find_type(uint8_t type)
{
    const block_device_t *primary =
        block_device_primary();

    for (uint32_t index = 0; index < detected_count; index++)
    {
        if (
            partitions[index].scheme == PARTITION_SCHEME_MBR &&
            partitions[index].type == type &&
            partitions[index].device == primary
        )
        {
            return &partitions[index];
        }
    }

    for (uint32_t index = 0; index < detected_count; index++)
    {
        if (
            partitions[index].scheme == PARTITION_SCHEME_MBR &&
            partitions[index].type == type
        )
        {
            return &partitions[index];
        }
    }

    return NULL;
}

static bool primary_has_partition(void)
{
    const block_device_t *primary =
        block_device_primary();

    for (uint32_t index = 0; index < detected_count; index++)
    {
        if (partitions[index].device == primary)
        {
            return true;
        }
    }

    return false;
}

bool partition_create_latteros_fs(void)
{
    partition_init();

    const block_device_t *device =
        block_device_primary();

    if (
        device == NULL ||
        !device->writable ||
        device->sector_size != 512U ||
        device->sector_count <=
            LATTEROS_FS_PARTITION_START + 4096U ||
        primary_has_partition()
    )
    {
        return false;
    }

    clear_bytes(sector_buffer, sizeof(sector_buffer));

    mbr_partition_entry_t *entry =
        (mbr_partition_entry_t *)(
            &sector_buffer[MBR_PARTITION_TABLE_OFFSET]
        );

    uint64_t available =
        device->sector_count -
        LATTEROS_FS_PARTITION_START;

    if (available > UINT32_MAX)
    {
        available = UINT32_MAX;
    }

    entry->status = 0;
    entry->type = PARTITION_TYPE_LATTEROS_FS;
    entry->first_lba = LATTEROS_FS_PARTITION_START;
    entry->sector_count = (uint32_t)available;

    sector_buffer[MBR_SIGNATURE_OFFSET] =
        MBR_SIGNATURE_LOW;
    sector_buffer[MBR_SIGNATURE_OFFSET + 1U] =
        MBR_SIGNATURE_HIGH;

    if (!block_device_write(
        device,
        0,
        1,
        sector_buffer
    ))
    {
        return false;
    }

    if (!append_partition(
        device,
        0,
        1,
        PARTITION_SCHEME_MBR,
        PARTITION_TYPE_LATTEROS_FS,
        NULL,
        NULL,
        NULL,
        false,
        LATTEROS_FS_PARTITION_START,
        available
    ))
    {
        return false;
    }

    return
        partition_find_type(
            PARTITION_TYPE_LATTEROS_FS
        ) != NULL;
}

bool partition_read(
    const partition_t *partition,
    uint64_t relative_lba,
    uint32_t sector_count,
    void *buffer
)
{
    if (
        partition == NULL ||
        buffer == NULL ||
        sector_count == 0 ||
        relative_lba >= partition->sector_count ||
        sector_count >
            partition->sector_count - relative_lba
    )
    {
        return false;
    }

    return block_device_read(
        partition->device,
        partition->start_lba + relative_lba,
        sector_count,
        buffer
    );
}

bool partition_write(
    const partition_t *partition,
    uint64_t relative_lba,
    uint32_t sector_count,
    const void *buffer
)
{
    if (
        partition == NULL ||
        buffer == NULL ||
        sector_count == 0 ||
        relative_lba >= partition->sector_count ||
        sector_count >
            partition->sector_count - relative_lba
    )
    {
        return false;
    }

    return block_device_write(
        partition->device,
        partition->start_lba + relative_lba,
        sector_count,
        buffer
    );
}

static void print_guid(const uint8_t *guid)
{
    kprintf(
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        (unsigned int)guid[3],
        (unsigned int)guid[2],
        (unsigned int)guid[1],
        (unsigned int)guid[0],
        (unsigned int)guid[5],
        (unsigned int)guid[4],
        (unsigned int)guid[7],
        (unsigned int)guid[6],
        (unsigned int)guid[8],
        (unsigned int)guid[9],
        (unsigned int)guid[10],
        (unsigned int)guid[11],
        (unsigned int)guid[12],
        (unsigned int)guid[13],
        (unsigned int)guid[14],
        (unsigned int)guid[15]
    );
}

void partition_print(void)
{
    kprintf(
        "Partitions: %u\n",
        (unsigned int)detected_count
    );

    for (uint32_t index = 0; index < detected_count; index++)
    {
        const partition_t *partition =
            &partitions[index];

        if (partition->scheme == PARTITION_SCHEME_GPT)
        {
            kprintf(
                "[%u] disk%u part%u GPT start=%llu sectors=%llu (%llu MiB) label=%s\n",
                (unsigned int)index,
                (unsigned int)partition->source_device_index,
                (unsigned int)partition->number,
                (unsigned long long)partition->start_lba,
                (unsigned long long)partition->sector_count,
                (unsigned long long)(
                    partition->sector_count *
                    partition->device->sector_size /
                    (1024ULL * 1024ULL)
                ),
                partition->label[0] != '\0' ?
                    partition->label : "(none)"
            );

            kprintf("    type-guid=");
            print_guid(partition->type_guid);
            kprintf("\n");
        }
        else
        {
            kprintf(
                "[%u] disk%u part%u MBR type=%02x boot=%s start=%llu sectors=%llu (%llu MiB)\n",
                (unsigned int)index,
                (unsigned int)partition->source_device_index,
                (unsigned int)partition->number,
                (unsigned int)partition->type,
                partition->bootable ? "yes" : "no",
                (unsigned long long)partition->start_lba,
                (unsigned long long)partition->sector_count,
                (unsigned long long)(
                    partition->sector_count *
                    partition->device->sector_size /
                    (1024ULL * 1024ULL)
                )
            );
        }

        kprintf(
            "    block-device=%s writable=%s\n",
            partition->name,
            partition->block_device.writable ?
                "yes" : "no"
        );
    }
}
