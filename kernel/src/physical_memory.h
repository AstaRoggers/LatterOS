#ifndef PHYSICAL_MEMORY_H
#define PHYSICAL_MEMORY_H

#include <stdint.h>
#include <limine.h>

#define PAGE_SIZE 4096

typedef struct
{
    uint64_t base;
    uint64_t length;
    uint8_t usable;
} memory_region_t;

void physical_memory_init(
    struct limine_memmap_response *memory_map
);

uint64_t physical_memory_total_bytes(void);
uint64_t physical_memory_usable_bytes(void);
uint64_t physical_memory_usable_pages(void);

uint64_t physical_memory_entry_count(void);

memory_region_t physical_memory_entry(
    uint64_t index
);

#endif