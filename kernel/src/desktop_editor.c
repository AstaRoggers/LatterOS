#include "desktop_editor.h"

#include "desktop_services.h"
#include "ui_controls.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EDITOR_BUFFER_CAPACITY 16384U
#define EDITOR_LINE_HEIGHT 12U
#define EDITOR_TOOLBAR_HEIGHT 32U
#define EDITOR_STATUS_HEIGHT 22U
#define EDITOR_SCROLLBAR_WIDTH 18U
#define EDITOR_GUTTER_WIDTH 38U
#define EDITOR_MAX_RENDER_COLUMNS 160U

static char text_buffer[EDITOR_BUFFER_CAPACITY];
static size_t text_length_value;
static size_t cursor_offset;
static size_t selection_anchor;
static size_t selection_focus;
static uint32_t scroll_line;
static vfs_node_t *open_node;
static bool dirty;
static bool selecting;
static bool initialized;
static char status_text[96];


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
            index + 1U < capacity
        )
        {
            destination[index] = source[index];
            index++;
        }
    }

    destination[index] = '\0';
}

static void set_status(const char *text)
{
    copy_text(status_text, sizeof(status_text), text);
}

static ui_palette_t palette(void)
{
    const gui_theme_t *theme = desktop_services_theme();

    return (ui_palette_t){
        .background = theme->desktop,
        .panel = theme->window,
        .border = theme->window_border,
        .text = theme->text,
        .light_text = theme->light_text,
        .field = theme->field,
        .accent = theme->accent,
        .accent_hover = theme->row_selected,
        .selected = theme->row_selected,
        .disabled = theme->title_idle,
        .danger = theme->close
    };
}

static ui_rect_t toolbar_bounds(const ui_rect_t *content)
{
    return (ui_rect_t){
        content->x + 6,
        content->y + 5,
        content->width > 12U ? content->width - 12U : 1U,
        EDITOR_TOOLBAR_HEIGHT
    };
}

static ui_rect_t save_button_bounds(const ui_rect_t *content)
{
    ui_rect_t toolbar = toolbar_bounds(content);

    return (ui_rect_t){
        toolbar.x + 4,
        toolbar.y + 4,
        72,
        24
    };
}

static ui_rect_t select_button_bounds(const ui_rect_t *content)
{
    ui_rect_t toolbar = toolbar_bounds(content);

    return (ui_rect_t){
        toolbar.x + 82,
        toolbar.y + 4,
        92,
        24
    };
}

static ui_rect_t text_area_bounds(const ui_rect_t *content)
{
    uint32_t vertical =
        EDITOR_TOOLBAR_HEIGHT + EDITOR_STATUS_HEIGHT + 18U;

    return (ui_rect_t){
        content->x + 6,
        content->y + (int32_t)EDITOR_TOOLBAR_HEIGHT + 10,
        content->width > 12U + EDITOR_SCROLLBAR_WIDTH ?
            content->width - 12U - EDITOR_SCROLLBAR_WIDTH : 1U,
        content->height > vertical ?
            content->height - vertical : 1U
    };
}

static ui_rect_t scrollbar_bounds(const ui_rect_t *content)
{
    ui_rect_t text = text_area_bounds(content);

    return (ui_rect_t){
        text.x + (int32_t)text.width + 2,
        text.y,
        EDITOR_SCROLLBAR_WIDTH - 2U,
        text.height
    };
}

static ui_rect_t status_bounds(const ui_rect_t *content)
{
    return (ui_rect_t){
        content->x + 6,
        content->y + (int32_t)content->height -
            (int32_t)EDITOR_STATUS_HEIGHT - 5,
        content->width > 12U ? content->width - 12U : 1U,
        EDITOR_STATUS_HEIGHT
    };
}

static uint32_t line_count(void)
{
    uint32_t count = 1;

    for (size_t index = 0; index < text_length_value; index++)
    {
        if (text_buffer[index] == '\n')
        {
            count++;
        }
    }

    return count;
}

static uint32_t visible_line_count(const ui_rect_t *content)
{
    ui_rect_t text = text_area_bounds(content);
    uint32_t count = text.height / EDITOR_LINE_HEIGHT;
    return count == 0 ? 1U : count;
}

static size_t line_start(uint32_t requested_line)
{
    uint32_t line = 0;
    size_t offset = 0;

    while (
        offset < text_length_value &&
        line < requested_line
    )
    {
        if (text_buffer[offset++] == '\n')
        {
            line++;
        }
    }

    return offset;
}

static size_t line_end(size_t start)
{
    size_t offset = start;

    while (
        offset < text_length_value &&
        text_buffer[offset] != '\n'
    )
    {
        offset++;
    }

    return offset;
}

static uint32_t line_for_offset(size_t requested_offset)
{
    if (requested_offset > text_length_value)
    {
        requested_offset = text_length_value;
    }

    uint32_t line = 0;

    for (size_t offset = 0; offset < requested_offset; offset++)
    {
        if (text_buffer[offset] == '\n')
        {
            line++;
        }
    }

    return line;
}

static void selection_bounds(size_t *start, size_t *end)
{
    size_t first = selection_anchor;
    size_t second = selection_focus;

    if (first > second)
    {
        size_t temporary = first;
        first = second;
        second = temporary;
    }

    if (first > text_length_value)
    {
        first = text_length_value;
    }

    if (second > text_length_value)
    {
        second = text_length_value;
    }

    if (start != NULL)
    {
        *start = first;
    }

    if (end != NULL)
    {
        *end = second;
    }
}

bool desktop_editor_has_selection(void)
{
    return selection_anchor != selection_focus;
}

static void clear_selection(void)
{
    selection_anchor = cursor_offset;
    selection_focus = cursor_offset;
}

static void delete_selection(void)
{
    if (!desktop_editor_has_selection())
    {
        return;
    }

    size_t start;
    size_t end;
    selection_bounds(&start, &end);
    size_t tail = text_length_value - end;

    for (size_t index = 0; index <= tail; index++)
    {
        text_buffer[start + index] = text_buffer[end + index];
    }

    text_length_value -= end - start;
    cursor_offset = start;
    clear_selection();
    dirty = true;
}

static bool insert_character(char character)
{
    if (text_length_value + 1U >= EDITOR_BUFFER_CAPACITY)
    {
        set_status("Document is full");
        return false;
    }

    delete_selection();

    for (
        size_t index = text_length_value;
        index > cursor_offset;
        index--
    )
    {
        text_buffer[index] = text_buffer[index - 1U];
    }

    text_buffer[cursor_offset++] = character;
    text_length_value++;
    text_buffer[text_length_value] = '\0';
    clear_selection();
    dirty = true;
    set_status("Modified");
    return true;
}

static void keep_cursor_visible(const ui_rect_t *content)
{
    uint32_t cursor_line = line_for_offset(cursor_offset);
    uint32_t visible = visible_line_count(content);

    if (cursor_line < scroll_line)
    {
        scroll_line = cursor_line;
    }
    else if (cursor_line >= scroll_line + visible)
    {
        scroll_line = cursor_line - visible + 1U;
    }
}

static size_t offset_from_point(
    const ui_rect_t *content,
    int32_t x,
    int32_t y
)
{
    ui_rect_t text = text_area_bounds(content);
    int32_t relative_y = y - text.y - 4;
    int32_t relative_x = x - text.x -
        (int32_t)EDITOR_GUTTER_WIDTH - 5;

    if (relative_y < 0)
    {
        relative_y = 0;
    }

    if (relative_x < 0)
    {
        relative_x = 0;
    }

    uint32_t line = scroll_line +
        (uint32_t)relative_y / EDITOR_LINE_HEIGHT;
    size_t start = line_start(line);
    size_t end = line_end(start);
    size_t column = (size_t)relative_x / 8U;

    if (column > end - start)
    {
        column = end - start;
    }

    return start + column;
}

void desktop_editor_init(void)
{
    if (initialized)
    {
        return;
    }

    initialized = true;
    text_buffer[0] = '\0';
    text_length_value = 0;
    cursor_offset = 0;
    selection_anchor = 0;
    selection_focus = 0;
    scroll_line = 0;
    open_node = NULL;
    dirty = false;
    selecting = false;
    set_status("No file open");
}

void desktop_editor_open_node(vfs_node_t *node)
{
    desktop_editor_init();

    if (node == NULL || node->type != VFS_NODE_FILE)
    {
        set_status("Unable to open file");
        return;
    }

    size_t count = vfs_read(
        node,
        0,
        text_buffer,
        sizeof(text_buffer) - 1U
    );

    text_buffer[count] = '\0';
    text_length_value = count;
    cursor_offset = 0;
    selection_anchor = 0;
    selection_focus = 0;
    scroll_line = 0;
    open_node = node;
    dirty = false;
    selecting = false;
    set_status(
        count + 1U < sizeof(text_buffer) ?
            "File loaded" : "File truncated to editor capacity"
    );
}

bool desktop_editor_save(void)
{
    desktop_editor_init();

    if (open_node == NULL)
    {
        set_status("Open a file before saving");
        return false;
    }

    if (
        !vfs_truncate(open_node) ||
        (
            text_length_value != 0 &&
            vfs_write(
                open_node,
                0,
                text_buffer,
                text_length_value
            ) != text_length_value
        )
    )
    {
        set_status("Save failed");
        return false;
    }

    open_node->size = text_length_value;
    dirty = false;
    set_status("Saved");
    desktop_notify("Document saved", 3000U);
    return true;
}

static void render_line_number(
    uint32_t line,
    int32_t x,
    int32_t y,
    uint32_t color
)
{
    char number[8];
    uint32_t value = line + 1U;
    uint32_t count = 0;

    do
    {
        number[count++] = (char)('0' + value % 10U);
        value /= 10U;
    }
    while (value != 0 && count < sizeof(number) - 1U);

    for (uint32_t index = 0; index < count / 2U; index++)
    {
        char temporary = number[index];
        number[index] = number[count - 1U - index];
        number[count - 1U - index] = temporary;
    }

    number[count] = '\0';
    ui_draw_text(number, x, y, color);
}

void desktop_editor_render(const ui_rect_t *content)
{
    desktop_editor_init();

    if (content == NULL)
    {
        return;
    }

    ui_palette_t colors = palette();
    ui_rect_t toolbar = toolbar_bounds(content);
    ui_rect_t text = text_area_bounds(content);
    ui_rect_t scrollbar = scrollbar_bounds(content);
    ui_rect_t status = status_bounds(content);
    ui_rect_t save = save_button_bounds(content);
    ui_rect_t select = select_button_bounds(content);

    ui_control_draw_toolbar(&toolbar, &colors);
    ui_control_draw_button(
        &save,
        dirty ? "Save *" : "Save",
        &colors,
        open_node == NULL ?
            UI_CONTROL_DISABLED : UI_CONTROL_NORMAL
    );
    ui_control_draw_button(
        &select,
        "Select All",
        &colors,
        text_length_value == 0 ?
            UI_CONTROL_DISABLED : UI_CONTROL_NORMAL
    );

    ui_draw_text_ellipsized(
        open_node == NULL ? "Untitled" : open_node->name,
        &(ui_rect_t){
            toolbar.x + 184,
            toolbar.y + 4,
            toolbar.width > 190U ? toolbar.width - 190U : 1U,
            24
        },
        4,
        colors.text
    );

    ui_fill_rect(&text, colors.field);
    ui_draw_border(&text, colors.border, 1);

    ui_rect_t gutter = {
        text.x + 1,
        text.y + 1,
        EDITOR_GUTTER_WIDTH,
        text.height > 2U ? text.height - 2U : 1U
    };
    ui_fill_rect(&gutter, colors.panel);
    ui_draw_vertical_line(
        gutter.x + (int32_t)gutter.width,
        gutter.y,
        gutter.height,
        1,
        colors.border
    );

    uint32_t visible = visible_line_count(content);
    uint32_t total_lines = line_count();
    uint32_t maximum_scroll =
        total_lines > visible ? total_lines - visible : 0U;

    if (scroll_line > maximum_scroll)
    {
        scroll_line = maximum_scroll;
    }

    size_t selected_start;
    size_t selected_end;
    selection_bounds(&selected_start, &selected_end);

    for (uint32_t row = 0; row < visible; row++)
    {
        uint32_t line = scroll_line + row;

        if (line >= total_lines)
        {
            break;
        }

        size_t start = line_start(line);
        size_t end = line_end(start);
        int32_t y = text.y + 4 +
            (int32_t)row * (int32_t)EDITOR_LINE_HEIGHT;

        render_line_number(
            line,
            text.x + 4,
            y,
            colors.disabled
        );

        size_t highlight_start =
            selected_start > start ? selected_start : start;
        size_t highlight_end =
            selected_end < end ? selected_end : end;

        if (
            selected_start != selected_end &&
            highlight_end > highlight_start
        )
        {
            ui_rect_t highlight = {
                .x = text.x +
                    (int32_t)EDITOR_GUTTER_WIDTH + 5 +
                    (int32_t)(highlight_start - start) * 8,
                .y = y - 2,
                .width = (uint32_t)(
                    (highlight_end - highlight_start) * 8U
                ),
                .height = EDITOR_LINE_HEIGHT
            };
            ui_fill_rect(&highlight, colors.selected);
        }

        char line_buffer[EDITOR_MAX_RENDER_COLUMNS + 1U];
        size_t count = end - start;

        if (count > EDITOR_MAX_RENDER_COLUMNS)
        {
            count = EDITOR_MAX_RENDER_COLUMNS;
        }

        for (size_t column = 0; column < count; column++)
        {
            char character = text_buffer[start + column];
            line_buffer[column] =
                character == '\t' ? ' ' : character;
        }

        line_buffer[count] = '\0';
        ui_draw_text(
            line_buffer,
            text.x + (int32_t)EDITOR_GUTTER_WIDTH + 5,
            y,
            colors.text
        );

        if (
            cursor_offset >= start &&
            cursor_offset <= end
        )
        {
            int32_t cursor_x =
                text.x + (int32_t)EDITOR_GUTTER_WIDTH + 5 +
                (int32_t)(cursor_offset - start) * 8;

            ui_draw_vertical_line(
                cursor_x,
                y - 2,
                EDITOR_LINE_HEIGHT,
                1,
                colors.accent
            );
        }
    }

    ui_scrollbar_t scroll = {
        .value = scroll_line,
        .page_size = visible,
        .maximum = maximum_scroll
    };
    ui_control_draw_scrollbar_vertical(
        &scrollbar,
        &scroll,
        &colors,
        UI_CONTROL_NORMAL
    );

    ui_control_draw_statusbar(
        &status,
        status_text,
        dirty ? "Modified" : "Ready",
        &colors
    );
}

bool desktop_editor_handle_click(
    const ui_rect_t *content,
    int32_t x,
    int32_t y
)
{
    desktop_editor_init();

    if (content == NULL)
    {
        return false;
    }

    ui_rect_t save = save_button_bounds(content);
    ui_rect_t select = select_button_bounds(content);
    ui_rect_t text = text_area_bounds(content);
    ui_rect_t scrollbar = scrollbar_bounds(content);

    if (ui_point_in_rect(x, y, &save))
    {
        return desktop_editor_save();
    }

    if (ui_point_in_rect(x, y, &select))
    {
        desktop_editor_select_all();
        return true;
    }

    if (ui_point_in_rect(x, y, &scrollbar))
    {
        uint32_t visible = visible_line_count(content);
        uint32_t total = line_count();
        ui_scrollbar_t scroll = {
            .value = scroll_line,
            .page_size = visible,
            .maximum = total > visible ? total - visible : 0U
        };

        scroll_line = ui_control_scrollbar_value_from_pointer(
            &scrollbar,
            &scroll,
            y
        );
        return true;
    }

    if (ui_point_in_rect(x, y, &text))
    {
        cursor_offset = offset_from_point(content, x, y);
        selection_anchor = cursor_offset;
        selection_focus = cursor_offset;
        selecting = true;
        keep_cursor_visible(content);
        return true;
    }

    return false;
}

bool desktop_editor_handle_mouse_move(
    const ui_rect_t *content,
    int32_t x,
    int32_t y
)
{
    if (!selecting || content == NULL)
    {
        return false;
    }

    selection_focus = offset_from_point(content, x, y);
    cursor_offset = selection_focus;
    keep_cursor_visible(content);
    return true;
}

void desktop_editor_handle_mouse_up(void)
{
    selecting = false;
}

bool desktop_editor_handle_key(char character)
{
    desktop_editor_init();

    if (character == '\b')
    {
        if (desktop_editor_has_selection())
        {
            delete_selection();
            set_status("Modified");
            return true;
        }

        if (cursor_offset == 0)
        {
            return false;
        }

        selection_anchor = cursor_offset - 1U;
        selection_focus = cursor_offset;
        delete_selection();
        set_status("Modified");
        return true;
    }

    if (character == '\n' || character == '\t')
    {
        return insert_character(character);
    }

    if (character >= 32 && character <= 126)
    {
        return insert_character(character);
    }

    return false;
}

bool desktop_editor_copy(bool cut)
{
    if (!desktop_editor_has_selection())
    {
        set_status("No text selected");
        return false;
    }

    size_t start;
    size_t end;
    selection_bounds(&start, &end);

    if (!desktop_clipboard_set_range(
        text_buffer,
        start,
        end
    ))
    {
        set_status("Selection was truncated in clipboard");
    }
    else
    {
        set_status(cut ? "Selection cut" : "Selection copied");
    }

    if (cut)
    {
        delete_selection();
    }

    return true;
}

bool desktop_editor_paste(void)
{
    const char *clipboard = desktop_clipboard_text();

    if (clipboard == NULL || clipboard[0] == '\0')
    {
        set_status("Clipboard is empty");
        return false;
    }

    delete_selection();
    bool changed = false;

    for (size_t index = 0; clipboard[index] != '\0'; index++)
    {
        char character = clipboard[index];

        if (
            character == '\n' ||
            character == '\t' ||
            (character >= 32 && character <= 126)
        )
        {
            changed = insert_character(character) || changed;
        }
    }

    if (changed)
    {
        set_status("Clipboard pasted");
    }

    return changed;
}

void desktop_editor_select_all(void)
{
    selection_anchor = 0;
    selection_focus = text_length_value;
    cursor_offset = text_length_value;
    set_status("All text selected");
}

bool desktop_editor_dirty(void)
{
    return dirty;
}

const char *desktop_editor_name(void)
{
    return open_node == NULL ? "Untitled" : open_node->name;
}
