#ifndef SERIAL_H
#define SERIAL_H

#include <stdbool.h>

bool serial_init(void);
bool serial_is_available(void);
bool serial_self_test(void);

bool serial_write_character(char character);
void serial_write(const char *text);
void serial_write_line(const char *text);

#endif
