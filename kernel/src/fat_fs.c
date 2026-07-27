#include "fat_fs.h"

#include "block_device.h"
#include "heap.h"
#include "kstdio.h"
#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FAT_MAX_NODES 512U
#define FAT_MAX_DEPTH 12U
#define FAT_ATTRIBUTE_DIRECTORY 0x10U
#define FAT_ATTRIBUTE_VOLUME_ID 0x08U
#define FAT_ATTRIBUTE_ARCHIVE 0x20U
#define FAT_ATTRIBUTE_LONG_NAME 0x0FU
#define FAT16_END_OF_CHAIN 0xFFF8U
#define FAT32_END_OF_CHAIN 0x0FFFFFF8U
#define FAT_LFN_LAST_ENTRY 0x40U
#define FAT_LFN_ORDER_MASK 0x1FU
#define FAT_LFN_CHARS_PER_ENTRY 13U
#define FAT_DIRECTORY_ENTRY_SIZE 32U
#define FAT_INVALID_SLOT UINT32_MAX
#define FAT_TEST_BUFFER_SIZE 1536U

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
    char name[VFS_NAME_MAX + 1];
    uint8_t checksum;
    uint8_t expected_order;
    uint16_t entry_count;
    uint32_t start_slot;
    bool active;
    bool complete;
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

    char volume_label[12];
    uint32_t files;
    uint32_t directories;
    uint32_t nodes;
    uint32_t long_names;
    uint32_t write_operations;
    uint32_t create_operations;
    uint32_t remove_operations;
    uint32_t move_operations;
    uint32_t allocated_clusters;
    uint32_t freed_clusters;
    uint32_t sync_operations;
    bool writable;
    bool dirty;
    char mount_path[VFS_PATH_MAX];

    vfs_node_t *root;
};

static fat_filesystem_t *mounted_filesystem;

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
    uint8_t *destination_bytes = destination;
    const uint8_t *source_bytes = source;

    for (size_t index = 0; index < count; index++)
    {
        destination_bytes[index] = source_bytes[index];
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

static char lower_ascii(char character)
{
    if (character >= 'A' && character <= 'Z')
    {
        return (char)(character + ('a' - 'A'));
    }

    return character;
}

static char upper_ascii(char character)
{
    if (character >= 'a' && character <= 'z')
    {
        return (char)(character - ('a' - 'A'));
    }

    return character;
}

static bool copy_node_name(
    char *destination,
    const char *source
)
{
    size_t index = 0;

    if (destination == NULL || source == NULL)
    {
        return false;
    }

    while (source[index] != '\0')
    {
        if (index >= VFS_NAME_MAX)
        {
            return false;
        }

        destination[index] = source[index];
        index++;
    }

    destination[index] = '\0';
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
    uint32_t cluster
)
{
    if (filesystem->kind == FAT_KIND_16)
    {
        return cluster >= FAT16_END_OF_CHAIN;
    }

    return cluster >= FAT32_END_OF_CHAIN;
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
            fat_index * filesystem->fat_size_sectors +
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

static uint32_t next_cluster(
    const fat_filesystem_t *filesystem,
    uint32_t cluster
)
{
    return fat_entry(filesystem, cluster);
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
    bool success = true;
    uint32_t first_sector =
        cluster_to_sector(filesystem, cluster);

    for (
        uint32_t index = 0;
        index < filesystem->sectors_per_cluster;
        index++
    )
    {
        if (!write_sector(
                filesystem,
                first_sector + index,
                buffer
            ))
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

    for (
        uint32_t cluster = 2U;
        cluster < filesystem->cluster_count + 2U;
        cluster++
    )
    {
        if (fat_entry(filesystem, cluster) != 0)
        {
            continue;
        }

        if (
            !set_fat_entry(
                filesystem,
                cluster,
                end_of_chain_value(filesystem)
            ) ||
            !zero_cluster(filesystem, cluster)
        )
        {
            (void)set_fat_entry(filesystem, cluster, 0);
            return 0;
        }

        filesystem->allocated_clusters++;
        return cluster;
    }

    return 0;
}

static bool free_cluster_chain(
    fat_filesystem_t *filesystem,
    uint32_t first_cluster
)
{
    if (first_cluster == 0)
    {
        return true;
    }

    if (!cluster_valid(filesystem, first_cluster))
    {
        return false;
    }

    uint32_t cluster = first_cluster;
    uint32_t visited = 0;

    while (
        cluster_valid(filesystem, cluster) &&
        visited <= filesystem->cluster_count
    )
    {
        uint32_t next = next_cluster(filesystem, cluster);

        if (!set_fat_entry(filesystem, cluster, 0))
        {
            return false;
        }

        filesystem->freed_clusters++;
        visited++;

        if (
            next == 0 ||
            cluster_is_end(filesystem, next)
        )
        {
            return true;
        }

        cluster = next;
    }

    return false;
}

static uint32_t cluster_chain_length(
    const fat_filesystem_t *filesystem,
    uint32_t first_cluster
)
{
    if (first_cluster == 0)
    {
        return 0;
    }

    if (!cluster_valid(filesystem, first_cluster))
    {
        return 0;
    }

    uint32_t count = 0;
    uint32_t cluster = first_cluster;

    while (
        cluster_valid(filesystem, cluster) &&
        count <= filesystem->cluster_count
    )
    {
        count++;
        uint32_t next = next_cluster(filesystem, cluster);

        if (
            next == 0 ||
            cluster_is_end(filesystem, next)
        )
        {
            break;
        }

        cluster = next;
    }

    return count;
}

static uint32_t cluster_at_index(
    fat_filesystem_t *filesystem,
    uint32_t first_cluster,
    uint32_t index,
    bool extend
)
{
    if (!cluster_valid(filesystem, first_cluster))
    {
        return 0;
    }

    uint32_t cluster = first_cluster;

    for (uint32_t current = 0; current < index; current++)
    {
        uint32_t next = next_cluster(filesystem, cluster);

        if (
            next == 0 ||
            cluster_is_end(filesystem, next)
        )
        {
            if (!extend)
            {
                return 0;
            }

            next = allocate_cluster(filesystem);

            if (
                next == 0 ||
                !set_fat_entry(filesystem, cluster, next)
            )
            {
                if (next != 0)
                {
                    (void)free_cluster_chain(filesystem, next);
                }

                return 0;
            }
        }

        if (!cluster_valid(filesystem, next))
        {
            return 0;
        }

        cluster = next;
    }

    return cluster;
}

static bool resize_cluster_chain(
    fat_filesystem_t *filesystem,
    uint32_t *first_cluster,
    uint32_t required_clusters
)
{
    if (
        filesystem == NULL ||
        first_cluster == NULL ||
        !filesystem->writable
    )
    {
        return false;
    }

    uint32_t existing = cluster_chain_length(
        filesystem,
        *first_cluster
    );

    if (required_clusters == existing)
    {
        return true;
    }

    if (required_clusters == 0)
    {
        bool success = free_cluster_chain(
            filesystem,
            *first_cluster
        );

        if (success)
        {
            *first_cluster = 0;
        }

        return success;
    }

    if (existing == 0)
    {
        uint32_t first = allocate_cluster(filesystem);

        if (first == 0)
        {
            return false;
        }

        *first_cluster = first;
        existing = 1;
    }

    if (required_clusters > existing)
    {
        uint32_t last = cluster_at_index(
            filesystem,
            *first_cluster,
            existing - 1U,
            false
        );

        if (last == 0)
        {
            return false;
        }

        uint32_t first_new = 0;
        uint32_t previous = last;

        for (
            uint32_t index = existing;
            index < required_clusters;
            index++
        )
        {
            uint32_t next = allocate_cluster(filesystem);

            if (
                next == 0 ||
                !set_fat_entry(filesystem, previous, next)
            )
            {
                if (next != 0)
                {
                    (void)free_cluster_chain(filesystem, next);
                }

                if (first_new != 0)
                {
                    (void)free_cluster_chain(
                        filesystem,
                        first_new
                    );
                }

                (void)set_fat_entry(
                    filesystem,
                    last,
                    end_of_chain_value(filesystem)
                );

                return false;
            }

            if (first_new == 0)
            {
                first_new = next;
            }

            previous = next;
        }

        return true;
    }

    uint32_t last_kept = cluster_at_index(
        filesystem,
        *first_cluster,
        required_clusters - 1U,
        false
    );

    if (last_kept == 0)
    {
        return false;
    }

    uint32_t removed = next_cluster(filesystem, last_kept);

    if (!set_fat_entry(
            filesystem,
            last_kept,
            end_of_chain_value(filesystem)
        ))
    {
        return false;
    }

    if (
        removed != 0 &&
        !cluster_is_end(filesystem, removed)
    )
    {
        return free_cluster_chain(filesystem, removed);
    }

    return true;
}

static bool directory_slot_location(
    fat_filesystem_t *filesystem,
    uint32_t first_cluster,
    bool fixed_root,
    uint32_t slot,
    bool extend,
    uint32_t *sector,
    uint16_t *offset
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

    uint32_t entries_per_sector =
        filesystem->bytes_per_sector /
        FAT_DIRECTORY_ENTRY_SIZE;

    if (fixed_root)
    {
        if (slot >= filesystem->root_entry_count)
        {
            return false;
        }

        *sector =
            filesystem->first_root_sector +
            slot / entries_per_sector;

        *offset = (uint16_t)(
            (slot % entries_per_sector) *
            FAT_DIRECTORY_ENTRY_SIZE
        );

        return true;
    }

    if (!cluster_valid(filesystem, first_cluster))
    {
        return false;
    }

    uint32_t entries_per_cluster =
        cluster_size_bytes(filesystem) /
        FAT_DIRECTORY_ENTRY_SIZE;

    uint32_t cluster_index =
        slot / entries_per_cluster;

    uint32_t slot_in_cluster =
        slot % entries_per_cluster;

    uint32_t cluster = cluster_at_index(
        filesystem,
        first_cluster,
        cluster_index,
        extend
    );

    if (cluster == 0)
    {
        return false;
    }

    *sector =
        cluster_to_sector(filesystem, cluster) +
        slot_in_cluster / entries_per_sector;

    *offset = (uint16_t)(
        (slot_in_cluster % entries_per_sector) *
        FAT_DIRECTORY_ENTRY_SIZE
    );

    return true;
}

static bool read_directory_entry(
    fat_filesystem_t *filesystem,
    uint32_t first_cluster,
    bool fixed_root,
    uint32_t slot,
    uint8_t *entry
)
{
    uint32_t sector_number;
    uint16_t offset;

    if (
        entry == NULL ||
        !directory_slot_location(
            filesystem,
            first_cluster,
            fixed_root,
            slot,
            false,
            &sector_number,
            &offset
        )
    )
    {
        return false;
    }

    uint8_t *sector =
        kmalloc(filesystem->bytes_per_sector);

    if (sector == NULL)
    {
        return false;
    }

    bool success = read_sector(
        filesystem,
        sector_number,
        sector
    );

    if (success)
    {
        copy_bytes(
            entry,
            sector + offset,
            FAT_DIRECTORY_ENTRY_SIZE
        );
    }

    kfree(sector);
    return success;
}

static bool write_directory_entry(
    fat_filesystem_t *filesystem,
    uint32_t first_cluster,
    bool fixed_root,
    uint32_t slot,
    const uint8_t *entry,
    bool extend
)
{
    uint32_t sector_number;
    uint16_t offset;

    if (
        entry == NULL ||
        !directory_slot_location(
            filesystem,
            first_cluster,
            fixed_root,
            slot,
            extend,
            &sector_number,
            &offset
        )
    )
    {
        return false;
    }

    uint8_t *sector =
        kmalloc(filesystem->bytes_per_sector);

    if (sector == NULL)
    {
        return false;
    }

    bool success = read_sector(
        filesystem,
        sector_number,
        sector
    );

    if (success)
    {
        copy_bytes(
            sector + offset,
            entry,
            FAT_DIRECTORY_ENTRY_SIZE
        );

        success = write_sector(
            filesystem,
            sector_number,
            sector
        );
    }

    kfree(sector);
    return success;
}

static uint32_t directory_slot_capacity(
    const fat_filesystem_t *filesystem,
    uint32_t first_cluster,
    bool fixed_root
)
{
    if (fixed_root)
    {
        return filesystem->root_entry_count;
    }

    return
        cluster_chain_length(
            filesystem,
            first_cluster
        ) *
        cluster_size_bytes(filesystem) /
        FAT_DIRECTORY_ENTRY_SIZE;
}

static void reset_lfn(fat_lfn_state_t *state)
{
    if (state == NULL)
    {
        return;
    }

    clear_bytes(state, sizeof(*state));
    state->start_slot = FAT_INVALID_SLOT;
}

static uint8_t short_name_checksum(
    const uint8_t *short_name
)
{
    uint8_t checksum = 0;

    for (uint32_t index = 0; index < 11U; index++)
    {
        checksum = (uint8_t)(
            (
                (checksum & 1U) != 0 ?
                    0x80U :
                    0U
            ) +
            (checksum >> 1) +
            short_name[index]
        );
    }

    return checksum;
}

static uint16_t read_lfn_character(
    const uint8_t *entry,
    uint32_t index
)
{
    static const uint8_t offsets[
        FAT_LFN_CHARS_PER_ENTRY
    ] = {
        1U, 3U, 5U, 7U, 9U,
        14U, 16U, 18U, 20U, 22U, 24U,
        28U, 30U
    };

    return read_u16(entry + offsets[index]);
}

static void write_lfn_character(
    uint8_t *entry,
    uint32_t index,
    uint16_t character
)
{
    static const uint8_t offsets[
        FAT_LFN_CHARS_PER_ENTRY
    ] = {
        1U, 3U, 5U, 7U, 9U,
        14U, 16U, 18U, 20U, 22U, 24U,
        28U, 30U
    };

    write_u16(entry + offsets[index], character);
}

static void process_lfn_entry(
    fat_lfn_state_t *state,
    const uint8_t *entry,
    uint32_t slot
)
{
    if (state == NULL || entry == NULL)
    {
        return;
    }

    uint8_t raw_order = entry[0];
    uint8_t order =
        (uint8_t)(raw_order & FAT_LFN_ORDER_MASK);

    if (
        order == 0 ||
        (uint32_t)(order - 1U) *
            FAT_LFN_CHARS_PER_ENTRY >
            VFS_NAME_MAX
    )
    {
        reset_lfn(state);
        return;
    }

    if ((raw_order & FAT_LFN_LAST_ENTRY) != 0)
    {
        reset_lfn(state);
        state->active = true;
        state->valid = true;
        state->checksum = entry[13];
        state->expected_order = order;
        state->entry_count = order;
        state->start_slot = slot;
    }

    if (
        !state->active ||
        !state->valid ||
        state->expected_order != order ||
        state->checksum != entry[13]
    )
    {
        reset_lfn(state);
        return;
    }

    uint32_t base =
        (uint32_t)(order - 1U) *
        FAT_LFN_CHARS_PER_ENTRY;

    for (
        uint32_t index = 0;
        index < FAT_LFN_CHARS_PER_ENTRY;
        index++
    )
    {
        uint16_t character =
            read_lfn_character(entry, index);

        uint32_t position = base + index;

        if (character == 0x0000U)
        {
            if (position <= VFS_NAME_MAX)
            {
                state->name[position] = '\0';
            }

            break;
        }

        if (character == 0xFFFFU)
        {
            continue;
        }

        if (position >= VFS_NAME_MAX)
        {
            state->valid = false;
            continue;
        }

        state->name[position] =
            character <= 0x007FU ?
                lower_ascii((char)character) :
                '?';
    }

    state->expected_order--;

    if (state->expected_order == 0)
    {
        state->complete =
            state->valid &&
            state->name[0] != '\0';
    }
}

static bool consume_lfn_name(
    fat_lfn_state_t *state,
    const uint8_t *short_entry,
    char *name,
    uint32_t *start_slot,
    uint16_t *entry_count
)
{
    if (
        state == NULL ||
        short_entry == NULL ||
        name == NULL ||
        !state->active ||
        !state->complete ||
        !state->valid ||
        state->checksum !=
            short_name_checksum(short_entry)
    )
    {
        reset_lfn(state);
        return false;
    }

    bool copied = copy_node_name(
        name,
        state->name
    );

    if (copied)
    {
        if (start_slot != NULL)
        {
            *start_slot = state->start_slot;
        }

        if (entry_count != NULL)
        {
            *entry_count = state->entry_count;
        }
    }

    reset_lfn(state);
    return copied;
}

static bool format_short_name(
    const uint8_t *entry,
    char *name
)
{
    size_t position = 0;
    size_t base_length = 8;
    size_t extension_length = 3;

    while (
        base_length > 0 &&
        entry[base_length - 1] == ' '
    )
    {
        base_length--;
    }

    while (
        extension_length > 0 &&
        entry[8 + extension_length - 1] == ' '
    )
    {
        extension_length--;
    }

    if (base_length == 0)
    {
        return false;
    }

    for (size_t index = 0; index < base_length; index++)
    {
        if (position >= VFS_NAME_MAX)
        {
            return false;
        }

        name[position++] =
            lower_ascii((char)entry[index]);
    }

    if (extension_length > 0)
    {
        if (position >= VFS_NAME_MAX)
        {
            return false;
        }

        name[position++] = '.';

        for (
            size_t index = 0;
            index < extension_length;
            index++
        )
        {
            if (position >= VFS_NAME_MAX)
            {
                return false;
            }

            name[position++] =
                lower_ascii((char)entry[8 + index]);
        }
    }

    name[position] = '\0';
    return true;
}

static bool is_dot_entry(const uint8_t *entry)
{
    return
        entry[0] == '.' &&
        (
            entry[1] == ' ' ||
            entry[1] == '.'
        );
}

static vfs_node_t *allocate_node(
    fat_filesystem_t *filesystem,
    const char *name,
    vfs_node_type_t type,
    uint32_t first_cluster,
    uint32_t size,
    uint32_t parent_first_cluster,
    bool parent_fixed_root,
    uint32_t short_slot,
    uint32_t lfn_start_slot,
    uint16_t lfn_count
)
{
    if (
        filesystem == NULL ||
        filesystem->nodes >= FAT_MAX_NODES
    )
    {
        return NULL;
    }

    vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
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

    if (!copy_node_name(node->name, name))
    {
        kfree(data);
        kfree(node);
        return NULL;
    }

    data->filesystem = filesystem;
    data->first_cluster = first_cluster;
    data->parent_first_cluster = parent_first_cluster;
    data->parent_fixed_root = parent_fixed_root;
    data->short_slot = short_slot;
    data->lfn_start_slot = lfn_start_slot;
    data->lfn_count = lfn_count;

    node->type = type;
    node->size = size;
    node->operations = &fat_operations;
    node->filesystem_data = data;
    node->owner_uid = 0;
    node->owner_gid = 0;
    node->mode =
        type == VFS_NODE_DIRECTORY ?
            (filesystem->writable ? 0777U : 0555U) :
            (filesystem->writable ? 0666U : 0444U);

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

    uint32_t capacity = directory_slot_capacity(
        filesystem,
        first_cluster,
        fixed_root
    );

    fat_lfn_state_t lfn;
    reset_lfn(&lfn);

    for (uint32_t slot = 0; slot < capacity; slot++)
    {
        uint8_t entry[FAT_DIRECTORY_ENTRY_SIZE];

        if (!read_directory_entry(
                filesystem,
                first_cluster,
                fixed_root,
                slot,
                entry
            ))
        {
            return false;
        }

        if (entry[0] == 0x00U)
        {
            return true;
        }

        if (entry[0] == 0xE5U)
        {
            reset_lfn(&lfn);
            continue;
        }

        if (entry[11] == FAT_ATTRIBUTE_LONG_NAME)
        {
            process_lfn_entry(&lfn, entry, slot);
            continue;
        }

        if (
            (entry[11] & FAT_ATTRIBUTE_VOLUME_ID) != 0 ||
            is_dot_entry(entry)
        )
        {
            reset_lfn(&lfn);
            continue;
        }

        char name[VFS_NAME_MAX + 1];
        uint32_t lfn_start_slot = FAT_INVALID_SLOT;
        uint16_t lfn_count = 0;

        bool long_name = consume_lfn_name(
            &lfn,
            entry,
            name,
            &lfn_start_slot,
            &lfn_count
        );

        if (
            !long_name &&
            !format_short_name(entry, name)
        )
        {
            continue;
        }

        if (long_name)
        {
            filesystem->long_names++;
        }

        bool directory =
            (entry[11] & FAT_ATTRIBUTE_DIRECTORY) != 0;

        uint32_t high_cluster =
            filesystem->kind == FAT_KIND_32 ?
                read_u16(entry + 20) :
                0;

        uint32_t entry_cluster =
            (high_cluster << 16) |
            read_u16(entry + 26);

        uint32_t file_size = read_u32(entry + 28);

        vfs_node_t *node = allocate_node(
            filesystem,
            name,
            directory ?
                VFS_NODE_DIRECTORY :
                VFS_NODE_FILE,
            entry_cluster,
            file_size,
            first_cluster,
            fixed_root,
            slot,
            lfn_start_slot,
            lfn_count
        );

        if (
            node == NULL ||
            !vfs_add_child(parent, node)
        )
        {
            return false;
        }

        if (
            directory &&
            cluster_valid(filesystem, entry_cluster) &&
            depth < FAT_MAX_DEPTH &&
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
    if (
        node == NULL ||
        buffer == NULL ||
        node->type != VFS_NODE_FILE ||
        node->filesystem_data == NULL ||
        offset >= node->size
    )
    {
        return 0;
    }

    fat_node_data_t *data = node->filesystem_data;
    fat_filesystem_t *filesystem = data->filesystem;

    if (
        filesystem == NULL ||
        !cluster_valid(filesystem, data->first_cluster)
    )
    {
        return 0;
    }

    size_t available = node->size - offset;

    if (count > available)
    {
        count = available;
    }

    uint32_t cluster_size =
        cluster_size_bytes(filesystem);

    uint32_t skip_clusters =
        (uint32_t)(offset / cluster_size);

    uint32_t cluster = cluster_at_index(
        filesystem,
        data->first_cluster,
        skip_clusters,
        false
    );

    if (cluster == 0)
    {
        return 0;
    }

    uint8_t *sector =
        kmalloc(filesystem->bytes_per_sector);

    if (sector == NULL)
    {
        return 0;
    }

    uint8_t *destination = buffer;
    size_t completed = 0;
    uint32_t cluster_offset =
        (uint32_t)(offset % cluster_size);

    while (
        completed < count &&
        cluster_valid(filesystem, cluster)
    )
    {
        uint32_t sector_index =
            cluster_offset /
            filesystem->bytes_per_sector;

        uint32_t byte_offset =
            cluster_offset %
            filesystem->bytes_per_sector;

        while (
            sector_index <
                filesystem->sectors_per_cluster &&
            completed < count
        )
        {
            uint32_t sector_number =
                cluster_to_sector(filesystem, cluster) +
                sector_index;

            if (!read_sector(
                    filesystem,
                    sector_number,
                    sector
                ))
            {
                kfree(sector);
                return completed;
            }

            size_t bytes =
                filesystem->bytes_per_sector -
                byte_offset;

            if (bytes > count - completed)
            {
                bytes = count - completed;
            }

            copy_bytes(
                destination + completed,
                sector + byte_offset,
                bytes
            );

            completed += bytes;
            sector_index++;
            byte_offset = 0;
        }

        if (completed >= count)
        {
            break;
        }

        uint32_t next = next_cluster(filesystem, cluster);

        if (
            next == 0 ||
            cluster_is_end(filesystem, next)
        )
        {
            break;
        }

        cluster = next;
        cluster_offset = 0;
    }

    kfree(sector);
    return completed;
}

static bool update_short_entry(
    vfs_node_t *node
)
{
    if (
        node == NULL ||
        node->filesystem_data == NULL
    )
    {
        return false;
    }

    fat_node_data_t *data = node->filesystem_data;
    fat_filesystem_t *filesystem = data->filesystem;
    uint8_t entry[FAT_DIRECTORY_ENTRY_SIZE];

    if (
        filesystem == NULL ||
        data->short_slot == FAT_INVALID_SLOT ||
        !read_directory_entry(
            filesystem,
            data->parent_first_cluster,
            data->parent_fixed_root,
            data->short_slot,
            entry
        )
    )
    {
        return false;
    }

    if (filesystem->kind == FAT_KIND_32)
    {
        write_u16(
            entry + 20,
            (uint16_t)(data->first_cluster >> 16)
        );
    }

    write_u16(
        entry + 26,
        (uint16_t)data->first_cluster
    );

    write_u32(
        entry + 28,
        node->type == VFS_NODE_FILE ?
            (uint32_t)node->size :
            0U
    );

    return write_directory_entry(
        filesystem,
        data->parent_first_cluster,
        data->parent_fixed_root,
        data->short_slot,
        entry,
        false
    );
}

static bool ensure_file_capacity(
    vfs_node_t *node,
    size_t size
)
{
    if (
        node == NULL ||
        node->filesystem_data == NULL ||
        size > UINT32_MAX
    )
    {
        return false;
    }

    fat_node_data_t *data = node->filesystem_data;
    fat_filesystem_t *filesystem = data->filesystem;

    if (
        filesystem == NULL ||
        !filesystem->writable
    )
    {
        return false;
    }

    uint32_t cluster_size =
        cluster_size_bytes(filesystem);

    uint32_t required =
        size == 0 ?
            0U :
            (uint32_t)(
                (size + cluster_size - 1U) /
                cluster_size
            );

    uint32_t previous_cluster = data->first_cluster;

    if (!resize_cluster_chain(
            filesystem,
            &data->first_cluster,
            required
        ))
    {
        return false;
    }

    if (
        data->first_cluster != previous_cluster &&
        !update_short_entry(node)
    )
    {
        return false;
    }

    return true;
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
        node->type != VFS_NODE_FILE ||
        node->filesystem_data == NULL ||
        offset > node->size ||
        count > UINT32_MAX - offset
    )
    {
        return 0;
    }

    fat_node_data_t *data = node->filesystem_data;
    fat_filesystem_t *filesystem = data->filesystem;
    size_t target_size = offset + count;

    if (
        filesystem == NULL ||
        !filesystem->writable ||
        !ensure_file_capacity(node, target_size) ||
        !cluster_valid(filesystem, data->first_cluster)
    )
    {
        return 0;
    }

    uint32_t cluster_size =
        cluster_size_bytes(filesystem);

    uint32_t skip_clusters =
        (uint32_t)(offset / cluster_size);

    uint32_t cluster = cluster_at_index(
        filesystem,
        data->first_cluster,
        skip_clusters,
        false
    );

    if (cluster == 0)
    {
        return 0;
    }

    uint8_t *sector =
        kmalloc(filesystem->bytes_per_sector);

    if (sector == NULL)
    {
        return 0;
    }

    const uint8_t *source = buffer;
    size_t completed = 0;
    uint32_t cluster_offset =
        (uint32_t)(offset % cluster_size);

    while (
        completed < count &&
        cluster_valid(filesystem, cluster)
    )
    {
        uint32_t sector_index =
            cluster_offset /
            filesystem->bytes_per_sector;

        uint32_t byte_offset =
            cluster_offset %
            filesystem->bytes_per_sector;

        while (
            sector_index <
                filesystem->sectors_per_cluster &&
            completed < count
        )
        {
            uint32_t sector_number =
                cluster_to_sector(filesystem, cluster) +
                sector_index;

            if (!read_sector(
                    filesystem,
                    sector_number,
                    sector
                ))
            {
                kfree(sector);
                return completed;
            }

            size_t bytes =
                filesystem->bytes_per_sector -
                byte_offset;

            if (bytes > count - completed)
            {
                bytes = count - completed;
            }

            copy_bytes(
                sector + byte_offset,
                source + completed,
                bytes
            );

            if (!write_sector(
                    filesystem,
                    sector_number,
                    sector
                ))
            {
                kfree(sector);
                return completed;
            }

            completed += bytes;
            sector_index++;
            byte_offset = 0;
        }

        if (completed >= count)
        {
            break;
        }

        uint32_t next = next_cluster(filesystem, cluster);

        if (
            next == 0 ||
            cluster_is_end(filesystem, next)
        )
        {
            break;
        }

        cluster = next;
        cluster_offset = 0;
    }

    kfree(sector);

    if (completed > 0)
    {
        size_t new_size = offset + completed;

        if (new_size > node->size)
        {
            node->size = new_size;

            if (!update_short_entry(node))
            {
                return 0;
            }
        }

        filesystem->write_operations++;
    }

    return completed;
}

static bool fat_truncate(vfs_node_t *node)
{
    if (
        node == NULL ||
        node->type != VFS_NODE_FILE ||
        node->filesystem_data == NULL
    )
    {
        return false;
    }

    fat_node_data_t *data = node->filesystem_data;
    fat_filesystem_t *filesystem = data->filesystem;

    if (
        filesystem == NULL ||
        !filesystem->writable ||
        !resize_cluster_chain(
            filesystem,
            &data->first_cluster,
            0
        )
    )
    {
        return false;
    }

    node->size = 0;

    if (!update_short_entry(node))
    {
        return false;
    }

    filesystem->write_operations++;
    return true;
}

static bool short_character_valid(char character)
{
    char upper = upper_ascii(character);

    return
        (upper >= 'A' && upper <= 'Z') ||
        (upper >= '0' && upper <= '9') ||
        upper == '$' ||
        upper == '%' ||
        upper == '\'' ||
        upper == '-' ||
        upper == '_' ||
        upper == '@' ||
        upper == '~' ||
        upper == '`' ||
        upper == '!' ||
        upper == '(' ||
        upper == ')' ||
        upper == '{' ||
        upper == '}' ||
        upper == '^' ||
        upper == '#' ||
        upper == '&';
}

static bool alias_exists(
    fat_filesystem_t *filesystem,
    uint32_t parent_first_cluster,
    bool parent_fixed_root,
    const uint8_t alias[11]
)
{
    uint32_t capacity = directory_slot_capacity(
        filesystem,
        parent_first_cluster,
        parent_fixed_root
    );

    for (uint32_t slot = 0; slot < capacity; slot++)
    {
        uint8_t entry[FAT_DIRECTORY_ENTRY_SIZE];

        if (!read_directory_entry(
                filesystem,
                parent_first_cluster,
                parent_fixed_root,
                slot,
                entry
            ))
        {
            return true;
        }

        if (entry[0] == 0x00U)
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

        for (uint32_t index = 0; index < 11U; index++)
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

static void clear_alias(uint8_t alias[11])
{
    for (uint32_t index = 0; index < 11U; index++)
    {
        alias[index] = ' ';
    }
}

static bool build_short_alias(
    fat_filesystem_t *filesystem,
    uint32_t parent_first_cluster,
    bool parent_fixed_root,
    const char *name,
    uint8_t alias[11],
    bool *needs_lfn
)
{
    if (
        filesystem == NULL ||
        name == NULL ||
        alias == NULL ||
        needs_lfn == NULL
    )
    {
        return false;
    }

    size_t length = string_length(name);

    if (length == 0 || length > VFS_NAME_MAX)
    {
        return false;
    }

    size_t dot = length;

    for (size_t index = length; index > 0; index--)
    {
        if (name[index - 1U] == '.')
        {
            dot = index - 1U;
            break;
        }
    }

    size_t base_length = dot;
    size_t extension_length =
        dot < length ?
            length - dot - 1U :
            0;

    bool direct =
        base_length >= 1U &&
        base_length <= 8U &&
        extension_length <= 3U;

    if (direct)
    {
        for (size_t index = 0; index < base_length; index++)
        {
            if (!short_character_valid(name[index]))
            {
                direct = false;
                break;
            }
        }
    }

    if (direct)
    {
        for (
            size_t index = 0;
            index < extension_length;
            index++
        )
        {
            if (!short_character_valid(name[dot + 1U + index]))
            {
                direct = false;
                break;
            }
        }
    }

    if (direct)
    {
        clear_alias(alias);

        for (size_t index = 0; index < base_length; index++)
        {
            alias[index] =
                (uint8_t)upper_ascii(name[index]);
        }

        for (
            size_t index = 0;
            index < extension_length;
            index++
        )
        {
            alias[8U + index] =
                (uint8_t)upper_ascii(
                    name[dot + 1U + index]
                );
        }

        if (!alias_exists(
                filesystem,
                parent_first_cluster,
                parent_fixed_root,
                alias
            ))
        {
            *needs_lfn = false;
            return true;
        }
    }

    char compact[7];
    size_t compact_length = 0;

    for (
        size_t index = 0;
        index < base_length && compact_length < 6U;
        index++
    )
    {
        if (short_character_valid(name[index]))
        {
            compact[compact_length++] =
                upper_ascii(name[index]);
        }
    }

    if (compact_length == 0)
    {
        compact[compact_length++] = 'F';
        compact[compact_length++] = 'I';
        compact[compact_length++] = 'L';
        compact[compact_length++] = 'E';
    }

    for (uint32_t number = 1U; number <= 9999U; number++)
    {
        clear_alias(alias);
        size_t position = 0;

        for (
            size_t index = 0;
            index < compact_length && position < 6U;
            index++
        )
        {
            alias[position++] =
                (uint8_t)compact[index];
        }

        alias[position++] = '~';

        char digits[4];
        uint32_t digit_count = 0;
        uint32_t value = number;

        do
        {
            digits[digit_count++] =
                (char)('0' + value % 10U);
            value /= 10U;
        }
        while (value != 0 && digit_count < 4U);

        while (
            digit_count > 0 &&
            position < 8U
        )
        {
            alias[position++] =
                (uint8_t)digits[--digit_count];
        }

        size_t extension_position = 0;

        for (
            size_t index = 0;
            index < extension_length &&
                extension_position < 3U;
            index++
        )
        {
            char character = name[dot + 1U + index];

            if (short_character_valid(character))
            {
                alias[8U + extension_position] =
                    (uint8_t)upper_ascii(character);
                extension_position++;
            }
        }

        if (!alias_exists(
                filesystem,
                parent_first_cluster,
                parent_fixed_root,
                alias
            ))
        {
            *needs_lfn = true;
            return true;
        }
    }

    return false;
}

static uint16_t lfn_entry_count_for_name(
    const char *name
)
{
    size_t length = string_length(name);

    return (uint16_t)(
        (length + 1U +
            FAT_LFN_CHARS_PER_ENTRY - 1U) /
        FAT_LFN_CHARS_PER_ENTRY
    );
}

static void build_lfn_entry(
    uint8_t entry[FAT_DIRECTORY_ENTRY_SIZE],
    const char *name,
    uint16_t order,
    uint16_t count,
    uint8_t checksum
)
{
    clear_bytes(entry, FAT_DIRECTORY_ENTRY_SIZE);
    entry[0] = (uint8_t)order;

    if (order == count)
    {
        entry[0] |= FAT_LFN_LAST_ENTRY;
    }

    entry[11] = FAT_ATTRIBUTE_LONG_NAME;
    entry[12] = 0;
    entry[13] = checksum;
    write_u16(entry + 26, 0);

    size_t length = string_length(name);
    uint32_t base =
        (uint32_t)(order - 1U) *
        FAT_LFN_CHARS_PER_ENTRY;

    for (
        uint32_t index = 0;
        index < FAT_LFN_CHARS_PER_ENTRY;
        index++
    )
    {
        uint32_t position = base + index;
        uint16_t character;

        if (position < length)
        {
            character =
                (uint8_t)name[position];
        }
        else if (position == length)
        {
            character = 0x0000U;
        }
        else
        {
            character = 0xFFFFU;
        }

        write_lfn_character(entry, index, character);
    }
}

static void build_short_entry(
    uint8_t entry[FAT_DIRECTORY_ENTRY_SIZE],
    const uint8_t alias[11],
    vfs_node_type_t type,
    uint32_t first_cluster,
    uint32_t size,
    fat_kind_t kind
)
{
    clear_bytes(entry, FAT_DIRECTORY_ENTRY_SIZE);
    copy_bytes(entry, alias, 11U);
    entry[11] =
        type == VFS_NODE_DIRECTORY ?
            FAT_ATTRIBUTE_DIRECTORY :
            FAT_ATTRIBUTE_ARCHIVE;

    if (kind == FAT_KIND_32)
    {
        write_u16(
            entry + 20,
            (uint16_t)(first_cluster >> 16)
        );
    }

    write_u16(
        entry + 26,
        (uint16_t)first_cluster
    );

    write_u32(
        entry + 28,
        type == VFS_NODE_FILE ? size : 0U
    );
}

static bool find_free_directory_run(
    fat_filesystem_t *filesystem,
    uint32_t parent_first_cluster,
    bool parent_fixed_root,
    uint16_t needed,
    uint32_t *start_slot
)
{
    if (
        filesystem == NULL ||
        needed == 0 ||
        start_slot == NULL
    )
    {
        return false;
    }

    uint32_t capacity = directory_slot_capacity(
        filesystem,
        parent_first_cluster,
        parent_fixed_root
    );

    uint32_t run_start = 0;
    uint16_t run = 0;

    for (uint32_t slot = 0; slot < capacity; slot++)
    {
        uint8_t entry[FAT_DIRECTORY_ENTRY_SIZE];

        if (!read_directory_entry(
                filesystem,
                parent_first_cluster,
                parent_fixed_root,
                slot,
                entry
            ))
        {
            return false;
        }

        bool free_entry =
            entry[0] == 0x00U ||
            entry[0] == 0xE5U;

        if (free_entry)
        {
            if (run == 0)
            {
                run_start = slot;
            }

            run++;

            if (run >= needed)
            {
                *start_slot = run_start;
                return true;
            }
        }
        else
        {
            run = 0;
        }
    }

    if (parent_fixed_root)
    {
        return false;
    }

    uint32_t entries_per_cluster =
        cluster_size_bytes(filesystem) /
        FAT_DIRECTORY_ENTRY_SIZE;

    uint32_t required_extra =
        needed > run ?
            (uint32_t)(needed - run) :
            0U;

    uint32_t extra_clusters =
        (required_extra + entries_per_cluster - 1U) /
        entries_per_cluster;

    if (extra_clusters == 0)
    {
        extra_clusters = 1;
    }

    uint32_t existing_clusters =
        cluster_chain_length(
            filesystem,
            parent_first_cluster
        );

    if (!resize_cluster_chain(
            filesystem,
            &parent_first_cluster,
            existing_clusters + extra_clusters
        ))
    {
        return false;
    }

    *start_slot =
        run > 0 ?
            capacity - run :
            capacity;

    return true;
}

static bool write_named_directory_entry(
    fat_filesystem_t *filesystem,
    uint32_t parent_first_cluster,
    bool parent_fixed_root,
    const char *name,
    vfs_node_type_t type,
    uint32_t first_cluster,
    uint32_t size,
    uint32_t *short_slot,
    uint32_t *lfn_start_slot,
    uint16_t *lfn_count
)
{
    uint8_t alias[11];
    bool needs_lfn;

    if (!build_short_alias(
            filesystem,
            parent_first_cluster,
            parent_fixed_root,
            name,
            alias,
            &needs_lfn
        ))
    {
        return false;
    }

    uint16_t count =
        needs_lfn ?
            lfn_entry_count_for_name(name) :
            0;

    uint32_t start;

    if (!find_free_directory_run(
            filesystem,
            parent_first_cluster,
            parent_fixed_root,
            (uint16_t)(count + 1U),
            &start
        ))
    {
        return false;
    }

    uint8_t checksum = short_name_checksum(alias);

    for (uint16_t index = 0; index < count; index++)
    {
        uint16_t order = (uint16_t)(count - index);
        uint8_t entry[FAT_DIRECTORY_ENTRY_SIZE];

        build_lfn_entry(
            entry,
            name,
            order,
            count,
            checksum
        );

        if (!write_directory_entry(
                filesystem,
                parent_first_cluster,
                parent_fixed_root,
                start + index,
                entry,
                true
            ))
        {
            return false;
        }
    }

    uint8_t short_entry[FAT_DIRECTORY_ENTRY_SIZE];

    build_short_entry(
        short_entry,
        alias,
        type,
        first_cluster,
        size,
        filesystem->kind
    );

    uint32_t short_position = start + count;

    if (!write_directory_entry(
            filesystem,
            parent_first_cluster,
            parent_fixed_root,
            short_position,
            short_entry,
            true
        ))
    {
        return false;
    }

    *short_slot = short_position;
    *lfn_start_slot =
        count > 0 ? start : FAT_INVALID_SLOT;
    *lfn_count = count;
    return true;
}

static bool mark_entry_set_deleted(
    fat_node_data_t *data
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

    uint32_t first_slot =
        data->lfn_count > 0 ?
            data->lfn_start_slot :
            data->short_slot;

    uint32_t last_slot = data->short_slot;

    for (
        uint32_t slot = first_slot;
        slot <= last_slot;
        slot++
    )
    {
        uint8_t entry[FAT_DIRECTORY_ENTRY_SIZE];

        if (!read_directory_entry(
                data->filesystem,
                data->parent_first_cluster,
                data->parent_fixed_root,
                slot,
                entry
            ))
        {
            return false;
        }

        entry[0] = 0xE5U;

        if (!write_directory_entry(
                data->filesystem,
                data->parent_first_cluster,
                data->parent_fixed_root,
                slot,
                entry,
                false
            ))
        {
            return false;
        }
    }

    return true;
}

static bool initialize_new_directory(
    fat_filesystem_t *filesystem,
    uint32_t cluster,
    uint32_t parent_cluster,
    bool parent_fixed_root
)
{
    if (!zero_cluster(filesystem, cluster))
    {
        return false;
    }

    uint8_t dot[FAT_DIRECTORY_ENTRY_SIZE];
    uint8_t dotdot[FAT_DIRECTORY_ENTRY_SIZE];
    uint8_t alias[11];

    clear_alias(alias);
    alias[0] = '.';
    build_short_entry(
        dot,
        alias,
        VFS_NODE_DIRECTORY,
        cluster,
        0,
        filesystem->kind
    );

    clear_alias(alias);
    alias[0] = '.';
    alias[1] = '.';
    build_short_entry(
        dotdot,
        alias,
        VFS_NODE_DIRECTORY,
        parent_fixed_root ? 0U : parent_cluster,
        0,
        filesystem->kind
    );

    return
        write_directory_entry(
            filesystem,
            cluster,
            false,
            0,
            dot,
            false
        ) &&
        write_directory_entry(
            filesystem,
            cluster,
            false,
            1,
            dotdot,
            false
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
        parent->filesystem_data == NULL ||
        name == NULL
    )
    {
        return NULL;
    }

    fat_node_data_t *parent_data =
        parent->filesystem_data;

    fat_filesystem_t *filesystem =
        parent_data->filesystem;

    if (
        filesystem == NULL ||
        !filesystem->writable ||
        filesystem->kind != FAT_KIND_16
    )
    {
        return NULL;
    }

    uint32_t first_cluster = 0;

    if (type == VFS_NODE_DIRECTORY)
    {
        first_cluster = allocate_cluster(filesystem);

        if (
            first_cluster == 0 ||
            !initialize_new_directory(
                filesystem,
                first_cluster,
                parent_data->first_cluster,
                parent == filesystem->root
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
    }

    uint32_t short_slot;
    uint32_t lfn_start_slot;
    uint16_t lfn_count;

    if (!write_named_directory_entry(
            filesystem,
            parent_data->first_cluster,
            parent == filesystem->root,
            name,
            type,
            first_cluster,
            0,
            &short_slot,
            &lfn_start_slot,
            &lfn_count
        ))
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

    vfs_node_t *node = allocate_node(
        filesystem,
        name,
        type,
        first_cluster,
        0,
        parent_data->first_cluster,
        parent == filesystem->root,
        short_slot,
        lfn_start_slot,
        lfn_count
    );

    if (
        node == NULL ||
        !vfs_add_child(parent, node)
    )
    {
        if (node != NULL)
        {
            fat_node_data_t *node_data =
                node->filesystem_data;
            (void)mark_entry_set_deleted(node_data);
            kfree(node_data);
            kfree(node);
        }

        if (first_cluster != 0)
        {
            (void)free_cluster_chain(
                filesystem,
                first_cluster
            );
        }

        return NULL;
    }

    filesystem->create_operations++;

    if (lfn_count > 0)
    {
        filesystem->long_names++;
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

    fat_node_data_t *data = node->filesystem_data;
    fat_filesystem_t *filesystem = data->filesystem;

    if (
        filesystem == NULL ||
        !filesystem->writable ||
        node == filesystem->root ||
        (
            node->type == VFS_NODE_DIRECTORY &&
            node->first_child != NULL
        )
    )
    {
        return false;
    }

    if (!mark_entry_set_deleted(data))
    {
        return false;
    }

    if (
        data->first_cluster != 0 &&
        !free_cluster_chain(
            filesystem,
            data->first_cluster
        )
    )
    {
        return false;
    }

    if (data->lfn_count > 0 && filesystem->long_names > 0)
    {
        filesystem->long_names--;
    }

    if (node->type == VFS_NODE_DIRECTORY)
    {
        if (filesystem->directories > 0)
        {
            filesystem->directories--;
        }
    }
    else if (filesystem->files > 0)
    {
        filesystem->files--;
    }

    if (filesystem->nodes > 0)
    {
        filesystem->nodes--;
    }

    filesystem->remove_operations++;
    kfree(data);
    node->filesystem_data = NULL;
    return true;
}

static bool update_directory_parent_entry(
    fat_filesystem_t *filesystem,
    uint32_t directory_cluster,
    uint32_t new_parent_cluster,
    bool new_parent_fixed_root
)
{
    uint8_t entry[FAT_DIRECTORY_ENTRY_SIZE];

    if (!read_directory_entry(
            filesystem,
            directory_cluster,
            false,
            1,
            entry
        ))
    {
        return false;
    }

    uint32_t cluster =
        new_parent_fixed_root ?
            0U :
            new_parent_cluster;

    if (filesystem->kind == FAT_KIND_32)
    {
        write_u16(
            entry + 20,
            (uint16_t)(cluster >> 16)
        );
    }

    write_u16(entry + 26, (uint16_t)cluster);

    return write_directory_entry(
        filesystem,
        directory_cluster,
        false,
        1,
        entry,
        false
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
        node->filesystem_data == NULL ||
        new_parent->filesystem_data == NULL
    )
    {
        return false;
    }

    fat_node_data_t *data = node->filesystem_data;
    fat_node_data_t *parent_data =
        new_parent->filesystem_data;

    fat_filesystem_t *filesystem = data->filesystem;

    if (
        filesystem == NULL ||
        filesystem != parent_data->filesystem ||
        !filesystem->writable ||
        filesystem->kind != FAT_KIND_16 ||
        node == filesystem->root
    )
    {
        return false;
    }

    uint32_t new_short_slot;
    uint32_t new_lfn_start;
    uint16_t new_lfn_count;
    bool new_parent_fixed =
        new_parent == filesystem->root;

    if (!write_named_directory_entry(
            filesystem,
            parent_data->first_cluster,
            new_parent_fixed,
            new_name,
            node->type,
            data->first_cluster,
            (uint32_t)node->size,
            &new_short_slot,
            &new_lfn_start,
            &new_lfn_count
        ))
    {
        return false;
    }

    bool parent_changed =
        data->parent_first_cluster !=
            parent_data->first_cluster ||
        data->parent_fixed_root !=
            new_parent_fixed;

    if (
        node->type == VFS_NODE_DIRECTORY &&
        parent_changed &&
        !update_directory_parent_entry(
            filesystem,
            data->first_cluster,
            parent_data->first_cluster,
            new_parent_fixed
        )
    )
    {
        fat_node_data_t temporary = *data;
        temporary.parent_first_cluster =
            parent_data->first_cluster;
        temporary.parent_fixed_root =
            new_parent_fixed;
        temporary.short_slot = new_short_slot;
        temporary.lfn_start_slot = new_lfn_start;
        temporary.lfn_count = new_lfn_count;
        (void)mark_entry_set_deleted(&temporary);
        return false;
    }

    uint16_t old_lfn_count = data->lfn_count;

    if (!mark_entry_set_deleted(data))
    {
        return false;
    }

    data->parent_first_cluster =
        parent_data->first_cluster;
    data->parent_fixed_root = new_parent_fixed;
    data->short_slot = new_short_slot;
    data->lfn_start_slot = new_lfn_start;
    data->lfn_count = new_lfn_count;

    if (old_lfn_count == 0 && new_lfn_count > 0)
    {
        filesystem->long_names++;
    }
    else if (
        old_lfn_count > 0 &&
        new_lfn_count == 0 &&
        filesystem->long_names > 0
    )
    {
        filesystem->long_names--;
    }

    filesystem->move_operations++;
    return true;
}

static bool fat_metadata(vfs_node_t *node)
{
    return node != NULL;
}

static void extract_volume_label(
    fat_filesystem_t *filesystem,
    const uint8_t *boot_sector
)
{
    uint32_t offset =
        filesystem->kind == FAT_KIND_32 ?
            71U :
            43U;

    size_t length = 11;

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
        const char fallback[] = "NO NAME";
        size_t index = 0;

        while (fallback[index] != '\0')
        {
            filesystem->volume_label[index] =
                fallback[index];
            index++;
        }

        filesystem->volume_label[index] = '\0';
    }
}

static bool initialize_filesystem(
    fat_filesystem_t *filesystem,
    const block_device_t *device
)
{
    uint8_t *boot_sector =
        kmalloc(device->sector_size);

    if (boot_sector == NULL)
    {
        return false;
    }

    if (
        !block_device_read(device, 0, 1, boot_sector) ||
        device->sector_size < 512U ||
        boot_sector[510] != 0x55U ||
        boot_sector[511] != 0xAAU
    )
    {
        kfree(boot_sector);
        return false;
    }

    uint16_t bytes_per_sector =
        read_u16(boot_sector + 11);
    uint8_t sectors_per_cluster = boot_sector[13];
    uint16_t reserved_sectors =
        read_u16(boot_sector + 14);
    uint8_t fat_count = boot_sector[16];
    uint16_t root_entry_count =
        read_u16(boot_sector + 17);
    uint32_t total_sectors =
        read_u16(boot_sector + 19);

    if (total_sectors == 0)
    {
        total_sectors = read_u32(boot_sector + 32);
    }

    uint32_t fat_size = read_u16(boot_sector + 22);

    if (fat_size == 0)
    {
        fat_size = read_u32(boot_sector + 36);
    }

    if (
        bytes_per_sector != device->sector_size ||
        bytes_per_sector < 512U ||
        sectors_per_cluster == 0 ||
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
            (uint32_t)root_entry_count * 32U +
            (bytes_per_sector - 1U)
        ) / bytes_per_sector;

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
        (kind == FAT_KIND_16 && root_entry_count == 0) ||
        (kind == FAT_KIND_32 && root_entry_count != 0)
    )
    {
        kfree(boot_sector);
        return false;
    }

    clear_bytes(filesystem, sizeof(*filesystem));
    filesystem->device = device;
    filesystem->kind = kind;
    filesystem->writable =
        device->writable &&
        device->write != NULL &&
        kind == FAT_KIND_16;
    filesystem->bytes_per_sector = bytes_per_sector;
    filesystem->sectors_per_cluster =
        sectors_per_cluster;
    filesystem->reserved_sectors = reserved_sectors;
    filesystem->fat_count = fat_count;
    filesystem->root_entry_count = root_entry_count;
    filesystem->total_sectors = total_sectors;
    filesystem->fat_size_sectors = fat_size;
    filesystem->root_directory_sectors =
        root_directory_sectors;
    filesystem->first_fat_sector = reserved_sectors;
    filesystem->first_root_sector =
        reserved_sectors +
        (uint32_t)fat_count * fat_size;
    filesystem->first_data_sector = first_data_sector;
    filesystem->cluster_count = cluster_count;
    filesystem->root_cluster =
        kind == FAT_KIND_32 ?
            read_u32(boot_sector + 44) :
            0;

    extract_volume_label(filesystem, boot_sector);
    kfree(boot_sector);

    return
        kind == FAT_KIND_16 ||
        cluster_valid(
            filesystem,
            filesystem->root_cluster
        );
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

    uint32_t index = 0;

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

static bool copy_mount_path(
    char destination[VFS_PATH_MAX],
    const char *source
)
{
    if (
        source == NULL ||
        source[0] != '/'
    )
    {
        return false;
    }

    uint32_t index = 0;

    while (source[index] != '\0')
    {
        if (index + 1U >= VFS_PATH_MAX)
        {
            return false;
        }

        destination[index] = source[index];
        index++;
    }

    destination[index] = '\0';
    return index > 1U;
}

static const char *mount_leaf_name(
    const char *mount_path
)
{
    const char *leaf = mount_path;

    for (uint32_t index = 0; mount_path[index] != '\0'; index++)
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
    if (
        mounted_filesystem != NULL ||
        device == NULL ||
        mount_path == NULL
    )
    {
        return false;
    }

    if (vfs_open("/media") == NULL)
    {
        if (!vfs_make_directory("/media"))
        {
            return false;
        }
    }

    fat_filesystem_t *filesystem =
        kmalloc(sizeof(fat_filesystem_t));

    if (filesystem == NULL)
    {
        return false;
    }

    if (
        !initialize_filesystem(filesystem, device) ||
        !copy_mount_path(
            filesystem->mount_path,
            mount_path
        )
    )
    {
        kfree(filesystem);
        return false;
    }

    vfs_node_t *root = allocate_node(
        filesystem,
        mount_leaf_name(mount_path),
        VFS_NODE_DIRECTORY,
        filesystem->root_cluster,
        0,
        0,
        true,
        FAT_INVALID_SLOT,
        FAT_INVALID_SLOT,
        0
    );

    if (root == NULL)
    {
        kfree(filesystem);
        return false;
    }

    filesystem->root = root;

    if (
        !parse_directory(
            filesystem,
            root,
            filesystem->root_cluster,
            filesystem->kind == FAT_KIND_16,
            0
        ) ||
        !vfs_mount_at(mount_path, root)
    )
    {
        free_node_tree(root);
        kfree(filesystem);
        return false;
    }

    mounted_filesystem = filesystem;
    return true;
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
        return false;
    }

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
            ) &&
            mount_device_at(device, "/media/usb")
        )
        {
            return true;
        }
    }

    return false;
}

bool fat_fs_mount_first_sata(void)
{
    if (mounted_filesystem != NULL)
    {
        return false;
    }

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
                "AHCI partition"
            ) &&
            mount_device_at(device, "/media/sata")
        )
        {
            return true;
        }
    }

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
                "AHCI SATA"
            ) &&
            mount_device_at(device, "/media/sata")
        )
        {
            return true;
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

    /* USB block writes issue SCSI SYNCHRONIZE CACHE(10). */
    mounted_filesystem->dirty = false;
    mounted_filesystem->sync_operations++;
    return true;
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
        return;
    }

    const char *mode;

    if (mounted_filesystem->writable)
    {
        mode = "read-write complete";
    }
    else
    {
        mode = "read-only";
    }

    kprintf(
        "FAT filesystem: FAT%u %s\n",
        mounted_filesystem->kind == FAT_KIND_16 ?
            16U :
            32U,
        mode
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
            cluster_size_bytes(mounted_filesystem),
        (unsigned int)
            mounted_filesystem->cluster_count,
        mounted_filesystem->dirty ? "yes" : "no"
    );

    kprintf(
        "Mounted at %s files=%u directories=%u long-names=%u\n",
        mounted_filesystem->mount_path,
        (unsigned int)mounted_filesystem->files,
        (unsigned int)(
            mounted_filesystem->directories > 0 ?
                mounted_filesystem->directories - 1U :
                0U
        ),
        (unsigned int)mounted_filesystem->long_names
    );

    kprintf(
        "Operations: writes=%u creates=%u removes=%u moves=%u syncs=%u\n",
        (unsigned int)mounted_filesystem->write_operations,
        (unsigned int)mounted_filesystem->create_operations,
        (unsigned int)mounted_filesystem->remove_operations,
        (unsigned int)mounted_filesystem->move_operations,
        (unsigned int)mounted_filesystem->sync_operations
    );

    kprintf(
        "Clusters: allocated=%u freed=%u\n",
        (unsigned int)mounted_filesystem->allocated_clusters,
        (unsigned int)mounted_filesystem->freed_clusters
    );
}

bool fat_fs_run_write_test(void)
{
    if (
        mounted_filesystem == NULL ||
        !mounted_filesystem->writable
    )
    {
        return false;
    }

    (void)vfs_remove("/media/usb/latteros test", true);
    (void)vfs_remove("/media/usb/archive", true);

    if (!vfs_make_directory(
            "/media/usb/latteros test"
        ))
    {
        return false;
    }

    const char *source_path =
        "/media/usb/latteros test/created long document.txt";

    if (!vfs_create_file(source_path))
    {
        return false;
    }

    vfs_node_t *file = vfs_open(source_path);

    if (file == NULL)
    {
        return false;
    }

    uint8_t buffer[FAT_TEST_BUFFER_SIZE];

    for (
        uint32_t index = 0;
        index < FAT_TEST_BUFFER_SIZE;
        index++
    )
    {
        buffer[index] =
            (uint8_t)('A' + index % 26U);
    }

    if (
        vfs_write(
            file,
            0,
            buffer,
            sizeof(buffer)
        ) != sizeof(buffer)
    )
    {
        return false;
    }

    if (!vfs_rename(
            source_path,
            "renamed long document.txt"
        ))
    {
        return false;
    }

    if (!vfs_make_directory("/media/usb/archive"))
    {
        return false;
    }

    const char *renamed =
        "/media/usb/latteros test/renamed long document.txt";

    if (!vfs_move(renamed, "/media/usb/archive"))
    {
        return false;
    }

    const char *moved =
        "/media/usb/archive/renamed long document.txt";

    file = vfs_open(moved);

    if (file == NULL || file->size != FAT_TEST_BUFFER_SIZE)
    {
        return false;
    }

    uint8_t verification[64];

    if (
        vfs_read(
            file,
            0,
            verification,
            sizeof(verification)
        ) != sizeof(verification)
    )
    {
        return false;
    }

    for (
        uint32_t index = 0;
        index < sizeof(verification);
        index++
    )
    {
        if (
            verification[index] !=
            (uint8_t)('A' + index % 26U)
        )
        {
            return false;
        }
    }

    if (
        !vfs_remove("/media/usb/latteros test", true) ||
        !vfs_remove("/media/usb/archive", true)
    )
    {
        return false;
    }

    return fat_fs_sync();
}
