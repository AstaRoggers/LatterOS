#include "executable.h"

#include "physical_memory.h"
#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EXECUTABLE_HEADER_SIZE 32U
#define EXECUTABLE_KNOWN_FLAGS \
    LATTEROS_EXECUTABLE_FLAG_64BIT

#pragma pack(push, 1)
typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t image_size;
    uint32_t entry_offset;
    uint32_t flags;
    uint32_t checksum;
    uint64_t reserved;
} executable_header_t;
#pragma pack(pop)

_Static_assert(
    sizeof(executable_header_t) ==
        EXECUTABLE_HEADER_SIZE,
    "Unexpected executable header size"
);

static uint32_t checksum_update(
    uint32_t checksum,
    const uint8_t *data,
    size_t size
)
{
    for (
        size_t index = 0;
        index < size;
        index++
    )
    {
        checksum ^= data[index];
        checksum *= 16777619U;
    }

    return checksum;
}

static uint32_t image_checksum(
    const uint8_t *data,
    size_t size
)
{
    return checksum_update(
        2166136261U,
        data,
        size
    );
}

static bool read_header(
    vfs_node_t *node,
    executable_header_t *header
)
{
    if (
        node == NULL ||
        header == NULL ||
        node->type != VFS_NODE_FILE ||
        node->size < sizeof(*header)
    )
    {
        return false;
    }

    return vfs_read(
        node,
        0,
        header,
        sizeof(*header)
    ) == sizeof(*header);
}

static bool header_valid(
    const executable_header_t *header,
    size_t file_size
)
{
    if (
        header == NULL ||
        header->magic !=
            LATTEROS_EXECUTABLE_MAGIC ||
        header->version !=
            LATTEROS_EXECUTABLE_VERSION ||
        header->header_size !=
            sizeof(executable_header_t) ||
        header->image_size == 0 ||
        header->image_size > PAGE_SIZE ||
        header->entry_offset >=
            header->image_size ||
        (header->flags &
            ~EXECUTABLE_KNOWN_FLAGS) != 0 ||
        (header->flags &
            LATTEROS_EXECUTABLE_FLAG_64BIT) == 0 ||
        header->reserved != 0
    )
    {
        return false;
    }

    size_t expected_size =
        sizeof(executable_header_t) +
        (size_t)header->image_size;

    return file_size == expected_size;
}

bool executable_install(
    const char *path,
    const uint8_t *image,
    size_t image_size,
    uint32_t entry_offset
)
{
    if (
        path == NULL ||
        image == NULL ||
        image_size == 0 ||
        image_size > PAGE_SIZE ||
        entry_offset >= image_size
    )
    {
        return false;
    }

    vfs_node_t *node = vfs_open(path);

    if (node == NULL)
    {
        if (!vfs_create_file(path))
        {
            return false;
        }

        node = vfs_open(path);
    }

    if (
        node == NULL ||
        node->type != VFS_NODE_FILE ||
        !vfs_truncate(node)
    )
    {
        return false;
    }

    executable_header_t header = {
        .magic = LATTEROS_EXECUTABLE_MAGIC,
        .version = LATTEROS_EXECUTABLE_VERSION,
        .header_size =
            sizeof(executable_header_t),
        .image_size = (uint32_t)image_size,
        .entry_offset = entry_offset,
        .flags =
            LATTEROS_EXECUTABLE_FLAG_64BIT,
        .checksum =
            image_checksum(image, image_size),
        .reserved = 0
    };

    if (
        vfs_write(
            node,
            0,
            &header,
            sizeof(header)
        ) != sizeof(header)
    )
    {
        return false;
    }

    if (
        vfs_write(
            node,
            sizeof(header),
            image,
            image_size
        ) != image_size
    )
    {
        return false;
    }

    return vfs_chmod(
        path,
        VFS_MODE_EXECUTABLE_DEFAULT
    );
}

bool executable_load(
    vfs_node_t *node,
    void *destination,
    size_t capacity,
    size_t *image_size,
    uint32_t *entry_offset
)
{
    if (
        node == NULL ||
        destination == NULL ||
        image_size == NULL ||
        entry_offset == NULL
    )
    {
        return false;
    }

    executable_header_t header;

    if (
        !read_header(node, &header) ||
        !header_valid(&header, node->size) ||
        header.image_size > capacity
    )
    {
        return false;
    }

    if (
        vfs_read(
            node,
            sizeof(header),
            destination,
            header.image_size
        ) != header.image_size
    )
    {
        return false;
    }

    if (
        image_checksum(
            destination,
            header.image_size
        ) != header.checksum
    )
    {
        return false;
    }

    *image_size = header.image_size;
    *entry_offset = header.entry_offset;
    return true;
}

bool executable_validate(vfs_node_t *node)
{
    if (node == NULL)
    {
        return false;
    }

    executable_header_t header;

    if (
        !read_header(node, &header) ||
        !header_valid(&header, node->size)
    )
    {
        return false;
    }

    uint8_t buffer[128];
    size_t offset = 0;
    uint32_t checksum = 2166136261U;

    while (offset < header.image_size)
    {
        size_t remaining =
            header.image_size - offset;

        size_t count =
            remaining < sizeof(buffer) ?
            remaining :
            sizeof(buffer);

        if (
            vfs_read(
                node,
                sizeof(header) + offset,
                buffer,
                count
            ) != count
        )
        {
            return false;
        }

        checksum = checksum_update(
            checksum,
            buffer,
            count
        );

        offset += count;
    }

    return checksum == header.checksum;
}
