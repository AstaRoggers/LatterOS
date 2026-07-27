#include "desktop_dialog.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DIALOG_WIDTH 560U
#define DIALOG_MESSAGE_HEIGHT 220U
#define DIALOG_PICKER_HEIGHT 440U
#define DIALOG_COLOR_HEIGHT 300U
#define DIALOG_PROPERTIES_HEIGHT 330U
#define DIALOG_ROW_HEIGHT 24U
#define DIALOG_VISIBLE_ROWS 11U

static size_t string_length(const char *text)
{
    size_t length = 0;

    if (text == NULL)
    {
        return 0;
    }

    while (text[length] != '\0')
    {
        length++;
    }

    return length;
}

static void clear_bytes(void *pointer, size_t count)
{
    uint8_t *bytes = pointer;

    if (bytes == NULL)
    {
        return;
    }

    for (size_t index = 0; index < count; index++)
    {
        bytes[index] = 0;
    }
}

static void copy_text(
    char *destination,
    size_t capacity,
    const char *source
)
{
    if (destination == NULL || capacity == 0)
    {
        return;
    }

    size_t index = 0;

    if (source != NULL)
    {
        while (
            source[index] != '\0' &&
            index + 1 < capacity
        )
        {
            destination[index] = source[index];
            index++;
        }
    }

    destination[index] = '\0';
}

static void append_text(
    char *destination,
    size_t capacity,
    const char *source
)
{
    if (
        destination == NULL ||
        capacity == 0 ||
        source == NULL
    )
    {
        return;
    }

    size_t position = string_length(destination);
    size_t index = 0;

    while (
        source[index] != '\0' &&
        position + 1 < capacity
    )
    {
        destination[position++] = source[index++];
    }

    destination[position] = '\0';
}

static void unsigned_to_text(
    uint64_t value,
    char *buffer,
    size_t capacity
)
{
    if (buffer == NULL || capacity == 0)
    {
        return;
    }

    char reverse[24];
    size_t count = 0;

    do
    {
        reverse[count++] = (char)('0' + value % 10U);
        value /= 10U;
    }
    while (value != 0 && count < sizeof(reverse));

    size_t output = 0;

    while (count > 0 && output + 1 < capacity)
    {
        buffer[output++] = reverse[--count];
    }

    buffer[output] = '\0';
}

static void build_path(
    const vfs_node_t *node,
    char *buffer,
    size_t capacity
)
{
    if (buffer == NULL || capacity == 0)
    {
        return;
    }

    buffer[0] = '\0';

    if (node == NULL || node->parent == NULL)
    {
        copy_text(buffer, capacity, "/");
        return;
    }

    const vfs_node_t *parts[32];
    uint32_t count = 0;
    const vfs_node_t *current = node;

    while (
        current != NULL &&
        current->parent != NULL &&
        count < 32U
    )
    {
        parts[count++] = current;
        current = current->parent;
    }

    append_text(buffer, capacity, "/");

    while (count > 0)
    {
        count--;
        append_text(buffer, capacity, parts[count]->name);

        if (count > 0)
        {
            append_text(buffer, capacity, "/");
        }
    }
}

static void build_selected_path(desktop_dialog_t *dialog)
{
    if (dialog == NULL)
    {
        return;
    }

    dialog->selected_path[0] = '\0';

    if (dialog->selected != NULL)
    {
        build_path(
            dialog->selected,
            dialog->selected_path,
            sizeof(dialog->selected_path)
        );

        return;
    }

    build_path(
        dialog->directory,
        dialog->selected_path,
        sizeof(dialog->selected_path)
    );

    if (
        dialog->kind == DESKTOP_DIALOG_SAVE_FILE &&
        dialog->input[0] != '\0'
    )
    {
        size_t length = string_length(dialog->selected_path);

        if (
            length > 0 &&
            dialog->selected_path[length - 1] != '/'
        )
        {
            append_text(
                dialog->selected_path,
                sizeof(dialog->selected_path),
                "/"
            );
        }

        append_text(
            dialog->selected_path,
            sizeof(dialog->selected_path),
            dialog->input
        );
    }
}

void desktop_dialog_init(desktop_dialog_t *dialog)
{
    if (dialog == NULL)
    {
        return;
    }

    clear_bytes(dialog, sizeof(*dialog));
    dialog->kind = DESKTOP_DIALOG_NONE;
    dialog->result = DESKTOP_DIALOG_RESULT_NONE;
    dialog->hovered_row = UINT32_MAX;
    dialog->selected_color = 0x39A0EDU;
}

static void show_common(
    desktop_dialog_t *dialog,
    desktop_dialog_kind_t kind,
    const char *title
)
{
    if (dialog == NULL)
    {
        return;
    }

    desktop_dialog_init(dialog);
    dialog->kind = kind;
    dialog->visible = true;
    copy_text(dialog->title, sizeof(dialog->title), title);
}

void desktop_dialog_show_message(
    desktop_dialog_t *dialog,
    const char *title,
    const char *message
)
{
    show_common(dialog, DESKTOP_DIALOG_MESSAGE, title);

    if (dialog != NULL)
    {
        copy_text(
            dialog->message,
            sizeof(dialog->message),
            message
        );
    }
}

void desktop_dialog_show_confirm(
    desktop_dialog_t *dialog,
    const char *title,
    const char *message
)
{
    show_common(dialog, DESKTOP_DIALOG_CONFIRM, title);

    if (dialog != NULL)
    {
        copy_text(
            dialog->message,
            sizeof(dialog->message),
            message
        );
    }
}

static vfs_node_t *valid_start_directory(vfs_node_t *node)
{
    if (node != NULL && node->type == VFS_NODE_DIRECTORY)
    {
        return node;
    }

    return vfs_root();
}

void desktop_dialog_show_open_file(
    desktop_dialog_t *dialog,
    const char *title,
    vfs_node_t *start_directory
)
{
    show_common(dialog, DESKTOP_DIALOG_OPEN_FILE, title);

    if (dialog != NULL)
    {
        dialog->directory = valid_start_directory(start_directory);
    }
}

void desktop_dialog_show_save_file(
    desktop_dialog_t *dialog,
    const char *title,
    vfs_node_t *start_directory,
    const char *default_name
)
{
    show_common(dialog, DESKTOP_DIALOG_SAVE_FILE, title);

    if (dialog != NULL)
    {
        dialog->directory = valid_start_directory(start_directory);
        copy_text(
            dialog->input,
            sizeof(dialog->input),
            default_name
        );

        dialog->input_length = string_length(dialog->input);
    }
}

void desktop_dialog_show_select_folder(
    desktop_dialog_t *dialog,
    const char *title,
    vfs_node_t *start_directory
)
{
    show_common(dialog, DESKTOP_DIALOG_SELECT_FOLDER, title);

    if (dialog != NULL)
    {
        dialog->directory = valid_start_directory(start_directory);
        dialog->selected = dialog->directory;
    }
}

void desktop_dialog_show_color(
    desktop_dialog_t *dialog,
    const char *title,
    uint32_t initial_color
)
{
    show_common(dialog, DESKTOP_DIALOG_COLOR, title);

    if (dialog != NULL)
    {
        dialog->selected_color = initial_color & 0xFFFFFFU;
    }
}

void desktop_dialog_show_properties(
    desktop_dialog_t *dialog,
    vfs_node_t *node
)
{
    show_common(
        dialog,
        DESKTOP_DIALOG_PROPERTIES,
        "Properties"
    );

    if (dialog == NULL)
    {
        return;
    }

    dialog->selected = node;

    if (node != NULL)
    {
        dialog->owner_uid = node->owner_uid;
        dialog->owner_gid = node->owner_gid;
        dialog->mode = node->mode;
        build_selected_path(dialog);
    }
}

void desktop_dialog_close(
    desktop_dialog_t *dialog,
    desktop_dialog_result_t result
)
{
    if (dialog == NULL)
    {
        return;
    }

    if (result == DESKTOP_DIALOG_RESULT_ACCEPTED)
    {
        build_selected_path(dialog);
    }

    dialog->result = result;
    dialog->visible = false;
}

void desktop_dialog_layout(
    desktop_dialog_t *dialog,
    uint32_t screen_width,
    uint32_t screen_height
)
{
    if (dialog == NULL)
    {
        return;
    }

    uint32_t width = DIALOG_WIDTH;
    uint32_t height = DIALOG_MESSAGE_HEIGHT;

    if (
        dialog->kind == DESKTOP_DIALOG_OPEN_FILE ||
        dialog->kind == DESKTOP_DIALOG_SAVE_FILE ||
        dialog->kind == DESKTOP_DIALOG_SELECT_FOLDER
    )
    {
        height = DIALOG_PICKER_HEIGHT;
    }
    else if (dialog->kind == DESKTOP_DIALOG_COLOR)
    {
        height = DIALOG_COLOR_HEIGHT;
    }
    else if (dialog->kind == DESKTOP_DIALOG_PROPERTIES)
    {
        height = DIALOG_PROPERTIES_HEIGHT;
    }

    if (width > screen_width)
    {
        width = screen_width;
    }

    if (height > screen_height)
    {
        height = screen_height;
    }

    dialog->bounds = (ui_rect_t){
        .x = (int32_t)(screen_width - width) / 2,
        .y = (int32_t)(screen_height - height) / 2,
        .width = width,
        .height = height
    };
}

static ui_rect_t title_bounds(const desktop_dialog_t *dialog)
{
    return (ui_rect_t){
        dialog->bounds.x,
        dialog->bounds.y,
        dialog->bounds.width,
        30
    };
}

static ui_rect_t accept_bounds(const desktop_dialog_t *dialog)
{
    return (ui_rect_t){
        dialog->bounds.x +
            (int32_t)dialog->bounds.width - 190,
        dialog->bounds.y +
            (int32_t)dialog->bounds.height - 42,
        82,
        28
    };
}

static ui_rect_t cancel_bounds(const desktop_dialog_t *dialog)
{
    return (ui_rect_t){
        dialog->bounds.x +
            (int32_t)dialog->bounds.width - 98,
        dialog->bounds.y +
            (int32_t)dialog->bounds.height - 42,
        82,
        28
    };
}

static ui_rect_t picker_list_bounds(const desktop_dialog_t *dialog)
{
    return (ui_rect_t){
        dialog->bounds.x + 16,
        dialog->bounds.y + 78,
        dialog->bounds.width - 52U,
        DIALOG_VISIBLE_ROWS * DIALOG_ROW_HEIGHT
    };
}

static ui_rect_t picker_scrollbar_bounds(
    const desktop_dialog_t *dialog
)
{
    ui_rect_t list = picker_list_bounds(dialog);

    return (ui_rect_t){
        list.x + (int32_t)list.width + 4,
        list.y,
        20,
        list.height
    };
}

static ui_rect_t picker_input_bounds(
    const desktop_dialog_t *dialog
)
{
    return (ui_rect_t){
        dialog->bounds.x + 16,
        dialog->bounds.y +
            (int32_t)dialog->bounds.height - 76,
        dialog->bounds.width - 222U,
        28
    };
}

static uint32_t directory_entry_count(
    const desktop_dialog_t *dialog
)
{
    if (dialog == NULL || dialog->directory == NULL)
    {
        return 0;
    }

    uint32_t count =
        dialog->directory->parent != NULL ? 1U : 0U;

    vfs_node_t *node = dialog->directory->first_child;

    while (node != NULL)
    {
        count++;
        node = node->next_sibling;
    }

    return count;
}

static vfs_node_t *directory_entry_at(
    const desktop_dialog_t *dialog,
    uint32_t row,
    bool *parent_entry
)
{
    if (parent_entry != NULL)
    {
        *parent_entry = false;
    }

    if (dialog == NULL || dialog->directory == NULL)
    {
        return NULL;
    }

    if (dialog->directory->parent != NULL)
    {
        if (row == 0)
        {
            if (parent_entry != NULL)
            {
                *parent_entry = true;
            }

            return dialog->directory->parent;
        }

        row--;
    }

    vfs_node_t *node = dialog->directory->first_child;

    while (node != NULL && row > 0)
    {
        node = node->next_sibling;
        row--;
    }

    return node;
}

static void render_wrapped_message(
    const char *message,
    int32_t x,
    int32_t y,
    uint32_t maximum_columns,
    uint32_t color
)
{
    if (message == NULL || maximum_columns == 0)
    {
        return;
    }

    char line[72];
    uint32_t column = 0;
    uint32_t visual_line = 0;

    for (size_t index = 0;; index++)
    {
        char character = message[index];
        bool finish = character == '\0';
        bool line_break = character == '\n';

        if (
            finish ||
            line_break ||
            column >= maximum_columns
        )
        {
            line[column] = '\0';
            ui_draw_text(
                line,
                x,
                y + (int32_t)visual_line * 14,
                color
            );

            visual_line++;
            column = 0;

            if (finish)
            {
                break;
            }

            if (line_break)
            {
                continue;
            }
        }

        if (character >= 32)
        {
            if (column + 1 < sizeof(line))
            {
                line[column++] = character;
            }
        }
    }
}

static void render_picker(
    const desktop_dialog_t *dialog,
    const ui_palette_t *palette
)
{
    char path[512];
    build_path(dialog->directory, path, sizeof(path));

    ui_draw_text(
        "Location:",
        dialog->bounds.x + 16,
        dialog->bounds.y + 48,
        palette->text
    );

    ui_rect_t path_rect = {
        dialog->bounds.x + 88,
        dialog->bounds.y + 39,
        dialog->bounds.width - 104U,
        26
    };

    ui_control_draw_text_input(
        &path_rect,
        path,
        "Root",
        0,
        palette,
        UI_CONTROL_DISABLED
    );

    ui_rect_t list = picker_list_bounds(dialog);
    ui_fill_rect(&list, palette->field);
    ui_draw_border(&list, palette->border, 1);

    for (uint32_t row = 0; row < DIALOG_VISIBLE_ROWS; row++)
    {
        uint32_t entry_index = dialog->scroll_offset + row;
        bool parent_entry = false;
        vfs_node_t *node = directory_entry_at(
            dialog,
            entry_index,
            &parent_entry
        );

        if (node == NULL)
        {
            break;
        }

        ui_rect_t row_rect = {
            list.x + 2,
            list.y + 2 + (int32_t)row *
                (int32_t)DIALOG_ROW_HEIGHT,
            list.width - 4U,
            DIALOG_ROW_HEIGHT
        };

        ui_control_draw_list_row(
            &row_rect,
            parent_entry ? "[UP]" :
                (node->type == VFS_NODE_DIRECTORY ?
                    "[D]" : "[F]"),
            parent_entry ? ".." : node->name,
            palette,
            !parent_entry && node == dialog->selected,
            row == dialog->hovered_row
        );
    }

    uint32_t entries = directory_entry_count(dialog);
    ui_scrollbar_t scrollbar = {
        .value = dialog->scroll_offset,
        .page_size = DIALOG_VISIBLE_ROWS,
        .maximum = entries > DIALOG_VISIBLE_ROWS ?
            entries - DIALOG_VISIBLE_ROWS : 0
    };

    ui_rect_t scroll = picker_scrollbar_bounds(dialog);
    ui_control_draw_scrollbar_vertical(
        &scroll,
        &scrollbar,
        palette,
        UI_CONTROL_NORMAL
    );

    if (dialog->kind == DESKTOP_DIALOG_SAVE_FILE)
    {
        ui_rect_t input = picker_input_bounds(dialog);
        ui_control_draw_text_input(
            &input,
            dialog->input,
            "File name",
            dialog->input_length,
            palette,
            UI_CONTROL_FOCUSED
        );
    }
}

static uint8_t color_component(
    uint32_t color,
    uint32_t shift
)
{
    return (uint8_t)((color >> shift) & 0xFFU);
}

static void render_color_dialog(
    const desktop_dialog_t *dialog,
    const ui_palette_t *palette
)
{
    ui_rect_t preview = {
        dialog->bounds.x + 24,
        dialog->bounds.y + 58,
        120,
        120
    };

    ui_fill_rect(&preview, dialog->selected_color);
    ui_draw_border(&preview, palette->border, 2);

    static const char *labels[3] = { "Red", "Green", "Blue" };
    static const uint32_t shifts[3] = { 16U, 8U, 0U };

    for (uint32_t index = 0; index < 3; index++)
    {
        int32_t y = dialog->bounds.y + 62 +
            (int32_t)index * 48;

        ui_draw_text(
            labels[index],
            dialog->bounds.x + 172,
            y + 8,
            palette->text
        );

        ui_rect_t slider = {
            dialog->bounds.x + 226,
            y,
            dialog->bounds.width - 254U,
            28
        };

        ui_control_draw_slider(
            &slider,
            color_component(
                dialog->selected_color,
                shifts[index]
            ),
            255,
            palette,
            UI_CONTROL_NORMAL
        );
    }
}

static void render_properties(
    const desktop_dialog_t *dialog,
    const ui_palette_t *palette
)
{
    vfs_node_t *node = dialog->selected;
    int32_t x = dialog->bounds.x + 24;
    int32_t y = dialog->bounds.y + 54;

    if (node == NULL)
    {
        ui_draw_text("No item selected.", x, y, palette->text);
        return;
    }

    char number[32];

    ui_draw_text("Name:", x, y, palette->text);
    ui_draw_text(node->name, x + 96, y, palette->text);
    y += 28;

    ui_draw_text("Type:", x, y, palette->text);
    ui_draw_text(
        node->type == VFS_NODE_DIRECTORY ?
            "Directory" : "File",
        x + 96,
        y,
        palette->text
    );
    y += 28;

    ui_draw_text("Size:", x, y, palette->text);
    unsigned_to_text(node->size, number, sizeof(number));
    ui_draw_text(number, x + 96, y, palette->text);
    y += 28;

    ui_draw_text("Owner UID:", x, y, palette->text);
    unsigned_to_text(node->owner_uid, number, sizeof(number));
    ui_draw_text(number, x + 96, y, palette->text);
    y += 28;

    ui_draw_text("Owner GID:", x, y, palette->text);
    unsigned_to_text(node->owner_gid, number, sizeof(number));
    ui_draw_text(number, x + 96, y, palette->text);
    y += 28;

    ui_draw_text("Path:", x, y, palette->text);

    ui_rect_t path = {
        x + 96,
        y - 8,
        dialog->bounds.width - 144U,
        26
    };

    ui_control_draw_text_input(
        &path,
        dialog->selected_path,
        "",
        0,
        palette,
        UI_CONTROL_DISABLED
    );
}

void desktop_dialog_render(
    const desktop_dialog_t *dialog,
    const ui_palette_t *palette
)
{
    if (
        dialog == NULL ||
        !dialog->visible ||
        palette == NULL
    )
    {
        return;
    }

    ui_rect_t dim = {
        0,
        0,
        (uint32_t)(dialog->bounds.x * 2) +
            dialog->bounds.width,
        (uint32_t)(dialog->bounds.y * 2) +
            dialog->bounds.height
    };

    ui_fill_rect(&dim, 0x203040U);
    ui_fill_rect(&dialog->bounds, palette->panel);
    ui_draw_border(&dialog->bounds, palette->border, 2);

    ui_rect_t title = title_bounds(dialog);
    ui_fill_rect(&title, palette->accent);
    ui_draw_text(
        dialog->title,
        title.x + 10,
        title.y + 11,
        palette->light_text
    );

    if (
        dialog->kind == DESKTOP_DIALOG_MESSAGE ||
        dialog->kind == DESKTOP_DIALOG_CONFIRM
    )
    {
        render_wrapped_message(
            dialog->message,
            dialog->bounds.x + 20,
            dialog->bounds.y + 54,
            62,
            palette->text
        );
    }
    else if (
        dialog->kind == DESKTOP_DIALOG_OPEN_FILE ||
        dialog->kind == DESKTOP_DIALOG_SAVE_FILE ||
        dialog->kind == DESKTOP_DIALOG_SELECT_FOLDER
    )
    {
        render_picker(dialog, palette);
    }
    else if (dialog->kind == DESKTOP_DIALOG_COLOR)
    {
        render_color_dialog(dialog, palette);
    }
    else if (dialog->kind == DESKTOP_DIALOG_PROPERTIES)
    {
        render_properties(dialog, palette);
    }

    ui_rect_t accept = accept_bounds(dialog);
    ui_rect_t cancel = cancel_bounds(dialog);

    const char *accept_label = "OK";

    if (dialog->kind == DESKTOP_DIALOG_OPEN_FILE)
    {
        accept_label = "Open";
    }
    else if (dialog->kind == DESKTOP_DIALOG_SAVE_FILE)
    {
        accept_label = "Save";
    }
    else if (dialog->kind == DESKTOP_DIALOG_SELECT_FOLDER)
    {
        accept_label = "Select";
    }
    else if (dialog->kind == DESKTOP_DIALOG_CONFIRM)
    {
        accept_label = "Yes";
    }

    bool accept_enabled = true;

    if (dialog->kind == DESKTOP_DIALOG_OPEN_FILE)
    {
        accept_enabled =
            dialog->selected != NULL &&
            dialog->selected->type == VFS_NODE_FILE;
    }
    else if (dialog->kind == DESKTOP_DIALOG_SAVE_FILE)
    {
        accept_enabled = dialog->input[0] != '\0';
    }
    else if (dialog->kind == DESKTOP_DIALOG_SELECT_FOLDER)
    {
        accept_enabled = dialog->directory != NULL;
    }

    ui_control_draw_button(
        &accept,
        accept_label,
        palette,
        accept_enabled ?
            UI_CONTROL_NORMAL : UI_CONTROL_DISABLED
    );

    if (dialog->kind != DESKTOP_DIALOG_MESSAGE)
    {
        ui_control_draw_button(
            &cancel,
            dialog->kind == DESKTOP_DIALOG_CONFIRM ?
                "No" : "Cancel",
            palette,
            UI_CONTROL_NORMAL
        );
    }
}

bool desktop_dialog_handle_mouse_move(
    desktop_dialog_t *dialog,
    int32_t x,
    int32_t y
)
{
    if (dialog == NULL || !dialog->visible)
    {
        return false;
    }

    uint32_t previous = dialog->hovered_row;
    dialog->hovered_row = UINT32_MAX;

    if (
        dialog->kind == DESKTOP_DIALOG_OPEN_FILE ||
        dialog->kind == DESKTOP_DIALOG_SAVE_FILE ||
        dialog->kind == DESKTOP_DIALOG_SELECT_FOLDER
    )
    {
        ui_rect_t list = picker_list_bounds(dialog);

        if (ui_point_in_rect(x, y, &list))
        {
            int32_t relative = y - list.y - 2;

            if (relative >= 0)
            {
                uint32_t row =
                    (uint32_t)relative / DIALOG_ROW_HEIGHT;

                if (row < DIALOG_VISIBLE_ROWS)
                {
                    dialog->hovered_row = row;
                }
            }
        }
    }

    return previous != dialog->hovered_row;
}

static bool picker_accept_valid(desktop_dialog_t *dialog)
{
    if (dialog == NULL)
    {
        return false;
    }

    if (dialog->kind == DESKTOP_DIALOG_OPEN_FILE)
    {
        return
            dialog->selected != NULL &&
            dialog->selected->type == VFS_NODE_FILE;
    }

    if (dialog->kind == DESKTOP_DIALOG_SAVE_FILE)
    {
        return dialog->input[0] != '\0';
    }

    if (dialog->kind == DESKTOP_DIALOG_SELECT_FOLDER)
    {
        dialog->selected = dialog->directory;
        return dialog->directory != NULL;
    }

    return true;
}

static bool handle_picker_click(
    desktop_dialog_t *dialog,
    int32_t x,
    int32_t y
)
{
    ui_rect_t list = picker_list_bounds(dialog);

    if (ui_point_in_rect(x, y, &list))
    {
        int32_t relative = y - list.y - 2;

        if (relative < 0)
        {
            return true;
        }

        uint32_t row =
            (uint32_t)relative / DIALOG_ROW_HEIGHT;

        if (row >= DIALOG_VISIBLE_ROWS)
        {
            return true;
        }

        bool parent_entry = false;
        vfs_node_t *node = directory_entry_at(
            dialog,
            dialog->scroll_offset + row,
            &parent_entry
        );

        if (node == NULL)
        {
            return true;
        }

        if (
            parent_entry ||
            node->type == VFS_NODE_DIRECTORY
        )
        {
            dialog->directory = node;
            dialog->selected =
                dialog->kind == DESKTOP_DIALOG_SELECT_FOLDER ?
                    node : NULL;

            dialog->scroll_offset = 0;
        }
        else
        {
            dialog->selected = node;

            if (dialog->kind == DESKTOP_DIALOG_SAVE_FILE)
            {
                copy_text(
                    dialog->input,
                    sizeof(dialog->input),
                    node->name
                );

                dialog->input_length =
                    string_length(dialog->input);
            }
        }

        return true;
    }

    ui_rect_t scroll = picker_scrollbar_bounds(dialog);

    if (ui_point_in_rect(x, y, &scroll))
    {
        uint32_t entries = directory_entry_count(dialog);
        ui_scrollbar_t scrollbar = {
            .value = dialog->scroll_offset,
            .page_size = DIALOG_VISIBLE_ROWS,
            .maximum = entries > DIALOG_VISIBLE_ROWS ?
                entries - DIALOG_VISIBLE_ROWS : 0
        };

        dialog->scroll_offset =
            ui_control_scrollbar_value_from_pointer(
                &scroll,
                &scrollbar,
                y
            );

        return true;
    }

    return false;
}

static bool handle_color_click(
    desktop_dialog_t *dialog,
    int32_t x,
    int32_t y
)
{
    static const uint32_t shifts[3] = { 16U, 8U, 0U };

    for (uint32_t index = 0; index < 3; index++)
    {
        int32_t slider_y = dialog->bounds.y + 62 +
            (int32_t)index * 48;

        ui_rect_t slider = {
            dialog->bounds.x + 226,
            slider_y,
            dialog->bounds.width - 254U,
            28
        };

        if (!ui_point_in_rect(x, y, &slider))
        {
            continue;
        }

        uint32_t value = ui_control_slider_value(
            &slider,
            x,
            255
        );

        uint32_t mask = 0xFFU << shifts[index];
        dialog->selected_color =
            (dialog->selected_color & ~mask) |
            (value << shifts[index]);

        return true;
    }

    return false;
}

bool desktop_dialog_handle_click(
    desktop_dialog_t *dialog,
    int32_t x,
    int32_t y
)
{
    if (dialog == NULL || !dialog->visible)
    {
        return false;
    }

    ui_rect_t accept = accept_bounds(dialog);
    ui_rect_t cancel = cancel_bounds(dialog);

    if (ui_point_in_rect(x, y, &accept))
    {
        if (picker_accept_valid(dialog))
        {
            desktop_dialog_close(
                dialog,
                DESKTOP_DIALOG_RESULT_ACCEPTED
            );
        }

        return true;
    }

    if (
        dialog->kind != DESKTOP_DIALOG_MESSAGE &&
        ui_point_in_rect(x, y, &cancel)
    )
    {
        desktop_dialog_close(
            dialog,
            DESKTOP_DIALOG_RESULT_CANCELLED
        );

        return true;
    }

    if (
        dialog->kind == DESKTOP_DIALOG_OPEN_FILE ||
        dialog->kind == DESKTOP_DIALOG_SAVE_FILE ||
        dialog->kind == DESKTOP_DIALOG_SELECT_FOLDER
    )
    {
        return handle_picker_click(dialog, x, y);
    }

    if (dialog->kind == DESKTOP_DIALOG_COLOR)
    {
        return handle_color_click(dialog, x, y);
    }

    return ui_point_in_rect(x, y, &dialog->bounds);
}

bool desktop_dialog_handle_key(
    desktop_dialog_t *dialog,
    char character
)
{
    if (dialog == NULL || !dialog->visible)
    {
        return false;
    }

    if (character == 27)
    {
        desktop_dialog_close(
            dialog,
            DESKTOP_DIALOG_RESULT_CANCELLED
        );

        return true;
    }

    if (character == '\n')
    {
        if (picker_accept_valid(dialog))
        {
            desktop_dialog_close(
                dialog,
                DESKTOP_DIALOG_RESULT_ACCEPTED
            );
        }

        return true;
    }

    if (dialog->kind != DESKTOP_DIALOG_SAVE_FILE)
    {
        return false;
    }

    if (character == '\b')
    {
        if (dialog->input_length > 0)
        {
            dialog->input[--dialog->input_length] = '\0';
        }

        return true;
    }

    if (
        character >= 32 &&
        character <= 126 &&
        character != '/' &&
        dialog->input_length + 1 < sizeof(dialog->input)
    )
    {
        dialog->input[dialog->input_length++] = character;
        dialog->input[dialog->input_length] = '\0';
        return true;
    }

    return false;
}

bool desktop_dialog_take_result(
    desktop_dialog_t *dialog,
    desktop_dialog_result_t *result
)
{
    if (
        dialog == NULL ||
        result == NULL ||
        dialog->result == DESKTOP_DIALOG_RESULT_NONE
    )
    {
        return false;
    }

    *result = dialog->result;
    dialog->result = DESKTOP_DIALOG_RESULT_NONE;
    return true;
}

const char *desktop_dialog_selected_path(
    const desktop_dialog_t *dialog
)
{
    return dialog == NULL ? NULL : dialog->selected_path;
}

vfs_node_t *desktop_dialog_selected_node(
    const desktop_dialog_t *dialog
)
{
    return dialog == NULL ? NULL : dialog->selected;
}

uint32_t desktop_dialog_selected_color(
    const desktop_dialog_t *dialog
)
{
    return dialog == NULL ? 0 : dialog->selected_color;
}
