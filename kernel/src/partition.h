#ifndef PARTITION_H
#define PARTITION_H

#include "block_device.h"

#include <stdbool.h>
#include <stdint.h>

#define PARTITION_MAX_COUNT 4
#define PARTITION_TYPE_LATTEROS_FS 0xDA
#define PARTITION_TYPE_LATTERFS PARTITION_TYPE_LATTEROS_FS

typedef struct
{
    const block_device_t *device;
    uint8_t type;
    uint64_t start_lba;
    uint64_t sector_count;
} partition_t;

void partition_init(void);

uint32_t partition_count(void);
const partition_t *partition_get(uint32_t index);

const partition_t *partition_find_type(uint8_t type);

bool partition_create_latteros_fs(void);

bool partition_read(
    const partition_t *partition,
    uint64_t relative_lba,
    uint32_t sector_count,
    void *buffer
);

bool partition_write(
    const partition_t *partition,
    uint64_t relative_lba,
    uint32_t sector_count,
    const void *buffer
);

#endif
