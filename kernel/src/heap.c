#include "heap.h"

#include "hhdm.h"
#include "irq.h"
#include "kstdio.h"
#include "page_allocator.h"
#include "physical_memory.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HEAP_ALIGNMENT 16
#define HEAP_BLOCK_MAGIC 0x4C41545445524850ULL
#define HEAP_SNAPSHOT_MAX 64

typedef struct heap_block
{
    uint64_t magic;
    size_t size;
    size_t requested_size;
    uint64_t allocation_id;
    bool free;

    struct heap_block *previous;
    struct heap_block *next;
} heap_block_t;

typedef struct
{
    void *pointer;
    size_t size;
    uint64_t allocation_id;
} heap_allocation_snapshot_t;

static heap_block_t *first_block;
static heap_block_t *last_block;
static heap_stats_t statistics;
static uint64_t next_allocation_id;

static uint64_t interrupt_save_disable(void)
{
    uint64_t flags;

    __asm__ volatile(
        "pushfq\n"
        "popq %0\n"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );

    return flags;
}

static void interrupt_restore(uint64_t flags)
{
    if (flags & (1ULL << 9))
    {
        irq_enable();
    }
}

static size_t align_size(size_t size)
{
    return (
        size + HEAP_ALIGNMENT - 1
    ) & ~(HEAP_ALIGNMENT - 1);
}

static bool blocks_are_adjacent(
    heap_block_t *first,
    heap_block_t *second
)
{
    uint8_t *first_end =
        (uint8_t *)first +
        sizeof(heap_block_t) +
        first->size;

    return first_end == (uint8_t *)second;
}

static void initialize_block(
    heap_block_t *block,
    size_t size
)
{
    block->magic = HEAP_BLOCK_MAGIC;
    block->size = size;
    block->requested_size = 0;
    block->allocation_id = 0;
    block->free = true;
}

static void split_block(
    heap_block_t *block,
    size_t requested_size
)
{
    size_t remaining_size =
        block->size - requested_size;

    if (
        remaining_size <=
        sizeof(heap_block_t) + HEAP_ALIGNMENT
    )
    {
        return;
    }

    heap_block_t *new_block =
        (heap_block_t *)(
            (uint8_t *)block +
            sizeof(heap_block_t) +
            requested_size
        );

    initialize_block(
        new_block,
        remaining_size - sizeof(heap_block_t)
    );

    new_block->previous = block;
    new_block->next = block->next;

    if (block->next != NULL)
    {
        block->next->previous = new_block;
    }
    else
    {
        last_block = new_block;
    }

    block->next = new_block;
    block->size = requested_size;
}

static heap_block_t *find_free_block(
    size_t requested_size
)
{
    heap_block_t *block = first_block;

    while (block != NULL)
    {
        if (
            block->magic == HEAP_BLOCK_MAGIC &&
            block->free &&
            block->size >= requested_size
        )
        {
            return block;
        }

        block = block->next;
    }

    return NULL;
}

static heap_block_t *find_block_by_pointer(
    const void *pointer
)
{
    heap_block_t *block = first_block;

    while (block != NULL)
    {
        void *payload =
            (void *)(
                (uint8_t *)block +
                sizeof(heap_block_t)
            );

        if (payload == pointer)
        {
            return block;
        }

        block = block->next;
    }

    return NULL;
}

static heap_block_t *request_page(void)
{
    void *physical_page = alloc_page();

    if (physical_page == NULL)
    {
        return NULL;
    }

    heap_block_t *block =
        (heap_block_t *)physical_to_virtual(
            (uint64_t)physical_page
        );

    initialize_block(
        block,
        PAGE_SIZE - sizeof(heap_block_t)
    );

    block->previous = last_block;
    block->next = NULL;

    if (last_block != NULL)
    {
        last_block->next = block;
    }
    else
    {
        first_block = block;
    }

    last_block = block;
    statistics.heap_pages++;

    return block;
}

static void merge_with_next(
    heap_block_t *block
)
{
    heap_block_t *next = block->next;

    if (
        next == NULL ||
        next->magic != HEAP_BLOCK_MAGIC ||
        !next->free ||
        !blocks_are_adjacent(block, next)
    )
    {
        return;
    }

    block->size +=
        sizeof(heap_block_t) +
        next->size;

    block->next = next->next;

    if (next->next != NULL)
    {
        next->next->previous = block;
    }
    else
    {
        last_block = block;
    }

    next->magic = 0;
}

void heap_init(void)
{
    first_block = NULL;
    last_block = NULL;

    statistics = (heap_stats_t){0};
    next_allocation_id = 1;
}

void *kmalloc(size_t size)
{
    if (size == 0)
    {
        return NULL;
    }

    size_t aligned_size = align_size(size);

    if (
        aligned_size >
        PAGE_SIZE - sizeof(heap_block_t)
    )
    {
        return NULL;
    }

    uint64_t flags = interrupt_save_disable();

    heap_block_t *block =
        find_free_block(aligned_size);

    if (block == NULL)
    {
        block = request_page();

        if (block == NULL)
        {
            interrupt_restore(flags);
            return NULL;
        }
    }

    split_block(block, aligned_size);

    block->free = false;
    block->requested_size = size;
    block->allocation_id =
        next_allocation_id++;

    statistics.total_allocations++;
    statistics.active_allocations++;
    statistics.active_bytes += size;

    if (
        statistics.active_bytes >
        statistics.peak_active_bytes
    )
    {
        statistics.peak_active_bytes =
            statistics.active_bytes;
    }

    void *pointer =
        (void *)(
            (uint8_t *)block +
            sizeof(heap_block_t)
        );

    interrupt_restore(flags);
    return pointer;
}

void kfree(void *pointer)
{
    if (pointer == NULL)
    {
        return;
    }

    uint64_t flags = interrupt_save_disable();

    heap_block_t *block =
        find_block_by_pointer(pointer);

    if (
        block == NULL ||
        block->magic != HEAP_BLOCK_MAGIC
    )
    {
        statistics.invalid_frees++;
        interrupt_restore(flags);
        return;
    }

    if (block->free)
    {
        statistics.double_frees++;
        interrupt_restore(flags);
        return;
    }

    statistics.total_frees++;

    if (statistics.active_allocations > 0)
    {
        statistics.active_allocations--;
    }

    if (
        statistics.active_bytes >=
        block->requested_size
    )
    {
        statistics.active_bytes -=
            block->requested_size;
    }
    else
    {
        statistics.active_bytes = 0;
    }

    block->free = true;
    block->requested_size = 0;
    block->allocation_id = 0;

    merge_with_next(block);

    if (
        block->previous != NULL &&
        block->previous->free
    )
    {
        merge_with_next(block->previous);
    }

    interrupt_restore(flags);
}

void heap_get_stats(heap_stats_t *stats)
{
    if (stats == NULL)
    {
        return;
    }

    uint64_t flags = interrupt_save_disable();
    *stats = statistics;
    interrupt_restore(flags);
}

bool heap_validate(void)
{
    uint64_t flags = interrupt_save_disable();

    heap_block_t *block = first_block;
    heap_block_t *previous = NULL;
    bool valid = true;
    uint64_t active_count = 0;
    uint64_t active_bytes = 0;

    while (block != NULL)
    {
        if (
            block->magic != HEAP_BLOCK_MAGIC ||
            block->previous != previous ||
            block->size == 0 ||
            block->size > PAGE_SIZE
        )
        {
            valid = false;
            break;
        }

        if (!block->free)
        {
            active_count++;
            active_bytes +=
                block->requested_size;
        }

        previous = block;
        block = block->next;
    }

    if (
        previous != last_block ||
        active_count !=
            statistics.active_allocations ||
        active_bytes !=
            statistics.active_bytes
    )
    {
        valid = false;
    }

    interrupt_restore(flags);
    return valid;
}

void heap_print_stats(void)
{
    heap_stats_t stats;
    heap_get_stats(&stats);

    kprintf(
        "Heap: pages=%llu active=%llu bytes=%llu peak=%llu\n",
        (unsigned long long)stats.heap_pages,
        (unsigned long long)stats.active_allocations,
        (unsigned long long)stats.active_bytes,
        (unsigned long long)stats.peak_active_bytes
    );

    kprintf(
        "Calls: alloc=%llu free=%llu invalid=%llu double=%llu\n",
        (unsigned long long)stats.total_allocations,
        (unsigned long long)stats.total_frees,
        (unsigned long long)stats.invalid_frees,
        (unsigned long long)stats.double_frees
    );

    kprintf(
        "Heap integrity: %s\n",
        heap_validate() ? "passed" : "FAILED"
    );
}

void heap_print_allocations(uint32_t maximum)
{
    if (
        maximum == 0 ||
        maximum > HEAP_SNAPSHOT_MAX
    )
    {
        maximum = HEAP_SNAPSHOT_MAX;
    }

    heap_allocation_snapshot_t snapshot[
        HEAP_SNAPSHOT_MAX
    ];

    uint32_t count = 0;
    uint64_t flags = interrupt_save_disable();
    heap_block_t *block = first_block;

    while (
        block != NULL &&
        count < maximum
    )
    {
        if (!block->free)
        {
            snapshot[count].pointer =
                (void *)(
                    (uint8_t *)block +
                    sizeof(heap_block_t)
                );

            snapshot[count].size =
                block->requested_size;

            snapshot[count].allocation_id =
                block->allocation_id;

            count++;
        }

        block = block->next;
    }

    uint64_t total_active =
        statistics.active_allocations;

    interrupt_restore(flags);

    if (count == 0)
    {
        kprintf("No active heap allocations.\n");
        return;
    }

    for (
        uint32_t index = 0;
        index < count;
        index++
    )
    {
        kprintf(
            "#%llu ptr=%p size=%zu\n",
            (unsigned long long)
                snapshot[index].allocation_id,
            snapshot[index].pointer,
            snapshot[index].size
        );
    }

    if (total_active > count)
    {
        kprintf(
            "... %llu more active allocations\n",
            (unsigned long long)(
                total_active - count
            )
        );
    }
}
