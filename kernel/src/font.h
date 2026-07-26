#ifndef FONT_H
#define FONT_H

#include <stdint.h>

#define FONT_WIDTH 8
#define FONT_HEIGHT 8
#define FONT_ADVANCE 8

const uint8_t *font_get_character(char character);

#endif
