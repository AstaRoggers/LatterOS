#ifndef HEAP_H
#define HEAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    uint64_t total_allocations;
    uint64_t total_frees;
    uint64_t active_allocations;
    uint64_t active_bytes;
    uint64_t peak_active_bytes;
    uint64_t heap_pages;
    uint64_t invalid_frees;
    uint64_t double_frees;
} heap_stats_t;

void heap_init(void);

void *kmalloc(size_t size);
void kfree(void *pointer);

void heap_get_stats(heap_stats_t *stats);
bool heap_validate(void);
void heap_print_stats(void);
void heap_print_allocations(uint32_t maximum);

#endif
