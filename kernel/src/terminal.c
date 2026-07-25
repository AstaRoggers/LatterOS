#include "terminal.h"
#include "graphics.h"
#include "shell.h"

#include <stdint.h>

#define TERMINAL_X 100
#define TERMINAL_Y 100

#define TERMINAL_FOREGROUND 0xFFFFFF
#define TERMINAL_BACKGROUND 0x0066CC

#define CHARACTER_WIDTH 8
#define CHARACTER_HEIGHT 8
#define LINE_HEIGHT 12

#define PROMPT "> "

#define COMMAND_BUFFER_SIZE 128

static uint32_t cursor_x;
static uint32_t cursor_y;

static char command_buffer[COMMAND_BUFFER_SIZE];
static uint32_t command_length;

static void terminal_clear_command_buffer(void)
{
    for (uint32_t i = 0; i < COMMAND_BUFFER_SIZE; i++)
    {
        command_buffer[i] = '\0';
    }

    command_length = 0;
}

static void terminal_new_line(void)
{
    cursor_x = TERMINAL_X;
    cursor_y += LINE_HEIGHT;
}

static void terminal_draw_prompt(void)
{
    terminal_write(PROMPT);
}

static void terminal_draw_header(void)
{
    terminal_write_line("LatterOS v0.1");
}

void terminal_write(const char *text)
{
    for (uint32_t i = 0; text[i] != '\0'; i++)
    {
        if (text[i] == '\n')
        {
            terminal_new_line();
            continue;
        }

        draw_character(
            text[i],
            cursor_x,
            cursor_y,
            TERMINAL_FOREGROUND
        );

        cursor_x += CHARACTER_WIDTH;
    }
}

void terminal_write_line(const char *text)
{
    terminal_write(text);
    terminal_new_line();
}

void terminal_clear(void)
{
    graphics_clear(TERMINAL_BACKGROUND);

    cursor_x = TERMINAL_X;
    cursor_y = TERMINAL_Y;

    terminal_draw_header();
}

void terminal_init(void)
{
    cursor_x = TERMINAL_X;
    cursor_y = TERMINAL_Y;

    terminal_clear_command_buffer();

    terminal_draw_header();
    terminal_draw_prompt();
}

void terminal_put_character(char character)
{
    if (character == '\b')
    {
        if (command_length == 0)
        {
            return;
        }

        command_length--;
        command_buffer[command_length] = '\0';

        cursor_x -= CHARACTER_WIDTH;

        draw_rectangle(
            cursor_x,
            cursor_y,
            CHARACTER_WIDTH,
            CHARACTER_HEIGHT,
            TERMINAL_BACKGROUND
        );

        return;
    }

    if (character == '\n')
    {
        command_buffer[command_length] = '\0';

        terminal_new_line();

        shell_execute(command_buffer);

        terminal_clear_command_buffer();
        terminal_draw_prompt();

        return;
    }

    if (command_length >= COMMAND_BUFFER_SIZE - 1)
    {
        return;
    }

    command_buffer[command_length] = character;
    command_length++;
    command_buffer[command_length] = '\0';

    draw_character(
        character,
        cursor_x,
        cursor_y,
        TERMINAL_FOREGROUND
    );

    cursor_x += CHARACTER_WIDTH;
}