#include "page_allocator.h"
#include "physical_memory.h"

#include <stdint.h>
#include <stddef.h>

#define MAX_PAGES 262144

static void *free_pages[MAX_PAGES];
static uint64_t available_page_count;

static uint64_t align_up(
    uint64_t value,
    uint64_t alignment
)
{
    return (
        value + alignment - 1
    ) & ~(alignment - 1);
}

void page_allocator_init(void)
{
    available_page_count = 0;

    uint64_t region_count =
        physical_memory_entry_count();

    for (
        uint64_t i = 0;
        i < region_count;
        i++
    )
    {
        memory_region_t region =
            physical_memory_entry(i);

        if (!region.usable)
        {
            continue;
        }

        uint64_t start =
            align_up(region.base, PAGE_SIZE);

        uint64_t end =
            region.base + region.length;

        for (
            uint64_t address = start;
            address + PAGE_SIZE <= end;
            address += PAGE_SIZE
        )
        {
            if (available_page_count >= MAX_PAGES)
            {
                return;
            }

            free_pages[available_page_count] =
                (void *)address;

            available_page_count++;
        }
    }
}

void *alloc_page(void)
{
    if (available_page_count == 0)
    {
        return NULL;
    }

    available_page_count--;

    return free_pages[available_page_count];
}

void free_page(void *page)
{
    if (page == NULL)
    {
        return;
    }

    if (available_page_count >= MAX_PAGES)
    {
        return;
    }

    free_pages[available_page_count] = page;
    available_page_count++;
}

uint64_t free_page_count(void)
{
    return available_page_count;
}