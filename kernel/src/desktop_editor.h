#ifndef DESKTOP_EDITOR_H
#define DESKTOP_EDITOR_H

#include "ui.h"
#include "vfs.h"

#include <stdbool.h>
#include <stdint.h>

void desktop_editor_init(void);
void desktop_editor_open_node(vfs_node_t *node);
void desktop_editor_render(const ui_rect_t *content);

bool desktop_editor_handle_click(
    const ui_rect_t *content,
    int32_t x,
    int32_t y
);

bool desktop_editor_handle_mouse_move(
    const ui_rect_t *content,
    int32_t x,
    int32_t y
);

void desktop_editor_handle_mouse_up(void);
bool desktop_editor_handle_key(char character);

bool desktop_editor_copy(bool cut);
bool desktop_editor_paste(void);
void desktop_editor_select_all(void);
bool desktop_editor_save(void);
bool desktop_editor_save_as(const char *path);
void desktop_editor_request_save_as(void);
bool desktop_editor_take_save_as_request(void);
bool desktop_editor_read_only(void);

bool desktop_editor_has_selection(void);
bool desktop_editor_dirty(void);
const char *desktop_editor_name(void);

#endif
