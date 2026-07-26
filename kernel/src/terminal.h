#ifndef TERMINAL_H
#define TERMINAL_H

typedef void (*terminal_output_handler_t)(
    const char *text
);

typedef void (*terminal_clear_handler_t)(void);

void terminal_init(void);
void terminal_put_character(char character);

void terminal_write(const char *text);
void terminal_write_line(const char *text);
void terminal_clear(void);

void terminal_set_redirect(
    terminal_output_handler_t output_handler,
    terminal_clear_handler_t clear_handler
);

void terminal_clear_redirect(void);

#endif
