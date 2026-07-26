#ifndef GDT_H
#define GDT_H

#include <stdint.h>

#define GDT_KERNEL_CODE_SELECTOR 0x08
#define GDT_KERNEL_DATA_SELECTOR 0x10
#define GDT_USER_DATA_SELECTOR   0x1B
#define GDT_USER_CODE_SELECTOR   0x23
#define GDT_TSS_SELECTOR         0x28

void gdt_init(void);
void gdt_load_secondary(void);

void gdt_set_kernel_stack(
    uint64_t stack_top
);

#endif
