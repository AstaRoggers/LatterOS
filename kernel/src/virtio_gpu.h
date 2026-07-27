#ifndef VIRTIO_GPU_H
#define VIRTIO_GPU_H

#include "ui.h"

#include <stdbool.h>
#include <stdint.h>

#define VIRTIO_GPU_MAX_DAMAGE_RECTS 32U

typedef struct
{
    bool pci_device_found;
    bool modern_transport;
    bool control_queue_ready;
    bool cursor_queue_ready;
    bool scanout_ready;
    bool cursor_ready;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t pci_device_id;
    uint16_t control_queue_size;
    uint16_t cursor_queue_size;
    uint32_t scanout_id;
    uint32_t width;
    uint32_t height;
    uint64_t commands;
    uint64_t command_errors;
    uint64_t transfers;
    uint64_t flushes;
    uint64_t presented_frames;
    uint64_t presented_pixels;
    uint64_t cursor_updates;
    uint64_t cursor_moves;
    uint32_t last_response_type;
} virtio_gpu_stats_t;

bool virtio_gpu_init(uint32_t width, uint32_t height);
bool virtio_gpu_available(void);

bool virtio_gpu_present(
    const uint32_t *source,
    uint32_t source_stride,
    const ui_rect_t *rectangles,
    uint32_t rectangle_count
);

bool virtio_gpu_cursor_show(int32_t x, int32_t y);
bool virtio_gpu_cursor_move(int32_t x, int32_t y);
bool virtio_gpu_cursor_hide(void);
bool virtio_gpu_cursor_available(void);

void virtio_gpu_get_stats(virtio_gpu_stats_t *statistics);
const char *virtio_gpu_status_text(void);

#endif
