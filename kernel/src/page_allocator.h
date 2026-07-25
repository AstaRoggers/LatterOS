#ifndef PAGE_ALLOCATOR_H
#define PAGE_ALLOCATOR_H

#include <stdint.h>

void page_allocator_init(void);

void *alloc_page(void);
void free_page(void *page);

uint64_t free_page_count(void);

#endif