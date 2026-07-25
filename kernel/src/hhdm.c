#include "hhdm.h"

#include <stddef.h>
#include <stdint.h>

static uint64_t offset;

void hhdm_init(
    struct limine_hhdm_response *response
)
{
    if (response == NULL)
    {
        offset = 0;
        return;
    }

    offset = response->offset;
}

void *physical_to_virtual(
    uint64_t physical_address
)
{
    return (void *)(physical_address + offset);
}

uint64_t virtual_to_physical(
    const void *virtual_address
)
{
    return (uint64_t)virtual_address - offset;
}

uint64_t hhdm_offset(void)
{
    return offset;
}