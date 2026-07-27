#ifndef DESKTOP_FONT_H
#define DESKTOP_FONT_H

#include <stdint.h>

uint32_t desktop_font_text_width(
    const char *text,
    uint32_t scale
);

uint32_t desktop_font_text_height(uint32_t scale);

void desktop_font_draw_text(
    const char *text,
    int32_t x,
    int32_t y,
    uint32_t scale,
    uint32_t color
);

#endif
