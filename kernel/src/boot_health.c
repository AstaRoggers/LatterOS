#include "boot_health.h"

#include "boot_mode.h"
#include "platform_detect.h"
#include "release_info.h"
#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BOOT_HEALTH_DIRECTORY "/home/user/.latteros"
#define BOOT_HEALTH_STATE_PATH BOOT_HEALTH_DIRECTORY "/boot-health.state"
#define BOOT_HEALTH_REPORT_PATH \
    "/home/user/Documents/Boot Health Report.txt"
#define BOOT_HEALTH_BUFFER_CAPACITY 1024U
#define BOOT_HEALTH_REPORT_CAPACITY 2048U

static bool initialized;
static bool previous_incomplete;
static bool desktop_ready;
static uint32_t attempt_count;
static uint32_t failure_count;
static char state_buffer[BOOT_HEALTH_BUFFER_CAPACITY];
static char report_buffer[BOOT_HEALTH_REPORT_CAPACITY];

static void clear_bytes(void *pointer, size_t count)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0; index < count; index++)
    {
        bytes[index] = 0;
    }
}

static bool starts_with(const char *text, const char *prefix)
{
    if (text == NULL || prefix == NULL)
    {
        return false;
    }

    size_t index = 0;

    while (prefix[index] != '\0')
    {
        if (text[index] != prefix[index])
        {
            return false;
        }

        index++;
    }

    return true;
}

static bool contains_text(const char *text, const char *needle)
{
    if (text == NULL || needle == NULL || needle[0] == '\0')
    {
        return false;
    }

    for (size_t start = 0; text[start] != '\0'; start++)
    {
        size_t offset = 0;

        while (
            needle[offset] != '\0' &&
            text[start + offset] == needle[offset]
        )
        {
            offset++;
        }

        if (needle[offset] == '\0')
        {
            return true;
        }
    }

    return false;
}

static uint32_t parse_unsigned_field(
    const char *text,
    const char *field
)
{
    if (text == NULL || field == NULL)
    {
        return 0U;
    }

    for (size_t start = 0; text[start] != '\0'; start++)
    {
        if (!starts_with(&text[start], field))
        {
            continue;
        }

        size_t index = start;

        while (field[index - start] != '\0')
        {
            index++;
        }

        uint32_t value = 0U;
        bool found = false;

        while (text[index] >= '0' && text[index] <= '9')
        {
            uint32_t digit = (uint32_t)(text[index] - '0');

            if (value > (UINT32_MAX - digit) / 10U)
            {
                return UINT32_MAX;
            }

            value = value * 10U + digit;
            found = true;
            index++;
        }

        return found ? value : 0U;
    }

    return 0U;
}

static void append_character(
    char *buffer,
    size_t capacity,
    size_t *position,
    char character
)
{
    if (
        buffer == NULL ||
        position == NULL ||
        *position + 1U >= capacity
    )
    {
        return;
    }

    buffer[*position] = character;
    (*position)++;
    buffer[*position] = '\0';
}

static void append_text(
    char *buffer,
    size_t capacity,
    size_t *position,
    const char *text
)
{
    if (text == NULL)
    {
        return;
    }

    for (size_t index = 0; text[index] != '\0'; index++)
    {
        append_character(buffer, capacity, position, text[index]);
    }
}

static void append_unsigned(
    char *buffer,
    size_t capacity,
    size_t *position,
    uint32_t value
)
{
    char reverse[16];
    uint32_t count = 0U;

    do
    {
        reverse[count++] = (char)('0' + value % 10U);
        value /= 10U;
    }
    while (value != 0U && count < sizeof(reverse));

    while (count > 0U)
    {
        append_character(buffer, capacity, position, reverse[--count]);
    }
}

static bool ensure_directory(const char *path)
{
    vfs_node_t *node = vfs_open(path);

    if (node != NULL)
    {
        return node->type == VFS_NODE_DIRECTORY;
    }

    return vfs_make_directory(path);
}

static size_t read_state(void)
{
    vfs_node_t *node = vfs_open(BOOT_HEALTH_STATE_PATH);

    if (node == NULL || node->type != VFS_NODE_FILE)
    {
        return 0U;
    }

    size_t count = vfs_read(
        node,
        0U,
        state_buffer,
        sizeof(state_buffer) - 1U
    );

    state_buffer[count] = '\0';
    return count;
}

static bool write_state(const char *state)
{
    clear_bytes(state_buffer, sizeof(state_buffer));
    size_t position = 0U;

    append_text(state_buffer, sizeof(state_buffer), &position, "state=");
    append_text(state_buffer, sizeof(state_buffer), &position, state);
    append_text(state_buffer, sizeof(state_buffer), &position, "\nattempts=");
    append_unsigned(state_buffer, sizeof(state_buffer), &position, attempt_count);
    append_text(state_buffer, sizeof(state_buffer), &position, "\nfailures=");
    append_unsigned(state_buffer, sizeof(state_buffer), &position, failure_count);
    append_text(state_buffer, sizeof(state_buffer), &position, "\nmode=");
    append_text(state_buffer, sizeof(state_buffer), &position, boot_mode_name());
    append_text(state_buffer, sizeof(state_buffer), &position, "\nsource=");
    append_text(state_buffer, sizeof(state_buffer), &position, boot_source_name());
    append_text(state_buffer, sizeof(state_buffer), &position, "\nplatform=");
    append_text(
        state_buffer,
        sizeof(state_buffer),
        &position,
        platform_hypervisor_name()
    );
    append_text(state_buffer, sizeof(state_buffer), &position, "\nversion=");
    append_text(state_buffer, sizeof(state_buffer), &position, release_info_version());
    append_character(state_buffer, sizeof(state_buffer), &position, '\n');

    return vfs_write_text(BOOT_HEALTH_STATE_PATH, state_buffer);
}

void boot_health_init(void)
{
    if (initialized)
    {
        return;
    }

    initialized = true;
    previous_incomplete = false;
    desktop_ready = false;
    attempt_count = 0U;
    failure_count = 0U;
    clear_bytes(state_buffer, sizeof(state_buffer));
    clear_bytes(report_buffer, sizeof(report_buffer));

    platform_detect_init();

    if (
        !ensure_directory("/home") ||
        !ensure_directory("/home/user") ||
        !ensure_directory(BOOT_HEALTH_DIRECTORY)
    )
    {
        return;
    }

    if (read_state() != 0U)
    {
        previous_incomplete = contains_text(state_buffer, "state=booting");
        attempt_count = parse_unsigned_field(state_buffer, "attempts=");
        failure_count = parse_unsigned_field(state_buffer, "failures=");

        if (previous_incomplete && failure_count != UINT32_MAX)
        {
            failure_count++;
        }
    }

    if (attempt_count != UINT32_MAX)
    {
        attempt_count++;
    }

    (void)write_state("booting");
    (void)boot_health_write_report();
}

void boot_health_mark_desktop_ready(void)
{
    boot_health_init();

    if (desktop_ready)
    {
        return;
    }

    desktop_ready = true;
    (void)write_state("ready");
    (void)boot_health_write_report();
}

bool boot_health_previous_incomplete(void)
{
    boot_health_init();
    return previous_incomplete;
}

uint32_t boot_health_attempt_count(void)
{
    boot_health_init();
    return attempt_count;
}

uint32_t boot_health_failure_count(void)
{
    boot_health_init();
    return failure_count;
}

const char *boot_health_status(void)
{
    boot_health_init();

    if (desktop_ready)
    {
        return "desktop-ready";
    }

    return previous_incomplete ?
        "recovering-after-incomplete-boot" :
        "booting";
}

bool boot_health_write_report(void)
{
    clear_bytes(report_buffer, sizeof(report_buffer));
    size_t position = 0U;

    append_text(report_buffer, sizeof(report_buffer), &position, "LatterOS Boot Health Report\n");
    append_text(report_buffer, sizeof(report_buffer), &position, "Milestone 20D\n\n");
    append_text(report_buffer, sizeof(report_buffer), &position, "Status: ");
    append_text(report_buffer, sizeof(report_buffer), &position, desktop_ready ? "desktop-ready" : "booting");
    append_text(report_buffer, sizeof(report_buffer), &position, "\nPrevious boot incomplete: ");
    append_text(report_buffer, sizeof(report_buffer), &position, previous_incomplete ? "yes" : "no");
    append_text(report_buffer, sizeof(report_buffer), &position, "\nBoot attempts: ");
    append_unsigned(report_buffer, sizeof(report_buffer), &position, attempt_count);
    append_text(report_buffer, sizeof(report_buffer), &position, "\nIncomplete boots: ");
    append_unsigned(report_buffer, sizeof(report_buffer), &position, failure_count);
    append_text(report_buffer, sizeof(report_buffer), &position, "\nCurrent mode: ");
    append_text(report_buffer, sizeof(report_buffer), &position, boot_mode_name());
    append_text(report_buffer, sizeof(report_buffer), &position, "\nBoot source: ");
    append_text(report_buffer, sizeof(report_buffer), &position, boot_source_name());
    append_text(report_buffer, sizeof(report_buffer), &position, "\nPlatform: ");
    append_text(report_buffer, sizeof(report_buffer), &position, platform_hypervisor_name());
    append_text(report_buffer, sizeof(report_buffer), &position, "\nHypervisor vendor: ");
    append_text(report_buffer, sizeof(report_buffer), &position, platform_hypervisor_vendor());
    append_text(report_buffer, sizeof(report_buffer), &position, "\nRelease: ");
    append_text(report_buffer, sizeof(report_buffer), &position, release_info_version());
    append_character(report_buffer, sizeof(report_buffer), &position, '\n');

    return vfs_write_text(BOOT_HEALTH_REPORT_PATH, report_buffer);
}

const char *boot_health_report_path(void)
{
    return BOOT_HEALTH_REPORT_PATH;
}
