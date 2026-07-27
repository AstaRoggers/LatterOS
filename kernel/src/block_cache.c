#include "block_cache.h"

#include "block_device.h"
#include "process.h"
#include "spinlock.h"
#include "timer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BLOCK_CACHE_ENTRY_COUNT       64U
#define BLOCK_CACHE_MAX_SECTOR_SIZE   4096U
#define BLOCK_CACHE_FLUSH_BUDGET      8U
#define BLOCK_CACHE_FLUSH_PERIOD_MS   250U
#define BLOCK_CACHE_DIRTY_AGE_MS      1000U
#define SCHEDULER_VECTOR              0x81

typedef struct
{
    bool valid;
    bool dirty;
    const block_device_t *device;
    uint64_t lba;
    uint32_t sector_size;
    uint64_t last_access_tick;
    uint64_t dirty_since_tick;
    uint8_t data[BLOCK_CACHE_MAX_SECTOR_SIZE];
} block_cache_entry_t;

static block_cache_entry_t entries[BLOCK_CACHE_ENTRY_COUNT];
static spinlock_t cache_lock;
static bool initialized;
static bool worker_started;
static uint64_t access_clock;
static block_cache_stats_t statistics;

static void clear_bytes(
    void *pointer,
    size_t count
)
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

static uint64_t milliseconds_to_ticks(
    uint32_t milliseconds
)
{
    uint64_t frequency = timer_frequency();

    if (frequency == 0)
    {
        frequency = 1000;
    }

    uint64_t ticks =
        frequency * milliseconds / 1000ULL;

    return ticks == 0 ? 1 : ticks;
}

static void yield_current_thread(void)
{
    __asm__ volatile(
        "int $0x81"
        :
        :
        : "memory"
    );
}

static block_cache_entry_t *find_entry_locked(
    const block_device_t *device,
    uint64_t lba
)
{
    for (
        uint32_t index = 0;
        index < BLOCK_CACHE_ENTRY_COUNT;
        index++
    )
    {
        block_cache_entry_t *entry = &entries[index];

        if (
            entry->valid &&
            entry->device == device &&
            entry->lba == lba
        )
        {
            return entry;
        }
    }

    return NULL;
}

static bool flush_entry_locked(
    block_cache_entry_t *entry
)
{
    if (
        entry == NULL ||
        !entry->valid ||
        !entry->dirty
    )
    {
        return true;
    }

    const block_device_t *device = entry->device;

    if (
        device == NULL ||
        device->write == NULL ||
        !device->writable ||
        !device_acquire(device)
    )
    {
        statistics.flush_failures++;
        return false;
    }

    bool success = device->write(
        device->context,
        entry->lba,
        1,
        entry->data
    );

    device_release(device);

    if (!success)
    {
        statistics.flush_failures++;
        return false;
    }

    entry->dirty = false;
    entry->dirty_since_tick = 0;
    statistics.flushes++;
    return true;
}

static block_cache_entry_t *select_entry_locked(void)
{
    block_cache_entry_t *oldest = NULL;

    for (
        uint32_t index = 0;
        index < BLOCK_CACHE_ENTRY_COUNT;
        index++
    )
    {
        block_cache_entry_t *entry = &entries[index];

        if (!entry->valid)
        {
            return entry;
        }

        if (
            oldest == NULL ||
            entry->last_access_tick <
                oldest->last_access_tick
        )
        {
            oldest = entry;
        }
    }

    if (
        oldest != NULL &&
        oldest->dirty &&
        !flush_entry_locked(oldest)
    )
    {
        return NULL;
    }

    if (oldest != NULL)
    {
        statistics.evictions++;
    }

    return oldest;
}

static block_cache_entry_t *prepare_entry_locked(
    const block_device_t *device,
    uint64_t lba
)
{
    block_cache_entry_t *entry =
        find_entry_locked(device, lba);

    if (entry != NULL)
    {
        return entry;
    }

    entry = select_entry_locked();

    if (entry == NULL)
    {
        return NULL;
    }

    entry->valid = true;
    entry->dirty = false;
    entry->device = device;
    entry->lba = lba;
    entry->sector_size = device->sector_size;
    entry->dirty_since_tick = 0;
    entry->last_access_tick = ++access_clock;
    return entry;
}

static bool insert_clean_sector(
    const block_device_t *device,
    uint64_t lba,
    const void *data
)
{
    uint64_t flags =
        spinlock_lock_irqsave(&cache_lock);

    block_cache_entry_t *entry =
        prepare_entry_locked(device, lba);

    if (entry == NULL)
    {
        spinlock_unlock_irqrestore(
            &cache_lock,
            flags
        );

        return false;
    }

    if (!entry->dirty)
    {
        copy_bytes(
            entry->data,
            data,
            device->sector_size
        );
    }

    entry->last_access_tick = ++access_clock;

    spinlock_unlock_irqrestore(
        &cache_lock,
        flags
    );

    return true;
}

static uint32_t flush_aged_entries(
    uint64_t now,
    uint64_t minimum_age,
    uint32_t budget
)
{
    uint64_t flags =
        spinlock_lock_irqsave(&cache_lock);

    uint32_t flushed = 0;

    for (
        uint32_t index = 0;
        index < BLOCK_CACHE_ENTRY_COUNT &&
            flushed < budget;
        index++
    )
    {
        block_cache_entry_t *entry = &entries[index];

        if (
            !entry->valid ||
            !entry->dirty ||
            entry->dirty_since_tick == 0 ||
            now - entry->dirty_since_tick <
                minimum_age
        )
        {
            continue;
        }

        if (flush_entry_locked(entry))
        {
            flushed++;
        }
    }

    spinlock_unlock_irqrestore(
        &cache_lock,
        flags
    );

    return flushed;
}

static void cache_worker(void *argument)
{
    (void)argument;

    uint64_t last_flush = timer_ticks();

    for (;;)
    {
        uint64_t now = timer_ticks();
        uint64_t period = milliseconds_to_ticks(
            BLOCK_CACHE_FLUSH_PERIOD_MS
        );

        if (now - last_flush >= period)
        {
            last_flush = now;

            (void)flush_aged_entries(
                now,
                milliseconds_to_ticks(
                    BLOCK_CACHE_DIRTY_AGE_MS
                ),
                BLOCK_CACHE_FLUSH_BUDGET
            );
        }

        yield_current_thread();
    }
}

void block_cache_init(void)
{
    if (initialized)
    {
        return;
    }

    initialized = true;
    worker_started = false;
    access_clock = 0;
    clear_bytes(entries, sizeof(entries));
    clear_bytes(&statistics, sizeof(statistics));
    spinlock_init(&cache_lock);
}

bool block_cache_start_worker(void)
{
    block_cache_init();

    if (worker_started)
    {
        return true;
    }

    worker_started = process_create_kernel_thread(
        "block-cache",
        cache_worker,
        NULL
    );

    return worker_started;
}

bool block_cache_device_supported(
    const block_device_t *device
)
{
    return
        device != NULL &&
        device->name != NULL &&
        device->sector_size > 0 &&
        device->sector_size <=
            BLOCK_CACHE_MAX_SECTOR_SIZE &&
        string_starts_with(
            device->name,
            "USB mass storage"
        );
}

bool block_cache_read(
    const block_device_t *device,
    uint64_t lba,
    uint32_t sector_count,
    void *buffer
)
{
    if (
        !block_cache_device_supported(device) ||
        device->read == NULL ||
        buffer == NULL ||
        sector_count == 0
    )
    {
        return false;
    }

    uint8_t *output = buffer;

    for (
        uint32_t sector = 0;
        sector < sector_count;
        sector++
    )
    {
        uint64_t current_lba = lba + sector;
        uint8_t *current_output =
            output +
            (uint64_t)sector *
                device->sector_size;

        uint64_t flags =
            spinlock_lock_irqsave(&cache_lock);

        block_cache_entry_t *entry =
            find_entry_locked(
                device,
                current_lba
            );

        if (entry != NULL)
        {
            copy_bytes(
                current_output,
                entry->data,
                device->sector_size
            );
            entry->last_access_tick = ++access_clock;
            statistics.read_hits++;

            spinlock_unlock_irqrestore(
                &cache_lock,
                flags
            );

            continue;
        }

        statistics.read_misses++;

        spinlock_unlock_irqrestore(
            &cache_lock,
            flags
        );

        if (!device->read(
            device->context,
            current_lba,
            1,
            current_output
        ))
        {
            return false;
        }

        (void)insert_clean_sector(
            device,
            current_lba,
            current_output
        );
    }

    return true;
}

bool block_cache_write(
    const block_device_t *device,
    uint64_t lba,
    uint32_t sector_count,
    const void *buffer
)
{
    if (
        !block_cache_device_supported(device) ||
        !device->writable ||
        device->write == NULL ||
        buffer == NULL ||
        sector_count == 0
    )
    {
        return false;
    }

    const uint8_t *input = buffer;

    uint64_t flags =
        spinlock_lock_irqsave(&cache_lock);

    for (
        uint32_t sector = 0;
        sector < sector_count;
        sector++
    )
    {
        uint64_t current_lba = lba + sector;

        block_cache_entry_t *entry =
            find_entry_locked(
                device,
                current_lba
            );

        if (entry != NULL)
        {
            statistics.write_hits++;
        }
        else
        {
            statistics.write_misses++;
            entry = prepare_entry_locked(
                device,
                current_lba
            );
        }

        if (entry == NULL)
        {
            spinlock_unlock_irqrestore(
                &cache_lock,
                flags
            );

            return false;
        }

        copy_bytes(
            entry->data,
            input +
                (uint64_t)sector *
                    device->sector_size,
            device->sector_size
        );

        entry->dirty = true;
        entry->sector_size = device->sector_size;
        entry->last_access_tick = ++access_clock;

        if (entry->dirty_since_tick == 0)
        {
            entry->dirty_since_tick =
                timer_ticks();

            if (entry->dirty_since_tick == 0)
            {
                entry->dirty_since_tick = 1;
            }
        }
    }

    spinlock_unlock_irqrestore(
        &cache_lock,
        flags
    );

    return true;
}

bool block_cache_flush_device(
    const block_device_t *device
)
{
    if (device == NULL)
    {
        return false;
    }

    uint64_t flags =
        spinlock_lock_irqsave(&cache_lock);

    bool success = true;

    for (
        uint32_t index = 0;
        index < BLOCK_CACHE_ENTRY_COUNT;
        index++
    )
    {
        block_cache_entry_t *entry = &entries[index];

        if (
            entry->valid &&
            entry->device == device &&
            entry->dirty &&
            !flush_entry_locked(entry)
        )
        {
            success = false;
        }
    }

    spinlock_unlock_irqrestore(
        &cache_lock,
        flags
    );

    return success;
}

bool block_cache_flush_prefix(
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

    uint64_t flags =
        spinlock_lock_irqsave(&cache_lock);

    bool success = true;
    bool matched = false;

    for (
        uint32_t index = 0;
        index < BLOCK_CACHE_ENTRY_COUNT;
        index++
    )
    {
        block_cache_entry_t *entry = &entries[index];

        if (
            !entry->valid ||
            entry->device == NULL ||
            !string_starts_with(
                entry->device->name,
                name_prefix
            )
        )
        {
            continue;
        }

        matched = true;

        if (
            entry->dirty &&
            !flush_entry_locked(entry)
        )
        {
            success = false;
        }
    }

    spinlock_unlock_irqrestore(
        &cache_lock,
        flags
    );

    return matched && success;
}

bool block_cache_flush_all(void)
{
    uint64_t flags =
        spinlock_lock_irqsave(&cache_lock);

    bool success = true;

    for (
        uint32_t index = 0;
        index < BLOCK_CACHE_ENTRY_COUNT;
        index++
    )
    {
        if (
            entries[index].valid &&
            entries[index].dirty &&
            !flush_entry_locked(&entries[index])
        )
        {
            success = false;
        }
    }

    spinlock_unlock_irqrestore(
        &cache_lock,
        flags
    );

    return success;
}

void block_cache_invalidate_device(
    const block_device_t *device
)
{
    if (device == NULL)
    {
        return;
    }

    uint64_t flags =
        spinlock_lock_irqsave(&cache_lock);

    for (
        uint32_t index = 0;
        index < BLOCK_CACHE_ENTRY_COUNT;
        index++
    )
    {
        if (
            entries[index].valid &&
            entries[index].device == device
        )
        {
            clear_bytes(
                &entries[index],
                sizeof(entries[index])
            );
        }
    }

    spinlock_unlock_irqrestore(
        &cache_lock,
        flags
    );
}

void block_cache_get_stats(
    block_cache_stats_t *stats
)
{
    if (stats == NULL)
    {
        return;
    }

    uint64_t flags =
        spinlock_lock_irqsave(&cache_lock);

    *stats = statistics;
    stats->valid_entries = 0;
    stats->dirty_entries = 0;

    for (
        uint32_t index = 0;
        index < BLOCK_CACHE_ENTRY_COUNT;
        index++
    )
    {
        if (entries[index].valid)
        {
            stats->valid_entries++;
        }

        if (
            entries[index].valid &&
            entries[index].dirty
        )
        {
            stats->dirty_entries++;
        }
    }

    spinlock_unlock_irqrestore(
        &cache_lock,
        flags
    );
}
