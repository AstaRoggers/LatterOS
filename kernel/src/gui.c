#include "gui.h"

#include "app_suite.h"
#include "boot_mode.h"
#include "boot_health.h"
#include "compositor.h"
#include "desktop_dialog.h"
#include "desktop_editor.h"
#include "desktop_font.h"
#include "desktop_services.h"
#include "display.h"
#include "graphics.h"
#include "hardware_compat.h"
#include "installer.h"
#include "keyboard.h"
#include "mouse.h"
#include "package_manager.h"
#include "power.h"
#include "process.h"
#include "recovery.h"
#include "release_info.h"
#include "stability_monitor.h"
#include "rtc.h"
#include "security.h"
#include "shell.h"
#include "terminal.h"
#include "timer.h"
#include "ui.h"
#include "ui_controls.h"
#include "user_home.h"
#include "vfs.h"
#include "virtio_gpu.h"
#include "visual_effects.h"
#include "window_animation.h"
#include "window_manager.h"
#include "window_surface.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GUI_EVENT_QUEUE_SIZE 96U
#define GUI_WINDOW_COUNT 8U
#define GUI_CURSOR_SIZE 16
#define GUI_TARGET_FRAME_RATE 60U
#define GUI_POINTER_FIXED_SHIFT 8
#define GUI_POINTER_FIXED_ONE (1 << GUI_POINTER_FIXED_SHIFT)
#define GUI_DRAG_CACHE_MAX_WIDTH 1024U
#define GUI_DRAG_CACHE_MAX_HEIGHT 768U
#define GUI_DRAG_CACHE_MAX_PIXELS \
    ((uint64_t)GUI_DRAG_CACHE_MAX_WIDTH * \
        GUI_DRAG_CACHE_MAX_HEIGHT)

#define TITLE_BAR_HEIGHT 28U
#define TASKBAR_HEIGHT 36U
#define WINDOW_RESIZE_BORDER 7U
#define WINDOW_SNAP_THRESHOLD 18U
#define WINDOW_SHADOW_SIZE 10U

#define TERMINAL_HISTORY_LINES 128U
#define TERMINAL_COLUMNS 96U
#define TERMINAL_INPUT_CAPACITY 128U
#define TERMINAL_LINE_HEIGHT 12U

#define EXPLORER_ROW_HEIGHT 22U
#define EXPLORER_TOOLBAR_HEIGHT 34U
#define EXPLORER_STATUS_HEIGHT 24U
#define EXPLORER_PREVIEW_HEIGHT 76U
#define EXPLORER_PATH_CAPACITY 512U
#define EXPLORER_STATUS_CAPACITY 96U

#define LAUNCHER_MENU_WIDTH 210U
#define SYSTEM_MENU_WIDTH 210U
#define CONTEXT_MENU_WIDTH 190U
#define DESKTOP_ICON_COUNT 7U
#define DESKTOP_ICON_WIDTH 92U
#define DESKTOP_ICON_HEIGHT 70U
#define DESKTOP_ICON_GAP 12U
#define DESKTOP_ICON_START_X 22
#define DESKTOP_ICON_START_Y 92
#define LAUNCHER_QUERY_CAPACITY 32U
#define LAUNCHER_DYNAMIC_ITEM_COUNT \
    (1U + GUI_WINDOW_COUNT + 1U + DESKTOP_RECENT_FILE_COUNT)
#define LAUNCHER_RECENT_COMMAND_BASE 300U
#define NOTIFICATION_WIDTH 320U
#define NOTIFICATION_HEIGHT 52U
#define NOTIFICATION_GAP 8U
#define FILE_DRAG_THRESHOLD 8
#define INSTALLER_VISIBLE_ROWS 6U
#define INSTALLER_STATUS_CAPACITY 160U
#define LAYOUT_AUTOSAVE_DELAY_MS 900U
#define DESKTOP_REPORT_CAPACITY 4096U

#define COLOR_TERMINAL 0x101820U
#define COLOR_TERMINAL_TEXT 0xD6F5D6U
#define COLOR_SHADOW 0x10253AU
#define COLOR_SNAP_PREVIEW 0x4D8FC4U

typedef enum
{
    GUI_EVENT_NONE,
    GUI_EVENT_MOUSE_MOVE,
    GUI_EVENT_MOUSE_DOWN,
    GUI_EVENT_MOUSE_UP,
    GUI_EVENT_MOUSE_RIGHT_DOWN,
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
    GUI_SHORTCUT_SHOW_DESKTOP,
    GUI_SHORTCUT_HELP,
    GUI_SHORTCUT_RENAME,
    GUI_SHORTCUT_REFRESH,
    GUI_SHORTCUT_COPY,
    GUI_SHORTCUT_CUT,
    GUI_SHORTCUT_PASTE,
    GUI_SHORTCUT_SELECT_ALL,
    GUI_SHORTCUT_SAVE
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
    GUI_APP_PROCESS_MANAGER,
    GUI_APP_INSTALLER
} gui_app_type_t;

typedef struct
{
    wm_window_state_t state;
    const char *title;
    gui_app_type_t app;
} gui_window_t;

typedef enum
{
    PENDING_ACTION_NONE,
    PENDING_ACTION_OPEN_FILE,
    PENDING_ACTION_NEW_FILE,
    PENDING_ACTION_NEW_FOLDER,
    PENDING_ACTION_RENAME,
    PENDING_ACTION_DELETE,
    PENDING_ACTION_COLOR,
    PENDING_ACTION_EDITOR_SAVE_AS,
    PENDING_ACTION_INSTALL_PREPARE,
    PENDING_ACTION_PACKAGE_INSTALL,
    PENDING_ACTION_PACKAGE_REMOVE
} pending_action_t;

typedef enum
{
    CONTEXT_COMMAND_OPEN = 1,
    CONTEXT_COMMAND_RENAME,
    CONTEXT_COMMAND_DELETE,
    CONTEXT_COMMAND_PROPERTIES,
    CONTEXT_COMMAND_OPEN_DIALOG,
    CONTEXT_COMMAND_NEW_FILE,
    CONTEXT_COMMAND_NEW_FOLDER,
    CONTEXT_COMMAND_REFRESH,
    CONTEXT_COMMAND_COPY_PATH,
    CONTEXT_COMMAND_CUT_PATH,
    CONTEXT_COMMAND_PASTE_ITEM
} context_command_t;

typedef enum
{
    SYSTEM_COMMAND_SWITCH_USER = 100,
    SYSTEM_COMMAND_TERMINAL_MODE,
    SYSTEM_COMMAND_ACCENT_COLOR,
    SYSTEM_COMMAND_THEME,
    SYSTEM_COMMAND_WALLPAPER,
    SYSTEM_COMMAND_RELOAD_THEME,
    SYSTEM_COMMAND_SAVE_LAYOUT,
    SYSTEM_COMMAND_CLEAR_NOTIFICATIONS,
    SYSTEM_COMMAND_INSTALLER,
    SYSTEM_COMMAND_RECOVERY,
    SYSTEM_COMMAND_ABOUT,
    SYSTEM_COMMAND_RESTART,
    SYSTEM_COMMAND_SHUTDOWN
} system_command_t;

static bool initialized;
static bool active;
static bool start_requested;

static gui_event_t event_queue[GUI_EVENT_QUEUE_SIZE];
static volatile uint32_t event_read_index;
static volatile uint32_t event_write_index;

static gui_window_t windows[GUI_WINDOW_COUNT];
static uint8_t window_order[GUI_WINDOW_COUNT];
static wm_window_state_t animation_targets[GUI_WINDOW_COUNT];
static bool animation_target_valid[GUI_WINDOW_COUNT];

static char terminal_lines[
    TERMINAL_HISTORY_LINES
][TERMINAL_COLUMNS + 1U];
static uint32_t terminal_line_count;
static uint32_t terminal_column;
static uint32_t terminal_scroll_offset;
static char terminal_input[TERMINAL_INPUT_CAPACITY];
static uint32_t terminal_input_length;
static uint32_t terminal_selection_start;
static uint32_t terminal_selection_end;
static bool terminal_focused;

static vfs_node_t *explorer_directory;
static vfs_node_t *explorer_selected;
static uint32_t explorer_scroll_offset;
static uint32_t explorer_hovered_row;
static char explorer_status[EXPLORER_STATUS_CAPACITY];
static bool explorer_drag_candidate;
static bool explorer_drag_active;
static int32_t explorer_drag_start_x;
static int32_t explorer_drag_start_y;
static vfs_node_t *explorer_drag_node;
static char explorer_drag_path[EXPLORER_PATH_CAPACITY];
static bool clipboard_file_cut;
static char clipboard_file_path[EXPLORER_PATH_CAPACITY];

static desktop_dialog_t dialog;
static pending_action_t pending_action;
static vfs_node_t *pending_node;
static char pending_path[EXPLORER_PATH_CAPACITY];

static ui_menu_t popup_menu;
static bool popup_is_launcher;
static bool popup_is_system;
static bool popup_is_desktop;
static char launcher_query[LAUNCHER_QUERY_CAPACITY];
static uint32_t launcher_query_length;
static char launcher_search_label[64];
static ui_menu_item_t launcher_dynamic_items[
    LAUNCHER_DYNAMIC_ITEM_COUNT
];
static uint32_t launcher_dynamic_count;

static int32_t cursor_x;
static int32_t cursor_y;
static int32_t cursor_target_x_fixed;
static int32_t cursor_target_y_fixed;
static int32_t last_mouse_x;
static int32_t last_mouse_y;
static uint64_t last_mouse_packets;
static bool last_left_button;
static bool last_right_button;
static bool cursor_visible;
static uint32_t desktop_hovered_icon;
static uint32_t desktop_selected_icon;

static uint32_t drag_cache[GUI_DRAG_CACHE_MAX_PIXELS]
    __attribute__((aligned(64)));
static uint32_t drag_cache_width;
static uint32_t drag_cache_height;
static uint8_t drag_cache_window;
static bool drag_cache_valid;

static uint64_t last_frame_tick;
static uint64_t frame_accumulator;
static char clock_text[6];
static uint64_t last_clock_update;
static uint32_t chrome_accent;
static uint32_t visible_notification_count;
static uint32_t installer_selected_target;
static char installer_status[INSTALLER_STATUS_CAPACITY];
static installer_report_t installer_report;
static uint32_t installer_visible_progress;
static bool recovery_panel_active;
typedef enum
{
    SETTINGS_PAGE_APPEARANCE,
    SETTINGS_PAGE_PACKAGES,
    SETTINGS_PAGE_HARDWARE,
    SETTINGS_PAGE_STABILITY,
    SETTINGS_PAGE_DESKTOP,
    SETTINGS_PAGE_COUNT
} settings_page_t;

static settings_page_t settings_page;
static bool animations_enabled;
static bool first_boot_notification_pending;
static stability_state_t visible_stability_state;

static ui_rect_t observed_layout[GUI_WINDOW_COUNT];
static bool layout_observer_ready;
static bool layout_autosave_pending;
static uint64_t layout_autosave_deadline;
static uint64_t layout_autosave_count;
static uint64_t launcher_clean_launch_count;
static uint64_t animation_surface_recovery_count;
static uint64_t manual_surface_rebuild_count;
static bool force_fresh_activation;

typedef struct
{
    const char *label;
    uint8_t window_index;
    uint32_t symbol_color;
} desktop_icon_t;

static const desktop_icon_t desktop_icons[DESKTOP_ICON_COUNT] = {
    { "Terminal", GUI_APP_TERMINAL, 0x202B36U },
    { "Files", GUI_APP_FILE_EXPLORER, 0xE0A52BU },
    { "Editor", GUI_APP_TEXT_EDITOR, 0x3B8CC4U },
    { "Paint", GUI_APP_PAINT, 0xC95D68U },
    { "Settings", GUI_APP_SETTINGS, 0x7C8794U },
    { "Tasks", GUI_APP_PROCESS_MANAGER, 0x48A06AU },
    { "Install", GUI_APP_INSTALLER, 0xB56B2DU }
};

static const ui_menu_item_t system_items[] = {
    { "Switch user", true, false, false, SYSTEM_COMMAND_SWITCH_USER },
    { "Terminal mode", true, false, false, SYSTEM_COMMAND_TERMINAL_MODE },
    { "Accent color...", true, false, false, SYSTEM_COMMAND_ACCENT_COLOR },
    { "Next theme", true, false, false, SYSTEM_COMMAND_THEME },
    { "Next wallpaper", true, false, false, SYSTEM_COMMAND_WALLPAPER },
    { "Reload external theme", true, false, false, SYSTEM_COMMAND_RELOAD_THEME },
    { "Save window layout", true, false, false, SYSTEM_COMMAND_SAVE_LAYOUT },
    { "Clear notifications", true, false, false, SYSTEM_COMMAND_CLEAR_NOTIFICATIONS },
    { "Install LatterOS", true, false, false, SYSTEM_COMMAND_INSTALLER },
    { "Recovery tools", true, false, false, SYSTEM_COMMAND_RECOVERY },
    { "About LatterOS", true, false, false, SYSTEM_COMMAND_ABOUT },
    { NULL, false, false, true, 0 },
    { "Restart", true, false, false, SYSTEM_COMMAND_RESTART },
    { "Shut down", true, false, false, SYSTEM_COMMAND_SHUTDOWN }
};

static ui_menu_item_t context_items[] = {
    { "Open", true, false, false, CONTEXT_COMMAND_OPEN },
    { "Rename...", true, false, false, CONTEXT_COMMAND_RENAME },
    { "Delete...", true, false, false, CONTEXT_COMMAND_DELETE },
    { "Properties", true, false, false, CONTEXT_COMMAND_PROPERTIES },
    { NULL, false, false, true, 0 },
    { "Copy", true, false, false, CONTEXT_COMMAND_COPY_PATH },
    { "Cut", true, false, false, CONTEXT_COMMAND_CUT_PATH },
    { "Paste", true, false, false, CONTEXT_COMMAND_PASTE_ITEM },
    { NULL, false, false, true, 0 },
    { "Open file...", true, false, false, CONTEXT_COMMAND_OPEN_DIALOG },
    { "New file...", true, false, false, CONTEXT_COMMAND_NEW_FILE },
    { "New folder...", true, false, false, CONTEXT_COMMAND_NEW_FOLDER },
    { "Refresh", true, false, false, CONTEXT_COMMAND_REFRESH }
};

static void stop_gui(void);
static void render_gui_scene(void);
static void activate_window(uint8_t index);
static void invalidate_window(uint8_t index);
static void refresh_explorer(void);
static void build_node_path(
    const vfs_node_t *node,
    char *buffer,
    size_t capacity
);
static void save_window_layout(void);
static void update_layout_autosave(void);
static void rebuild_launcher_items(void);
static void show_editor_save_as_dialog(void);
static void show_installer_confirmation(void);
static void show_package_install_dialog(const char *path);
static void show_package_remove_dialog(const char *name);
static void open_associated_node(vfs_node_t *node);
static void update_window_animations(void);
static bool begin_window_state_animation(
    uint8_t index,
    window_animation_type_t type,
    const wm_window_state_t *target,
    const ui_rect_t *visual_target,
    uint32_t duration_ms
);

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

static void clear_text(char *text, size_t capacity)
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

static void append_unsigned_decimal(
    char *destination,
    size_t capacity,
    uint64_t value
)
{
    char reverse[24];
    uint32_t count = 0;

    if (value == 0)
    {
        append_text(destination, capacity, "0");
        return;
    }

    while (value != 0 && count < sizeof(reverse))
    {
        reverse[count++] = (char)('0' + value % 10ULL);
        value /= 10ULL;
    }

    while (count != 0)
    {
        char text[2] = { reverse[--count], '\0' };
        append_text(destination, capacity, text);
    }
}

static int32_t absolute_value(int32_t value)
{
    return value < 0 ? -value : value;
}

static const char *path_basename(const char *path)
{
    if (path == NULL)
    {
        return "";
    }

    const char *name = path;

    for (size_t index = 0; path[index] != '\0'; index++)
    {
        if (path[index] == '/')
        {
            name = &path[index + 1];
        }
    }

    return name;
}

static void build_node_path(
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

static ui_palette_t current_palette(void)
{
    const gui_theme_t *theme = desktop_services_theme();

    ui_palette_t palette = {
        .background = theme->desktop,
        .panel = theme->window,
        .border = theme->window_border,
        .text = theme->text,
        .light_text = theme->light_text,
        .field = theme->field,
        .accent = chrome_accent,
        .accent_hover = theme->row_selected,
        .selected = theme->row_selected,
        .disabled = theme->title_idle,
        .danger = theme->close
    };

    return palette;
}

static bool frame_is_due(void)
{
    uint32_t frequency = timer_frequency();
    uint64_t now = timer_ticks();

    if (frequency == 0)
    {
        return true;
    }

    if (now == last_frame_tick)
    {
        return false;
    }

    uint64_t elapsed = now - last_frame_tick;
    last_frame_tick = now;
    frame_accumulator +=
        elapsed * GUI_TARGET_FRAME_RATE;

    if (frame_accumulator < frequency)
    {
        return false;
    }

    frame_accumulator %= frequency;
    return true;
}

static void queue_event(gui_event_t event)
{
    if (
        event.type == GUI_EVENT_MOUSE_MOVE &&
        event_read_index != event_write_index
    )
    {
        uint32_t previous =
            (event_write_index + GUI_EVENT_QUEUE_SIZE - 1U) %
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
        (event_write_index + 1U) %
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
        (event_read_index + 1U) %
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
    if (event == NULL || !event->pressed)
    {
        return false;
    }

    gui_shortcut_t shortcut = GUI_SHORTCUT_NONE;

    if (event->key == KEYBOARD_KEY_F11)
    {
        shortcut = GUI_SHORTCUT_FULLSCREEN;
    }
    else if (event->key == KEYBOARD_KEY_F1)
    {
        shortcut = GUI_SHORTCUT_HELP;
    }
    else if (event->key == KEYBOARD_KEY_F2)
    {
        shortcut = GUI_SHORTCUT_RENAME;
    }
    else if (event->key == KEYBOARD_KEY_F5)
    {
        shortcut = GUI_SHORTCUT_REFRESH;
    }
    else if (
        event->control &&
        (event->character == 'c' || event->character == 'C')
    )
    {
        shortcut = GUI_SHORTCUT_COPY;
    }
    else if (
        event->control &&
        (event->character == 'x' || event->character == 'X')
    )
    {
        shortcut = GUI_SHORTCUT_CUT;
    }
    else if (
        event->control &&
        (event->character == 'v' || event->character == 'V')
    )
    {
        shortcut = GUI_SHORTCUT_PASTE;
    }
    else if (
        event->control &&
        (event->character == 'a' || event->character == 'A')
    )
    {
        shortcut = GUI_SHORTCUT_SELECT_ALL;
    }
    else if (
        event->control &&
        (event->character == 's' || event->character == 'S')
    )
    {
        shortcut = GUI_SHORTCUT_SAVE;
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

static int32_t pointer_scale_fixed(
    int32_t delta_x,
    int32_t delta_y
)
{
    (void)delta_x;
    (void)delta_y;

    /*
     * Keep the virtual mouse strictly one-to-one. QEMU can coalesce
     * relative reports when its window is moved, resized, grabbed, or
     * released. Accelerating those reports turns a harmless burst into a
     * jump across the desktop.
     */
    return GUI_POINTER_FIXED_ONE;
}

static int32_t clamp_pointer_delta(int32_t delta)
{
    const int32_t maximum_delta = 96;

    if (delta > maximum_delta)
    {
        return maximum_delta;
    }

    if (delta < -maximum_delta)
    {
        return -maximum_delta;
    }

    return delta;
}

static void clamp_cursor_target(void)
{
    int32_t maximum_x =
        (int32_t)graphics_width() - GUI_CURSOR_SIZE;

    int32_t maximum_y =
        (int32_t)graphics_height() - GUI_CURSOR_SIZE;

    int32_t maximum_x_fixed =
        maximum_x * GUI_POINTER_FIXED_ONE;

    int32_t maximum_y_fixed =
        maximum_y * GUI_POINTER_FIXED_ONE;

    if (cursor_target_x_fixed < 0)
    {
        cursor_target_x_fixed = 0;
    }

    if (cursor_target_y_fixed < 0)
    {
        cursor_target_y_fixed = 0;
    }

    if (cursor_target_x_fixed > maximum_x_fixed)
    {
        cursor_target_x_fixed = maximum_x_fixed;
    }

    if (cursor_target_y_fixed > maximum_y_fixed)
    {
        cursor_target_y_fixed = maximum_y_fixed;
    }
}

static ui_rect_t taskbar_bounds(void)
{
    return (ui_rect_t){
        .x = 0,
        .y = (int32_t)graphics_height() -
            (int32_t)TASKBAR_HEIGHT,
        .width = graphics_width(),
        .height = TASKBAR_HEIGHT
    };
}

static ui_rect_t start_button_bounds(void)
{
    ui_rect_t taskbar = taskbar_bounds();

    return (ui_rect_t){
        .x = 8,
        .y = taskbar.y + 6,
        .width = 82,
        .height = 24
    };
}

static ui_rect_t system_button_bounds(void)
{
    ui_rect_t taskbar = taskbar_bounds();

    return (ui_rect_t){
        .x = (int32_t)graphics_width() - 96,
        .y = taskbar.y + 6,
        .width = 88,
        .height = 24
    };
}


static ui_rect_t notification_tray_bounds(void)
{
    ui_rect_t taskbar = taskbar_bounds();

    return (ui_rect_t){
        .x = (int32_t)graphics_width() - 270,
        .y = taskbar.y + 7,
        .width = 28,
        .height = 22
    };
}

static ui_rect_t clipboard_tray_bounds(void)
{
    ui_rect_t taskbar = taskbar_bounds();

    return (ui_rect_t){
        .x = (int32_t)graphics_width() - 236,
        .y = taskbar.y + 7,
        .width = 28,
        .height = 22
    };
}

static ui_rect_t taskbar_app_button(uint8_t index)
{
    ui_rect_t taskbar = taskbar_bounds();
    uint32_t left_reserved = 98U;
    uint32_t right_reserved = 360U;
    uint32_t available =
        graphics_width() > left_reserved + right_reserved ?
            graphics_width() - left_reserved - right_reserved :
            GUI_WINDOW_COUNT * 48U;

    uint32_t gap = 4U;
    uint32_t button_width =
        available > gap * (GUI_WINDOW_COUNT - 1U) ?
            (available - gap * (GUI_WINDOW_COUNT - 1U)) /
                GUI_WINDOW_COUNT : 48U;

    if (button_width > 110U)
    {
        button_width = 110U;
    }

    if (button_width < 48U)
    {
        button_width = 48U;
    }

    return (ui_rect_t){
        .x = 98 + (int32_t)index *
            (int32_t)(button_width + gap),
        .y = taskbar.y + 6,
        .width = button_width,
        .height = 24
    };
}

static ui_rect_t window_visual_bounds(
    const gui_window_t *window
)
{
    if (window == NULL)
    {
        return (ui_rect_t){ 0, 0, 0, 0 };
    }

    ui_rect_t rectangle = window->state.bounds;

    if (!window->state.fullscreen)
    {
        rectangle.width += WINDOW_SHADOW_SIZE;
        rectangle.height += WINDOW_SHADOW_SIZE;
    }

    return rectangle;
}

static ui_rect_t window_title_bar(
    const gui_window_t *window
)
{
    return (ui_rect_t){
        .x = window->state.bounds.x,
        .y = window->state.bounds.y,
        .width = window->state.bounds.width,
        .height = TITLE_BAR_HEIGHT
    };
}

static ui_rect_t window_close_button(
    const gui_window_t *window
)
{
    return (ui_rect_t){
        .x = window->state.bounds.x +
            (int32_t)window->state.bounds.width - 24,
        .y = window->state.bounds.y + 5,
        .width = 18,
        .height = 18
    };
}

static ui_rect_t window_maximize_button(
    const gui_window_t *window
)
{
    return (ui_rect_t){
        .x = window->state.bounds.x +
            (int32_t)window->state.bounds.width - 46,
        .y = window->state.bounds.y + 5,
        .width = 18,
        .height = 18
    };
}

static ui_rect_t window_minimize_button(
    const gui_window_t *window
)
{
    return (ui_rect_t){
        .x = window->state.bounds.x +
            (int32_t)window->state.bounds.width - 68,
        .y = window->state.bounds.y + 5,
        .width = 18,
        .height = 18
    };
}

static ui_rect_t window_content_bounds(
    const gui_window_t *window
)
{
    if (window->state.fullscreen)
    {
        return window->state.bounds;
    }

    return (ui_rect_t){
        .x = window->state.bounds.x + 2,
        .y = window->state.bounds.y +
            (int32_t)TITLE_BAR_HEIGHT,
        .width = window->state.bounds.width > 4U ?
            window->state.bounds.width - 4U : 1U,
        .height = window->state.bounds.height >
            TITLE_BAR_HEIGHT + 2U ?
            window->state.bounds.height -
                TITLE_BAR_HEIGHT - 2U : 1U
    };
}

static const char *app_label(uint8_t index)
{
    static const char *labels[GUI_WINDOW_COUNT] = {
        "Terminal",
        "Files",
        "Editor",
        "Calc",
        "Paint",
        "Settings",
        "Tasks",
        "Installer"
    };

    if (index == 7U && recovery_panel_active)
    {
        return "Recovery";
    }

    return index < GUI_WINDOW_COUNT ?
        labels[index] : "App";
}


static bool text_matches_query(
    const char *text,
    const char *query
)
{
    if (query == NULL || query[0] == '\0')
    {
        return true;
    }

    if (text == NULL)
    {
        return false;
    }

    size_t query_length = string_length(query);
    size_t text_length = string_length(text);

    if (query_length > text_length)
    {
        return false;
    }

    for (
        size_t start = 0;
        start + query_length <= text_length;
        start++
    )
    {
        bool matched = true;

        for (size_t index = 0; index < query_length; index++)
        {
            char first = text[start + index];
            char second = query[index];

            if (first >= 'A' && first <= 'Z')
            {
                first = (char)(first - 'A' + 'a');
            }

            if (second >= 'A' && second <= 'Z')
            {
                second = (char)(second - 'A' + 'a');
            }

            if (first != second)
            {
                matched = false;
                break;
            }
        }

        if (matched)
        {
            return true;
        }
    }

    return false;
}

static ui_rect_t desktop_icon_bounds(uint32_t index)
{
    uint32_t column = index % 2U;
    uint32_t row = index / 2U;

    ui_rect_t bounds = {
        .x = DESKTOP_ICON_START_X +
            (int32_t)column *
                (int32_t)(DESKTOP_ICON_WIDTH + DESKTOP_ICON_GAP),
        .y = DESKTOP_ICON_START_Y +
            (int32_t)row *
                (int32_t)(DESKTOP_ICON_HEIGHT + DESKTOP_ICON_GAP),
        .width = DESKTOP_ICON_WIDTH,
        .height = DESKTOP_ICON_HEIGHT
    };

    return bounds;
}

static uint32_t desktop_icon_at(int32_t x, int32_t y)
{
    for (uint32_t index = 0; index < DESKTOP_ICON_COUNT; index++)
    {
        ui_rect_t bounds = desktop_icon_bounds(index);

        if (ui_point_in_rect(x, y, &bounds))
        {
            return index;
        }
    }

    return UINT32_MAX;
}

static void rebuild_launcher_items(void)
{
    launcher_dynamic_count = 0;

    copy_text(
        launcher_search_label,
        sizeof(launcher_search_label),
        launcher_query_length == 0 ?
            "Search: type to filter" : "Search: "
    );

    if (launcher_query_length != 0)
    {
        append_text(
            launcher_search_label,
            sizeof(launcher_search_label),
            launcher_query
        );
    }

    launcher_dynamic_items[launcher_dynamic_count++] =
        (ui_menu_item_t){
            launcher_search_label,
            false,
            false,
            false,
            0
        };

    for (uint32_t index = 0; index < GUI_WINDOW_COUNT; index++)
    {
        const char *label = app_label((uint8_t)index);

        if (!text_matches_query(label, launcher_query))
        {
            continue;
        }

        launcher_dynamic_items[launcher_dynamic_count++] =
            (ui_menu_item_t){
                label,
                true,
                windows[index].state.visible &&
                    !windows[index].state.minimized,
                false,
                index
            };
    }

    uint32_t recent_count = desktop_recent_count();
    bool added_separator = false;

    for (uint32_t index = 0; index < recent_count; index++)
    {
        const char *path = desktop_recent_path(index);

        if (
            path == NULL ||
            !text_matches_query(path, launcher_query)
        )
        {
            continue;
        }

        if (!added_separator)
        {
            launcher_dynamic_items[launcher_dynamic_count++] =
                (ui_menu_item_t){ NULL, false, false, true, 0 };
            added_separator = true;
        }

        launcher_dynamic_items[launcher_dynamic_count++] =
            (ui_menu_item_t){
                path_basename(path),
                true,
                false,
                false,
                LAUNCHER_RECENT_COMMAND_BASE + index
            };
    }
}

static const ui_rect_t *persisted_window_bounds(uint32_t index)
{
    if (index >= GUI_WINDOW_COUNT)
    {
        return NULL;
    }

    const wm_window_state_t *state = &windows[index].state;

    if (
        state->maximized ||
        state->fullscreen ||
        state->snap != WM_SNAP_NONE
    )
    {
        return &state->restore_bounds;
    }

    return &state->bounds;
}

static bool rectangles_equal(
    const ui_rect_t *first,
    const ui_rect_t *second
)
{
    return
        first != NULL &&
        second != NULL &&
        first->x == second->x &&
        first->y == second->y &&
        first->width == second->width &&
        first->height == second->height;
}

static void refresh_layout_observer(void)
{
    for (uint32_t index = 0; index < GUI_WINDOW_COUNT; index++)
    {
        const ui_rect_t *bounds = persisted_window_bounds(index);

        if (bounds != NULL)
        {
            observed_layout[index] = *bounds;
        }
    }

    layout_observer_ready = true;
}

static bool save_window_layout_internal(
    bool notify_user,
    bool automatic
)
{
    for (uint32_t index = 0; index < GUI_WINDOW_COUNT; index++)
    {
        const ui_rect_t *bounds = persisted_window_bounds(index);

        if (bounds != NULL)
        {
            desktop_services_store_window(index, bounds);
        }
    }

    bool saved = desktop_services_save();

    if (saved)
    {
        if (automatic)
        {
            layout_autosave_count++;
        }

        refresh_layout_observer();
        layout_autosave_pending = false;
    }

    if (notify_user)
    {
        desktop_notify(
            saved ? "Desktop layout saved" :
                "Unable to save desktop layout",
            saved ? 3500U : 5000U
        );
    }

    return saved;
}

static void save_window_layout(void)
{
    (void)save_window_layout_internal(true, false);
}

static void update_layout_autosave(void)
{
    if (!layout_observer_ready)
    {
        refresh_layout_observer();
        return;
    }

    bool changed = false;

    for (uint32_t index = 0; index < GUI_WINDOW_COUNT; index++)
    {
        const ui_rect_t *bounds = persisted_window_bounds(index);

        if (
            bounds != NULL &&
            !rectangles_equal(bounds, &observed_layout[index])
        )
        {
            observed_layout[index] = *bounds;
            changed = true;
        }
    }

    uint64_t ticks = timer_ticks();
    uint32_t frequency = timer_frequency();

    if (changed)
    {
        uint64_t delay_ticks = frequency == 0U ?
            1ULL :
            ((uint64_t)frequency * LAYOUT_AUTOSAVE_DELAY_MS + 999ULL) /
                1000ULL;

        if (delay_ticks == 0ULL)
        {
            delay_ticks = 1ULL;
        }

        layout_autosave_pending = true;
        layout_autosave_deadline = ticks + delay_ticks;
    }

    if (
        layout_autosave_pending &&
        ticks >= layout_autosave_deadline
    )
    {
        (void)save_window_layout_internal(false, true);
    }
}

static void reset_window_positions(void)
{
    uint32_t screen_width = graphics_width();
    uint32_t screen_height = graphics_height();

    static const int32_t x_offsets[GUI_WINDOW_COUNT] = {
        -390, -220, -280, -155, -200, -185, -280, -310
    };

    static const int32_t y_positions[GUI_WINDOW_COUNT] = {
        70, 90, 70, 100, 90, 115, 100, 86
    };

    static const uint32_t widths[GUI_WINDOW_COUNT] = {
        650U, 620U, 580U, 320U, 420U, 470U, 580U, 620U
    };

    static const uint32_t heights[GUI_WINDOW_COUNT] = {
        390U, 500U, 430U, 350U, 330U, 410U, 360U, 420U
    };

    for (uint32_t index = 0; index < GUI_WINDOW_COUNT; index++)
    {
        wm_window_state_t *state = &windows[index].state;
        ui_rect_t bounds = {
            .x = (int32_t)(screen_width / 2U) + x_offsets[index],
            .y = y_positions[index],
            .width = widths[index],
            .height = heights[index]
        };

        state->bounds = bounds;
        state->restore_bounds = bounds;
        state->maximized = false;
        state->fullscreen = false;
        state->snap = WM_SNAP_NONE;
        state->pending_snap = WM_SNAP_NONE;
        wm_cancel_interaction(state);

        wm_clamp_window(
            state,
            screen_width,
            screen_height,
            TASKBAR_HEIGHT
        );

        state->restore_bounds = state->bounds;
        window_animation_cancel(index);
        animation_target_valid[index] = false;
        window_surface_invalidate(index);
    }

    drag_cache_valid = false;
    compositor_invalidate_all();
    (void)save_window_layout_internal(false, false);
    desktop_notify("Window positions reset and saved", 4500U);
}

static bool write_desktop_reliability_report(void)
{
    char report[DESKTOP_REPORT_CAPACITY];
    report[0] = '\0';

    append_text(report, sizeof(report), "LatterOS Desktop Reliability Report\n");
    append_text(report, sizeof(report), "Release: ");
    append_text(report, sizeof(report), release_info_version());
    append_text(report, sizeof(report), " / Milestone ");
    append_text(report, sizeof(report), release_info_milestone());
    append_text(report, sizeof(report), "\n\n");

    append_text(report, sizeof(report), "Launcher fresh-surface launches: ");
    append_unsigned_decimal(
        report,
        sizeof(report),
        launcher_clean_launch_count
    );
    append_text(report, sizeof(report), "\nAnimation surface recoveries: ");
    append_unsigned_decimal(
        report,
        sizeof(report),
        animation_surface_recovery_count
    );
    append_text(report, sizeof(report), "\nManual surface rebuilds: ");
    append_unsigned_decimal(
        report,
        sizeof(report),
        manual_surface_rebuild_count
    );
    append_text(report, sizeof(report), "\nAutomatic layout saves: ");
    append_unsigned_decimal(report, sizeof(report), layout_autosave_count);
    append_text(report, sizeof(report), "\nLayout autosave pending: ");
    append_text(
        report,
        sizeof(report),
        layout_autosave_pending ? "yes" : "no"
    );

    append_text(report, sizeof(report), "\n\nWindow surface captures: ");
    append_unsigned_decimal(
        report,
        sizeof(report),
        window_surface_total_captures()
    );
    append_text(report, sizeof(report), "\nWindow surface draws: ");
    append_unsigned_decimal(
        report,
        sizeof(report),
        window_surface_total_draws()
    );
    append_text(report, sizeof(report), "\nWindow surface scaled draws: ");
    append_unsigned_decimal(
        report,
        sizeof(report),
        window_surface_total_scaled_draws()
    );

    append_text(report, sizeof(report), "\n\nCompositor backend: ");
    append_text(report, sizeof(report), compositor_display_backend());
    append_text(report, sizeof(report), "\nCompositor frames: ");
    append_unsigned_decimal(
        report,
        sizeof(report),
        compositor_frame_count()
    );
    append_text(report, sizeof(report), "\nRecent active FPS: ");
    append_unsigned_decimal(
        report,
        sizeof(report),
        compositor_recent_active_fps()
    );
    append_text(report, sizeof(report), "\nDropped frames: ");
    append_unsigned_decimal(
        report,
        sizeof(report),
        compositor_dropped_frame_count()
    );
    append_text(report, sizeof(report), "\nTriple buffered: ");
    append_text(
        report,
        sizeof(report),
        compositor_triple_buffered() ? "yes" : "no"
    );
    append_text(report, sizeof(report), "\n");

    return vfs_write_text(
        "/home/user/Documents/Desktop Reliability Report.txt",
        report
    );
}

static bool destination_path_for_node(
    const vfs_node_t *directory,
    const char *name,
    char *path,
    size_t capacity
)
{
    if (
        directory == NULL ||
        name == NULL ||
        name[0] == '\0' ||
        path == NULL ||
        capacity == 0
    )
    {
        return false;
    }

    build_node_path(directory, path, capacity);

    if (
        string_length(path) > 1U &&
        path[string_length(path) - 1U] != '/'
    )
    {
        append_text(path, capacity, "/");
    }

    append_text(path, capacity, name);
    return path[0] != '\0';
}

static bool paste_file_into_explorer(void)
{
    if (
        explorer_directory == NULL ||
        clipboard_file_path[0] == '\0'
    )
    {
        desktop_notify("No copied file or folder", 3500U);
        return false;
    }

    char destination[EXPLORER_PATH_CAPACITY];

    if (!destination_path_for_node(
        explorer_directory,
        path_basename(clipboard_file_path),
        destination,
        sizeof(destination)
    ))
    {
        desktop_notify("Invalid paste destination", 3500U);
        return false;
    }

    bool success = clipboard_file_cut ?
        vfs_move(clipboard_file_path, destination) :
        vfs_copy(clipboard_file_path, destination);

    if (success)
    {
        desktop_notify(
            clipboard_file_cut ? "Item moved" : "Item copied",
            3500U
        );

        if (clipboard_file_cut)
        {
            clipboard_file_path[0] = '\0';
            clipboard_file_cut = false;
        }

        refresh_explorer();
        return true;
    }

    desktop_notify("Paste failed", 4500U);
    return false;
}

static void copy_selected_file(bool cut)
{
    if (explorer_selected == NULL)
    {
        desktop_notify("Select a file or folder first", 3500U);
        return;
    }

    build_node_path(
        explorer_selected,
        clipboard_file_path,
        sizeof(clipboard_file_path)
    );

    clipboard_file_cut = cut;
    (void)desktop_clipboard_set_text(clipboard_file_path);
    desktop_notify(cut ? "Item cut" : "Item copied", 3000U);
}

static void terminal_clear_selection(void)
{
    terminal_selection_start = terminal_input_length;
    terminal_selection_end = terminal_input_length;
}

static bool terminal_has_selection(void)
{
    return terminal_selection_start != terminal_selection_end;
}

static void terminal_selection_bounds(
    uint32_t *start,
    uint32_t *end
)
{
    uint32_t first = terminal_selection_start;
    uint32_t second = terminal_selection_end;

    if (first > second)
    {
        uint32_t temporary = first;
        first = second;
        second = temporary;
    }

    if (first > terminal_input_length)
    {
        first = terminal_input_length;
    }

    if (second > terminal_input_length)
    {
        second = terminal_input_length;
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

static void terminal_delete_selection(void)
{
    if (!terminal_has_selection())
    {
        return;
    }

    uint32_t start;
    uint32_t end;
    terminal_selection_bounds(&start, &end);
    uint32_t tail = terminal_input_length - end;

    for (uint32_t index = 0; index <= tail; index++)
    {
        terminal_input[start + index] =
            terminal_input[end + index];
    }

    terminal_input_length -= end - start;
    terminal_selection_start = start;
    terminal_selection_end = start;
}

static void terminal_copy_selection(bool cut)
{
    if (!terminal_has_selection())
    {
        desktop_notify("No terminal text selected", 3000U);
        return;
    }

    uint32_t start;
    uint32_t end;
    terminal_selection_bounds(&start, &end);
    (void)desktop_clipboard_set_range(
        terminal_input,
        start,
        end
    );

    if (cut)
    {
        terminal_delete_selection();
    }

    desktop_notify(cut ? "Text cut" : "Text copied", 2500U);
    invalidate_window(0);
}

static void terminal_paste(void)
{
    const char *text = desktop_clipboard_text();
    size_t length = desktop_clipboard_length();

    if (text == NULL || length == 0)
    {
        desktop_notify("Clipboard is empty", 3000U);
        return;
    }

    terminal_delete_selection();

    size_t available =
        TERMINAL_INPUT_CAPACITY - 1U - terminal_input_length;

    if (length > available)
    {
        length = available;
    }

    for (size_t index = 0; index < length; index++)
    {
        char character = text[index];

        if (character < 32 || character > 126)
        {
            character = ' ';
        }

        terminal_input[terminal_input_length++] = character;
    }

    terminal_input[terminal_input_length] = '\0';
    terminal_clear_selection();
    desktop_notify("Clipboard pasted", 2500U);
    invalidate_window(0);
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
        !windows[index].state.visible ||
        windows[index].state.minimized
    )
    {
        return;
    }

    window_surface_invalidate(index);

    ui_rect_t rectangle =
        window_visual_bounds(&windows[index]);

    compositor_invalidate(&rectangle);
}

static void invalidate_visible_windows(void)
{
    for (uint8_t index = 0; index < GUI_WINDOW_COUNT; index++)
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
        current + 1U < GUI_WINDOW_COUNT;
        current++
    )
    {
        window_order[current] =
            window_order[current + 1U];
    }

    window_order[GUI_WINDOW_COUNT - 1U] = index;
}

static uint8_t active_window_index(void)
{
    for (
        int32_t order = (int32_t)GUI_WINDOW_COUNT - 1;
        order >= 0;
        order--
    )
    {
        uint8_t candidate = window_order[order];

        if (
            windows[candidate].state.visible &&
            !windows[candidate].state.minimized
        )
        {
            return candidate;
        }
    }

    return GUI_WINDOW_COUNT;
}

static bool window_is_front(uint8_t index)
{
    return active_window_index() == index;
}

static uint8_t active_fullscreen_window(void)
{
    uint8_t index = active_window_index();

    return
        index < GUI_WINDOW_COUNT &&
        windows[index].state.fullscreen ?
            index : GUI_WINDOW_COUNT;
}

static void close_popup_menu(void)
{
    bool was_visible = popup_menu.visible;
    ui_rect_t old_bounds = popup_menu.bounds;

    ui_menu_close(&popup_menu);
    popup_is_launcher = false;
    popup_is_system = false;
    popup_is_desktop = false;

    /*
     * Popup menus are composed after the desktop and windows. Closing one
     * must damage its previous screen area even when the selected command
     * immediately starts a window animation. Without this invalidation the
     * old launcher pixels remain in the scanout until another full redraw.
     */
    if (was_visible)
    {
        compositor_invalidate(&old_bounds);
    }
}

static ui_rect_t animation_visual_bounds(
    const ui_rect_t *bounds
)
{
    if (bounds == NULL)
    {
        return (ui_rect_t){ 0, 0, 0, 0 };
    }

    ui_rect_t result = *bounds;
    result.width += WINDOW_SHADOW_SIZE + 8U;
    result.height += WINDOW_SHADOW_SIZE + 8U;
    return result;
}

static bool begin_window_state_animation(
    uint8_t index,
    window_animation_type_t type,
    const wm_window_state_t *target,
    const ui_rect_t *visual_target,
    uint32_t duration_ms
)
{
    if (
        !animations_enabled ||
        index >= GUI_WINDOW_COUNT ||
        target == NULL ||
        visual_target == NULL ||
        window_animation_active(index) ||
        !window_surface_valid_for(
            index,
            windows[index].state.bounds.width,
            windows[index].state.bounds.height
        )
    )
    {
        return false;
    }

    ui_rect_t from = windows[index].state.bounds;

    if (!window_animation_start(
        index,
        type,
        &from,
        visual_target,
        duration_ms
    ))
    {
        return false;
    }

    animation_targets[index] = *target;
    animation_target_valid[index] = true;
    wm_cancel_interaction(&windows[index].state);
    drag_cache_valid = false;

    ui_rect_t first = animation_visual_bounds(&from);
    ui_rect_t last = animation_visual_bounds(visual_target);
    ui_rect_t damage = ui_union(&first, &last);
    compositor_invalidate(&damage);
    invalidate_taskbar();
    return true;
}

static void update_window_animations(void)
{
    for (uint8_t index = 0; index < GUI_WINDOW_COUNT; index++)
    {
        if (!window_animation_active(index))
        {
            continue;
        }

        window_animation_type_t type =
            window_animation_type(index);

        ui_rect_t previous;
        ui_rect_t current;
        bool finished = false;

        if (!window_animation_step(
            index,
            &previous,
            &current,
            &finished
        ))
        {
            continue;
        }

        ui_rect_t previous_visual =
            animation_visual_bounds(&previous);

        ui_rect_t current_visual =
            animation_visual_bounds(&current);

        ui_rect_t damage = ui_union(
            &previous_visual,
            &current_visual
        );

        compositor_invalidate(&damage);

        if (!finished || !animation_target_valid[index])
        {
            continue;
        }

        uint8_t previous_active = active_window_index();
        windows[index].state = animation_targets[index];
        animation_target_valid[index] = false;

        if (
            type == WINDOW_ANIMATION_MAXIMIZE ||
            type == WINDOW_ANIMATION_RESTORE
        )
        {
            window_surface_invalidate(index);
        }

        if (
            windows[index].state.visible &&
            !windows[index].state.minimized
        )
        {
            bring_window_to_front(index);

            if (
                previous_active < GUI_WINDOW_COUNT &&
                previous_active != index
            )
            {
                window_surface_invalidate(previous_active);
                ui_rect_t previous_bounds =
                    window_visual_bounds(&windows[previous_active]);
                compositor_invalidate(&previous_bounds);
            }

            window_surface_invalidate(index);
            ui_rect_t final_bounds =
                window_visual_bounds(&windows[index]);
            compositor_invalidate(&final_bounds);
        }

        invalidate_taskbar();
    }
}

static void minimize_window(uint8_t index)
{
    if (
        index >= GUI_WINDOW_COUNT ||
        !windows[index].state.visible ||
        windows[index].state.minimized ||
        window_animation_active(index)
    )
    {
        return;
    }

    wm_window_state_t target = windows[index].state;
    target.minimized = true;
    target.dragging = false;
    target.resizing = false;
    target.pending_snap = WM_SNAP_NONE;

    ui_rect_t destination = taskbar_app_button(index);

    if (begin_window_state_animation(
        index,
        WINDOW_ANIMATION_MINIMIZE,
        &target,
        &destination,
        150U
    ))
    {
        terminal_focused = false;
        return;
    }

    ui_rect_t old = window_visual_bounds(&windows[index]);
    windows[index].state = target;
    terminal_focused = false;
    drag_cache_valid = false;
    compositor_invalidate(&old);
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
        !window->state.visible ||
        window->state.minimized ||
        window->state.fullscreen ||
        window_animation_active(index)
    )
    {
        return;
    }

    wm_window_state_t target = window->state;

    wm_toggle_maximize(
        &target,
        graphics_width(),
        graphics_height(),
        TASKBAR_HEIGHT
    );

    window_animation_type_t type = target.maximized ?
        WINDOW_ANIMATION_MAXIMIZE : WINDOW_ANIMATION_RESTORE;

    if (begin_window_state_animation(
        index,
        type,
        &target,
        &target.bounds,
        165U
    ))
    {
        return;
    }

    ui_rect_t old = window_visual_bounds(window);
    window->state = target;
    drag_cache_valid = false;
    window_surface_invalidate(index);
    compositor_invalidate(&old);
    ui_rect_t next = window_visual_bounds(window);
    compositor_invalidate(&next);
    invalidate_taskbar();
}

static void toggle_fullscreen_window(uint8_t index)
{
    if (index >= GUI_WINDOW_COUNT)
    {
        return;
    }

    window_animation_cancel(index);
    animation_target_valid[index] = false;
    window_surface_invalidate(index);

    wm_toggle_fullscreen(
        &windows[index].state,
        graphics_width(),
        graphics_height()
    );

    close_popup_menu();
    drag_cache_valid = false;
    bring_window_to_front(index);
    compositor_invalidate_all();
}

static void close_window(uint8_t index)
{
    if (
        index >= GUI_WINDOW_COUNT ||
        !windows[index].state.visible ||
        window_animation_active(index)
    )
    {
        return;
    }

    wm_window_state_t target = windows[index].state;
    target.visible = false;
    target.minimized = false;
    target.maximized = false;
    target.fullscreen = false;
    target.dragging = false;
    target.resizing = false;
    target.snap = WM_SNAP_NONE;
    target.pending_snap = WM_SNAP_NONE;

    const ui_rect_t *bounds = &windows[index].state.bounds;

    ui_rect_t destination = {
        .x = bounds->x + (int32_t)bounds->width / 2 - 36,
        .y = bounds->y + (int32_t)bounds->height / 2 - 24,
        .width = 72U,
        .height = 48U
    };

    if (begin_window_state_animation(
        index,
        WINDOW_ANIMATION_CLOSE,
        &target,
        &destination,
        135U
    ))
    {
        terminal_focused = false;
        return;
    }

    bool was_fullscreen = windows[index].state.fullscreen;
    ui_rect_t old = window_visual_bounds(&windows[index]);
    windows[index].state = target;
    terminal_focused = false;
    drag_cache_valid = false;

    if (was_fullscreen)
    {
        compositor_invalidate_all();
    }
    else
    {
        compositor_invalidate(&old);
        invalidate_visible_windows();
        invalidate_taskbar();
    }
}

static void activate_window(uint8_t index)
{
    if (
        index >= GUI_WINDOW_COUNT ||
        window_animation_active(index)
    )
    {
        return;
    }

    uint8_t previous = active_window_index();
    bool was_visible = windows[index].state.visible;
    bool was_minimized = windows[index].state.minimized;

    bring_window_to_front(index);
    terminal_focused =
        windows[index].app == GUI_APP_TERMINAL;

    if (windows[index].app == GUI_APP_PROCESS_MANAGER)
    {
        process_manager_refresh();
    }

    if (windows[index].app == GUI_APP_INSTALLER)
    {
        windows[index].title = recovery_panel_active ?
            "LatterOS Recovery" : "Install LatterOS";

        if (recovery_panel_active)
        {
            recovery_refresh();
        }
        else
        {
            installer_refresh();

            if (installer_target_count() == 0)
            {
                installer_selected_target = UINT32_MAX;
                copy_text(
                    installer_status,
                    sizeof(installer_status),
                    installer_last_error()
                );
            }
            else if (
                installer_selected_target >=
                    installer_target_count()
            )
            {
                installer_selected_target = 0;
                copy_text(
                    installer_status,
                    sizeof(installer_status),
                    "Select the dedicated installation target"
                );
            }
        }
    }

    close_popup_menu();

    if (force_fresh_activation)
    {
        window_surface_invalidate(index);
    }

    if (
        !force_fresh_activation &&
        (was_minimized || !was_visible) &&
        window_surface_valid_for(
            index,
            windows[index].state.bounds.width,
            windows[index].state.bounds.height
        )
    )
    {
        wm_window_state_t target = windows[index].state;
        target.visible = true;
        target.minimized = false;
        target.dragging = false;
        target.resizing = false;

        ui_rect_t final_bounds = target.bounds;
        ui_rect_t source;
        window_animation_type_t type;

        if (was_minimized)
        {
            source = taskbar_app_button(index);
            type = WINDOW_ANIMATION_RESTORE;
        }
        else
        {
            source = (ui_rect_t){
                .x = final_bounds.x +
                    (int32_t)final_bounds.width / 2 - 44,
                .y = final_bounds.y +
                    (int32_t)final_bounds.height / 2 - 30,
                .width = 88U,
                .height = 60U
            };
            type = WINDOW_ANIMATION_OPEN;
        }

        if (
            animations_enabled &&
            window_animation_start(
                index,
                type,
                &source,
                &final_bounds,
                155U
            )
        )
        {
            animation_targets[index] = target;
            animation_target_valid[index] = true;

            ui_rect_t first = animation_visual_bounds(&source);
            ui_rect_t last = animation_visual_bounds(&final_bounds);
            ui_rect_t damage = ui_union(&first, &last);
            compositor_invalidate(&damage);
            invalidate_taskbar();
            return;
        }
    }

    if (
        previous < GUI_WINDOW_COUNT &&
        previous != index
    )
    {
        window_surface_invalidate(previous);
        ui_rect_t previous_bounds =
            window_visual_bounds(&windows[previous]);
        compositor_invalidate(&previous_bounds);
    }

    windows[index].state.visible = true;
    windows[index].state.minimized = false;
    window_surface_invalidate(index);
    ui_rect_t current_bounds = window_visual_bounds(&windows[index]);
    compositor_invalidate(&current_bounds);
    invalidate_taskbar();
}

static void cycle_windows(bool reverse)
{
    uint8_t current = active_window_index();
    int32_t current_order = -1;

    for (uint8_t order = 0; order < GUI_WINDOW_COUNT; order++)
    {
        if (window_order[order] == current)
        {
            current_order = order;
            break;
        }
    }

    for (uint8_t step = 1; step <= GUI_WINDOW_COUNT; step++)
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

        uint8_t candidate = window_order[order];

        if (!windows[candidate].state.visible)
        {
            continue;
        }

        activate_window(candidate);
        return;
    }
}

static void show_desktop(void)
{
    bool changed = false;

    for (uint8_t index = 0; index < GUI_WINDOW_COUNT; index++)
    {
        if (
            windows[index].state.visible &&
            !windows[index].state.minimized
        )
        {
            windows[index].state.minimized = true;
            wm_cancel_interaction(&windows[index].state);
            changed = true;
        }
    }

    terminal_focused = false;
    drag_cache_valid = false;
    close_popup_menu();

    if (changed)
    {
        compositor_invalidate_all();
    }
}

uint32_t gui_application_count(void)
{
    uint32_t count = 0;

    for (uint32_t index = 0; index < GUI_WINDOW_COUNT; index++)
    {
        if (
            windows[index].state.visible &&
            windows[index].app != GUI_APP_PROCESS_MANAGER
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
    uint8_t active_index = active_window_index();

    for (
        uint32_t window_index = 0;
        window_index < GUI_WINDOW_COUNT;
        window_index++
    )
    {
        gui_window_t *window = &windows[window_index];

        if (
            !window->state.visible ||
            window->app == GUI_APP_PROCESS_MANAGER
        )
        {
            continue;
        }

        if (current == index)
        {
            information->id = window_index;
            information->title = window->title;

            if (window->state.minimized)
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
        !windows[id].state.visible ||
        windows[id].app == GUI_APP_PROCESS_MANAGER
    )
    {
        return false;
    }

    close_window((uint8_t)id);
    return true;
}

static void gui_terminal_new_line(void)
{
    if (terminal_line_count == 0)
    {
        terminal_line_count = 1;
    }
    else if (terminal_line_count < TERMINAL_HISTORY_LINES)
    {
        terminal_line_count++;
    }
    else
    {
        for (
            uint32_t line = 1;
            line < TERMINAL_HISTORY_LINES;
            line++
        )
        {
            copy_text(
                terminal_lines[line - 1U],
                sizeof(terminal_lines[line - 1U]),
                terminal_lines[line]
            );
        }
    }

    clear_text(
        terminal_lines[terminal_line_count - 1U],
        sizeof(terminal_lines[terminal_line_count - 1U])
    );

    terminal_column = 0;
    terminal_scroll_offset = UINT32_MAX;
}

static void gui_terminal_reset_output(void)
{
    for (uint32_t line = 0; line < TERMINAL_HISTORY_LINES; line++)
    {
        clear_text(
            terminal_lines[line],
            sizeof(terminal_lines[line])
        );
    }

    terminal_line_count = 1;
    terminal_column = 0;
    terminal_scroll_offset = 0;
}

static void gui_terminal_append(const char *text)
{
    if (text == NULL)
    {
        return;
    }

    for (uint32_t index = 0; text[index] != '\0'; index++)
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

        if (terminal_column >= TERMINAL_COLUMNS)
        {
            gui_terminal_new_line();
        }

        terminal_lines[
            terminal_line_count - 1U
        ][terminal_column++] = character;

        terminal_lines[
            terminal_line_count - 1U
        ][terminal_column] = '\0';
    }
}

static void gui_terminal_redirect_output(const char *text)
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

    clear_text(terminal_input, sizeof(terminal_input));
    terminal_input_length = 0;
    terminal_clear_selection();
    terminal_scroll_offset = UINT32_MAX;
    invalidate_window(0);
}

static ui_rect_t terminal_output_bounds(
    const gui_window_t *window
)
{
    ui_rect_t content = window_content_bounds(window);

    return (ui_rect_t){
        .x = content.x + 10,
        .y = content.y + 10,
        .width = content.width > 42U ?
            content.width - 42U : 1U,
        .height = content.height > 58U ?
            content.height - 58U : 1U
    };
}

static ui_rect_t terminal_scrollbar_bounds(
    const gui_window_t *window
)
{
    ui_rect_t output = terminal_output_bounds(window);

    return (ui_rect_t){
        .x = output.x + (int32_t)output.width + 4,
        .y = output.y,
        .width = 18,
        .height = output.height
    };
}

static ui_rect_t terminal_input_bounds(
    const gui_window_t *window
)
{
    ui_rect_t content = window_content_bounds(window);

    return (ui_rect_t){
        .x = content.x + 10,
        .y = content.y + (int32_t)content.height - 38,
        .width = content.width > 20U ?
            content.width - 20U : 1U,
        .height = 28
    };
}

static uint32_t terminal_visible_lines(
    const gui_window_t *window
)
{
    ui_rect_t output = terminal_output_bounds(window);
    uint32_t lines =
        output.height > 12U ?
            (output.height - 8U) /
                TERMINAL_LINE_HEIGHT : 1U;

    return lines == 0 ? 1U : lines;
}

static uint32_t terminal_maximum_scroll(
    const gui_window_t *window
)
{
    uint32_t visible = terminal_visible_lines(window);

    return terminal_line_count > visible ?
        terminal_line_count - visible : 0U;
}

static void normalize_terminal_scroll(
    const gui_window_t *window
)
{
    uint32_t maximum = terminal_maximum_scroll(window);

    if (terminal_scroll_offset == UINT32_MAX)
    {
        terminal_scroll_offset = maximum;
    }
    else if (terminal_scroll_offset > maximum)
    {
        terminal_scroll_offset = maximum;
    }
}

static ui_rect_t explorer_toolbar_bounds(
    const gui_window_t *window
)
{
    ui_rect_t content = window_content_bounds(window);

    return (ui_rect_t){
        content.x,
        content.y,
        content.width,
        EXPLORER_TOOLBAR_HEIGHT
    };
}

static ui_rect_t explorer_toolbar_button(
    const gui_window_t *window,
    uint32_t index
)
{
    ui_rect_t toolbar = explorer_toolbar_bounds(window);
    static const uint32_t widths[5] = {
        46U, 54U, 76U, 84U, 86U
    };

    int32_t x = toolbar.x + 6;

    for (uint32_t current = 0; current < index && current < 5U; current++)
    {
        x += (int32_t)widths[current] + 4;
    }

    return (ui_rect_t){
        .x = x,
        .y = toolbar.y + 5,
        .width = index < 5U ? widths[index] : 60U,
        .height = 24
    };
}

static ui_rect_t explorer_path_bounds(
    const gui_window_t *window
)
{
    ui_rect_t content = window_content_bounds(window);

    return (ui_rect_t){
        .x = content.x + 10,
        .y = content.y +
            (int32_t)EXPLORER_TOOLBAR_HEIGHT + 6,
        .width = content.width > 20U ?
            content.width - 20U : 1U,
        .height = 26
    };
}

static ui_rect_t explorer_status_bounds(
    const gui_window_t *window
)
{
    ui_rect_t content = window_content_bounds(window);

    return (ui_rect_t){
        .x = content.x,
        .y = content.y + (int32_t)content.height -
            (int32_t)EXPLORER_STATUS_HEIGHT,
        .width = content.width,
        .height = EXPLORER_STATUS_HEIGHT
    };
}

static ui_rect_t explorer_preview_bounds(
    const gui_window_t *window
)
{
    ui_rect_t status = explorer_status_bounds(window);
    ui_rect_t content = window_content_bounds(window);

    return (ui_rect_t){
        .x = content.x + 10,
        .y = status.y -
            (int32_t)EXPLORER_PREVIEW_HEIGHT - 8,
        .width = content.width > 20U ?
            content.width - 20U : 1U,
        .height = EXPLORER_PREVIEW_HEIGHT
    };
}

static ui_rect_t explorer_list_bounds(
    const gui_window_t *window
)
{
    ui_rect_t path = explorer_path_bounds(window);
    ui_rect_t preview = explorer_preview_bounds(window);

    int32_t top = path.y + (int32_t)path.height + 6;
    int32_t bottom = preview.y - 8;

    return (ui_rect_t){
        .x = path.x,
        .y = top,
        .width = path.width > 24U ?
            path.width - 24U : 1U,
        .height = bottom > top ?
            (uint32_t)(bottom - top) : 1U
    };
}

static ui_rect_t explorer_scrollbar_bounds(
    const gui_window_t *window
)
{
    ui_rect_t list = explorer_list_bounds(window);

    return (ui_rect_t){
        .x = list.x + (int32_t)list.width + 4,
        .y = list.y,
        .width = 18,
        .height = list.height
    };
}

static uint32_t explorer_visible_rows(
    const gui_window_t *window
)
{
    ui_rect_t list = explorer_list_bounds(window);
    uint32_t rows = list.height / EXPLORER_ROW_HEIGHT;
    return rows == 0 ? 1U : rows;
}

static uint32_t explorer_entry_count(void)
{
    if (explorer_directory == NULL)
    {
        return 0;
    }

    uint32_t count =
        explorer_directory->parent != NULL ? 1U : 0U;

    vfs_node_t *node = explorer_directory->first_child;

    while (node != NULL)
    {
        count++;
        node = node->next_sibling;
    }

    return count;
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

    vfs_node_t *node = explorer_directory->first_child;

    while (node != NULL && row > 0)
    {
        node = node->next_sibling;
        row--;
    }

    return node;
}

static uint32_t explorer_maximum_scroll(
    const gui_window_t *window
)
{
    uint32_t count = explorer_entry_count();
    uint32_t visible = explorer_visible_rows(window);

    return count > visible ? count - visible : 0U;
}

static void normalize_explorer_scroll(
    const gui_window_t *window
)
{
    uint32_t maximum = explorer_maximum_scroll(window);

    if (explorer_scroll_offset > maximum)
    {
        explorer_scroll_offset = maximum;
    }
}

static void set_explorer_status(const char *message)
{
    copy_text(
        explorer_status,
        sizeof(explorer_status),
        message == NULL ? "" : message
    );
}

static void refresh_explorer(void)
{
    if (explorer_directory == NULL)
    {
        explorer_directory = vfs_root();
    }

    if (
        explorer_selected != NULL &&
        explorer_selected->parent != explorer_directory
    )
    {
        explorer_selected = NULL;
    }

    set_explorer_status("Ready");
    invalidate_window(1);
}

static void open_associated_node(vfs_node_t *node)
{
    if (node == NULL)
    {
        return;
    }

    if (node->type == VFS_NODE_DIRECTORY)
    {
        explorer_directory = node;
        explorer_selected = NULL;
        explorer_scroll_offset = 0;
        set_explorer_status("Folder opened");
        invalidate_window(1);
        return;
    }

    char path[EXPLORER_PATH_CAPACITY];
    build_node_path(node, path, sizeof(path));
    desktop_association_t association =
        desktop_association_for_path(path);

    if (association == DESKTOP_ASSOCIATION_TEXT)
    {
        desktop_editor_open_node(node);
        activate_window(2);
        desktop_recent_add(path);
        desktop_notify("Opened in Text Editor", 3000U);
        return;
    }

    if (association == DESKTOP_ASSOCIATION_IMAGE)
    {
        activate_window(4);
        desktop_recent_add(path);
        desktop_notify("Opened with Paint association", 3500U);
        return;
    }

    if (association == DESKTOP_ASSOCIATION_EXECUTABLE)
    {
        uint64_t pid = process_create_user_program(path);

        if (pid != 0)
        {
            desktop_recent_add(path);
            desktop_notify("Application started", 3000U);
        }
        else
        {
            desktop_notify("Unable to start application", 4500U);
        }

        return;
    }

    if (association == DESKTOP_ASSOCIATION_PACKAGE)
    {
        show_package_install_dialog(path);
        return;
    }

    desktop_editor_open_node(node);
    activate_window(2);
    desktop_recent_add(path);
    desktop_notify("Unknown type opened as text", 3500U);
}

static void explorer_open_selected(void)
{
    if (explorer_selected == NULL)
    {
        set_explorer_status("Select a file or folder first");
        invalidate_window(1);
        return;
    }

    open_associated_node(explorer_selected);
}

static void open_file_picker(void)
{
    pending_action = PENDING_ACTION_OPEN_FILE;
    pending_node = NULL;

    desktop_dialog_show_open_file(
        &dialog,
        "Open file",
        explorer_directory
    );

    desktop_dialog_layout(
        &dialog,
        graphics_width(),
        graphics_height()
    );

    close_popup_menu();
    compositor_invalidate_all();
}

static vfs_node_t *writable_user_directory(void)
{
    vfs_node_t *directory = vfs_open("/home/user/Documents");

    if (directory == NULL)
    {
        directory = vfs_open("/home/user");
    }

    return directory != NULL ? directory : vfs_root();
}

static void show_editor_save_as_dialog(void)
{
    pending_action = PENDING_ACTION_EDITOR_SAVE_AS;
    pending_node = NULL;

    const char *name = desktop_editor_name();

    if (name == NULL || name[0] == '\0')
    {
        name = "Untitled.txt";
    }

    desktop_dialog_show_save_file(
        &dialog,
        "Save document as",
        writable_user_directory(),
        name
    );

    desktop_dialog_layout(
        &dialog,
        graphics_width(),
        graphics_height()
    );

    close_popup_menu();
    compositor_invalidate_all();
}

static void show_installer_confirmation(void)
{
    installer_target_info_t target;

    if (
        installer_selected_target == UINT32_MAX ||
        !installer_target_get(
            installer_selected_target,
            &target
        )
    )
    {
        copy_text(
            installer_status,
            sizeof(installer_status),
            "Select a valid target disk first"
        );
        invalidate_window(7);
        return;
    }

    pending_action = PENDING_ACTION_INSTALL_PREPARE;
    pending_node = NULL;

    char message[256];
    clear_text(message, sizeof(message));
    append_text(message, sizeof(message), "ERASE ALL DATA on ");
    append_text(message, sizeof(message), target.name);
    append_text(message, sizeof(message), " (");
    append_unsigned_decimal(
        message,
        sizeof(message),
        target.capacity_mib
    );
    append_text(
        message,
        sizeof(message),
        " MiB)? LatterOS will write GPT, format EFI and system FAT32 partitions, install Limine UEFI, copy the kernel, and verify every boot file."
    );

    desktop_dialog_show_confirm(
        &dialog,
        recovery_panel_active ?
            "Reinstall LatterOS" : "Install LatterOS",
        message
    );

    desktop_dialog_layout(
        &dialog,
        graphics_width(),
        graphics_height()
    );

    close_popup_menu();
    compositor_invalidate_all();
}

static void show_new_file_dialog(void)
{
    pending_action = PENDING_ACTION_NEW_FILE;
    pending_node = NULL;

    desktop_dialog_show_save_file(
        &dialog,
        "Create file",
        explorer_directory,
        "New File.txt"
    );

    desktop_dialog_layout(
        &dialog,
        graphics_width(),
        graphics_height()
    );

    close_popup_menu();
    compositor_invalidate_all();
}

static void show_new_folder_dialog(void)
{
    pending_action = PENDING_ACTION_NEW_FOLDER;
    pending_node = NULL;

    desktop_dialog_show_save_file(
        &dialog,
        "Create folder",
        explorer_directory,
        "New Folder"
    );

    desktop_dialog_layout(
        &dialog,
        graphics_width(),
        graphics_height()
    );

    close_popup_menu();
    compositor_invalidate_all();
}

static void show_rename_dialog(void)
{
    if (explorer_selected == NULL)
    {
        set_explorer_status("Select an item to rename");
        invalidate_window(1);
        return;
    }

    pending_action = PENDING_ACTION_RENAME;
    pending_node = explorer_selected;
    build_node_path(
        explorer_selected,
        pending_path,
        sizeof(pending_path)
    );

    desktop_dialog_show_save_file(
        &dialog,
        "Rename item",
        explorer_directory,
        explorer_selected->name
    );

    desktop_dialog_layout(
        &dialog,
        graphics_width(),
        graphics_height()
    );

    close_popup_menu();
    compositor_invalidate_all();
}

static void show_delete_dialog(void)
{
    if (explorer_selected == NULL)
    {
        set_explorer_status("Select an item to delete");
        invalidate_window(1);
        return;
    }

    pending_action = PENDING_ACTION_DELETE;
    pending_node = explorer_selected;
    build_node_path(
        explorer_selected,
        pending_path,
        sizeof(pending_path)
    );

    char message[256];
    copy_text(message, sizeof(message), "Delete ");
    append_text(message, sizeof(message), explorer_selected->name);
    append_text(
        message,
        sizeof(message),
        "? Directories are removed recursively."
    );

    desktop_dialog_show_confirm(
        &dialog,
        "Confirm delete",
        message
    );

    desktop_dialog_layout(
        &dialog,
        graphics_width(),
        graphics_height()
    );

    close_popup_menu();
    compositor_invalidate_all();
}

static void show_properties_dialog(void)
{
    if (explorer_selected == NULL)
    {
        set_explorer_status("Select an item to inspect");
        invalidate_window(1);
        return;
    }

    pending_action = PENDING_ACTION_NONE;
    pending_node = explorer_selected;
    desktop_dialog_show_properties(&dialog, explorer_selected);

    desktop_dialog_layout(
        &dialog,
        graphics_width(),
        graphics_height()
    );

    close_popup_menu();
    compositor_invalidate_all();
}

static void show_color_dialog(void)
{
    pending_action = PENDING_ACTION_COLOR;
    pending_node = NULL;

    desktop_dialog_show_color(
        &dialog,
        "Window accent color",
        chrome_accent
    );

    desktop_dialog_layout(
        &dialog,
        graphics_width(),
        graphics_height()
    );

    close_popup_menu();
    compositor_invalidate_all();
}

static void show_help_dialog(void)
{
    pending_action = PENDING_ACTION_NONE;

    desktop_dialog_show_message(
        &dialog,
        "LatterOS Desktop Help",
        "Drag a title bar to move a window. Drag any border or corner to resize. Drag to the top to maximize, to a side for half-screen snapping, or to a corner for quarter-screen snapping. F11 toggles fullscreen. F2 renames a selected file. F5 refreshes Files. Right-click in Files for its context menu."
    );

    desktop_dialog_layout(
        &dialog,
        graphics_width(),
        graphics_height()
    );

    close_popup_menu();
    compositor_invalidate_all();
}

static void show_about_dialog(void)
{
    pending_action = PENDING_ACTION_NONE;

    char message[256];
    message[0] = '\0';
    append_text(message, sizeof(message), release_info_name());
    append_text(message, sizeof(message), " ");
    append_text(message, sizeof(message), release_info_version());
    append_text(message, sizeof(message), " | Milestone ");
    append_text(message, sizeof(message), release_info_milestone());
    append_text(message, sizeof(message), " | ");
    append_text(message, sizeof(message), release_info_architecture());
    append_text(message, sizeof(message), ". Bootable installer, Safe Mode, Recovery, LDS theme, versioned release images, and LPKG package management. Open .lpkg files in Files and manage installed packages in Settings.");

    desktop_dialog_show_message(
        &dialog,
        "About LatterOS",
        message
    );

    desktop_dialog_layout(
        &dialog,
        graphics_width(),
        graphics_height()
    );

    close_popup_menu();
    compositor_invalidate_all();
}

static void show_package_install_dialog(const char *path)
{
    package_info_t information;
    package_result_t result = package_manager_probe(path, &information);

    if (result != PACKAGE_RESULT_OK)
    {
        desktop_notify(package_result_message(result), 6500U);
        return;
    }

    pending_action = PENDING_ACTION_PACKAGE_INSTALL;
    pending_node = NULL;
    copy_text(pending_path, sizeof(pending_path), path);

    char message[256];
    message[0] = '\0';
    append_text(message, sizeof(message), information.installed ?
        "Update " : "Install ");
    append_text(message, sizeof(message), information.name);
    append_text(message, sizeof(message), " ");
    append_text(message, sizeof(message), information.version);
    append_text(message, sizeof(message), "? ");
    append_text(message, sizeof(message), information.description);

    if (information.depends[0] != '\0')
    {
        append_text(message, sizeof(message), " Dependencies: ");
        append_text(message, sizeof(message), information.depends);
    }

    desktop_dialog_show_confirm(
        &dialog,
        information.installed ? "Update package" : "Install package",
        message
    );

    desktop_dialog_layout(
        &dialog,
        graphics_width(),
        graphics_height()
    );

    close_popup_menu();
    compositor_invalidate_all();
}

static void show_package_remove_dialog(const char *name)
{
    if (name == NULL || name[0] == '\0')
    {
        return;
    }

    pending_action = PENDING_ACTION_PACKAGE_REMOVE;
    pending_node = NULL;
    copy_text(pending_path, sizeof(pending_path), name);

    char message[192];
    message[0] = '\0';
    append_text(message, sizeof(message), "Remove installed package ");
    append_text(message, sizeof(message), name);
    append_text(message, sizeof(message), " and its application files?");

    desktop_dialog_show_confirm(
        &dialog,
        "Remove package",
        message
    );

    desktop_dialog_layout(
        &dialog,
        graphics_width(),
        graphics_height()
    );

    close_popup_menu();
    compositor_invalidate_all();
}

static void handle_dialog_result(void)
{
    desktop_dialog_result_t result;

    if (!desktop_dialog_take_result(&dialog, &result))
    {
        return;
    }

    if (result != DESKTOP_DIALOG_RESULT_ACCEPTED)
    {
        pending_action = PENDING_ACTION_NONE;
        pending_node = NULL;
        compositor_invalidate_all();
        return;
    }

    const char *selected_path =
        desktop_dialog_selected_path(&dialog);

    vfs_node_t *selected_node =
        desktop_dialog_selected_node(&dialog);

    bool success = true;

    switch (pending_action)
    {
        case PENDING_ACTION_OPEN_FILE:
            if (
                selected_node == NULL ||
                selected_node->type != VFS_NODE_FILE
            )
            {
                success = false;
            }
            else
            {
                open_associated_node(selected_node);
            }
            break;

        case PENDING_ACTION_NEW_FILE:
            success =
                selected_path != NULL &&
                vfs_write_text(selected_path, "");

            set_explorer_status(
                success ? "File created" :
                    "Unable to create file"
            );
            break;

        case PENDING_ACTION_NEW_FOLDER:
            success =
                selected_path != NULL &&
                vfs_make_directory(selected_path);

            set_explorer_status(
                success ? "Folder created" :
                    "Unable to create folder"
            );
            break;

        case PENDING_ACTION_RENAME:
            success =
                pending_node != NULL &&
                selected_path != NULL &&
                vfs_rename(
                    pending_path,
                    path_basename(selected_path)
                );

            if (success)
            {
                explorer_selected = pending_node;
            }
            else
            {
                explorer_selected = NULL;
            }

            set_explorer_status(
                success ? "Item renamed" :
                    "Unable to rename item"
            );
            break;

        case PENDING_ACTION_DELETE:
            success =
                pending_path[0] != '\0' &&
                vfs_remove(pending_path, true);

            explorer_selected = NULL;
            set_explorer_status(
                success ? "Item deleted" :
                    "Unable to delete item"
            );
            break;

        case PENDING_ACTION_COLOR:
            chrome_accent =
                desktop_dialog_selected_color(&dialog);
            success = true;
            break;

        case PENDING_ACTION_EDITOR_SAVE_AS:
            success =
                selected_path != NULL &&
                desktop_editor_save_as(selected_path);

            if (success)
            {
                window_surface_invalidate(2);
            }
            break;

        case PENDING_ACTION_INSTALL_PREPARE:
            recovery_panel_active = false;
            windows[7].title = "Install LatterOS";
            success = installer_start_target(
                installer_selected_target
            );

            copy_text(
                installer_status,
                sizeof(installer_status),
                success ?
                    "[1%] Starting background installation" :
                    installer_last_error()
            );

            desktop_notify(
                success ?
                    "Installation started - the desktop remains responsive" :
                    "Unable to start LatterOS installation",
                success ? 4000U : 7000U
            );
            installer_visible_progress = success ? 1U : 0U;
            window_surface_invalidate(7);
            break;

        case PENDING_ACTION_PACKAGE_INSTALL:
        {
            package_info_t information;
            package_result_t package_result =
                package_manager_install(
                    pending_path,
                    &information
                );

            success = package_result == PACKAGE_RESULT_OK;

            if (success)
            {
                char notification[96];
                notification[0] = '\0';
                append_text(notification, sizeof(notification), "Installed ");
                append_text(notification, sizeof(notification), information.name);
                append_text(notification, sizeof(notification), " ");
                append_text(notification, sizeof(notification), information.version);
                desktop_notify(notification, 4500U);
                desktop_recent_add(pending_path);
            }
            else
            {
                desktop_notify(
                    package_result_message(package_result),
                    7000U
                );
            }

            settings_page = SETTINGS_PAGE_PACKAGES;
            window_surface_invalidate(5);
            break;
        }

        case PENDING_ACTION_PACKAGE_REMOVE:
        {
            package_result_t package_result =
                package_manager_remove(pending_path);
            success = package_result == PACKAGE_RESULT_OK;

            desktop_notify(
                success ? "Package removed" :
                    package_result_message(package_result),
                success ? 4000U : 7000U
            );

            settings_page = SETTINGS_PAGE_PACKAGES;
            window_surface_invalidate(5);
            break;
        }

        case PENDING_ACTION_NONE:
        default:
            break;
    }

    pending_action = PENDING_ACTION_NONE;
    pending_node = NULL;
    pending_path[0] = '\0';
    refresh_explorer();
    compositor_invalidate_all();
}

static void open_context_menu(int32_t x, int32_t y)
{
    bool item_selected = explorer_selected != NULL;

    context_items[0].enabled = item_selected;
    context_items[1].enabled = item_selected;
    context_items[2].enabled = item_selected;
    context_items[3].enabled = item_selected;
    context_items[5].enabled = item_selected;
    context_items[6].enabled = item_selected;
    context_items[7].enabled = clipboard_file_path[0] != '\0';

    ui_menu_open(
        &popup_menu,
        x,
        y,
        CONTEXT_MENU_WIDTH,
        context_items,
        sizeof(context_items) /
            sizeof(context_items[0]),
        graphics_width(),
        graphics_height() - TASKBAR_HEIGHT
    );

    popup_is_launcher = false;
    popup_is_system = false;
    popup_is_desktop = false;
    compositor_invalidate_all();
}

static void open_launcher_menu(void)
{
    rebuild_launcher_items();
    ui_rect_t start = start_button_bounds();
    uint32_t estimated_height =
        8U + launcher_dynamic_count * 24U;

    ui_menu_open(
        &popup_menu,
        start.x,
        start.y - (int32_t)estimated_height - 4,
        LAUNCHER_MENU_WIDTH + 70U,
        launcher_dynamic_items,
        launcher_dynamic_count,
        graphics_width(),
        graphics_height() - TASKBAR_HEIGHT
    );

    popup_is_launcher = true;
    popup_is_system = false;
    popup_is_desktop = false;
    compositor_invalidate_all();
}

static void open_system_menu(void)
{
    ui_rect_t system = system_button_bounds();
    uint32_t estimated_height =
        8U + (uint32_t)(sizeof(system_items) /
            sizeof(system_items[0])) * 24U + 8U;

    ui_menu_open(
        &popup_menu,
        system.x + (int32_t)system.width -
            (int32_t)SYSTEM_MENU_WIDTH,
        system.y - (int32_t)estimated_height - 4,
        SYSTEM_MENU_WIDTH,
        system_items,
        sizeof(system_items) /
            sizeof(system_items[0]),
        graphics_width(),
        graphics_height() - TASKBAR_HEIGHT
    );

    popup_is_launcher = false;
    popup_is_system = true;
    popup_is_desktop = false;
    compositor_invalidate_all();
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

        for (uint8_t index = 0; index < 6U; index++)
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

static uint32_t blend_color(
    uint32_t first,
    uint32_t second,
    uint32_t numerator,
    uint32_t denominator
)
{
    if (denominator == 0)
    {
        return first;
    }

    uint32_t inverse = denominator - numerator;
    uint32_t red =
        (((first >> 16) & 0xFFU) * inverse +
        ((second >> 16) & 0xFFU) * numerator) /
            denominator;
    uint32_t green =
        (((first >> 8) & 0xFFU) * inverse +
        ((second >> 8) & 0xFFU) * numerator) /
            denominator;
    uint32_t blue =
        ((first & 0xFFU) * inverse +
        (second & 0xFFU) * numerator) /
            denominator;

    return (red << 16) | (green << 8) | blue;
}

static void render_theme_mark(void)
{
    if (desktop_services_theme_kind() != DESKTOP_THEME_LDS)
    {
        return;
    }

    const gui_theme_t *theme = desktop_services_theme();
    uint32_t width = graphics_width();
    uint32_t height = graphics_height() - TASKBAR_HEIGHT;
    const char *mark = "LDS";
    uint32_t scale = 6U;
    uint32_t mark_width = desktop_font_text_width(mark, scale);
    uint32_t mark_height = desktop_font_text_height(scale);
    int32_t x = width > mark_width + 34U ?
        (int32_t)(width - mark_width - 34U) : 16;
    int32_t y = height > mark_height + 30U ?
        (int32_t)(height - mark_height - 30U) : 16;
    uint32_t color = blend_color(
        theme->desktop,
        theme->accent,
        2U,
        5U
    );

    desktop_font_draw_text(mark, x, y, scale, color);
}

static void render_wallpaper(void)
{
    const gui_theme_t *theme = desktop_services_theme();
    desktop_wallpaper_t wallpaper =
        desktop_services_wallpaper();
    uint32_t width = graphics_width();
    uint32_t height = graphics_height() - TASKBAR_HEIGHT;

    if (wallpaper == DESKTOP_WALLPAPER_SOLID)
    {
        graphics_clear(theme->desktop);
        render_theme_mark();
        return;
    }

    if (wallpaper == DESKTOP_WALLPAPER_GRADIENT)
    {
        uint32_t bottom = blend_color(
            theme->desktop,
            0x081018U,
            2U,
            5U
        );
        uint32_t bands = 32U;

        for (uint32_t band = 0; band < bands; band++)
        {
            uint32_t top_y = height * band / bands;
            uint32_t bottom_y = height * (band + 1U) / bands;
            uint32_t color = blend_color(
                theme->desktop,
                bottom,
                band,
                bands - 1U
            );

            draw_rectangle(
                0,
                top_y,
                width,
                bottom_y - top_y,
                color
            );
        }

        render_theme_mark();
        return;
    }

    if (wallpaper == DESKTOP_WALLPAPER_GRID)
    {
        graphics_clear(theme->desktop);
        uint32_t grid = blend_color(
            theme->desktop,
            theme->light_text,
            1U,
            8U
        );

        for (uint32_t x = 0; x < width; x += 48U)
        {
            draw_rectangle(x, 0, 1, height, grid);
        }

        for (uint32_t y = 0; y < height; y += 48U)
        {
            draw_rectangle(0, y, width, 1, grid);
        }

        render_theme_mark();
        return;
    }

    graphics_clear(0x08111FU);

    for (uint32_t index = 0; index < 96U; index++)
    {
        uint32_t x =
            (index * 97U + index * index * 13U) %
                (width == 0 ? 1U : width);
        uint32_t y =
            (index * 53U + index * index * 7U) %
                (height == 0 ? 1U : height);
        uint32_t size = index % 11U == 0 ? 2U : 1U;

        draw_rectangle(x, y, size, size, 0xDDEBFFU);
    }

    render_theme_mark();
}

static const char *desktop_icon_label(uint32_t index)
{
    if (index == DESKTOP_ICON_COUNT - 1U && boot_mode_is_recovery())
    {
        return "Recovery";
    }

    return index < DESKTOP_ICON_COUNT ?
        desktop_icons[index].label : "";
}

static void render_desktop_icons(void)
{
    ui_palette_t palette = current_palette();

    for (uint32_t index = 0; index < DESKTOP_ICON_COUNT; index++)
    {
        ui_rect_t bounds = desktop_icon_bounds(index);
        bool selected = desktop_selected_icon == index;
        bool hovered = desktop_hovered_icon == index;

        if (selected || hovered)
        {
            ui_fill_rect(
                &bounds,
                selected ? palette.selected :
                    blend_color(
                        palette.background,
                        palette.light_text,
                        1U,
                        5U
                    )
            );
            ui_draw_border(&bounds, palette.accent, 1);
        }

        ui_rect_t symbol = {
            .x = bounds.x + 29,
            .y = bounds.y + 7,
            .width = 34,
            .height = 34
        };

        ui_fill_rect(&symbol, desktop_icons[index].symbol_color);
        ui_draw_border(&symbol, palette.light_text, 2);

        const char *label = desktop_icon_label(index);

        char symbol_text[2] = {
            label[0],
            '\0'
        };

        desktop_font_draw_text(
            symbol_text,
            symbol.x + 11,
            symbol.y + 10,
            2,
            palette.light_text
        );

        uint32_t label_width = desktop_font_text_width(
            label,
            1
        );
        int32_t label_x = bounds.x;

        if (bounds.width > label_width)
        {
            label_x += (int32_t)(
                (bounds.width - label_width) / 2U
            );
        }

        desktop_font_draw_text(
            label,
            label_x,
            bounds.y + 51,
            1,
            palette.light_text
        );
    }
}

static ui_rect_t notification_bounds(uint32_t index)
{
    return (ui_rect_t){
        .x = (int32_t)graphics_width() -
            (int32_t)NOTIFICATION_WIDTH - 14,
        .y = (int32_t)graphics_height() -
            (int32_t)TASKBAR_HEIGHT - 14 -
            (int32_t)(index + 1U) *
                (int32_t)(NOTIFICATION_HEIGHT + NOTIFICATION_GAP),
        .width = NOTIFICATION_WIDTH,
        .height = NOTIFICATION_HEIGHT
    };
}

static void render_notifications(void)
{
    ui_palette_t palette = current_palette();
    uint32_t count = desktop_notification_count();

    for (uint32_t index = 0; index < count; index++)
    {
        const desktop_notification_t *notification =
            desktop_notification_get(index);

        if (notification == NULL)
        {
            continue;
        }

        ui_rect_t bounds = notification_bounds(index);
        ui_fill_rect(&bounds, palette.panel);
        ui_draw_border(&bounds, palette.accent, 2);

        desktop_font_draw_text(
            "LATTEROS",
            bounds.x + 10,
            bounds.y + 8,
            1,
            palette.accent
        );

        ui_draw_text_ellipsized(
            notification->text,
            &(ui_rect_t){
                bounds.x + 8,
                bounds.y + 24,
                bounds.width - 16U,
                20
            },
            2,
            palette.text
        );
    }
}

static void render_file_drag(void)
{
    if (!explorer_drag_active || explorer_drag_node == NULL)
    {
        return;
    }

    ui_palette_t palette = current_palette();
    ui_rect_t badge = {
        .x = cursor_x + 14,
        .y = cursor_y + 14,
        .width = 190,
        .height = 30
    };

    ui_fill_rect(&badge, palette.panel);
    ui_draw_border(&badge, palette.accent, 2);
    ui_draw_text_ellipsized(
        explorer_drag_node->name,
        &badge,
        7,
        palette.text
    );
}

static void render_desktop(void)
{
    ui_palette_t palette = current_palette();
    render_wallpaper();

    desktop_font_draw_text(
        "LATTEROS",
        22,
        20,
        2,
        palette.light_text
    );

    const char *desktop_status =
        boot_mode_is_recovery() ?
            "Recovery mode - F1 help" :
        boot_mode_is_safe() ?
            "Safe mode - F1 help" :
            "Desktop platform - F1 help";

    ui_draw_text(
        desktop_status,
        22,
        42,
        palette.light_text
    );

    render_desktop_icons();
}

static void render_taskbar(void)
{
    const gui_theme_t *theme = desktop_services_theme();
    ui_palette_t palette = current_palette();
    ui_rect_t taskbar = taskbar_bounds();

    ui_fill_rect(&taskbar, theme->taskbar);
    ui_draw_horizontal_line(
        taskbar.x,
        taskbar.y,
        taskbar.width,
        2,
        theme->taskbar_top
    );

    ui_rect_t start = start_button_bounds();
    ui_control_draw_button(
        &start,
        "LatterOS",
        &palette,
        popup_is_launcher && popup_menu.visible ?
            UI_CONTROL_PRESSED : UI_CONTROL_NORMAL
    );

    for (uint8_t index = 0; index < GUI_WINDOW_COUNT; index++)
    {
        ui_rect_t button = taskbar_app_button(index);
        ui_control_state_t state = UI_CONTROL_DISABLED;

        if (windows[index].state.visible)
        {
            if (windows[index].state.minimized)
            {
                state = UI_CONTROL_HOVERED;
            }
            else if (window_is_front(index))
            {
                state = UI_CONTROL_PRESSED;
            }
            else
            {
                state = UI_CONTROL_NORMAL;
            }
        }

        ui_control_draw_button(
            &button,
            app_label(index),
            &palette,
            state
        );
    }

    ui_draw_text(
        security_current_username(),
        (int32_t)graphics_width() - 350,
        taskbar.y + 14,
        palette.light_text
    );

    ui_rect_t notification_tray = notification_tray_bounds();
    ui_control_draw_button(
        &notification_tray,
        desktop_notification_count() == 0 ? "N" : "N+",
        &palette,
        desktop_notification_count() == 0 ?
            UI_CONTROL_NORMAL : UI_CONTROL_PRESSED
    );

    ui_rect_t clipboard_tray = clipboard_tray_bounds();
    ui_control_draw_button(
        &clipboard_tray,
        desktop_clipboard_has_text() ? "CB" : "--",
        &palette,
        desktop_clipboard_has_text() ?
            UI_CONTROL_PRESSED : UI_CONTROL_DISABLED
    );

    ui_draw_text(
        clock_text,
        (int32_t)graphics_width() - 190,
        taskbar.y + 14,
        palette.light_text
    );

    ui_rect_t system = system_button_bounds();
    ui_control_draw_button(
        &system,
        "System",
        &palette,
        popup_is_system && popup_menu.visible ?
            UI_CONTROL_PRESSED : UI_CONTROL_NORMAL
    );
}

static void render_snap_preview(void)
{
    for (uint8_t index = 0; index < GUI_WINDOW_COUNT; index++)
    {
        wm_snap_t snap = windows[index].state.pending_snap;

        if (
            !windows[index].state.dragging ||
            snap == WM_SNAP_NONE
        )
        {
            continue;
        }

        ui_rect_t preview = wm_snap_preview(
            snap,
            graphics_width(),
            graphics_height(),
            TASKBAR_HEIGHT
        );

        visual_effects_draw_snap_preview(
            &preview,
            COLOR_SNAP_PREVIEW
        );
        return;
    }
}

static void render_window_shadow(
    const gui_window_t *window
)
{
    if (
        window == NULL ||
        window->state.fullscreen ||
        window->state.maximized
    )
    {
        return;
    }

    visual_effects_draw_shadow(
        &window->state.bounds,
        COLOR_SHADOW
    );
}

static void render_window_frame(
    uint8_t index,
    const gui_window_t *window
)
{
    if (window->state.fullscreen)
    {
        return;
    }

    ui_palette_t palette = current_palette();
    uint32_t radius = window->state.maximized ? 0U : 7U;

    visual_effects_draw_rounded_border(
        &window->state.bounds,
        radius,
        2U,
        palette.border,
        palette.panel
    );

    ui_rect_t title = window_title_bar(window);
    title.x += 2;
    title.y += 2;
    title.width = title.width > 4U ? title.width - 4U : 1U;
    title.height = title.height > 2U ? title.height - 2U : 1U;

    visual_effects_fill_top_rounded_rect(
        &title,
        radius > 2U ? radius - 2U : 0U,
        window_is_front(index) ?
            palette.accent : desktop_services_theme()->title_idle
    );

    ui_draw_text_ellipsized(
        window->title,
        &title,
        8,
        palette.light_text
    );

    ui_rect_t minimize = window_minimize_button(window);
    ui_fill_rect(&minimize, desktop_services_theme()->title_idle);
    ui_draw_border(&minimize, palette.border, 1);
    ui_draw_horizontal_line(
        minimize.x + 4,
        minimize.y + 12,
        10,
        2,
        palette.light_text
    );

    ui_rect_t maximize = window_maximize_button(window);
    ui_fill_rect(&maximize, desktop_services_theme()->title_idle);
    ui_draw_border(&maximize, palette.border, 1);

    ui_rect_t maximize_box = {
        maximize.x + 4,
        maximize.y + 4,
        10,
        10
    };

    ui_draw_border(&maximize_box, palette.light_text, 1);

    ui_rect_t close = window_close_button(window);
    ui_fill_rect(&close, palette.danger);
    ui_draw_border(&close, palette.border, 1);
    ui_draw_text_centered("X", &close, palette.light_text);

    if (
        !window->state.maximized &&
        window->state.snap == WM_SNAP_NONE
    )
    {
        int32_t right =
            window->state.bounds.x +
            (int32_t)window->state.bounds.width;

        int32_t bottom =
            window->state.bounds.y +
            (int32_t)window->state.bounds.height;

        ui_draw_horizontal_line(
            right - 12,
            bottom - 4,
            8,
            1,
            palette.border
        );

        ui_draw_horizontal_line(
            right - 8,
            bottom - 7,
            4,
            1,
            palette.border
        );
    }
}

static void render_terminal_app(
    const gui_window_t *window
)
{
    ui_palette_t palette = current_palette();
    ui_rect_t output = terminal_output_bounds(window);
    ui_rect_t scroll = terminal_scrollbar_bounds(window);
    ui_rect_t input = terminal_input_bounds(window);

    normalize_terminal_scroll(window);

    ui_fill_rect(&output, COLOR_TERMINAL);
    ui_draw_border(&output, palette.border, 1);

    uint32_t visible = terminal_visible_lines(window);
    uint32_t first = terminal_scroll_offset;

    for (uint32_t row = 0; row < visible; row++)
    {
        uint32_t line = first + row;

        if (line >= terminal_line_count)
        {
            break;
        }

        ui_draw_text(
            terminal_lines[line],
            output.x + 5,
            output.y + 5 +
                (int32_t)row *
                    (int32_t)TERMINAL_LINE_HEIGHT,
            COLOR_TERMINAL_TEXT
        );
    }

    ui_scrollbar_t scrollbar = {
        .value = terminal_scroll_offset,
        .page_size = visible,
        .maximum = terminal_maximum_scroll(window)
    };

    ui_control_draw_scrollbar_vertical(
        &scroll,
        &scrollbar,
        &palette,
        UI_CONTROL_NORMAL
    );

    char displayed_input[TERMINAL_INPUT_CAPACITY + 3U];
    copy_text(
        displayed_input,
        sizeof(displayed_input),
        "> "
    );
    append_text(
        displayed_input,
        sizeof(displayed_input),
        terminal_input
    );

    ui_control_draw_text_input(
        &input,
        displayed_input,
        "> Enter a shell command",
        terminal_input_length + 2U,
        &palette,
        terminal_focused ?
            UI_CONTROL_FOCUSED : UI_CONTROL_NORMAL
    );

    if (terminal_has_selection())
    {
        uint32_t start;
        uint32_t end;
        terminal_selection_bounds(&start, &end);

        ui_rect_t selection = {
            .x = input.x + 6 +
                (int32_t)(start + 2U) * 8,
            .y = input.y + 5,
            .width = (end - start) * 8U,
            .height = input.height > 10U ?
                input.height - 10U : 1U
        };

        ui_fill_rect(&selection, palette.selected);

        char selected[TERMINAL_INPUT_CAPACITY];
        uint32_t count = end - start;

        for (uint32_t index = 0; index < count; index++)
        {
            selected[index] = terminal_input[start + index];
        }

        selected[count] = '\0';
        ui_draw_text(
            selected,
            selection.x,
            input.y +
                (int32_t)((input.height - ui_text_height()) / 2U),
            palette.text
        );
    }
}

static void render_explorer_preview(
    const gui_window_t *window
)
{
    ui_palette_t palette = current_palette();
    ui_rect_t preview = explorer_preview_bounds(window);
    ui_fill_rect(&preview, palette.field);
    ui_draw_border(&preview, palette.border, 1);

    if (explorer_selected == NULL)
    {
        ui_draw_text(
            "Select a file to preview it.",
            preview.x + 7,
            preview.y + 10,
            palette.disabled
        );
        return;
    }

    ui_draw_text_ellipsized(
        explorer_selected->name,
        &(ui_rect_t){
            preview.x + 6,
            preview.y + 4,
            preview.width - 12U,
            20
        },
        0,
        palette.text
    );

    if (explorer_selected->type == VFS_NODE_DIRECTORY)
    {
        ui_draw_text(
            "Directory",
            preview.x + 7,
            preview.y + 30,
            palette.text
        );
        return;
    }

    char buffer[256];
    size_t count = vfs_read(
        explorer_selected,
        0,
        buffer,
        sizeof(buffer) - 1U
    );

    buffer[count] = '\0';
    uint32_t row = 0;
    uint32_t column = 0;
    char line[64];

    for (size_t index = 0; index <= count && row < 3U; index++)
    {
        char character = buffer[index];
        bool finish = character == '\0';

        if (
            finish ||
            character == '\n' ||
            column >= 58U
        )
        {
            line[column] = '\0';
            ui_draw_text(
                line,
                preview.x + 7,
                preview.y + 28 +
                    (int32_t)row * 14,
                palette.text
            );

            row++;
            column = 0;

            if (finish)
            {
                break;
            }

            if (character == '\n')
            {
                continue;
            }
        }

        if (character == '\r')
        {
            continue;
        }

        if (character < 32 || character > 126)
        {
            character = '.';
        }

        if (column + 1U < sizeof(line))
        {
            line[column++] = character;
        }
    }
}

static void render_explorer_app(
    const gui_window_t *window
)
{
    ui_palette_t palette = current_palette();
    ui_rect_t toolbar = explorer_toolbar_bounds(window);
    ui_rect_t path = explorer_path_bounds(window);
    ui_rect_t list = explorer_list_bounds(window);
    ui_rect_t scroll = explorer_scrollbar_bounds(window);
    ui_rect_t status = explorer_status_bounds(window);

    ui_control_draw_toolbar(&toolbar, &palette);

    static const char *button_labels[5] = {
        "Up", "Open", "New file", "New folder", "Properties"
    };

    for (uint32_t index = 0; index < 5U; index++)
    {
        ui_rect_t button = explorer_toolbar_button(window, index);
        bool enabled = true;

        if (index == 0)
        {
            enabled =
                explorer_directory != NULL &&
                explorer_directory->parent != NULL;
        }
        else if (index == 1 || index == 4)
        {
            enabled = explorer_selected != NULL;
        }

        ui_control_draw_button(
            &button,
            button_labels[index],
            &palette,
            enabled ?
                UI_CONTROL_NORMAL : UI_CONTROL_DISABLED
        );
    }

    char current_path[EXPLORER_PATH_CAPACITY];
    build_node_path(
        explorer_directory,
        current_path,
        sizeof(current_path)
    );

    ui_control_draw_text_input(
        &path,
        current_path,
        "Root",
        0,
        &palette,
        UI_CONTROL_DISABLED
    );

    ui_fill_rect(&list, palette.field);
    ui_draw_border(&list, palette.border, 1);

    normalize_explorer_scroll(window);
    uint32_t visible = explorer_visible_rows(window);

    for (uint32_t row = 0; row < visible; row++)
    {
        uint32_t entry = explorer_scroll_offset + row;
        bool parent_entry = false;
        vfs_node_t *node = explorer_entry_at(
            entry,
            &parent_entry
        );

        if (node == NULL)
        {
            break;
        }

        ui_rect_t row_rect = {
            list.x + 2,
            list.y + 2 +
                (int32_t)row *
                    (int32_t)EXPLORER_ROW_HEIGHT,
            list.width - 4U,
            EXPLORER_ROW_HEIGHT
        };

        ui_control_draw_list_row(
            &row_rect,
            parent_entry ? "[UP]" :
                (node->type == VFS_NODE_DIRECTORY ?
                    "[D]" : "[F]"),
            parent_entry ? ".." : node->name,
            &palette,
            !parent_entry && node == explorer_selected,
            row == explorer_hovered_row
        );
    }

    ui_scrollbar_t scrollbar = {
        .value = explorer_scroll_offset,
        .page_size = visible,
        .maximum = explorer_maximum_scroll(window)
    };

    ui_control_draw_scrollbar_vertical(
        &scroll,
        &scrollbar,
        &palette,
        UI_CONTROL_NORMAL
    );

    render_explorer_preview(window);

    char right_status[32];
    clear_text(right_status, sizeof(right_status));

    if (explorer_selected != NULL)
    {
        copy_text(
            right_status,
            sizeof(right_status),
            explorer_selected->type == VFS_NODE_DIRECTORY ?
                "Directory" : "File"
        );
    }

    ui_control_draw_statusbar(
        &status,
        explorer_status,
        right_status,
        &palette
    );
}

static ui_rect_t installer_refresh_bounds(
    const ui_rect_t *content
)
{
    return (ui_rect_t){
        content->x + 12,
        content->y + 10,
        92,
        26
    };
}

static ui_rect_t installer_prepare_bounds(
    const ui_rect_t *content
)
{
    return (ui_rect_t){
        content->x + 112,
        content->y + 10,
        142,
        26
    };
}

static ui_rect_t installer_list_bounds(
    const ui_rect_t *content
)
{
    return (ui_rect_t){
        content->x + 12,
        content->y + 68,
        content->width > 24U ? content->width - 24U : 1U,
        content->height > 112U ? content->height - 112U : 1U
    };
}

static ui_rect_t installer_status_bounds(
    const ui_rect_t *content
)
{
    return (ui_rect_t){
        content->x + 12,
        content->y + (int32_t)content->height - 34,
        content->width > 24U ? content->width - 24U : 1U,
        24
    };
}

static void installer_target_label(
    const installer_target_info_t *target,
    char *label,
    size_t capacity
)
{
    clear_text(label, capacity);

    if (target == NULL)
    {
        return;
    }

    append_text(label, capacity, target->name);
    append_text(label, capacity, " - ");
    append_unsigned_decimal(label, capacity, target->capacity_mib);
    append_text(label, capacity, " MiB - 512-byte sectors");
}

static void render_installer_app(const ui_rect_t *content)
{
    const gui_theme_t *theme = desktop_services_theme();
    ui_palette_t colors = {
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

    ui_rect_t refresh = installer_refresh_bounds(content);
    ui_rect_t prepare = installer_prepare_bounds(content);
    ui_rect_t list = installer_list_bounds(content);
    ui_rect_t status = installer_status_bounds(content);

    bool installing = installer_running();

    ui_control_draw_button(
        &refresh,
        "Refresh",
        &colors,
        installing ? UI_CONTROL_DISABLED : UI_CONTROL_NORMAL
    );

    ui_control_draw_button(
        &prepare,
        installing ? "Installing..." : "Install LatterOS",
        &colors,
        installing || installer_selected_target == UINT32_MAX ?
            UI_CONTROL_DISABLED : UI_CONTROL_NORMAL
    );

    ui_draw_text(
        "Destructive installation: all data on the selected disk will be erased.",
        content->x + 12,
        content->y + 46,
        colors.danger
    );

    ui_fill_rect(&list, colors.field);
    ui_draw_border(&list, colors.border, 1);

    uint32_t count = installer_target_count();
    uint32_t visible = count < INSTALLER_VISIBLE_ROWS ?
        count : INSTALLER_VISIBLE_ROWS;

    if (visible == 0)
    {
        ui_draw_text(
            "No safe installation target detected.",
            list.x + 8,
            list.y + 10,
            colors.disabled
        );
    }

    for (uint32_t row = 0; row < visible; row++)
    {
        installer_target_info_t target;

        if (!installer_target_get(row, &target))
        {
            continue;
        }

        ui_rect_t row_bounds = {
            list.x + 3,
            list.y + 3 + (int32_t)row * 30,
            list.width > 6U ? list.width - 6U : 1U,
            28
        };

        char label[128];
        installer_target_label(
            &target,
            label,
            sizeof(label)
        );

        ui_control_draw_list_row(
            &row_bounds,
            row == installer_selected_target ? ">" : "",
            label,
            &colors,
            row == installer_selected_target,
            false
        );
    }

    ui_control_draw_statusbar(
        &status,
        installer_status,
        installing ? "Installing" : "Milestone 19D",
        &colors
    );
}

static bool handle_installer_click(
    const ui_rect_t *content,
    int32_t x,
    int32_t y
)
{
    ui_rect_t refresh = installer_refresh_bounds(content);
    ui_rect_t prepare = installer_prepare_bounds(content);
    ui_rect_t list = installer_list_bounds(content);

    if (installer_running())
    {
        copy_text(
            installer_status,
            sizeof(installer_status),
            "Installation in progress - wait for verification"
        );
        return true;
    }

    if (ui_point_in_rect(x, y, &refresh))
    {
        installer_refresh();
        installer_selected_target =
            installer_target_count() == 0 ? UINT32_MAX : 0U;
        copy_text(
            installer_status,
            sizeof(installer_status),
            installer_target_count() == 0 ?
                installer_last_error() :
                "Disk list refreshed"
        );
        return true;
    }

    if (
        ui_point_in_rect(x, y, &prepare) &&
        installer_selected_target != UINT32_MAX
    )
    {
        show_installer_confirmation();
        return true;
    }

    if (ui_point_in_rect(x, y, &list))
    {
        int32_t relative = y - list.y - 3;

        if (relative >= 0)
        {
            uint32_t row = (uint32_t)relative / 30U;

            if (row < installer_target_count())
            {
                installer_selected_target = row;
                copy_text(
                    installer_status,
                    sizeof(installer_status),
                    "Target selected - verify capacity before preparing"
                );
                return true;
            }
        }
    }

    return false;
}

static ui_rect_t desktop_settings_tab_button(
    const ui_rect_t *content,
    uint32_t index
)
{
    uint32_t gap = 8U;
    uint32_t width =
        (content->width - 36U -
            gap * (SETTINGS_PAGE_COUNT - 1U)) /
        SETTINGS_PAGE_COUNT;

    return (ui_rect_t){
        .x = content->x + 18 +
            (int32_t)index * (int32_t)(width + gap),
        .y = content->y + 12,
        .width = width,
        .height = 32U
    };
}

static ui_rect_t desktop_settings_theme_button(
    const ui_rect_t *content,
    uint32_t index
)
{
    uint32_t column = index % 2U;
    uint32_t row = index / 2U;
    uint32_t gap = 10U;
    uint32_t width = (content->width - 46U) / 2U;

    return (ui_rect_t){
        .x = content->x + 18 +
            (int32_t)column * (int32_t)(width + gap),
        .y = content->y + 92 + (int32_t)row * 40,
        .width = width,
        .height = 30U
    };
}

static ui_rect_t desktop_settings_wallpaper_button(
    const ui_rect_t *content,
    uint32_t index
)
{
    uint32_t gap = 8U;
    uint32_t width = (content->width - 60U) / 4U;

    return (ui_rect_t){
        .x = content->x + 18 +
            (int32_t)index * (int32_t)(width + gap),
        .y = content->y + 244,
        .width = width,
        .height = 30U
    };
}

static ui_rect_t desktop_settings_reset_button(
    const ui_rect_t *content
)
{
    return (ui_rect_t){
        .x = content->x + 18,
        .y = content->y + 292,
        .width = content->width - 36U,
        .height = 32U
    };
}

static ui_rect_t desktop_settings_package_row(
    const ui_rect_t *content,
    uint32_t index
)
{
    return (ui_rect_t){
        .x = content->x + 18,
        .y = content->y + 104 + (int32_t)index * 48,
        .width = content->width - 36U,
        .height = 40U
    };
}

static ui_rect_t desktop_settings_package_remove_button(
    const ui_rect_t *row
)
{
    return (ui_rect_t){
        .x = row->x + (int32_t)row->width - 86,
        .y = row->y + 7,
        .width = 76U,
        .height = 26U
    };
}

static ui_rect_t desktop_settings_hardware_action_button(
    const ui_rect_t *content,
    uint32_t index
)
{
    uint32_t gap = 8U;
    uint32_t width = (content->width - 52U) / 3U;

    return (ui_rect_t){
        .x = content->x + 18 +
            (int32_t)index * (int32_t)(width + gap),
        .y = content->y + 322,
        .width = width,
        .height = 30U
    };
}

static ui_rect_t desktop_settings_stability_action_button(
    const ui_rect_t *content,
    uint32_t index
)
{
    uint32_t gap = 8U;
    uint32_t width = (content->width - 52U) / 3U;

    return (ui_rect_t){
        .x = content->x + 18 +
            (int32_t)index * (int32_t)(width + gap),
        .y = content->y + 322,
        .width = width,
        .height = 30U
    };
}

static ui_rect_t desktop_settings_desktop_action_button(
    const ui_rect_t *content,
    uint32_t index
)
{
    uint32_t column = index % 2U;
    uint32_t row = index / 2U;
    uint32_t gap = 10U;
    uint32_t width = (content->width - 46U) / 2U;

    return (ui_rect_t){
        .x = content->x + 18 +
            (int32_t)column * (int32_t)(width + gap),
        .y = content->y + 276 + (int32_t)row * 42,
        .width = width,
        .height = 32U
    };
}

static void desktop_settings_render_tabs(
    const ui_rect_t *content,
    const ui_palette_t *palette
)
{
    static const char *labels[SETTINGS_PAGE_COUNT] = {
        "Look",
        "Packages",
        "Hardware",
        "Stability",
        "Desktop"
    };

    for (uint32_t index = 0; index < SETTINGS_PAGE_COUNT; index++)
    {
        ui_rect_t tab = desktop_settings_tab_button(content, index);

        ui_control_draw_button(
            &tab,
            labels[index],
            palette,
            settings_page == (settings_page_t)index ?
                UI_CONTROL_FOCUSED : UI_CONTROL_NORMAL
        );
    }
}

static void desktop_settings_render_appearance(
    const ui_rect_t *content,
    const ui_palette_t *palette
)
{
    ui_draw_text(
        "Desktop theme",
        content->x + 18,
        content->y + 66,
        palette->disabled
    );

    static const char *theme_labels[6] = {
        "Blue",
        "Graphite",
        "Teal",
        "Aubergine",
        "External",
        "LDS"
    };

    static const desktop_theme_kind_t theme_kinds[6] = {
        DESKTOP_THEME_BLUE,
        DESKTOP_THEME_GRAPHITE,
        DESKTOP_THEME_TEAL,
        DESKTOP_THEME_AUBERGINE,
        DESKTOP_THEME_EXTERNAL,
        DESKTOP_THEME_LDS
    };

    desktop_theme_kind_t selected_theme =
        desktop_services_theme_kind();

    for (uint32_t index = 0; index < 6U; index++)
    {
        ui_rect_t button =
            desktop_settings_theme_button(content, index);

        ui_control_draw_button(
            &button,
            theme_labels[index],
            palette,
            selected_theme == theme_kinds[index] ?
                UI_CONTROL_FOCUSED : UI_CONTROL_NORMAL
        );
    }

    ui_draw_text(
        "Wallpaper",
        content->x + 18,
        content->y + 222,
        palette->disabled
    );

    static const char *wallpaper_labels[4] = {
        "Solid", "Gradient", "Grid", "Night"
    };

    desktop_wallpaper_t selected_wallpaper =
        desktop_services_wallpaper();

    for (uint32_t index = 0; index < 4U; index++)
    {
        ui_rect_t button =
            desktop_settings_wallpaper_button(content, index);

        ui_control_draw_button(
            &button,
            wallpaper_labels[index],
            palette,
            selected_wallpaper == (desktop_wallpaper_t)index ?
                UI_CONTROL_FOCUSED : UI_CONTROL_NORMAL
        );
    }

    ui_rect_t reset = desktop_settings_reset_button(content);
    ui_control_draw_button(
        &reset,
        "Reset desktop appearance and saved layout",
        palette,
        UI_CONTROL_NORMAL
    );

    ui_draw_text(
        "LDS uses deep blue, white, and gold with a subtle LDS desktop mark.",
        content->x + 18,
        content->y + 338,
        palette->disabled
    );
}

static void desktop_settings_render_packages(
    const ui_rect_t *content,
    const ui_palette_t *palette
)
{
    ui_draw_text(
        "Open a .lpkg file in Files to inspect, install, or update it.",
        content->x + 18,
        content->y + 60,
        palette->disabled
    );

    uint32_t count = package_manager_installed_count();
    char count_text[64] = "Installed packages: ";
    append_unsigned_decimal(count_text, sizeof(count_text), count);

    ui_draw_text(
        count_text,
        content->x + 18,
        content->y + 82,
        palette->text
    );

    if (count == 0)
    {
        ui_draw_text(
            "No application packages are installed yet.",
            content->x + 18,
            content->y + 124,
            palette->disabled
        );
        return;
    }

    uint32_t visible = count > 5U ? 5U : count;

    for (uint32_t index = 0; index < visible; index++)
    {
        package_info_t information;

        if (!package_manager_installed_get(index, &information))
        {
            continue;
        }

        ui_rect_t row = desktop_settings_package_row(content, index);
        ui_fill_rect(&row, palette->field);
        ui_draw_border(&row, palette->border, 1U);

        char label[96];
        label[0] = '\0';
        append_text(label, sizeof(label), information.name);
        append_text(label, sizeof(label), "  ");
        append_text(label, sizeof(label), information.version);

        ui_draw_text(
            label,
            row.x + 10,
            row.y + 6,
            palette->text
        );

        ui_rect_t description_bounds = {
            .x = row.x + 6,
            .y = row.y + 18,
            .width = row.width > 104U ? row.width - 104U : row.width,
            .height = 18U
        };

        ui_draw_text_ellipsized(
            information.description[0] != '\0' ?
                information.description : "Installed LPKG package",
            &description_bounds,
            4,
            palette->disabled
        );

        ui_rect_t remove =
            desktop_settings_package_remove_button(&row);
        ui_control_draw_button(
            &remove,
            "Remove",
            palette,
            UI_CONTROL_NORMAL
        );
    }

    if (count > visible)
    {
        ui_draw_text(
            "Only the first five packages are shown.",
            content->x + 18,
            content->y + 354,
            palette->disabled
        );
    }
}

static void desktop_settings_draw_hardware_line(
    const ui_rect_t *content,
    const ui_palette_t *palette,
    int32_t y,
    const char *text,
    bool emphasized
)
{
    ui_rect_t bounds = {
        .x = content->x + 18,
        .y = content->y + y,
        .width = content->width - 36U,
        .height = 18U
    };

    ui_draw_text_ellipsized(
        text,
        &bounds,
        4,
        emphasized ? palette->text : palette->disabled
    );
}

static void desktop_settings_render_hardware(
    const ui_rect_t *content,
    const ui_palette_t *palette
)
{
    const hardware_compat_snapshot_t *snapshot =
        hardware_compat_snapshot();

    char line[192];
    line[0] = '\0';
    append_text(line, sizeof(line), "Compatibility: ");
    append_text(line, sizeof(line), hardware_compat_status_name());
    desktop_settings_draw_hardware_line(
        content,
        palette,
        60,
        line,
        true
    );

    desktop_settings_draw_hardware_line(
        content,
        palette,
        80,
        hardware_compat_summary(),
        false
    );

    line[0] = '\0';
    append_text(line, sizeof(line), "Platform: ");
    append_text(line, sizeof(line), snapshot->hypervisor_name);
    append_text(line, sizeof(line), " | CPU: ");
    append_text(line, sizeof(line), snapshot->cpu_brand);
    desktop_settings_draw_hardware_line(content, palette, 106, line, true);

    line[0] = '\0';
    append_text(line, sizeof(line), "CPU cores online/detected: ");
    append_unsigned_decimal(line, sizeof(line), snapshot->online_cpus);
    append_text(line, sizeof(line), "/");
    append_unsigned_decimal(line, sizeof(line), snapshot->detected_cpus);
    desktop_settings_draw_hardware_line(content, palette, 126, line, false);

    line[0] = '\0';
    append_text(line, sizeof(line), "Memory usable/total MiB: ");
    append_unsigned_decimal(
        line,
        sizeof(line),
        (uint32_t)(snapshot->usable_memory_bytes / (1024ULL * 1024ULL))
    );
    append_text(line, sizeof(line), "/");
    append_unsigned_decimal(
        line,
        sizeof(line),
        (uint32_t)(snapshot->total_memory_bytes / (1024ULL * 1024ULL))
    );
    desktop_settings_draw_hardware_line(content, palette, 146, line, false);

    line[0] = '\0';
    append_text(line, sizeof(line), "ACPI: ");
    append_text(line, sizeof(line), snapshot->acpi_available ? "ready " : "missing ");
    append_text(line, sizeof(line), snapshot->acpi_root);
    append_text(line, sizeof(line), " OEM ");
    append_text(line, sizeof(line), snapshot->acpi_oem);
    desktop_settings_draw_hardware_line(content, palette, 166, line, false);

    line[0] = '\0';
    append_text(line, sizeof(line), "PCI devices: ");
    append_unsigned_decimal(line, sizeof(line), snapshot->pci_devices);
    append_text(line, sizeof(line), "  storage/network/display: ");
    append_unsigned_decimal(line, sizeof(line), snapshot->pci_storage);
    append_text(line, sizeof(line), "/");
    append_unsigned_decimal(line, sizeof(line), snapshot->pci_network);
    append_text(line, sizeof(line), "/");
    append_unsigned_decimal(line, sizeof(line), snapshot->pci_display);
    desktop_settings_draw_hardware_line(content, palette, 186, line, false);

    line[0] = '\0';
    append_text(line, sizeof(line), "Block devices: ");
    append_unsigned_decimal(line, sizeof(line), snapshot->block_devices);
    append_text(line, sizeof(line), "  AHCI/NVMe: ");
    append_unsigned_decimal(line, sizeof(line), snapshot->ahci_devices);
    append_text(line, sizeof(line), "/");
    append_unsigned_decimal(line, sizeof(line), snapshot->nvme_namespaces);
    desktop_settings_draw_hardware_line(content, palette, 206, line, false);

    line[0] = '\0';
    append_text(line, sizeof(line), "USB: ");
    append_text(line, sizeof(line), snapshot->usb_ready ? "ready" : "unavailable");
    append_text(line, sizeof(line), " devices=");
    append_unsigned_decimal(line, sizeof(line), snapshot->usb_devices);
    append_text(line, sizeof(line), " keyboard/mouse=");
    append_text(line, sizeof(line), snapshot->usb_keyboard ? "yes" : "no");
    append_text(line, sizeof(line), "/");
    append_text(line, sizeof(line), snapshot->usb_mouse ? "yes" : "no");
    desktop_settings_draw_hardware_line(content, palette, 226, line, false);

    line[0] = '\0';
    append_text(line, sizeof(line), "Network ready/link: ");
    append_text(line, sizeof(line), snapshot->network_ready ? "yes" : "no");
    append_text(line, sizeof(line), "/");
    append_text(line, sizeof(line), snapshot->network_link ? "up" : "down");
    desktop_settings_draw_hardware_line(content, palette, 246, line, false);

    line[0] = '\0';
    append_text(line, sizeof(line), "Display: ");
    append_text(line, sizeof(line), snapshot->display_backend);
    append_text(line, sizeof(line), " ");
    append_unsigned_decimal(line, sizeof(line), snapshot->display_width);
    append_text(line, sizeof(line), "x");
    append_unsigned_decimal(line, sizeof(line), snapshot->display_height);
    append_text(line, sizeof(line), " @ ");
    append_unsigned_decimal(line, sizeof(line), snapshot->display_refresh_hz);
    append_text(line, sizeof(line), " Hz");
    desktop_settings_draw_hardware_line(content, palette, 266, line, false);

    line[0] = '\0';
    append_text(line, sizeof(line), "Warnings/failures: ");
    append_unsigned_decimal(line, sizeof(line), snapshot->warning_count);
    append_text(line, sizeof(line), "/");
    append_unsigned_decimal(line, sizeof(line), snapshot->failure_count);
    desktop_settings_draw_hardware_line(content, palette, 286, line, true);

    static const char *action_labels[3] = {
        "Refresh",
        "Quick test",
        "Save report"
    };

    for (uint32_t index = 0; index < 3U; index++)
    {
        ui_rect_t button =
            desktop_settings_hardware_action_button(content, index);
        ui_control_draw_button(
            &button,
            action_labels[index],
            palette,
            UI_CONTROL_NORMAL
        );
    }

    desktop_settings_draw_hardware_line(
        content,
        palette,
        360,
        "Reports are saved to Documents/Hardware Compatibility Report.txt",
        false
    );
}

static void desktop_settings_render_stability(
    const ui_rect_t *content,
    const ui_palette_t *palette
)
{
    const stability_snapshot_t *snapshot =
        stability_monitor_snapshot();

    char line[192];
    line[0] = '\0';
    append_text(line, sizeof(line), "System stability: ");
    append_text(line, sizeof(line), stability_monitor_state_name());
    desktop_settings_draw_hardware_line(
        content,
        palette,
        60,
        line,
        true
    );

    line[0] = '\0';
    append_text(line, sizeof(line), "Boot profile/source: ");
    append_text(line, sizeof(line), boot_mode_name());
    append_text(line, sizeof(line), " / ");
    append_text(line, sizeof(line), boot_source_name());
    desktop_settings_draw_hardware_line(
        content,
        palette,
        80,
        line,
        false
    );

    desktop_settings_draw_hardware_line(
        content,
        palette,
        100,
        stability_monitor_stage(),
        false
    );

    ui_rect_t progress_border = {
        .x = content->x + 18,
        .y = content->y + 124,
        .width = content->width - 36U,
        .height = 16U
    };
    ui_fill_rect(&progress_border, palette->field);
    ui_draw_border(&progress_border, palette->border, 1U);

    ui_rect_t progress_fill = {
        .x = progress_border.x + 2,
        .y = progress_border.y + 2,
        .width = ((progress_border.width - 4U) * snapshot->progress) / 100U,
        .height = progress_border.height - 4U
    };

    if (progress_fill.width != 0U)
    {
        ui_fill_rect(&progress_fill, palette->accent);
    }

    line[0] = '\0';
    append_text(line, sizeof(line), "Progress: ");
    append_unsigned_decimal(line, sizeof(line), snapshot->progress);
    append_text(line, sizeof(line), "%  uptime=");
    append_unsigned_decimal(
        line,
        sizeof(line),
        (uint32_t)snapshot->uptime_seconds
    );
    append_text(line, sizeof(line), "s  timer=");
    append_unsigned_decimal(line, sizeof(line), snapshot->timer_frequency);
    append_text(line, sizeof(line), " Hz");
    desktop_settings_draw_hardware_line(content, palette, 150, line, false);

    line[0] = '\0';
    append_text(line, sizeof(line), "Timer/memory passed: ");
    append_text(line, sizeof(line), snapshot->timer_passed ? "yes" : "no");
    append_text(line, sizeof(line), "/");
    append_text(line, sizeof(line), snapshot->memory_passed ? "yes" : "no");
    desktop_settings_draw_hardware_line(content, palette, 174, line, false);

    line[0] = '\0';
    append_text(line, sizeof(line), "Block devices online/invalid: ");
    append_unsigned_decimal(line, sizeof(line), snapshot->online_block_devices);
    append_text(line, sizeof(line), "/");
    append_unsigned_decimal(line, sizeof(line), snapshot->invalid_block_devices);
    desktop_settings_draw_hardware_line(content, palette, 194, line, false);

    line[0] = '\0';
    append_text(line, sizeof(line), "AP scheduler submitted/completed: ");
    append_text(line, sizeof(line), snapshot->smp_submitted ? "yes" : "no");
    append_text(line, sizeof(line), "/");
    append_text(line, sizeof(line), snapshot->smp_completed ? "yes" : "no");
    desktop_settings_draw_hardware_line(content, palette, 214, line, false);

    line[0] = '\0';
    append_text(line, sizeof(line), "Display presented/dropped/failures: ");
    append_unsigned_decimal(
        line,
        sizeof(line),
        (uint32_t)snapshot->display_presented_frames
    );
    append_text(line, sizeof(line), "/");
    append_unsigned_decimal(
        line,
        sizeof(line),
        (uint32_t)snapshot->display_dropped_frames
    );
    append_text(line, sizeof(line), "/");
    append_unsigned_decimal(
        line,
        sizeof(line),
        (uint32_t)snapshot->display_capture_failures
    );
    desktop_settings_draw_hardware_line(content, palette, 234, line, false);

    line[0] = '\0';
    append_text(line, sizeof(line), "Warnings/failures: ");
    append_unsigned_decimal(line, sizeof(line), snapshot->warning_count);
    append_text(line, sizeof(line), "/");
    append_unsigned_decimal(line, sizeof(line), snapshot->failure_count);
    desktop_settings_draw_hardware_line(content, palette, 254, line, true);

    desktop_settings_draw_hardware_line(
        content,
        palette,
        280,
        boot_mode_is_compatibility() ?
            "Compatibility Mode: software graphics fallback is active" :
            "Compatibility Mode is available from the Limine boot menu",
        false
    );

    static const char *labels[3] = {
        "Run test",
        "Save report",
        "Reset stats"
    };

    for (uint32_t index = 0U; index < 3U; index++)
    {
        ui_rect_t button =
            desktop_settings_stability_action_button(content, index);
        ui_control_draw_button(
            &button,
            labels[index],
            palette,
            stability_monitor_running() ?
                UI_CONTROL_DISABLED : UI_CONTROL_NORMAL
        );
    }

    desktop_settings_draw_hardware_line(
        content,
        palette,
        360,
        "Reports are saved to Documents/System Stability Report.txt",
        false
    );
}

static void desktop_settings_render_desktop(
    const ui_rect_t *content,
    const ui_palette_t *palette
)
{
    char line[192];

    desktop_settings_draw_hardware_line(
        content,
        palette,
        60,
        "Desktop reliability: READY",
        true
    );

    desktop_settings_draw_hardware_line(
        content,
        palette,
        82,
        "Launcher first-frame protection: fresh opaque surface enforced",
        false
    );

    line[0] = '\0';
    append_text(line, sizeof(line), "Layout autosave: ");
    append_text(
        line,
        sizeof(line),
        layout_autosave_pending ? "waiting for movement to settle" : "idle"
    );
    desktop_settings_draw_hardware_line(content, palette, 104, line, false);

    line[0] = '\0';
    append_text(line, sizeof(line), "Fresh launches / animation recoveries: ");
    append_unsigned_decimal(
        line,
        sizeof(line),
        launcher_clean_launch_count
    );
    append_text(line, sizeof(line), " / ");
    append_unsigned_decimal(
        line,
        sizeof(line),
        animation_surface_recovery_count
    );
    desktop_settings_draw_hardware_line(content, palette, 130, line, false);

    line[0] = '\0';
    append_text(line, sizeof(line), "Autosaves / manual rebuilds: ");
    append_unsigned_decimal(line, sizeof(line), layout_autosave_count);
    append_text(line, sizeof(line), " / ");
    append_unsigned_decimal(
        line,
        sizeof(line),
        manual_surface_rebuild_count
    );
    desktop_settings_draw_hardware_line(content, palette, 150, line, false);

    line[0] = '\0';
    append_text(line, sizeof(line), "Surface captures/draws/scaled: ");
    append_unsigned_decimal(
        line,
        sizeof(line),
        window_surface_total_captures()
    );
    append_text(line, sizeof(line), "/");
    append_unsigned_decimal(
        line,
        sizeof(line),
        window_surface_total_draws()
    );
    append_text(line, sizeof(line), "/");
    append_unsigned_decimal(
        line,
        sizeof(line),
        window_surface_total_scaled_draws()
    );
    desktop_settings_draw_hardware_line(content, palette, 176, line, false);

    line[0] = '\0';
    append_text(line, sizeof(line), "Compositor FPS / dropped frames: ");
    append_unsigned_decimal(
        line,
        sizeof(line),
        compositor_recent_active_fps()
    );
    append_text(line, sizeof(line), " / ");
    append_unsigned_decimal(
        line,
        sizeof(line),
        compositor_dropped_frame_count()
    );
    desktop_settings_draw_hardware_line(content, palette, 196, line, false);

    line[0] = '\0';
    append_text(line, sizeof(line), "Display backend: ");
    append_text(line, sizeof(line), compositor_display_backend());
    append_text(line, sizeof(line), compositor_triple_buffered() ?
        " (triple buffered)" : "");
    desktop_settings_draw_hardware_line(content, palette, 216, line, false);

    static const char *labels[4] = {
        "Rebuild surfaces",
        "Save layout",
        "Reset positions",
        "Save report"
    };

    for (uint32_t index = 0U; index < 4U; index++)
    {
        ui_rect_t button =
            desktop_settings_desktop_action_button(content, index);
        ui_control_draw_button(
            &button,
            labels[index],
            palette,
            UI_CONTROL_NORMAL
        );
    }

    desktop_settings_draw_hardware_line(
        content,
        palette,
        364,
        "Report: Documents/Desktop Reliability Report.txt",
        false
    );
}

static void desktop_settings_render(const ui_rect_t *content)
{
    if (content == NULL)
    {
        return;
    }

    ui_palette_t palette = current_palette();
    desktop_settings_render_tabs(content, &palette);

    switch (settings_page)
    {
        case SETTINGS_PAGE_PACKAGES:
            desktop_settings_render_packages(content, &palette);
            break;

        case SETTINGS_PAGE_HARDWARE:
            desktop_settings_render_hardware(content, &palette);
            break;

        case SETTINGS_PAGE_STABILITY:
            desktop_settings_render_stability(content, &palette);
            break;

        case SETTINGS_PAGE_DESKTOP:
            desktop_settings_render_desktop(content, &palette);
            break;

        case SETTINGS_PAGE_APPEARANCE:
        default:
            desktop_settings_render_appearance(content, &palette);
            break;
    }
}

static bool desktop_settings_handle_click(
    const ui_rect_t *content,
    int32_t x,
    int32_t y
)
{
    if (content == NULL)
    {
        return false;
    }

    for (uint32_t index = 0; index < SETTINGS_PAGE_COUNT; index++)
    {
        ui_rect_t tab = desktop_settings_tab_button(content, index);

        if (ui_point_in_rect(x, y, &tab))
        {
            settings_page = (settings_page_t)index;
            return true;
        }
    }

    if (settings_page == SETTINGS_PAGE_PACKAGES)
    {
        uint32_t count = package_manager_installed_count();
        uint32_t visible = count > 5U ? 5U : count;

        for (uint32_t index = 0; index < visible; index++)
        {
            package_info_t information;

            if (!package_manager_installed_get(index, &information))
            {
                continue;
            }

            ui_rect_t row = desktop_settings_package_row(content, index);
            ui_rect_t remove =
                desktop_settings_package_remove_button(&row);

            if (ui_point_in_rect(x, y, &remove))
            {
                show_package_remove_dialog(information.name);
                return true;
            }
        }

        return false;
    }

    if (settings_page == SETTINGS_PAGE_HARDWARE)
    {
        for (uint32_t index = 0; index < 3U; index++)
        {
            ui_rect_t button =
                desktop_settings_hardware_action_button(content, index);

            if (!ui_point_in_rect(x, y, &button))
            {
                continue;
            }

            if (index == 0U)
            {
                hardware_compat_refresh();
            }
            else if (index == 1U)
            {
                (void)hardware_compat_run_quick_test();
            }
            else
            {
                (void)hardware_compat_write_report();
            }

            desktop_notify(hardware_compat_last_message(), 5000U);
            return true;
        }

        return false;
    }

    if (settings_page == SETTINGS_PAGE_STABILITY)
    {
        if (stability_monitor_running())
        {
            return false;
        }

        for (uint32_t index = 0U; index < 3U; index++)
        {
            ui_rect_t button =
                desktop_settings_stability_action_button(content, index);

            if (!ui_point_in_rect(x, y, &button))
            {
                continue;
            }

            if (index == 0U)
            {
                (void)stability_monitor_start_test();
            }
            else if (index == 1U)
            {
                (void)stability_monitor_write_report();
            }
            else
            {
                stability_monitor_reset_statistics();
            }

            visible_stability_state = stability_monitor_state();
            desktop_notify(stability_monitor_last_message(), 5000U);
            invalidate_window(5);
            return true;
        }

        return false;
    }

    if (settings_page == SETTINGS_PAGE_DESKTOP)
    {
        for (uint32_t index = 0U; index < 4U; index++)
        {
            ui_rect_t button =
                desktop_settings_desktop_action_button(content, index);

            if (!ui_point_in_rect(x, y, &button))
            {
                continue;
            }

            if (index == 0U)
            {
                window_animation_init();

                for (
                    uint32_t window_index = 0;
                    window_index < GUI_WINDOW_COUNT;
                    window_index++
                )
                {
                    animation_target_valid[window_index] = false;
                }

                window_surface_invalidate_all();
                drag_cache_valid = false;
                manual_surface_rebuild_count++;
                compositor_invalidate_all();
                desktop_notify("All window surfaces rebuilt", 4000U);
            }
            else if (index == 1U)
            {
                save_window_layout();
            }
            else if (index == 2U)
            {
                reset_window_positions();
            }
            else
            {
                bool saved = write_desktop_reliability_report();
                desktop_notify(
                    saved ? "Desktop reliability report saved" :
                        "Unable to save desktop reliability report",
                    saved ? 4500U : 6500U
                );
            }

            invalidate_window(5);
            return true;
        }

        return false;
    }

    static const desktop_theme_kind_t theme_kinds[6] = {
        DESKTOP_THEME_BLUE,
        DESKTOP_THEME_GRAPHITE,
        DESKTOP_THEME_TEAL,
        DESKTOP_THEME_AUBERGINE,
        DESKTOP_THEME_EXTERNAL,
        DESKTOP_THEME_LDS
    };

    for (uint32_t index = 0; index < 6U; index++)
    {
        ui_rect_t button =
            desktop_settings_theme_button(content, index);

        if (!ui_point_in_rect(x, y, &button))
        {
            continue;
        }

        bool saved = desktop_services_set_theme(theme_kinds[index]);
        chrome_accent = desktop_services_theme()->accent;
        desktop_notify(
            saved ? desktop_services_theme_name() :
                "Theme applied, but configuration could not be saved",
            4500U
        );
        return true;
    }

    for (uint32_t index = 0; index < 4U; index++)
    {
        ui_rect_t button =
            desktop_settings_wallpaper_button(content, index);

        if (!ui_point_in_rect(x, y, &button))
        {
            continue;
        }

        (void)desktop_services_set_wallpaper(
            (desktop_wallpaper_t)index
        );
        desktop_notify(desktop_services_wallpaper_name(), 3500U);
        return true;
    }

    ui_rect_t reset = desktop_settings_reset_button(content);

    if (ui_point_in_rect(x, y, &reset))
    {
        bool saved = desktop_services_reset_configuration();
        chrome_accent = desktop_services_theme()->accent;
        desktop_notify(
            saved ? "Desktop settings reset" :
                "Desktop reset applied, but save failed",
            4500U
        );
        return true;
    }

    return false;
}

static void render_window(
    uint8_t index,
    const gui_window_t *window
)
{
    if (
        !window->state.visible ||
        window->state.minimized
    )
    {
        return;
    }

    render_window_shadow(window);

    if (
        !window->state.dragging &&
        !window->state.resizing &&
        window_surface_draw(index, &window->state.bounds)
    )
    {
        return;
    }

    render_window_frame(index, window);

    if (window->app == GUI_APP_TERMINAL)
    {
        render_terminal_app(window);
    }
    else if (window->app == GUI_APP_FILE_EXPLORER)
    {
        render_explorer_app(window);
    }
    else
    {
        ui_rect_t content = window_content_bounds(window);

        switch (window->app)
        {
            case GUI_APP_TEXT_EDITOR:
                desktop_editor_render(&content);
                break;

            case GUI_APP_CALCULATOR:
                calculator_render(&content);
                break;

            case GUI_APP_PAINT:
                paint_render(&content);
                break;

            case GUI_APP_SETTINGS:
                desktop_settings_render(&content);
                break;

            case GUI_APP_PROCESS_MANAGER:
                process_manager_render(&content);
                break;

            case GUI_APP_INSTALLER:
                if (recovery_panel_active)
                {
                    recovery_render(&content);
                }
                else
                {
                    render_installer_app(&content);
                }
                break;

            default:
                break;
        }
    }

    if (
        !window->state.dragging &&
        !window->state.resizing
    )
    {
        (void)window_surface_capture(
            index,
            &window->state.bounds
        );
    }
}

static bool render_animated_window(uint8_t index)
{
    ui_rect_t bounds;

    if (
        index >= GUI_WINDOW_COUNT ||
        !window_animation_bounds(index, &bounds)
    )
    {
        return false;
    }

    if (
        bounds.width > 96U &&
        bounds.height > 64U
    )
    {
        visual_effects_draw_shadow(&bounds, COLOR_SHADOW);
    }

    if (window_surface_draw_scaled(index, &bounds))
    {
        return true;
    }

    animation_surface_recovery_count++;
    window_animation_cancel(index);

    if (animation_target_valid[index])
    {
        windows[index].state = animation_targets[index];
        animation_target_valid[index] = false;

        if (
            windows[index].state.visible &&
            !windows[index].state.minimized
        )
        {
            bring_window_to_front(index);
        }
    }

    window_surface_invalidate(index);
    render_window(index, &windows[index]);
    compositor_invalidate_all();
    return true;
}

static void render_cached_drag_window(
    const gui_window_t *window
)
{
    render_window_shadow(window);

    graphics_blit_surface(
        drag_cache,
        drag_cache_width,
        drag_cache_width,
        drag_cache_height,
        window->state.bounds.x,
        window->state.bounds.y
    );
}

static void render_gui_scene(void)
{
    uint8_t fullscreen = active_fullscreen_window();

    if (fullscreen < GUI_WINDOW_COUNT)
    {
        ui_palette_t palette = current_palette();
        graphics_clear(palette.panel);
        render_window(fullscreen, &windows[fullscreen]);
    }
    else
    {
        render_desktop();
        render_snap_preview();

        for (uint8_t order = 0; order < GUI_WINDOW_COUNT; order++)
        {
            uint8_t index = window_order[order];

            if (window_animation_active(index))
            {
                (void)render_animated_window(index);
            }
            else if (
                drag_cache_valid &&
                drag_cache_window == index &&
                windows[index].state.dragging
            )
            {
                render_cached_drag_window(&windows[index]);
            }
            else
            {
                render_window(index, &windows[index]);
            }
        }

        render_taskbar();
    }

    ui_palette_t palette = current_palette();
    render_notifications();
    render_file_drag();
    ui_menu_render(&popup_menu, &palette);
    desktop_dialog_render(&dialog, &palette);

    /*
     * Native Virtio scanout cannot use the Limine front-buffer overlay.
     * Compose the pointer into the same damage-tracked scene as windows.
     * This keeps the visible pointer and GUI hit-testing on one coordinate
     * path and completely avoids the unstable Virtio cursor queue.
     */
    if (cursor_visible && virtio_gpu_available())
    {
        ui_draw_cursor(cursor_x, cursor_y);
    }
}

static void prepare_drag_cache(uint8_t index)
{
    drag_cache_valid = false;

    if (index >= GUI_WINDOW_COUNT)
    {
        return;
    }

    gui_window_t *window = &windows[index];

    if (
        window->state.bounds.width >
            GUI_DRAG_CACHE_MAX_WIDTH ||
        window->state.bounds.height >
            GUI_DRAG_CACHE_MAX_HEIGHT
    )
    {
        return;
    }

    if (compositor_has_damage())
    {
        compositor_render();
    }

    if (!graphics_capture_rectangle(
        (uint32_t)window->state.bounds.x,
        (uint32_t)window->state.bounds.y,
        window->state.bounds.width,
        window->state.bounds.height,
        drag_cache,
        window->state.bounds.width
    ))
    {
        return;
    }

    drag_cache_width = window->state.bounds.width;
    drag_cache_height = window->state.bounds.height;
    drag_cache_window = index;
    drag_cache_valid = true;
}

static void execute_context_command(uint32_t command)
{
    switch ((context_command_t)command)
    {
        case CONTEXT_COMMAND_OPEN:
            explorer_open_selected();
            break;

        case CONTEXT_COMMAND_RENAME:
            show_rename_dialog();
            break;

        case CONTEXT_COMMAND_DELETE:
            show_delete_dialog();
            break;

        case CONTEXT_COMMAND_PROPERTIES:
            show_properties_dialog();
            break;

        case CONTEXT_COMMAND_OPEN_DIALOG:
            open_file_picker();
            break;

        case CONTEXT_COMMAND_NEW_FILE:
            show_new_file_dialog();
            break;

        case CONTEXT_COMMAND_NEW_FOLDER:
            show_new_folder_dialog();
            break;

        case CONTEXT_COMMAND_REFRESH:
            refresh_explorer();
            break;

        case CONTEXT_COMMAND_COPY_PATH:
            copy_selected_file(false);
            break;

        case CONTEXT_COMMAND_CUT_PATH:
            copy_selected_file(true);
            break;

        case CONTEXT_COMMAND_PASTE_ITEM:
            (void)paste_file_into_explorer();
            break;

        default:
            break;
    }
}

static void execute_system_command(uint32_t command)
{
    switch ((system_command_t)command)
    {
        case SYSTEM_COMMAND_SWITCH_USER:
            security_logout();
            activate_window(0);
            terminal_focused = true;
            gui_terminal_append(
                "\nGuest session active.\n"
                "Use: login USER PASSWORD\n"
            );
            compositor_invalidate_all();
            break;

        case SYSTEM_COMMAND_TERMINAL_MODE:
            stop_gui();
            break;

        case SYSTEM_COMMAND_ACCENT_COLOR:
            show_color_dialog();
            break;

        case SYSTEM_COMMAND_THEME:
            desktop_services_cycle_theme();
            chrome_accent = desktop_services_theme()->accent;
            desktop_notify(desktop_services_theme_name(), 3500U);
            compositor_invalidate_all();
            break;

        case SYSTEM_COMMAND_WALLPAPER:
            desktop_services_cycle_wallpaper();
            desktop_notify(
                desktop_services_wallpaper_name(),
                3500U
            );
            compositor_invalidate_all();
            break;

        case SYSTEM_COMMAND_RELOAD_THEME:
            if (desktop_services_reload_external_theme())
            {
                chrome_accent = desktop_services_theme()->accent;
                (void)desktop_services_save();
                desktop_notify("External theme loaded", 3500U);
            }
            else
            {
                desktop_notify("External theme load failed", 5000U);
            }
            compositor_invalidate_all();
            break;

        case SYSTEM_COMMAND_SAVE_LAYOUT:
            save_window_layout();
            compositor_invalidate_all();
            break;

        case SYSTEM_COMMAND_CLEAR_NOTIFICATIONS:
        {
            uint32_t count = desktop_notification_count();

            while (count > 0)
            {
                const desktop_notification_t *notification =
                    desktop_notification_get(0);

                if (notification == NULL)
                {
                    break;
                }

                desktop_notification_dismiss(notification->id);
                count = desktop_notification_count();
            }

            compositor_invalidate_all();
            break;
        }

        case SYSTEM_COMMAND_INSTALLER:
            recovery_panel_active = false;
            windows[7].title = "Install LatterOS";
            activate_window(7);
            break;

        case SYSTEM_COMMAND_RECOVERY:
            recovery_panel_active = true;
            windows[7].title = "LatterOS Recovery";
            activate_window(7);
            break;

        case SYSTEM_COMMAND_ABOUT:
            show_about_dialog();
            break;

        case SYSTEM_COMMAND_RESTART:
            power_reboot();
            break;

        case SYSTEM_COMMAND_SHUTDOWN:
            power_shutdown();
            break;

        default:
            break;
    }
}

static bool handle_popup_click(int32_t x, int32_t y)
{
    if (!popup_menu.visible)
    {
        return false;
    }

    uint32_t command = 0;

    if (ui_menu_command_at(
        &popup_menu,
        x,
        y,
        &command
    ))
    {
        bool launcher = popup_is_launcher;
        bool system = popup_is_system;
        close_popup_menu();

        if (launcher)
        {
            if (command < GUI_WINDOW_COUNT)
            {
                uint8_t window_index = (uint8_t)command;
                force_fresh_activation = true;
                window_animation_cancel(window_index);
                animation_target_valid[window_index] = false;
                window_surface_invalidate(window_index);
                launcher_clean_launch_count++;
                activate_window(window_index);
                force_fresh_activation = false;
                compositor_invalidate_all();
            }
            else if (
                command >= LAUNCHER_RECENT_COMMAND_BASE &&
                command < LAUNCHER_RECENT_COMMAND_BASE +
                    DESKTOP_RECENT_FILE_COUNT
            )
            {
                uint32_t recent_index =
                    command - LAUNCHER_RECENT_COMMAND_BASE;
                const char *path =
                    desktop_recent_path(recent_index);
                vfs_node_t *node =
                    path == NULL ? NULL : vfs_open(path);

                if (node != NULL)
                {
                    open_associated_node(node);
                }
                else
                {
                    desktop_notify("Recent file is unavailable", 4500U);
                }
            }
        }
        else if (system)
        {
            execute_system_command(command);
        }
        else
        {
            execute_context_command(command);
        }

        return true;
    }

    if (ui_point_in_rect(x, y, &popup_menu.bounds))
    {
        return true;
    }

    close_popup_menu();
    compositor_invalidate_all();
    return false;
}

static bool handle_taskbar_click(int32_t x, int32_t y)
{
    ui_rect_t start = start_button_bounds();

    if (ui_point_in_rect(x, y, &start))
    {
        if (popup_is_launcher && popup_menu.visible)
        {
            close_popup_menu();
            compositor_invalidate_all();
        }
        else
        {
            open_launcher_menu();
        }

        return true;
    }

    ui_rect_t notification_tray = notification_tray_bounds();

    if (ui_point_in_rect(x, y, &notification_tray))
    {
        const desktop_notification_t *notification =
            desktop_notification_get(0);

        if (notification != NULL)
        {
            desktop_notification_dismiss(notification->id);
            desktop_notify("Notification dismissed", 1800U);
        }
        else
        {
            desktop_notify("No active notifications", 2500U);
        }

        compositor_invalidate_all();
        return true;
    }

    ui_rect_t clipboard_tray = clipboard_tray_bounds();

    if (ui_point_in_rect(x, y, &clipboard_tray))
    {
        desktop_notify(
            desktop_clipboard_has_text() ?
                desktop_clipboard_text() : "Clipboard is empty",
            4000U
        );
        compositor_invalidate_all();
        return true;
    }

    ui_rect_t system = system_button_bounds();

    if (ui_point_in_rect(x, y, &system))
    {
        if (popup_is_system && popup_menu.visible)
        {
            close_popup_menu();
            compositor_invalidate_all();
        }
        else
        {
            open_system_menu();
        }

        return true;
    }

    for (uint8_t index = 0; index < GUI_WINDOW_COUNT; index++)
    {
        ui_rect_t button = taskbar_app_button(index);

        if (!ui_point_in_rect(x, y, &button))
        {
            continue;
        }

        close_popup_menu();

        if (
            windows[index].state.visible &&
            !windows[index].state.minimized &&
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

static bool handle_terminal_click(
    gui_window_t *window,
    int32_t x,
    int32_t y
)
{
    ui_rect_t input = terminal_input_bounds(window);
    ui_rect_t scroll = terminal_scrollbar_bounds(window);

    if (ui_point_in_rect(x, y, &input))
    {
        terminal_focused = true;
        compositor_invalidate(&input);
        return true;
    }

    if (ui_point_in_rect(x, y, &scroll))
    {
        uint32_t visible = terminal_visible_lines(window);
        ui_scrollbar_t scrollbar = {
            .value = terminal_scroll_offset,
            .page_size = visible,
            .maximum = terminal_maximum_scroll(window)
        };

        terminal_scroll_offset =
            ui_control_scrollbar_value_from_pointer(
                &scroll,
                &scrollbar,
                y
            );

        invalidate_window(0);
        return true;
    }

    terminal_focused = true;
    return true;
}

static bool handle_explorer_toolbar_click(
    const gui_window_t *window,
    int32_t x,
    int32_t y
)
{
    for (uint32_t index = 0; index < 5U; index++)
    {
        ui_rect_t button =
            explorer_toolbar_button(window, index);

        if (!ui_point_in_rect(x, y, &button))
        {
            continue;
        }

        switch (index)
        {
            case 0:
                if (
                    explorer_directory != NULL &&
                    explorer_directory->parent != NULL
                )
                {
                    explorer_directory =
                        explorer_directory->parent;
                    explorer_selected = NULL;
                    explorer_scroll_offset = 0;
                    set_explorer_status("Moved to parent folder");
                    invalidate_window(1);
                }
                break;

            case 1:
                explorer_open_selected();
                break;

            case 2:
                show_new_file_dialog();
                break;

            case 3:
                show_new_folder_dialog();
                break;

            case 4:
                show_properties_dialog();
                break;

            default:
                break;
        }

        return true;
    }

    return false;
}

static bool handle_explorer_click(
    gui_window_t *window,
    int32_t x,
    int32_t y
)
{
    if (handle_explorer_toolbar_click(window, x, y))
    {
        return true;
    }

    ui_rect_t list = explorer_list_bounds(window);
    ui_rect_t scroll = explorer_scrollbar_bounds(window);

    if (ui_point_in_rect(x, y, &scroll))
    {
        uint32_t visible = explorer_visible_rows(window);
        ui_scrollbar_t scrollbar = {
            .value = explorer_scroll_offset,
            .page_size = visible,
            .maximum = explorer_maximum_scroll(window)
        };

        explorer_scroll_offset =
            ui_control_scrollbar_value_from_pointer(
                &scroll,
                &scrollbar,
                y
            );

        invalidate_window(1);
        return true;
    }

    if (!ui_point_in_rect(x, y, &list))
    {
        return false;
    }

    int32_t relative = y - list.y - 2;

    if (relative < 0)
    {
        return true;
    }

    uint32_t row =
        (uint32_t)relative / EXPLORER_ROW_HEIGHT;

    if (row >= explorer_visible_rows(window))
    {
        return true;
    }

    bool parent_entry = false;
    vfs_node_t *node = explorer_entry_at(
        explorer_scroll_offset + row,
        &parent_entry
    );

    if (node == NULL)
    {
        return true;
    }

    if (parent_entry)
    {
        explorer_directory = node;
        explorer_selected = NULL;
        explorer_scroll_offset = 0;
        set_explorer_status("Moved to parent folder");
    }
    else
    {
        explorer_selected = node;
        explorer_drag_candidate = true;
        explorer_drag_active = false;
        explorer_drag_start_x = x;
        explorer_drag_start_y = y;
        explorer_drag_node = node;
        build_node_path(
            node,
            explorer_drag_path,
            sizeof(explorer_drag_path)
        );
        set_explorer_status(
            node->type == VFS_NODE_DIRECTORY ?
                "Folder selected" : "File selected"
        );
    }

    invalidate_window(1);
    return true;
}

static bool handle_window_content_click(
    uint8_t index,
    int32_t x,
    int32_t y
)
{
    gui_window_t *window = &windows[index];
    terminal_focused = false;

    if (window->app == GUI_APP_TERMINAL)
    {
        return handle_terminal_click(window, x, y);
    }

    if (window->app == GUI_APP_FILE_EXPLORER)
    {
        return handle_explorer_click(window, x, y);
    }

    ui_rect_t content = window_content_bounds(window);
    bool changed = false;

    switch (window->app)
    {
        case GUI_APP_TEXT_EDITOR:
            changed = desktop_editor_handle_click(&content, x, y);

            if (desktop_editor_take_save_as_request())
            {
                show_editor_save_as_dialog();
                return true;
            }
            break;

        case GUI_APP_CALCULATOR:
            changed = calculator_handle_click(&content, x, y);
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

            if (changed)
            {
                /*
                 * Paint changes its canvas model continuously. The layered
                 * compositor must discard the cached window surface before
                 * presenting the damage, otherwise it redraws stale pixels.
                 */
                window_surface_invalidate(index);
            }

            if (
                changed &&
                damage.width != 0 &&
                damage.height != 0
            )
            {
                compositor_invalidate(&damage);
                return true;
            }

            break;
        }

        case GUI_APP_SETTINGS:
            changed = desktop_settings_handle_click(
                &content,
                x,
                y
            );

            if (changed)
            {
                compositor_invalidate_all();
                return true;
            }
            break;

        case GUI_APP_PROCESS_MANAGER:
            changed = process_manager_handle_click(
                &content,
                x,
                y
            );
            break;

        case GUI_APP_INSTALLER:
            if (recovery_panel_active)
            {
                changed = recovery_handle_click(&content, x, y);

                uint32_t reinstall_target = UINT32_MAX;

                if (recovery_take_reinstall_request(&reinstall_target))
                {
                    installer_selected_target = reinstall_target;
                    show_installer_confirmation();
                    return true;
                }

                if (changed)
                {
                    chrome_accent = desktop_services_theme()->accent;
                    compositor_invalidate_all();
                    return true;
                }
            }
            else
            {
                changed = handle_installer_click(&content, x, y);
            }
            break;

        default:
            break;
    }

    if (changed)
    {
        invalidate_window(index);
    }

    return changed;
}

static void handle_mouse_down(int32_t x, int32_t y)
{
    if (dialog.visible)
    {
        if (desktop_dialog_handle_click(&dialog, x, y))
        {
            compositor_invalidate_all();
        }

        handle_dialog_result();
        return;
    }

    uint32_t notification_count = desktop_notification_count();

    for (uint32_t index = 0; index < notification_count; index++)
    {
        ui_rect_t bounds = notification_bounds(index);

        if (ui_point_in_rect(x, y, &bounds))
        {
            const desktop_notification_t *notification =
                desktop_notification_get(index);

            if (notification != NULL)
            {
                desktop_notification_dismiss(notification->id);
            }

            compositor_invalidate_all();
            return;
        }
    }

    if (handle_popup_click(x, y))
    {
        return;
    }

    ui_rect_t taskbar = taskbar_bounds();

    if (ui_point_in_rect(x, y, &taskbar))
    {
        (void)handle_taskbar_click(x, y);
        return;
    }

    for (
        int32_t order = (int32_t)GUI_WINDOW_COUNT - 1;
        order >= 0;
        order--
    )
    {
        uint8_t index = window_order[order];
        gui_window_t *window = &windows[index];

        if (
            window_animation_active(index) ||
            !window->state.visible ||
            window->state.minimized
        )
        {
            continue;
        }

        uint8_t resize_edges = wm_resize_hit_test(
            &window->state,
            x,
            y,
            WINDOW_RESIZE_BORDER
        );

        bool inside = ui_point_in_rect(
            x,
            y,
            &window->state.bounds
        );

        if (resize_edges == WM_RESIZE_NONE && !inside)
        {
            continue;
        }

        activate_window(index);

        if (resize_edges != WM_RESIZE_NONE)
        {
            drag_cache_valid = false;
            window_surface_invalidate(index);
            (void)wm_begin_resize(
                &window->state,
                resize_edges,
                x,
                y
            );
            return;
        }

        if (!window->state.fullscreen)
        {
            ui_rect_t minimize = window_minimize_button(window);
            ui_rect_t maximize = window_maximize_button(window);
            ui_rect_t close = window_close_button(window);

            if (ui_point_in_rect(x, y, &minimize))
            {
                minimize_window(index);
                return;
            }

            if (ui_point_in_rect(x, y, &maximize))
            {
                toggle_maximize_window(index);
                return;
            }

            if (ui_point_in_rect(x, y, &close))
            {
                close_window(index);
                return;
            }

            ui_rect_t title = window_title_bar(window);

            if (ui_point_in_rect(x, y, &title))
            {
                if (wm_begin_drag(
                    &window->state,
                    x,
                    y,
                    graphics_width(),
                    graphics_height(),
                    TASKBAR_HEIGHT
                ))
                {
                    prepare_drag_cache(index);
                }

                return;
            }
        }

        (void)handle_window_content_click(index, x, y);
        return;
    }

    uint32_t icon = desktop_icon_at(x, y);

    if (icon < DESKTOP_ICON_COUNT)
    {
        desktop_selected_icon = icon;
        activate_window(desktop_icons[icon].window_index);
        compositor_invalidate_all();
        return;
    }

    desktop_selected_icon = UINT32_MAX;
    terminal_focused = false;
    close_popup_menu();
    compositor_invalidate_all();
}

static void handle_mouse_right_down(
    int32_t x,
    int32_t y
)
{
    if (dialog.visible)
    {
        return;
    }

    uint8_t active_index = active_window_index();

    if (
        active_index < GUI_WINDOW_COUNT &&
        windows[active_index].app == GUI_APP_FILE_EXPLORER &&
        ui_point_in_rect(
            x,
            y,
            &windows[active_index].state.bounds
        )
    )
    {
        ui_rect_t list =
            explorer_list_bounds(&windows[active_index]);

        if (ui_point_in_rect(x, y, &list))
        {
            int32_t relative = y - list.y - 2;

            if (relative >= 0)
            {
                uint32_t row =
                    (uint32_t)relative /
                        EXPLORER_ROW_HEIGHT;

                bool parent_entry = false;
                vfs_node_t *node = explorer_entry_at(
                    explorer_scroll_offset + row,
                    &parent_entry
                );

                if (node != NULL && !parent_entry)
                {
                    explorer_selected = node;
                    invalidate_window(active_index);
                }
            }
        }

        open_context_menu(x, y);
        return;
    }

    ui_menu_open(
        &popup_menu,
        x,
        y,
        SYSTEM_MENU_WIDTH,
        system_items,
        sizeof(system_items) / sizeof(system_items[0]),
        graphics_width(),
        graphics_height() - TASKBAR_HEIGHT
    );
    popup_is_launcher = false;
    popup_is_system = true;
    popup_is_desktop = true;
    compositor_invalidate_all();
}

static void handle_mouse_move(int32_t x, int32_t y)
{
    bool changed = false;

    uint32_t hovered_icon = desktop_icon_at(x, y);

    if (hovered_icon != desktop_hovered_icon)
    {
        desktop_hovered_icon = hovered_icon;
        compositor_invalidate_all();
    }

    if (
        explorer_drag_candidate &&
        explorer_drag_node != NULL &&
        (
            absolute_value(x - explorer_drag_start_x) >=
                FILE_DRAG_THRESHOLD ||
            absolute_value(y - explorer_drag_start_y) >=
                FILE_DRAG_THRESHOLD
        )
    )
    {
        explorer_drag_candidate = false;
        explorer_drag_active = true;
        desktop_notify("Drag item to Editor or a folder", 2500U);
        compositor_invalidate_all();
    }

    if (dialog.visible)
    {
        if (desktop_dialog_handle_mouse_move(&dialog, x, y))
        {
            compositor_invalidate_all();
        }

        return;
    }

    if (popup_menu.visible)
    {
        uint32_t previous = popup_menu.hovered_index;
        ui_menu_update_hover(&popup_menu, x, y);

        if (previous != popup_menu.hovered_index)
        {
            compositor_invalidate(&popup_menu.bounds);
        }
    }

    for (uint8_t index = 0; index < GUI_WINDOW_COUNT; index++)
    {
        gui_window_t *window = &windows[index];

        if (window->state.resizing)
        {
            window_surface_invalidate(index);
            ui_rect_t old = window_visual_bounds(window);

            if (wm_update_resize(
                &window->state,
                x,
                y,
                graphics_width(),
                graphics_height(),
                TASKBAR_HEIGHT
            ))
            {
                ui_rect_t next = window_visual_bounds(window);
                ui_rect_t damage = ui_union(&old, &next);
                compositor_invalidate(&damage);
                changed = true;
            }
        }
        else if (window->state.dragging)
        {
            ui_rect_t old = window_visual_bounds(window);

            if (wm_update_drag(
                &window->state,
                x,
                y,
                graphics_width(),
                graphics_height(),
                TASKBAR_HEIGHT,
                WINDOW_SNAP_THRESHOLD
            ))
            {
                ui_rect_t next = window_visual_bounds(window);
                ui_rect_t damage = ui_union(&old, &next);
                compositor_invalidate(&damage);
                changed = true;
            }
        }
    }

    if (changed)
    {
        if (compositor_has_damage())
        {
            compositor_render();
        }

        return;
    }

    uint8_t active_index = active_window_index();

    if (
        active_index < GUI_WINDOW_COUNT &&
        windows[active_index].app == GUI_APP_FILE_EXPLORER
    )
    {
        ui_rect_t list =
            explorer_list_bounds(&windows[active_index]);

        uint32_t previous = explorer_hovered_row;
        explorer_hovered_row = UINT32_MAX;

        if (ui_point_in_rect(x, y, &list))
        {
            int32_t relative = y - list.y - 2;

            if (relative >= 0)
            {
                uint32_t row =
                    (uint32_t)relative /
                        EXPLORER_ROW_HEIGHT;

                if (row < explorer_visible_rows(
                    &windows[active_index]
                ))
                {
                    explorer_hovered_row = row;
                }
            }
        }

        if (previous != explorer_hovered_row)
        {
            invalidate_window(active_index);
        }
    }

    if (
        windows[2].state.visible &&
        !windows[2].state.minimized
    )
    {
        ui_rect_t editor_content =
            window_content_bounds(&windows[2]);

        if (desktop_editor_handle_mouse_move(
            &editor_content,
            x,
            y
        ))
        {
            invalidate_window(2);
        }
    }

    for (uint8_t index = 0; index < GUI_WINDOW_COUNT; index++)
    {
        gui_window_t *window = &windows[index];

        if (
            !window->state.visible ||
            window->state.minimized ||
            window->app != GUI_APP_PAINT
        )
        {
            continue;
        }

        ui_rect_t content = window_content_bounds(window);
        ui_rect_t damage = { 0, 0, 0, 0 };

        if (paint_handle_mouse_move(
            &content,
            x,
            y,
            &damage
        ))
        {
            /* Keep every live stroke segment visible in the same frame. */
            window_surface_invalidate(index);

            if (damage.width != 0 && damage.height != 0)
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


static bool point_targets_editor(int32_t x, int32_t y)
{
    ui_rect_t task_button = taskbar_app_button(2);

    if (ui_point_in_rect(x, y, &task_button))
    {
        return true;
    }

    for (uint32_t index = 0; index < DESKTOP_ICON_COUNT; index++)
    {
        if (desktop_icons[index].window_index != GUI_APP_TEXT_EDITOR)
        {
            continue;
        }

        ui_rect_t icon = desktop_icon_bounds(index);

        if (ui_point_in_rect(x, y, &icon))
        {
            return true;
        }
    }

    return
        windows[2].state.visible &&
        !windows[2].state.minimized &&
        ui_point_in_rect(x, y, &windows[2].state.bounds);
}

static vfs_node_t *drop_target_directory(
    int32_t x,
    int32_t y
)
{
    gui_window_t *window = &windows[1];

    if (
        !window->state.visible ||
        window->state.minimized ||
        !ui_point_in_rect(x, y, &window->state.bounds)
    )
    {
        return NULL;
    }

    ui_rect_t list = explorer_list_bounds(window);

    if (!ui_point_in_rect(x, y, &list))
    {
        return explorer_directory;
    }

    int32_t relative = y - list.y - 2;

    if (relative < 0)
    {
        return explorer_directory;
    }

    uint32_t row =
        (uint32_t)relative / EXPLORER_ROW_HEIGHT;
    bool parent_entry = false;
    vfs_node_t *node = explorer_entry_at(
        explorer_scroll_offset + row,
        &parent_entry
    );

    if (node == NULL)
    {
        return explorer_directory;
    }

    if (parent_entry || node->type == VFS_NODE_DIRECTORY)
    {
        return node;
    }

    return explorer_directory;
}

static void finish_explorer_drag(int32_t x, int32_t y)
{
    if (!explorer_drag_active || explorer_drag_node == NULL)
    {
        explorer_drag_candidate = false;
        explorer_drag_active = false;
        explorer_drag_node = NULL;
        explorer_drag_path[0] = '\0';
        return;
    }

    vfs_node_t *dragged = explorer_drag_node;

    if (
        dragged->type == VFS_NODE_FILE &&
        point_targets_editor(x, y)
    )
    {
        open_associated_node(dragged);
        desktop_notify("Dropped into associated application", 3500U);
    }
    else
    {
        vfs_node_t *directory = drop_target_directory(x, y);

        if (
            directory != NULL &&
            directory->type == VFS_NODE_DIRECTORY &&
            directory != dragged &&
            directory != dragged->parent
        )
        {
            char destination[EXPLORER_PATH_CAPACITY];

            if (
                destination_path_for_node(
                    directory,
                    dragged->name,
                    destination,
                    sizeof(destination)
                ) &&
                vfs_move(explorer_drag_path, destination)
            )
            {
                desktop_notify("Item moved by drag and drop", 3500U);
                explorer_selected = NULL;
                refresh_explorer();
            }
            else
            {
                desktop_notify("Drag and drop move failed", 4500U);
            }
        }
        else
        {
            desktop_notify("Drag cancelled", 2200U);
        }
    }

    explorer_drag_candidate = false;
    explorer_drag_active = false;
    explorer_drag_node = NULL;
    explorer_drag_path[0] = '\0';
    compositor_invalidate_all();
}

static void handle_mouse_up(void)
{
    finish_explorer_drag(cursor_x, cursor_y);
    bool interacted = false;

    for (uint8_t index = 0; index < GUI_WINDOW_COUNT; index++)
    {
        gui_window_t *window = &windows[index];

        if (
            window->state.dragging ||
            window->state.resizing
        )
        {
            interacted = wm_end_interaction(
                &window->state,
                graphics_width(),
                graphics_height(),
                TASKBAR_HEIGHT
            ) || interacted;
        }
    }

    drag_cache_valid = false;
    desktop_editor_handle_mouse_up();
    paint_handle_mouse_up();

    if (interacted)
    {
        window_surface_invalidate_all();

        for (uint32_t index = 0; index < GUI_WINDOW_COUNT; index++)
        {
            const wm_window_state_t *state = &windows[index].state;
            const ui_rect_t *bounds = &state->bounds;

            if (
                state->maximized ||
                state->snap != WM_SNAP_NONE
            )
            {
                bounds = &state->restore_bounds;
            }

            desktop_services_store_window(index, bounds);
        }

        (void)desktop_services_save();
        compositor_invalidate_all();
    }
}

static void handle_key(char character)
{
    if (dialog.visible)
    {
        if (desktop_dialog_handle_key(&dialog, character))
        {
            compositor_invalidate_all();
        }

        handle_dialog_result();
        return;
    }

    if (popup_is_launcher && popup_menu.visible)
    {
        bool changed = false;

        if (character == '\b')
        {
            if (launcher_query_length > 0)
            {
                launcher_query[--launcher_query_length] = '\0';
                changed = true;
            }
        }
        else if (
            character >= 32 && character <= 126 &&
            launcher_query_length + 1U <
                LAUNCHER_QUERY_CAPACITY
        )
        {
            launcher_query[launcher_query_length++] = character;
            launcher_query[launcher_query_length] = '\0';
            changed = true;
        }

        if (changed)
        {
            open_launcher_menu();
        }

        if (character != 27)
        {
            return;
        }
    }

    if (character == 27)
    {
        uint8_t fullscreen = active_fullscreen_window();

        if (fullscreen < GUI_WINDOW_COUNT)
        {
            toggle_fullscreen_window(fullscreen);
            return;
        }

        if (popup_menu.visible)
        {
            close_popup_menu();
            compositor_invalidate_all();
        }

        return;
    }

    uint8_t index = active_window_index();

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
            if (terminal_has_selection())
            {
                terminal_delete_selection();
            }
            else if (terminal_input_length > 0)
            {
                terminal_input[--terminal_input_length] = '\0';
                terminal_clear_selection();
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
            terminal_input_length + 1U <
                TERMINAL_INPUT_CAPACITY
        )
        {
            terminal_delete_selection();
            terminal_input[terminal_input_length++] = character;
            terminal_input[terminal_input_length] = '\0';
            terminal_clear_selection();
            invalidate_window(index);
        }

        return;
    }

    bool changed = false;

    if (window->app == GUI_APP_TEXT_EDITOR)
    {
        changed = desktop_editor_handle_key(character);
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

static void handle_shortcut(gui_shortcut_t shortcut)
{
    if (dialog.visible)
    {
        if (shortcut == GUI_SHORTCUT_CLOSE_WINDOW)
        {
            desktop_dialog_close(
                &dialog,
                DESKTOP_DIALOG_RESULT_CANCELLED
            );

            handle_dialog_result();
            compositor_invalidate_all();
        }

        return;
    }

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
            if (popup_is_launcher && popup_menu.visible)
            {
                close_popup_menu();
                compositor_invalidate_all();
            }
            else
            {
                open_launcher_menu();
            }
            break;

        case GUI_SHORTCUT_OPEN_TERMINAL:
            activate_window(0);
            break;

        case GUI_SHORTCUT_OPEN_FILES:
            activate_window(1);
            break;

        case GUI_SHORTCUT_SHOW_DESKTOP:
            show_desktop();
            break;

        case GUI_SHORTCUT_HELP:
            show_help_dialog();
            break;

        case GUI_SHORTCUT_RENAME:
            if (
                index < GUI_WINDOW_COUNT &&
                windows[index].app == GUI_APP_FILE_EXPLORER
            )
            {
                show_rename_dialog();
            }
            break;

        case GUI_SHORTCUT_REFRESH:
            if (
                index < GUI_WINDOW_COUNT &&
                windows[index].app == GUI_APP_FILE_EXPLORER
            )
            {
                refresh_explorer();
            }
            break;

        case GUI_SHORTCUT_SELECT_ALL:
            if (
                index < GUI_WINDOW_COUNT &&
                windows[index].app == GUI_APP_TERMINAL &&
                terminal_focused
            )
            {
                terminal_selection_start = 0;
                terminal_selection_end = terminal_input_length;
                invalidate_window(index);
            }
            else if (
                index < GUI_WINDOW_COUNT &&
                windows[index].app == GUI_APP_TEXT_EDITOR
            )
            {
                desktop_editor_select_all();
                invalidate_window(index);
            }
            else
            {
                desktop_notify(
                    "Select All is available in focused text fields",
                    3500U
                );
            }
            break;

        case GUI_SHORTCUT_COPY:
            if (
                index < GUI_WINDOW_COUNT &&
                windows[index].app == GUI_APP_TERMINAL &&
                terminal_focused
            )
            {
                terminal_copy_selection(false);
            }
            else if (
                index < GUI_WINDOW_COUNT &&
                windows[index].app == GUI_APP_FILE_EXPLORER
            )
            {
                copy_selected_file(false);
            }
            else if (
                index < GUI_WINDOW_COUNT &&
                windows[index].app == GUI_APP_TEXT_EDITOR
            )
            {
                if (desktop_editor_copy(false))
                {
                    invalidate_window(index);
                    desktop_notify("Editor selection copied", 2500U);
                }
                else
                {
                    desktop_notify("No editor text selected", 3000U);
                }
            }
            else
            {
                desktop_notify("Nothing selected to copy", 3000U);
            }
            break;

        case GUI_SHORTCUT_CUT:
            if (
                index < GUI_WINDOW_COUNT &&
                windows[index].app == GUI_APP_TERMINAL &&
                terminal_focused
            )
            {
                terminal_copy_selection(true);
            }
            else if (
                index < GUI_WINDOW_COUNT &&
                windows[index].app == GUI_APP_FILE_EXPLORER
            )
            {
                copy_selected_file(true);
            }
            else if (
                index < GUI_WINDOW_COUNT &&
                windows[index].app == GUI_APP_TEXT_EDITOR
            )
            {
                if (desktop_editor_copy(true))
                {
                    invalidate_window(index);
                    desktop_notify("Editor selection cut", 2500U);
                }
                else
                {
                    desktop_notify("No editor text selected", 3000U);
                }
            }
            else
            {
                desktop_notify("Nothing selected to cut", 3000U);
            }
            break;

        case GUI_SHORTCUT_SAVE:
            if (
                index < GUI_WINDOW_COUNT &&
                windows[index].app == GUI_APP_TEXT_EDITOR
            )
            {
                if (desktop_editor_save())
                {
                    window_surface_invalidate(index);
                    invalidate_window(index);
                }
                else if (desktop_editor_take_save_as_request())
                {
                    show_editor_save_as_dialog();
                }
            }
            break;

        case GUI_SHORTCUT_PASTE:
            if (
                index < GUI_WINDOW_COUNT &&
                windows[index].app == GUI_APP_TERMINAL &&
                terminal_focused
            )
            {
                terminal_paste();
            }
            else if (
                index < GUI_WINDOW_COUNT &&
                windows[index].app == GUI_APP_FILE_EXPLORER
            )
            {
                (void)paste_file_into_explorer();
            }
            else if (
                index < GUI_WINDOW_COUNT &&
                windows[index].app == GUI_APP_TEXT_EDITOR
            )
            {
                if (desktop_editor_paste())
                {
                    invalidate_window(index);
                    desktop_notify("Clipboard pasted into Editor", 3000U);
                }
                else
                {
                    desktop_notify("Clipboard is empty", 3000U);
                }
            }
            else
            {
                desktop_notify("Clipboard is empty", 3000U);
            }
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
        if (
            installer_running() &&
            event.type != GUI_EVENT_MOUSE_MOVE
        )
        {
            continue;
        }

        switch (event.type)
        {
            case GUI_EVENT_MOUSE_MOVE:
                handle_mouse_move(event.x, event.y);
                break;

            case GUI_EVENT_MOUSE_DOWN:
                handle_mouse_down(event.x, event.y);
                break;

            case GUI_EVENT_MOUSE_UP:
                handle_mouse_up();
                break;

            case GUI_EVENT_MOUSE_RIGHT_DOWN:
                handle_mouse_right_down(event.x, event.y);
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

        handle_dialog_result();
    }
}

static void commit_cursor_position(void)
{
    int32_t new_x =
        (cursor_target_x_fixed +
            GUI_POINTER_FIXED_ONE / 2) >>
            GUI_POINTER_FIXED_SHIFT;

    int32_t new_y =
        (cursor_target_y_fixed +
            GUI_POINTER_FIXED_ONE / 2) >>
            GUI_POINTER_FIXED_SHIFT;

    if (new_x == cursor_x && new_y == cursor_y)
    {
        return;
    }

    int32_t old_x = cursor_x;
    int32_t old_y = cursor_y;

    cursor_x = new_x;
    cursor_y = new_y;

    if (cursor_visible)
    {
        if (virtio_gpu_available())
        {
            ui_rect_t old_cursor = {
                .x = old_x,
                .y = old_y,
                .width = GUI_CURSOR_SIZE,
                .height = GUI_CURSOR_SIZE
            };

            ui_rect_t new_cursor = {
                .x = cursor_x,
                .y = cursor_y,
                .width = GUI_CURSOR_SIZE,
                .height = GUI_CURSOR_SIZE
            };

            compositor_invalidate(&old_cursor);
            compositor_invalidate(&new_cursor);
        }
        else
        {
            graphics_cursor_move(cursor_x, cursor_y);
        }
    }

    gui_event_t move = {
        .type = GUI_EVENT_MOUSE_MOVE,
        .x = cursor_x,
        .y = cursor_y,
        .character = 0,
        .shortcut = GUI_SHORTCUT_NONE
    };

    queue_event(move);
}

static void update_mouse_events(void)
{
    mouse_state_t state;
    mouse_get_state(&state);

    bool packet_changed =
        state.packet_count != last_mouse_packets;

    bool left_changed =
        state.left_button != last_left_button;

    bool right_changed =
        state.right_button != last_right_button;

    if (packet_changed)
    {
        int32_t delta_x = clamp_pointer_delta(
            state.x - last_mouse_x
        );

        int32_t delta_y = clamp_pointer_delta(
            state.y - last_mouse_y
        );

        int32_t scale = pointer_scale_fixed(
            delta_x,
            delta_y
        );

        cursor_target_x_fixed += delta_x * scale;
        cursor_target_y_fixed += delta_y * scale;
        clamp_cursor_target();

        last_mouse_x = state.x;
        last_mouse_y = state.y;
        last_mouse_packets = state.packet_count;
    }

    if (packet_changed || left_changed || right_changed)
    {
        commit_cursor_position();
    }

    if (state.left_button && !last_left_button)
    {
        queue_event((gui_event_t){
            .type = GUI_EVENT_MOUSE_DOWN,
            .x = cursor_x,
            .y = cursor_y,
            .character = 0,
            .shortcut = GUI_SHORTCUT_NONE
        });
    }

    if (!state.left_button && last_left_button)
    {
        queue_event((gui_event_t){
            .type = GUI_EVENT_MOUSE_UP,
            .x = cursor_x,
            .y = cursor_y,
            .character = 0,
            .shortcut = GUI_SHORTCUT_NONE
        });
    }

    if (state.right_button && !last_right_button)
    {
        queue_event((gui_event_t){
            .type = GUI_EVENT_MOUSE_RIGHT_DOWN,
            .x = cursor_x,
            .y = cursor_y,
            .character = 0,
            .shortcut = GUI_SHORTCUT_NONE
        });
    }

    last_left_button = state.left_button;
    last_right_button = state.right_button;
}

static void stop_gui(void)
{
    graphics_cursor_hide();
    cursor_visible = false;
    drag_cache_valid = false;
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

static void initialize_window(
    uint8_t index,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height,
    uint32_t minimum_width,
    uint32_t minimum_height,
    const char *title,
    gui_app_type_t app
)
{
    ui_rect_t bounds = {
        .x = x,
        .y = y,
        .width = width,
        .height = height
    };

    wm_initialize_window(
        &windows[index].state,
        &bounds,
        minimum_width,
        minimum_height
    );

    windows[index].title = title;
    windows[index].app = app;

    wm_clamp_window(
        &windows[index].state,
        graphics_width(),
        graphics_height(),
        TASKBAR_HEIGHT
    );

    windows[index].state.restore_bounds =
        windows[index].state.bounds;
}

static void start_gui(void)
{
    active = true;
    start_requested = false;

    graphics_set_deferred(true);
    compositor_init(render_gui_scene);
    window_surface_init();
    window_animation_init();

    for (uint32_t index = 0; index < GUI_WINDOW_COUNT; index++)
    {
        animation_target_valid[index] = false;
    }

    if (boot_mode_conservative_graphics())
    {
        desktop_notify(
            boot_mode_is_recovery() ?
                "Recovery: conservative software graphics active" :
                "Safe Mode: conservative software graphics active",
            6000U
        );
    }
    else if (virtio_gpu_available())
    {
        desktop_notify(
            "Display: Virtio-GPU scanout with stable composited cursor",
            5000U
        );
    }
    else
    {
        desktop_notify(
            display_triple_buffered() ?
                "Display: triple-buffered software fallback" :
                "Display: direct framebuffer fallback",
            5000U
        );

        desktop_notify(
            virtio_gpu_status_text(),
            7000U
        );
    }

    event_read_index = 0;
    event_write_index = 0;

    uint32_t screen_width = graphics_width();
    uint32_t screen_height = graphics_height();

    cursor_x = (int32_t)(screen_width / 2U);
    cursor_y = (int32_t)(screen_height / 2U);
    cursor_target_x_fixed =
        cursor_x * GUI_POINTER_FIXED_ONE;
    cursor_target_y_fixed =
        cursor_y * GUI_POINTER_FIXED_ONE;
    cursor_visible = true;
    drag_cache_valid = false;
    drag_cache_window = GUI_WINDOW_COUNT;
    if (virtio_gpu_available())
    {
        /* The pointer is rendered by render_gui_scene(). */
        graphics_cursor_hide();
    }
    else
    {
        graphics_cursor_show(cursor_x, cursor_y);
    }

    last_frame_tick = timer_ticks();
    frame_accumulator = timer_frequency();

    mouse_state_t state;
    mouse_get_state(&state);
    last_mouse_x = state.x;
    last_mouse_y = state.y;
    last_mouse_packets = state.packet_count;
    last_left_button = state.left_button;
    last_right_button = state.right_button;

    initialize_window(
        0,
        (int32_t)(screen_width / 2U) - 390,
        70,
        650,
        390,
        430,
        270,
        "LatterOS Terminal",
        GUI_APP_TERMINAL
    );

    initialize_window(
        1,
        (int32_t)(screen_width / 2U) - 220,
        90,
        620,
        500,
        470,
        370,
        "File Explorer",
        GUI_APP_FILE_EXPLORER
    );

    initialize_window(
        2,
        (int32_t)(screen_width / 2U) - 280,
        70,
        580,
        430,
        430,
        320,
        "Text Editor",
        GUI_APP_TEXT_EDITOR
    );

    initialize_window(
        3,
        (int32_t)(screen_width / 2U) - 155,
        100,
        320,
        350,
        280,
        320,
        "Calculator",
        GUI_APP_CALCULATOR
    );

    initialize_window(
        4,
        (int32_t)(screen_width / 2U) - 200,
        90,
        420,
        330,
        340,
        260,
        "Paint",
        GUI_APP_PAINT
    );

    initialize_window(
        5,
        (int32_t)(screen_width / 2U) - 185,
        115,
        470,
        410,
        420,
        370,
        "Settings",
        GUI_APP_SETTINGS
    );

    initialize_window(
        6,
        (int32_t)(screen_width / 2U) - 280,
        100,
        580,
        360,
        460,
        290,
        "Process Manager",
        GUI_APP_PROCESS_MANAGER
    );

    initialize_window(
        7,
        (int32_t)(screen_width / 2U) - 310,
        86,
        620,
        420,
        520,
        330,
        recovery_panel_active ?
            "LatterOS Recovery" : "Install LatterOS",
        GUI_APP_INSTALLER
    );

    for (uint32_t index = 0; index < GUI_WINDOW_COUNT; index++)
    {
        ui_rect_t restored;

        if (desktop_services_restore_window(index, &restored))
        {
            windows[index].state.bounds = restored;
            wm_clamp_window(
                &windows[index].state,
                screen_width,
                screen_height,
                TASKBAR_HEIGHT
            );
            windows[index].state.restore_bounds =
                windows[index].state.bounds;
        }
    }

    refresh_layout_observer();
    layout_autosave_pending = false;

    window_order[0] = 2;
    window_order[1] = 3;
    window_order[2] = 4;
    window_order[3] = 5;
    window_order[4] = 6;
    window_order[5] = 7;
    window_order[6] = 1;
    window_order[7] = 0;

    gui_terminal_reset_output();
    gui_terminal_append(
        "LatterOS desktop terminal ready.\n"
        "Type help to list shell commands.\n"
        "F1 opens desktop help.\n"
    );

    clear_text(terminal_input, sizeof(terminal_input));
    terminal_input_length = 0;
    terminal_selection_start = 0;
    terminal_selection_end = 0;
    terminal_focused = false;

    explorer_directory = vfs_root();
    explorer_selected = NULL;
    explorer_scroll_offset = 0;
    explorer_hovered_row = UINT32_MAX;
    explorer_drag_candidate = false;
    explorer_drag_active = false;
    explorer_drag_node = NULL;
    explorer_drag_path[0] = '\0';
    clipboard_file_cut = false;
    clipboard_file_path[0] = '\0';
    set_explorer_status("Ready");

    desktop_dialog_init(&dialog);
    pending_action = PENDING_ACTION_NONE;
    pending_node = NULL;
    pending_path[0] = '\0';

    popup_menu.visible = false;
    popup_menu.hovered_index = UINT32_MAX;
    popup_is_launcher = false;
    popup_is_system = false;
    popup_is_desktop = false;
    launcher_query[0] = '\0';
    launcher_query_length = 0;
    launcher_dynamic_count = 0;
    desktop_hovered_icon = UINT32_MAX;
    desktop_selected_icon = UINT32_MAX;
    installer_refresh();
    installer_selected_target =
        installer_target_count() == 0 ? UINT32_MAX : 0U;
    clear_text(installer_status, sizeof(installer_status));
    copy_text(
        installer_status,
        sizeof(installer_status),
        installer_target_count() == 0 ?
            installer_last_error() :
            "Select the dedicated blank target disk"
    );

    clock_text[0] = '-';
    clock_text[1] = '-';
    clock_text[2] = ':';
    clock_text[3] = '-';
    clock_text[4] = '-';
    clock_text[5] = '\0';
    last_clock_update = 0;
    update_clock(true);

    keyboard_set_character_handler(gui_keyboard_input);
    keyboard_set_event_handler(gui_keyboard_event);

    desktop_notify(
        animations_enabled ?
            "Compositor: layered window surfaces and animations ready" :
            "Compositor: animations disabled for conservative boot mode",
        5000U
    );

    if (first_boot_notification_pending)
    {
        desktop_notify(
            "Welcome to the installed LatterOS system",
            7000U
        );
        first_boot_notification_pending = false;
    }

    if (boot_health_previous_incomplete())
    {
        desktop_notify(
            "Previous boot did not reach the desktop; Compatibility Mode is available",
            8000U
        );
    }

    if (boot_mode_is_safe())
    {
        desktop_notify(
            "Safe Mode: use Recovery tools for disk checks",
            6500U
        );
    }

    if (boot_mode_is_recovery())
    {
        recovery_panel_active = true;
        windows[7].title = "LatterOS Recovery";
        activate_window(7);
        desktop_notify(
            "Recovery mode started - destructive actions require confirmation",
            7000U
        );
    }

    if (boot_mode_is_hardware_test())
    {
        settings_page = SETTINGS_PAGE_HARDWARE;
        activate_window(5);
        desktop_notify(
            "Hardware Test mode: conservative graphics and compatibility dashboard active",
            7500U
        );
    }

    if (boot_mode_is_compatibility())
    {
        settings_page = SETTINGS_PAGE_STABILITY;
        activate_window(5);
        (void)stability_monitor_start_test();
        visible_stability_state = stability_monitor_state();
        desktop_notify(
            "Compatibility Mode: software graphics and stability testing active",
            7500U
        );
    }

    if (boot_mode_is_virtualbox())
    {
        settings_page = SETTINGS_PAGE_HARDWARE;
        activate_window(5);
        desktop_notify(
            "VirtualBox Mode: EFI framebuffer, PS/2 input, and AHCI profile active",
            8000U
        );
    }

    boot_health_mark_desktop_ready();
    visible_notification_count = desktop_notification_count();
    compositor_invalidate_all();
}

void gui_init(void)
{
    boot_mode_init();
    app_suite_init();
    (void)user_home_ensure();
    release_info_init();
    boot_health_init();
    hardware_compat_init();
    stability_monitor_init();
    package_manager_init();
    first_boot_notification_pending =
        boot_mode_prepare_first_boot();
    visible_stability_state = stability_monitor_state();
    layout_observer_ready = false;
    layout_autosave_pending = false;
    layout_autosave_deadline = 0ULL;
    layout_autosave_count = 0ULL;
    launcher_clean_launch_count = 0ULL;
    animation_surface_recovery_count = 0ULL;
    manual_surface_rebuild_count = 0ULL;
    force_fresh_activation = false;
    desktop_services_init();
    desktop_editor_init();
    installer_init();
    recovery_init();
    window_surface_init();
    window_animation_init();

    initialized = true;
    active = false;
    start_requested = false;
    animations_enabled = !boot_mode_conservative_graphics();
    recovery_panel_active = boot_mode_is_recovery();
    settings_page = SETTINGS_PAGE_APPEARANCE;
    event_read_index = 0;
    event_write_index = 0;
    cursor_visible = false;
    drag_cache_valid = false;
    drag_cache_window = GUI_WINDOW_COUNT;
    drag_cache_width = 0;
    drag_cache_height = 0;
    last_frame_tick = 0;
    frame_accumulator = 0;
    last_clock_update = 0;
    chrome_accent = desktop_services_theme()->accent;

    desktop_dialog_init(&dialog);
    popup_menu.visible = false;
    popup_menu.hovered_index = UINT32_MAX;
    popup_is_launcher = false;
    popup_is_system = false;
    popup_is_desktop = false;
    launcher_query[0] = '\0';
    launcher_query_length = 0;
    desktop_hovered_icon = UINT32_MAX;
    desktop_selected_icon = UINT32_MAX;
    visible_notification_count = 0;
    installer_selected_target = UINT32_MAX;
    installer_visible_progress = 0U;
    clear_text(installer_status, sizeof(installer_status));
    clear_text(installer_report.message, sizeof(installer_report.message));

    for (uint32_t index = 0; index < GUI_WINDOW_COUNT; index++)
    {
        animation_target_valid[index] = false;
    }
}

void gui_notify_session_changed(void)
{
    if (active)
    {
        invalidate_taskbar();
    }
}

void gui_request_start(void)
{
    if (!initialized)
    {
        gui_init();
    }

    start_requested = true;
}

static void update_installer_background(void)
{
    if (installer_running())
    {
        uint32_t percentage = installer_visible_progress;
        char message[INSTALLER_MESSAGE_CAPACITY];
        clear_text(message, sizeof(message));

        if (installer_progress(
            &percentage,
            message,
            sizeof(message)
        ))
        {
            clear_text(
                installer_status,
                sizeof(installer_status)
            );
            append_text(
                installer_status,
                sizeof(installer_status),
                "["
            );
            append_unsigned_decimal(
                installer_status,
                sizeof(installer_status),
                percentage
            );
            append_text(
                installer_status,
                sizeof(installer_status),
                "%] "
            );
            append_text(
                installer_status,
                sizeof(installer_status),
                message
            );
            installer_visible_progress = percentage;
            invalidate_window(7);
        }

        return;
    }

    installer_report_t completed;

    if (!installer_take_completion(&completed))
    {
        return;
    }

    installer_report = completed;
    installer_visible_progress = completed.success ? 100U : 0U;
    copy_text(
        installer_status,
        sizeof(installer_status),
        completed.message
    );

    desktop_notify(
        completed.success ?
            "LatterOS installed and verified" :
            "LatterOS installation failed",
        completed.success ? 6000U : 8000U
    );

    installer_refresh();
    invalidate_window(7);
    compositor_invalidate_all();
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

    display_update();
    update_installer_background();

    bool stability_changed = stability_monitor_update();
    stability_state_t stability_state = stability_monitor_state();

    if (stability_state != visible_stability_state)
    {
        visible_stability_state = stability_state;

        if (
            stability_state == STABILITY_STATE_PASSED ||
            stability_state == STABILITY_STATE_WARNING ||
            stability_state == STABILITY_STATE_FAILED
        )
        {
            desktop_notify(stability_monitor_last_message(), 6000U);
        }

        stability_changed = true;
    }

    if (stability_changed && settings_page == SETTINGS_PAGE_STABILITY)
    {
        invalidate_window(5);
    }

    bool render_frame = frame_is_due();

    update_clock(false);
    desktop_notifications_update();
    uint32_t notification_count = desktop_notification_count();

    if (notification_count != visible_notification_count)
    {
        visible_notification_count = notification_count;
        compositor_invalidate_all();
    }

    update_window_animations();
    update_mouse_events();
    process_events();
    update_layout_autosave();

    if (!active)
    {
        return;
    }

    if (render_frame && compositor_has_damage())
    {
        compositor_render();
    }

    display_update();
}

bool gui_is_active(void)
{
    return active;
}
