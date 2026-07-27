#include "fat_fs.h"

#include "block_device.h"
#include "heap.h"
#include "kstdio.h"
#include "unicode.h"
#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FAT_MAX_NODES 1024U
#define FAT_MAX_DEPTH 24U
#define FAT_DIRECTORY_ENTRY_SIZE 32U
#define FAT_ATTRIBUTE_DIRECTORY 0x10U
#define FAT_ATTRIBUTE_VOLUME_ID 0x08U
#define FAT_ATTRIBUTE_ARCHIVE 0x20U
#define FAT_ATTRIBUTE_LONG_NAME 0x0FU
#define FAT16_END_OF_CHAIN 0xFFF8U
#define FAT32_END_OF_CHAIN 0x0FFFFFF8U
#define FAT_LFN_LAST_ENTRY 0x40U
#define FAT_LFN_ORDER_MASK 0x1FU
#define FAT_LFN_CHARS_PER_ENTRY 13U
#define FAT_MAX_LFN_UNITS 255U
#define FAT_INVALID_SLOT UINT32_MAX
#define FAT_MAX_DIRECTORY_SLOTS 65536U
#define FAT_TEST_BUFFER_SIZE 1536U
#define FAT_MOUNT_PATH_MAX 256U
#define FAT_TEST_PATH_MAX 512U
#define FAT_FSINFO_LEAD_SIGNATURE 0x41615252U
#define FAT_FSINFO_STRUCT_SIGNATURE 0x61417272U
#define FAT_FSINFO_TRAIL_SIGNATURE 0xAA550000U
#define FAT_UNKNOWN_COUNT 0xFFFFFFFFU

typedef enum
{
    FAT_KIND_NONE,
    FAT_KIND_16,
    FAT_KIND_32
} fat_kind_t;

typedef struct fat_filesystem fat_filesystem_t;

typedef struct
{
    fat_filesystem_t *filesystem;
    uint32_t first_cluster;
    uint32_t parent_first_cluster;
    uint32_t short_slot;
    uint32_t lfn_start_slot;
    uint16_t lfn_count;
    bool parent_fixed_root;
} fat_node_data_t;

typedef struct
{
    uint16_t units[FAT_MAX_LFN_UNITS + 1U];
    uint16_t unit_count;
    uint16_t entry_count;
    uint32_t start_slot;
    uint8_t checksum;
    uint8_t expected_order;
    bool active;
    bool valid;
} fat_lfn_state_t;

struct fat_filesystem
{
    const block_device_t *device;
    fat_kind_t kind;

    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint16_t root_entry_count;
    uint32_t total_sectors;
    uint32_t fat_size_sectors;
    uint32_t root_directory_sectors;
    uint32_t first_fat_sector;
    uint32_t first_root_sector;
    uint32_t first_data_sector;
    uint32_t cluster_count;
    uint32_t root_cluster;

    uint16_t fsinfo_sector;
    uint16_t backup_boot_sector;
    uint32_t free_clusters;
    uint32_t next_free_cluster;
    bool fsinfo_valid;

    char volume_label[12];
    char mount_path[FAT_MOUNT_PATH_MAX];

    uint32_t files;
    uint32_t directories;
    uint32_t nodes;
    uint32_t long_names;
    uint32_t unicode_names;
    uint32_t write_operations;
    uint32_t create_operations;
    uint32_t remove_operations;
    uint32_t move_operations;
    uint32_t allocated_clusters;
    uint32_t freed_clusters;
    uint32_t sync_operations;
    uint32_t fsinfo_updates;

    bool writable;
    bool dirty;
    vfs_node_t *root;
};

static fat_filesystem_t *mounted_filesystem;
static const char *last_mount_error = "none";
static const char *last_write_test_error = "not run";

static size_t fat_read(
    vfs_node_t *node,
    size_t offset,
    void *buffer,
    size_t count
);

static size_t fat_write(
    vfs_node_t *node,
    size_t offset,
    const void *buffer,
    size_t count
);

static bool fat_truncate(vfs_node_t *node);

static vfs_node_t *fat_create(
    vfs_node_t *parent,
    const char *name,
    vfs_node_type_t type
);

static bool fat_remove(vfs_node_t *node);

static bool fat_move(
    vfs_node_t *node,
    vfs_node_t *new_parent,
    const char *new_name
);

static bool fat_metadata(vfs_node_t *node);

static const vfs_operations_t fat_operations = {
    .read = fat_read,
    .write = fat_write,
    .truncate = fat_truncate,
    .create = fat_create,
    .remove = fat_remove,
    .move = fat_move,
    .metadata = fat_metadata
};

static uint16_t read_u16(const uint8_t *bytes)
{
    return (uint16_t)(
        (uint16_t)bytes[0] |
        ((uint16_t)bytes[1] << 8)
    );
}

static uint32_t read_u32(const uint8_t *bytes)
{
    return
        (uint32_t)bytes[0] |
        ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) |
        ((uint32_t)bytes[3] << 24);
}

static void write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void write_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static void clear_bytes(void *pointer, size_t count)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0; index < count; index++)
    {
        bytes[index] = 0;
    }
}

static void copy_bytes(
    void *destination,
    const void *source,
    size_t count
)
{
    uint8_t *output = destination;
    const uint8_t *input = source;

    for (size_t index = 0; index < count; index++)
    {
        output[index] = input[index];
    }
}

static size_t string_length(const char *text)
{
    size_t length = 0;

    if (text == NULL)
    {
        return 0;
    }

    while (text[length] != '\0')
    {
        length++;
    }

    return length;
}

static bool strings_equal(
    const char *first,
    const char *second
)
{
    if (first == NULL || second == NULL)
    {
        return false;
    }

    size_t index = 0;

    while (
        first[index] != '\0' &&
        second[index] != '\0'
    )
    {
        if (first[index] != second[index])
        {
            return false;
        }

        index++;
    }

    return first[index] == second[index];
}

static char upper_ascii(char character)
{
    if (character >= 'a' && character <= 'z')
    {
        return (char)(character - ('a' - 'A'));
    }

    return character;
}

static bool copy_text(
    char *destination,
    size_t capacity,
    const char *source
)
{
    if (
        destination == NULL ||
        source == NULL ||
        capacity == 0
    )
    {
        return false;
    }

    size_t length = string_length(source);

    if (length >= capacity)
    {
        return false;
    }

    copy_bytes(destination, source, length + 1U);
    return true;
}

static bool device_name_starts_with(
    const block_device_t *device,
    const char *prefix
)
{
    if (
        device == NULL ||
        device->name == NULL ||
        prefix == NULL
    )
    {
        return false;
    }

    size_t index = 0;

    while (prefix[index] != '\0')
    {
        if (device->name[index] != prefix[index])
        {
            return false;
        }

        index++;
    }

    return true;
}

static bool read_sector(
    const fat_filesystem_t *filesystem,
    uint32_t sector,
    void *buffer
)
{
    return
        filesystem != NULL &&
        buffer != NULL &&
        sector < filesystem->total_sectors &&
        block_device_read(
            filesystem->device,
            sector,
            1,
            buffer
        );
}

static bool write_sector(
    fat_filesystem_t *filesystem,
    uint32_t sector,
    const void *buffer
)
{
    if (
        filesystem == NULL ||
        !filesystem->writable ||
        buffer == NULL ||
        sector >= filesystem->total_sectors ||
        !block_device_write(
            filesystem->device,
            sector,
            1,
            buffer
        )
    )
    {
        return false;
    }

    filesystem->dirty = true;
    return true;
}

static uint32_t cluster_size_bytes(
    const fat_filesystem_t *filesystem
)
{
    return
        (uint32_t)filesystem->bytes_per_sector *
        filesystem->sectors_per_cluster;
}

static uint32_t cluster_to_sector(
    const fat_filesystem_t *filesystem,
    uint32_t cluster
)
{
    return
        filesystem->first_data_sector +
        (cluster - 2U) *
            filesystem->sectors_per_cluster;
}

static bool cluster_valid(
    const fat_filesystem_t *filesystem,
    uint32_t cluster
)
{
    return
        filesystem != NULL &&
        cluster >= 2U &&
        cluster < filesystem->cluster_count + 2U;
}

static bool cluster_is_end(
    const fat_filesystem_t *filesystem,
    uint32_t value
)
{
    if (filesystem->kind == FAT_KIND_16)
    {
        return value >= FAT16_END_OF_CHAIN;
    }

    return value >= FAT32_END_OF_CHAIN;
}

static uint32_t end_of_chain_value(
    const fat_filesystem_t *filesystem
)
{
    return
        filesystem->kind == FAT_KIND_16 ?
            0xFFFFU :
            0x0FFFFFFFU;
}

static uint32_t fat_entry(
    const fat_filesystem_t *filesystem,
    uint32_t cluster
)
{
    if (!cluster_valid(filesystem, cluster))
    {
        return 0;
    }

    uint32_t entry_size =
        filesystem->kind == FAT_KIND_16 ? 2U : 4U;

    uint32_t fat_offset = cluster * entry_size;
    uint32_t sector =
        filesystem->first_fat_sector +
        fat_offset / filesystem->bytes_per_sector;

    uint32_t offset =
        fat_offset % filesystem->bytes_per_sector;

    uint8_t *buffer =
        kmalloc(filesystem->bytes_per_sector);

    if (buffer == NULL)
    {
        return 0;
    }

    if (!read_sector(filesystem, sector, buffer))
    {
        kfree(buffer);
        return 0;
    }

    uint32_t value =
        filesystem->kind == FAT_KIND_16 ?
            read_u16(buffer + offset) :
            (read_u32(buffer + offset) & 0x0FFFFFFFU);

    kfree(buffer);
    return value;
}

static bool set_fat_entry(
    fat_filesystem_t *filesystem,
    uint32_t cluster,
    uint32_t value
)
{
    if (
        filesystem == NULL ||
        !filesystem->writable ||
        !cluster_valid(filesystem, cluster)
    )
    {
        return false;
    }

    uint32_t entry_size =
        filesystem->kind == FAT_KIND_16 ? 2U : 4U;

    uint32_t fat_offset = cluster * entry_size;
    uint32_t relative_sector =
        fat_offset / filesystem->bytes_per_sector;

    uint32_t offset =
        fat_offset % filesystem->bytes_per_sector;

    uint8_t *buffer =
        kmalloc(filesystem->bytes_per_sector);

    if (buffer == NULL)
    {
        return false;
    }

    bool success = true;

    for (
        uint32_t fat_index = 0;
        fat_index < filesystem->fat_count;
        fat_index++
    )
    {
        uint32_t sector =
            filesystem->first_fat_sector +
            fat_index *
                filesystem->fat_size_sectors +
            relative_sector;

        if (!read_sector(filesystem, sector, buffer))
        {
            success = false;
            break;
        }

        if (filesystem->kind == FAT_KIND_16)
        {
            write_u16(
                buffer + offset,
                (uint16_t)value
            );
        }
        else
        {
            uint32_t existing =
                read_u32(buffer + offset);

            write_u32(
                buffer + offset,
                (existing & 0xF0000000U) |
                    (value & 0x0FFFFFFFU)
            );
        }

        if (!write_sector(filesystem, sector, buffer))
        {
            success = false;
            break;
        }
    }

    kfree(buffer);
    return success;
}

static bool write_fsinfo(
    fat_filesystem_t *filesystem
)
{
    if (
        filesystem == NULL ||
        filesystem->kind != FAT_KIND_32 ||
        !filesystem->fsinfo_valid ||
        filesystem->fsinfo_sector == 0 ||
        filesystem->fsinfo_sector >=
            filesystem->reserved_sectors
    )
    {
        return true;
    }

    uint8_t *buffer =
        kmalloc(filesystem->bytes_per_sector);

    if (buffer == NULL)
    {
        return false;
    }

    if (
        !read_sector(
            filesystem,
            filesystem->fsinfo_sector,
            buffer
        )
    )
    {
        kfree(buffer);
        return false;
    }

    write_u32(
        buffer + 488,
        filesystem->free_clusters
    );

    write_u32(
        buffer + 492,
        filesystem->next_free_cluster
    );

    bool success =
        write_sector(
            filesystem,
            filesystem->fsinfo_sector,
            buffer
        );

    uint32_t backup_fsinfo =
        (uint32_t)filesystem->backup_boot_sector +
        filesystem->fsinfo_sector;

    if (
        success &&
        filesystem->backup_boot_sector != 0 &&
        backup_fsinfo <
            filesystem->reserved_sectors
    )
    {
        success =
            write_sector(
                filesystem,
                backup_fsinfo,
                buffer
            );
    }

    kfree(buffer);

    if (success)
    {
        filesystem->fsinfo_updates++;
    }

    return success;
}

static bool zero_cluster(
    fat_filesystem_t *filesystem,
    uint32_t cluster
)
{
    if (!cluster_valid(filesystem, cluster))
    {
        return false;
    }

    uint8_t *buffer =
        kmalloc(filesystem->bytes_per_sector);

    if (buffer == NULL)
    {
        return false;
    }

    clear_bytes(buffer, filesystem->bytes_per_sector);

    uint32_t first_sector =
        cluster_to_sector(filesystem, cluster);

    bool success = true;

    for (
        uint32_t index = 0;
        index < filesystem->sectors_per_cluster;
        index++
    )
    {
        if (
            !write_sector(
                filesystem,
                first_sector + index,
                buffer
            )
        )
        {
            success = false;
            break;
        }
    }

    kfree(buffer);
    return success;
}

static uint32_t allocate_cluster(
    fat_filesystem_t *filesystem
)
{
    if (
        filesystem == NULL ||
        !filesystem->writable
    )
    {
        return 0;
    }

    uint32_t first =
        cluster_valid(
            filesystem,
            filesystem->next_free_cluster
        ) ?
            filesystem->next_free_cluster :
            2U;

    uint32_t cluster = first;

    do
    {
        if (fat_entry(filesystem, cluster) == 0)
        {
            if (
                !set_fat_entry(
                    filesystem,
                    cluster,
                    end_of_chain_value(filesystem)
                ) ||
                !zero_cluster(filesystem, cluster)
            )
            {
                (void)set_fat_entry(
                    filesystem,
                    cluster,
                    0
                );
                return 0;
            }

            filesystem->allocated_clusters++;

            if (
                filesystem->free_clusters !=
                    FAT_UNKNOWN_COUNT &&
                filesystem->free_clusters > 0
            )
            {
                filesystem->free_clusters--;
            }

            uint32_t next = cluster + 1U;

            if (!cluster_valid(filesystem, next))
            {
                next = 2U;
            }

            filesystem->next_free_cluster = next;
            (void)write_fsinfo(filesystem);
            return cluster;
        }

        cluster++;

        if (!cluster_valid(filesystem, cluster))
        {
            cluster = 2U;
        }
    }
    while (cluster != first);

    return 0;
}

static bool free_cluster_chain(
    fat_filesystem_t *filesystem,
    uint32_t first_cluster
)
{
    if (
        first_cluster == 0 ||
        !cluster_valid(filesystem, first_cluster)
    )
    {
        return true;
    }

    uint32_t cluster = first_cluster;
    uint32_t guard = 0;
    bool success = true;

    while (
        cluster_valid(filesystem, cluster) &&
        guard++ < filesystem->cluster_count
    )
    {
        uint32_t next = fat_entry(filesystem, cluster);

        if (!set_fat_entry(filesystem, cluster, 0))
        {
            success = false;
            break;
        }

        filesystem->freed_clusters++;

        if (
            filesystem->free_clusters !=
                FAT_UNKNOWN_COUNT
        )
        {
            filesystem->free_clusters++;
        }

        if (
            filesystem->next_free_cluster ==
                FAT_UNKNOWN_COUNT ||
            cluster < filesystem->next_free_cluster
        )
        {
            filesystem->next_free_cluster = cluster;
        }

        if (
            next == 0 ||
            cluster_is_end(filesystem, next)
        )
        {
            break;
        }

        cluster = next;
    }

    if (success)
    {
        (void)write_fsinfo(filesystem);
    }

    return success;
}

static bool append_cluster(
    fat_filesystem_t *filesystem,
    uint32_t cluster,
    uint32_t *new_cluster
)
{
    if (
        filesystem == NULL ||
        new_cluster == NULL ||
        !cluster_valid(filesystem, cluster)
    )
    {
        return false;
    }

    uint32_t allocated =
        allocate_cluster(filesystem);

    if (allocated == 0)
    {
        return false;
    }

    if (!set_fat_entry(filesystem, cluster, allocated))
    {
        (void)free_cluster_chain(
            filesystem,
            allocated
        );
        return false;
    }

    *new_cluster = allocated;
    return true;
}

static bool chain_cluster_at(
    fat_filesystem_t *filesystem,
    uint32_t first_cluster,
    uint32_t index,
    bool create,
    uint32_t *result
)
{
    if (
        filesystem == NULL ||
        result == NULL ||
        !cluster_valid(filesystem, first_cluster)
    )
    {
        return false;
    }

    uint32_t cluster = first_cluster;

    for (uint32_t position = 0; position < index; position++)
    {
        uint32_t next = fat_entry(filesystem, cluster);

        if (
            next == 0 ||
            cluster_is_end(filesystem, next)
        )
        {
            if (
                !create ||
                !append_cluster(
                    filesystem,
                    cluster,
                    &next
                )
            )
            {
                return false;
            }
        }

        if (!cluster_valid(filesystem, next))
        {
            return false;
        }

        cluster = next;
    }

    *result = cluster;
    return true;
}

static bool directory_slot_location(
    fat_filesystem_t *filesystem,
    uint32_t first_cluster,
    bool fixed_root,
    uint32_t slot,
    bool create,
    uint32_t *sector,
    uint32_t *offset
)
{
    if (
        filesystem == NULL ||
        sector == NULL ||
        offset == NULL
    )
    {
        return false;
    }

    uint64_t byte_offset =
        (uint64_t)slot *
        FAT_DIRECTORY_ENTRY_SIZE;

    if (fixed_root)
    {
        if (slot >= filesystem->root_entry_count)
        {
            return false;
        }

        *sector =
            filesystem->first_root_sector +
            (uint32_t)(
                byte_offset /
                filesystem->bytes_per_sector
            );

        *offset =
            (uint32_t)(
                byte_offset %
                filesystem->bytes_per_sector
            );

        return true;
    }

    uint32_t cluster_bytes =
        cluster_size_bytes(filesystem);

    uint32_t chain_index =
        (uint32_t)(byte_offset / cluster_bytes);

    uint32_t within_cluster =
        (uint32_t)(byte_offset % cluster_bytes);

    uint32_t cluster;

    if (
        !chain_cluster_at(
            filesystem,
            first_cluster,
            chain_index,
            create,
            &cluster
        )
    )
    {
        return false;
    }

    *sector =
        cluster_to_sector(filesystem, cluster) +
        within_cluster /
            filesystem->bytes_per_sector;

    *offset =
        within_cluster %
        filesystem->bytes_per_sector;

    return true;
}

static bool read_directory_slot(
    fat_filesystem_t *filesystem,
    uint32_t first_cluster,
    bool fixed_root,
    uint32_t slot,
    uint8_t entry[FAT_DIRECTORY_ENTRY_SIZE]
)
{
    uint32_t sector;
    uint32_t offset;

    if (
        entry == NULL ||
        !directory_slot_location(
            filesystem,
            first_cluster,
            fixed_root,
            slot,
            false,
            &sector,
            &offset
        )
    )
    {
        return false;
    }

    uint8_t *buffer =
        kmalloc(filesystem->bytes_per_sector);

    if (buffer == NULL)
    {
        return false;
    }

    bool success =
        read_sector(filesystem, sector, buffer);

    if (success)
    {
        copy_bytes(
            entry,
            buffer + offset,
            FAT_DIRECTORY_ENTRY_SIZE
        );
    }

    kfree(buffer);
    return success;
}

static bool write_directory_slot(
    fat_filesystem_t *filesystem,
    uint32_t first_cluster,
    bool fixed_root,
    uint32_t slot,
    bool create,
    const uint8_t entry[FAT_DIRECTORY_ENTRY_SIZE]
)
{
    uint32_t sector;
    uint32_t offset;

    if (
        entry == NULL ||
        !directory_slot_location(
            filesystem,
            first_cluster,
            fixed_root,
            slot,
            create,
            &sector,
            &offset
        )
    )
    {
        return false;
    }

    uint8_t *buffer =
        kmalloc(filesystem->bytes_per_sector);

    if (buffer == NULL)
    {
        return false;
    }

    bool success =
        read_sector(filesystem, sector, buffer);

    if (success)
    {
        copy_bytes(
            buffer + offset,
            entry,
            FAT_DIRECTORY_ENTRY_SIZE
        );

        success =
            write_sector(
                filesystem,
                sector,
                buffer
            );
    }

    kfree(buffer);
    return success;
}

static uint32_t entry_first_cluster(
    const uint8_t entry[FAT_DIRECTORY_ENTRY_SIZE]
)
{
    return
        ((uint32_t)read_u16(entry + 20) << 16) |
        read_u16(entry + 26);
}

static void set_entry_first_cluster(
    uint8_t entry[FAT_DIRECTORY_ENTRY_SIZE],
    uint32_t cluster
)
{
    write_u16(
        entry + 20,
        (uint16_t)(cluster >> 16)
    );

    write_u16(
        entry + 26,
        (uint16_t)cluster
    );
}

static void short_name_to_text(
    const uint8_t entry[FAT_DIRECTORY_ENTRY_SIZE],
    char output[13]
)
{
    size_t position = 0;

    for (size_t index = 0; index < 8; index++)
    {
        if (entry[index] == ' ')
        {
            break;
        }

        uint8_t value = entry[index];

        if (index == 0 && value == 0x05U)
        {
            value = 0xE5U;
        }

        output[position++] = (char)value;
    }

    bool has_extension = false;

    for (size_t index = 8; index < 11; index++)
    {
        if (entry[index] != ' ')
        {
            has_extension = true;
            break;
        }
    }

    if (has_extension)
    {
        output[position++] = '.';

        for (size_t index = 8; index < 11; index++)
        {
            if (entry[index] == ' ')
            {
                break;
            }

            output[position++] = (char)entry[index];
        }
    }

    output[position] = '\0';
}

static uint8_t short_name_checksum(
    const uint8_t alias[11]
)
{
    uint8_t checksum = 0;

    for (size_t index = 0; index < 11; index++)
    {
        checksum =
            (uint8_t)(
                ((checksum & 1U) << 7) |
                (checksum >> 1)
            );

        checksum =
            (uint8_t)(checksum + alias[index]);
    }

    return checksum;
}

static void lfn_reset(fat_lfn_state_t *state)
{
    if (state == NULL)
    {
        return;
    }

    clear_bytes(state, sizeof(*state));
    state->start_slot = FAT_INVALID_SLOT;
}

static void lfn_read_units(
    const uint8_t entry[FAT_DIRECTORY_ENTRY_SIZE],
    uint16_t output[FAT_LFN_CHARS_PER_ENTRY]
)
{
    static const uint8_t offsets[
        FAT_LFN_CHARS_PER_ENTRY
    ] = {
        1, 3, 5, 7, 9,
        14, 16, 18, 20, 22, 24,
        28, 30
    };

    for (
        size_t index = 0;
        index < FAT_LFN_CHARS_PER_ENTRY;
        index++
    )
    {
        output[index] =
            read_u16(entry + offsets[index]);
    }
}

static bool lfn_accept(
    fat_lfn_state_t *state,
    const uint8_t entry[FAT_DIRECTORY_ENTRY_SIZE],
    uint32_t slot
)
{
    uint8_t raw_order = entry[0];
    uint8_t order =
        raw_order & FAT_LFN_ORDER_MASK;

    if (
        order == 0 ||
        order > 20 ||
        entry[12] != 0 ||
        read_u16(entry + 26) != 0
    )
    {
        lfn_reset(state);
        return false;
    }

    if (raw_order & FAT_LFN_LAST_ENTRY)
    {
        lfn_reset(state);
        state->active = true;
        state->valid = true;
        state->expected_order = order;
        state->entry_count = order;
        state->start_slot = slot;
        state->checksum = entry[13];
    }

    if (
        !state->active ||
        !state->valid ||
        order != state->expected_order ||
        entry[13] != state->checksum
    )
    {
        lfn_reset(state);
        return false;
    }

    uint16_t units[FAT_LFN_CHARS_PER_ENTRY];
    lfn_read_units(entry, units);

    uint32_t base =
        (uint32_t)(order - 1U) *
        FAT_LFN_CHARS_PER_ENTRY;

    for (
        uint32_t index = 0;
        index < FAT_LFN_CHARS_PER_ENTRY;
        index++
    )
    {
        uint32_t target = base + index;

        if (target > FAT_MAX_LFN_UNITS)
        {
            state->valid = false;
            break;
        }

        state->units[target] = units[index];

        if (
            units[index] != 0 &&
            units[index] != 0xFFFFU &&
            target + 1U > state->unit_count
        )
        {
            state->unit_count =
                (uint16_t)(target + 1U);
        }
    }

    if (state->expected_order > 0)
    {
        state->expected_order--;
    }

    return state->valid;
}

static bool lfn_complete_name(
    fat_lfn_state_t *state,
    const uint8_t short_entry[
        FAT_DIRECTORY_ENTRY_SIZE
    ],
    char output[VFS_NAME_MAX + 1]
)
{
    if (
        state == NULL ||
        output == NULL ||
        !state->active ||
        !state->valid ||
        state->expected_order != 0 ||
        state->checksum !=
            short_name_checksum(short_entry)
    )
    {
        return false;
    }

    size_t output_bytes;

    return unicode_utf16_to_utf8(
        state->units,
        state->unit_count,
        output,
        VFS_NAME_MAX + 1U,
        &output_bytes
    ) && output_bytes > 0;
}

static bool alias_char_valid(char character)
{
    return
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9') ||
        character == '$' ||
        character == '%' ||
        character == '\'' ||
        character == '-' ||
        character == '_' ||
        character == '@' ||
        character == '~' ||
        character == '`' ||
        character == '!' ||
        character == '(' ||
        character == ')' ||
        character == '{' ||
        character == '}' ||
        character == '^' ||
        character == '#' ||
        character == '&';
}

static bool exact_short_name(
    const char *name,
    uint8_t alias[11]
)
{
    size_t length = string_length(name);

    if (
        length == 0 ||
        length > 12 ||
        !unicode_utf8_validate(name)
    )
    {
        return false;
    }

    size_t dot = length;
    size_t dot_count = 0;

    for (size_t index = 0; index < length; index++)
    {
        uint8_t value = (uint8_t)name[index];

        if (value >= 0x80U)
        {
            return false;
        }

        if (name[index] == '.')
        {
            dot = index;
            dot_count++;
        }
    }

    if (
        dot_count > 1 ||
        dot == 0 ||
        dot > 8 ||
        (dot < length &&
         length - dot - 1U > 3U)
    )
    {
        return false;
    }

    clear_bytes(alias, 11);
    for (size_t index = 0; index < 11; index++)
    {
        alias[index] = ' ';
    }

    for (size_t index = 0; index < dot; index++)
    {
        char character = upper_ascii(name[index]);

        if (
            !alias_char_valid(character) ||
            name[index] != character
        )
        {
            return false;
        }

        alias[index] = (uint8_t)character;
    }

    if (dot < length)
    {
        for (
            size_t index = dot + 1U;
            index < length;
            index++
        )
        {
            char character =
                upper_ascii(name[index]);

            if (
                !alias_char_valid(character) ||
                name[index] != character
            )
            {
                return false;
            }

            alias[
                8U + index - dot - 1U
            ] = (uint8_t)character;
        }
    }

    return true;
}

static void sanitize_alias_part(
    const char *text,
    size_t start,
    size_t end,
    char *output,
    size_t capacity,
    size_t *written
)
{
    size_t position = 0;

    for (
        size_t index = start;
        index < end &&
        position < capacity;
        index++
    )
    {
        uint8_t value = (uint8_t)text[index];

        if (value >= 0x80U)
        {
            if (
                position == 0 ||
                output[position - 1U] != '_'
            )
            {
                output[position++] = '_';
            }

            while (
                index + 1U < end &&
                ((uint8_t)text[index + 1U] &
                    0xC0U) == 0x80U
            )
            {
                index++;
            }

            continue;
        }

        char character =
            upper_ascii((char)value);

        if (alias_char_valid(character))
        {
            output[position++] = character;
        }
        else if (
            character != ' ' &&
            character != '.'
        )
        {
            output[position++] = '_';
        }
    }

    if (position == 0 && capacity > 0)
    {
        output[position++] = '_';
    }

    *written = position;
}

static bool short_alias_exists(
    fat_filesystem_t *filesystem,
    uint32_t directory_cluster,
    bool fixed_root,
    const uint8_t alias[11]
)
{
    uint8_t entry[FAT_DIRECTORY_ENTRY_SIZE];

    for (
        uint32_t slot = 0;
        slot < FAT_MAX_DIRECTORY_SLOTS;
        slot++
    )
    {
        if (
            !read_directory_slot(
                filesystem,
                directory_cluster,
                fixed_root,
                slot,
                entry
            )
        )
        {
            return false;
        }

        if (entry[0] == 0)
        {
            return false;
        }

        if (
            entry[0] == 0xE5U ||
            entry[11] == FAT_ATTRIBUTE_LONG_NAME
        )
        {
            continue;
        }

        bool equal = true;

        for (size_t index = 0; index < 11; index++)
        {
            if (entry[index] != alias[index])
            {
                equal = false;
                break;
            }
        }

        if (equal)
        {
            return true;
        }
    }

    return false;
}

static bool generate_short_alias(
    fat_filesystem_t *filesystem,
    uint32_t directory_cluster,
    bool fixed_root,
    const char *name,
    uint8_t alias[11],
    bool *requires_lfn
)
{
    if (
        filesystem == NULL ||
        name == NULL ||
        alias == NULL ||
        requires_lfn == NULL ||
        !unicode_utf8_validate(name)
    )
    {
        return false;
    }

    if (
        exact_short_name(name, alias) &&
        !short_alias_exists(
            filesystem,
            directory_cluster,
            fixed_root,
            alias
        )
    )
    {
        *requires_lfn = false;
        return true;
    }

    *requires_lfn = true;

    size_t length = string_length(name);
    size_t dot = length;

    for (size_t index = 0; index < length; index++)
    {
        if (name[index] == '.')
        {
            dot = index;
        }
    }

    char base[32];
    char extension[8];
    size_t base_length;
    size_t extension_length = 0;

    sanitize_alias_part(
        name,
        0,
        dot,
        base,
        sizeof(base),
        &base_length
    );

    if (dot < length)
    {
        sanitize_alias_part(
            name,
            dot + 1U,
            length,
            extension,
            sizeof(extension),
            &extension_length
        );
    }

    for (uint32_t sequence = 1; sequence <= 999999U; sequence++)
    {
        for (size_t index = 0; index < 11; index++)
        {
            alias[index] = ' ';
        }

        char digits[7];
        size_t digit_count = 0;
        uint32_t value = sequence;

        do
        {
            digits[digit_count++] =
                (char)('0' + value % 10U);
            value /= 10U;
        }
        while (
            value > 0 &&
            digit_count < sizeof(digits)
        );

        size_t prefix_limit =
            8U - 1U - digit_count;

        if (prefix_limit > base_length)
        {
            prefix_limit = base_length;
        }

        for (
            size_t index = 0;
            index < prefix_limit;
            index++
        )
        {
            alias[index] =
                (uint8_t)base[index];
        }

        alias[prefix_limit] = '~';

        for (
            size_t index = 0;
            index < digit_count;
            index++
        )
        {
            alias[
                prefix_limit + 1U + index
            ] =
                (uint8_t)digits[
                    digit_count - index - 1U
                ];
        }

        for (
            size_t index = 0;
            index < extension_length &&
            index < 3U;
            index++
        )
        {
            alias[8U + index] =
                (uint8_t)extension[index];
        }

        if (
            !short_alias_exists(
                filesystem,
                directory_cluster,
                fixed_root,
                alias
            )
        )
        {
            return true;
        }
    }

    return false;
}

static bool find_free_slots(
    fat_filesystem_t *filesystem,
    uint32_t directory_cluster,
    bool fixed_root,
    uint32_t count,
    uint32_t *start_slot
)
{
    if (
        filesystem == NULL ||
        count == 0 ||
        start_slot == NULL
    )
    {
        return false;
    }

    uint32_t run_start = 0;
    uint32_t run_count = 0;
    uint8_t entry[FAT_DIRECTORY_ENTRY_SIZE];

    uint32_t limit =
        fixed_root ?
            filesystem->root_entry_count :
            FAT_MAX_DIRECTORY_SLOTS;

    for (uint32_t slot = 0; slot < limit; slot++)
    {
        bool readable =
            read_directory_slot(
                filesystem,
                directory_cluster,
                fixed_root,
                slot,
                entry
            );

        bool free_slot = false;

        if (!readable)
        {
            if (fixed_root)
            {
                return false;
            }

            free_slot = true;
        }
        else
        {
            free_slot =
                entry[0] == 0 ||
                entry[0] == 0xE5U;
        }

        if (free_slot)
        {
            if (run_count == 0)
            {
                run_start = slot;
            }

            run_count++;

            if (run_count == count)
            {
                uint32_t sector;
                uint32_t offset;

                if (
                    !directory_slot_location(
                        filesystem,
                        directory_cluster,
                        fixed_root,
                        slot,
                        true,
                        &sector,
                        &offset
                    )
                )
                {
                    return false;
                }

                *start_slot = run_start;
                return true;
            }
        }
        else
        {
            run_count = 0;
        }
    }

    return false;
}

static void write_lfn_units(
    uint8_t entry[FAT_DIRECTORY_ENTRY_SIZE],
    const uint16_t units[
        FAT_LFN_CHARS_PER_ENTRY
    ]
)
{
    static const uint8_t offsets[
        FAT_LFN_CHARS_PER_ENTRY
    ] = {
        1, 3, 5, 7, 9,
        14, 16, 18, 20, 22, 24,
        28, 30
    };

    for (
        size_t index = 0;
        index < FAT_LFN_CHARS_PER_ENTRY;
        index++
    )
    {
        write_u16(
            entry + offsets[index],
            units[index]
        );
    }
}

static bool write_named_directory_entry(
    fat_filesystem_t *filesystem,
    uint32_t directory_cluster,
    bool fixed_root,
    const char *name,
    uint8_t attributes,
    uint32_t first_cluster,
    uint32_t size,
    uint32_t *lfn_start_slot,
    uint16_t *lfn_count,
    uint32_t *short_slot
)
{
    uint8_t alias[11];
    bool requires_lfn;

    if (
        !generate_short_alias(
            filesystem,
            directory_cluster,
            fixed_root,
            name,
            alias,
            &requires_lfn
        )
    )
    {
        return false;
    }

    uint16_t utf16[FAT_MAX_LFN_UNITS + 1U];
    size_t utf16_units = 0;
    uint16_t long_count = 0;

    if (requires_lfn)
    {
        if (
            !unicode_utf8_to_utf16(
                name,
                utf16,
                FAT_MAX_LFN_UNITS,
                &utf16_units
            ) ||
            utf16_units == 0 ||
            utf16_units > FAT_MAX_LFN_UNITS
        )
        {
            return false;
        }

        long_count =
            (uint16_t)(
                (utf16_units + 1U +
                 FAT_LFN_CHARS_PER_ENTRY - 1U) /
                FAT_LFN_CHARS_PER_ENTRY
            );
    }

    uint32_t total_slots =
        (uint32_t)long_count + 1U;

    uint32_t start;

    if (
        !find_free_slots(
            filesystem,
            directory_cluster,
            fixed_root,
            total_slots,
            &start
        )
    )
    {
        return false;
    }

    uint8_t checksum =
        short_name_checksum(alias);

    for (
        uint16_t entry_index = 0;
        entry_index < long_count;
        entry_index++
    )
    {
        uint16_t order =
            (uint16_t)(
                long_count - entry_index
            );

        uint8_t entry[FAT_DIRECTORY_ENTRY_SIZE];
        clear_bytes(entry, sizeof(entry));

        entry[0] =
            (uint8_t)(
                order |
                (order == long_count ?
                    FAT_LFN_LAST_ENTRY : 0U)
            );

        entry[11] = FAT_ATTRIBUTE_LONG_NAME;
        entry[12] = 0;
        entry[13] = checksum;
        write_u16(entry + 26, 0);

        uint16_t part[FAT_LFN_CHARS_PER_ENTRY];

        for (
            uint32_t unit = 0;
            unit < FAT_LFN_CHARS_PER_ENTRY;
            unit++
        )
        {
            uint32_t source =
                (uint32_t)(order - 1U) *
                    FAT_LFN_CHARS_PER_ENTRY +
                unit;

            if (source < utf16_units)
            {
                part[unit] = utf16[source];
            }
            else if (source == utf16_units)
            {
                part[unit] = 0;
            }
            else
            {
                part[unit] = 0xFFFFU;
            }
        }

        write_lfn_units(entry, part);

        if (
            !write_directory_slot(
                filesystem,
                directory_cluster,
                fixed_root,
                start + entry_index,
                true,
                entry
            )
        )
        {
            return false;
        }
    }

    uint8_t short_entry[FAT_DIRECTORY_ENTRY_SIZE];
    clear_bytes(short_entry, sizeof(short_entry));

    copy_bytes(short_entry, alias, 11);
    short_entry[11] = attributes;
    set_entry_first_cluster(
        short_entry,
        first_cluster
    );
    write_u32(short_entry + 28, size);

    uint32_t short_index =
        start + long_count;

    if (
        !write_directory_slot(
            filesystem,
            directory_cluster,
            fixed_root,
            short_index,
            true,
            short_entry
        )
    )
    {
        return false;
    }

    if (lfn_start_slot != NULL)
    {
        *lfn_start_slot =
            long_count > 0 ?
                start :
                FAT_INVALID_SLOT;
    }

    if (lfn_count != NULL)
    {
        *lfn_count = long_count;
    }

    if (short_slot != NULL)
    {
        *short_slot = short_index;
    }

    return true;
}

static bool update_short_entry(
    fat_node_data_t *data,
    uint32_t first_cluster,
    uint32_t size
)
{
    if (
        data == NULL ||
        data->filesystem == NULL ||
        data->short_slot == FAT_INVALID_SLOT
    )
    {
        return false;
    }

    uint8_t entry[FAT_DIRECTORY_ENTRY_SIZE];

    if (
        !read_directory_slot(
            data->filesystem,
            data->parent_first_cluster,
            data->parent_fixed_root,
            data->short_slot,
            entry
        ) ||
        entry[0] == 0 ||
        entry[0] == 0xE5U
    )
    {
        return false;
    }

    set_entry_first_cluster(entry, first_cluster);
    write_u32(entry + 28, size);

    return write_directory_slot(
        data->filesystem,
        data->parent_first_cluster,
        data->parent_fixed_root,
        data->short_slot,
        false,
        entry
    );
}

static bool delete_directory_entries(
    fat_node_data_t *data
)
{
    if (
        data == NULL ||
        data->filesystem == NULL
    )
    {
        return false;
    }

    uint32_t start =
        data->lfn_count > 0 ?
            data->lfn_start_slot :
            data->short_slot;

    uint32_t count =
        (uint32_t)data->lfn_count + 1U;

    for (uint32_t index = 0; index < count; index++)
    {
        uint8_t entry[FAT_DIRECTORY_ENTRY_SIZE];

        if (
            !read_directory_slot(
                data->filesystem,
                data->parent_first_cluster,
                data->parent_fixed_root,
                start + index,
                entry
            )
        )
        {
            return false;
        }

        entry[0] = 0xE5U;

        if (
            !write_directory_slot(
                data->filesystem,
                data->parent_first_cluster,
                data->parent_fixed_root,
                start + index,
                false,
                entry
            )
        )
        {
            return false;
        }
    }

    return true;
}

static vfs_node_t *allocate_node(
    fat_filesystem_t *filesystem,
    const char *name,
    vfs_node_type_t type,
    uint32_t first_cluster,
    size_t size,
    uint32_t parent_cluster,
    bool parent_fixed_root,
    uint32_t short_slot,
    uint32_t lfn_start_slot,
    uint16_t lfn_count
)
{
    if (
        filesystem == NULL ||
        name == NULL ||
        filesystem->nodes >= FAT_MAX_NODES
    )
    {
        return NULL;
    }

    vfs_node_t *node =
        kmalloc(sizeof(vfs_node_t));

    fat_node_data_t *data =
        kmalloc(sizeof(fat_node_data_t));

    if (node == NULL || data == NULL)
    {
        if (node != NULL)
        {
            kfree(node);
        }

        if (data != NULL)
        {
            kfree(data);
        }

        return NULL;
    }

    clear_bytes(node, sizeof(*node));
    clear_bytes(data, sizeof(*data));

    if (
        !copy_text(
            node->name,
            sizeof(node->name),
            name
        )
    )
    {
        kfree(data);
        kfree(node);
        return NULL;
    }

    node->type = type;
    node->size = size;
    node->operations = &fat_operations;
    node->filesystem_data = data;
    vfs_initialize_metadata(node, type);

    data->filesystem = filesystem;
    data->first_cluster = first_cluster;
    data->parent_first_cluster = parent_cluster;
    data->parent_fixed_root = parent_fixed_root;
    data->short_slot = short_slot;
    data->lfn_start_slot = lfn_start_slot;
    data->lfn_count = lfn_count;

    filesystem->nodes++;

    if (type == VFS_NODE_DIRECTORY)
    {
        filesystem->directories++;
    }
    else
    {
        filesystem->files++;
    }

    return node;
}

static void free_node_tree(vfs_node_t *node)
{
    if (node == NULL)
    {
        return;
    }

    vfs_node_t *child = node->first_child;

    while (child != NULL)
    {
        vfs_node_t *next = child->next_sibling;
        free_node_tree(child);
        child = next;
    }

    if (node->filesystem_data != NULL)
    {
        kfree(node->filesystem_data);
    }

    kfree(node);
}

static bool entry_is_dot(
    const uint8_t entry[FAT_DIRECTORY_ENTRY_SIZE]
)
{
    return
        entry[0] == '.' &&
        (
            entry[1] == ' ' ||
            entry[1] == '.'
        );
}

static bool name_has_non_ascii(const char *name)
{
    if (name == NULL)
    {
        return false;
    }

    for (size_t index = 0; name[index] != '\0'; index++)
    {
        if ((uint8_t)name[index] >= 0x80U)
        {
            return true;
        }
    }

    return false;
}

static bool parse_directory(
    fat_filesystem_t *filesystem,
    vfs_node_t *parent,
    uint32_t first_cluster,
    bool fixed_root,
    uint32_t depth
)
{
    if (
        filesystem == NULL ||
        parent == NULL ||
        depth > FAT_MAX_DEPTH
    )
    {
        return false;
    }

    fat_lfn_state_t lfn;
    lfn_reset(&lfn);

    uint8_t entry[FAT_DIRECTORY_ENTRY_SIZE];

    uint32_t limit =
        fixed_root ?
            filesystem->root_entry_count :
            FAT_MAX_DIRECTORY_SLOTS;

    for (uint32_t slot = 0; slot < limit; slot++)
    {
        if (
            !read_directory_slot(
                filesystem,
                first_cluster,
                fixed_root,
                slot,
                entry
            )
        )
        {
            if (!fixed_root)
            {
                break;
            }

            return false;
        }

        if (entry[0] == 0)
        {
            break;
        }

        if (entry[0] == 0xE5U)
        {
            lfn_reset(&lfn);
            continue;
        }

        if (entry[11] == FAT_ATTRIBUTE_LONG_NAME)
        {
            (void)lfn_accept(&lfn, entry, slot);
            continue;
        }

        if (
            entry[11] & FAT_ATTRIBUTE_VOLUME_ID ||
            entry_is_dot(entry)
        )
        {
            lfn_reset(&lfn);
            continue;
        }

        char name[VFS_NAME_MAX + 1];
        bool long_name =
            lfn_complete_name(&lfn, entry, name);

        if (!long_name)
        {
            char short_name[13];
            short_name_to_text(entry, short_name);

            if (
                !copy_text(
                    name,
                    sizeof(name),
                    short_name
                )
            )
            {
                lfn_reset(&lfn);
                continue;
            }
        }

        uint32_t entry_cluster =
            entry_first_cluster(entry);

        bool directory =
            (entry[11] & FAT_ATTRIBUTE_DIRECTORY) != 0;

        uint32_t entry_size =
            directory ? 0 : read_u32(entry + 28);

        uint32_t lfn_start =
            long_name ?
                lfn.start_slot :
                FAT_INVALID_SLOT;

        uint16_t lfn_count =
            long_name ?
                lfn.entry_count :
                0;

        vfs_node_t *node =
            allocate_node(
                filesystem,
                name,
                directory ?
                    VFS_NODE_DIRECTORY :
                    VFS_NODE_FILE,
                entry_cluster,
                entry_size,
                first_cluster,
                fixed_root,
                slot,
                lfn_start,
                lfn_count
            );

        if (node == NULL)
        {
            return false;
        }

        if (!vfs_add_child(parent, node))
        {
            free_node_tree(node);
            return false;
        }

        if (long_name)
        {
            filesystem->long_names++;

            if (name_has_non_ascii(name))
            {
                filesystem->unicode_names++;
            }
        }

        if (
            directory &&
            cluster_valid(
                filesystem,
                entry_cluster
            ) &&
            !parse_directory(
                filesystem,
                node,
                entry_cluster,
                false,
                depth + 1U
            )
        )
        {
            return false;
        }

        lfn_reset(&lfn);
    }

    return true;
}

static bool initialize_fsinfo(
    fat_filesystem_t *filesystem,
    const uint8_t *boot_sector
)
{
    filesystem->free_clusters = FAT_UNKNOWN_COUNT;
    filesystem->next_free_cluster = 2U;
    filesystem->fsinfo_valid = false;

    if (filesystem->kind != FAT_KIND_32)
    {
        return true;
    }

    filesystem->fsinfo_sector =
        read_u16(boot_sector + 48);

    filesystem->backup_boot_sector =
        read_u16(boot_sector + 50);

    if (
        filesystem->fsinfo_sector == 0 ||
        filesystem->fsinfo_sector >=
            filesystem->reserved_sectors
    )
    {
        return true;
    }

    uint8_t *buffer =
        kmalloc(filesystem->bytes_per_sector);

    if (buffer == NULL)
    {
        return false;
    }

    bool success =
        read_sector(
            filesystem,
            filesystem->fsinfo_sector,
            buffer
        );

    if (
        success &&
        read_u32(buffer) ==
            FAT_FSINFO_LEAD_SIGNATURE &&
        read_u32(buffer + 484) ==
            FAT_FSINFO_STRUCT_SIGNATURE &&
        read_u32(buffer + 508) ==
            FAT_FSINFO_TRAIL_SIGNATURE
    )
    {
        filesystem->free_clusters =
            read_u32(buffer + 488);

        filesystem->next_free_cluster =
            read_u32(buffer + 492);

        if (
            filesystem->free_clusters >
                filesystem->cluster_count
        )
        {
            filesystem->free_clusters =
                FAT_UNKNOWN_COUNT;
        }

        if (
            !cluster_valid(
                filesystem,
                filesystem->next_free_cluster
            )
        )
        {
            filesystem->next_free_cluster = 2U;
        }

        filesystem->fsinfo_valid = true;
    }

    kfree(buffer);
    return success;
}

static void extract_volume_label(
    fat_filesystem_t *filesystem,
    const uint8_t *boot_sector
)
{
    uint32_t offset =
        filesystem->kind == FAT_KIND_16 ?
            43U :
            71U;

    size_t length = 11U;

    while (
        length > 0 &&
        boot_sector[offset + length - 1U] == ' '
    )
    {
        length--;
    }

    for (size_t index = 0; index < length; index++)
    {
        filesystem->volume_label[index] =
            (char)boot_sector[offset + index];
    }

    filesystem->volume_label[length] = '\0';

    if (length == 0)
    {
        (void)copy_text(
            filesystem->volume_label,
            sizeof(filesystem->volume_label),
            "NO NAME"
        );
    }
}

static bool initialize_filesystem(
    fat_filesystem_t *filesystem,
    const block_device_t *device
)
{
    if (
        filesystem == NULL ||
        device == NULL ||
        !device->online ||
        device->sector_size < 512U ||
        device->sector_size > 4096U
    )
    {
        return false;
    }

    uint8_t *boot_sector =
        kmalloc(device->sector_size);

    if (boot_sector == NULL)
    {
        return false;
    }

    if (
        !block_device_read(
            device,
            0,
            1,
            boot_sector
        ) ||
        boot_sector[510] != 0x55U ||
        boot_sector[511] != 0xAAU
    )
    {
        kfree(boot_sector);
        return false;
    }

    uint16_t bytes_per_sector =
        read_u16(boot_sector + 11);

    uint8_t sectors_per_cluster =
        boot_sector[13];

    uint16_t reserved_sectors =
        read_u16(boot_sector + 14);

    uint8_t fat_count = boot_sector[16];

    uint16_t root_entry_count =
        read_u16(boot_sector + 17);

    uint32_t total_sectors =
        read_u16(boot_sector + 19);

    if (total_sectors == 0)
    {
        total_sectors =
            read_u32(boot_sector + 32);
    }

    uint32_t fat_size =
        read_u16(boot_sector + 22);

    if (fat_size == 0)
    {
        fat_size =
            read_u32(boot_sector + 36);
    }

    if (
        bytes_per_sector != device->sector_size ||
        sectors_per_cluster == 0 ||
        (sectors_per_cluster &
            (sectors_per_cluster - 1U)) != 0 ||
        reserved_sectors == 0 ||
        fat_count == 0 ||
        fat_size == 0 ||
        total_sectors == 0 ||
        total_sectors > device->sector_count
    )
    {
        kfree(boot_sector);
        return false;
    }

    uint32_t root_directory_sectors =
        (
            (uint32_t)root_entry_count *
                FAT_DIRECTORY_ENTRY_SIZE +
            bytes_per_sector - 1U
        ) /
        bytes_per_sector;

    uint32_t first_data_sector =
        reserved_sectors +
        (uint32_t)fat_count * fat_size +
        root_directory_sectors;

    if (first_data_sector >= total_sectors)
    {
        kfree(boot_sector);
        return false;
    }

    uint32_t data_sectors =
        total_sectors - first_data_sector;

    uint32_t cluster_count =
        data_sectors / sectors_per_cluster;

    fat_kind_t kind;

    if (cluster_count < 4085U)
    {
        kfree(boot_sector);
        return false;
    }
    else if (cluster_count < 65525U)
    {
        kind = FAT_KIND_16;
    }
    else
    {
        kind = FAT_KIND_32;
    }

    if (
        (kind == FAT_KIND_16 &&
         root_entry_count == 0) ||
        (kind == FAT_KIND_32 &&
         root_entry_count != 0)
    )
    {
        kfree(boot_sector);
        return false;
    }

    clear_bytes(filesystem, sizeof(*filesystem));

    filesystem->device = device;
    filesystem->kind = kind;
    filesystem->bytes_per_sector =
        bytes_per_sector;
    filesystem->sectors_per_cluster =
        sectors_per_cluster;
    filesystem->reserved_sectors =
        reserved_sectors;
    filesystem->fat_count = fat_count;
    filesystem->root_entry_count =
        root_entry_count;
    filesystem->total_sectors =
        total_sectors;
    filesystem->fat_size_sectors =
        fat_size;
    filesystem->root_directory_sectors =
        root_directory_sectors;
    filesystem->first_fat_sector =
        reserved_sectors;
    filesystem->first_root_sector =
        reserved_sectors +
        (uint32_t)fat_count * fat_size;
    filesystem->first_data_sector =
        first_data_sector;
    filesystem->cluster_count =
        cluster_count;
    filesystem->root_cluster =
        kind == FAT_KIND_32 ?
            (read_u32(boot_sector + 44) &
                0x0FFFFFFFU) :
            0U;

    filesystem->writable =
        device->writable &&
        device->write != NULL;

    extract_volume_label(
        filesystem,
        boot_sector
    );

    bool fsinfo_ok =
        initialize_fsinfo(
            filesystem,
            boot_sector
        );

    kfree(boot_sector);

    return
        fsinfo_ok &&
        (
            kind == FAT_KIND_16 ||
            cluster_valid(
                filesystem,
                filesystem->root_cluster
            )
        );
}

static const char *mount_leaf_name(
    const char *mount_path
)
{
    const char *leaf = mount_path;

    for (
        size_t index = 0;
        mount_path[index] != '\0';
        index++
    )
    {
        if (
            mount_path[index] == '/' &&
            mount_path[index + 1U] != '\0'
        )
        {
            leaf = &mount_path[index + 1U];
        }
    }

    return leaf;
}

static bool mount_device_at(
    const block_device_t *device,
    const char *mount_path
)
{
    if (mounted_filesystem != NULL)
    {
        last_mount_error = "a FAT filesystem is already mounted";
        return false;
    }

    if (
        device == NULL ||
        mount_path == NULL ||
        mount_path[0] != '/'
    )
    {
        last_mount_error = "invalid device or mount path";
        return false;
    }

    if (vfs_open("/media") == NULL)
    {
        if (!vfs_make_directory("/media"))
        {
            last_mount_error = "unable to create /media";
            return false;
        }
    }

    fat_filesystem_t *filesystem =
        kmalloc(sizeof(fat_filesystem_t));

    if (filesystem == NULL)
    {
        last_mount_error = "filesystem state allocation failed";
        return false;
    }

    if (!initialize_filesystem(filesystem, device))
    {
        last_mount_error = "FAT boot sector, FSInfo, or geometry rejected";
        kfree(filesystem);
        return false;
    }

    if (
        !copy_text(
            filesystem->mount_path,
            sizeof(filesystem->mount_path),
            mount_path
        )
    )
    {
        last_mount_error = "mount path exceeds FAT mount-state capacity";
        kfree(filesystem);
        return false;
    }

    bool fixed_root =
        filesystem->kind == FAT_KIND_16;

    uint32_t root_cluster =
        fixed_root ?
            0U :
            filesystem->root_cluster;

    vfs_node_t *root =
        allocate_node(
            filesystem,
            mount_leaf_name(mount_path),
            VFS_NODE_DIRECTORY,
            root_cluster,
            0,
            root_cluster,
            fixed_root,
            FAT_INVALID_SLOT,
            FAT_INVALID_SLOT,
            0
        );

    if (root == NULL)
    {
        last_mount_error = "root-node allocation failed";
        kfree(filesystem);
        return false;
    }

    filesystem->root = root;

    if (
        !parse_directory(
            filesystem,
            root,
            root_cluster,
            fixed_root,
            0
        )
    )
    {
        last_mount_error = "FAT root-directory parsing failed";
        free_node_tree(root);
        kfree(filesystem);
        return false;
    }

    if (!vfs_mount_at(mount_path, root))
    {
        last_mount_error = "VFS mount point is unavailable";
        free_node_tree(root);
        kfree(filesystem);
        return false;
    }

    mounted_filesystem = filesystem;
    last_mount_error = "none";
    return true;
}

static bool file_cluster_for_offset(
    fat_node_data_t *data,
    size_t offset,
    bool create,
    uint32_t *cluster
)
{
    if (
        data == NULL ||
        data->filesystem == NULL ||
        cluster == NULL
    )
    {
        return false;
    }

    fat_filesystem_t *filesystem =
        data->filesystem;

    uint32_t cluster_bytes =
        cluster_size_bytes(filesystem);

    uint32_t index =
        (uint32_t)(offset / cluster_bytes);

    if (data->first_cluster == 0)
    {
        if (!create)
        {
            return false;
        }

        data->first_cluster =
            allocate_cluster(filesystem);

        if (data->first_cluster == 0)
        {
            return false;
        }
    }

    return chain_cluster_at(
        filesystem,
        data->first_cluster,
        index,
        create,
        cluster
    );
}

static size_t file_transfer(
    vfs_node_t *node,
    size_t offset,
    void *buffer,
    size_t count,
    bool writing
)
{
    if (
        node == NULL ||
        buffer == NULL ||
        count == 0 ||
        node->type != VFS_NODE_FILE
    )
    {
        return 0;
    }

    fat_node_data_t *data =
        node->filesystem_data;

    if (
        data == NULL ||
        data->filesystem == NULL ||
        (writing &&
         !data->filesystem->writable)
    )
    {
        return 0;
    }

    if (!writing)
    {
        if (offset >= node->size)
        {
            return 0;
        }

        if (count > node->size - offset)
        {
            count = node->size - offset;
        }
    }

    fat_filesystem_t *filesystem =
        data->filesystem;

    uint8_t *sector_buffer =
        kmalloc(filesystem->bytes_per_sector);

    if (sector_buffer == NULL)
    {
        return 0;
    }

    uint8_t *bytes = buffer;
    size_t completed = 0;

    while (completed < count)
    {
        size_t current_offset =
            offset + completed;

        uint32_t cluster;

        if (
            !file_cluster_for_offset(
                data,
                current_offset,
                writing,
                &cluster
            )
        )
        {
            break;
        }

        uint32_t cluster_offset =
            (uint32_t)(
                current_offset %
                cluster_size_bytes(filesystem)
            );

        uint32_t sector =
            cluster_to_sector(filesystem, cluster) +
            cluster_offset /
                filesystem->bytes_per_sector;

        uint32_t sector_offset =
            cluster_offset %
            filesystem->bytes_per_sector;

        size_t available =
            filesystem->bytes_per_sector -
            sector_offset;

        size_t chunk = count - completed;

        if (chunk > available)
        {
            chunk = available;
        }

        if (
            !read_sector(
                filesystem,
                sector,
                sector_buffer
            )
        )
        {
            break;
        }

        if (writing)
        {
            copy_bytes(
                sector_buffer + sector_offset,
                bytes + completed,
                chunk
            );

            if (
                !write_sector(
                    filesystem,
                    sector,
                    sector_buffer
                )
            )
            {
                break;
            }
        }
        else
        {
            copy_bytes(
                bytes + completed,
                sector_buffer + sector_offset,
                chunk
            );
        }

        completed += chunk;
    }

    kfree(sector_buffer);
    return completed;
}

static bool zero_file_range(
    vfs_node_t *node,
    size_t offset,
    size_t count
)
{
    uint8_t zeros[256];
    clear_bytes(zeros, sizeof(zeros));

    size_t completed = 0;

    while (completed < count)
    {
        size_t chunk = count - completed;

        if (chunk > sizeof(zeros))
        {
            chunk = sizeof(zeros);
        }

        if (
            file_transfer(
                node,
                offset + completed,
                zeros,
                chunk,
                true
            ) != chunk
        )
        {
            return false;
        }

        completed += chunk;
    }

    return true;
}

static size_t fat_read(
    vfs_node_t *node,
    size_t offset,
    void *buffer,
    size_t count
)
{
    return file_transfer(
        node,
        offset,
        buffer,
        count,
        false
    );
}

static size_t fat_write(
    vfs_node_t *node,
    size_t offset,
    const void *buffer,
    size_t count
)
{
    if (
        node == NULL ||
        buffer == NULL ||
        count == 0 ||
        offset > UINT32_MAX ||
        count > UINT32_MAX - offset
    )
    {
        return 0;
    }

    fat_node_data_t *data =
        node->filesystem_data;

    if (
        data == NULL ||
        data->filesystem == NULL ||
        !data->filesystem->writable
    )
    {
        return 0;
    }

    if (
        offset > node->size &&
        !zero_file_range(
            node,
            node->size,
            offset - node->size
        )
    )
    {
        return 0;
    }

    size_t written =
        file_transfer(
            node,
            offset,
            (void *)buffer,
            count,
            true
        );

    size_t new_size = offset + written;

    if (new_size > node->size)
    {
        node->size = new_size;
    }

    if (
        written > 0 &&
        !update_short_entry(
            data,
            data->first_cluster,
            (uint32_t)node->size
        )
    )
    {
        return 0;
    }

    if (written > 0)
    {
        data->filesystem->write_operations++;
    }

    return written;
}

static bool fat_truncate(vfs_node_t *node)
{
    if (
        node == NULL ||
        node->type != VFS_NODE_FILE
    )
    {
        return false;
    }

    fat_node_data_t *data =
        node->filesystem_data;

    if (
        data == NULL ||
        data->filesystem == NULL ||
        !data->filesystem->writable
    )
    {
        return false;
    }

    if (
        !free_cluster_chain(
            data->filesystem,
            data->first_cluster
        )
    )
    {
        return false;
    }

    data->first_cluster = 0;
    node->size = 0;

    return update_short_entry(data, 0, 0);
}

static bool initialize_new_directory(
    fat_filesystem_t *filesystem,
    uint32_t cluster,
    uint32_t parent_cluster
)
{
    uint8_t entry[FAT_DIRECTORY_ENTRY_SIZE];

    clear_bytes(entry, sizeof(entry));
    entry[0] = '.';

    for (size_t index = 1; index < 11; index++)
    {
        entry[index] = ' ';
    }

    entry[11] = FAT_ATTRIBUTE_DIRECTORY;
    set_entry_first_cluster(entry, cluster);

    if (
        !write_directory_slot(
            filesystem,
            cluster,
            false,
            0,
            true,
            entry
        )
    )
    {
        return false;
    }

    clear_bytes(entry, sizeof(entry));
    entry[0] = '.';
    entry[1] = '.';

    for (size_t index = 2; index < 11; index++)
    {
        entry[index] = ' ';
    }

    entry[11] = FAT_ATTRIBUTE_DIRECTORY;
    set_entry_first_cluster(entry, parent_cluster);

    return write_directory_slot(
        filesystem,
        cluster,
        false,
        1,
        true,
        entry
    );
}

static vfs_node_t *fat_create(
    vfs_node_t *parent,
    const char *name,
    vfs_node_type_t type
)
{
    if (
        parent == NULL ||
        name == NULL ||
        name[0] == '\0' ||
        !unicode_utf8_validate(name) ||
        type > VFS_NODE_DIRECTORY
    )
    {
        return NULL;
    }

    fat_node_data_t *parent_data =
        parent->filesystem_data;

    if (
        parent_data == NULL ||
        parent_data->filesystem == NULL ||
        !parent_data->filesystem->writable
    )
    {
        return NULL;
    }

    fat_filesystem_t *filesystem =
        parent_data->filesystem;

    uint32_t first_cluster = 0;
    uint8_t attributes = FAT_ATTRIBUTE_ARCHIVE;

    if (type == VFS_NODE_DIRECTORY)
    {
        first_cluster =
            allocate_cluster(filesystem);

        if (first_cluster == 0)
        {
            return NULL;
        }

        attributes = FAT_ATTRIBUTE_DIRECTORY;

        uint32_t parent_cluster =
            parent == filesystem->root ?
                (filesystem->kind == FAT_KIND_32 ?
                    filesystem->root_cluster :
                    0U) :
                parent_data->first_cluster;

        if (
            !initialize_new_directory(
                filesystem,
                first_cluster,
                parent_cluster
            )
        )
        {
            (void)free_cluster_chain(
                filesystem,
                first_cluster
            );
            return NULL;
        }
    }

    uint32_t lfn_start;
    uint16_t lfn_count;
    uint32_t short_slot;

    bool fixed_root =
        parent == filesystem->root &&
        filesystem->kind == FAT_KIND_16;

    uint32_t directory_cluster =
        fixed_root ?
            0U :
            parent_data->first_cluster;

    if (
        !write_named_directory_entry(
            filesystem,
            directory_cluster,
            fixed_root,
            name,
            attributes,
            first_cluster,
            0,
            &lfn_start,
            &lfn_count,
            &short_slot
        )
    )
    {
        if (first_cluster != 0)
        {
            (void)free_cluster_chain(
                filesystem,
                first_cluster
            );
        }

        return NULL;
    }

    vfs_node_t *node =
        allocate_node(
            filesystem,
            name,
            type,
            first_cluster,
            0,
            directory_cluster,
            fixed_root,
            short_slot,
            lfn_start,
            lfn_count
        );

    if (node == NULL)
    {
        fat_node_data_t temporary = {
            .filesystem = filesystem,
            .parent_first_cluster =
                directory_cluster,
            .parent_fixed_root = fixed_root,
            .short_slot = short_slot,
            .lfn_start_slot = lfn_start,
            .lfn_count = lfn_count
        };

        (void)delete_directory_entries(
            &temporary
        );

        if (first_cluster != 0)
        {
            (void)free_cluster_chain(
                filesystem,
                first_cluster
            );
        }

        return NULL;
    }

    /*
     * VFS create_node() delegates ownership of insertion to the
     * filesystem create callback.  Parsed nodes already follow this
     * contract; newly created FAT nodes must do the same.
     */
    if (!vfs_add_child(parent, node))
    {
        fat_node_data_t temporary = {
            .filesystem = filesystem,
            .parent_first_cluster =
                directory_cluster,
            .parent_fixed_root = fixed_root,
            .short_slot = short_slot,
            .lfn_start_slot = lfn_start,
            .lfn_count = lfn_count
        };

        (void)delete_directory_entries(
            &temporary
        );

        if (first_cluster != 0)
        {
            (void)free_cluster_chain(
                filesystem,
                first_cluster
            );
        }

        free_node_tree(node);
        return NULL;
    }

    filesystem->create_operations++;

    if (lfn_count > 0)
    {
        filesystem->long_names++;

        if (name_has_non_ascii(name))
        {
            filesystem->unicode_names++;
        }
    }

    return node;
}

static bool fat_remove(vfs_node_t *node)
{
    if (
        node == NULL ||
        node->filesystem_data == NULL
    )
    {
        return false;
    }

    fat_node_data_t *data =
        node->filesystem_data;

    if (
        data->filesystem == NULL ||
        !data->filesystem->writable ||
        (
            node->type == VFS_NODE_DIRECTORY &&
            node->first_child != NULL
        )
    )
    {
        return false;
    }

    if (
        !delete_directory_entries(data) ||
        !free_cluster_chain(
            data->filesystem,
            data->first_cluster
        )
    )
    {
        return false;
    }

    data->filesystem->remove_operations++;
    return true;
}

static bool update_dotdot_entry(
    fat_filesystem_t *filesystem,
    uint32_t directory_cluster,
    uint32_t parent_cluster
)
{
    uint8_t entry[FAT_DIRECTORY_ENTRY_SIZE];

    if (
        !read_directory_slot(
            filesystem,
            directory_cluster,
            false,
            1,
            entry
        ) ||
        entry[0] != '.' ||
        entry[1] != '.'
    )
    {
        return false;
    }

    set_entry_first_cluster(
        entry,
        parent_cluster
    );

    return write_directory_slot(
        filesystem,
        directory_cluster,
        false,
        1,
        false,
        entry
    );
}

static bool fat_move(
    vfs_node_t *node,
    vfs_node_t *new_parent,
    const char *new_name
)
{
    if (
        node == NULL ||
        new_parent == NULL ||
        new_name == NULL ||
        new_name[0] == '\0' ||
        !unicode_utf8_validate(new_name)
    )
    {
        return false;
    }

    fat_node_data_t *data =
        node->filesystem_data;

    fat_node_data_t *parent_data =
        new_parent->filesystem_data;

    if (
        data == NULL ||
        parent_data == NULL ||
        data->filesystem == NULL ||
        data->filesystem !=
            parent_data->filesystem ||
        !data->filesystem->writable
    )
    {
        return false;
    }

    fat_filesystem_t *filesystem =
        data->filesystem;

    bool fixed_root =
        new_parent == filesystem->root &&
        filesystem->kind == FAT_KIND_16;

    uint32_t directory_cluster =
        fixed_root ?
            0U :
            parent_data->first_cluster;

    uint32_t new_lfn_start;
    uint16_t new_lfn_count;
    uint32_t new_short_slot;

    if (
        !write_named_directory_entry(
            filesystem,
            directory_cluster,
            fixed_root,
            new_name,
            node->type == VFS_NODE_DIRECTORY ?
                FAT_ATTRIBUTE_DIRECTORY :
                FAT_ATTRIBUTE_ARCHIVE,
            data->first_cluster,
            (uint32_t)node->size,
            &new_lfn_start,
            &new_lfn_count,
            &new_short_slot
        )
    )
    {
        return false;
    }

    if (
        node->type == VFS_NODE_DIRECTORY &&
        data->first_cluster != 0
    )
    {
        uint32_t parent_cluster =
            fixed_root ?
                0U :
                parent_data->first_cluster;

        if (
            !update_dotdot_entry(
                filesystem,
                data->first_cluster,
                parent_cluster
            )
        )
        {
            fat_node_data_t temporary = {
                .filesystem = filesystem,
                .parent_first_cluster =
                    directory_cluster,
                .parent_fixed_root = fixed_root,
                .short_slot = new_short_slot,
                .lfn_start_slot =
                    new_lfn_start,
                .lfn_count = new_lfn_count
            };

            (void)delete_directory_entries(
                &temporary
            );

            return false;
        }
    }

    if (!delete_directory_entries(data))
    {
        return false;
    }

    data->parent_first_cluster =
        directory_cluster;
    data->parent_fixed_root = fixed_root;
    data->short_slot = new_short_slot;
    data->lfn_start_slot = new_lfn_start;
    data->lfn_count = new_lfn_count;

    if (
        !copy_text(
            node->name,
            sizeof(node->name),
            new_name
        )
    )
    {
        return false;
    }

    filesystem->move_operations++;
    return true;
}

static bool fat_metadata(vfs_node_t *node)
{
    return
        node != NULL &&
        node->filesystem_data != NULL;
}

bool fat_fs_mount_device_index(
    uint32_t device_index,
    const char *mount_path
)
{
    return mount_device_at(
        block_device_get(device_index),
        mount_path
    );
}

bool fat_fs_mount_first_usb(void)
{
    if (mounted_filesystem != NULL)
    {
        last_mount_error = "a FAT filesystem is already mounted";
        return false;
    }

    bool matched = false;

    for (
        uint32_t index = 0;
        index < block_device_count();
        index++
    )
    {
        const block_device_t *device =
            block_device_get(index);

        if (
            device_name_starts_with(
                device,
                "USB mass storage"
            )
        )
        {
            matched = true;

            if (
                mount_device_at(
                    device,
                    "/media/usb"
                )
            )
            {
                return true;
            }
        }
    }

    if (!matched)
    {
        last_mount_error = "no online USB mass-storage block device";
    }

    return false;
}

bool fat_fs_mount_first_sata(void)
{
    if (mounted_filesystem != NULL)
    {
        return false;
    }

    static const char *prefixes[] = {
        "AHCI partition",
        "AHCI SATA"
    };

    for (
        size_t prefix = 0;
        prefix < sizeof(prefixes) /
            sizeof(prefixes[0]);
        prefix++
    )
    {
        for (
            uint32_t index = 0;
            index < block_device_count();
            index++
        )
        {
            const block_device_t *device =
                block_device_get(index);

            if (
                device_name_starts_with(
                    device,
                    prefixes[prefix]
                ) &&
                mount_device_at(
                    device,
                    "/media/sata"
                )
            )
            {
                return true;
            }
        }
    }

    return false;
}

bool fat_fs_mount_first_nvme(void)
{
    if (mounted_filesystem != NULL)
    {
        return false;
    }

    static const char *prefixes[] = {
        "NVMe partition",
        "NVMe namespace"
    };

    for (
        size_t prefix = 0;
        prefix < sizeof(prefixes) /
            sizeof(prefixes[0]);
        prefix++
    )
    {
        for (
            uint32_t index = 0;
            index < block_device_count();
            index++
        )
        {
            const block_device_t *device =
                block_device_get(index);

            if (
                device_name_starts_with(
                    device,
                    prefixes[prefix]
                ) &&
                mount_device_at(
                    device,
                    "/media/nvme"
                )
            )
            {
                return true;
            }
        }
    }

    return false;
}

bool fat_fs_sync(void)
{
    if (mounted_filesystem == NULL)
    {
        return false;
    }

    bool success =
        write_fsinfo(mounted_filesystem) &&
        block_device_sync(
            mounted_filesystem->device
        );

    if (success)
    {
        mounted_filesystem->dirty = false;
        mounted_filesystem->sync_operations++;
    }

    return success;
}

bool fat_fs_unmount(void)
{
    if (mounted_filesystem == NULL)
    {
        return false;
    }

    if (!fat_fs_sync())
    {
        return false;
    }

    vfs_node_t *root =
        vfs_detach_mount(
            mounted_filesystem->mount_path
        );

    if (root == NULL)
    {
        return false;
    }

    fat_filesystem_t *filesystem =
        mounted_filesystem;

    mounted_filesystem = NULL;
    free_node_tree(root);
    kfree(filesystem);
    return true;
}

bool fat_fs_mounted(void)
{
    return mounted_filesystem != NULL;
}

const char *fat_fs_mount_path(void)
{
    if (mounted_filesystem == NULL)
    {
        return NULL;
    }

    return mounted_filesystem->mount_path;
}

void fat_fs_print_status(void)
{
    if (mounted_filesystem == NULL)
    {
        kprintf("FAT filesystem: not mounted\n");
        kprintf(
            "Last mount error: %s\n",
            last_mount_error
        );
        return;
    }

    kprintf(
        "FAT filesystem: FAT%u %s UTF-8/VFAT\n",
        mounted_filesystem->kind ==
            FAT_KIND_16 ?
                16U :
                32U,
        mounted_filesystem->writable ?
            "read-write complete" :
            "read-only"
    );

    kprintf(
        "Device: %s\n",
        mounted_filesystem->device->name
    );

    kprintf(
        "Volume: %s\n",
        mounted_filesystem->volume_label
    );

    kprintf(
        "Sector=%u cluster=%u bytes clusters=%u dirty=%s\n",
        (unsigned int)
            mounted_filesystem->bytes_per_sector,
        (unsigned int)
            cluster_size_bytes(
                mounted_filesystem
            ),
        (unsigned int)
            mounted_filesystem->cluster_count,
        mounted_filesystem->dirty ?
            "yes" :
            "no"
    );

    kprintf(
        "Mounted at %s files=%u directories=%u long-names=%u unicode=%u\n",
        mounted_filesystem->mount_path,
        (unsigned int)mounted_filesystem->files,
        (unsigned int)(
            mounted_filesystem->directories > 0 ?
                mounted_filesystem->directories - 1U :
                0U
        ),
        (unsigned int)
            mounted_filesystem->long_names,
        (unsigned int)
            mounted_filesystem->unicode_names
    );

    if (mounted_filesystem->kind == FAT_KIND_32)
    {
        kprintf(
            "FAT32 FSInfo: valid=%s free=%u next=%u updates=%u\n",
            mounted_filesystem->fsinfo_valid ?
                "yes" :
                "no",
            (unsigned int)
                mounted_filesystem->free_clusters,
            (unsigned int)
                mounted_filesystem->next_free_cluster,
            (unsigned int)
                mounted_filesystem->fsinfo_updates
        );
    }

    kprintf(
        "FAT write self-test: %s\n",
        last_write_test_error
    );

    kprintf(
        "Operations: writes=%u creates=%u removes=%u moves=%u syncs=%u alloc=%u free=%u\n",
        (unsigned int)
            mounted_filesystem->write_operations,
        (unsigned int)
            mounted_filesystem->create_operations,
        (unsigned int)
            mounted_filesystem->remove_operations,
        (unsigned int)
            mounted_filesystem->move_operations,
        (unsigned int)
            mounted_filesystem->sync_operations,
        (unsigned int)
            mounted_filesystem->allocated_clusters,
        (unsigned int)
            mounted_filesystem->freed_clusters
    );
}

bool fat_fs_run_write_test(void)
{
    if (mounted_filesystem == NULL)
    {
        last_write_test_error = "filesystem is not mounted";
        return false;
    }

    if (!mounted_filesystem->writable)
    {
        last_write_test_error = "filesystem is read-only";
        return false;
    }

    last_write_test_error = "building test paths";

    char directory[FAT_TEST_PATH_MAX];
    char first_path[FAT_TEST_PATH_MAX];
    char renamed_path[FAT_TEST_PATH_MAX];

    static const char directory_suffix[] =
        "/Unicode Runtime";

    static const char first_suffix[] =
        "/runtime caf\xC3\xA9.txt";

    static const char renamed_name[] =
        "\xE1\x83\x92\xE1\x83\xA3\xE1\x83\xA0"
        "\xE1\x83\x90\xE1\x83\x9B.txt";

    static const char payload[] =
        "FAT16/FAT32 UTF-8 VFAT write test passed";

    size_t mount_length =
        string_length(
            mounted_filesystem->mount_path
        );

    if (
        mount_length + sizeof(directory_suffix) >
            sizeof(directory)
    )
    {
        last_write_test_error = "directory test path is too long";
        return false;
    }

    copy_bytes(
        directory,
        mounted_filesystem->mount_path,
        mount_length
    );

    copy_bytes(
        directory + mount_length,
        directory_suffix,
        sizeof(directory_suffix)
    );

    size_t directory_length =
        string_length(directory);

    if (
        directory_length + sizeof(first_suffix) >
            sizeof(first_path) ||
        directory_length + 1U +
            sizeof(renamed_name) >
            sizeof(renamed_path)
    )
    {
        last_write_test_error = "file test path is too long";
        return false;
    }

    copy_bytes(
        first_path,
        directory,
        directory_length
    );

    copy_bytes(
        first_path + directory_length,
        first_suffix,
        sizeof(first_suffix)
    );

    copy_bytes(
        renamed_path,
        directory,
        directory_length
    );

    renamed_path[directory_length] = '/';

    copy_bytes(
        renamed_path + directory_length + 1U,
        renamed_name,
        sizeof(renamed_name)
    );

    (void)vfs_remove(directory, true);

    last_write_test_error = "creating Unicode directory";

    if (!vfs_make_directory(directory))
    {
        return false;
    }

    last_write_test_error = "creating and writing UTF-8 file";

    if (!vfs_write_text(first_path, payload))
    {
        (void)vfs_remove(directory, true);
        return false;
    }

    last_write_test_error = "renaming file to Unicode VFAT name";

    if (!vfs_rename(first_path, renamed_name))
    {
        (void)vfs_remove(directory, true);
        return false;
    }

    last_write_test_error = "opening renamed Unicode file";

    vfs_node_t *file =
        vfs_open(renamed_path);

    if (file == NULL)
    {
        (void)vfs_remove(directory, true);
        return false;
    }

    char buffer[FAT_TEST_BUFFER_SIZE];
    clear_bytes(buffer, sizeof(buffer));

    last_write_test_error = "reading renamed Unicode file";

    size_t read =
        vfs_read(
            file,
            0,
            buffer,
            sizeof(payload)
        );

    if (read != sizeof(payload) - 1U)
    {
        (void)vfs_remove(directory, true);
        last_write_test_error = "renamed file length mismatch";
        return false;
    }

    if (!strings_equal(buffer, payload))
    {
        (void)vfs_remove(directory, true);
        last_write_test_error = "renamed file contents mismatch";
        return false;
    }

    last_write_test_error = "removing Unicode test directory";

    if (!vfs_remove(directory, true))
    {
        return false;
    }

    last_write_test_error = "synchronizing FAT and block cache";

    if (!fat_fs_sync())
    {
        return false;
    }

    last_write_test_error = "passed";
    return true;
}
