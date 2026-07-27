#include "block_device.h"

#include "block_cache.h"
#include "spinlock.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static block_device_t devices[BLOCK_DEVICE_MAX];
static uint32_t device_slots;
static spinlock_t registry_lock;

static bool device_online(
    const block_device_t *device
)
{
    return device != NULL &&
        __atomic_load_n(
            &device->online,
            __ATOMIC_ACQUIRE
        );
}

static bool device_acquire(
    const block_device_t *device
)
{
    if (!device_online(device))
    {
        return false;
    }

    __atomic_add_fetch(
        &((block_device_t *)device)->io_references,
        1U,
        __ATOMIC_ACQ_REL
    );

    if (!device_online(device))
    {
        __atomic_sub_fetch(
            &((block_device_t *)device)->io_references,
            1U,
            __ATOMIC_RELEASE
        );

        return false;
    }

    return true;
}

static void device_release(
    const block_device_t *device
)
{
    __atomic_sub_fetch(
        &((block_device_t *)device)->io_references,
        1U,
        __ATOMIC_RELEASE
    );
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

void block_device_init(void)
{
    device_slots = 0;
    spinlock_init(&registry_lock);
    block_cache_init();

    for (
        uint32_t index = 0;
        index < BLOCK_DEVICE_MAX;
        index++
    )
    {
        __atomic_store_n(
            &devices[index].online,
            false,
            __ATOMIC_RELEASE
        );
        devices[index].io_references = 0;
    }
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
        device->read == NULL
    )
    {
        return false;
    }

    uint64_t flags =
        spinlock_lock_irqsave(&registry_lock);

    uint32_t selected = BLOCK_DEVICE_MAX;

    if (device->context != NULL)
    {
        for (
            uint32_t index = 0;
            index < device_slots;
            index++
        )
        {
            if (devices[index].context == device->context)
            {
                selected = index;
                break;
            }
        }
    }

    if (selected == BLOCK_DEVICE_MAX)
    {
        for (
            uint32_t index = 0;
            index < device_slots;
            index++
        )
        {
            if (!device_online(&devices[index]))
            {
                selected = index;
                break;
            }
        }
    }

    if (selected == BLOCK_DEVICE_MAX)
    {
        if (device_slots >= BLOCK_DEVICE_MAX)
        {
            spinlock_unlock_irqrestore(
                &registry_lock,
                flags
            );

            return false;
        }

        selected = device_slots++;
    }

    block_cache_invalidate_device(
        &devices[selected]
    );

    devices[selected] = *device;
    devices[selected].io_references = 0;
    devices[selected].removable =
        device->removable ||
        string_starts_with(
            device->name,
            "USB mass storage"
        );

    __atomic_store_n(
        &devices[selected].online,
        true,
        __ATOMIC_RELEASE
    );

    spinlock_unlock_irqrestore(
        &registry_lock,
        flags
    );

    return true;
}

uint32_t block_device_count(void)
{
    return device_slots;
}

uint32_t block_device_online_count(void)
{
    uint64_t flags =
        spinlock_lock_irqsave(&registry_lock);

    uint32_t count = 0;

    for (
        uint32_t index = 0;
        index < device_slots;
        index++
    )
    {
        if (device_online(&devices[index]))
        {
            count++;
        }
    }

    spinlock_unlock_irqrestore(
        &registry_lock,
        flags
    );

    return count;
}

const block_device_t *block_device_get(
    uint32_t index
)
{
    if (
        index >= device_slots ||
        !device_online(&devices[index])
    )
    {
        return NULL;
    }

    return &devices[index];
}

const block_device_t *block_device_primary(void)
{
    for (
        uint32_t index = 0;
        index < device_slots;
        index++
    )
    {
        const block_device_t *device =
            block_device_get(index);

        if (device != NULL)
        {
            return device;
        }
    }

    return NULL;
}

const block_device_t *block_device_find_context(
    const void *context
)
{
    if (context == NULL)
    {
        return NULL;
    }

    for (
        uint32_t index = 0;
        index < device_slots;
        index++
    )
    {
        if (
            device_online(&devices[index]) &&
            devices[index].context == context
        )
        {
            return &devices[index];
        }
    }

    return NULL;
}

bool block_device_sync(
    const block_device_t *device
)
{
    if (device == NULL)
    {
        return false;
    }

    if (!block_cache_device_supported(device))
    {
        return device_online(device);
    }

    return block_cache_flush_device(device);
}

bool block_device_sync_prefix(
    const char *name_prefix
)
{
    if (
        name_prefix == NULL ||
        name_prefix[0] == '\0'
    )
    {
        return false;
    }

    bool matched = false;
    bool success = true;

    for (
        uint32_t index = 0;
        index < device_slots;
        index++
    )
    {
        const block_device_t *device =
            block_device_get(index);

        if (
            device == NULL ||
            !string_starts_with(
                device->name,
                name_prefix
            )
        )
        {
            continue;
        }

        matched = true;

        if (!block_device_sync(device))
        {
            success = false;
        }
    }

    return matched && success;
}

bool block_device_sync_all(void)
{
    bool success = true;

    for (
        uint32_t index = 0;
        index < device_slots;
        index++
    )
    {
        const block_device_t *device =
            block_device_get(index);

        if (
            device != NULL &&
            !block_device_sync(device)
        )
        {
            success = false;
        }
    }

    return success;
}

uint32_t block_device_unregister_prefix(
    const char *name_prefix
)
{
    if (
        name_prefix == NULL ||
        name_prefix[0] == '\0'
    )
    {
        return 0;
    }

    block_device_t *selected[BLOCK_DEVICE_MAX];
    uint32_t selected_count = 0;

    uint64_t flags =
        spinlock_lock_irqsave(&registry_lock);

    for (
        uint32_t index = 0;
        index < device_slots &&
            selected_count < BLOCK_DEVICE_MAX;
        index++
    )
    {
        if (
            device_online(&devices[index]) &&
            string_starts_with(
                devices[index].name,
                name_prefix
            )
        )
        {
            selected[selected_count++] =
                &devices[index];
        }
    }

    spinlock_unlock_irqrestore(
        &registry_lock,
        flags
    );

    for (
        uint32_t index = 0;
        index < selected_count;
        index++
    )
    {
        (void)block_device_sync(selected[index]);
    }

    flags = spinlock_lock_irqsave(&registry_lock);

    for (
        uint32_t index = 0;
        index < selected_count;
        index++
    )
    {
        __atomic_store_n(
            &selected[index]->online,
            false,
            __ATOMIC_RELEASE
        );
    }

    spinlock_unlock_irqrestore(
        &registry_lock,
        flags
    );

    for (
        uint32_t index = 0;
        index < selected_count;
        index++
    )
    {
        while (
            __atomic_load_n(
                &selected[index]->io_references,
                __ATOMIC_ACQUIRE
            ) != 0
        )
        {
            __asm__ volatile("pause");
        }

        block_cache_invalidate_device(
            selected[index]
        );
    }

    return selected_count;
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
        sector_count > device->sector_count - lba ||
        !device_acquire(device)
    )
    {
        return false;
    }

    bool success;

    if (block_cache_device_supported(device))
    {
        success = block_cache_read(
            device,
            lba,
            sector_count,
            buffer
        );
    }
    else
    {
        success = device->read(
            device->context,
            lba,
            sector_count,
            buffer
        );
    }

    device_release(device);
    return success;
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
        sector_count > device->sector_count - lba ||
        !device_acquire(device)
    )
    {
        return false;
    }

    bool success;

    if (block_cache_device_supported(device))
    {
        success = block_cache_write(
            device,
            lba,
            sector_count,
            buffer
        );
    }
    else
    {
        success = device->write(
            device->context,
            lba,
            sector_count,
            buffer
        );
    }

    device_release(device);
    return success;
}
