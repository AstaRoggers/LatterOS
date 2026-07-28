#include "recovery.h"

#include "block_device.h"
#include "desktop_services.h"
#include "fat_fs.h"
#include "installer.h"
#include "ui_controls.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RECOVERY_STATUS_CAPACITY 160U
#define RECOVERY_GPT_ENTRY_SIZE 128U
#define RECOVERY_GPT_HEADER_SIZE 92U
#define RECOVERY_GPT_SIGNATURE 0x5452415020494645ULL

static uint32_t selected_target;
static bool reinstall_requested;
static char status_text[RECOVERY_STATUS_CAPACITY];
static uint8_t sector[512];

static void clear_bytes(void *pointer, size_t count)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0; index < count; index++)
    {
        bytes[index] = 0;
    }
}

static void copy_text(
    char *destination,
    size_t capacity,
    const char *source
)
{
    if (destination == NULL || capacity == 0U)
    {
        return;
    }

    size_t index = 0;

    if (source != NULL)
    {
        while (source[index] != '\0' && index + 1U < capacity)
        {
            destination[index] = source[index];
            index++;
        }
    }

    destination[index] = '\0';
}

static void append_text(
    char *destination,
    size_t capacity,
    const char *source
)
{
    size_t length = 0;

    while (length < capacity && destination[length] != '\0')
    {
        length++;
    }

    if (length >= capacity)
    {
        return;
    }

    size_t source_index = 0;

    while (
        source != NULL &&
        source[source_index] != '\0' &&
        length + 1U < capacity
    )
    {
        destination[length++] = source[source_index++];
    }

    destination[length] = '\0';
}

static void append_unsigned(
    char *destination,
    size_t capacity,
    uint64_t value
)
{
    char reverse[21];
    uint32_t count = 0;

    if (value == 0U)
    {
        append_text(destination, capacity, "0");
        return;
    }

    while (value != 0U && count < sizeof(reverse))
    {
        reverse[count++] = (char)('0' + value % 10U);
        value /= 10U;
    }

    while (count != 0U)
    {
        char text[2] = { reverse[--count], '\0' };
        append_text(destination, capacity, text);
    }
}

static uint32_t read_u32(const uint8_t *buffer, uint32_t offset)
{
    return
        (uint32_t)buffer[offset] |
        ((uint32_t)buffer[offset + 1U] << 8) |
        ((uint32_t)buffer[offset + 2U] << 16) |
        ((uint32_t)buffer[offset + 3U] << 24);
}

static uint64_t read_u64(const uint8_t *buffer, uint32_t offset)
{
    return
        (uint64_t)read_u32(buffer, offset) |
        ((uint64_t)read_u32(buffer, offset + 4U) << 32);
}

static uint32_t crc32_bytes(const uint8_t *data, uint32_t count)
{
    uint32_t crc = 0xFFFFFFFFU;

    for (uint32_t index = 0; index < count; index++)
    {
        crc ^= data[index];

        for (uint32_t bit = 0; bit < 8U; bit++)
        {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }

    return ~crc;
}

static bool guid_present(const uint8_t *entry)
{
    for (uint32_t index = 0; index < 16U; index++)
    {
        if (entry[index] != 0U)
        {
            return true;
        }
    }

    return false;
}

static bool bytes_equal(
    const uint8_t *buffer,
    uint32_t offset,
    const char *text,
    uint32_t count
)
{
    for (uint32_t index = 0; index < count; index++)
    {
        if (buffer[offset + index] != (uint8_t)text[index])
        {
            return false;
        }
    }

    return true;
}

static bool verify_fat32_boot(
    const block_device_t *device,
    uint64_t first_lba
)
{
    return
        block_device_read(device, first_lba, 1U, sector) &&
        sector[510] == 0x55U &&
        sector[511] == 0xAAU &&
        bytes_equal(sector, 82U, "FAT32   ", 8U);
}

static bool verify_selected_disk(void)
{
    installer_target_info_t target;

    if (!installer_target_get(selected_target, &target))
    {
        copy_text(status_text, sizeof(status_text), "Select an available disk first");
        return false;
    }

    const block_device_t *device = block_device_get(target.device_index);

    if (
        device == NULL ||
        device->sector_size != 512U ||
        !block_device_read(device, 1U, 1U, sector)
    )
    {
        copy_text(status_text, sizeof(status_text), "Unable to read the GPT header");
        return false;
    }

    if (read_u64(sector, 0U) != RECOVERY_GPT_SIGNATURE)
    {
        copy_text(status_text, sizeof(status_text), "GPT signature is missing");
        return false;
    }

    uint32_t header_size = read_u32(sector, 12U);
    uint32_t stored_crc = read_u32(sector, 16U);

    if (
        header_size < RECOVERY_GPT_HEADER_SIZE ||
        header_size > 512U
    )
    {
        copy_text(status_text, sizeof(status_text), "GPT header size is invalid");
        return false;
    }

    sector[16] = 0;
    sector[17] = 0;
    sector[18] = 0;
    sector[19] = 0;

    if (crc32_bytes(sector, header_size) != stored_crc)
    {
        copy_text(status_text, sizeof(status_text), "GPT header CRC check failed");
        return false;
    }

    uint64_t entries_lba = read_u64(sector, 72U);
    uint32_t entry_size = read_u32(sector, 84U);

    if (
        entry_size != RECOVERY_GPT_ENTRY_SIZE ||
        !block_device_read(device, entries_lba, 1U, sector)
    )
    {
        copy_text(status_text, sizeof(status_text), "GPT partition entries are unavailable");
        return false;
    }

    const uint8_t *efi = &sector[0];
    const uint8_t *system = &sector[RECOVERY_GPT_ENTRY_SIZE];

    if (!guid_present(efi) || !guid_present(system))
    {
        copy_text(status_text, sizeof(status_text), "EFI or system partition is missing");
        return false;
    }

    uint64_t efi_first = read_u64(efi, 32U);
    uint64_t efi_last = read_u64(efi, 40U);
    uint64_t system_first = read_u64(system, 32U);
    uint64_t system_last = read_u64(system, 40U);

    if (
        efi_first == 0U ||
        efi_last < efi_first ||
        system_first == 0U ||
        system_last < system_first ||
        system_last >= device->sector_count
    )
    {
        copy_text(status_text, sizeof(status_text), "Installed partition bounds are invalid");
        return false;
    }

    if (!verify_fat32_boot(device, efi_first))
    {
        copy_text(status_text, sizeof(status_text), "EFI FAT32 volume check failed");
        return false;
    }

    if (!verify_fat32_boot(device, system_first))
    {
        copy_text(status_text, sizeof(status_text), "System FAT32 volume check failed");
        return false;
    }

    copy_text(
        status_text,
        sizeof(status_text),
        "PASS: GPT, EFI FAT32, and system FAT32 structures verified"
    );
    return true;
}

static ui_palette_t recovery_palette(void)
{
    const gui_theme_t *theme = desktop_services_theme();

    return (ui_palette_t){
        .background = theme->desktop,
        .panel = theme->window,
        .border = theme->window_border,
        .text = theme->text,
        .light_text = theme->light_text,
        .field = theme->field,
        .accent = theme->accent,
        .accent_hover = theme->row_selected,
        .selected = theme->row_selected,
        .disabled = theme->title_idle,
        .danger = theme->close
    };
}

static ui_rect_t make_rect(
    const ui_rect_t *content,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height
)
{
    return (ui_rect_t){
        .x = content->x + x,
        .y = content->y + y,
        .width = width,
        .height = height
    };
}

static ui_rect_t target_list_bounds(const ui_rect_t *content)
{
    return make_rect(content, 16, 52, content->width - 32U, 74U);
}

static ui_rect_t action_button(
    const ui_rect_t *content,
    uint32_t column,
    uint32_t row
)
{
    uint32_t gap = 10U;
    uint32_t width = (content->width - 42U) / 2U;

    return make_rect(
        content,
        16 + (int32_t)column * (int32_t)(width + gap),
        142 + (int32_t)row * 42,
        width,
        32U
    );
}

static void target_label(
    const installer_target_info_t *target,
    char *buffer,
    size_t capacity
)
{
    buffer[0] = '\0';

    if (target == NULL)
    {
        append_text(buffer, capacity, "No recovery target");
        return;
    }

    append_text(buffer, capacity, target->name);
    append_text(buffer, capacity, " - ");
    append_unsigned(buffer, capacity, target->capacity_mib);
    append_text(buffer, capacity, " MiB");
}

void recovery_init(void)
{
    selected_target = 0U;
    reinstall_requested = false;
    clear_bytes(status_text, sizeof(status_text));
    recovery_refresh();
}

void recovery_refresh(void)
{
    installer_refresh();

    if (installer_target_count() == 0U)
    {
        selected_target = UINT32_MAX;
        copy_text(status_text, sizeof(status_text), installer_last_error());
        return;
    }

    if (selected_target >= installer_target_count())
    {
        selected_target = 0U;
    }

    copy_text(status_text, sizeof(status_text), "Select a disk, then verify or reinstall it");
}

void recovery_render(const ui_rect_t *content)
{
    if (content == NULL)
    {
        return;
    }

    ui_palette_t palette = recovery_palette();

    ui_draw_text("Recovery tools", content->x + 16, content->y + 14, palette.text);
    ui_draw_text(
        "Verify storage, mount volumes, reset desktop settings, or reinstall LatterOS.",
        content->x + 16,
        content->y + 32,
        palette.disabled
    );

    ui_rect_t list = target_list_bounds(content);
    ui_control_draw_panel(&list, &palette, false);

    uint32_t count = installer_target_count();

    for (uint32_t row = 0; row < count && row < 2U; row++)
    {
        installer_target_info_t target;

        if (!installer_target_get(row, &target))
        {
            continue;
        }

        char label[112];
        target_label(&target, label, sizeof(label));
        ui_rect_t item = {
            .x = list.x + 4,
            .y = list.y + 4 + (int32_t)row * 31,
            .width = list.width - 8U,
            .height = 27U
        };

        ui_control_draw_list_row(
            &item,
            row == selected_target ? ">" : "",
            label,
            &palette,
            row == selected_target,
            false
        );
    }

    static const char *labels[8] = {
        "Refresh disks",
        "Verify selected disk",
        "Sync all disks",
        "Reset desktop settings",
        "Mount USB FAT",
        "Mount SATA FAT",
        "Mount NVMe FAT",
        "Reinstall selected disk"
    };

    for (uint32_t row = 0; row < 4U; row++)
    {
        for (uint32_t column = 0; column < 2U; column++)
        {
            uint32_t index = row * 2U + column;
            ui_rect_t button = action_button(content, column, row);
            ui_control_draw_button(
                &button,
                labels[index],
                &palette,
                index == 7U ? UI_CONTROL_FOCUSED : UI_CONTROL_NORMAL
            );
        }
    }

    ui_rect_t status = make_rect(
        content,
        16,
        (int32_t)content->height - 42,
        content->width - 32U,
        28U
    );

    ui_control_draw_statusbar(&status, status_text, "Milestone 19C", &palette);
}

static bool handle_target_click(
    const ui_rect_t *content,
    int32_t x,
    int32_t y
)
{
    ui_rect_t list = target_list_bounds(content);

    if (!ui_point_in_rect(x, y, &list))
    {
        return false;
    }

    int32_t relative = y - list.y - 4;

    if (relative < 0)
    {
        return true;
    }

    uint32_t row = (uint32_t)relative / 31U;

    if (row < installer_target_count())
    {
        selected_target = row;
        copy_text(status_text, sizeof(status_text), "Recovery target selected");
    }

    return true;
}

bool recovery_handle_click(
    const ui_rect_t *content,
    int32_t x,
    int32_t y
)
{
    if (content == NULL)
    {
        return false;
    }

    if (handle_target_click(content, x, y))
    {
        return true;
    }

    for (uint32_t row = 0; row < 4U; row++)
    {
        for (uint32_t column = 0; column < 2U; column++)
        {
            ui_rect_t button = action_button(content, column, row);

            if (!ui_point_in_rect(x, y, &button))
            {
                continue;
            }

            uint32_t action = row * 2U + column;

            switch (action)
            {
                case 0:
                    recovery_refresh();
                    break;

                case 1:
                    (void)verify_selected_disk();
                    break;

                case 2:
                    copy_text(
                        status_text,
                        sizeof(status_text),
                        block_device_sync_all() ?
                            "All online block devices synchronized" :
                            "One or more block devices failed to synchronize"
                    );
                    break;

                case 3:
                    copy_text(
                        status_text,
                        sizeof(status_text),
                        desktop_services_reset_configuration() ?
                            "Desktop settings reset to LatterOS defaults" :
                            "Unable to save reset desktop settings"
                    );
                    break;

                case 4:
                    copy_text(
                        status_text,
                        sizeof(status_text),
                        fat_fs_mount_first_usb() ?
                            "USB FAT volume mounted at /media/usb" :
                            "Unable to mount a USB FAT volume"
                    );
                    break;

                case 5:
                    copy_text(
                        status_text,
                        sizeof(status_text),
                        fat_fs_mount_first_sata() ?
                            "SATA FAT volume mounted" :
                            "Unable to mount a SATA FAT volume"
                    );
                    break;

                case 6:
                    copy_text(
                        status_text,
                        sizeof(status_text),
                        fat_fs_mount_first_nvme() ?
                            "NVMe FAT volume mounted" :
                            "Unable to mount an NVMe FAT volume"
                    );
                    break;

                case 7:
                    if (selected_target == UINT32_MAX)
                    {
                        copy_text(status_text, sizeof(status_text), "Select a reinstall target first");
                    }
                    else
                    {
                        reinstall_requested = true;
                        copy_text(status_text, sizeof(status_text), "Reinstall confirmation requested");
                    }
                    break;

                default:
                    break;
            }

            return true;
        }
    }

    return false;
}

bool recovery_take_reinstall_request(uint32_t *target_index)
{
    if (!reinstall_requested)
    {
        return false;
    }

    reinstall_requested = false;

    if (target_index != NULL)
    {
        *target_index = selected_target;
    }

    return true;
}

const char *recovery_status(void)
{
    return status_text;
}

uint32_t recovery_selected_target(void)
{
    return selected_target;
}
