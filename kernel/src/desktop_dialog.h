#ifndef DESKTOP_DIALOG_H
#define DESKTOP_DIALOG_H

#include "ui.h"
#include "ui_controls.h"
#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum
{
    DESKTOP_DIALOG_NONE,
    DESKTOP_DIALOG_MESSAGE,
    DESKTOP_DIALOG_CONFIRM,
    DESKTOP_DIALOG_OPEN_FILE,
    DESKTOP_DIALOG_SAVE_FILE,
    DESKTOP_DIALOG_SELECT_FOLDER,
    DESKTOP_DIALOG_COLOR,
    DESKTOP_DIALOG_PROPERTIES
} desktop_dialog_kind_t;

typedef enum
{
    DESKTOP_DIALOG_RESULT_NONE,
    DESKTOP_DIALOG_RESULT_ACCEPTED,
    DESKTOP_DIALOG_RESULT_CANCELLED
} desktop_dialog_result_t;

typedef struct
{
    desktop_dialog_kind_t kind;
    desktop_dialog_result_t result;
    bool visible;

    char title[64];
    char message[256];
    char input[128];
    size_t input_length;

    ui_rect_t bounds;
    vfs_node_t *directory;
    vfs_node_t *selected;
    uint32_t scroll_offset;
    uint32_t hovered_row;
    uint32_t selected_color;
    uint32_t owner_uid;
    uint32_t owner_gid;
    uint16_t mode;

    char selected_path[512];
} desktop_dialog_t;

void desktop_dialog_init(desktop_dialog_t *dialog);

void desktop_dialog_show_message(
    desktop_dialog_t *dialog,
    const char *title,
    const char *message
);

void desktop_dialog_show_confirm(
    desktop_dialog_t *dialog,
    const char *title,
    const char *message
);

void desktop_dialog_show_open_file(
    desktop_dialog_t *dialog,
    const char *title,
    vfs_node_t *start_directory
);

void desktop_dialog_show_save_file(
    desktop_dialog_t *dialog,
    const char *title,
    vfs_node_t *start_directory,
    const char *default_name
);

void desktop_dialog_show_select_folder(
    desktop_dialog_t *dialog,
    const char *title,
    vfs_node_t *start_directory
);

void desktop_dialog_show_color(
    desktop_dialog_t *dialog,
    const char *title,
    uint32_t initial_color
);

void desktop_dialog_show_properties(
    desktop_dialog_t *dialog,
    vfs_node_t *node
);

void desktop_dialog_close(
    desktop_dialog_t *dialog,
    desktop_dialog_result_t result
);

void desktop_dialog_layout(
    desktop_dialog_t *dialog,
    uint32_t screen_width,
    uint32_t screen_height
);

void desktop_dialog_render(
    const desktop_dialog_t *dialog,
    const ui_palette_t *palette
);

bool desktop_dialog_handle_mouse_move(
    desktop_dialog_t *dialog,
    int32_t x,
    int32_t y
);

bool desktop_dialog_handle_click(
    desktop_dialog_t *dialog,
    int32_t x,
    int32_t y
);

bool desktop_dialog_handle_key(
    desktop_dialog_t *dialog,
    char character
);

bool desktop_dialog_take_result(
    desktop_dialog_t *dialog,
    desktop_dialog_result_t *result
);

const char *desktop_dialog_selected_path(
    const desktop_dialog_t *dialog
);

vfs_node_t *desktop_dialog_selected_node(
    const desktop_dialog_t *dialog
);

uint32_t desktop_dialog_selected_color(
    const desktop_dialog_t *dialog
);

#endif
