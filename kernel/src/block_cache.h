#ifndef BLOCK_CACHE_H
#define BLOCK_CACHE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct block_device block_device_t;

typedef struct
{
    uint64_t read_hits;
    uint64_t read_misses;
    uint64_t write_hits;
    uint64_t write_misses;
    uint64_t evictions;
    uint64_t flushes;
    uint64_t flush_failures;
    uint32_t valid_entries;
    uint32_t dirty_entries;
} block_cache_stats_t;

void block_cache_init(void);
bool block_cache_start_worker(void);

bool block_cache_device_supported(
    const block_device_t *device
);

bool block_cache_read(
    const block_device_t *device,
    uint64_t lba,
    uint32_t sector_count,
    void *buffer
);

bool block_cache_write(
    const block_device_t *device,
    uint64_t lba,
    uint32_t sector_count,
    const void *buffer
);

bool block_cache_flush_device(
    const block_device_t *device
);

bool block_cache_flush_prefix(
    const char *name_prefix
);

bool block_cache_flush_all(void);

void block_cache_invalidate_device(
    const block_device_t *device
);

void block_cache_get_stats(
    block_cache_stats_t *stats
);

#endif
