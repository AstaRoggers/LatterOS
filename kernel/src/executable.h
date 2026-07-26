#ifndef EXECUTABLE_H
#define EXECUTABLE_H

#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LATTEROS_EXECUTABLE_MAGIC 0x4558454CU
#define LATTEROS_EXECUTABLE_VERSION 1U

#define LATTEROS_EXECUTABLE_FLAG_64BIT 0x00000001U

bool executable_install(
    const char *path,
    const uint8_t *image,
    size_t image_size,
    uint32_t entry_offset
);

bool executable_load(
    vfs_node_t *node,
    void *destination,
    size_t capacity,
    size_t *image_size,
    uint32_t *entry_offset
);

bool executable_validate(vfs_node_t *node);

#endif
