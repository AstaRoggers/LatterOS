#ifndef APP_SUITE_H
#define APP_SUITE_H

#include "ui.h"
#include "vfs.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    uint32_t desktop;
    uint32_t taskbar;
    uint32_t taskbar_top;
    uint32_t window;
    uint32_t window_border;
    uint32_t title_active;
    uint32_t title_idle;
    uint32_t text;
    uint32_t light_text;
    uint32_t field;
    uint32_t accent;
    uint32_t close;
    uint32_t row_selected;
} gui_theme_t;

void app_suite_init(void);
const gui_theme_t *app_suite_theme(void);

void editor_open_node(vfs_node_t *node);
void editor_render(const ui_rect_t *content);
bool editor_handle_click(
    const ui_rect_t *content,
    int32_t x,
    int32_t y
);
bool editor_handle_key(char character);

void calculator_render(const ui_rect_t *content);
bool calculator_handle_click(
    const ui_rect_t *content,
    int32_t x,
    int32_t y
);
bool calculator_handle_key(char character);

void paint_render(const ui_rect_t *content);
bool paint_handle_mouse_down(
    const ui_rect_t *content,
    int32_t x,
    int32_t y,
    ui_rect_t *damage
);
bool paint_handle_mouse_move(
    const ui_rect_t *content,
    int32_t x,
    int32_t y,
    ui_rect_t *damage
);
void paint_handle_mouse_up(void);

void settings_render(const ui_rect_t *content);
bool settings_handle_click(
    const ui_rect_t *content,
    int32_t x,
    int32_t y
);

void process_manager_refresh(void);
void process_manager_render(const ui_rect_t *content);
bool process_manager_handle_click(
    const ui_rect_t *content,
    int32_t x,
    int32_t y
);

#endif
