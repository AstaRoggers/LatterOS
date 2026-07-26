#include "block_device.h"

#include <stddef.h>
#include <stdint.h>

static block_device_t devices[BLOCK_DEVICE_MAX];
static uint32_t device_count;

void block_device_init(void)
{
    device_count = 0;
}

bool block_device_register(
    const block_device_t *device
)
{
    if (
        device == NULL ||
        device->name == NULL ||
        device->sector_size == 0 ||
        device->sector_count == 0 ||
        device->read == NULL ||
        device_count >= BLOCK_DEVICE_MAX
    )
    {
        return false;
    }

    devices[device_count] = *device;
    device_count++;

    return true;
}

uint32_t block_device_count(void)
{
    return device_count;
}

const block_device_t *block_device_get(
    uint32_t index
)
{
    if (index >= device_count)
    {
        return NULL;
    }

    return &devices[index];
}

const block_device_t *block_device_primary(void)
{
    return block_device_get(0);
}

bool block_device_read(
    const block_device_t *device,
    uint64_t lba,
    uint32_t sector_count,
    void *buffer
)
{
    if (
        device == NULL ||
        device->read == NULL ||
        buffer == NULL ||
        sector_count == 0 ||
        lba >= device->sector_count ||
        sector_count > device->sector_count - lba
    )
    {
        return false;
    }

    return device->read(
        device->context,
        lba,
        sector_count,
        buffer
    );
}

bool block_device_write(
    const block_device_t *device,
    uint64_t lba,
    uint32_t sector_count,
    const void *buffer
)
{
    if (
        device == NULL ||
        !device->writable ||
        device->write == NULL ||
        buffer == NULL ||
        sector_count == 0 ||
        lba >= device->sector_count ||
        sector_count > device->sector_count - lba
    )
    {
        return false;
    }

    return device->write(
        device->context,
        lba,
        sector_count,
        buffer
    );
}
