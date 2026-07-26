#include "app_suite.h"

#include "graphics.h"
#include "gui.h"
#include "irq.h"
#include "kstdio.h"
#include "process.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EDITOR_CAPACITY 2048
#define EDITOR_STATUS_CAPACITY 64

#define CALCULATOR_EXPRESSION_CAPACITY 64
#define CALCULATOR_RESULT_CAPACITY 64

#define PAINT_WIDTH  320
#define PAINT_HEIGHT 180
#define PAINT_COLOR_COUNT 6
#define PAINT_BRUSH_RADIUS 2

#define PROCESS_SNAPSHOT_COUNT PROCESS_MAX_COUNT

static const gui_theme_t themes[] = {
    {
        .desktop = 0x1C4A72,
        .taskbar = 0x172330,
        .taskbar_top = 0x34495E,
        .window = 0xE8EDF2,
        .window_border = 0x23384D,
        .title_active = 0x245E9A,
        .title_idle = 0x536878,
        .text = 0x102030,
        .light_text = 0xFFFFFF,
        .field = 0xFFFFFF,
        .accent = 0x39A0ED,
        .close = 0xC74848,
        .row_selected = 0xA9CDE8
    },
    {
        .desktop = 0x17191D,
        .taskbar = 0x0C0D10,
        .taskbar_top = 0x333842,
        .window = 0x282C33,
        .window_border = 0x08090B,
        .title_active = 0x4B5360,
        .title_idle = 0x353B45,
        .text = 0xEEF2F7,
        .light_text = 0xFFFFFF,
        .field = 0x1D2026,
        .accent = 0x7AA2F7,
        .close = 0xD05C64,
        .row_selected = 0x46536B
    },
    {
        .desktop = 0x174B3A,
        .taskbar = 0x102D25,
        .taskbar_top = 0x2F6B56,
        .window = 0xE6F0EA,
        .window_border = 0x1D493A,
        .title_active = 0x2C765B,
        .title_idle = 0x577568,
        .text = 0x10291F,
        .light_text = 0xFFFFFF,
        .field = 0xFFFFFF,
        .accent = 0x39B982,
        .close = 0xC54B4B,
        .row_selected = 0xA8D8C5
    }
};

static uint32_t theme_index;

static vfs_node_t *editor_node;
static char editor_path[VFS_PATH_MAX];
static char editor_text[EDITOR_CAPACITY];
static size_t editor_length;
static char editor_status[EDITOR_STATUS_CAPACITY];

static char calculator_expression[
    CALCULATOR_EXPRESSION_CAPACITY
];
static size_t calculator_length;
static char calculator_result[
    CALCULATOR_RESULT_CAPACITY
];

static uint32_t paint_pixels[
    PAINT_WIDTH * PAINT_HEIGHT
];

static const uint32_t paint_colors[
    PAINT_COLOR_COUNT
] = {
    0x111111,
    0xE74C3C,
    0x3498DB,
    0x2ECC71,
    0xF1C40F,
    0x9B59B6
};

static uint32_t paint_color_index;
static bool paint_drawing;
static int32_t paint_last_x;
static int32_t paint_last_y;

typedef enum
{
    TASK_SNAPSHOT_APPLICATION,
    TASK_SNAPSHOT_PROCESS
} task_snapshot_kind_t;

typedef struct
{
    task_snapshot_kind_t kind;
    uint64_t id;
    gui_application_state_t application_state;
    process_mode_t process_mode;
    process_state_t process_state;
    uint64_t ticks;
    uint64_t switches;
    char name[PROCESS_NAME_LENGTH];
} task_snapshot_t;

#define TASK_SNAPSHOT_COUNT \
    (PROCESS_SNAPSHOT_COUNT + 7)

static task_snapshot_t task_snapshots[
    TASK_SNAPSHOT_COUNT
];

static uint32_t task_snapshot_count;
static uint32_t application_snapshot_count;
static uint32_t process_snapshot_count;
static int32_t process_selected_row;
static char process_status[64];

static size_t text_length(const char *text)
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

static void append_character(
    char *text,
    size_t capacity,
    size_t *length,
    char character
)
{
    if (
        text == NULL ||
        length == NULL ||
        *length + 1 >= capacity
    )
    {
        return;
    }

    text[*length] = character;
    (*length)++;
    text[*length] = '\0';
}

static ui_rect_t make_button(
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height
)
{
    ui_rect_t rectangle = {
        .x = x,
        .y = y,
        .width = width,
        .height = height
    };

    return rectangle;
}

static void draw_button(
    const ui_rect_t *rectangle,
    const char *label,
    bool active
)
{
    const gui_theme_t *theme =
        app_suite_theme();

    ui_fill_rect(
        rectangle,
        active ? theme->accent : theme->field
    );

    ui_draw_border(
        rectangle,
        active ? theme->title_active : theme->window_border,
        1
    );

    ui_draw_text_centered(
        label,
        rectangle,
        active ? theme->light_text : theme->text
    );
}

static void build_node_path(
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

const gui_theme_t *app_suite_theme(void)
{
    return &themes[theme_index];
}

static void editor_load_node(vfs_node_t *node)
{
    clear_text(editor_text, sizeof(editor_text));
    editor_length = 0;
    editor_node = node;

    if (
        node == NULL ||
        node->type != VFS_NODE_FILE
    )
    {
        copy_text(
            editor_status,
            sizeof(editor_status),
            "New file"
        );
        return;
    }

    build_node_path(
        node,
        editor_path,
        sizeof(editor_path)
    );

    size_t count = vfs_read(
        node,
        0,
        editor_text,
        sizeof(editor_text) - 1
    );

    editor_text[count] = '\0';
    editor_length = count;

    copy_text(
        editor_status,
        sizeof(editor_status),
        "Loaded"
    );
}

void editor_open_node(vfs_node_t *node)
{
    editor_load_node(node);
}

static ui_rect_t editor_new_button(
    const ui_rect_t *content
)
{
    return make_button(
        content->x + 10,
        content->y + 10,
        64,
        24
    );
}

static ui_rect_t editor_save_button(
    const ui_rect_t *content
)
{
    return make_button(
        content->x + 82,
        content->y + 10,
        64,
        24
    );
}

static ui_rect_t editor_text_area(
    const ui_rect_t *content
)
{
    ui_rect_t rectangle = {
        .x = content->x + 10,
        .y = content->y + 66,
        .width = content->width - 20,
        .height = content->height - 78
    };

    return rectangle;
}

static bool editor_save(void)
{
    if (editor_path[0] == '\0')
    {
        copy_text(
            editor_path,
            sizeof(editor_path),
            "/home/notes.txt"
        );
    }

    bool saved = vfs_write_text(
        editor_path,
        editor_text
    );

    if (saved)
    {
        editor_node = vfs_open(editor_path);
    }

    copy_text(
        editor_status,
        sizeof(editor_status),
        saved ? "Saved" : "Save failed"
    );

    return saved;
}

void editor_render(const ui_rect_t *content)
{
    if (content == NULL)
    {
        return;
    }

    const gui_theme_t *theme =
        app_suite_theme();

    ui_rect_t new_button =
        editor_new_button(content);

    ui_rect_t save_button =
        editor_save_button(content);

    draw_button(&new_button, "New", false);
    draw_button(&save_button, "Save", true);

    ui_draw_text(
        "File:",
        content->x + 158,
        content->y + 18,
        theme->text
    );

    ui_draw_text(
        editor_path,
        content->x + 198,
        content->y + 18,
        theme->text
    );

    ui_draw_text(
        editor_status,
        content->x + 10,
        content->y + 46,
        theme->title_idle
    );

    ui_rect_t area =
        editor_text_area(content);

    ui_fill_rect(&area, theme->field);
    ui_draw_border(
        &area,
        theme->window_border,
        1
    );

    uint32_t columns =
        area.width > 16 ?
            (area.width - 12) / 8 : 1;

    uint32_t visible_lines =
        area.height > 16 ?
            (area.height - 12) / 12 : 1;

    uint32_t total_lines = 1;
    uint32_t column = 0;

    for (
        size_t index = 0;
        index < editor_length;
        index++
    )
    {
        if (
            editor_text[index] == '\n' ||
            column >= columns
        )
        {
            total_lines++;
            column = 0;

            if (editor_text[index] == '\n')
            {
                continue;
            }
        }

        column++;
    }

    uint32_t first_line =
        total_lines > visible_lines ?
            total_lines - visible_lines : 0;

    uint32_t current_line = 0;
    uint32_t visible_line = 0;
    char line[96];
    uint32_t line_length = 0;

    clear_text(line, sizeof(line));

    for (
        size_t index = 0;
        index <= editor_length;
        index++
    )
    {
        char character =
            index < editor_length ?
                editor_text[index] : '\n';

        bool line_end =
            character == '\n' ||
            line_length >= columns ||
            index == editor_length;

        if (line_end)
        {
            line[line_length] = '\0';

            if (
                current_line >= first_line &&
                visible_line < visible_lines
            )
            {
                ui_draw_text(
                    line,
                    area.x + 6,
                    area.y + 6 +
                        (int32_t)visible_line * 12,
                    theme->text
                );

                visible_line++;
            }

            current_line++;
            line_length = 0;
            clear_text(line, sizeof(line));

            if (character == '\n')
            {
                continue;
            }
        }

        if (
            character >= 32 &&
            character <= 126 &&
            line_length + 1 < sizeof(line)
        )
        {
            line[line_length] = character;
            line_length++;
            line[line_length] = '\0';
        }
    }
}

bool editor_handle_click(
    const ui_rect_t *content,
    int32_t x,
    int32_t y
)
{
    if (content == NULL)
    {
        return false;
    }

    ui_rect_t new_button =
        editor_new_button(content);

    ui_rect_t save_button =
        editor_save_button(content);

    if (ui_point_in_rect(x, y, &new_button))
    {
        editor_node = NULL;
        clear_text(editor_text, sizeof(editor_text));
        editor_length = 0;

        copy_text(
            editor_path,
            sizeof(editor_path),
            "/home/notes.txt"
        );

        copy_text(
            editor_status,
            sizeof(editor_status),
            "New file"
        );

        return true;
    }

    if (ui_point_in_rect(x, y, &save_button))
    {
        (void)editor_save();
        return true;
    }

    ui_rect_t area =
        editor_text_area(content);

    return ui_point_in_rect(
        x,
        y,
        &area
    );
}

bool editor_handle_key(char character)
{
    if (character == '\b')
    {
        if (editor_length > 0)
        {
            editor_length--;
            editor_text[editor_length] = '\0';

            copy_text(
                editor_status,
                sizeof(editor_status),
                "Modified"
            );

            return true;
        }

        return false;
    }

    if (character == '\n')
    {
        append_character(
            editor_text,
            sizeof(editor_text),
            &editor_length,
            '\n'
        );

        copy_text(
            editor_status,
            sizeof(editor_status),
            "Modified"
        );

        return true;
    }

    if (
        character >= 32 &&
        character <= 126 &&
        editor_length + 1 < sizeof(editor_text)
    )
    {
        append_character(
            editor_text,
            sizeof(editor_text),
            &editor_length,
            character
        );

        copy_text(
            editor_status,
            sizeof(editor_status),
            "Modified"
        );

        return true;
    }

    return false;
}

static bool calculator_parse_number(
    const char **cursor,
    int64_t *value
)
{
    if (
        cursor == NULL ||
        *cursor == NULL ||
        value == NULL
    )
    {
        return false;
    }

    const char *text = *cursor;

    while (*text == ' ')
    {
        text++;
    }

    bool negative = false;

    if (*text == '-')
    {
        negative = true;
        text++;
    }
    else if (*text == '+')
    {
        text++;
    }

    if (
        *text < '0' ||
        *text > '9'
    )
    {
        return false;
    }

    uint64_t number = 0;

    while (
        *text >= '0' &&
        *text <= '9'
    )
    {
        number =
            number * 10 +
            (uint64_t)(*text - '0');

        text++;
    }

    *value = negative ?
        -(int64_t)number :
        (int64_t)number;

    *cursor = text;
    return true;
}

static void calculator_evaluate(void)
{
    const char *cursor =
        calculator_expression;

    int64_t left;
    int64_t right;

    if (!calculator_parse_number(&cursor, &left))
    {
        copy_text(
            calculator_result,
            sizeof(calculator_result),
            "Invalid expression"
        );
        return;
    }

    while (*cursor == ' ')
    {
        cursor++;
    }

    char operation = *cursor;

    if (
        operation != '+' &&
        operation != '-' &&
        operation != '*' &&
        operation != '/'
    )
    {
        ksnprintf(
            calculator_result,
            sizeof(calculator_result),
            "%lld",
            (long long)left
        );
        return;
    }

    cursor++;

    if (!calculator_parse_number(&cursor, &right))
    {
        copy_text(
            calculator_result,
            sizeof(calculator_result),
            "Invalid expression"
        );
        return;
    }

    while (*cursor == ' ')
    {
        cursor++;
    }

    if (*cursor != '\0')
    {
        copy_text(
            calculator_result,
            sizeof(calculator_result),
            "One operation at a time"
        );
        return;
    }

    int64_t result = 0;

    switch (operation)
    {
        case '+':
            result = left + right;
            break;

        case '-':
            result = left - right;
            break;

        case '*':
            result = left * right;
            break;

        case '/':
            if (right == 0)
            {
                copy_text(
                    calculator_result,
                    sizeof(calculator_result),
                    "Division by zero"
                );
                return;
            }

            result = left / right;
            break;

        default:
            return;
    }

    ksnprintf(
        calculator_result,
        sizeof(calculator_result),
        "%lld",
        (long long)result
    );
}

static ui_rect_t calculator_input_bounds(
    const ui_rect_t *content
)
{
    ui_rect_t rectangle = {
        .x = content->x + 14,
        .y = content->y + 14,
        .width = content->width - 28,
        .height = 34
    };

    return rectangle;
}

static ui_rect_t calculator_button_bounds(
    const ui_rect_t *content,
    uint32_t row,
    uint32_t column
)
{
    uint32_t gap = 6;
    uint32_t width =
        (content->width - 28 - gap * 3) / 4;

    uint32_t height = 42;

    ui_rect_t rectangle = {
        .x = content->x + 14 +
            (int32_t)column *
            (int32_t)(width + gap),
        .y = content->y + 92 +
            (int32_t)row *
            (int32_t)(height + gap),
        .width = width,
        .height = height
    };

    return rectangle;
}

static void calculator_apply_character(char character)
{
    if (character == 'C')
    {
        clear_text(
            calculator_expression,
            sizeof(calculator_expression)
        );

        calculator_length = 0;

        copy_text(
            calculator_result,
            sizeof(calculator_result),
            "Ready"
        );

        return;
    }

    if (character == '=')
    {
        calculator_evaluate();
        return;
    }

    append_character(
        calculator_expression,
        sizeof(calculator_expression),
        &calculator_length,
        character
    );
}

void calculator_render(const ui_rect_t *content)
{
    if (content == NULL)
    {
        return;
    }

    const gui_theme_t *theme =
        app_suite_theme();

    ui_rect_t input =
        calculator_input_bounds(content);

    ui_fill_rect(&input, theme->field);
    ui_draw_border(
        &input,
        theme->accent,
        2
    );

    ui_draw_text(
        calculator_expression,
        input.x + 8,
        input.y + 13,
        theme->text
    );

    ui_draw_text(
        "Result:",
        content->x + 14,
        content->y + 64,
        theme->text
    );

    ui_draw_text(
        calculator_result,
        content->x + 74,
        content->y + 64,
        theme->text
    );

    static const char labels[4][4] = {
        { '7', '8', '9', '/' },
        { '4', '5', '6', '*' },
        { '1', '2', '3', '-' },
        { '0', 'C', '=', '+' }
    };

    char label[2] = { 0, 0 };

    for (uint32_t row = 0; row < 4; row++)
    {
        for (uint32_t column = 0; column < 4; column++)
        {
            label[0] = labels[row][column];

            ui_rect_t button =
                calculator_button_bounds(
                    content,
                    row,
                    column
                );

            draw_button(
                &button,
                label,
                labels[row][column] == '='
            );
        }
    }
}

bool calculator_handle_click(
    const ui_rect_t *content,
    int32_t x,
    int32_t y
)
{
    if (content == NULL)
    {
        return false;
    }

    static const char labels[4][4] = {
        { '7', '8', '9', '/' },
        { '4', '5', '6', '*' },
        { '1', '2', '3', '-' },
        { '0', 'C', '=', '+' }
    };

    for (uint32_t row = 0; row < 4; row++)
    {
        for (uint32_t column = 0; column < 4; column++)
        {
            ui_rect_t button =
                calculator_button_bounds(
                    content,
                    row,
                    column
                );

            if (ui_point_in_rect(x, y, &button))
            {
                calculator_apply_character(
                    labels[row][column]
                );

                return true;
            }
        }
    }

    ui_rect_t input =
        calculator_input_bounds(content);

    return ui_point_in_rect(
        x,
        y,
        &input
    );
}

bool calculator_handle_key(char character)
{
    if (character == '\b')
    {
        if (calculator_length == 0)
        {
            return false;
        }

        calculator_length--;
        calculator_expression[
            calculator_length
        ] = '\0';

        return true;
    }

    if (character == '\n' || character == '=')
    {
        calculator_evaluate();
        return true;
    }

    if (character == 'c' || character == 'C')
    {
        calculator_apply_character('C');
        return true;
    }

    if (
        (character >= '0' && character <= '9') ||
        character == '+' ||
        character == '-' ||
        character == '*' ||
        character == '/' ||
        character == ' '
    )
    {
        calculator_apply_character(character);
        return true;
    }

    return false;
}

static ui_rect_t paint_canvas_bounds(
    const ui_rect_t *content
)
{
    ui_rect_t rectangle = {
        .x = content->x + 12,
        .y = content->y + 48,
        .width = PAINT_WIDTH,
        .height = PAINT_HEIGHT
    };

    if (rectangle.width > content->width - 24)
    {
        rectangle.width = content->width - 24;
    }

    if (rectangle.height > content->height - 60)
    {
        rectangle.height = content->height - 60;
    }

    return rectangle;
}

static ui_rect_t paint_palette_bounds(
    const ui_rect_t *content,
    uint32_t index
)
{
    return make_button(
        content->x + 12 +
            (int32_t)index * 30,
        content->y + 12,
        24,
        22
    );
}

static ui_rect_t paint_clear_bounds(
    const ui_rect_t *content
)
{
    return make_button(
        content->x + 204,
        content->y + 12,
        72,
        22
    );
}

static void paint_clear_canvas(void)
{
    for (
        uint32_t index = 0;
        index < PAINT_WIDTH * PAINT_HEIGHT;
        index++
    )
    {
        paint_pixels[index] = 0xFFFFFF;
    }
}

static void paint_set_pixel(
    int32_t x,
    int32_t y
)
{
    if (
        x < 0 ||
        y < 0 ||
        x >= PAINT_WIDTH ||
        y >= PAINT_HEIGHT
    )
    {
        return;
    }

    paint_pixels[
        (uint32_t)y * PAINT_WIDTH +
        (uint32_t)x
    ] = paint_colors[paint_color_index];
}

static void paint_draw_brush(
    int32_t x,
    int32_t y
)
{
    for (
        int32_t offset_y = -PAINT_BRUSH_RADIUS;
        offset_y <= PAINT_BRUSH_RADIUS;
        offset_y++
    )
    {
        for (
            int32_t offset_x = -PAINT_BRUSH_RADIUS;
            offset_x <= PAINT_BRUSH_RADIUS;
            offset_x++
        )
        {
            paint_set_pixel(
                x + offset_x,
                y + offset_y
            );
        }
    }
}

static void paint_draw_line(
    int32_t first_x,
    int32_t first_y,
    int32_t second_x,
    int32_t second_y
)
{
    int32_t delta_x = second_x - first_x;
    int32_t delta_y = second_y - first_y;

    int32_t steps = delta_x < 0 ?
        -delta_x : delta_x;

    int32_t vertical_steps = delta_y < 0 ?
        -delta_y : delta_y;

    if (vertical_steps > steps)
    {
        steps = vertical_steps;
    }

    if (steps == 0)
    {
        paint_draw_brush(first_x, first_y);
        return;
    }

    for (int32_t step = 0; step <= steps; step++)
    {
        int32_t x =
            first_x +
            (delta_x * step) / steps;

        int32_t y =
            first_y +
            (delta_y * step) / steps;

        paint_draw_brush(x, y);
    }
}

static ui_rect_t paint_damage_bounds(
    const ui_rect_t *canvas,
    int32_t first_x,
    int32_t first_y,
    int32_t second_x,
    int32_t second_y
)
{
    int32_t left = first_x < second_x ?
        first_x : second_x;

    int32_t top = first_y < second_y ?
        first_y : second_y;

    int32_t right = first_x > second_x ?
        first_x : second_x;

    int32_t bottom = first_y > second_y ?
        first_y : second_y;

    left -= PAINT_BRUSH_RADIUS + 2;
    top -= PAINT_BRUSH_RADIUS + 2;
    right += PAINT_BRUSH_RADIUS + 3;
    bottom += PAINT_BRUSH_RADIUS + 3;

    if (left < 0)
    {
        left = 0;
    }

    if (top < 0)
    {
        top = 0;
    }

    if (right > (int32_t)canvas->width)
    {
        right = (int32_t)canvas->width;
    }

    if (bottom > (int32_t)canvas->height)
    {
        bottom = (int32_t)canvas->height;
    }

    ui_rect_t damage = {
        .x = canvas->x + left,
        .y = canvas->y + top,
        .width = (uint32_t)(right - left),
        .height = (uint32_t)(bottom - top)
    };

    return damage;
}

void paint_render(const ui_rect_t *content)
{
    if (content == NULL)
    {
        return;
    }

    const gui_theme_t *theme =
        app_suite_theme();

    for (
        uint32_t index = 0;
        index < PAINT_COLOR_COUNT;
        index++
    )
    {
        ui_rect_t palette =
            paint_palette_bounds(content, index);

        ui_fill_rect(
            &palette,
            paint_colors[index]
        );

        ui_draw_border(
            &palette,
            index == paint_color_index ?
                theme->accent :
                theme->window_border,
            index == paint_color_index ? 3 : 1
        );
    }

    ui_rect_t clear =
        paint_clear_bounds(content);

    draw_button(&clear, "Clear", false);

    ui_rect_t canvas =
        paint_canvas_bounds(content);

    ui_fill_rect(&canvas, 0xFFFFFF);
    ui_draw_border(
        &canvas,
        theme->window_border,
        1
    );

    int32_t clip_x;
    int32_t clip_y;
    int32_t clip_right;
    int32_t clip_bottom;

    graphics_get_clip(
        &clip_x,
        &clip_y,
        &clip_right,
        &clip_bottom
    );

    int32_t start_x = clip_x > canvas.x ?
        clip_x : canvas.x;

    int32_t start_y = clip_y > canvas.y ?
        clip_y : canvas.y;

    int32_t end_x = clip_right <
        canvas.x + (int32_t)canvas.width ?
            clip_right :
            canvas.x + (int32_t)canvas.width;

    int32_t end_y = clip_bottom <
        canvas.y + (int32_t)canvas.height ?
            clip_bottom :
            canvas.y + (int32_t)canvas.height;

    for (int32_t y = start_y; y < end_y; y++)
    {
        uint32_t paint_y =
            (uint32_t)(y - canvas.y);

        for (int32_t x = start_x; x < end_x; x++)
        {
            uint32_t paint_x =
                (uint32_t)(x - canvas.x);

            draw_pixel(
                (uint32_t)x,
                (uint32_t)y,
                paint_pixels[
                    paint_y * PAINT_WIDTH +
                    paint_x
                ]
            );
        }
    }
}

bool paint_handle_mouse_down(
    const ui_rect_t *content,
    int32_t x,
    int32_t y,
    ui_rect_t *damage
)
{
    if (content == NULL)
    {
        return false;
    }

    for (
        uint32_t index = 0;
        index < PAINT_COLOR_COUNT;
        index++
    )
    {
        ui_rect_t palette =
            paint_palette_bounds(content, index);

        if (ui_point_in_rect(x, y, &palette))
        {
            paint_color_index = index;
            return true;
        }
    }

    ui_rect_t clear =
        paint_clear_bounds(content);

    if (ui_point_in_rect(x, y, &clear))
    {
        paint_clear_canvas();

        if (damage != NULL)
        {
            *damage = paint_canvas_bounds(content);
        }

        return true;
    }

    ui_rect_t canvas =
        paint_canvas_bounds(content);

    if (!ui_point_in_rect(x, y, &canvas))
    {
        return false;
    }

    paint_drawing = true;
    paint_last_x = x - canvas.x;
    paint_last_y = y - canvas.y;

    paint_draw_brush(
        paint_last_x,
        paint_last_y
    );

    if (damage != NULL)
    {
        *damage = paint_damage_bounds(
            &canvas,
            paint_last_x,
            paint_last_y,
            paint_last_x,
            paint_last_y
        );
    }

    return true;
}

bool paint_handle_mouse_move(
    const ui_rect_t *content,
    int32_t x,
    int32_t y,
    ui_rect_t *damage
)
{
    if (
        content == NULL ||
        !paint_drawing
    )
    {
        return false;
    }

    ui_rect_t canvas =
        paint_canvas_bounds(content);

    int32_t current_x = x - canvas.x;
    int32_t current_y = y - canvas.y;

    if (current_x < 0)
    {
        current_x = 0;
    }

    if (current_y < 0)
    {
        current_y = 0;
    }

    if (current_x >= (int32_t)canvas.width)
    {
        current_x = (int32_t)canvas.width - 1;
    }

    if (current_y >= (int32_t)canvas.height)
    {
        current_y = (int32_t)canvas.height - 1;
    }

    paint_draw_line(
        paint_last_x,
        paint_last_y,
        current_x,
        current_y
    );

    if (damage != NULL)
    {
        *damage = paint_damage_bounds(
            &canvas,
            paint_last_x,
            paint_last_y,
            current_x,
            current_y
        );
    }

    paint_last_x = current_x;
    paint_last_y = current_y;
    return true;
}

void paint_handle_mouse_up(void)
{
    paint_drawing = false;
}

static ui_rect_t settings_theme_button(
    const ui_rect_t *content,
    uint32_t index
)
{
    return make_button(
        content->x + 18,
        content->y + 60 +
            (int32_t)index * 44,
        content->width - 36,
        34
    );
}

void settings_render(const ui_rect_t *content)
{
    if (content == NULL)
    {
        return;
    }

    const gui_theme_t *theme =
        app_suite_theme();

    ui_draw_text(
        "Appearance",
        content->x + 18,
        content->y + 18,
        theme->text
    );

    ui_draw_text(
        "Choose a desktop theme:",
        content->x + 18,
        content->y + 38,
        theme->title_idle
    );

    static const char *labels[] = {
        "LatterOS Blue",
        "Midnight",
        "Forest"
    };

    for (uint32_t index = 0; index < 3; index++)
    {
        ui_rect_t button =
            settings_theme_button(
                content,
                index
            );

        draw_button(
            &button,
            labels[index],
            index == theme_index
        );
    }
}

bool settings_handle_click(
    const ui_rect_t *content,
    int32_t x,
    int32_t y
)
{
    if (content == NULL)
    {
        return false;
    }

    for (uint32_t index = 0; index < 3; index++)
    {
        ui_rect_t button =
            settings_theme_button(
                content,
                index
            );

        if (ui_point_in_rect(x, y, &button))
        {
            theme_index = index;

            char setting[2] = {
                (char)('0' + index),
                '\0'
            };

            (void)vfs_write_text(
                "/home/.theme",
                setting
            );

            return true;
        }
    }

    return false;
}

static const char *process_mode_name(
    process_mode_t mode
)
{
    return mode == PROCESS_USER ?
        "USER" : "KERNEL";
}

static const char *process_state_name(
    process_state_t state
)
{
    switch (state)
    {
        case PROCESS_READY:
            return "READY";

        case PROCESS_RUNNING:
            return "RUNNING";

        case PROCESS_TERMINATED:
            return "DEAD";

        default:
            return "UNUSED";
    }
}

static const char *application_state_name(
    gui_application_state_t state
)
{
    switch (state)
    {
        case GUI_APPLICATION_ACTIVE:
            return "ACTIVE";

        case GUI_APPLICATION_MINIMIZED:
            return "MINIMIZED";

        default:
            return "BACKGROUND";
    }
}

void process_manager_refresh(void)
{
    task_snapshot_count = 0;
    application_snapshot_count = 0;
    process_snapshot_count = 0;

    uint32_t application_count =
        gui_application_count();

    for (
        uint32_t index = 0;
        index < application_count &&
        task_snapshot_count <
            TASK_SNAPSHOT_COUNT;
        index++
    )
    {
        gui_application_info_t information;

        if (
            !gui_application_get(
                index,
                &information
            )
        )
        {
            continue;
        }

        task_snapshot_t *snapshot =
            &task_snapshots[
                task_snapshot_count
            ];

        snapshot->kind =
            TASK_SNAPSHOT_APPLICATION;
        snapshot->id = information.id;
        snapshot->application_state =
            information.state;
        snapshot->ticks = 0;
        snapshot->switches = 0;

        copy_text(
            snapshot->name,
            sizeof(snapshot->name),
            information.title
        );

        task_snapshot_count++;
        application_snapshot_count++;
    }

    irq_disable();

    uint32_t count = process_count();

    for (
        uint32_t index = 0;
        index < count &&
        task_snapshot_count <
            TASK_SNAPSHOT_COUNT;
        index++
    )
    {
        const process_t *process =
            process_get(index);

        if (process == NULL)
        {
            continue;
        }

        task_snapshot_t *snapshot =
            &task_snapshots[
                task_snapshot_count
            ];

        snapshot->kind =
            TASK_SNAPSHOT_PROCESS;
        snapshot->id = process->pid;
        snapshot->process_mode =
            process->mode;
        snapshot->process_state =
            process->state;
        snapshot->ticks =
            process->cpu_ticks;
        snapshot->switches =
            process->switches;

        copy_text(
            snapshot->name,
            sizeof(snapshot->name),
            process->name
        );

        task_snapshot_count++;
        process_snapshot_count++;
    }

    irq_enable();

    if (
        process_selected_row >=
        (int32_t)task_snapshot_count
    )
    {
        process_selected_row = -1;
    }

    ksnprintf(
        process_status,
        sizeof(process_status),
        "%u app(s), %u process(es)",
        application_snapshot_count,
        process_snapshot_count
    );
}

static ui_rect_t process_refresh_button(
    const ui_rect_t *content
)
{
    return make_button(
        content->x + 12,
        content->y + 12,
        82,
        24
    );
}

static ui_rect_t process_end_button(
    const ui_rect_t *content
)
{
    return make_button(
        content->x + 102,
        content->y + 12,
        92,
        24
    );
}

static ui_rect_t process_list_bounds(
    const ui_rect_t *content
)
{
    ui_rect_t rectangle = {
        .x = content->x + 12,
        .y = content->y + 62,
        .width = content->width - 24,
        .height = content->height - 74
    };

    return rectangle;
}

void process_manager_render(const ui_rect_t *content)
{
    if (content == NULL)
    {
        return;
    }

    const gui_theme_t *theme =
        app_suite_theme();

    ui_rect_t refresh =
        process_refresh_button(content);

    ui_rect_t end =
        process_end_button(content);

    draw_button(&refresh, "Refresh", false);
    draw_button(&end, "End task", true);

    ui_draw_text(
        process_status,
        content->x + 210,
        content->y + 20,
        theme->text
    );

    ui_draw_text(
        "TYPE  ID  MODE/STATE       TICKS  NAME",
        content->x + 14,
        content->y + 48,
        theme->text
    );

    ui_rect_t list =
        process_list_bounds(content);

    ui_fill_rect(&list, theme->field);
    ui_draw_border(
        &list,
        theme->window_border,
        1
    );

    uint32_t visible_rows =
        list.height / 22;

    if (visible_rows > task_snapshot_count)
    {
        visible_rows = task_snapshot_count;
    }

    for (
        uint32_t row = 0;
        row < visible_rows;
        row++
    )
    {
        ui_rect_t row_bounds = {
            .x = list.x + 2,
            .y = list.y + 2 +
                (int32_t)row * 22,
            .width = list.width - 4,
            .height = 21
        };

        if ((int32_t)row == process_selected_row)
        {
            ui_fill_rect(
                &row_bounds,
                theme->row_selected
            );
        }

        task_snapshot_t *snapshot =
            &task_snapshots[row];

        char line[112];

        if (
            snapshot->kind ==
            TASK_SNAPSHOT_APPLICATION
        )
        {
            ksnprintf(
                line,
                sizeof(line),
                "APP   %llu  %s  -  %s",
                (unsigned long long)
                    snapshot->id,
                application_state_name(
                    snapshot->application_state
                ),
                snapshot->name
            );
        }
        else
        {
            ksnprintf(
                line,
                sizeof(line),
                "PROC  %llu  %s/%s  %llu  %s",
                (unsigned long long)
                    snapshot->id,
                process_mode_name(
                    snapshot->process_mode
                ),
                process_state_name(
                    snapshot->process_state
                ),
                (unsigned long long)
                    snapshot->ticks,
                snapshot->name
            );
        }

        ui_draw_text(
            line,
            row_bounds.x + 5,
            row_bounds.y + 7,
            theme->text
        );
    }
}

bool process_manager_handle_click(
    const ui_rect_t *content,
    int32_t x,
    int32_t y
)
{
    if (content == NULL)
    {
        return false;
    }

    ui_rect_t refresh =
        process_refresh_button(content);

    if (ui_point_in_rect(x, y, &refresh))
    {
        process_manager_refresh();
        return true;
    }

    ui_rect_t end =
        process_end_button(content);

    if (ui_point_in_rect(x, y, &end))
    {
        if (
            process_selected_row < 0 ||
            process_selected_row >=
                (int32_t)task_snapshot_count
        )
        {
            copy_text(
                process_status,
                sizeof(process_status),
                "Select an application or process"
            );

            return true;
        }

        task_snapshot_t *snapshot =
            &task_snapshots[
                process_selected_row
            ];

        bool ended;

        if (
            snapshot->kind ==
            TASK_SNAPSHOT_APPLICATION
        )
        {
            ended = gui_application_close(
                (uint32_t)snapshot->id
            );

            copy_text(
                process_status,
                sizeof(process_status),
                ended ?
                    "Application closed" :
                    "Cannot close application"
            );
        }
        else
        {
            ended = process_terminate(
                snapshot->id
            );

            copy_text(
                process_status,
                sizeof(process_status),
                ended ?
                    "Process terminated" :
                    "Cannot terminate process"
            );
        }

        process_manager_refresh();
        return true;
    }

    ui_rect_t list =
        process_list_bounds(content);

    if (!ui_point_in_rect(x, y, &list))
    {
        return false;
    }

    int32_t relative_y = y - list.y - 2;

    if (relative_y < 0)
    {
        return false;
    }

    uint32_t row =
        (uint32_t)relative_y / 22;

    if (row >= task_snapshot_count)
    {
        return false;
    }

    process_selected_row = (int32_t)row;
    return true;
}

void app_suite_init(void)
{
    theme_index = 0;

    vfs_node_t *theme_file =
        vfs_open("/home/.theme");

    if (
        theme_file != NULL &&
        theme_file->type == VFS_NODE_FILE
    )
    {
        char saved_theme = '\0';

        if (
            vfs_read(
                theme_file,
                0,
                &saved_theme,
                1
            ) == 1 &&
            saved_theme >= '0' &&
            saved_theme <= '2'
        )
        {
            theme_index =
                (uint32_t)(
                    saved_theme - '0'
                );
        }
    }

    editor_node = NULL;

    copy_text(
        editor_path,
        sizeof(editor_path),
        "/home/notes.txt"
    );

    copy_text(
        editor_text,
        sizeof(editor_text),
        "Welcome to LatterOS Editor.\n"
        "Open a file from File Explorer or type here."
    );

    editor_length = text_length(editor_text);

    copy_text(
        editor_status,
        sizeof(editor_status),
        "New file"
    );

    clear_text(
        calculator_expression,
        sizeof(calculator_expression)
    );

    calculator_length = 0;

    copy_text(
        calculator_result,
        sizeof(calculator_result),
        "Ready"
    );

    paint_color_index = 0;
    paint_drawing = false;
    paint_clear_canvas();

    task_snapshot_count = 0;
    application_snapshot_count = 0;
    process_snapshot_count = 0;
    process_selected_row = -1;

    copy_text(
        process_status,
        sizeof(process_status),
        "Click Refresh"
    );
}
