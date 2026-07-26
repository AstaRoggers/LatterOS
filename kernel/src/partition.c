#include "partition.h"

#include <stddef.h>
#include <stdint.h>

#define MBR_PARTITION_TABLE_OFFSET 446
#define MBR_SIGNATURE_OFFSET       510
#define MBR_SIGNATURE_LOW          0x55
#define MBR_SIGNATURE_HIGH         0xAA
#define LATTEROS_FS_PARTITION_START   2048U

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
static uint8_t sector_buffer[512];

static void clear_bytes(
    void *pointer,
    uint32_t count
)
{
    uint8_t *bytes = pointer;

    for (
        uint32_t index = 0;
        index < count;
        index++
    )
    {
        bytes[index] = 0;
    }
}

void partition_init(void)
{
    detected_count = 0;

    const block_device_t *device =
        block_device_primary();

    if (
        device == NULL ||
        device->sector_size != 512 ||
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
        sector_buffer[MBR_SIGNATURE_OFFSET + 1] !=
            MBR_SIGNATURE_HIGH
    )
    {
        return;
    }

    const mbr_partition_entry_t *entries =
        (const mbr_partition_entry_t *)(
            &sector_buffer[
                MBR_PARTITION_TABLE_OFFSET
            ]
        );

    for (
        uint32_t index = 0;
        index < PARTITION_MAX_COUNT;
        index++
    )
    {
        const mbr_partition_entry_t *entry =
            &entries[index];

        if (
            entry->type == 0 ||
            entry->sector_count == 0
        )
        {
            continue;
        }

        uint64_t start = entry->first_lba;
        uint64_t count = entry->sector_count;

        if (
            start >= device->sector_count ||
            count > device->sector_count - start
        )
        {
            continue;
        }

        partition_t *partition =
            &partitions[detected_count];

        partition->device = device;
        partition->type = entry->type;
        partition->start_lba = start;
        partition->sector_count = count;

        detected_count++;

        if (
            detected_count >=
            PARTITION_MAX_COUNT
        )
        {
            break;
        }
    }
}

uint32_t partition_count(void)
{
    return detected_count;
}

const partition_t *partition_get(
    uint32_t index
)
{
    if (index >= detected_count)
    {
        return NULL;
    }

    return &partitions[index];
}

const partition_t *partition_find_type(
    uint8_t type
)
{
    for (
        uint32_t index = 0;
        index < detected_count;
        index++
    )
    {
        if (partitions[index].type == type)
        {
            return &partitions[index];
        }
    }

    return NULL;
}

bool partition_create_latteros_fs(void)
{
    const block_device_t *device =
        block_device_primary();

    if (
        device == NULL ||
        !device->writable ||
        device->sector_size != 512 ||
        device->sector_count <=
            LATTEROS_FS_PARTITION_START + 4096
    )
    {
        return false;
    }

    if (detected_count != 0)
    {
        return false;
    }

    clear_bytes(
        sector_buffer,
        sizeof(sector_buffer)
    );

    mbr_partition_entry_t *entry =
        (mbr_partition_entry_t *)(
            &sector_buffer[
                MBR_PARTITION_TABLE_OFFSET
            ]
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
    entry->first_lba =
        LATTEROS_FS_PARTITION_START;
    entry->sector_count =
        (uint32_t)available;

    sector_buffer[MBR_SIGNATURE_OFFSET] =
        MBR_SIGNATURE_LOW;

    sector_buffer[MBR_SIGNATURE_OFFSET + 1] =
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

    partition_init();

    return (
        partition_find_type(
            PARTITION_TYPE_LATTEROS_FS
        ) != NULL
    );
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
        relative_lba >=
            partition->sector_count ||
        sector_count >
            partition->sector_count -
            relative_lba
    )
    {
        return false;
    }

    return block_device_read(
        partition->device,
        partition->start_lba +
            relative_lba,
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
        relative_lba >=
            partition->sector_count ||
        sector_count >
            partition->sector_count -
            relative_lba
    )
    {
        return false;
    }

    return block_device_write(
        partition->device,
        partition->start_lba +
            relative_lba,
        sector_count,
        buffer
    );
}
