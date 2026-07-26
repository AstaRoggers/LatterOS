#include "paging.h"

#include "hhdm.h"
#include "page_allocator.h"
#include "physical_memory.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PAGE_PRESENT  (1ULL << 0)
#define PAGE_WRITABLE (1ULL << 1)
#define PAGE_USER     (1ULL << 2)
#define PAGE_HUGE     (1ULL << 7)

#define PAGE_ADDRESS_MASK \
    0x000FFFFFFFFFF000ULL

static void clear_page(void *page)
{
    uint64_t *words = page;

    for (
        uint32_t index = 0;
        index < PAGE_SIZE / sizeof(uint64_t);
        index++
    )
    {
        words[index] = 0;
    }
}

static uint64_t read_cr3(void)
{
    uint64_t value;

    __asm__ volatile(
        "mov %%cr3, %0"
        : "=r"(value)
    );

    return value;
}

static uint64_t *table_from_entry(
    uint64_t entry
)
{
    return physical_to_virtual(
        entry & PAGE_ADDRESS_MASK
    );
}

static uint64_t *next_table(
    uint64_t *table,
    uint16_t index,
    bool create
)
{
    uint64_t entry = table[index];

    if (entry & PAGE_PRESENT)
    {
        if (entry & PAGE_HUGE)
        {
            return NULL;
        }

        table[index] |=
            PAGE_WRITABLE |
            PAGE_USER;

        return table_from_entry(
            table[index]
        );
    }

    if (!create)
    {
        return NULL;
    }

    void *physical_page =
        alloc_page();

    if (physical_page == NULL)
    {
        return NULL;
    }

    uint64_t *new_table =
        physical_to_virtual(
            (uint64_t)physical_page
        );

    clear_page(new_table);

    table[index] =
        ((uint64_t)physical_page &
            PAGE_ADDRESS_MASK) |
        PAGE_PRESENT |
        PAGE_WRITABLE |
        PAGE_USER;

    return new_table;
}

bool paging_map_user_page(
    uint64_t virtual_address,
    uint64_t physical_address,
    bool writable
)
{
    if (
        (virtual_address & (PAGE_SIZE - 1)) != 0 ||
        (physical_address & (PAGE_SIZE - 1)) != 0
    )
    {
        return false;
    }

    uint16_t pml4_index =
        (uint16_t)((virtual_address >> 39) & 0x1FF);

    uint16_t pdpt_index =
        (uint16_t)((virtual_address >> 30) & 0x1FF);

    uint16_t pd_index =
        (uint16_t)((virtual_address >> 21) & 0x1FF);

    uint16_t pt_index =
        (uint16_t)((virtual_address >> 12) & 0x1FF);

    uint64_t cr3 = read_cr3();

    uint64_t *pml4 =
        physical_to_virtual(
            cr3 & PAGE_ADDRESS_MASK
        );

    uint64_t *pdpt =
        next_table(
            pml4,
            pml4_index,
            true
        );

    if (pdpt == NULL)
    {
        return false;
    }

    uint64_t *pd =
        next_table(
            pdpt,
            pdpt_index,
            true
        );

    if (pd == NULL)
    {
        return false;
    }

    uint64_t *pt =
        next_table(
            pd,
            pd_index,
            true
        );

    if (pt == NULL)
    {
        return false;
    }

    if (pt[pt_index] & PAGE_PRESENT)
    {
        return false;
    }

    uint64_t flags =
        PAGE_PRESENT |
        PAGE_USER;

    if (writable)
    {
        flags |= PAGE_WRITABLE;
    }

    pt[pt_index] =
        (physical_address & PAGE_ADDRESS_MASK) |
        flags;

    __asm__ volatile(
        "invlpg (%0)"
        :
        : "r"(virtual_address)
        : "memory"
    );

    return true;
}

bool paging_unmap_page(
    uint64_t virtual_address
)
{
    if (
        (virtual_address & (PAGE_SIZE - 1)) != 0
    )
    {
        return false;
    }

    uint16_t pml4_index =
        (uint16_t)((virtual_address >> 39) & 0x1FF);

    uint16_t pdpt_index =
        (uint16_t)((virtual_address >> 30) & 0x1FF);

    uint16_t pd_index =
        (uint16_t)((virtual_address >> 21) & 0x1FF);

    uint16_t pt_index =
        (uint16_t)((virtual_address >> 12) & 0x1FF);

    uint64_t cr3 = read_cr3();

    uint64_t *pml4 =
        physical_to_virtual(
            cr3 & PAGE_ADDRESS_MASK
        );

    uint64_t *pdpt =
        next_table(
            pml4,
            pml4_index,
            false
        );

    if (pdpt == NULL)
    {
        return false;
    }

    uint64_t *pd =
        next_table(
            pdpt,
            pdpt_index,
            false
        );

    if (pd == NULL)
    {
        return false;
    }

    uint64_t *pt =
        next_table(
            pd,
            pd_index,
            false
        );

    if (
        pt == NULL ||
        !(pt[pt_index] & PAGE_PRESENT)
    )
    {
        return false;
    }

    pt[pt_index] = 0;

    __asm__ volatile(
        "invlpg (%0)"
        :
        : "r"(virtual_address)
        : "memory"
    );

    return true;
}
