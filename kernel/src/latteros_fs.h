#ifndef LATTEROS_FS_H
#define LATTEROS_FS_H

#include <stdbool.h>
#include <stdint.h>

bool latteros_fs_init(void);
bool latteros_fs_is_mounted(void);
bool latteros_fs_was_formatted(void);
bool latteros_fs_sync(void);

uint32_t latteros_fs_entry_count(void);
uint64_t latteros_fs_used_bytes(void);
uint64_t latteros_fs_capacity_bytes(void);

void latteros_fs_print_info(void);

#endif
