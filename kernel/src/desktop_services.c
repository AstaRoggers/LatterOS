#include "desktop_services.h"

#include "timer.h"
#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DESKTOP_CONFIG_PATH "/home/user/.latteros-desktop.cfg"
#define DESKTOP_THEME_PATH  "/home/user/latteros.theme"
#define DESKTOP_CONFIG_BUFFER_SIZE 4096U

static gui_theme_t current_theme;
static desktop_theme_kind_t theme_kind;
static desktop_wallpaper_t wallpaper_kind;
static desktop_saved_window_t saved_windows[
    DESKTOP_SERVICE_WINDOW_COUNT
];
static char recent_files[
    DESKTOP_RECENT_FILE_COUNT
][DESKTOP_RECENT_PATH_CAPACITY];
static uint32_t recent_file_count;
static char clipboard[DESKTOP_CLIPBOARD_CAPACITY];
static size_t clipboard_length;
static uint64_t clipboard_generation;
static desktop_notification_t notifications[
    DESKTOP_NOTIFICATION_CAPACITY
];
static uint64_t next_notification_id;
static uint64_t notification_total;
static bool initialized;
static char configuration_buffer[DESKTOP_CONFIG_BUFFER_SIZE];
static char theme_buffer[2048];

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

static bool strings_equal(
    const char *first,
    const char *second
)
{
    if (first == NULL || second == NULL)
    {
        return false;
    }

    size_t index = 0;

    while (
        first[index] != '\0' &&
        second[index] != '\0'
    )
    {
        if (first[index] != second[index])
        {
            return false;
        }

        index++;
    }

    return first[index] == second[index];
}

static bool string_ends_with_case_insensitive(
    const char *text,
    const char *suffix
)
{
    size_t text_length = string_length(text);
    size_t suffix_length = string_length(suffix);

    if (suffix_length > text_length)
    {
        return false;
    }

    size_t offset = text_length - suffix_length;

    for (size_t index = 0; index < suffix_length; index++)
    {
        char first = text[offset + index];
        char second = suffix[index];

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
            return false;
        }
    }

    return true;
}

static void clear_bytes(void *pointer, size_t count)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0; index < count; index++)
    {
        bytes[index] = 0;
    }
}

static bool copy_text(
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
        return false;
    }

    size_t index = 0;

    while (
        source[index] != '\0' &&
        index + 1U < capacity
    )
    {
        destination[index] = source[index];
        index++;
    }

    destination[index] = '\0';
    return source[index] == '\0';
}

static uint32_t parse_unsigned(const char *text)
{
    if (text == NULL)
    {
        return 0;
    }

    uint32_t base = 10U;
    size_t index = 0;

    if (
        text[0] == '0' &&
        (text[1] == 'x' || text[1] == 'X')
    )
    {
        base = 16U;
        index = 2;
    }

    uint32_t value = 0;

    while (text[index] != '\0')
    {
        uint32_t digit;
        char character = text[index];

        if (character >= '0' && character <= '9')
        {
            digit = (uint32_t)(character - '0');
        }
        else if (
            base == 16U &&
            character >= 'a' && character <= 'f'
        )
        {
            digit = 10U + (uint32_t)(character - 'a');
        }
        else if (
            base == 16U &&
            character >= 'A' && character <= 'F'
        )
        {
            digit = 10U + (uint32_t)(character - 'A');
        }
        else
        {
            break;
        }

        if (digit >= base)
        {
            break;
        }

        value = value * base + digit;
        index++;
    }

    return value;
}

static int32_t parse_signed(const char *text)
{
    if (text == NULL)
    {
        return 0;
    }

    if (text[0] == '-')
    {
        return -(int32_t)parse_unsigned(text + 1);
    }

    return (int32_t)parse_unsigned(text);
}

static void append_character(
    char *buffer,
    size_t capacity,
    size_t *position,
    char character
)
{
    if (
        buffer == NULL ||
        position == NULL ||
        *position + 1U >= capacity
    )
    {
        return;
    }

    buffer[*position] = character;
    (*position)++;
    buffer[*position] = '\0';
}

static void append_text(
    char *buffer,
    size_t capacity,
    size_t *position,
    const char *text
)
{
    if (text == NULL)
    {
        return;
    }

    for (size_t index = 0; text[index] != '\0'; index++)
    {
        append_character(
            buffer,
            capacity,
            position,
            text[index]
        );
    }
}

static void append_unsigned(
    char *buffer,
    size_t capacity,
    size_t *position,
    uint32_t value
)
{
    char digits[16];
    uint32_t count = 0;

    do
    {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    }
    while (value != 0 && count < sizeof(digits));

    while (count > 0)
    {
        append_character(
            buffer,
            capacity,
            position,
            digits[--count]
        );
    }
}

static void append_signed(
    char *buffer,
    size_t capacity,
    size_t *position,
    int32_t value
)
{
    if (value < 0)
    {
        append_character(buffer, capacity, position, '-');
        append_unsigned(
            buffer,
            capacity,
            position,
            (uint32_t)(-(int64_t)value)
        );
        return;
    }

    append_unsigned(
        buffer,
        capacity,
        position,
        (uint32_t)value
    );
}


static void apply_builtin_theme(desktop_theme_kind_t kind)
{
    const gui_theme_t *base = app_suite_theme();

    if (base != NULL)
    {
        current_theme = *base;
    }
    else
    {
        current_theme = (gui_theme_t){
            .desktop = 0x1C4A72U,
            .taskbar = 0x172330U,
            .taskbar_top = 0x34495EU,
            .window = 0xE8EDF2U,
            .window_border = 0x23384DU,
            .title_active = 0x245E9AU,
            .title_idle = 0x536878U,
            .text = 0x102030U,
            .light_text = 0xFFFFFFU,
            .field = 0xFFFFFFU,
            .accent = 0x39A0EDU,
            .close = 0xC74848U,
            .row_selected = 0xA9CDE8U
        };
    }

    switch (kind)
    {
        case DESKTOP_THEME_GRAPHITE:
            current_theme.desktop = 0x25282CU;
            current_theme.taskbar = 0x181A1DU;
            current_theme.taskbar_top = 0x454A50U;
            current_theme.window = 0xE7E9EBU;
            current_theme.window_border = 0x30353AU;
            current_theme.title_active = 0x454C54U;
            current_theme.title_idle = 0x676D73U;
            current_theme.accent = 0x6E94B8U;
            current_theme.row_selected = 0xB7CBDDU;
            break;

        case DESKTOP_THEME_TEAL:
            current_theme.desktop = 0x123F46U;
            current_theme.taskbar = 0x0D292EU;
            current_theme.taskbar_top = 0x2F6B70U;
            current_theme.window_border = 0x173D42U;
            current_theme.title_active = 0x147A82U;
            current_theme.title_idle = 0x4E7478U;
            current_theme.accent = 0x19A7AEU;
            current_theme.row_selected = 0xA8DDE0U;
            break;

        case DESKTOP_THEME_AUBERGINE:
            current_theme.desktop = 0x3C2547U;
            current_theme.taskbar = 0x26172DU;
            current_theme.taskbar_top = 0x704D79U;
            current_theme.window_border = 0x3E2A45U;
            current_theme.title_active = 0x7D3F87U;
            current_theme.title_idle = 0x735E78U;
            current_theme.accent = 0xB35CC1U;
            current_theme.row_selected = 0xDEC2E2U;
            break;

        case DESKTOP_THEME_LDS:
            current_theme.desktop = 0x102A43U;
            current_theme.taskbar = 0x081A2BU;
            current_theme.taskbar_top = 0xC7A84BU;
            current_theme.window = 0xF2F5F7U;
            current_theme.window_border = 0x173A59U;
            current_theme.title_active = 0x1E527CU;
            current_theme.title_idle = 0x587386U;
            current_theme.text = 0x102536U;
            current_theme.light_text = 0xFFFFFFU;
            current_theme.field = 0xFFFFFFU;
            current_theme.accent = 0xC7A84BU;
            current_theme.close = 0xA74444U;
            current_theme.row_selected = 0xE6D89AU;
            break;

        case DESKTOP_THEME_BLUE:
        case DESKTOP_THEME_EXTERNAL:
        case DESKTOP_THEME_COUNT:
        default:
            break;
    }
}

static bool parse_theme_line(
    const char *key,
    const char *value
)
{
    uint32_t color = parse_unsigned(value);

    if (strings_equal(key, "desktop"))
    {
        current_theme.desktop = color;
    }
    else if (strings_equal(key, "taskbar"))
    {
        current_theme.taskbar = color;
    }
    else if (strings_equal(key, "taskbar_top"))
    {
        current_theme.taskbar_top = color;
    }
    else if (strings_equal(key, "window"))
    {
        current_theme.window = color;
    }
    else if (strings_equal(key, "window_border"))
    {
        current_theme.window_border = color;
    }
    else if (strings_equal(key, "title_active"))
    {
        current_theme.title_active = color;
    }
    else if (strings_equal(key, "title_idle"))
    {
        current_theme.title_idle = color;
    }
    else if (strings_equal(key, "text"))
    {
        current_theme.text = color;
    }
    else if (strings_equal(key, "light_text"))
    {
        current_theme.light_text = color;
    }
    else if (strings_equal(key, "field"))
    {
        current_theme.field = color;
    }
    else if (strings_equal(key, "accent"))
    {
        current_theme.accent = color;
    }
    else if (strings_equal(key, "close"))
    {
        current_theme.close = color;
    }
    else if (strings_equal(key, "row_selected"))
    {
        current_theme.row_selected = color;
    }
    else
    {
        return false;
    }

    return true;
}

static void parse_key_value_lines(
    char *buffer,
    bool theme_file
)
{
    if (buffer == NULL)
    {
        return;
    }

    char *cursor = buffer;

    while (*cursor != '\0')
    {
        char *line = cursor;

        while (*cursor != '\0' && *cursor != '\n')
        {
            if (*cursor == '\r')
            {
                *cursor = '\0';
            }

            cursor++;
        }

        if (*cursor == '\n')
        {
            *cursor = '\0';
            cursor++;
        }

        if (line[0] == '\0' || line[0] == '#')
        {
            continue;
        }

        char *equals = line;

        while (*equals != '\0' && *equals != '=')
        {
            equals++;
        }

        if (*equals != '=')
        {
            continue;
        }

        *equals = '\0';
        const char *key = line;
        const char *value = equals + 1;

        if (theme_file)
        {
            (void)parse_theme_line(key, value);
            continue;
        }

        if (strings_equal(key, "theme"))
        {
            uint32_t selected = parse_unsigned(value);

            if (selected < DESKTOP_THEME_COUNT)
            {
                theme_kind = (desktop_theme_kind_t)selected;
            }
        }
        else if (strings_equal(key, "wallpaper"))
        {
            uint32_t selected = parse_unsigned(value);

            if (selected < DESKTOP_WALLPAPER_COUNT)
            {
                wallpaper_kind = (desktop_wallpaper_t)selected;
            }
        }
        else if (
            key[0] == 'w' &&
            key[1] >= '0' && key[1] <= '7' &&
            key[2] == '_'
        )
        {
            uint32_t index = (uint32_t)(key[1] - '0');
            const char *member = key + 3;

            if (strings_equal(member, "valid"))
            {
                saved_windows[index].valid =
                    parse_unsigned(value) != 0;
            }
            else if (strings_equal(member, "x"))
            {
                saved_windows[index].bounds.x =
                    parse_signed(value);
            }
            else if (strings_equal(member, "y"))
            {
                saved_windows[index].bounds.y =
                    parse_signed(value);
            }
            else if (strings_equal(member, "width"))
            {
                saved_windows[index].bounds.width =
                    parse_unsigned(value);
            }
            else if (strings_equal(member, "height"))
            {
                saved_windows[index].bounds.height =
                    parse_unsigned(value);
            }
        }
        else if (
            key[0] == 'r' &&
            key[1] >= '0' && key[1] <= '4' &&
            key[2] == '\0'
        )
        {
            uint32_t index = (uint32_t)(key[1] - '0');

            if (value[0] != '\0')
            {
                (void)copy_text(
                    recent_files[index],
                    sizeof(recent_files[index]),
                    value
                );

                if (index + 1U > recent_file_count)
                {
                    recent_file_count = index + 1U;
                }
            }
        }
    }
}

static bool read_text_file(
    const char *path,
    char *buffer,
    size_t capacity
)
{
    if (path == NULL || buffer == NULL || capacity < 2U)
    {
        return false;
    }

    vfs_node_t *node = vfs_open(path);

    if (node == NULL || node->type != VFS_NODE_FILE)
    {
        return false;
    }

    size_t count = vfs_read(
        node,
        0,
        buffer,
        capacity - 1U
    );

    buffer[count] = '\0';
    return true;
}

static void ensure_external_theme_file(void)
{
    if (vfs_open(DESKTOP_THEME_PATH) != NULL)
    {
        return;
    }

    (void)vfs_write_text(
        DESKTOP_THEME_PATH,
        "# LatterOS external desktop theme\n"
        "desktop=0x20364A\n"
        "taskbar=0x152431\n"
        "taskbar_top=0x3D647D\n"
        "window=0xE8EDF2\n"
        "window_border=0x23384D\n"
        "title_active=0x2C769F\n"
        "title_idle=0x536878\n"
        "text=0x102030\n"
        "light_text=0xFFFFFF\n"
        "field=0xFFFFFF\n"
        "accent=0x42A5D5\n"
        "close=0xC74848\n"
        "row_selected=0xB8DCEC\n"
    );
}

bool desktop_services_reload_external_theme(void)
{
    apply_builtin_theme(DESKTOP_THEME_BLUE);

    if (!read_text_file(
        DESKTOP_THEME_PATH,
        theme_buffer,
        sizeof(theme_buffer)
    ))
    {
        return false;
    }

    parse_key_value_lines(theme_buffer, true);
    theme_kind = DESKTOP_THEME_EXTERNAL;
    return true;
}

void desktop_services_init(void)
{
    if (initialized)
    {
        return;
    }

    initialized = true;
    theme_kind = DESKTOP_THEME_BLUE;
    wallpaper_kind = DESKTOP_WALLPAPER_GRADIENT;
    recent_file_count = 0;
    clipboard_length = 0;
    clipboard_generation = 0;
    clipboard[0] = '\0';
    next_notification_id = 1;
    notification_total = 0;

    clear_bytes(saved_windows, sizeof(saved_windows));
    clear_bytes(recent_files, sizeof(recent_files));
    clear_bytes(notifications, sizeof(notifications));

    ensure_external_theme_file();

    if (read_text_file(
        DESKTOP_CONFIG_PATH,
        configuration_buffer,
        sizeof(configuration_buffer)
    ))
    {
        parse_key_value_lines(configuration_buffer, false);
    }

    if (theme_kind == DESKTOP_THEME_EXTERNAL)
    {
        if (!desktop_services_reload_external_theme())
        {
            theme_kind = DESKTOP_THEME_BLUE;
            apply_builtin_theme(theme_kind);
        }
    }
    else
    {
        apply_builtin_theme(theme_kind);
    }
}

const gui_theme_t *desktop_services_theme(void)
{
    desktop_services_init();
    return &current_theme;
}

desktop_theme_kind_t desktop_services_theme_kind(void)
{
    desktop_services_init();
    return theme_kind;
}

desktop_wallpaper_t desktop_services_wallpaper(void)
{
    desktop_services_init();
    return wallpaper_kind;
}

void desktop_services_cycle_theme(void)
{
    desktop_services_init();

    uint32_t next = (uint32_t)theme_kind + 1U;

    if (next >= DESKTOP_THEME_COUNT)
    {
        next = 0;
    }

    theme_kind = (desktop_theme_kind_t)next;

    if (theme_kind == DESKTOP_THEME_EXTERNAL)
    {
        if (!desktop_services_reload_external_theme())
        {
            theme_kind = DESKTOP_THEME_BLUE;
            apply_builtin_theme(theme_kind);
        }
    }
    else
    {
        apply_builtin_theme(theme_kind);
    }

    (void)desktop_services_save();
}

bool desktop_services_set_theme(desktop_theme_kind_t kind)
{
    desktop_services_init();

    if (kind >= DESKTOP_THEME_COUNT)
    {
        return false;
    }

    theme_kind = kind;

    if (theme_kind == DESKTOP_THEME_EXTERNAL)
    {
        if (!desktop_services_reload_external_theme())
        {
            theme_kind = DESKTOP_THEME_BLUE;
            apply_builtin_theme(theme_kind);
            return false;
        }
    }
    else
    {
        apply_builtin_theme(theme_kind);
    }

    return desktop_services_save();
}

bool desktop_services_set_wallpaper(desktop_wallpaper_t kind)
{
    desktop_services_init();

    if (kind >= DESKTOP_WALLPAPER_COUNT)
    {
        return false;
    }

    wallpaper_kind = kind;
    return desktop_services_save();
}

bool desktop_services_reset_configuration(void)
{
    desktop_services_init();

    theme_kind = DESKTOP_THEME_BLUE;
    wallpaper_kind = DESKTOP_WALLPAPER_GRADIENT;
    recent_file_count = 0;
    clear_bytes(saved_windows, sizeof(saved_windows));
    clear_bytes(recent_files, sizeof(recent_files));
    apply_builtin_theme(theme_kind);
    return desktop_services_save();
}

void desktop_services_cycle_wallpaper(void)
{
    desktop_services_init();

    wallpaper_kind = (desktop_wallpaper_t)(
        ((uint32_t)wallpaper_kind + 1U) %
            DESKTOP_WALLPAPER_COUNT
    );

    (void)desktop_services_save();
}

const char *desktop_services_theme_name(void)
{
    switch (theme_kind)
    {
        case DESKTOP_THEME_BLUE:
            return "Blue";
        case DESKTOP_THEME_GRAPHITE:
            return "Graphite";
        case DESKTOP_THEME_TEAL:
            return "Teal";
        case DESKTOP_THEME_AUBERGINE:
            return "Aubergine";
        case DESKTOP_THEME_EXTERNAL:
            return "External";
        case DESKTOP_THEME_LDS:
            return "LDS";
        default:
            return "Unknown";
    }
}

const char *desktop_services_wallpaper_name(void)
{
    switch (wallpaper_kind)
    {
        case DESKTOP_WALLPAPER_SOLID:
            return "Solid";
        case DESKTOP_WALLPAPER_GRADIENT:
            return "Gradient";
        case DESKTOP_WALLPAPER_GRID:
            return "Grid";
        case DESKTOP_WALLPAPER_NIGHT:
            return "Night";
        default:
            return "Unknown";
    }
}

void desktop_services_store_window(
    uint32_t index,
    const ui_rect_t *bounds
)
{
    desktop_services_init();

    if (
        index >= DESKTOP_SERVICE_WINDOW_COUNT ||
        bounds == NULL ||
        bounds->width == 0 ||
        bounds->height == 0
    )
    {
        return;
    }

    saved_windows[index].valid = true;
    saved_windows[index].bounds = *bounds;
}

bool desktop_services_restore_window(
    uint32_t index,
    ui_rect_t *bounds
)
{
    desktop_services_init();

    if (
        index >= DESKTOP_SERVICE_WINDOW_COUNT ||
        bounds == NULL ||
        !saved_windows[index].valid
    )
    {
        return false;
    }

    *bounds = saved_windows[index].bounds;
    return true;
}

bool desktop_services_save(void)
{
    desktop_services_init();

    size_t position = 0;
    configuration_buffer[0] = '\0';

    append_text(configuration_buffer, sizeof(configuration_buffer), &position, "theme=");
    append_unsigned(
        configuration_buffer,
        sizeof(configuration_buffer),
        &position,
        (uint32_t)theme_kind
    );
    append_character(configuration_buffer, sizeof(configuration_buffer), &position, '\n');

    append_text(configuration_buffer, sizeof(configuration_buffer), &position, "wallpaper=");
    append_unsigned(
        configuration_buffer,
        sizeof(configuration_buffer),
        &position,
        (uint32_t)wallpaper_kind
    );
    append_character(configuration_buffer, sizeof(configuration_buffer), &position, '\n');

    for (
        uint32_t index = 0;
        index < DESKTOP_SERVICE_WINDOW_COUNT;
        index++
    )
    {
        append_character(configuration_buffer, sizeof(configuration_buffer), &position, 'w');
        append_unsigned(configuration_buffer, sizeof(configuration_buffer), &position, index);
        append_text(configuration_buffer, sizeof(configuration_buffer), &position, "_valid=");
        append_unsigned(
            configuration_buffer,
            sizeof(configuration_buffer),
            &position,
            saved_windows[index].valid ? 1U : 0U
        );
        append_character(configuration_buffer, sizeof(configuration_buffer), &position, '\n');

        append_character(configuration_buffer, sizeof(configuration_buffer), &position, 'w');
        append_unsigned(configuration_buffer, sizeof(configuration_buffer), &position, index);
        append_text(configuration_buffer, sizeof(configuration_buffer), &position, "_x=");
        append_signed(
            configuration_buffer,
            sizeof(configuration_buffer),
            &position,
            saved_windows[index].bounds.x
        );
        append_character(configuration_buffer, sizeof(configuration_buffer), &position, '\n');

        append_character(configuration_buffer, sizeof(configuration_buffer), &position, 'w');
        append_unsigned(configuration_buffer, sizeof(configuration_buffer), &position, index);
        append_text(configuration_buffer, sizeof(configuration_buffer), &position, "_y=");
        append_signed(
            configuration_buffer,
            sizeof(configuration_buffer),
            &position,
            saved_windows[index].bounds.y
        );
        append_character(configuration_buffer, sizeof(configuration_buffer), &position, '\n');

        append_character(configuration_buffer, sizeof(configuration_buffer), &position, 'w');
        append_unsigned(configuration_buffer, sizeof(configuration_buffer), &position, index);
        append_text(configuration_buffer, sizeof(configuration_buffer), &position, "_width=");
        append_unsigned(
            configuration_buffer,
            sizeof(configuration_buffer),
            &position,
            saved_windows[index].bounds.width
        );
        append_character(configuration_buffer, sizeof(configuration_buffer), &position, '\n');

        append_character(configuration_buffer, sizeof(configuration_buffer), &position, 'w');
        append_unsigned(configuration_buffer, sizeof(configuration_buffer), &position, index);
        append_text(configuration_buffer, sizeof(configuration_buffer), &position, "_height=");
        append_unsigned(
            configuration_buffer,
            sizeof(configuration_buffer),
            &position,
            saved_windows[index].bounds.height
        );
        append_character(configuration_buffer, sizeof(configuration_buffer), &position, '\n');
    }

    for (uint32_t index = 0; index < recent_file_count; index++)
    {
        append_character(configuration_buffer, sizeof(configuration_buffer), &position, 'r');
        append_unsigned(configuration_buffer, sizeof(configuration_buffer), &position, index);
        append_character(configuration_buffer, sizeof(configuration_buffer), &position, '=');
        append_text(
            configuration_buffer,
            sizeof(configuration_buffer),
            &position,
            recent_files[index]
        );
        append_character(configuration_buffer, sizeof(configuration_buffer), &position, '\n');
    }

    return vfs_write_text(DESKTOP_CONFIG_PATH, configuration_buffer);
}

void desktop_clipboard_clear(void)
{
    clipboard[0] = '\0';
    clipboard_length = 0;
    clipboard_generation++;
}

bool desktop_clipboard_set_text(const char *text)
{
    if (text == NULL)
    {
        desktop_clipboard_clear();
        return false;
    }

    bool complete = copy_text(
        clipboard,
        sizeof(clipboard),
        text
    );

    clipboard_length = string_length(clipboard);
    clipboard_generation++;
    return complete;
}

bool desktop_clipboard_set_range(
    const char *text,
    size_t start,
    size_t end
)
{
    if (text == NULL)
    {
        return false;
    }

    size_t length = string_length(text);

    if (start > end)
    {
        size_t temporary = start;
        start = end;
        end = temporary;
    }

    if (start > length)
    {
        start = length;
    }

    if (end > length)
    {
        end = length;
    }

    size_t count = end - start;

    if (count >= sizeof(clipboard))
    {
        count = sizeof(clipboard) - 1U;
    }

    for (size_t index = 0; index < count; index++)
    {
        clipboard[index] = text[start + index];
    }

    clipboard[count] = '\0';
    clipboard_length = count;
    clipboard_generation++;
    return end - start == count;
}

const char *desktop_clipboard_text(void)
{
    return clipboard;
}

size_t desktop_clipboard_length(void)
{
    return clipboard_length;
}

bool desktop_clipboard_has_text(void)
{
    return clipboard_length != 0;
}

uint64_t desktop_clipboard_generation(void)
{
    return clipboard_generation;
}

void desktop_recent_add(const char *path)
{
    if (path == NULL || path[0] == '\0')
    {
        return;
    }

    uint32_t existing = DESKTOP_RECENT_FILE_COUNT;

    for (uint32_t index = 0; index < recent_file_count; index++)
    {
        if (strings_equal(recent_files[index], path))
        {
            existing = index;
            break;
        }
    }

    bool new_entry =
        existing >= DESKTOP_RECENT_FILE_COUNT;
    uint32_t limit = recent_file_count;

    if (limit >= DESKTOP_RECENT_FILE_COUNT)
    {
        limit = DESKTOP_RECENT_FILE_COUNT - 1U;
    }

    if (existing < DESKTOP_RECENT_FILE_COUNT)
    {
        limit = existing;
    }

    for (uint32_t index = limit; index > 0; index--)
    {
        (void)copy_text(
            recent_files[index],
            sizeof(recent_files[index]),
            recent_files[index - 1U]
        );
    }

    (void)copy_text(
        recent_files[0],
        sizeof(recent_files[0]),
        path
    );

    if (
        new_entry &&
        recent_file_count < DESKTOP_RECENT_FILE_COUNT
    )
    {
        recent_file_count++;
    }

    (void)desktop_services_save();
}

uint32_t desktop_recent_count(void)
{
    return recent_file_count;
}

const char *desktop_recent_path(uint32_t index)
{
    if (index >= recent_file_count)
    {
        return NULL;
    }

    return recent_files[index];
}

desktop_association_t desktop_association_for_path(
    const char *path
)
{
    if (path == NULL)
    {
        return DESKTOP_ASSOCIATION_UNKNOWN;
    }

    static const char *text_extensions[] = {
        ".txt", ".md", ".log", ".cfg", ".conf",
        ".ini", ".c", ".h", ".asm", ".s", ".json"
    };

    for (
        size_t index = 0;
        index < sizeof(text_extensions) /
            sizeof(text_extensions[0]);
        index++
    )
    {
        if (string_ends_with_case_insensitive(
            path,
            text_extensions[index]
        ))
        {
            return DESKTOP_ASSOCIATION_TEXT;
        }
    }

    if (
        string_ends_with_case_insensitive(path, ".bmp") ||
        string_ends_with_case_insensitive(path, ".ppm") ||
        string_ends_with_case_insensitive(path, ".lpaint")
    )
    {
        return DESKTOP_ASSOCIATION_IMAGE;
    }

    if (
        string_ends_with_case_insensitive(path, ".elf") ||
        string_ends_with_case_insensitive(path, ".bin") ||
        string_ends_with_case_insensitive(path, ".app")
    )
    {
        return DESKTOP_ASSOCIATION_EXECUTABLE;
    }

    if (string_ends_with_case_insensitive(path, ".lpkg"))
    {
        return DESKTOP_ASSOCIATION_PACKAGE;
    }

    return DESKTOP_ASSOCIATION_UNKNOWN;
}

static uint64_t milliseconds_to_ticks(uint32_t milliseconds)
{
    uint64_t frequency = timer_frequency();

    if (frequency == 0)
    {
        frequency = 1000;
    }

    uint64_t ticks =
        frequency * (uint64_t)milliseconds / 1000ULL;

    return ticks == 0 ? 1 : ticks;
}

void desktop_notify(const char *text, uint32_t lifetime_ms)
{
    if (text == NULL || text[0] == '\0')
    {
        return;
    }

    desktop_notifications_update();

    uint32_t slot = DESKTOP_NOTIFICATION_CAPACITY;

    for (
        uint32_t index = 0;
        index < DESKTOP_NOTIFICATION_CAPACITY;
        index++
    )
    {
        if (!notifications[index].active)
        {
            slot = index;
            break;
        }
    }

    if (slot >= DESKTOP_NOTIFICATION_CAPACITY)
    {
        uint64_t oldest_id = UINT64_MAX;

        for (
            uint32_t index = 0;
            index < DESKTOP_NOTIFICATION_CAPACITY;
            index++
        )
        {
            if (notifications[index].id < oldest_id)
            {
                oldest_id = notifications[index].id;
                slot = index;
            }
        }
    }

    desktop_notification_t *notification =
        &notifications[slot];

    notification->active = true;
    notification->id = next_notification_id++;
    notification->expires_at = timer_ticks() +
        milliseconds_to_ticks(
            lifetime_ms == 0 ? 5000U : lifetime_ms
        );

    (void)copy_text(
        notification->text,
        sizeof(notification->text),
        text
    );

    notification_total++;
}

void desktop_notifications_update(void)
{
    uint64_t now = timer_ticks();

    for (
        uint32_t index = 0;
        index < DESKTOP_NOTIFICATION_CAPACITY;
        index++
    )
    {
        if (
            notifications[index].active &&
            notifications[index].expires_at <= now
        )
        {
            notifications[index].active = false;
        }
    }
}

uint32_t desktop_notification_count(void)
{
    desktop_notifications_update();
    uint32_t count = 0;

    for (
        uint32_t index = 0;
        index < DESKTOP_NOTIFICATION_CAPACITY;
        index++
    )
    {
        if (notifications[index].active)
        {
            count++;
        }
    }

    return count;
}

const desktop_notification_t *desktop_notification_get(
    uint32_t index
)
{
    desktop_notifications_update();
    uint32_t current = 0;

    for (
        uint32_t slot = 0;
        slot < DESKTOP_NOTIFICATION_CAPACITY;
        slot++
    )
    {
        if (!notifications[slot].active)
        {
            continue;
        }

        if (current == index)
        {
            return &notifications[slot];
        }

        current++;
    }

    return NULL;
}

void desktop_notification_dismiss(uint64_t id)
{
    for (
        uint32_t index = 0;
        index < DESKTOP_NOTIFICATION_CAPACITY;
        index++
    )
    {
        if (
            notifications[index].active &&
            notifications[index].id == id
        )
        {
            notifications[index].active = false;
            return;
        }
    }
}

uint64_t desktop_notification_total(void)
{
    return notification_total;
}
