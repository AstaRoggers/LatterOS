#include "heap.h"

#include "hhdm.h"
#include "page_allocator.h"
#include "physical_memory.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HEAP_ALIGNMENT 16

typedef struct heap_block
{
    size_t size;
    bool free;

    struct heap_block *previous;
    struct heap_block *next;
} heap_block_t;

static heap_block_t *first_block;
static heap_block_t *last_block;

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

    new_block->size =
        remaining_size - sizeof(heap_block_t);

    new_block->free = true;
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

    block->size =
        PAGE_SIZE - sizeof(heap_block_t);

    block->free = true;
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

    return block;
}

static void merge_with_next(
    heap_block_t *block
)
{
    heap_block_t *next = block->next;

    if (
        next == NULL ||
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
}

void heap_init(void)
{
    first_block = NULL;
    last_block = NULL;
}

void *kmalloc(size_t size)
{
    if (size == 0)
    {
        return NULL;
    }

    size = align_size(size);

    if (
        size >
        PAGE_SIZE - sizeof(heap_block_t)
    )
    {
        return NULL;
    }

    heap_block_t *block =
        find_free_block(size);

    if (block == NULL)
    {
        block = request_page();

        if (block == NULL)
        {
            return NULL;
        }
    }

    split_block(block, size);

    block->free = false;

    return (void *)(
        (uint8_t *)block +
        sizeof(heap_block_t)
    );
}

void kfree(void *pointer)
{
    if (pointer == NULL)
    {
        return;
    }

    heap_block_t *block =
        (heap_block_t *)(
            (uint8_t *)pointer -
            sizeof(heap_block_t)
        );

    block->free = true;

    merge_with_next(block);

    if (
        block->previous != NULL &&
        block->previous->free
    )
    {
        merge_with_next(block->previous);
    }
}