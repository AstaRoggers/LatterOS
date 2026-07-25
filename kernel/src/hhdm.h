#ifndef HHDM_H
#define HHDM_H

#include <stdint.h>
#include <limine.h>

void hhdm_init(
    struct limine_hhdm_response *response
);

void *physical_to_virtual(
    uint64_t physical_address
);

uint64_t virtual_to_physical(
    const void *virtual_address
);

uint64_t hhdm_offset(void);

#endif