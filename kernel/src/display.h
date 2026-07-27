#ifndef DISPLAY_H
#define DISPLAY_H

#include "ui.h"

#include <stdbool.h>
#include <stdint.h>

#define DISPLAY_SCANOUT_BUFFER_COUNT 3U
#define DISPLAY_MAX_DAMAGE_RECTS 32U

typedef enum
{
    DISPLAY_BACKEND_SOFTWARE_FRAMEBUFFER,
    DISPLAY_BACKEND_VIRTIO_GPU
} display_backend_t;

typedef struct
{
    display_backend_t backend;
    uint32_t width;
    uint32_t height;
    uint32_t refresh_hz;
    uint32_t scanout_buffers;
    bool triple_buffered;
    bool frame_pending;
    uint64_t queued_frames;
    uint64_t presented_frames;
    uint64_t dropped_frames;
    uint64_t vsync_events;
    uint64_t captured_pixels;
    uint64_t presented_pixels;
    uint64_t capture_failures;
    uint64_t present_ticks;
} display_stats_t;

void display_init(void);
void display_set_refresh_rate(uint32_t refresh_hz);

bool display_queue_frame(
    const ui_rect_t *rectangles,
    uint32_t rectangle_count
);

void display_update(void);
void display_force_present(void);
void display_cancel_pending(void);

bool display_triple_buffered(void);
bool display_frame_pending(void);
const char *display_backend_name(void);
uint32_t display_scanout_buffer_count(void);
uint32_t display_refresh_rate(void);

uint64_t display_presented_frames(void);
uint64_t display_dropped_frames(void);
uint64_t display_vsync_events(void);
uint32_t display_recent_fps(void);
uint64_t display_average_present_ms(void);

void display_get_stats(display_stats_t *statistics);
void display_reset_statistics(void);

#endif
