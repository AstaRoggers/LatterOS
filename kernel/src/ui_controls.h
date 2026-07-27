#ifndef UI_CONTROLS_H
#define UI_CONTROLS_H

#include "ui.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    uint32_t background;
    uint32_t panel;
    uint32_t border;
    uint32_t text;
    uint32_t light_text;
    uint32_t field;
    uint32_t accent;
    uint32_t accent_hover;
    uint32_t selected;
    uint32_t disabled;
    uint32_t danger;
} ui_palette_t;

typedef enum
{
    UI_CONTROL_NORMAL,
    UI_CONTROL_HOVERED,
    UI_CONTROL_PRESSED,
    UI_CONTROL_FOCUSED,
    UI_CONTROL_DISABLED
} ui_control_state_t;

typedef struct
{
    const char *label;
    bool enabled;
    bool checked;
    bool separator;
    uint32_t command;
} ui_menu_item_t;

typedef struct
{
    ui_rect_t bounds;
    const ui_menu_item_t *items;
    uint32_t item_count;
    uint32_t hovered_index;
    bool visible;
} ui_menu_t;

typedef struct
{
    uint32_t value;
    uint32_t page_size;
    uint32_t maximum;
} ui_scrollbar_t;

void ui_control_draw_panel(
    const ui_rect_t *rect,
    const ui_palette_t *palette,
    bool raised
);

void ui_control_draw_button(
    const ui_rect_t *rect,
    const char *label,
    const ui_palette_t *palette,
    ui_control_state_t state
);

void ui_control_draw_checkbox(
    const ui_rect_t *rect,
    const char *label,
    bool checked,
    const ui_palette_t *palette,
    ui_control_state_t state
);

void ui_control_draw_radio(
    const ui_rect_t *rect,
    const char *label,
    bool selected,
    const ui_palette_t *palette,
    ui_control_state_t state
);

void ui_control_draw_text_input(
    const ui_rect_t *rect,
    const char *text,
    const char *placeholder,
    size_t cursor,
    const ui_palette_t *palette,
    ui_control_state_t state
);

void ui_control_draw_slider(
    const ui_rect_t *rect,
    uint32_t value,
    uint32_t maximum,
    const ui_palette_t *palette,
    ui_control_state_t state
);

uint32_t ui_control_slider_value(
    const ui_rect_t *rect,
    int32_t pointer_x,
    uint32_t maximum
);

void ui_control_draw_scrollbar_vertical(
    const ui_rect_t *rect,
    const ui_scrollbar_t *scrollbar,
    const ui_palette_t *palette,
    ui_control_state_t state
);

uint32_t ui_control_scrollbar_value_from_pointer(
    const ui_rect_t *rect,
    const ui_scrollbar_t *scrollbar,
    int32_t pointer_y
);

void ui_control_draw_list_row(
    const ui_rect_t *rect,
    const char *prefix,
    const char *label,
    const ui_palette_t *palette,
    bool selected,
    bool hovered
);

void ui_control_draw_tab(
    const ui_rect_t *rect,
    const char *label,
    const ui_palette_t *palette,
    bool active,
    bool hovered
);

void ui_control_draw_toolbar(
    const ui_rect_t *rect,
    const ui_palette_t *palette
);

void ui_control_draw_statusbar(
    const ui_rect_t *rect,
    const char *left_text,
    const char *right_text,
    const ui_palette_t *palette
);

void ui_menu_open(
    ui_menu_t *menu,
    int32_t x,
    int32_t y,
    uint32_t width,
    const ui_menu_item_t *items,
    uint32_t item_count,
    uint32_t screen_width,
    uint32_t screen_height
);

void ui_menu_close(ui_menu_t *menu);

void ui_menu_update_hover(
    ui_menu_t *menu,
    int32_t pointer_x,
    int32_t pointer_y
);

bool ui_menu_command_at(
    const ui_menu_t *menu,
    int32_t pointer_x,
    int32_t pointer_y,
    uint32_t *command
);

void ui_menu_render(
    const ui_menu_t *menu,
    const ui_palette_t *palette
);

#endif
