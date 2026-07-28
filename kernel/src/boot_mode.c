#include "boot_mode.h"

#include "vfs.h"

#include <limine.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FIRST_BOOT_MARKER "/home/user/.latteros-first-boot-complete"
#define FIRST_BOOT_GUIDE  "/home/user/Documents/Getting Started.txt"

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_cmdline_request command_line_request = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID,
    .revision = 0
};

static bool initialized;
static bool first_boot_checked;
static bool first_boot_detected;
static boot_mode_kind_t selected_mode;
static boot_source_kind_t selected_source;

static bool token_equal(
    const char *token,
    size_t token_length,
    const char *expected
)
{
    if (token == NULL || expected == NULL)
    {
        return false;
    }

    size_t index = 0;

    while (index < token_length && expected[index] != '\0')
    {
        if (token[index] != expected[index])
        {
            return false;
        }

        index++;
    }

    return index == token_length && expected[index] == '\0';
}

static void parse_token(const char *token, size_t length)
{
    if (token_equal(token, length, "mode=safe"))
    {
        selected_mode = BOOT_MODE_SAFE;
    }
    else if (token_equal(token, length, "mode=recovery"))
    {
        selected_mode = BOOT_MODE_RECOVERY;
    }
    else if (token_equal(token, length, "mode=hardware"))
    {
        selected_mode = BOOT_MODE_HARDWARE_TEST;
    }
    else if (token_equal(token, length, "mode=compatibility"))
    {
        selected_mode = BOOT_MODE_COMPATIBILITY;
    }
    else if (token_equal(token, length, "mode=virtualbox"))
    {
        selected_mode = BOOT_MODE_VIRTUALBOX;
    }
    else if (token_equal(token, length, "mode=normal"))
    {
        selected_mode = BOOT_MODE_NORMAL;
    }
    else if (token_equal(token, length, "source=installer"))
    {
        selected_source = BOOT_SOURCE_INSTALLER;
    }
    else if (token_equal(token, length, "source=installed"))
    {
        selected_source = BOOT_SOURCE_INSTALLED;
    }
    else if (token_equal(token, length, "source=recovery-media"))
    {
        selected_source = BOOT_SOURCE_RECOVERY_MEDIA;
    }
}

void boot_mode_init(void)
{
    if (initialized)
    {
        return;
    }

    initialized = true;
    selected_mode = BOOT_MODE_NORMAL;
    selected_source = BOOT_SOURCE_UNKNOWN;
    first_boot_checked = false;
    first_boot_detected = false;

    struct limine_executable_cmdline_response *response =
        command_line_request.response;

    if (response == NULL || response->cmdline == NULL)
    {
        return;
    }

    const char *cursor = response->cmdline;

    while (*cursor != '\0')
    {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n')
        {
            cursor++;
        }

        if (*cursor == '\0')
        {
            break;
        }

        const char *token = cursor;

        while (
            *cursor != '\0' &&
            *cursor != ' ' &&
            *cursor != '\t' &&
            *cursor != '\n'
        )
        {
            cursor++;
        }

        parse_token(token, (size_t)(cursor - token));
    }
}

boot_mode_kind_t boot_mode_kind(void)
{
    boot_mode_init();
    return selected_mode;
}

boot_source_kind_t boot_mode_source(void)
{
    boot_mode_init();
    return selected_source;
}

bool boot_mode_is_safe(void)
{
    return boot_mode_kind() == BOOT_MODE_SAFE;
}

bool boot_mode_is_recovery(void)
{
    return boot_mode_kind() == BOOT_MODE_RECOVERY;
}

bool boot_mode_is_hardware_test(void)
{
    return boot_mode_kind() == BOOT_MODE_HARDWARE_TEST;
}

bool boot_mode_is_compatibility(void)
{
    return boot_mode_kind() == BOOT_MODE_COMPATIBILITY;
}

bool boot_mode_is_virtualbox(void)
{
    return boot_mode_kind() == BOOT_MODE_VIRTUALBOX;
}

bool boot_mode_is_installed(void)
{
    return boot_mode_source() == BOOT_SOURCE_INSTALLED;
}

bool boot_mode_conservative_graphics(void)
{
    boot_mode_kind_t mode = boot_mode_kind();
    return
        mode == BOOT_MODE_SAFE ||
        mode == BOOT_MODE_RECOVERY ||
        mode == BOOT_MODE_HARDWARE_TEST ||
        mode == BOOT_MODE_COMPATIBILITY ||
        mode == BOOT_MODE_VIRTUALBOX;
}

const char *boot_mode_name(void)
{
    switch (boot_mode_kind())
    {
        case BOOT_MODE_SAFE:
            return "Safe Mode";

        case BOOT_MODE_RECOVERY:
            return "Recovery";

        case BOOT_MODE_HARDWARE_TEST:
            return "Hardware Test";

        case BOOT_MODE_COMPATIBILITY:
            return "Compatibility Mode";

        case BOOT_MODE_VIRTUALBOX:
            return "VirtualBox Mode";

        case BOOT_MODE_NORMAL:
        default:
            return "Normal";
    }
}

const char *boot_source_name(void)
{
    switch (boot_mode_source())
    {
        case BOOT_SOURCE_INSTALLER:
            return "Installer media";

        case BOOT_SOURCE_INSTALLED:
            return "Installed system";

        case BOOT_SOURCE_RECOVERY_MEDIA:
            return "Recovery media";

        case BOOT_SOURCE_UNKNOWN:
        default:
            return "Unknown source";
    }
}

bool boot_mode_prepare_first_boot(void)
{
    boot_mode_init();

    if (first_boot_checked)
    {
        return first_boot_detected;
    }

    first_boot_checked = true;

    if (!boot_mode_is_installed())
    {
        return false;
    }

    if (vfs_open(FIRST_BOOT_MARKER) != NULL)
    {
        return false;
    }

    first_boot_detected = vfs_write_text(
        FIRST_BOOT_MARKER,
        "LatterOS installed-system first boot completed.\n"
        "milestone=20D\n"
    );

    if (first_boot_detected && vfs_open(FIRST_BOOT_GUIDE) == NULL)
    {
        (void)vfs_write_text(
            FIRST_BOOT_GUIDE,
            "Welcome to LatterOS.\n\n"
            "Open Settings to choose a desktop theme, including the LDS theme.\n"
            "Use System > Recovery tools for disk checks and recovery actions.\n"
            "At boot, choose Safe Mode for conservative software graphics.\n"
            "Choose Hardware Test to open the compatibility dashboard.\n"
            "Choose Compatibility Mode for conservative software graphics and stability testing.\n"
        );
    }

    return first_boot_detected;
}

bool boot_mode_first_boot(void)
{
    return first_boot_checked && first_boot_detected;
}
