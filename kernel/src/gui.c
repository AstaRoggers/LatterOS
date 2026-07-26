#include "gui.h"

#include "app_suite.h"
#include "compositor.h"
#include "graphics.h"
#include "keyboard.h"
#include "mouse.h"
#include "power.h"
#include "rtc.h"
#include "shell.h"
#include "terminal.h"
#include "timer.h"
#include "ui.h"
#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GUI_EVENT_QUEUE_SIZE 64
#define GUI_WINDOW_COUNT 7

#define GUI_TERMINAL_LINES 20
#define GUI_TERMINAL_COLUMNS 68
#define GUI_TERMINAL_INPUT_CAPACITY 96

#define EXPLORER_VISIBLE_ROWS 10
#define EXPLORER_PREVIEW_LINES 4
#define EXPLORER_PREVIEW_COLUMNS 46

#define GUI_CURSOR_SIZE 16
#define TITLE_BAR_HEIGHT 24
#define TASKBAR_HEIGHT 34
#define LAUNCHER_ROW_HEIGHT 28
#define LAUNCHER_WIDTH 210
#define SYSTEM_MENU_WIDTH 190
#define SYSTEM_MENU_ROW_HEIGHT 30
#define SYSTEM_MENU_ROWS 4

#define COLOR_DESKTOP       0x1C4A72
#define COLOR_TASKBAR       0x172330
#define COLOR_TASKBAR_TOP   0x34495E
#define COLOR_WINDOW        0xE8EDF2
#define COLOR_WINDOW_BORDER 0x23384D
#define COLOR_TITLE_ACTIVE  0x245E9A
#define COLOR_TITLE_IDLE    0x536878
#define COLOR_TEXT          0x102030
#define COLOR_LIGHT_TEXT    0xFFFFFF
#define COLOR_FIELD         0xFFFFFF
#define COLOR_ACCENT        0x39A0ED
#define COLOR_CLOSE         0xC74848
#define COLOR_TERMINAL      0x101820
#define COLOR_TERMINAL_TEXT 0xD6F5D6
#define COLOR_ROW_HOVER     0xD4E5F2
#define COLOR_ROW_SELECTED  0xA9CDE8

typedef enum
{
    GUI_EVENT_NONE,
    GUI_EVENT_MOUSE_MOVE,
    GUI_EVENT_MOUSE_DOWN,
    GUI_EVENT_MOUSE_UP,
    GUI_EVENT_KEY,
    GUI_EVENT_SHORTCUT
} gui_event_type_t;

typedef enum
{
    GUI_SHORTCUT_NONE,
    GUI_SHORTCUT_FULLSCREEN,
    GUI_SHORTCUT_NEXT_WINDOW,
    GUI_SHORTCUT_PREVIOUS_WINDOW,
    GUI_SHORTCUT_CLOSE_WINDOW,
    GUI_SHORTCUT_TOGGLE_LAUNCHER,
    GUI_SHORTCUT_OPEN_TERMINAL,
    GUI_SHORTCUT_OPEN_FILES,
    GUI_SHORTCUT_SHOW_DESKTOP
} gui_shortcut_t;

typedef struct
{
    gui_event_type_t type;
    int32_t x;
    int32_t y;
    char character;
    gui_shortcut_t shortcut;
} gui_event_t;

typedef enum
{
    GUI_APP_TERMINAL,
    GUI_APP_FILE_EXPLORER,
    GUI_APP_TEXT_EDITOR,
    GUI_APP_CALCULATOR,
    GUI_APP_PAINT,
    GUI_APP_SETTINGS,
    GUI_APP_PROCESS_MANAGER
} gui_app_type_t;

typedef struct
{
    ui_rect_t bounds;
    ui_rect_t restore_bounds;
    const char *title;
    gui_app_type_t app;
    bool visible;
    bool minimized;
    bool maximized;
    bool fullscreen;
    bool fullscreen_restore_maximized;
    ui_rect_t fullscreen_restore_bounds;
    bool dragging;
    int32_t drag_offset_x;
    int32_t drag_offset_y;
} gui_window_t;

static bool initialized;
static bool active;
static bool start_requested;

static gui_event_t event_queue[GUI_EVENT_QUEUE_SIZE];
static volatile uint32_t event_read_index;
static volatile uint32_t event_write_index;

static gui_window_t windows[GUI_WINDOW_COUNT];
static uint8_t window_order[GUI_WINDOW_COUNT];

static char terminal_lines[
    GUI_TERMINAL_LINES
][GUI_TERMINAL_COLUMNS + 1];

static uint32_t terminal_line_count;
static uint32_t terminal_column;
static char terminal_input[GUI_TERMINAL_INPUT_CAPACITY];
static uint32_t terminal_input_length;
static bool terminal_focused;

static vfs_node_t *explorer_directory;
static vfs_node_t *explorer_selected;
static char explorer_preview[
    EXPLORER_PREVIEW_LINES
][EXPLORER_PREVIEW_COLUMNS + 1];

static int32_t cursor_x;
static int32_t cursor_y;
static int32_t last_mouse_x;
static int32_t last_mouse_y;
static uint64_t last_mouse_packets;
static bool last_left_button;

static uint32_t cursor_background[
    GUI_CURSOR_SIZE * GUI_CURSOR_SIZE
];

static bool cursor_visible;
static bool cursor_moved;
static bool launcher_open;
static bool system_menu_open;
static char clock_text[6];
static uint64_t last_clock_update;

static void stop_gui(void);

static void clear_text(
    char *text,
    size_t capacity
)
{
    if (text == NULL)
    {
        return;
    }

    for (size_t index = 0; index < capacity; index++)
    {
        text[index] = '\0';
    }
}

static void copy_text(
    char *destination,
    size_t capacity,
    const char *source
)
{
    if (
        destination == NULL ||
        capacity == 0
    )
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

static void hide_cursor(void)
{
    if (!cursor_visible)
    {
        return;
    }

    for (
        uint32_t row = 0;
        row < GUI_CURSOR_SIZE;
        row++
    )
    {
        for (
            uint32_t column = 0;
            column < GUI_CURSOR_SIZE;
            column++
        )
        {
            draw_pixel(
                (uint32_t)(cursor_x + (int32_t)column),
                (uint32_t)(cursor_y + (int32_t)row),
                cursor_background[
                    row * GUI_CURSOR_SIZE + column
                ]
            );
        }
    }

    graphics_present_rectangle(
        (uint32_t)cursor_x,
        (uint32_t)cursor_y,
        GUI_CURSOR_SIZE,
        GUI_CURSOR_SIZE
    );

    cursor_visible = false;
}

static void show_cursor(void)
{
    if (cursor_visible)
    {
        return;
    }

    for (
        uint32_t row = 0;
        row < GUI_CURSOR_SIZE;
        row++
    )
    {
        for (
            uint32_t column = 0;
            column < GUI_CURSOR_SIZE;
            column++
        )
        {
            cursor_background[
                row * GUI_CURSOR_SIZE + column
            ] = graphics_get_pixel(
                (uint32_t)(cursor_x + (int32_t)column),
                (uint32_t)(cursor_y + (int32_t)row)
            );
        }
    }

    ui_draw_cursor(cursor_x, cursor_y);

    graphics_present_rectangle(
        (uint32_t)cursor_x,
        (uint32_t)cursor_y,
        GUI_CURSOR_SIZE,
        GUI_CURSOR_SIZE
    );

    cursor_visible = true;
}

static void queue_event(gui_event_t event)
{
    if (
        event.type == GUI_EVENT_MOUSE_MOVE &&
        event_read_index != event_write_index
    )
    {
        uint32_t previous =
            (event_write_index + GUI_EVENT_QUEUE_SIZE - 1) %
            GUI_EVENT_QUEUE_SIZE;

        if (
            event_queue[previous].type ==
            GUI_EVENT_MOUSE_MOVE
        )
        {
            event_queue[previous] = event;
            return;
        }
    }

    uint32_t next =
        (event_write_index + 1) %
        GUI_EVENT_QUEUE_SIZE;

    if (next == event_read_index)
    {
        return;
    }

    event_queue[event_write_index] = event;
    event_write_index = next;
}

static bool pop_event(gui_event_t *event)
{
    if (
        event == NULL ||
        event_read_index == event_write_index
    )
    {
        return false;
    }

    *event = event_queue[event_read_index];

    event_read_index =
        (event_read_index + 1) %
        GUI_EVENT_QUEUE_SIZE;

    return true;
}

static void gui_keyboard_input(char character)
{
    gui_event_t event = {
        .type = GUI_EVENT_KEY,
        .x = 0,
        .y = 0,
        .character = character,
        .shortcut = GUI_SHORTCUT_NONE
    };

    queue_event(event);
}

static bool gui_keyboard_event(
    const keyboard_event_t *event
)
{
    if (
        event == NULL ||
        !event->pressed
    )
    {
        return false;
    }

    gui_shortcut_t shortcut =
        GUI_SHORTCUT_NONE;

    if (event->key == KEYBOARD_KEY_F11)
    {
        shortcut = GUI_SHORTCUT_FULLSCREEN;
    }
    else if (
        (event->control || event->alt) &&
        event->key == KEYBOARD_KEY_TAB
    )
    {
        shortcut = event->shift ?
            GUI_SHORTCUT_PREVIOUS_WINDOW :
            GUI_SHORTCUT_NEXT_WINDOW;
    }
    else if (
        (event->alt &&
            event->key == KEYBOARD_KEY_F4) ||
        (event->control &&
            event->key == KEYBOARD_KEY_Q)
    )
    {
        shortcut = GUI_SHORTCUT_CLOSE_WINDOW;
    }
    else if (
        event->control &&
        (event->key == KEYBOARD_KEY_ESCAPE ||
            event->key == KEYBOARD_KEY_SPACE)
    )
    {
        shortcut = GUI_SHORTCUT_TOGGLE_LAUNCHER;
    }
    else if (event->key == KEYBOARD_KEY_SUPER)
    {
        shortcut = GUI_SHORTCUT_TOGGLE_LAUNCHER;
    }
    else if (
        event->control &&
        (event->alt || event->shift) &&
        event->key == KEYBOARD_KEY_T
    )
    {
        shortcut = GUI_SHORTCUT_OPEN_TERMINAL;
    }
    else if (
        event->control &&
        (event->alt || event->shift) &&
        event->key == KEYBOARD_KEY_F
    )
    {
        shortcut = GUI_SHORTCUT_OPEN_FILES;
    }
    else if (
        event->control &&
        (event->alt || event->shift) &&
        event->key == KEYBOARD_KEY_D
    )
    {
        shortcut = GUI_SHORTCUT_SHOW_DESKTOP;
    }

    if (shortcut == GUI_SHORTCUT_NONE)
    {
        return false;
    }

    gui_event_t gui_event = {
        .type = GUI_EVENT_SHORTCUT,
        .x = 0,
        .y = 0,
        .character = 0,
        .shortcut = shortcut
    };

    queue_event(gui_event);
    return true;
}

static ui_rect_t window_title_bar(
    const gui_window_t *window
)
{
    ui_rect_t rectangle = {
        .x = window->bounds.x,
        .y = window->bounds.y,
        .width = window->bounds.width,
        .height = TITLE_BAR_HEIGHT
    };

    return rectangle;
}

static ui_rect_t window_close_button(
    const gui_window_t *window
)
{
    ui_rect_t rectangle = {
        .x = window->bounds.x +
            (int32_t)window->bounds.width - 22,
        .y = window->bounds.y + 4,
        .width = 16,
        .height = 16
    };

    return rectangle;
}

static ui_rect_t window_maximize_button(
    const gui_window_t *window
)
{
    ui_rect_t rectangle = {
        .x = window->bounds.x +
            (int32_t)window->bounds.width - 42,
        .y = window->bounds.y + 4,
        .width = 16,
        .height = 16
    };

    return rectangle;
}

static ui_rect_t window_minimize_button(
    const gui_window_t *window
)
{
    ui_rect_t rectangle = {
        .x = window->bounds.x +
            (int32_t)window->bounds.width - 62,
        .y = window->bounds.y + 4,
        .width = 16,
        .height = 16
    };

    return rectangle;
}

static ui_rect_t window_visual_bounds(
    const gui_window_t *window
)
{
    ui_rect_t rectangle = window->bounds;

    if (!window->fullscreen)
    {
        rectangle.width += 5;
        rectangle.height += 5;
    }

    return rectangle;
}

static ui_rect_t taskbar_bounds(void)
{
    ui_rect_t rectangle = {
        .x = 0,
        .y = (int32_t)graphics_height() -
            TASKBAR_HEIGHT,
        .width = graphics_width(),
        .height = TASKBAR_HEIGHT
    };

    return rectangle;
}

static ui_rect_t start_button_bounds(void)
{
    ui_rect_t taskbar = taskbar_bounds();

    ui_rect_t rectangle = {
        .x = 8,
        .y = taskbar.y + 5,
        .width = 78,
        .height = 24
    };

    return rectangle;
}

static ui_rect_t launcher_bounds(void)
{
    uint32_t height =
        34 + GUI_WINDOW_COUNT * LAUNCHER_ROW_HEIGHT;

    ui_rect_t taskbar = taskbar_bounds();

    ui_rect_t rectangle = {
        .x = 8,
        .y = taskbar.y - (int32_t)height,
        .width = LAUNCHER_WIDTH,
        .height = height
    };

    return rectangle;
}

static ui_rect_t launcher_row_bounds(uint8_t index)
{
    ui_rect_t launcher = launcher_bounds();

    ui_rect_t rectangle = {
        .x = launcher.x + 6,
        .y = launcher.y + 28 +
            (int32_t)index * LAUNCHER_ROW_HEIGHT,
        .width = launcher.width - 12,
        .height = LAUNCHER_ROW_HEIGHT - 3
    };

    return rectangle;
}

static ui_rect_t system_button_bounds(void)
{
    ui_rect_t taskbar = taskbar_bounds();

    ui_rect_t rectangle = {
        .x = (int32_t)graphics_width() - 94,
        .y = taskbar.y + 5,
        .width = 86,
        .height = 24
    };

    return rectangle;
}

static ui_rect_t system_menu_bounds(void)
{
    uint32_t height =
        34 + SYSTEM_MENU_ROWS *
            SYSTEM_MENU_ROW_HEIGHT;

    ui_rect_t taskbar = taskbar_bounds();

    ui_rect_t rectangle = {
        .x = (int32_t)graphics_width() -
            SYSTEM_MENU_WIDTH - 8,
        .y = taskbar.y - (int32_t)height,
        .width = SYSTEM_MENU_WIDTH,
        .height = height
    };

    return rectangle;
}

static ui_rect_t system_menu_row_bounds(
    uint8_t index
)
{
    ui_rect_t menu = system_menu_bounds();

    ui_rect_t rectangle = {
        .x = menu.x + 6,
        .y = menu.y + 28 +
            (int32_t)index *
                SYSTEM_MENU_ROW_HEIGHT,
        .width = menu.width - 12,
        .height = SYSTEM_MENU_ROW_HEIGHT - 3
    };

    return rectangle;
}

static ui_rect_t window_content_bounds(
    const gui_window_t *window
)
{
    if (window->fullscreen)
    {
        return window->bounds;
    }

    ui_rect_t rectangle = {
        .x = window->bounds.x + 2,
        .y = window->bounds.y + TITLE_BAR_HEIGHT,
        .width = window->bounds.width - 4,
        .height = window->bounds.height -
            TITLE_BAR_HEIGHT - 2
    };

    return rectangle;
}

static const char *taskbar_app_label(uint8_t index)
{
    static const char *labels[GUI_WINDOW_COUNT] = {
        "Terminal",
        "Files",
        "Editor",
        "Calc",
        "Paint",
        "Settings",
        "Tasks"
    };

    if (index >= GUI_WINDOW_COUNT)
    {
        return "App";
    }

    return labels[index];
}

static ui_rect_t taskbar_app_button(uint8_t index)
{
    ui_rect_t taskbar = taskbar_bounds();

    uint32_t available =
        graphics_width() > 300 ?
            graphics_width() - 300 :
            graphics_width();

    uint32_t gap = 4;
    uint32_t button_width =
        available > gap * (GUI_WINDOW_COUNT - 1) ?
            (available - gap *
                (GUI_WINDOW_COUNT - 1)) /
                GUI_WINDOW_COUNT : 48;

    if (button_width > 108)
    {
        button_width = 108;
    }

    if (button_width < 48)
    {
        button_width = 48;
    }

    ui_rect_t rectangle = {
        .x = 94 + (int32_t)index *
            (int32_t)(button_width + gap),
        .y = taskbar.y + 5,
        .width = button_width,
        .height = 24
    };

    return rectangle;
}

static void invalidate_taskbar(void)
{
    ui_rect_t rectangle = taskbar_bounds();
    compositor_invalidate(&rectangle);
}

static void invalidate_window(uint8_t index)
{
    if (
        index >= GUI_WINDOW_COUNT ||
        !windows[index].visible ||
        windows[index].minimized
    )
    {
        return;
    }

    ui_rect_t rectangle =
        window_visual_bounds(&windows[index]);

    compositor_invalidate(&rectangle);
}

static void invalidate_visible_windows(void)
{
    for (
        uint8_t index = 0;
        index < GUI_WINDOW_COUNT;
        index++
    )
    {
        invalidate_window(index);
    }
}

static void bring_window_to_front(uint8_t index)
{
    uint8_t position = 0;

    while (
        position < GUI_WINDOW_COUNT &&
        window_order[position] != index
    )
    {
        position++;
    }

    if (position >= GUI_WINDOW_COUNT)
    {
        return;
    }

    for (
        uint8_t current = position;
        current + 1 < GUI_WINDOW_COUNT;
        current++
    )
    {
        window_order[current] =
            window_order[current + 1];
    }

    window_order[GUI_WINDOW_COUNT - 1] = index;
}

static bool window_is_front(uint8_t index)
{
    if (
        index >= GUI_WINDOW_COUNT ||
        !windows[index].visible ||
        windows[index].minimized
    )
    {
        return false;
    }

    for (
        int32_t order = GUI_WINDOW_COUNT - 1;
        order >= 0;
        order--
    )
    {
        uint8_t candidate = window_order[order];

        if (
            windows[candidate].visible &&
            !windows[candidate].minimized
        )
        {
            return candidate == index;
        }
    }

    return false;
}

static uint8_t active_window_index(void)
{
    for (
        int32_t order = GUI_WINDOW_COUNT - 1;
        order >= 0;
        order--
    )
    {
        uint8_t candidate = window_order[order];

        if (
            windows[candidate].visible &&
            !windows[candidate].minimized
        )
        {
            return candidate;
        }
    }

    return GUI_WINDOW_COUNT;
}

static uint8_t active_fullscreen_window(void)
{
    uint8_t index = active_window_index();

    if (
        index < GUI_WINDOW_COUNT &&
        windows[index].fullscreen
    )
    {
        return index;
    }

    return GUI_WINDOW_COUNT;
}

static void minimize_window(uint8_t index)
{
    if (
        index >= GUI_WINDOW_COUNT ||
        !windows[index].visible ||
        windows[index].minimized
    )
    {
        return;
    }

    ui_rect_t old_rectangle =
        window_visual_bounds(&windows[index]);

    windows[index].minimized = true;
    windows[index].dragging = false;

    compositor_invalidate(&old_rectangle);
    invalidate_visible_windows();
    invalidate_taskbar();
}

static void toggle_maximize_window(uint8_t index)
{
    if (index >= GUI_WINDOW_COUNT)
    {
        return;
    }

    gui_window_t *window = &windows[index];

    if (
        !window->visible ||
        window->minimized ||
        window->fullscreen
    )
    {
        return;
    }

    ui_rect_t old_rectangle =
        window_visual_bounds(window);

    if (window->maximized)
    {
        window->bounds = window->restore_bounds;
        window->maximized = false;
    }
    else
    {
        window->restore_bounds = window->bounds;
        window->bounds.x = 0;
        window->bounds.y = 0;
        window->bounds.width = graphics_width();
        window->bounds.height =
            graphics_height() - TASKBAR_HEIGHT;
        window->maximized = true;
    }

    window->dragging = false;

    compositor_invalidate(&old_rectangle);

    ui_rect_t new_rectangle =
        window_visual_bounds(window);

    compositor_invalidate(&new_rectangle);
    invalidate_taskbar();
}

static void toggle_fullscreen_window(uint8_t index)
{
    if (index >= GUI_WINDOW_COUNT)
    {
        return;
    }

    gui_window_t *window = &windows[index];

    if (
        !window->visible ||
        window->minimized
    )
    {
        return;
    }

    if (window->fullscreen)
    {
        window->bounds =
            window->fullscreen_restore_bounds;

        window->maximized =
            window->fullscreen_restore_maximized;

        window->fullscreen = false;
    }
    else
    {
        window->fullscreen_restore_bounds =
            window->bounds;

        window->fullscreen_restore_maximized =
            window->maximized;

        window->bounds.x = 0;
        window->bounds.y = 0;
        window->bounds.width = graphics_width();
        window->bounds.height = graphics_height();
        window->fullscreen = true;
    }

    window->dragging = false;
    launcher_open = false;
    system_menu_open = false;
    bring_window_to_front(index);
    compositor_invalidate_all();
}

static void close_window(uint8_t index)
{
    if (
        index >= GUI_WINDOW_COUNT ||
        !windows[index].visible
    )
    {
        return;
    }

    bool was_fullscreen =
        windows[index].fullscreen;

    ui_rect_t old_rectangle =
        window_visual_bounds(&windows[index]);

    windows[index].visible = false;
    windows[index].minimized = false;
    windows[index].maximized = false;
    windows[index].fullscreen = false;
    windows[index].dragging = false;

    terminal_focused = false;

    if (was_fullscreen)
    {
        compositor_invalidate_all();
    }
    else
    {
        compositor_invalidate(&old_rectangle);
        invalidate_visible_windows();
        invalidate_taskbar();
    }
}

uint32_t gui_application_count(void)
{
    uint32_t count = 0;

    for (
        uint32_t index = 0;
        index < GUI_WINDOW_COUNT;
        index++
    )
    {
        if (
            windows[index].visible &&
            windows[index].app !=
                GUI_APP_PROCESS_MANAGER
        )
        {
            count++;
        }
    }

    return count;
}

bool gui_application_get(
    uint32_t index,
    gui_application_info_t *information
)
{
    if (information == NULL)
    {
        return false;
    }

    uint32_t current = 0;
    uint8_t active_index =
        active_window_index();

    for (
        uint32_t window_index = 0;
        window_index < GUI_WINDOW_COUNT;
        window_index++
    )
    {
        gui_window_t *window =
            &windows[window_index];

        if (
            !window->visible ||
            window->app ==
                GUI_APP_PROCESS_MANAGER
        )
        {
            continue;
        }

        if (current == index)
        {
            information->id = window_index;
            information->title = window->title;

            if (window->minimized)
            {
                information->state =
                    GUI_APPLICATION_MINIMIZED;
            }
            else if (window_index == active_index)
            {
                information->state =
                    GUI_APPLICATION_ACTIVE;
            }
            else
            {
                information->state =
                    GUI_APPLICATION_BACKGROUND;
            }

            return true;
        }

        current++;
    }

    return false;
}

bool gui_application_close(uint32_t id)
{
    if (
        id >= GUI_WINDOW_COUNT ||
        !windows[id].visible ||
        windows[id].app ==
            GUI_APP_PROCESS_MANAGER
    )
    {
        return false;
    }

    close_window((uint8_t)id);
    return true;
}

static void cycle_windows(bool reverse)
{
    uint8_t current = active_window_index();
    int32_t current_order = -1;

    for (
        uint8_t order = 0;
        order < GUI_WINDOW_COUNT;
        order++
    )
    {
        if (window_order[order] == current)
        {
            current_order = (int32_t)order;
            break;
        }
    }

    for (
        uint8_t step = 1;
        step <= GUI_WINDOW_COUNT;
        step++
    )
    {
        int32_t order;

        if (reverse)
        {
            order = current_order - (int32_t)step;

            while (order < 0)
            {
                order += GUI_WINDOW_COUNT;
            }
        }
        else
        {
            order = current_order + (int32_t)step;
            order %= GUI_WINDOW_COUNT;
        }

        uint8_t candidate =
            window_order[order];

        if (!windows[candidate].visible)
        {
            continue;
        }

        windows[candidate].minimized = false;
        bring_window_to_front(candidate);

        terminal_focused =
            windows[candidate].app ==
                GUI_APP_TERMINAL;

        launcher_open = false;
        compositor_invalidate_all();
        return;
    }
}

static void show_desktop(void)
{
    bool changed = false;

    for (
        uint8_t index = 0;
        index < GUI_WINDOW_COUNT;
        index++
    )
    {
        if (
            windows[index].visible &&
            !windows[index].minimized
        )
        {
            windows[index].minimized = true;
            windows[index].dragging = false;
            changed = true;
        }
    }

    terminal_focused = false;
    launcher_open = false;

    if (changed)
    {
        compositor_invalidate_all();
    }
}

static void clamp_window(gui_window_t *window)
{
    int32_t maximum_x =
        (int32_t)graphics_width() -
        (int32_t)window->bounds.width;

    int32_t maximum_y =
        (int32_t)graphics_height() -
        TASKBAR_HEIGHT -
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

static void gui_terminal_new_line(void)
{
    if (terminal_line_count == 0)
    {
        terminal_line_count = 1;
    }
    else if (terminal_line_count < GUI_TERMINAL_LINES)
    {
        terminal_line_count++;
    }
    else
    {
        for (
            uint32_t line = 1;
            line < GUI_TERMINAL_LINES;
            line++
        )
        {
            copy_text(
                terminal_lines[line - 1],
                sizeof(terminal_lines[line - 1]),
                terminal_lines[line]
            );
        }
    }

    clear_text(
        terminal_lines[terminal_line_count - 1],
        sizeof(terminal_lines[terminal_line_count - 1])
    );

    terminal_column = 0;
}

static void gui_terminal_reset_output(void)
{
    for (
        uint32_t line = 0;
        line < GUI_TERMINAL_LINES;
        line++
    )
    {
        clear_text(
            terminal_lines[line],
            sizeof(terminal_lines[line])
        );
    }

    terminal_line_count = 1;
    terminal_column = 0;
}

static void gui_terminal_append(const char *text)
{
    if (text == NULL)
    {
        return;
    }

    for (
        uint32_t index = 0;
        text[index] != '\0';
        index++
    )
    {
        char character = text[index];

        if (character == '\r')
        {
            continue;
        }

        if (character == '\n')
        {
            gui_terminal_new_line();
            continue;
        }

        if (terminal_column >= GUI_TERMINAL_COLUMNS)
        {
            gui_terminal_new_line();
        }

        terminal_lines[
            terminal_line_count - 1
        ][terminal_column] = character;

        terminal_column++;

        terminal_lines[
            terminal_line_count - 1
        ][terminal_column] = '\0';
    }
}

static void gui_terminal_redirect_output(
    const char *text
)
{
    gui_terminal_append(text);
}

static void gui_terminal_redirect_clear(void)
{
    gui_terminal_reset_output();
}

static void gui_terminal_execute(void)
{
    gui_terminal_append("> ");
    gui_terminal_append(terminal_input);
    gui_terminal_append("\n");

    terminal_set_redirect(
        gui_terminal_redirect_output,
        gui_terminal_redirect_clear
    );

    shell_execute(terminal_input);

    terminal_clear_redirect();

    clear_text(
        terminal_input,
        sizeof(terminal_input)
    );

    terminal_input_length = 0;
    invalidate_window(0);
}

static void explorer_clear_preview(void)
{
    for (
        uint32_t line = 0;
        line < EXPLORER_PREVIEW_LINES;
        line++
    )
    {
        clear_text(
            explorer_preview[line],
            sizeof(explorer_preview[line])
        );
    }
}

static void explorer_load_preview(vfs_node_t *node)
{
    explorer_clear_preview();

    if (
        node == NULL ||
        node->type != VFS_NODE_FILE
    )
    {
        return;
    }

    char buffer[
        EXPLORER_PREVIEW_LINES *
        EXPLORER_PREVIEW_COLUMNS + 1
    ];

    size_t count = vfs_read(
        node,
        0,
        buffer,
        sizeof(buffer) - 1
    );

    buffer[count] = '\0';

    uint32_t line = 0;
    uint32_t column = 0;

    for (
        size_t index = 0;
        index < count &&
        line < EXPLORER_PREVIEW_LINES;
        index++
    )
    {
        char character = buffer[index];

        if (character == '\r')
        {
            continue;
        }

        if (
            character == '\n' ||
            column >= EXPLORER_PREVIEW_COLUMNS
        )
        {
            explorer_preview[line][column] = '\0';
            line++;
            column = 0;

            if (
                character == '\n' ||
                line >= EXPLORER_PREVIEW_LINES
            )
            {
                continue;
            }
        }

        if (
            character < 32 ||
            character > 126
        )
        {
            character = '.';
        }

        explorer_preview[line][column] = character;
        column++;
        explorer_preview[line][column] = '\0';
    }
}

static void explorer_build_path(
    const vfs_node_t *node,
    char *buffer,
    size_t capacity
)
{
    if (
        buffer == NULL ||
        capacity == 0
    )
    {
        return;
    }

    buffer[0] = '\0';

    if (
        node == NULL ||
        node->parent == NULL
    )
    {
        copy_text(buffer, capacity, "/");
        return;
    }

    const vfs_node_t *parts[16];
    uint32_t count = 0;
    const vfs_node_t *current = node;

    while (
        current != NULL &&
        current->parent != NULL &&
        count < 16
    )
    {
        parts[count] = current;
        count++;
        current = current->parent;
    }

    size_t position = 0;

    if (position + 1 < capacity)
    {
        buffer[position] = '/';
        position++;
        buffer[position] = '\0';
    }

    while (count > 0)
    {
        count--;
        const char *name = parts[count]->name;

        for (
            uint32_t index = 0;
            name[index] != '\0' &&
            position + 1 < capacity;
            index++
        )
        {
            buffer[position] = name[index];
            position++;
        }

        if (
            count > 0 &&
            position + 1 < capacity
        )
        {
            buffer[position] = '/';
            position++;
        }

        buffer[position] = '\0';
    }
}

static vfs_node_t *explorer_entry_at(
    uint32_t row,
    bool *parent_entry
)
{
    if (parent_entry != NULL)
    {
        *parent_entry = false;
    }

    if (explorer_directory == NULL)
    {
        return NULL;
    }

    if (explorer_directory->parent != NULL)
    {
        if (row == 0)
        {
            if (parent_entry != NULL)
            {
                *parent_entry = true;
            }

            return explorer_directory->parent;
        }

        row--;
    }

    vfs_node_t *node =
        explorer_directory->first_child;

    while (
        node != NULL &&
        row > 0
    )
    {
        node = node->next_sibling;
        row--;
    }

    return node;
}

static ui_rect_t terminal_output_bounds(
    const gui_window_t *window
)
{
    ui_rect_t rectangle = {
        .x = window->bounds.x + 12,
        .y = window->bounds.y + 34,
        .width = window->bounds.width - 24,
        .height = window->bounds.height - 86
    };

    return rectangle;
}

static ui_rect_t terminal_input_bounds(
    const gui_window_t *window
)
{
    ui_rect_t rectangle = {
        .x = window->bounds.x + 12,
        .y = window->bounds.y +
            (int32_t)window->bounds.height - 42,
        .width = window->bounds.width - 24,
        .height = 30
    };

    return rectangle;
}

static ui_rect_t explorer_list_bounds(
    const gui_window_t *window
)
{
    ui_rect_t rectangle = {
        .x = window->bounds.x + 12,
        .y = window->bounds.y + 58,
        .width = window->bounds.width - 24,
        .height = EXPLORER_VISIBLE_ROWS * 18
    };

    return rectangle;
}

static ui_rect_t explorer_edit_button(
    const gui_window_t *window
)
{
    ui_rect_t rectangle = {
        .x = window->bounds.x +
            (int32_t)window->bounds.width - 138,
        .y = window->bounds.y +
            (int32_t)window->bounds.height - 28,
        .width = 124,
        .height = 20
    };

    return rectangle;
}

static void update_clock(bool force)
{
    uint32_t frequency = timer_frequency();
    uint64_t ticks = timer_ticks();

    if (
        !force &&
        frequency != 0 &&
        ticks - last_clock_update < frequency
    )
    {
        return;
    }

    rtc_datetime_t datetime;

    if (rtc_read(&datetime))
    {
        char next[6] = {
            (char)('0' + datetime.hour / 10),
            (char)('0' + datetime.hour % 10),
            ':',
            (char)('0' + datetime.minute / 10),
            (char)('0' + datetime.minute % 10),
            '\0'
        };

        bool changed = false;

        for (uint8_t index = 0; index < 6; index++)
        {
            if (clock_text[index] != next[index])
            {
                clock_text[index] = next[index];
                changed = true;
            }
        }

        if (changed && active)
        {
            invalidate_taskbar();
        }
    }

    last_clock_update = ticks;
}

static void render_desktop(void)
{
    const gui_theme_t *theme =
        app_suite_theme();

    graphics_clear(theme->desktop);

    ui_draw_text(
        "LatterOS Desktop",
        20,
        18,
        theme->light_text
    );

    ui_draw_text(
        "Click LatterOS for apps or System for power",
        20,
        38,
        theme->light_text
    );
}

static void render_taskbar(void)
{
    const gui_theme_t *theme =
        app_suite_theme();

    ui_rect_t taskbar = taskbar_bounds();
    ui_fill_rect(&taskbar, theme->taskbar);

    ui_rect_t top_line = {
        .x = 0,
        .y = taskbar.y,
        .width = taskbar.width,
        .height = 2
    };

    ui_fill_rect(&top_line, theme->taskbar_top);

    ui_rect_t start = start_button_bounds();

    ui_fill_rect(
        &start,
        launcher_open ?
            theme->title_active :
            theme->accent
    );

    ui_draw_border(&start, theme->window_border, 1);
    ui_draw_text_centered(
        "LatterOS",
        &start,
        theme->light_text
    );

    for (
        uint8_t index = 0;
        index < GUI_WINDOW_COUNT;
        index++
    )
    {
        ui_rect_t button = taskbar_app_button(index);

        uint32_t color;

        if (!windows[index].visible)
        {
            color = theme->taskbar;
        }
        else if (windows[index].minimized)
        {
            color = theme->window_border;
        }
        else if (window_is_front(index))
        {
            color = theme->accent;
        }
        else
        {
            color = theme->title_idle;
        }

        ui_fill_rect(&button, color);
        ui_draw_border(
            &button,
            theme->window_border,
            1
        );

        ui_draw_text_centered(
            taskbar_app_label(index),
            &button,
            theme->light_text
        );
    }

    ui_draw_text(
        clock_text,
        (int32_t)graphics_width() - 150,
        taskbar.y + 13,
        theme->light_text
    );

    ui_rect_t system = system_button_bounds();

    ui_fill_rect(
        &system,
        system_menu_open ?
            theme->title_active :
            theme->title_idle
    );

    ui_draw_border(
        &system,
        theme->window_border,
        1
    );

    ui_draw_text_centered(
        "System",
        &system,
        theme->light_text
    );
}

static void render_launcher(void)
{
    if (!launcher_open)
    {
        return;
    }

    const gui_theme_t *theme =
        app_suite_theme();

    ui_rect_t launcher = launcher_bounds();
    ui_fill_rect(&launcher, theme->window);
    ui_draw_border(
        &launcher,
        theme->window_border,
        2
    );

    ui_draw_text(
        "Applications",
        launcher.x + 10,
        launcher.y + 10,
        theme->text
    );

    for (
        uint8_t index = 0;
        index < GUI_WINDOW_COUNT;
        index++
    )
    {
        ui_rect_t row = launcher_row_bounds(index);

        uint32_t color =
            windows[index].visible &&
            !windows[index].minimized ?
                theme->row_selected :
                theme->field;

        ui_fill_rect(&row, color);
        ui_draw_border(
            &row,
            theme->window_border,
            1
        );

        ui_draw_text(
            taskbar_app_label(index),
            row.x + 8,
            row.y + 9,
            theme->text
        );
    }
}

static void render_system_menu(void)
{
    if (!system_menu_open)
    {
        return;
    }

    const gui_theme_t *theme =
        app_suite_theme();

    static const char *labels[
        SYSTEM_MENU_ROWS
    ] = {
        "Terminal mode",
        "Restart",
        "Shut down",
        "Cancel"
    };

    ui_rect_t menu = system_menu_bounds();
    ui_fill_rect(&menu, theme->window);
    ui_draw_border(
        &menu,
        theme->window_border,
        2
    );

    ui_draw_text(
        "System",
        menu.x + 10,
        menu.y + 10,
        theme->text
    );

    for (
        uint8_t index = 0;
        index < SYSTEM_MENU_ROWS;
        index++
    )
    {
        ui_rect_t row =
            system_menu_row_bounds(index);

        ui_fill_rect(&row, theme->field);
        ui_draw_border(
            &row,
            theme->window_border,
            1
        );

        ui_draw_text(
            labels[index],
            row.x + 8,
            row.y + 10,
            theme->text
        );
    }
}

static void render_window_frame(
    uint8_t index,
    const gui_window_t *window
)
{
    if (window->fullscreen)
    {
        return;
    }

    const gui_theme_t *theme =
        app_suite_theme();

    ui_rect_t shadow = {
        .x = window->bounds.x + 5,
        .y = window->bounds.y + 5,
        .width = window->bounds.width,
        .height = window->bounds.height
    };

    ui_fill_rect(&shadow, 0x10253A);
    ui_fill_rect(&window->bounds, theme->window);
    ui_draw_border(
        &window->bounds,
        theme->window_border,
        2
    );

    ui_rect_t title = window_title_bar(window);

    ui_fill_rect(
        &title,
        window_is_front(index) ?
            theme->title_active :
            theme->title_idle
    );

    ui_draw_text(
        window->title,
        title.x + 8,
        title.y + 8,
        theme->light_text
    );

    ui_rect_t minimize =
        window_minimize_button(window);

    ui_fill_rect(&minimize, theme->title_idle);

    ui_rect_t minimize_line = {
        .x = minimize.x + 4,
        .y = minimize.y + 11,
        .width = 8,
        .height = 2
    };

    ui_fill_rect(&minimize_line, theme->light_text);

    ui_rect_t maximize =
        window_maximize_button(window);

    ui_fill_rect(&maximize, theme->title_idle);

    ui_rect_t maximize_box = {
        .x = maximize.x + 4,
        .y = maximize.y + 4,
        .width = 8,
        .height = 8
    };

    ui_draw_border(
        &maximize_box,
        theme->light_text,
        1
    );

    ui_rect_t close = window_close_button(window);
    ui_fill_rect(&close, theme->close);
    ui_draw_text_centered(
        "X",
        &close,
        theme->light_text
    );
}

static void render_terminal_app(
    const gui_window_t *window
)
{
    ui_rect_t output =
        terminal_output_bounds(window);

    ui_fill_rect(&output, COLOR_TERMINAL);
    ui_draw_border(&output, 0x334655, 2);

    uint32_t maximum_lines =
        (output.height - 12) / 12;

    uint32_t first_line = 0;

    if (terminal_line_count > maximum_lines)
    {
        first_line =
            terminal_line_count - maximum_lines;
    }

    uint32_t visual_line = 0;

    for (
        uint32_t line = first_line;
        line < terminal_line_count;
        line++
    )
    {
        ui_draw_text(
            terminal_lines[line],
            output.x + 6,
            output.y + 6 +
                (int32_t)visual_line * 12,
            COLOR_TERMINAL_TEXT
        );

        visual_line++;
    }

    ui_rect_t input =
        terminal_input_bounds(window);

    ui_fill_rect(&input, COLOR_FIELD);
    ui_draw_border(
        &input,
        terminal_focused ?
            COLOR_ACCENT : 0x667788,
        2
    );

    ui_draw_text(
        ">",
        input.x + 7,
        input.y + 11,
        COLOR_TEXT
    );

    ui_draw_text(
        terminal_input,
        input.x + 23,
        input.y + 11,
        COLOR_TEXT
    );
}

static void render_explorer_app(
    const gui_window_t *window
)
{
    const gui_theme_t *theme =
        app_suite_theme();

    char path[VFS_PATH_MAX];

    explorer_build_path(
        explorer_directory,
        path,
        sizeof(path)
    );

    ui_draw_text(
        "Path:",
        window->bounds.x + 14,
        window->bounds.y + 38,
        theme->text
    );

    ui_draw_text(
        path,
        window->bounds.x + 54,
        window->bounds.y + 38,
        theme->text
    );

    ui_rect_t list = explorer_list_bounds(window);
    ui_fill_rect(&list, theme->field);
    ui_draw_border(
        &list,
        theme->window_border,
        1
    );

    for (
        uint32_t row = 0;
        row < EXPLORER_VISIBLE_ROWS;
        row++
    )
    {
        bool parent_entry;
        vfs_node_t *node =
            explorer_entry_at(row, &parent_entry);

        if (node == NULL)
        {
            break;
        }

        ui_rect_t row_rectangle = {
            .x = list.x + 2,
            .y = list.y + 2 +
                (int32_t)row * 18,
            .width = list.width - 4,
            .height = 18
        };

        if (
            !parent_entry &&
            node == explorer_selected
        )
        {
            ui_fill_rect(
                &row_rectangle,
                theme->row_selected
            );
        }

        ui_draw_text(
            parent_entry ?
                "[UP]" :
                (node->type == VFS_NODE_DIRECTORY ?
                    "[D]" : "[F]"),
            row_rectangle.x + 4,
            row_rectangle.y + 5,
            theme->text
        );

        ui_draw_text(
            parent_entry ? ".." : node->name,
            row_rectangle.x + 42,
            row_rectangle.y + 5,
            theme->text
        );
    }

    int32_t preview_y =
        list.y + (int32_t)list.height + 12;

    ui_draw_text(
        explorer_selected != NULL ?
            explorer_selected->name :
            "File preview",
        window->bounds.x + 14,
        preview_y,
        theme->text
    );

    ui_rect_t preview = {
        .x = window->bounds.x + 12,
        .y = preview_y + 16,
        .width = window->bounds.width - 24,
        .height = 56
    };

    ui_fill_rect(&preview, theme->field);
    ui_draw_border(
        &preview,
        theme->window_border,
        1
    );

    if (explorer_selected == NULL)
    {
        ui_draw_text(
            "Select a file to preview it.",
            preview.x + 6,
            preview.y + 8,
            theme->title_idle
        );
    }
    else
    {
        for (
            uint32_t line = 0;
            line < EXPLORER_PREVIEW_LINES;
            line++
        )
        {
            ui_draw_text(
                explorer_preview[line],
                preview.x + 6,
                preview.y + 7 +
                    (int32_t)line * 12,
                theme->text
            );
        }

        ui_rect_t edit =
            explorer_edit_button(window);

        ui_fill_rect(&edit, theme->accent);
        ui_draw_border(
            &edit,
            theme->window_border,
            1
        );

        ui_draw_text_centered(
            "Open in Editor",
            &edit,
            theme->light_text
        );
    }
}

static void render_window(
    uint8_t index,
    const gui_window_t *window
)
{
    if (
        !window->visible ||
        window->minimized
    )
    {
        return;
    }

    render_window_frame(index, window);

    if (window->app == GUI_APP_TERMINAL)
    {
        render_terminal_app(window);
        return;
    }

    if (window->app == GUI_APP_FILE_EXPLORER)
    {
        render_explorer_app(window);
        return;
    }

    ui_rect_t content =
        window_content_bounds(window);

    switch (window->app)
    {
        case GUI_APP_TEXT_EDITOR:
            editor_render(&content);
            break;

        case GUI_APP_CALCULATOR:
            calculator_render(&content);
            break;

        case GUI_APP_PAINT:
            paint_render(&content);
            break;

        case GUI_APP_SETTINGS:
            settings_render(&content);
            break;

        case GUI_APP_PROCESS_MANAGER:
            process_manager_render(&content);
            break;

        default:
            break;
    }
}

static void render_gui_scene(void)
{
    uint8_t fullscreen =
        active_fullscreen_window();

    if (fullscreen < GUI_WINDOW_COUNT)
    {
        const gui_theme_t *theme =
            app_suite_theme();

        graphics_clear(theme->window);
        render_window(
            fullscreen,
            &windows[fullscreen]
        );
        return;
    }

    render_desktop();

    for (
        uint8_t order = 0;
        order < GUI_WINDOW_COUNT;
        order++
    )
    {
        uint8_t index = window_order[order];
        render_window(index, &windows[index]);
    }

    render_taskbar();
    render_launcher();
    render_system_menu();
}

static void stop_gui(void)
{
    hide_cursor();

    active = false;
    start_requested = false;

    terminal_clear_redirect();
    graphics_set_deferred(false);

    keyboard_set_character_handler(
        terminal_put_character
    );

    keyboard_set_event_handler(NULL);

    terminal_clear();
    terminal_init();
}

static void activate_window(uint8_t index)
{
    if (index >= GUI_WINDOW_COUNT)
    {
        return;
    }

    invalidate_visible_windows();
    invalidate_taskbar();

    windows[index].visible = true;
    windows[index].minimized = false;
    bring_window_to_front(index);

    terminal_focused =
        windows[index].app == GUI_APP_TERMINAL;

    if (
        windows[index].app ==
        GUI_APP_PROCESS_MANAGER
    )
    {
        process_manager_refresh();
    }

    invalidate_visible_windows();
    invalidate_taskbar();
}

static bool handle_taskbar_click(
    int32_t x,
    int32_t y
)
{
    ui_rect_t start = start_button_bounds();

    if (ui_point_in_rect(x, y, &start))
    {
        ui_rect_t launcher = launcher_bounds();
        launcher_open = !launcher_open;
        system_menu_open = false;
        compositor_invalidate(&launcher);
        invalidate_taskbar();
        return true;
    }

    ui_rect_t system = system_button_bounds();

    if (ui_point_in_rect(x, y, &system))
    {
        system_menu_open = !system_menu_open;
        launcher_open = false;
        compositor_invalidate_all();
        return true;
    }

    for (
        uint8_t index = 0;
        index < GUI_WINDOW_COUNT;
        index++
    )
    {
        ui_rect_t button =
            taskbar_app_button(index);

        if (!ui_point_in_rect(x, y, &button))
        {
            continue;
        }

        launcher_open = false;
        system_menu_open = false;

        if (
            windows[index].visible &&
            !windows[index].minimized &&
            window_is_front(index)
        )
        {
            minimize_window(index);
        }
        else
        {
            activate_window(index);
        }

        return true;
    }

    return false;
}

static void handle_explorer_click(
    const gui_window_t *window,
    int32_t x,
    int32_t y
)
{
    if (explorer_selected != NULL)
    {
        ui_rect_t edit =
            explorer_edit_button(window);

        if (ui_point_in_rect(x, y, &edit))
        {
            editor_open_node(explorer_selected);
            activate_window(2);
            return;
        }
    }

    ui_rect_t list = explorer_list_bounds(window);

    if (!ui_point_in_rect(x, y, &list))
    {
        return;
    }

    int32_t relative_y = y - list.y - 2;

    if (relative_y < 0)
    {
        return;
    }

    uint32_t row =
        (uint32_t)relative_y / 18;

    if (row >= EXPLORER_VISIBLE_ROWS)
    {
        return;
    }

    bool parent_entry;
    vfs_node_t *node =
        explorer_entry_at(row, &parent_entry);

    if (node == NULL)
    {
        return;
    }

    if (
        parent_entry ||
        node->type == VFS_NODE_DIRECTORY
    )
    {
        explorer_directory = node;
        explorer_selected = NULL;
        explorer_clear_preview();
    }
    else
    {
        explorer_selected = node;
        explorer_load_preview(node);
    }

    invalidate_window(1);
}

static void handle_mouse_down(
    int32_t x,
    int32_t y
)
{
    if (system_menu_open)
    {
        ui_rect_t menu = system_menu_bounds();
        ui_rect_t button = system_button_bounds();

        if (ui_point_in_rect(x, y, &menu))
        {
            for (
                uint8_t index = 0;
                index < SYSTEM_MENU_ROWS;
                index++
            )
            {
                ui_rect_t row =
                    system_menu_row_bounds(index);

                if (!ui_point_in_rect(x, y, &row))
                {
                    continue;
                }

                system_menu_open = false;

                if (index == 0)
                {
                    stop_gui();
                    return;
                }

                if (index == 1)
                {
                    power_reboot();
                    return;
                }

                if (index == 2)
                {
                    power_shutdown();
                    return;
                }

                compositor_invalidate_all();
                return;
            }

            return;
        }

        if (!ui_point_in_rect(x, y, &button))
        {
            system_menu_open = false;
            compositor_invalidate_all();
        }
    }

    if (launcher_open)
    {
        ui_rect_t launcher = launcher_bounds();
        ui_rect_t start = start_button_bounds();

        if (ui_point_in_rect(x, y, &launcher))
        {
            for (
                uint8_t index = 0;
                index < GUI_WINDOW_COUNT;
                index++
            )
            {
                ui_rect_t row =
                    launcher_row_bounds(index);

                if (ui_point_in_rect(x, y, &row))
                {
                    launcher_open = false;
                    system_menu_open = false;
                    compositor_invalidate(&launcher);
                    activate_window(index);
                    return;
                }
            }

            return;
        }

        if (!ui_point_in_rect(x, y, &start))
        {
            launcher_open = false;
            compositor_invalidate(&launcher);
            invalidate_taskbar();
        }
    }

    for (
        int32_t order = GUI_WINDOW_COUNT - 1;
        order >= 0;
        order--
    )
    {
        uint8_t index = window_order[order];
        gui_window_t *window = &windows[index];

        if (
            !window->visible ||
            window->minimized ||
            !ui_point_in_rect(
                x,
                y,
                &window->bounds
            )
        )
        {
            continue;
        }

        activate_window(index);

        if (!window->fullscreen)
        {
            ui_rect_t minimize =
                window_minimize_button(window);

            if (ui_point_in_rect(x, y, &minimize))
            {
                minimize_window(index);
                return;
            }

            ui_rect_t maximize =
                window_maximize_button(window);

            if (ui_point_in_rect(x, y, &maximize))
            {
                toggle_maximize_window(index);
                return;
            }

            ui_rect_t close =
                window_close_button(window);

            if (ui_point_in_rect(x, y, &close))
            {
                close_window(index);
                return;
            }

            ui_rect_t title =
                window_title_bar(window);

            if (
                ui_point_in_rect(x, y, &title) &&
                !window->maximized
            )
            {
                window->dragging = true;
                window->drag_offset_x =
                    x - window->bounds.x;
                window->drag_offset_y =
                    y - window->bounds.y;
                return;
            }
        }

        terminal_focused = false;

        if (window->app == GUI_APP_TERMINAL)
        {
            ui_rect_t input =
                terminal_input_bounds(window);

            terminal_focused =
                ui_point_in_rect(x, y, &input) ||
                ui_point_in_rect(
                    x,
                    y,
                    &window->bounds
                );

            compositor_invalidate(&input);
            return;
        }

        if (window->app == GUI_APP_FILE_EXPLORER)
        {
            handle_explorer_click(
                window,
                x,
                y
            );

            return;
        }

        ui_rect_t content =
            window_content_bounds(window);

        bool changed = false;

        switch (window->app)
        {
            case GUI_APP_TEXT_EDITOR:
                changed = editor_handle_click(
                    &content,
                    x,
                    y
                );
                break;

            case GUI_APP_CALCULATOR:
                changed = calculator_handle_click(
                    &content,
                    x,
                    y
                );
                break;

            case GUI_APP_PAINT:
            {
                ui_rect_t damage = { 0, 0, 0, 0 };

                changed = paint_handle_mouse_down(
                    &content,
                    x,
                    y,
                    &damage
                );

                if (
                    changed &&
                    damage.width != 0 &&
                    damage.height != 0
                )
                {
                    compositor_invalidate(&damage);
                    return;
                }

                break;
            }

            case GUI_APP_SETTINGS:
                changed = settings_handle_click(
                    &content,
                    x,
                    y
                );

                if (changed)
                {
                    compositor_invalidate_all();
                    return;
                }
                break;

            case GUI_APP_PROCESS_MANAGER:
                changed = process_manager_handle_click(
                    &content,
                    x,
                    y
                );
                break;

            default:
                break;
        }

        if (changed)
        {
            invalidate_window(index);
        }

        return;
    }

    ui_rect_t taskbar = taskbar_bounds();

    if (ui_point_in_rect(x, y, &taskbar))
    {
        (void)handle_taskbar_click(x, y);
        return;
    }

    terminal_focused = false;
}

static void handle_mouse_move(
    int32_t x,
    int32_t y
)
{
    bool moved_window = false;

    for (
        uint8_t index = 0;
        index < GUI_WINDOW_COUNT;
        index++
    )
    {
        gui_window_t *window = &windows[index];

        if (!window->dragging)
        {
            continue;
        }

        moved_window = true;

        ui_rect_t old_rectangle =
            window_visual_bounds(window);

        window->bounds.x =
            x - window->drag_offset_x;

        window->bounds.y =
            y - window->drag_offset_y;

        clamp_window(window);

        ui_rect_t new_rectangle =
            window_visual_bounds(window);

        compositor_invalidate(&old_rectangle);
        compositor_invalidate(&new_rectangle);
    }

    if (moved_window)
    {
        return;
    }

    for (
        uint8_t index = 0;
        index < GUI_WINDOW_COUNT;
        index++
    )
    {
        gui_window_t *window = &windows[index];

        if (
            !window->visible ||
            window->minimized ||
            window->app != GUI_APP_PAINT
        )
        {
            continue;
        }

        ui_rect_t content =
            window_content_bounds(window);

        ui_rect_t damage = { 0, 0, 0, 0 };

        if (paint_handle_mouse_move(
            &content,
            x,
            y,
            &damage
        ))
        {
            if (
                damage.width != 0 &&
                damage.height != 0
            )
            {
                compositor_invalidate(&damage);
            }
            else
            {
                invalidate_window(index);
            }
        }
    }
}

static void handle_mouse_up(void)
{
    for (
        uint8_t index = 0;
        index < GUI_WINDOW_COUNT;
        index++
    )
    {
        windows[index].dragging = false;
    }

    paint_handle_mouse_up();
}

static void handle_key(char character)
{
    if (character == 27)
    {
        uint8_t fullscreen =
            active_fullscreen_window();

        if (fullscreen < GUI_WINDOW_COUNT)
        {
            toggle_fullscreen_window(fullscreen);
            return;
        }

        if (
            launcher_open ||
            system_menu_open
        )
        {
            launcher_open = false;
            system_menu_open = false;
            compositor_invalidate_all();
        }

        return;
    }

    uint8_t index = GUI_WINDOW_COUNT;

    for (
        int32_t order = GUI_WINDOW_COUNT - 1;
        order >= 0;
        order--
    )
    {
        uint8_t candidate = window_order[order];

        if (
            windows[candidate].visible &&
            !windows[candidate].minimized
        )
        {
            index = candidate;
            break;
        }
    }

    if (index >= GUI_WINDOW_COUNT)
    {
        return;
    }

    gui_window_t *window = &windows[index];

    if (window->app == GUI_APP_TERMINAL)
    {
        if (!terminal_focused)
        {
            return;
        }

        if (character == '\b')
        {
            if (terminal_input_length > 0)
            {
                terminal_input_length--;
                terminal_input[
                    terminal_input_length
                ] = '\0';
            }

            invalidate_window(index);
            return;
        }

        if (character == '\n')
        {
            gui_terminal_execute();
            return;
        }

        if (
            character >= 32 &&
            character <= 126 &&
            terminal_input_length <
                GUI_TERMINAL_INPUT_CAPACITY - 1
        )
        {
            terminal_input[terminal_input_length] =
                character;

            terminal_input_length++;
            terminal_input[terminal_input_length] =
                '\0';

            invalidate_window(index);
        }

        return;
    }

    bool changed = false;

    if (window->app == GUI_APP_TEXT_EDITOR)
    {
        changed = editor_handle_key(character);
    }
    else if (window->app == GUI_APP_CALCULATOR)
    {
        changed = calculator_handle_key(character);
    }

    if (changed)
    {
        invalidate_window(index);
    }
}

static void handle_shortcut(
    gui_shortcut_t shortcut
)
{
    uint8_t index = active_window_index();

    switch (shortcut)
    {
        case GUI_SHORTCUT_FULLSCREEN:
            toggle_fullscreen_window(index);
            break;

        case GUI_SHORTCUT_NEXT_WINDOW:
            cycle_windows(false);
            break;

        case GUI_SHORTCUT_PREVIOUS_WINDOW:
            cycle_windows(true);
            break;

        case GUI_SHORTCUT_CLOSE_WINDOW:
            close_window(index);
            break;

        case GUI_SHORTCUT_TOGGLE_LAUNCHER:
        {
            uint8_t fullscreen =
                active_fullscreen_window();

            if (fullscreen < GUI_WINDOW_COUNT)
            {
                toggle_fullscreen_window(fullscreen);
            }

            launcher_open = !launcher_open;
            system_menu_open = false;
            compositor_invalidate_all();
            break;
        }

        case GUI_SHORTCUT_OPEN_TERMINAL:
            activate_window(0);
            break;

        case GUI_SHORTCUT_OPEN_FILES:
            activate_window(1);
            break;

        case GUI_SHORTCUT_SHOW_DESKTOP:
            show_desktop();
            break;

        default:
            break;
    }
}

static void process_events(void)
{
    gui_event_t event;

    while (active && pop_event(&event))
    {
        switch (event.type)
        {
            case GUI_EVENT_MOUSE_MOVE:
                handle_mouse_move(
                    event.x,
                    event.y
                );
                break;

            case GUI_EVENT_MOUSE_DOWN:
                handle_mouse_down(
                    event.x,
                    event.y
                );
                break;

            case GUI_EVENT_MOUSE_UP:
                handle_mouse_up();
                break;

            case GUI_EVENT_KEY:
                handle_key(event.character);
                break;

            case GUI_EVENT_SHORTCUT:
                handle_shortcut(event.shortcut);
                break;

            default:
                break;
        }
    }
}

static void update_mouse_events(void)
{
    mouse_state_t state;
    mouse_get_state(&state);

    if (state.packet_count == last_mouse_packets)
    {
        return;
    }

    int32_t delta_x = state.x - last_mouse_x;
    int32_t delta_y = state.y - last_mouse_y;

    hide_cursor();

    last_mouse_x = state.x;
    last_mouse_y = state.y;
    last_mouse_packets = state.packet_count;

    cursor_x += delta_x;
    cursor_y += delta_y;

    int32_t maximum_x =
        (int32_t)graphics_width() - GUI_CURSOR_SIZE;

    int32_t maximum_y =
        (int32_t)graphics_height() - GUI_CURSOR_SIZE;

    if (cursor_x < 0)
    {
        cursor_x = 0;
    }

    if (cursor_y < 0)
    {
        cursor_y = 0;
    }

    if (cursor_x > maximum_x)
    {
        cursor_x = maximum_x;
    }

    if (cursor_y > maximum_y)
    {
        cursor_y = maximum_y;
    }

    gui_event_t move = {
        .type = GUI_EVENT_MOUSE_MOVE,
        .x = cursor_x,
        .y = cursor_y,
        .character = 0
    };

    queue_event(move);

    if (
        state.left_button &&
        !last_left_button
    )
    {
        gui_event_t down = {
            .type = GUI_EVENT_MOUSE_DOWN,
            .x = cursor_x,
            .y = cursor_y,
            .character = 0
        };

        queue_event(down);
    }

    if (
        !state.left_button &&
        last_left_button
    )
    {
        gui_event_t up = {
            .type = GUI_EVENT_MOUSE_UP,
            .x = cursor_x,
            .y = cursor_y,
            .character = 0
        };

        queue_event(up);
    }

    last_left_button = state.left_button;
    cursor_moved = true;
}

static void start_gui(void)
{
    active = true;
    start_requested = false;

    graphics_set_deferred(true);
    compositor_init(render_gui_scene);

    event_read_index = 0;
    event_write_index = 0;

    uint32_t screen_width = graphics_width();
    uint32_t screen_height = graphics_height();

    cursor_x = (int32_t)(screen_width / 2);
    cursor_y = (int32_t)(screen_height / 2);
    cursor_visible = false;
    cursor_moved = true;

    mouse_state_t state;
    mouse_get_state(&state);

    last_mouse_x = state.x;
    last_mouse_y = state.y;
    last_mouse_packets = state.packet_count;
    last_left_button = state.left_button;

    windows[0].bounds.x =
        (int32_t)(screen_width / 2) - 390;
    windows[0].bounds.y = 80;
    windows[0].bounds.width = 620;
    windows[0].bounds.height = 370;
    windows[0].title = "LatterOS Terminal";
    windows[0].app = GUI_APP_TERMINAL;
    windows[0].visible = false;
    windows[0].dragging = false;

    windows[1].bounds.x =
        (int32_t)(screen_width / 2) + 80;
    windows[1].bounds.y = 135;
    windows[1].bounds.width = 430;
    windows[1].bounds.height = 360;
    windows[1].title = "File Explorer";
    windows[1].app = GUI_APP_FILE_EXPLORER;
    windows[1].visible = false;
    windows[1].dragging = false;

    windows[2].bounds.x =
        (int32_t)(screen_width / 2) - 280;
    windows[2].bounds.y = 70;
    windows[2].bounds.width = 560;
    windows[2].bounds.height = 400;
    windows[2].title = "Text Editor";
    windows[2].app = GUI_APP_TEXT_EDITOR;
    windows[2].visible = false;
    windows[2].dragging = false;

    windows[3].bounds.x =
        (int32_t)(screen_width / 2) - 155;
    windows[3].bounds.y = 100;
    windows[3].bounds.width = 310;
    windows[3].bounds.height = 330;
    windows[3].title = "Calculator";
    windows[3].app = GUI_APP_CALCULATOR;
    windows[3].visible = false;
    windows[3].dragging = false;

    windows[4].bounds.x =
        (int32_t)(screen_width / 2) - 185;
    windows[4].bounds.y = 90;
    windows[4].bounds.width = 370;
    windows[4].bounds.height = 280;
    windows[4].title = "Paint";
    windows[4].app = GUI_APP_PAINT;
    windows[4].visible = false;
    windows[4].dragging = false;

    windows[5].bounds.x =
        (int32_t)(screen_width / 2) - 175;
    windows[5].bounds.y = 115;
    windows[5].bounds.width = 350;
    windows[5].bounds.height = 240;
    windows[5].title = "Settings";
    windows[5].app = GUI_APP_SETTINGS;
    windows[5].visible = false;
    windows[5].dragging = false;

    windows[6].bounds.x =
        (int32_t)(screen_width / 2) - 280;
    windows[6].bounds.y = 100;
    windows[6].bounds.width = 560;
    windows[6].bounds.height = 330;
    windows[6].title = "Process Manager";
    windows[6].app = GUI_APP_PROCESS_MANAGER;
    windows[6].visible = false;
    windows[6].dragging = false;

    for (
        uint8_t index = 0;
        index < GUI_WINDOW_COUNT;
        index++
    )
    {
        windows[index].minimized = false;
        windows[index].maximized = false;
        windows[index].fullscreen = false;
        windows[index].fullscreen_restore_maximized = false;
        windows[index].dragging = false;
        clamp_window(&windows[index]);
        windows[index].restore_bounds =
            windows[index].bounds;
        windows[index].fullscreen_restore_bounds =
            windows[index].bounds;
    }

    window_order[0] = 2;
    window_order[1] = 3;
    window_order[2] = 4;
    window_order[3] = 5;
    window_order[4] = 6;
    window_order[5] = 1;
    window_order[6] = 0;

    gui_terminal_reset_output();
    gui_terminal_append(
        "LatterOS GUI terminal ready.\n"
    );
    gui_terminal_append(
        "Type help to list commands.\n"
    );

    clear_text(
        terminal_input,
        sizeof(terminal_input)
    );

    terminal_input_length = 0;
    terminal_focused = false;

    launcher_open = false;
    system_menu_open = false;
    clock_text[0] = '-';
    clock_text[1] = '-';
    clock_text[2] = ':';
    clock_text[3] = '-';
    clock_text[4] = '-';
    clock_text[5] = '\0';
    last_clock_update = 0;
    update_clock(true);

    explorer_directory = vfs_root();
    explorer_selected = NULL;
    explorer_clear_preview();

    keyboard_set_character_handler(
        gui_keyboard_input
    );

    keyboard_set_event_handler(
        gui_keyboard_event
    );

    compositor_invalidate_all();
}

void gui_init(void)
{
    app_suite_init();

    initialized = true;
    active = false;
    start_requested = false;
    event_read_index = 0;
    event_write_index = 0;
    cursor_visible = false;
    cursor_moved = false;
    launcher_open = false;
    system_menu_open = false;
    clock_text[0] = '-';
    clock_text[1] = '-';
    clock_text[2] = ':';
    clock_text[3] = '-';
    clock_text[4] = '-';
    clock_text[5] = '\0';
    last_clock_update = 0;
}

void gui_request_start(void)
{
    if (!initialized)
    {
        gui_init();
    }

    start_requested = true;
}

void gui_update(void)
{
    if (start_requested && !active)
    {
        start_gui();
    }

    if (!active)
    {
        return;
    }

    update_clock(false);
    update_mouse_events();
    process_events();

    if (!active)
    {
        return;
    }

    if (compositor_has_damage())
    {
        hide_cursor();
        compositor_render();
        show_cursor();
        cursor_moved = false;
    }
    else if (cursor_moved || !cursor_visible)
    {
        show_cursor();
        cursor_moved = false;
    }
}

bool gui_is_active(void)
{
    return active;
}
