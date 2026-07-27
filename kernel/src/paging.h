#ifndef PAGING_H
#define PAGING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PAGING_TLB_IPI_VECTOR 0xF3

uint64_t paging_kernel_root(void);
uint64_t paging_current_root(void);

bool paging_nx_enabled(void);

uint64_t paging_create_user_space(void);
void paging_destroy_user_space(uint64_t root_physical);
void paging_activate(uint64_t root_physical);

bool paging_map_user_page_in(
    uint64_t root_physical,
    uint64_t virtual_address,
    uint64_t physical_address,
    bool writable,
    bool executable
);

bool paging_map_user_page(
    uint64_t virtual_address,
    uint64_t physical_address,
    bool writable
);

bool paging_map_kernel_page(
    uint64_t virtual_address,
    uint64_t physical_address,
    bool writable
);

bool paging_unmap_page_in(
    uint64_t root_physical,
    uint64_t virtual_address
);

bool paging_unmap_page(
    uint64_t virtual_address
);

bool paging_is_mapped_in(
    uint64_t root_physical,
    uint64_t virtual_address
);

bool paging_is_mapped(
    uint64_t virtual_address
);

bool paging_user_range_valid(
    uint64_t root_physical,
    uint64_t address,
    size_t size,
    bool writable
);

bool paging_run_shootdown_self_test(void);

void paging_register_current_cpu(void);
void paging_handle_tlb_ipi(void);
void paging_print_status(void);

#endif
