#ifndef DESKTOP_SERVICES_H
#define DESKTOP_SERVICES_H

#include "app_suite.h"
#include "ui.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DESKTOP_SERVICE_WINDOW_COUNT 8U
#define DESKTOP_RECENT_FILE_COUNT 5U
#define DESKTOP_RECENT_PATH_CAPACITY 256U
#define DESKTOP_CLIPBOARD_CAPACITY 1024U
#define DESKTOP_NOTIFICATION_CAPACITY 4U
#define DESKTOP_NOTIFICATION_TEXT_CAPACITY 96U

typedef enum
{
    DESKTOP_WALLPAPER_SOLID,
    DESKTOP_WALLPAPER_GRADIENT,
    DESKTOP_WALLPAPER_GRID,
    DESKTOP_WALLPAPER_NIGHT,
    DESKTOP_WALLPAPER_COUNT
} desktop_wallpaper_t;

typedef enum
{
    DESKTOP_THEME_BLUE,
    DESKTOP_THEME_GRAPHITE,
    DESKTOP_THEME_TEAL,
    DESKTOP_THEME_AUBERGINE,
    DESKTOP_THEME_EXTERNAL,
    DESKTOP_THEME_LDS,
    DESKTOP_THEME_COUNT
} desktop_theme_kind_t;

typedef enum
{
    DESKTOP_ASSOCIATION_UNKNOWN,
    DESKTOP_ASSOCIATION_TEXT,
    DESKTOP_ASSOCIATION_IMAGE,
    DESKTOP_ASSOCIATION_EXECUTABLE,
    DESKTOP_ASSOCIATION_PACKAGE
} desktop_association_t;

typedef struct
{
    bool valid;
    ui_rect_t bounds;
} desktop_saved_window_t;

typedef struct
{
    bool active;
    uint64_t id;
    uint64_t expires_at;
    char text[DESKTOP_NOTIFICATION_TEXT_CAPACITY];
} desktop_notification_t;

void desktop_services_init(void);

const gui_theme_t *desktop_services_theme(void);
desktop_theme_kind_t desktop_services_theme_kind(void);
desktop_wallpaper_t desktop_services_wallpaper(void);

void desktop_services_cycle_theme(void);
void desktop_services_cycle_wallpaper(void);
bool desktop_services_set_theme(desktop_theme_kind_t kind);
bool desktop_services_set_wallpaper(desktop_wallpaper_t kind);
bool desktop_services_reset_configuration(void);
bool desktop_services_reload_external_theme(void);
const char *desktop_services_theme_name(void);
const char *desktop_services_wallpaper_name(void);

void desktop_services_store_window(
    uint32_t index,
    const ui_rect_t *bounds
);

bool desktop_services_restore_window(
    uint32_t index,
    ui_rect_t *bounds
);

bool desktop_services_save(void);

void desktop_clipboard_clear(void);
bool desktop_clipboard_set_text(const char *text);
bool desktop_clipboard_set_range(
    const char *text,
    size_t start,
    size_t end
);
const char *desktop_clipboard_text(void);
size_t desktop_clipboard_length(void);
bool desktop_clipboard_has_text(void);
uint64_t desktop_clipboard_generation(void);

void desktop_recent_add(const char *path);
uint32_t desktop_recent_count(void);
const char *desktop_recent_path(uint32_t index);

desktop_association_t desktop_association_for_path(
    const char *path
);

void desktop_notify(const char *text, uint32_t lifetime_ms);
void desktop_notifications_update(void);
uint32_t desktop_notification_count(void);
const desktop_notification_t *desktop_notification_get(
    uint32_t index
);
void desktop_notification_dismiss(uint64_t id);
uint64_t desktop_notification_total(void);

#endif
