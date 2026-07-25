#include "physical_memory.h"

#include <stddef.h>
#include <stdint.h>

#define MAX_MEMORY_REGIONS 256

static uint64_t total_bytes;
static uint64_t usable_bytes;
static uint64_t usable_pages;

static memory_region_t memory_regions[MAX_MEMORY_REGIONS];
static uint64_t memory_region_count;

void physical_memory_init(
    struct limine_memmap_response *memory_map
)
{
    total_bytes = 0;
    usable_bytes = 0;
    usable_pages = 0;
    memory_region_count = 0;

    if (memory_map == NULL)
    {
        return;
    }

    for (
        uint64_t i = 0;
        i < memory_map->entry_count;
        i++
    )
    {
        struct limine_memmap_entry *entry =
            memory_map->entries[i];

        if (entry == NULL)
        {
            continue;
        }

        total_bytes += entry->length;

        if (memory_region_count < MAX_MEMORY_REGIONS)
        {
            memory_regions[memory_region_count].base =
                entry->base;

            memory_regions[memory_region_count].length =
                entry->length;

            memory_regions[memory_region_count].usable =
                entry->type == LIMINE_MEMMAP_USABLE;

            memory_region_count++;
        }

        if (entry->type == LIMINE_MEMMAP_USABLE)
        {
            usable_bytes += entry->length;
            usable_pages += entry->length / PAGE_SIZE;
        }
    }
}

uint64_t physical_memory_total_bytes(void)
{
    return total_bytes;
}

uint64_t physical_memory_usable_bytes(void)
{
    return usable_bytes;
}

uint64_t physical_memory_usable_pages(void)
{
    return usable_pages;
}

uint64_t physical_memory_entry_count(void)
{
    return memory_region_count;
}

memory_region_t physical_memory_entry(
    uint64_t index
)
{
    memory_region_t empty_region = {
        .base = 0,
        .length = 0,
        .usable = 0
    };

    if (index >= memory_region_count)
    {
        return empty_region;
    }

    return memory_regions[index];
}