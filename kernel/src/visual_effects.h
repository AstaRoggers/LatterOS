#ifndef VISUAL_EFFECTS_H
#define VISUAL_EFFECTS_H

#include "ui.h"

#include <stdint.h>

void visual_effects_blend_rect(
    const ui_rect_t *rectangle,
    uint32_t color,
    uint8_t opacity
);

void visual_effects_draw_shadow(
    const ui_rect_t *bounds,
    uint32_t color
);

void visual_effects_fill_rounded_rect(
    const ui_rect_t *bounds,
    uint32_t radius,
    uint32_t color
);

void visual_effects_fill_top_rounded_rect(
    const ui_rect_t *bounds,
    uint32_t radius,
    uint32_t color
);

void visual_effects_draw_rounded_border(
    const ui_rect_t *bounds,
    uint32_t radius,
    uint32_t thickness,
    uint32_t border_color,
    uint32_t interior_color
);

void visual_effects_draw_snap_preview(
    const ui_rect_t *bounds,
    uint32_t color
);

#endif
