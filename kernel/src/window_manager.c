#include "window_manager.h"

#include <stddef.h>
#include <stdint.h>

static int32_t maximum_int32(int32_t first, int32_t second)
{
    return first > second ? first : second;
}

static uint32_t clamp_u32(
    uint32_t value,
    uint32_t minimum,
    uint32_t maximum
)
{
    if (value < minimum)
    {
        return minimum;
    }

    if (maximum != 0 && value > maximum)
    {
        return maximum;
    }

    return value;
}

static uint32_t available_height(
    uint32_t screen_height,
    uint32_t reserved_bottom
)
{
    return screen_height > reserved_bottom ?
        screen_height - reserved_bottom : 1U;
}

void wm_initialize_window(
    wm_window_state_t *window,
    const ui_rect_t *bounds,
    uint32_t minimum_width,
    uint32_t minimum_height
)
{
    if (window == NULL || bounds == NULL)
    {
        return;
    }

    window->bounds = *bounds;
    window->restore_bounds = *bounds;
    window->fullscreen_restore_bounds = *bounds;
    window->interaction_start_bounds = *bounds;

    window->minimum_width = minimum_width;
    window->minimum_height = minimum_height;
    window->maximum_width = 0;
    window->maximum_height = 0;

    window->visible = false;
    window->minimized = false;
    window->maximized = false;
    window->fullscreen = false;
    window->fullscreen_restore_maximized = false;
    window->dragging = false;
    window->resizing = false;
    window->snap = WM_SNAP_NONE;
    window->pending_snap = WM_SNAP_NONE;
    window->resize_edges = WM_RESIZE_NONE;
    window->drag_offset_x = 0;
    window->drag_offset_y = 0;
    window->interaction_start_x = 0;
    window->interaction_start_y = 0;
}

void wm_set_size_limits(
    wm_window_state_t *window,
    uint32_t minimum_width,
    uint32_t minimum_height,
    uint32_t maximum_width,
    uint32_t maximum_height
)
{
    if (window == NULL)
    {
        return;
    }

    window->minimum_width = minimum_width;
    window->minimum_height = minimum_height;
    window->maximum_width = maximum_width;
    window->maximum_height = maximum_height;
}

void wm_clamp_window(
    wm_window_state_t *window,
    uint32_t screen_width,
    uint32_t screen_height,
    uint32_t reserved_bottom
)
{
    if (window == NULL)
    {
        return;
    }

    uint32_t work_height = available_height(
        screen_height,
        reserved_bottom
    );

    uint32_t maximum_width = window->maximum_width;
    uint32_t maximum_height = window->maximum_height;

    if (maximum_width == 0 || maximum_width > screen_width)
    {
        maximum_width = screen_width;
    }

    if (maximum_height == 0 || maximum_height > work_height)
    {
        maximum_height = work_height;
    }

    uint32_t minimum_width = window->minimum_width;
    uint32_t minimum_height = window->minimum_height;

    if (minimum_width > maximum_width)
    {
        minimum_width = maximum_width;
    }

    if (minimum_height > maximum_height)
    {
        minimum_height = maximum_height;
    }

    window->bounds.width = clamp_u32(
        window->bounds.width,
        minimum_width,
        maximum_width
    );

    window->bounds.height = clamp_u32(
        window->bounds.height,
        minimum_height,
        maximum_height
    );

    int32_t maximum_x =
        (int32_t)screen_width -
        (int32_t)window->bounds.width;

    int32_t maximum_y =
        (int32_t)work_height -
        (int32_t)window->bounds.height;

    if (maximum_x < 0)
    {
        maximum_x = 0;
    }

    if (maximum_y < 0)
    {
        maximum_y = 0;
    }

    if (window->bounds.x < 0)
    {
        window->bounds.x = 0;
    }

    if (window->bounds.y < 0)
    {
        window->bounds.y = 0;
    }

    if (window->bounds.x > maximum_x)
    {
        window->bounds.x = maximum_x;
    }

    if (window->bounds.y > maximum_y)
    {
        window->bounds.y = maximum_y;
    }
}

uint8_t wm_resize_hit_test(
    const wm_window_state_t *window,
    int32_t x,
    int32_t y,
    uint32_t border_thickness
)
{
    if (
        window == NULL ||
        !window->visible ||
        window->minimized ||
        window->maximized ||
        window->fullscreen ||
        window->snap != WM_SNAP_NONE
    )
    {
        return WM_RESIZE_NONE;
    }

    int32_t right =
        window->bounds.x +
        (int32_t)window->bounds.width;

    int32_t bottom =
        window->bounds.y +
        (int32_t)window->bounds.height;

    int32_t border = (int32_t)border_thickness;

    if (
        x < window->bounds.x - border ||
        x > right + border ||
        y < window->bounds.y - border ||
        y > bottom + border
    )
    {
        return WM_RESIZE_NONE;
    }

    uint8_t edges = WM_RESIZE_NONE;

    if (x <= window->bounds.x + border)
    {
        edges |= WM_RESIZE_LEFT;
    }
    else if (x >= right - border)
    {
        edges |= WM_RESIZE_RIGHT;
    }

    if (y <= window->bounds.y + border)
    {
        edges |= WM_RESIZE_TOP;
    }
    else if (y >= bottom - border)
    {
        edges |= WM_RESIZE_BOTTOM;
    }

    return edges;
}

bool wm_begin_resize(
    wm_window_state_t *window,
    uint8_t edges,
    int32_t pointer_x,
    int32_t pointer_y
)
{
    if (
        window == NULL ||
        edges == WM_RESIZE_NONE ||
        window->maximized ||
        window->fullscreen ||
        window->snap != WM_SNAP_NONE
    )
    {
        return false;
    }

    window->resizing = true;
    window->dragging = false;
    window->resize_edges = edges;
    window->interaction_start_x = pointer_x;
    window->interaction_start_y = pointer_y;
    window->interaction_start_bounds = window->bounds;
    window->pending_snap = WM_SNAP_NONE;
    return true;
}

bool wm_update_resize(
    wm_window_state_t *window,
    int32_t pointer_x,
    int32_t pointer_y,
    uint32_t screen_width,
    uint32_t screen_height,
    uint32_t reserved_bottom
)
{
    if (window == NULL || !window->resizing)
    {
        return false;
    }

    int32_t delta_x =
        pointer_x - window->interaction_start_x;

    int32_t delta_y =
        pointer_y - window->interaction_start_y;

    int32_t left = window->interaction_start_bounds.x;
    int32_t top = window->interaction_start_bounds.y;
    int32_t right =
        left +
        (int32_t)window->interaction_start_bounds.width;

    int32_t bottom =
        top +
        (int32_t)window->interaction_start_bounds.height;

    if ((window->resize_edges & WM_RESIZE_LEFT) != 0)
    {
        left += delta_x;
    }

    if ((window->resize_edges & WM_RESIZE_RIGHT) != 0)
    {
        right += delta_x;
    }

    if ((window->resize_edges & WM_RESIZE_TOP) != 0)
    {
        top += delta_y;
    }

    if ((window->resize_edges & WM_RESIZE_BOTTOM) != 0)
    {
        bottom += delta_y;
    }

    int32_t minimum_width = (int32_t)window->minimum_width;
    int32_t minimum_height = (int32_t)window->minimum_height;

    if (right - left < minimum_width)
    {
        if ((window->resize_edges & WM_RESIZE_LEFT) != 0)
        {
            left = right - minimum_width;
        }
        else
        {
            right = left + minimum_width;
        }
    }

    if (bottom - top < minimum_height)
    {
        if ((window->resize_edges & WM_RESIZE_TOP) != 0)
        {
            top = bottom - minimum_height;
        }
        else
        {
            bottom = top + minimum_height;
        }
    }

    window->bounds.x = left;
    window->bounds.y = top;
    window->bounds.width =
        (uint32_t)maximum_int32(right - left, 1);

    window->bounds.height =
        (uint32_t)maximum_int32(bottom - top, 1);

    wm_clamp_window(
        window,
        screen_width,
        screen_height,
        reserved_bottom
    );

    return true;
}

static void restore_for_drag(
    wm_window_state_t *window,
    int32_t pointer_x,
    int32_t pointer_y,
    uint32_t screen_width,
    uint32_t screen_height,
    uint32_t reserved_bottom
)
{
    uint32_t old_width = window->bounds.width;

    ui_rect_t restored = window->restore_bounds;

    if (restored.width == 0 || restored.height == 0)
    {
        restored.width = screen_width > 640 ? 640 : screen_width;
        restored.height = screen_height > 420 ? 420 : screen_height;
        restored.x = 0;
        restored.y = 0;
    }

    uint32_t relative = old_width == 0 ? 0 :
        (uint32_t)(pointer_x - window->bounds.x) *
        restored.width / old_width;

    window->bounds = restored;
    window->bounds.x = pointer_x - (int32_t)relative;
    window->bounds.y = pointer_y - 12;
    window->maximized = false;
    window->snap = WM_SNAP_NONE;

    wm_clamp_window(
        window,
        screen_width,
        screen_height,
        reserved_bottom
    );
}

bool wm_begin_drag(
    wm_window_state_t *window,
    int32_t pointer_x,
    int32_t pointer_y,
    uint32_t screen_width,
    uint32_t screen_height,
    uint32_t reserved_bottom
)
{
    if (
        window == NULL ||
        !window->visible ||
        window->minimized ||
        window->fullscreen
    )
    {
        return false;
    }

    if (window->maximized || window->snap != WM_SNAP_NONE)
    {
        restore_for_drag(
            window,
            pointer_x,
            pointer_y,
            screen_width,
            screen_height,
            reserved_bottom
        );
    }

    window->dragging = true;
    window->resizing = false;
    window->resize_edges = WM_RESIZE_NONE;
    window->drag_offset_x = pointer_x - window->bounds.x;
    window->drag_offset_y = pointer_y - window->bounds.y;
    window->interaction_start_bounds = window->bounds;
    window->pending_snap = WM_SNAP_NONE;
    return true;
}

static wm_snap_t snap_for_pointer(
    int32_t pointer_x,
    int32_t pointer_y,
    uint32_t screen_width,
    uint32_t screen_height,
    uint32_t reserved_bottom,
    uint32_t threshold
)
{
    uint32_t work_height = available_height(
        screen_height,
        reserved_bottom
    );

    int32_t edge = (int32_t)threshold;
    bool left = pointer_x <= edge;
    bool right = pointer_x >=
        (int32_t)screen_width - edge;

    bool top = pointer_y <= edge;
    bool bottom = pointer_y >=
        (int32_t)work_height - edge;

    if (top && left)
    {
        return WM_SNAP_TOP_LEFT;
    }

    if (top && right)
    {
        return WM_SNAP_TOP_RIGHT;
    }

    if (bottom && left)
    {
        return WM_SNAP_BOTTOM_LEFT;
    }

    if (bottom && right)
    {
        return WM_SNAP_BOTTOM_RIGHT;
    }

    if (top)
    {
        return WM_SNAP_MAXIMIZED;
    }

    if (left)
    {
        return WM_SNAP_LEFT;
    }

    if (right)
    {
        return WM_SNAP_RIGHT;
    }

    return WM_SNAP_NONE;
}

bool wm_update_drag(
    wm_window_state_t *window,
    int32_t pointer_x,
    int32_t pointer_y,
    uint32_t screen_width,
    uint32_t screen_height,
    uint32_t reserved_bottom,
    uint32_t snap_threshold
)
{
    if (window == NULL || !window->dragging)
    {
        return false;
    }

    window->bounds.x = pointer_x - window->drag_offset_x;
    window->bounds.y = pointer_y - window->drag_offset_y;

    wm_clamp_window(
        window,
        screen_width,
        screen_height,
        reserved_bottom
    );

    window->pending_snap = snap_for_pointer(
        pointer_x,
        pointer_y,
        screen_width,
        screen_height,
        reserved_bottom,
        snap_threshold
    );

    return true;
}

ui_rect_t wm_snap_preview(
    wm_snap_t snap,
    uint32_t screen_width,
    uint32_t screen_height,
    uint32_t reserved_bottom
)
{
    uint32_t work_height = available_height(
        screen_height,
        reserved_bottom
    );

    uint32_t half_width = screen_width / 2U;
    uint32_t half_height = work_height / 2U;

    ui_rect_t rectangle = {
        .x = 0,
        .y = 0,
        .width = screen_width,
        .height = work_height
    };

    switch (snap)
    {
        case WM_SNAP_LEFT:
            rectangle.width = half_width;
            break;

        case WM_SNAP_RIGHT:
            rectangle.x = (int32_t)half_width;
            rectangle.width = screen_width - half_width;
            break;

        case WM_SNAP_TOP_LEFT:
            rectangle.width = half_width;
            rectangle.height = half_height;
            break;

        case WM_SNAP_TOP_RIGHT:
            rectangle.x = (int32_t)half_width;
            rectangle.width = screen_width - half_width;
            rectangle.height = half_height;
            break;

        case WM_SNAP_BOTTOM_LEFT:
            rectangle.y = (int32_t)half_height;
            rectangle.width = half_width;
            rectangle.height = work_height - half_height;
            break;

        case WM_SNAP_BOTTOM_RIGHT:
            rectangle.x = (int32_t)half_width;
            rectangle.y = (int32_t)half_height;
            rectangle.width = screen_width - half_width;
            rectangle.height = work_height - half_height;
            break;

        case WM_SNAP_MAXIMIZED:
        case WM_SNAP_NONE:
        default:
            break;
    }

    return rectangle;
}

void wm_apply_snap(
    wm_window_state_t *window,
    wm_snap_t snap,
    uint32_t screen_width,
    uint32_t screen_height,
    uint32_t reserved_bottom
)
{
    if (window == NULL || snap == WM_SNAP_NONE)
    {
        return;
    }

    if (
        !window->maximized &&
        window->snap == WM_SNAP_NONE &&
        !window->fullscreen
    )
    {
        window->restore_bounds = window->bounds;
    }

    window->bounds = wm_snap_preview(
        snap,
        screen_width,
        screen_height,
        reserved_bottom
    );

    window->snap = snap;
    window->maximized = snap == WM_SNAP_MAXIMIZED;
    window->pending_snap = WM_SNAP_NONE;
    window->dragging = false;
    window->resizing = false;
}

bool wm_end_interaction(
    wm_window_state_t *window,
    uint32_t screen_width,
    uint32_t screen_height,
    uint32_t reserved_bottom
)
{
    if (window == NULL)
    {
        return false;
    }

    bool changed = window->dragging || window->resizing;
    wm_snap_t snap = window->pending_snap;

    window->dragging = false;
    window->resizing = false;
    window->resize_edges = WM_RESIZE_NONE;
    window->pending_snap = WM_SNAP_NONE;

    if (snap != WM_SNAP_NONE)
    {
        wm_apply_snap(
            window,
            snap,
            screen_width,
            screen_height,
            reserved_bottom
        );

        return true;
    }

    if (changed && !window->maximized && !window->fullscreen)
    {
        window->restore_bounds = window->bounds;
    }

    return changed;
}

void wm_cancel_interaction(wm_window_state_t *window)
{
    if (window == NULL)
    {
        return;
    }

    if (window->dragging || window->resizing)
    {
        window->bounds = window->interaction_start_bounds;
    }

    window->dragging = false;
    window->resizing = false;
    window->resize_edges = WM_RESIZE_NONE;
    window->pending_snap = WM_SNAP_NONE;
}

void wm_toggle_maximize(
    wm_window_state_t *window,
    uint32_t screen_width,
    uint32_t screen_height,
    uint32_t reserved_bottom
)
{
    if (
        window == NULL ||
        !window->visible ||
        window->minimized ||
        window->fullscreen
    )
    {
        return;
    }

    if (window->maximized || window->snap != WM_SNAP_NONE)
    {
        window->bounds = window->restore_bounds;
        window->maximized = false;
        window->snap = WM_SNAP_NONE;

        wm_clamp_window(
            window,
            screen_width,
            screen_height,
            reserved_bottom
        );
    }
    else
    {
        wm_apply_snap(
            window,
            WM_SNAP_MAXIMIZED,
            screen_width,
            screen_height,
            reserved_bottom
        );
    }
}

void wm_toggle_fullscreen(
    wm_window_state_t *window,
    uint32_t screen_width,
    uint32_t screen_height
)
{
    if (
        window == NULL ||
        !window->visible ||
        window->minimized
    )
    {
        return;
    }

    if (window->fullscreen)
    {
        window->bounds = window->fullscreen_restore_bounds;
        window->maximized = window->fullscreen_restore_maximized;
        window->fullscreen = false;
    }
    else
    {
        window->fullscreen_restore_bounds = window->bounds;
        window->fullscreen_restore_maximized = window->maximized;
        window->bounds.x = 0;
        window->bounds.y = 0;
        window->bounds.width = screen_width;
        window->bounds.height = screen_height;
        window->fullscreen = true;
    }

    window->dragging = false;
    window->resizing = false;
    window->pending_snap = WM_SNAP_NONE;
}
