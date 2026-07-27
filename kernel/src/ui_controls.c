#include "ui_controls.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UI_MENU_ITEM_HEIGHT 24U
#define UI_MENU_SEPARATOR_HEIGHT 8U
#define UI_SCROLLBAR_MIN_THUMB 18U

static uint32_t state_background(
    const ui_palette_t *palette,
    ui_control_state_t state
)
{
    if (palette == NULL)
    {
        return 0;
    }

    switch (state)
    {
        case UI_CONTROL_PRESSED:
            return palette->selected;

        case UI_CONTROL_HOVERED:
            return palette->accent_hover;

        case UI_CONTROL_FOCUSED:
            return palette->field;

        case UI_CONTROL_DISABLED:
            return palette->disabled;

        case UI_CONTROL_NORMAL:
        default:
            return palette->panel;
    }
}

static uint32_t state_text(
    const ui_palette_t *palette,
    ui_control_state_t state
)
{
    if (palette == NULL)
    {
        return 0;
    }

    return state == UI_CONTROL_DISABLED ?
        palette->border : palette->text;
}

void ui_control_draw_panel(
    const ui_rect_t *rect,
    const ui_palette_t *palette,
    bool raised
)
{
    if (rect == NULL || palette == NULL)
    {
        return;
    }

    ui_fill_rect(rect, palette->panel);
    ui_draw_border(rect, palette->border, raised ? 2U : 1U);
}

void ui_control_draw_button(
    const ui_rect_t *rect,
    const char *label,
    const ui_palette_t *palette,
    ui_control_state_t state
)
{
    if (rect == NULL || palette == NULL)
    {
        return;
    }

    uint32_t background = state_background(palette, state);

    if (state == UI_CONTROL_NORMAL)
    {
        background = palette->accent;
    }

    ui_fill_rect(rect, background);
    ui_draw_border(rect, palette->border, 1);
    ui_draw_text_centered(
        label == NULL ? "" : label,
        rect,
        state == UI_CONTROL_DISABLED ?
            palette->border : palette->light_text
    );
}

void ui_control_draw_checkbox(
    const ui_rect_t *rect,
    const char *label,
    bool checked,
    const ui_palette_t *palette,
    ui_control_state_t state
)
{
    if (rect == NULL || palette == NULL)
    {
        return;
    }

    ui_rect_t box = {
        .x = rect->x,
        .y = rect->y +
            (int32_t)(rect->height > 16 ?
                (rect->height - 16U) / 2U : 0),
        .width = 16,
        .height = 16
    };

    ui_fill_rect(
        &box,
        state == UI_CONTROL_DISABLED ?
            palette->disabled : palette->field
    );

    ui_draw_border(&box, palette->border, 1);

    if (checked)
    {
        ui_rect_t mark = ui_inset(&box, 4, 4);
        ui_fill_rect(&mark, palette->accent);
    }

    ui_draw_text(
        label == NULL ? "" : label,
        rect->x + 22,
        rect->y +
            (int32_t)(rect->height > 8 ?
                (rect->height - 8U) / 2U : 0),
        state_text(palette, state)
    );
}

void ui_control_draw_radio(
    const ui_rect_t *rect,
    const char *label,
    bool selected,
    const ui_palette_t *palette,
    ui_control_state_t state
)
{
    if (rect == NULL || palette == NULL)
    {
        return;
    }

    ui_rect_t outer = {
        .x = rect->x,
        .y = rect->y +
            (int32_t)(rect->height > 16 ?
                (rect->height - 16U) / 2U : 0),
        .width = 16,
        .height = 16
    };

    ui_fill_rect(&outer, palette->field);
    ui_draw_border(&outer, palette->border, 1);

    if (selected)
    {
        ui_rect_t inner = ui_inset(&outer, 5, 5);
        ui_fill_rect(&inner, palette->accent);
    }

    ui_draw_text(
        label == NULL ? "" : label,
        rect->x + 22,
        rect->y +
            (int32_t)(rect->height > 8 ?
                (rect->height - 8U) / 2U : 0),
        state_text(palette, state)
    );
}

void ui_control_draw_text_input(
    const ui_rect_t *rect,
    const char *text,
    const char *placeholder,
    size_t cursor,
    const ui_palette_t *palette,
    ui_control_state_t state
)
{
    if (rect == NULL || palette == NULL)
    {
        return;
    }

    ui_fill_rect(
        rect,
        state == UI_CONTROL_DISABLED ?
            palette->disabled : palette->field
    );

    ui_draw_border(
        rect,
        state == UI_CONTROL_FOCUSED ?
            palette->accent : palette->border,
        state == UI_CONTROL_FOCUSED ? 2U : 1U
    );

    const char *visible = text;
    uint32_t color = state_text(palette, state);

    if (visible == NULL || visible[0] == '\0')
    {
        visible = placeholder == NULL ? "" : placeholder;
        color = palette->border;
    }

    ui_draw_text_ellipsized(
        visible,
        rect,
        6,
        color
    );

    if (
        state == UI_CONTROL_FOCUSED &&
        text != NULL &&
        cursor <= 96U
    )
    {
        int32_t cursor_x =
            rect->x + 6 +
            (int32_t)cursor * 8;

        int32_t maximum_x =
            rect->x +
            (int32_t)rect->width - 4;

        if (cursor_x > maximum_x)
        {
            cursor_x = maximum_x;
        }

        ui_draw_vertical_line(
            cursor_x,
            rect->y + 5,
            rect->height > 10 ?
                rect->height - 10U : 1U,
            1,
            palette->accent
        );
    }
}

void ui_control_draw_slider(
    const ui_rect_t *rect,
    uint32_t value,
    uint32_t maximum,
    const ui_palette_t *palette,
    ui_control_state_t state
)
{
    if (rect == NULL || palette == NULL)
    {
        return;
    }

    ui_rect_t track = {
        .x = rect->x + 6,
        .y = rect->y + (int32_t)rect->height / 2 - 2,
        .width = rect->width > 12 ? rect->width - 12U : 1U,
        .height = 4
    };

    ui_fill_rect(&track, palette->border);

    if (maximum == 0)
    {
        maximum = 1;
    }

    if (value > maximum)
    {
        value = maximum;
    }

    uint32_t position =
        track.width > 12 ?
            (track.width - 12U) * value / maximum : 0;

    ui_rect_t thumb = {
        .x = track.x + (int32_t)position,
        .y = rect->y + 2,
        .width = 12,
        .height = rect->height > 4 ? rect->height - 4U : 1U
    };

    ui_fill_rect(
        &thumb,
        state == UI_CONTROL_DISABLED ?
            palette->disabled : palette->accent
    );

    ui_draw_border(&thumb, palette->border, 1);
}

uint32_t ui_control_slider_value(
    const ui_rect_t *rect,
    int32_t pointer_x,
    uint32_t maximum
)
{
    if (rect == NULL || rect->width <= 12U || maximum == 0)
    {
        return 0;
    }

    int32_t relative = pointer_x - rect->x - 6;
    int32_t length = (int32_t)rect->width - 24;

    if (relative <= 0)
    {
        return 0;
    }

    if (relative >= length)
    {
        return maximum;
    }

    return (uint32_t)relative * maximum /
        (uint32_t)length;
}

static ui_rect_t scrollbar_thumb(
    const ui_rect_t *rect,
    const ui_scrollbar_t *scrollbar
)
{
    if (
        rect == NULL ||
        scrollbar == NULL ||
        rect->height == 0
    )
    {
        return (ui_rect_t){ 0, 0, 0, 0 };
    }

    uint32_t total = scrollbar->maximum +
        scrollbar->page_size;

    if (total == 0)
    {
        total = 1;
    }

    uint32_t thumb_height =
        rect->height * scrollbar->page_size / total;

    if (thumb_height < UI_SCROLLBAR_MIN_THUMB)
    {
        thumb_height = UI_SCROLLBAR_MIN_THUMB;
    }

    if (thumb_height > rect->height)
    {
        thumb_height = rect->height;
    }

    uint32_t travel = rect->height - thumb_height;
    uint32_t position = 0;

    if (scrollbar->maximum != 0)
    {
        uint32_t value = scrollbar->value;

        if (value > scrollbar->maximum)
        {
            value = scrollbar->maximum;
        }

        position = travel * value /
            scrollbar->maximum;
    }

    ui_rect_t thumb = {
        .x = rect->x + 2,
        .y = rect->y + (int32_t)position,
        .width = rect->width > 4 ? rect->width - 4U : 1U,
        .height = thumb_height
    };

    return thumb;
}

void ui_control_draw_scrollbar_vertical(
    const ui_rect_t *rect,
    const ui_scrollbar_t *scrollbar,
    const ui_palette_t *palette,
    ui_control_state_t state
)
{
    if (rect == NULL || scrollbar == NULL || palette == NULL)
    {
        return;
    }

    ui_fill_rect(rect, palette->panel);
    ui_draw_border(rect, palette->border, 1);

    ui_rect_t thumb = scrollbar_thumb(rect, scrollbar);

    ui_fill_rect(
        &thumb,
        state == UI_CONTROL_PRESSED ?
            palette->selected :
            (state == UI_CONTROL_HOVERED ?
                palette->accent_hover : palette->accent)
    );

    ui_draw_border(&thumb, palette->border, 1);
}

uint32_t ui_control_scrollbar_value_from_pointer(
    const ui_rect_t *rect,
    const ui_scrollbar_t *scrollbar,
    int32_t pointer_y
)
{
    if (
        rect == NULL ||
        scrollbar == NULL ||
        scrollbar->maximum == 0
    )
    {
        return 0;
    }

    ui_rect_t thumb = scrollbar_thumb(rect, scrollbar);
    int32_t travel =
        (int32_t)rect->height -
        (int32_t)thumb.height;

    if (travel <= 0)
    {
        return 0;
    }

    int32_t relative =
        pointer_y - rect->y -
        (int32_t)thumb.height / 2;

    if (relative <= 0)
    {
        return 0;
    }

    if (relative >= travel)
    {
        return scrollbar->maximum;
    }

    return (uint32_t)relative *
        scrollbar->maximum /
        (uint32_t)travel;
}

void ui_control_draw_list_row(
    const ui_rect_t *rect,
    const char *prefix,
    const char *label,
    const ui_palette_t *palette,
    bool selected,
    bool hovered
)
{
    if (rect == NULL || palette == NULL)
    {
        return;
    }

    uint32_t background = palette->field;

    if (selected)
    {
        background = palette->selected;
    }
    else if (hovered)
    {
        background = palette->accent_hover;
    }

    ui_fill_rect(rect, background);

    if (prefix != NULL)
    {
        ui_draw_text(
            prefix,
            rect->x + 5,
            rect->y +
                (int32_t)(rect->height > 8 ?
                    (rect->height - 8U) / 2U : 0),
            palette->text
        );
    }

    ui_rect_t label_rect = *rect;
    label_rect.x += 40;

    if (label_rect.width > 40)
    {
        label_rect.width -= 40U;
    }
    else
    {
        label_rect.width = 0;
    }

    ui_draw_text_ellipsized(
        label == NULL ? "" : label,
        &label_rect,
        3,
        palette->text
    );
}

void ui_control_draw_tab(
    const ui_rect_t *rect,
    const char *label,
    const ui_palette_t *palette,
    bool active,
    bool hovered
)
{
    if (rect == NULL || palette == NULL)
    {
        return;
    }

    ui_fill_rect(
        rect,
        active ? palette->field :
            (hovered ? palette->accent_hover : palette->panel)
    );

    ui_draw_border(rect, palette->border, 1);

    if (active)
    {
        ui_draw_horizontal_line(
            rect->x,
            rect->y,
            rect->width,
            2,
            palette->accent
        );
    }

    ui_draw_text_centered(
        label == NULL ? "" : label,
        rect,
        palette->text
    );
}

void ui_control_draw_toolbar(
    const ui_rect_t *rect,
    const ui_palette_t *palette
)
{
    if (rect == NULL || palette == NULL)
    {
        return;
    }

    ui_fill_rect(rect, palette->panel);
    ui_draw_horizontal_line(
        rect->x,
        rect->y + (int32_t)rect->height - 1,
        rect->width,
        1,
        palette->border
    );
}

void ui_control_draw_statusbar(
    const ui_rect_t *rect,
    const char *left_text,
    const char *right_text,
    const ui_palette_t *palette
)
{
    if (rect == NULL || palette == NULL)
    {
        return;
    }

    ui_fill_rect(rect, palette->panel);
    ui_draw_horizontal_line(
        rect->x,
        rect->y,
        rect->width,
        1,
        palette->border
    );

    ui_draw_text(
        left_text == NULL ? "" : left_text,
        rect->x + 6,
        rect->y +
            (int32_t)(rect->height > 8 ?
                (rect->height - 8U) / 2U : 0),
        palette->text
    );

    if (right_text != NULL)
    {
        uint32_t width = ui_text_width(right_text);
        int32_t x =
            rect->x + (int32_t)rect->width -
            (int32_t)width - 6;

        ui_draw_text(
            right_text,
            x,
            rect->y +
                (int32_t)(rect->height > 8 ?
                    (rect->height - 8U) / 2U : 0),
            palette->text
        );
    }
}

static uint32_t menu_height(
    const ui_menu_item_t *items,
    uint32_t item_count
)
{
    uint32_t height = 8;

    for (uint32_t index = 0; index < item_count; index++)
    {
        height += items[index].separator ?
            UI_MENU_SEPARATOR_HEIGHT : UI_MENU_ITEM_HEIGHT;
    }

    return height;
}

void ui_menu_open(
    ui_menu_t *menu,
    int32_t x,
    int32_t y,
    uint32_t width,
    const ui_menu_item_t *items,
    uint32_t item_count,
    uint32_t screen_width,
    uint32_t screen_height
)
{
    if (
        menu == NULL ||
        items == NULL ||
        item_count == 0 ||
        width == 0
    )
    {
        return;
    }

    uint32_t height = menu_height(items, item_count);

    if (width > screen_width)
    {
        width = screen_width;
    }

    if (height > screen_height)
    {
        height = screen_height;
    }

    if (x < 0)
    {
        x = 0;
    }

    if (y < 0)
    {
        y = 0;
    }

    if ((uint64_t)x + width > screen_width)
    {
        x = (int32_t)(screen_width - width);
    }

    if ((uint64_t)y + height > screen_height)
    {
        y = (int32_t)(screen_height - height);
    }

    menu->bounds = (ui_rect_t){ x, y, width, height };
    menu->items = items;
    menu->item_count = item_count;
    menu->hovered_index = UINT32_MAX;
    menu->visible = true;
}

void ui_menu_close(ui_menu_t *menu)
{
    if (menu == NULL)
    {
        return;
    }

    menu->visible = false;
    menu->hovered_index = UINT32_MAX;
}

static bool menu_item_bounds(
    const ui_menu_t *menu,
    uint32_t target,
    ui_rect_t *bounds
)
{
    if (
        menu == NULL ||
        bounds == NULL ||
        target >= menu->item_count
    )
    {
        return false;
    }

    int32_t y = menu->bounds.y + 4;

    for (uint32_t index = 0; index < menu->item_count; index++)
    {
        uint32_t height = menu->items[index].separator ?
            UI_MENU_SEPARATOR_HEIGHT : UI_MENU_ITEM_HEIGHT;

        if (index == target)
        {
            *bounds = (ui_rect_t){
                .x = menu->bounds.x + 4,
                .y = y,
                .width = menu->bounds.width > 8 ?
                    menu->bounds.width - 8U : 1U,
                .height = height
            };

            return true;
        }

        y += (int32_t)height;
    }

    return false;
}

void ui_menu_update_hover(
    ui_menu_t *menu,
    int32_t pointer_x,
    int32_t pointer_y
)
{
    if (menu == NULL || !menu->visible)
    {
        return;
    }

    menu->hovered_index = UINT32_MAX;

    for (uint32_t index = 0; index < menu->item_count; index++)
    {
        ui_rect_t item;

        if (
            menu_item_bounds(menu, index, &item) &&
            !menu->items[index].separator &&
            ui_point_in_rect(pointer_x, pointer_y, &item)
        )
        {
            menu->hovered_index = index;
            return;
        }
    }
}

bool ui_menu_command_at(
    const ui_menu_t *menu,
    int32_t pointer_x,
    int32_t pointer_y,
    uint32_t *command
)
{
    if (
        menu == NULL ||
        !menu->visible ||
        command == NULL
    )
    {
        return false;
    }

    for (uint32_t index = 0; index < menu->item_count; index++)
    {
        ui_rect_t item;
        const ui_menu_item_t *entry = &menu->items[index];

        if (
            entry->separator ||
            !entry->enabled ||
            !menu_item_bounds(menu, index, &item) ||
            !ui_point_in_rect(pointer_x, pointer_y, &item)
        )
        {
            continue;
        }

        *command = entry->command;
        return true;
    }

    return false;
}

void ui_menu_render(
    const ui_menu_t *menu,
    const ui_palette_t *palette
)
{
    if (
        menu == NULL ||
        !menu->visible ||
        palette == NULL
    )
    {
        return;
    }

    ui_fill_rect(&menu->bounds, palette->panel);
    ui_draw_border(&menu->bounds, palette->border, 2);

    for (uint32_t index = 0; index < menu->item_count; index++)
    {
        ui_rect_t item;

        if (!menu_item_bounds(menu, index, &item))
        {
            continue;
        }

        const ui_menu_item_t *entry = &menu->items[index];

        if (entry->separator)
        {
            ui_draw_horizontal_line(
                item.x + 4,
                item.y + (int32_t)item.height / 2,
                item.width > 8 ? item.width - 8U : 1U,
                1,
                palette->border
            );

            continue;
        }

        if (index == menu->hovered_index && entry->enabled)
        {
            ui_fill_rect(&item, palette->selected);
        }

        if (entry->checked)
        {
            ui_draw_text(
                "*",
                item.x + 5,
                item.y + 8,
                palette->accent
            );
        }

        ui_draw_text_ellipsized(
            entry->label == NULL ? "" : entry->label,
            &item,
            20,
            entry->enabled ?
                palette->text : palette->disabled
        );
    }
}
