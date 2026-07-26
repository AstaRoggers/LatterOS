#ifndef COMPOSITOR_H
#define COMPOSITOR_H

#include "ui.h"

#include <stdbool.h>

typedef void (*compositor_render_function_t)(void);

void compositor_init(
    compositor_render_function_t renderer
);

void compositor_invalidate(
    const ui_rect_t *rectangle
);

void compositor_invalidate_all(void);

bool compositor_has_damage(void);
void compositor_render(void);

#endif
