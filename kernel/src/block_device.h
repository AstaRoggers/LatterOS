#ifndef BLOCK_DEVICE_H
#define BLOCK_DEVICE_H

#include <stdbool.h>
#include <stdint.h>

#define BLOCK_DEVICE_MAX 16

typedef bool (*block_read_function_t)(
    void *context,
    uint64_t lba,
    uint32_t sector_count,
    void *buffer
);

typedef bool (*block_write_function_t)(
    void *context,
    uint64_t lba,
    uint32_t sector_count,
    const void *buffer
);

typedef struct
{
    const char *name;
    uint32_t sector_size;
    uint64_t sector_count;
    bool writable;

    void *context;
    block_read_function_t read;
    block_write_function_t write;
} block_device_t;

void block_device_init(void);

bool block_device_register(
    const block_device_t *device
);

uint32_t block_device_count(void);

const block_device_t *block_device_get(
    uint32_t index
);

const block_device_t *block_device_primary(void);

bool block_device_read(
    const block_device_t *device,
    uint64_t lba,
    uint32_t sector_count,
    void *buffer
);

bool block_device_write(
    const block_device_t *device,
    uint64_t lba,
    uint32_t sector_count,
    const void *buffer
);

#endif
