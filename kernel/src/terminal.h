#ifndef TERMINAL_H
#define TERMINAL_H

void terminal_init(void);
void terminal_put_character(char character);

void terminal_write(const char *text);
void terminal_write_line(const char *text);
void terminal_clear(void);

#endif