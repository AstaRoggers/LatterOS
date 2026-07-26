#include "latteros_fs.h"

#include "heap.h"
#include "kstdio.h"
#include "partition.h"
#include "terminal.h"
#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LATTEROS_FS_VERSION          1U
#define LATTEROS_FS_ENTRY_COUNT      128U
#define LATTEROS_FS_ENTRY_SIZE       64U
#define LATTEROS_FS_METADATA_START   1U
#define LATTEROS_FS_METADATA_SECTORS 16U
#define LATTEROS_FS_DATA_START       17U
#define LATTEROS_FS_SECTORS_PER_FILE 8U
#define LATTEROS_FS_FILE_CAPACITY    4096U
#define LATTEROS_FS_ROOT_INDEX       0U
#define LATTEROS_FS_NO_PARENT        UINT32_MAX

#define LATTEROS_FS_ENTRY_UNUSED     0U
#define LATTEROS_FS_ENTRY_FILE       1U
#define LATTEROS_FS_ENTRY_DIRECTORY  2U

typedef struct __attribute__((packed))
{
    char magic[8];
    uint32_t version;
    uint32_t entry_count;
    uint32_t metadata_start;
    uint32_t metadata_sectors;
    uint32_t data_start;
    uint32_t sectors_per_file;
    uint32_t root_entry;
    uint8_t reserved[476];
} latteros_fs_superblock_t;

typedef struct __attribute__((packed))
{
    uint8_t used;
    uint8_t type;
    uint16_t reserved0;
    uint32_t parent_index;
    uint32_t size;
    uint32_t generation;
    char name[32];
    uint8_t reserved1[16];
} latteros_fs_disk_entry_t;

typedef struct
{
    uint32_t entry_index;
} latteros_fs_node_data_t;

static const char latteros_fs_magic[8] = {
    'L', 'O', 'S', 'F', 'S', '0', '1', '\0'
};

/* Accept disks created by the earlier development name without reformatting. */
static const char legacy_latterfs_magic[8] = {
    'L', 'A', 'T', 'T', 'E', 'R', '1', '\0'
};

static const partition_t *mounted_partition;
static latteros_fs_superblock_t superblock;
static latteros_fs_disk_entry_t entries[
    LATTEROS_FS_ENTRY_COUNT
];
static vfs_node_t *nodes[LATTEROS_FS_ENTRY_COUNT];
static uint8_t sector_buffer[512];
static bool mounted;
static bool formatted_on_boot;

static size_t latteros_fs_read(
    vfs_node_t *node,
    size_t offset,
    void *buffer,
    size_t count
);

static size_t latteros_fs_write(
    vfs_node_t *node,
    size_t offset,
    const void *buffer,
    size_t count
);

static bool latteros_fs_truncate(
    vfs_node_t *node
);

static vfs_node_t *latteros_fs_create(
    vfs_node_t *parent,
    const char *name,
    vfs_node_type_t type
);

static bool latteros_fs_remove(
    vfs_node_t *node
);

static bool latteros_fs_move(
    vfs_node_t *node,
    vfs_node_t *new_parent,
    const char *new_name
);

static const vfs_operations_t latteros_fs_operations = {
    .read = latteros_fs_read,
    .write = latteros_fs_write,
    .truncate = latteros_fs_truncate,
    .create = latteros_fs_create,
    .remove = latteros_fs_remove,
    .move = latteros_fs_move
};

_Static_assert(
    sizeof(latteros_fs_superblock_t) == 512,
    "LatterOS FS superblock must be one sector"
);

_Static_assert(
    sizeof(latteros_fs_disk_entry_t) ==
        LATTEROS_FS_ENTRY_SIZE,
    "LatterOS FS entry must be 64 bytes"
);

static void clear_bytes(
    void *pointer,
    size_t count
)
{
    uint8_t *bytes = pointer;

    for (
        size_t index = 0;
        index < count;
        index++
    )
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

    for (
        size_t index = 0;
        index < count;
        index++
    )
    {
        output[index] = input[index];
    }
}

static bool bytes_equal(
    const void *first,
    const void *second,
    size_t count
)
{
    const uint8_t *left = first;
    const uint8_t *right = second;

    for (
        size_t index = 0;
        index < count;
        index++
    )
    {
        if (left[index] != right[index])
        {
            return false;
        }
    }

    return true;
}

static bool copy_name(
    char *destination,
    const char *source
)
{
    if (
        destination == NULL ||
        source == NULL
    )
    {
        return false;
    }

    size_t index = 0;

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

static uint64_t entry_data_lba(
    uint32_t entry_index
)
{
    return (
        superblock.data_start +
        (uint64_t)entry_index *
            superblock.sectors_per_file
    );
}

static bool write_entry(
    uint32_t entry_index
)
{
    if (
        mounted_partition == NULL ||
        entry_index >=
            LATTEROS_FS_ENTRY_COUNT
    )
    {
        return false;
    }

    uint32_t entries_per_sector =
        512U / LATTEROS_FS_ENTRY_SIZE;

    uint32_t sector_index =
        entry_index /
        entries_per_sector;

    uint32_t sector_entry =
        entry_index %
        entries_per_sector;

    uint64_t lba =
        superblock.metadata_start +
        sector_index;

    if (
        !partition_read(
            mounted_partition,
            lba,
            1,
            sector_buffer
        )
    )
    {
        return false;
    }

    copy_bytes(
        &sector_buffer[
            sector_entry *
            LATTEROS_FS_ENTRY_SIZE
        ],
        &entries[entry_index],
        sizeof(latteros_fs_disk_entry_t)
    );

    return partition_write(
        mounted_partition,
        lba,
        1,
        sector_buffer
    );
}

static bool load_entries(void)
{
    if (
        !partition_read(
            mounted_partition,
            superblock.metadata_start,
            superblock.metadata_sectors,
            entries
        )
    )
    {
        return false;
    }

    return true;
}

static bool format_filesystem(void)
{
    if (
        mounted_partition == NULL ||
        mounted_partition->sector_count <=
            LATTEROS_FS_DATA_START +
            LATTEROS_FS_ENTRY_COUNT *
                LATTEROS_FS_SECTORS_PER_FILE
    )
    {
        return false;
    }

    clear_bytes(
        &superblock,
        sizeof(superblock)
    );

    copy_bytes(
        superblock.magic,
        latteros_fs_magic,
        sizeof(latteros_fs_magic)
    );

    superblock.version =
        LATTEROS_FS_VERSION;
    superblock.entry_count =
        LATTEROS_FS_ENTRY_COUNT;
    superblock.metadata_start =
        LATTEROS_FS_METADATA_START;
    superblock.metadata_sectors =
        LATTEROS_FS_METADATA_SECTORS;
    superblock.data_start =
        LATTEROS_FS_DATA_START;
    superblock.sectors_per_file =
        LATTEROS_FS_SECTORS_PER_FILE;
    superblock.root_entry =
        LATTEROS_FS_ROOT_INDEX;

    if (
        !partition_write(
            mounted_partition,
            0,
            1,
            &superblock
        )
    )
    {
        return false;
    }

    clear_bytes(entries, sizeof(entries));

    for (
        uint32_t sector = 0;
        sector <
            LATTEROS_FS_METADATA_SECTORS;
        sector++
    )
    {
        clear_bytes(
            sector_buffer,
            sizeof(sector_buffer)
        );

        if (
            !partition_write(
                mounted_partition,
                LATTEROS_FS_METADATA_START +
                    sector,
                1,
                sector_buffer
            )
        )
        {
            return false;
        }
    }

    entries[LATTEROS_FS_ROOT_INDEX].used = 1;
    entries[LATTEROS_FS_ROOT_INDEX].type =
        LATTEROS_FS_ENTRY_DIRECTORY;
    entries[LATTEROS_FS_ROOT_INDEX].parent_index =
        LATTEROS_FS_NO_PARENT;
    entries[LATTEROS_FS_ROOT_INDEX].generation = 1;

    if (!write_entry(LATTEROS_FS_ROOT_INDEX))
    {
        return false;
    }

    formatted_on_boot = true;
    return true;
}

static bool superblock_valid(void)
{
    return (
        (
            bytes_equal(
                superblock.magic,
                latteros_fs_magic,
                sizeof(latteros_fs_magic)
            ) ||
            bytes_equal(
                superblock.magic,
                legacy_latterfs_magic,
                sizeof(legacy_latterfs_magic)
            )
        ) &&
        superblock.version ==
            LATTEROS_FS_VERSION &&
        superblock.entry_count ==
            LATTEROS_FS_ENTRY_COUNT &&
        superblock.metadata_start ==
            LATTEROS_FS_METADATA_START &&
        superblock.metadata_sectors ==
            LATTEROS_FS_METADATA_SECTORS &&
        superblock.data_start ==
            LATTEROS_FS_DATA_START &&
        superblock.sectors_per_file ==
            LATTEROS_FS_SECTORS_PER_FILE &&
        superblock.root_entry ==
            LATTEROS_FS_ROOT_INDEX
    );
}

static vfs_node_t *allocate_node(
    uint32_t entry_index,
    const char *name,
    vfs_node_type_t type
)
{
    vfs_node_t *node =
        kmalloc(sizeof(vfs_node_t));

    if (node == NULL)
    {
        return NULL;
    }

    latteros_fs_node_data_t *data =
        kmalloc(
            sizeof(latteros_fs_node_data_t)
        );

    if (data == NULL)
    {
        kfree(node);
        return NULL;
    }

    clear_bytes(node, sizeof(vfs_node_t));

    if (!copy_name(node->name, name))
    {
        kfree(data);
        kfree(node);
        return NULL;
    }

    data->entry_index = entry_index;

    node->type = type;
    node->size = entries[entry_index].size;
    node->operations = &latteros_fs_operations;
    node->filesystem_data = data;

    return node;
}

static bool build_vfs_tree(void)
{
    clear_bytes(nodes, sizeof(nodes));

    for (
        uint32_t index = 0;
        index < LATTEROS_FS_ENTRY_COUNT;
        index++
    )
    {
        latteros_fs_disk_entry_t *entry =
            &entries[index];

        if (!entry->used)
        {
            continue;
        }

        vfs_node_type_t type;

        if (
            entry->type ==
            LATTEROS_FS_ENTRY_DIRECTORY
        )
        {
            type = VFS_NODE_DIRECTORY;
        }
        else if (
            entry->type ==
            LATTEROS_FS_ENTRY_FILE
        )
        {
            type = VFS_NODE_FILE;
        }
        else
        {
            return false;
        }

        const char *name =
            index == LATTEROS_FS_ROOT_INDEX ?
            "" : entry->name;

        nodes[index] =
            allocate_node(index, name, type);

        if (nodes[index] == NULL)
        {
            return false;
        }
    }

    if (
        nodes[LATTEROS_FS_ROOT_INDEX] == NULL ||
        nodes[LATTEROS_FS_ROOT_INDEX]->type !=
            VFS_NODE_DIRECTORY
    )
    {
        return false;
    }

    for (
        uint32_t index = 1;
        index < LATTEROS_FS_ENTRY_COUNT;
        index++
    )
    {
        if (nodes[index] == NULL)
        {
            continue;
        }

        uint32_t parent_index =
            entries[index].parent_index;

        if (
            parent_index >=
                LATTEROS_FS_ENTRY_COUNT ||
            nodes[parent_index] == NULL ||
            nodes[parent_index]->type !=
                VFS_NODE_DIRECTORY ||
            !vfs_add_child(
                nodes[parent_index],
                nodes[index]
            )
        )
        {
            return false;
        }
    }

    return vfs_mount_at(
        "/home",
        nodes[LATTEROS_FS_ROOT_INDEX]
    );
}

static int32_t free_entry_index(void)
{
    for (
        uint32_t index = 1;
        index < LATTEROS_FS_ENTRY_COUNT;
        index++
    )
    {
        if (!entries[index].used)
        {
            return (int32_t)index;
        }
    }

    return -1;
}

static latteros_fs_node_data_t *node_data(
    vfs_node_t *node
)
{
    if (
        node == NULL ||
        node->filesystem_data == NULL
    )
    {
        return NULL;
    }

    return node->filesystem_data;
}

static size_t latteros_fs_read(
    vfs_node_t *node,
    size_t offset,
    void *buffer,
    size_t count
)
{
    latteros_fs_node_data_t *data =
        node_data(node);

    if (
        data == NULL ||
        buffer == NULL ||
        node->type != VFS_NODE_FILE ||
        offset >= node->size
    )
    {
        return 0;
    }

    size_t available =
        node->size - offset;

    if (count > available)
    {
        count = available;
    }

    uint8_t *output = buffer;
    size_t completed = 0;

    while (completed < count)
    {
        size_t absolute =
            offset + completed;

        uint32_t sector_offset =
            (uint32_t)(absolute / 512);

        uint32_t byte_offset =
            (uint32_t)(absolute % 512);

        size_t chunk = 512 - byte_offset;

        if (chunk > count - completed)
        {
            chunk = count - completed;
        }

        if (
            !partition_read(
                mounted_partition,
                entry_data_lba(
                    data->entry_index
                ) + sector_offset,
                1,
                sector_buffer
            )
        )
        {
            break;
        }

        copy_bytes(
            &output[completed],
            &sector_buffer[byte_offset],
            chunk
        );

        completed += chunk;
    }

    return completed;
}

static size_t latteros_fs_write(
    vfs_node_t *node,
    size_t offset,
    const void *buffer,
    size_t count
)
{
    latteros_fs_node_data_t *data =
        node_data(node);

    if (
        data == NULL ||
        buffer == NULL ||
        node->type != VFS_NODE_FILE ||
        offset >= LATTEROS_FS_FILE_CAPACITY
    )
    {
        return 0;
    }

    size_t available =
        LATTEROS_FS_FILE_CAPACITY - offset;

    if (count > available)
    {
        count = available;
    }

    const uint8_t *input = buffer;
    size_t completed = 0;

    while (completed < count)
    {
        size_t absolute =
            offset + completed;

        uint32_t sector_offset =
            (uint32_t)(absolute / 512);

        uint32_t byte_offset =
            (uint32_t)(absolute % 512);

        size_t chunk = 512 - byte_offset;

        if (chunk > count - completed)
        {
            chunk = count - completed;
        }

        if (
            byte_offset != 0 ||
            chunk != 512
        )
        {
            if (
                !partition_read(
                    mounted_partition,
                    entry_data_lba(
                        data->entry_index
                    ) + sector_offset,
                    1,
                    sector_buffer
                )
            )
            {
                clear_bytes(
                    sector_buffer,
                    sizeof(sector_buffer)
                );
            }
        }

        if (
            byte_offset == 0 &&
            chunk == 512
        )
        {
            copy_bytes(
                sector_buffer,
                &input[completed],
                512
            );
        }
        else
        {
            copy_bytes(
                &sector_buffer[byte_offset],
                &input[completed],
                chunk
            );
        }

        if (
            !partition_write(
                mounted_partition,
                entry_data_lba(
                    data->entry_index
                ) + sector_offset,
                1,
                sector_buffer
            )
        )
        {
            break;
        }

        completed += chunk;
    }

    size_t end = offset + completed;

    if (end > node->size)
    {
        node->size = end;
        entries[data->entry_index].size =
            (uint32_t)node->size;
        entries[data->entry_index].generation++;
        (void)write_entry(data->entry_index);
    }

    return completed;
}

static bool latteros_fs_truncate(
    vfs_node_t *node
)
{
    latteros_fs_node_data_t *data =
        node_data(node);

    if (
        data == NULL ||
        node->type != VFS_NODE_FILE
    )
    {
        return false;
    }

    node->size = 0;
    entries[data->entry_index].size = 0;
    entries[data->entry_index].generation++;

    return write_entry(data->entry_index);
}

static vfs_node_t *latteros_fs_create(
    vfs_node_t *parent,
    const char *name,
    vfs_node_type_t type
)
{
    latteros_fs_node_data_t *parent_data =
        node_data(parent);

    if (
        parent_data == NULL ||
        name == NULL ||
        parent->type != VFS_NODE_DIRECTORY
    )
    {
        return NULL;
    }

    int32_t free_index =
        free_entry_index();

    if (free_index < 0)
    {
        return NULL;
    }

    uint32_t index =
        (uint32_t)free_index;

    clear_bytes(
        &entries[index],
        sizeof(latteros_fs_disk_entry_t)
    );

    entries[index].used = 1;
    entries[index].type =
        type == VFS_NODE_DIRECTORY ?
            LATTEROS_FS_ENTRY_DIRECTORY :
            LATTEROS_FS_ENTRY_FILE;
    entries[index].parent_index =
        parent_data->entry_index;
    entries[index].generation = 1;

    if (
        !copy_name(
            entries[index].name,
            name
        ) ||
        !write_entry(index)
    )
    {
        clear_bytes(
            &entries[index],
            sizeof(latteros_fs_disk_entry_t)
        );
        return NULL;
    }

    vfs_node_t *node =
        allocate_node(index, name, type);

    if (
        node == NULL ||
        !vfs_add_child(parent, node)
    )
    {
        clear_bytes(
            &entries[index],
            sizeof(latteros_fs_disk_entry_t)
        );
        (void)write_entry(index);

        if (node != NULL)
        {
            kfree(node->filesystem_data);
            kfree(node);
        }

        return NULL;
    }

    nodes[index] = node;
    return node;
}


static bool latteros_fs_remove(
    vfs_node_t *node
)
{
    latteros_fs_node_data_t *data =
        node_data(node);

    if (
        data == NULL ||
        data->entry_index ==
            LATTEROS_FS_ROOT_INDEX
    )
    {
        return false;
    }

    uint32_t index = data->entry_index;
    latteros_fs_disk_entry_t previous =
        entries[index];

    clear_bytes(
        &entries[index],
        sizeof(latteros_fs_disk_entry_t)
    );

    if (!write_entry(index))
    {
        entries[index] = previous;
        return false;
    }

    nodes[index] = NULL;
    kfree(data);
    node->filesystem_data = NULL;
    return true;
}

static bool latteros_fs_move(
    vfs_node_t *node,
    vfs_node_t *new_parent,
    const char *new_name
)
{
    latteros_fs_node_data_t *data =
        node_data(node);

    latteros_fs_node_data_t *parent_data =
        node_data(new_parent);

    if (
        data == NULL ||
        parent_data == NULL ||
        new_name == NULL ||
        data->entry_index ==
            LATTEROS_FS_ROOT_INDEX ||
        new_parent->type !=
            VFS_NODE_DIRECTORY
    )
    {
        return false;
    }

    uint32_t index = data->entry_index;
    latteros_fs_disk_entry_t previous =
        entries[index];

    entries[index].parent_index =
        parent_data->entry_index;
    entries[index].generation++;

    if (
        !copy_name(
            entries[index].name,
            new_name
        ) ||
        !write_entry(index)
    )
    {
        entries[index] = previous;
        return false;
    }

    return true;
}

bool latteros_fs_init(void)
{
    mounted = false;
    formatted_on_boot = false;
    mounted_partition = NULL;

    partition_init();

    mounted_partition =
        partition_find_type(
            PARTITION_TYPE_LATTEROS_FS
        );

    if (mounted_partition == NULL)
    {
        if (!partition_create_latteros_fs())
        {
            return false;
        }

        mounted_partition =
            partition_find_type(
                PARTITION_TYPE_LATTEROS_FS
            );
    }

    if (mounted_partition == NULL)
    {
        return false;
    }

    if (
        !partition_read(
            mounted_partition,
            0,
            1,
            &superblock
        )
    )
    {
        return false;
    }

    if (!superblock_valid())
    {
        if (!format_filesystem())
        {
            return false;
        }
    }

    if (
        !load_entries() ||
        !build_vfs_tree()
    )
    {
        return false;
    }

    mounted = true;

    if (formatted_on_boot)
    {
        (void)vfs_write_text(
            "/home/welcome.txt",
            "Your LatterOS home directory is persistent.\n"
        );
    }

    return true;
}

bool latteros_fs_is_mounted(void)
{
    return mounted;
}

bool latteros_fs_was_formatted(void)
{
    return formatted_on_boot;
}

bool latteros_fs_sync(void)
{
    return mounted;
}

uint32_t latteros_fs_entry_count(void)
{
    uint32_t count = 0;

    for (
        uint32_t index = 0;
        index < LATTEROS_FS_ENTRY_COUNT;
        index++
    )
    {
        if (entries[index].used)
        {
            count++;
        }
    }

    return count;
}

uint64_t latteros_fs_used_bytes(void)
{
    uint64_t bytes = 0;

    for (
        uint32_t index = 0;
        index < LATTEROS_FS_ENTRY_COUNT;
        index++
    )
    {
        if (
            entries[index].used &&
            entries[index].type ==
                LATTEROS_FS_ENTRY_FILE
        )
        {
            bytes += entries[index].size;
        }
    }

    return bytes;
}

uint64_t latteros_fs_capacity_bytes(void)
{
    return (
        (uint64_t)(
            LATTEROS_FS_ENTRY_COUNT - 1
        ) * LATTEROS_FS_FILE_CAPACITY
    );
}

void latteros_fs_print_info(void)
{
    if (!mounted)
    {
        terminal_write_line(
            "LatterOS filesystem: not mounted"
        );
        return;
    }

    char line[128];

    ksnprintf(
        line,
        sizeof(line),
        "LatterOS filesystem mounted at /home"
    );
    terminal_write_line(line);

    ksnprintf(
        line,
        sizeof(line),
        "Partition: start=%llu sectors=%llu",
        (unsigned long long)
            mounted_partition->start_lba,
        (unsigned long long)
            mounted_partition->sector_count
    );
    terminal_write_line(line);

    ksnprintf(
        line,
        sizeof(line),
        "Entries: %u/%u",
        latteros_fs_entry_count(),
        LATTEROS_FS_ENTRY_COUNT
    );
    terminal_write_line(line);

    ksnprintf(
        line,
        sizeof(line),
        "File data: %llu/%llu bytes",
        (unsigned long long)
            latteros_fs_used_bytes(),
        (unsigned long long)
            latteros_fs_capacity_bytes()
    );
    terminal_write_line(line);

    if (formatted_on_boot)
    {
        terminal_write_line(
            "Filesystem formatted this boot"
        );
    }
}
