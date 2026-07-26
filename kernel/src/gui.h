#ifndef GUI_H
#define GUI_H

#include <stdbool.h>
#include <stdint.h>

void gui_init(void);
void gui_request_start(void);
void gui_update(void);
bool gui_is_active(void);

typedef enum
{
    GUI_APPLICATION_ACTIVE,
    GUI_APPLICATION_BACKGROUND,
    GUI_APPLICATION_MINIMIZED
} gui_application_state_t;

typedef struct
{
    uint32_t id;
    const char *title;
    gui_application_state_t state;
} gui_application_info_t;

uint32_t gui_application_count(void);
bool gui_application_get(
    uint32_t index,
    gui_application_info_t *information
);
bool gui_application_close(uint32_t id);

#endif
