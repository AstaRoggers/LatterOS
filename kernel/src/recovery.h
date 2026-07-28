#ifndef RECOVERY_H
#define RECOVERY_H

#include "ui.h"

#include <stdbool.h>
#include <stdint.h>

void recovery_init(void);
void recovery_refresh(void);
void recovery_render(const ui_rect_t *content);
bool recovery_handle_click(
    const ui_rect_t *content,
    int32_t x,
    int32_t y
);

bool recovery_take_reinstall_request(uint32_t *target_index);
const char *recovery_status(void);
uint32_t recovery_selected_target(void);

#endif
