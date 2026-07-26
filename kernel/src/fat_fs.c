#include "fat_fs.h"

#include "block_device.h"
#include "heap.h"
#include "kstdio.h"
#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FAT_MAX_NODES 256U
#define FAT_MAX_DEPTH 12U
#define FAT_ATTRIBUTE_DIRECTORY 0x10U
#define FAT_ATTRIBUTE_VOLUME_ID 0x08U
#define FAT_ATTRIBUTE_LONG_NAME 0x0FU
#define FAT16_END_OF_CHAIN 0xFFF8U
#define FAT32_END_OF_CHAIN 0x0FFFFFF8U
#define FAT_LFN_LAST_ENTRY 0x40U
#define FAT_LFN_ORDER_MASK 0x1FU
#define FAT_LFN_CHARS_PER_ENTRY 13U

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
    uint32_t directory_sector;
    uint16_t directory_offset;
} fat_node_data_t;

typedef struct
{
    char name[VFS_NAME_MAX + 1];
    uint8_t checksum;
    uint8_t expected_order;
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
    bool writable;

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
static bool fat_metadata(vfs_node_t *node);

static const vfs_operations_t fat_operations = {
    .read = fat_read,
    .write = fat_write,
    .truncate = fat_truncate,
    .create = NULL,
    .remove = NULL,
    .move = NULL,
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

static void write_u32(
    uint8_t *bytes,
    uint32_t value
)
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

static bool copy_node_name(
    char *destination,
    const char *source
);

static char lower_ascii(char character)
{
    if (character >= 'A' && character <= 'Z')
    {
        return (char)(character + ('a' - 'A'));
    }

    return character;
}
static void reset_lfn(fat_lfn_state_t *state)
{
    if (state == NULL)
    {
        return;
    }

    clear_bytes(state, sizeof(*state));
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

static void process_lfn_entry(
    fat_lfn_state_t *state,
    const uint8_t *entry
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
    char *name
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

    reset_lfn(state);
    return copied;
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
    return
        filesystem != NULL &&
        filesystem->writable &&
        buffer != NULL &&
        sector < filesystem->total_sectors &&
        block_device_write(
            filesystem->device,
            sector,
            1,
            buffer
        );
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

static uint32_t next_cluster(
    const fat_filesystem_t *filesystem,
    uint32_t cluster
)
{
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

    uint32_t value;

    if (filesystem->kind == FAT_KIND_16)
    {
        value = read_u16(buffer + offset);
    }
    else
    {
        value = read_u32(buffer + offset) & 0x0FFFFFFFU;
    }

    kfree(buffer);
    return value;
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
    uint32_t directory_sector,
    uint16_t directory_offset
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
    data->directory_sector = directory_sector;
    data->directory_offset = directory_offset;

    node->type = type;
    node->size = size;
    node->operations = &fat_operations;
    node->filesystem_data = data;
    node->owner_uid = 0;
    node->owner_gid = 0;
    node->mode =
        type == VFS_NODE_DIRECTORY ?
            0555U :
            (
                filesystem->writable ?
                    0666U :
                    0444U
            );

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
);

static bool process_directory_sector(
    fat_filesystem_t *filesystem,
    vfs_node_t *parent,
    const uint8_t *sector,
    uint32_t sector_number,
    uint32_t depth,
    bool *reached_end,
    fat_lfn_state_t *lfn
)
{
    uint32_t entries =
        filesystem->bytes_per_sector / 32U;

    for (uint32_t index = 0; index < entries; index++)
    {
        const uint8_t *entry = sector + index * 32U;

        if (entry[0] == 0x00U)
        {
            reset_lfn(lfn);
            *reached_end = true;
            return true;
        }

        if (entry[0] == 0xE5U)
        {
            reset_lfn(lfn);
            continue;
        }

        if (entry[11] == FAT_ATTRIBUTE_LONG_NAME)
        {
            process_lfn_entry(lfn, entry);
            continue;
        }

        if (
            (entry[11] & FAT_ATTRIBUTE_VOLUME_ID) != 0 ||
            is_dot_entry(entry)
        )
        {
            reset_lfn(lfn);
            continue;
        }

        char name[VFS_NAME_MAX + 1];
        bool long_name =
            consume_lfn_name(lfn, entry, name);

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

        uint32_t first_entry_cluster =
            (high_cluster << 16) |
            read_u16(entry + 26);

        uint32_t file_size = read_u32(entry + 28);

        vfs_node_t *node = allocate_node(
            filesystem,
            name,
            directory ?
                VFS_NODE_DIRECTORY :
                VFS_NODE_FILE,
            first_entry_cluster,
            file_size,
            sector_number,
            (uint16_t)(index * 32U)
        );

        if (node == NULL)
        {
            return false;
        }

        if (!vfs_add_child(parent, node))
        {
            return false;
        }

        if (
            directory &&
            cluster_valid(
                filesystem,
                first_entry_cluster
            ) &&
            depth < FAT_MAX_DEPTH
        )
        {
            if (
                !parse_directory(
                    filesystem,
                    node,
                    first_entry_cluster,
                    false,
                    depth + 1U
                )
            )
            {
                return false;
            }
        }
    }

    return true;
}

static bool parse_directory(
    fat_filesystem_t *filesystem,
    vfs_node_t *parent,
    uint32_t first_cluster,
    bool fixed_root,
    uint32_t depth
)
{
    uint8_t *sector =
        kmalloc(filesystem->bytes_per_sector);

    if (sector == NULL)
    {
        return false;
    }

    bool reached_end = false;
    bool success = true;
    fat_lfn_state_t lfn;
    reset_lfn(&lfn);

    if (fixed_root)
    {
        for (
            uint32_t index = 0;
            index < filesystem->root_directory_sectors &&
                !reached_end;
            index++
        )
        {
            if (
                !read_sector(
                    filesystem,
                    filesystem->first_root_sector + index,
                    sector
                ) ||
                !process_directory_sector(
                    filesystem,
                    parent,
                    sector,
                    filesystem->first_root_sector + index,
                    depth,
                    &reached_end,
                    &lfn
                )
            )
            {
                success = false;
                break;
            }
        }
    }
    else
    {
        uint32_t cluster = first_cluster;
        uint32_t visited = 0;

        while (
            cluster_valid(filesystem, cluster) &&
            !cluster_is_end(filesystem, cluster) &&
            !reached_end &&
            visited <= filesystem->cluster_count
        )
        {
            uint32_t first_sector =
                cluster_to_sector(
                    filesystem,
                    cluster
                );

            for (
                uint32_t index = 0;
                index < filesystem->sectors_per_cluster &&
                    !reached_end;
                index++
            )
            {
                if (
                    !read_sector(
                        filesystem,
                        first_sector + index,
                        sector
                    ) ||
                    !process_directory_sector(
                        filesystem,
                        parent,
                        sector,
                        first_sector + index,
                        depth,
                        &reached_end,
                        &lfn
                    )
                )
                {
                    success = false;
                    break;
                }
            }

            if (!success || reached_end)
            {
                break;
            }

            uint32_t next =
                next_cluster(filesystem, cluster);

            if (
                next == 0 ||
                cluster_is_end(filesystem, next)
            )
            {
                break;
            }

            cluster = next;
            visited++;
        }
    }

    kfree(sector);
    return success;
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
        !cluster_valid(
            filesystem,
            data->first_cluster
        )
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
        (uint32_t)filesystem->bytes_per_sector *
        filesystem->sectors_per_cluster;

    uint32_t skip_clusters =
        (uint32_t)(offset / cluster_size);

    uint32_t cluster = data->first_cluster;

    for (
        uint32_t index = 0;
        index < skip_clusters;
        index++
    )
    {
        cluster = next_cluster(filesystem, cluster);

        if (
            !cluster_valid(filesystem, cluster) ||
            cluster_is_end(filesystem, cluster)
        )
        {
            return 0;
        }
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
        cluster_valid(filesystem, cluster) &&
        !cluster_is_end(filesystem, cluster)
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
                cluster_to_sector(
                    filesystem,
                    cluster
                ) + sector_index;

            if (
                !read_sector(
                    filesystem,
                    sector_number,
                    sector
                )
            )
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

        cluster = next_cluster(filesystem, cluster);
        cluster_offset = 0;
    }

    kfree(sector);
    return completed;
}

static size_t cluster_chain_capacity(
    const fat_filesystem_t *filesystem,
    uint32_t first_cluster
)
{
    if (
        filesystem == NULL ||
        !cluster_valid(
            filesystem,
            first_cluster
        )
    )
    {
        return 0;
    }

    size_t cluster_size =
        (size_t)filesystem->bytes_per_sector *
        filesystem->sectors_per_cluster;

    size_t capacity = 0;
    uint32_t cluster = first_cluster;
    uint32_t visited = 0;

    while (
        cluster_valid(filesystem, cluster) &&
        !cluster_is_end(filesystem, cluster) &&
        visited <= filesystem->cluster_count
    )
    {
        if (capacity > SIZE_MAX - cluster_size)
        {
            return 0;
        }

        capacity += cluster_size;

        uint32_t next =
            next_cluster(filesystem, cluster);

        if (
            next == 0 ||
            cluster_is_end(filesystem, next)
        )
        {
            break;
        }

        cluster = next;
        visited++;
    }

    return capacity;
}

static bool update_directory_size(
    vfs_node_t *node,
    uint32_t size
)
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

    fat_filesystem_t *filesystem =
        data->filesystem;

    if (
        filesystem == NULL ||
        !filesystem->writable ||
        data->directory_sector == UINT32_MAX ||
        data->directory_offset >
            filesystem->bytes_per_sector - 32U
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

    bool success =
        read_sector(
            filesystem,
            data->directory_sector,
            sector
        );

    if (success)
    {
        write_u32(
            sector +
                data->directory_offset +
                28U,
            size
        );

        success = write_sector(
            filesystem,
            data->directory_sector,
            sector
        );
    }

    kfree(sector);
    return success;
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
        offset > node->size
    )
    {
        return 0;
    }

    fat_node_data_t *data =
        node->filesystem_data;

    fat_filesystem_t *filesystem =
        data->filesystem;

    if (
        filesystem == NULL ||
        !filesystem->writable ||
        !cluster_valid(
            filesystem,
            data->first_cluster
        )
    )
    {
        return 0;
    }

    size_t capacity = cluster_chain_capacity(
        filesystem,
        data->first_cluster
    );

    if (
        capacity == 0 ||
        offset > capacity ||
        count > capacity - offset ||
        offset + count > UINT32_MAX
    )
    {
        return 0;
    }

    uint32_t cluster_size =
        (uint32_t)filesystem->bytes_per_sector *
        filesystem->sectors_per_cluster;

    uint32_t skip_clusters =
        (uint32_t)(offset / cluster_size);

    uint32_t cluster = data->first_cluster;

    for (
        uint32_t index = 0;
        index < skip_clusters;
        index++
    )
    {
        cluster = next_cluster(
            filesystem,
            cluster
        );

        if (
            !cluster_valid(filesystem, cluster) ||
            cluster_is_end(filesystem, cluster)
        )
        {
            return 0;
        }
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
        cluster_valid(filesystem, cluster) &&
        !cluster_is_end(filesystem, cluster)
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
                cluster_to_sector(
                    filesystem,
                    cluster
                ) + sector_index;

            if (
                !read_sector(
                    filesystem,
                    sector_number,
                    sector
                )
            )
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

            if (
                !write_sector(
                    filesystem,
                    sector_number,
                    sector
                )
            )
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

        cluster = next_cluster(
            filesystem,
            cluster
        );

        cluster_offset = 0;
    }

    kfree(sector);

    size_t new_size = offset + completed;

    if (
        completed > 0 &&
        new_size > node->size
    )
    {
        if (
            !update_directory_size(
                node,
                (uint32_t)new_size
            )
        )
        {
            return 0;
        }

        node->size = new_size;
    }

    if (completed > 0)
    {
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

    fat_node_data_t *data =
        node->filesystem_data;

    fat_filesystem_t *filesystem =
        data->filesystem;

    if (
        filesystem == NULL ||
        !filesystem->writable ||
        !update_directory_size(node, 0)
    )
    {
        return false;
    }

    node->size = 0;
    filesystem->write_operations++;
    return true;
}

static bool fat_metadata(vfs_node_t *node)
{
    (void)node;
    return false;
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
        boot_sector[offset + length - 1] == ' '
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
        !block_device_read(
            device,
            0,
            1,
            boot_sector
        ) ||
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
        device->write != NULL;
    filesystem->bytes_per_sector = bytes_per_sector;
    filesystem->sectors_per_cluster =
        sectors_per_cluster;
    filesystem->reserved_sectors =
        reserved_sectors;
    filesystem->fat_count = fat_count;
    filesystem->root_entry_count =
        root_entry_count;
    filesystem->total_sectors = total_sectors;
    filesystem->fat_size_sectors = fat_size;
    filesystem->root_directory_sectors =
        root_directory_sectors;
    filesystem->first_fat_sector =
        reserved_sectors;
    filesystem->first_root_sector =
        reserved_sectors +
        (uint32_t)fat_count * fat_size;
    filesystem->first_data_sector =
        first_data_sector;
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

bool fat_fs_mount_first_usb(void)
{
    if (mounted_filesystem != NULL)
    {
        return true;
    }

    if (vfs_open("/media") == NULL)
    {
        if (!vfs_make_directory("/media"))
        {
            return false;
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
            device == NULL ||
            device == block_device_primary()
        )
        {
            continue;
        }

        fat_filesystem_t *filesystem =
            kmalloc(sizeof(fat_filesystem_t));

        if (filesystem == NULL)
        {
            return false;
        }

        if (!initialize_filesystem(filesystem, device))
        {
            kfree(filesystem);
            continue;
        }

        vfs_node_t *root = allocate_node(
            filesystem,
            "usb",
            VFS_NODE_DIRECTORY,
            filesystem->root_cluster,
            0,
            UINT32_MAX,
            0
        );

        if (root == NULL)
        {
            kfree(filesystem);
            return false;
        }

        filesystem->root = root;

        bool fixed_root =
            filesystem->kind == FAT_KIND_16;

        if (
            !parse_directory(
                filesystem,
                root,
                filesystem->root_cluster,
                fixed_root,
                0
            ) ||
            !vfs_mount_at("/media/usb", root)
        )
        {
            return false;
        }

        mounted_filesystem = filesystem;
        return true;
    }

    return false;
}

bool fat_fs_mounted(void)
{
    return mounted_filesystem != NULL;
}

void fat_fs_print_status(void)
{
    if (mounted_filesystem == NULL)
    {
        kprintf("FAT filesystem: not mounted\n");
        return;
    }

    kprintf(
        "FAT filesystem: FAT%u %s\n",
        mounted_filesystem->kind == FAT_KIND_16 ?
            16U :
            32U,
        mounted_filesystem->writable ?
            "read-write (existing files)" :
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
        "Sector=%u cluster=%u bytes clusters=%u\n",
        (unsigned int)
            mounted_filesystem->bytes_per_sector,
        (unsigned int)(
            mounted_filesystem->bytes_per_sector *
            mounted_filesystem->sectors_per_cluster
        ),
        (unsigned int)
            mounted_filesystem->cluster_count
    );

    kprintf(
        "Mounted at /media/usb files=%u directories=%u long-names=%u writes=%u\n",
        (unsigned int)mounted_filesystem->files,
        (unsigned int)(
            mounted_filesystem->directories > 0 ?
                mounted_filesystem->directories - 1U :
                0U
        ),
        (unsigned int)mounted_filesystem->long_names,
        (unsigned int)mounted_filesystem->write_operations
    );
}
