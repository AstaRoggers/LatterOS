#ifndef PAGING_H
#define PAGING_H

#include <stdbool.h>
#include <stdint.h>

bool paging_map_user_page(
    uint64_t virtual_address,
    uint64_t physical_address,
    bool writable
);

bool paging_unmap_page(
    uint64_t virtual_address
);

#endif
