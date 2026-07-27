#ifndef WINDOW_MANAGER_H
#define WINDOW_MANAGER_H

#include "ui.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    WM_SNAP_NONE,
    WM_SNAP_MAXIMIZED,
    WM_SNAP_LEFT,
    WM_SNAP_RIGHT,
    WM_SNAP_TOP_LEFT,
    WM_SNAP_TOP_RIGHT,
    WM_SNAP_BOTTOM_LEFT,
    WM_SNAP_BOTTOM_RIGHT
} wm_snap_t;

typedef enum
{
    WM_RESIZE_NONE   = 0,
    WM_RESIZE_LEFT   = 1U << 0,
    WM_RESIZE_RIGHT  = 1U << 1,
    WM_RESIZE_TOP    = 1U << 2,
    WM_RESIZE_BOTTOM = 1U << 3
} wm_resize_edge_t;

typedef struct
{
    ui_rect_t bounds;
    ui_rect_t restore_bounds;
    ui_rect_t fullscreen_restore_bounds;
    ui_rect_t interaction_start_bounds;

    uint32_t minimum_width;
    uint32_t minimum_height;
    uint32_t maximum_width;
    uint32_t maximum_height;

    bool visible;
    bool minimized;
    bool maximized;
    bool fullscreen;
    bool fullscreen_restore_maximized;
    bool dragging;
    bool resizing;

    wm_snap_t snap;
    wm_snap_t pending_snap;
    uint8_t resize_edges;

    int32_t drag_offset_x;
    int32_t drag_offset_y;
    int32_t interaction_start_x;
    int32_t interaction_start_y;
} wm_window_state_t;

void wm_initialize_window(
    wm_window_state_t *window,
    const ui_rect_t *bounds,
    uint32_t minimum_width,
    uint32_t minimum_height
);

void wm_set_size_limits(
    wm_window_state_t *window,
    uint32_t minimum_width,
    uint32_t minimum_height,
    uint32_t maximum_width,
    uint32_t maximum_height
);

void wm_clamp_window(
    wm_window_state_t *window,
    uint32_t screen_width,
    uint32_t screen_height,
    uint32_t reserved_bottom
);

uint8_t wm_resize_hit_test(
    const wm_window_state_t *window,
    int32_t x,
    int32_t y,
    uint32_t border_thickness
);

bool wm_begin_resize(
    wm_window_state_t *window,
    uint8_t edges,
    int32_t pointer_x,
    int32_t pointer_y
);

bool wm_update_resize(
    wm_window_state_t *window,
    int32_t pointer_x,
    int32_t pointer_y,
    uint32_t screen_width,
    uint32_t screen_height,
    uint32_t reserved_bottom
);

bool wm_begin_drag(
    wm_window_state_t *window,
    int32_t pointer_x,
    int32_t pointer_y,
    uint32_t screen_width,
    uint32_t screen_height,
    uint32_t reserved_bottom
);

bool wm_update_drag(
    wm_window_state_t *window,
    int32_t pointer_x,
    int32_t pointer_y,
    uint32_t screen_width,
    uint32_t screen_height,
    uint32_t reserved_bottom,
    uint32_t snap_threshold
);

bool wm_end_interaction(
    wm_window_state_t *window,
    uint32_t screen_width,
    uint32_t screen_height,
    uint32_t reserved_bottom
);

void wm_cancel_interaction(wm_window_state_t *window);

void wm_toggle_maximize(
    wm_window_state_t *window,
    uint32_t screen_width,
    uint32_t screen_height,
    uint32_t reserved_bottom
);

void wm_toggle_fullscreen(
    wm_window_state_t *window,
    uint32_t screen_width,
    uint32_t screen_height
);

void wm_apply_snap(
    wm_window_state_t *window,
    wm_snap_t snap,
    uint32_t screen_width,
    uint32_t screen_height,
    uint32_t reserved_bottom
);

ui_rect_t wm_snap_preview(
    wm_snap_t snap,
    uint32_t screen_width,
    uint32_t screen_height,
    uint32_t reserved_bottom
);

#endif
