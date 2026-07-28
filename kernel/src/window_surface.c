#include "window_surface.h"

#include "graphics.h"
#include "surface.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WINDOW_SURFACE_MAX_PIXELS \
    ((uint64_t)WINDOW_SURFACE_MAX_WIDTH * WINDOW_SURFACE_MAX_HEIGHT)

typedef struct
{
    surface_t surface;
    bool valid;
    uint32_t width;
    uint32_t height;
    uint64_t captures;
    uint64_t draws;
    uint64_t scaled_draws;
    uint64_t capture_failures;
} backing_entry_t;

static uint32_t backing_pixels[
    WINDOW_SURFACE_COUNT
][WINDOW_SURFACE_MAX_PIXELS] __attribute__((aligned(64)));

static uint32_t scaled_pixels[
    WINDOW_SURFACE_MAX_PIXELS
] __attribute__((aligned(64)));

static backing_entry_t entries[WINDOW_SURFACE_COUNT];

static bool dimensions_supported(uint32_t width, uint32_t height)
{
    return
        width != 0U &&
        height != 0U &&
        width <= WINDOW_SURFACE_MAX_WIDTH &&
        height <= WINDOW_SURFACE_MAX_HEIGHT &&
        (uint64_t)width * height <= WINDOW_SURFACE_MAX_PIXELS;
}

void window_surface_init(void)
{
    for (uint32_t index = 0; index < WINDOW_SURFACE_COUNT; index++)
    {
        entries[index].surface.pixels = backing_pixels[index];
        entries[index].surface.width = 0U;
        entries[index].surface.height = 0U;
        entries[index].surface.stride = 0U;
        entries[index].valid = false;
        entries[index].width = 0U;
        entries[index].height = 0U;
        entries[index].captures = 0U;
        entries[index].draws = 0U;
        entries[index].scaled_draws = 0U;
        entries[index].capture_failures = 0U;
    }
}

void window_surface_invalidate(uint32_t window_id)
{
    if (window_id >= WINDOW_SURFACE_COUNT)
    {
        return;
    }

    entries[window_id].valid = false;
}

void window_surface_invalidate_all(void)
{
    for (uint32_t index = 0; index < WINDOW_SURFACE_COUNT; index++)
    {
        entries[index].valid = false;
    }
}

bool window_surface_capture(
    uint32_t window_id,
    const ui_rect_t *bounds
)
{
    if (
        window_id >= WINDOW_SURFACE_COUNT ||
        bounds == NULL ||
        bounds->x < 0 ||
        bounds->y < 0 ||
        !dimensions_supported(bounds->width, bounds->height) ||
        (uint64_t)bounds->x + bounds->width > graphics_width() ||
        (uint64_t)bounds->y + bounds->height > graphics_height()
    )
    {
        if (window_id < WINDOW_SURFACE_COUNT)
        {
            entries[window_id].capture_failures++;
            entries[window_id].valid = false;
        }

        return false;
    }

    backing_entry_t *entry = &entries[window_id];

    if (!graphics_capture_rectangle(
        (uint32_t)bounds->x,
        (uint32_t)bounds->y,
        bounds->width,
        bounds->height,
        backing_pixels[window_id],
        bounds->width
    ))
    {
        entry->capture_failures++;
        entry->valid = false;
        return false;
    }

    if (!surface_init(
        &entry->surface,
        backing_pixels[window_id],
        bounds->width,
        bounds->height,
        bounds->width
    ))
    {
        entry->capture_failures++;
        entry->valid = false;
        return false;
    }

    entry->width = bounds->width;
    entry->height = bounds->height;
    entry->valid = true;
    entry->captures++;
    return true;
}

bool window_surface_valid_for(
    uint32_t window_id,
    uint32_t width,
    uint32_t height
)
{
    if (window_id >= WINDOW_SURFACE_COUNT)
    {
        return false;
    }

    const backing_entry_t *entry = &entries[window_id];

    return
        entry->valid &&
        entry->width == width &&
        entry->height == height &&
        surface_valid(&entry->surface);
}

bool window_surface_draw(
    uint32_t window_id,
    const ui_rect_t *bounds
)
{
    if (
        bounds == NULL ||
        !window_surface_valid_for(
            window_id,
            bounds->width,
            bounds->height
        )
    )
    {
        return false;
    }

    backing_entry_t *entry = &entries[window_id];

    graphics_blit_surface(
        entry->surface.pixels,
        entry->surface.stride,
        entry->surface.width,
        entry->surface.height,
        bounds->x,
        bounds->y
    );

    entry->draws++;
    return true;
}

bool window_surface_draw_scaled(
    uint32_t window_id,
    const ui_rect_t *bounds
)
{
    if (
        window_id >= WINDOW_SURFACE_COUNT ||
        bounds == NULL ||
        !entries[window_id].valid ||
        !surface_valid(&entries[window_id].surface) ||
        !dimensions_supported(bounds->width, bounds->height)
    )
    {
        return false;
    }

    surface_t scaled;

    if (!surface_init(
        &scaled,
        scaled_pixels,
        bounds->width,
        bounds->height,
        bounds->width
    ))
    {
        return false;
    }

    ui_rect_t destination_rectangle = {
        .x = 0,
        .y = 0,
        .width = bounds->width,
        .height = bounds->height
    };

    ui_rect_t source_rectangle = {
        .x = 0,
        .y = 0,
        .width = entries[window_id].surface.width,
        .height = entries[window_id].surface.height
    };

    if (!surface_scale_nearest(
        &scaled,
        &destination_rectangle,
        &entries[window_id].surface,
        &source_rectangle
    ))
    {
        return false;
    }

    graphics_blit_surface(
        scaled.pixels,
        scaled.stride,
        scaled.width,
        scaled.height,
        bounds->x,
        bounds->y
    );

    entries[window_id].scaled_draws++;
    return true;
}

void window_surface_get_info(
    uint32_t window_id,
    window_surface_info_t *information
)
{
    if (information == NULL)
    {
        return;
    }

    *information = (window_surface_info_t){ 0 };

    if (window_id >= WINDOW_SURFACE_COUNT)
    {
        return;
    }

    const backing_entry_t *entry = &entries[window_id];
    information->valid = entry->valid;
    information->width = entry->width;
    information->height = entry->height;
    information->captures = entry->captures;
    information->draws = entry->draws;
    information->scaled_draws = entry->scaled_draws;
    information->capture_failures = entry->capture_failures;
}

uint64_t window_surface_total_captures(void)
{
    uint64_t total = 0U;

    for (uint32_t index = 0; index < WINDOW_SURFACE_COUNT; index++)
    {
        total += entries[index].captures;
    }

    return total;
}

uint64_t window_surface_total_draws(void)
{
    uint64_t total = 0U;

    for (uint32_t index = 0; index < WINDOW_SURFACE_COUNT; index++)
    {
        total += entries[index].draws;
    }

    return total;
}

uint64_t window_surface_total_scaled_draws(void)
{
    uint64_t total = 0U;

    for (uint32_t index = 0; index < WINDOW_SURFACE_COUNT; index++)
    {
        total += entries[index].scaled_draws;
    }

    return total;
}
