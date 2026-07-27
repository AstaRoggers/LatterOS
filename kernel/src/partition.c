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


typedef struct __attribute__((packed))
{
    uint8_t status;
    uint8_t first_chs[3];
    uint8_t type;
    uint8_t last_chs[3];
    uint32_t first_lba;
    uint32_t sector_count;
} mbr_partition_entry_t;

static partition_t partitions[PARTITION_MAX_COUNT];
static uint32_t detected_count;
static uint32_t raw_device_count;
static bool initialized;
static uint8_t sector_buffer[512];

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

static void build_partition_name(partition_t *partition)
{
    const char *prefix = "Disk partition ";

    if (
        partition->device != NULL &&
        string_starts_with(
            partition->device->name,
            "AHCI SATA"
        )
    )
    {
        prefix = "AHCI partition ";
    }
    else if (
        partition->device != NULL &&
        string_starts_with(
            partition->device->name,
            "ATA"
        )
    )
    {
        prefix = "ATA partition ";
    }

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
    uint8_t type,
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
    partition->scheme = PARTITION_SCHEME_MBR;
    partition->type = type;
    partition->bootable = bootable;
    partition->start_lba = start_lba;
    partition->sector_count = sector_count;
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
            entry->type,
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
            partitions[index].type == type &&
            partitions[index].device == primary
        )
        {
            return &partitions[index];
        }
    }

    for (uint32_t index = 0; index < detected_count; index++)
    {
        if (partitions[index].type == type)
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

    if (
        !block_device_write(
            device,
            0,
            1,
            sector_buffer
        )
    )
    {
        return false;
    }

    if (
        !append_partition(
            device,
            0,
            1,
            PARTITION_TYPE_LATTEROS_FS,
            false,
            LATTEROS_FS_PARTITION_START,
            available
        )
    )
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

        kprintf(
            "    block-device=%s writable=%s\n",
            partition->name,
            partition->block_device.writable ?
                "yes" :
                "no"
        );
    }
}
