#include "page_allocator.h"

#include "physical_memory.h"
#include "spinlock.h"

#include <stddef.h>
#include <stdint.h>

#define MAX_PAGES 262144

static void *free_pages[MAX_PAGES];
static uint64_t available_page_count;
static spinlock_t allocator_lock =
    SPINLOCK_INITIALIZER;

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
    spinlock_init(&allocator_lock);
    available_page_count = 0;

    uint64_t region_count =
        physical_memory_entry_count();

    for (
        uint64_t index = 0;
        index < region_count;
        index++
    )
    {
        memory_region_t region =
            physical_memory_entry(index);

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
    uint64_t interrupt_flags =
        spinlock_lock_irqsave(
            &allocator_lock
        );

    void *page = NULL;

    if (available_page_count != 0)
    {
        available_page_count--;
        page = free_pages[available_page_count];
        free_pages[available_page_count] = NULL;
    }

    spinlock_unlock_irqrestore(
        &allocator_lock,
        interrupt_flags
    );

    return page;
}

void free_page(void *page)
{
    if (page == NULL)
    {
        return;
    }

    uint64_t interrupt_flags =
        spinlock_lock_irqsave(
            &allocator_lock
        );

    if (available_page_count < MAX_PAGES)
    {
        free_pages[available_page_count] = page;
        available_page_count++;
    }

    spinlock_unlock_irqrestore(
        &allocator_lock,
        interrupt_flags
    );
}

uint64_t free_page_count(void)
{
    uint64_t interrupt_flags =
        spinlock_lock_irqsave(
            &allocator_lock
        );

    uint64_t count =
        available_page_count;

    spinlock_unlock_irqrestore(
        &allocator_lock,
        interrupt_flags
    );

    return count;
}
