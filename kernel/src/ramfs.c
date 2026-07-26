#include "ramfs.h"

#include "heap.h"
#include "vfs.h"

#include <stddef.h>
#include <stdint.h>

extern const uint8_t user_hello_start[];
extern const uint8_t user_hello_end[];
extern const uint8_t user_spin_start[];
extern const uint8_t user_spin_end[];
extern const uint8_t user_apitest_start[];
extern const uint8_t user_apitest_end[];

#define RAMFS_FILE_CAPACITY 2048

typedef struct
{
    uint8_t *data;
    size_t capacity;
} ramfs_file_data_t;

static size_t ramfs_read(
    vfs_node_t *node,
    size_t offset,
    void *buffer,
    size_t count
);

static size_t ramfs_write(
    vfs_node_t *node,
    size_t offset,
    const void *buffer,
    size_t count
);

static bool ramfs_truncate(
    vfs_node_t *node
);

static vfs_node_t *ramfs_create(
    vfs_node_t *parent,
    const char *name,
    vfs_node_type_t type
);

static bool ramfs_remove(
    vfs_node_t *node
);

static bool ramfs_move(
    vfs_node_t *node,
    vfs_node_t *new_parent,
    const char *new_name
);

static const vfs_operations_t ramfs_operations = {
    .read = ramfs_read,
    .write = ramfs_write,
    .truncate = ramfs_truncate,
    .create = ramfs_create,
    .remove = ramfs_remove,
    .move = ramfs_move
};

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

static bool copy_name(
    char *destination,
    const char *source
)
{
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

static vfs_node_t *allocate_node(
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

    clear_bytes(
        node,
        sizeof(vfs_node_t)
    );

    if (!copy_name(node->name, name))
    {
        kfree(node);
        return NULL;
    }

    node->type = type;
    node->operations =
        &ramfs_operations;

    if (type == VFS_NODE_FILE)
    {
        ramfs_file_data_t *file_data =
            kmalloc(
                sizeof(ramfs_file_data_t)
            );

        if (file_data == NULL)
        {
            kfree(node);
            return NULL;
        }

        file_data->data =
            kmalloc(RAMFS_FILE_CAPACITY);

        if (file_data->data == NULL)
        {
            kfree(file_data);
            kfree(node);
            return NULL;
        }

        clear_bytes(
            file_data->data,
            RAMFS_FILE_CAPACITY
        );

        file_data->capacity =
            RAMFS_FILE_CAPACITY;

        node->filesystem_data =
            file_data;
    }

    return node;
}

static size_t ramfs_read(
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

    ramfs_file_data_t *file_data =
        node->filesystem_data;

    size_t available =
        node->size - offset;

    if (count > available)
    {
        count = available;
    }

    uint8_t *destination = buffer;

    for (
        size_t index = 0;
        index < count;
        index++
    )
    {
        destination[index] =
            file_data->data[offset + index];
    }

    return count;
}

static size_t ramfs_write(
    vfs_node_t *node,
    size_t offset,
    const void *buffer,
    size_t count
)
{
    if (
        node == NULL ||
        buffer == NULL ||
        node->type != VFS_NODE_FILE ||
        node->filesystem_data == NULL
    )
    {
        return 0;
    }

    ramfs_file_data_t *file_data =
        node->filesystem_data;

    if (offset >= file_data->capacity)
    {
        return 0;
    }

    size_t available =
        file_data->capacity - offset;

    if (count > available)
    {
        count = available;
    }

    const uint8_t *source = buffer;

    for (
        size_t index = 0;
        index < count;
        index++
    )
    {
        file_data->data[offset + index] =
            source[index];
    }

    size_t end = offset + count;

    if (end > node->size)
    {
        node->size = end;
    }

    return count;
}

static bool ramfs_truncate(
    vfs_node_t *node
)
{
    if (
        node == NULL ||
        node->type != VFS_NODE_FILE ||
        node->filesystem_data == NULL
    )
    {
        return false;
    }

    node->size = 0;
    return true;
}

static vfs_node_t *ramfs_create(
    vfs_node_t *parent,
    const char *name,
    vfs_node_type_t type
)
{
    vfs_node_t *node =
        allocate_node(name, type);

    if (node == NULL)
    {
        return NULL;
    }

    if (!vfs_add_child(parent, node))
    {
        if (node->filesystem_data != NULL)
        {
            ramfs_file_data_t *file_data =
                node->filesystem_data;

            kfree(file_data->data);
            kfree(file_data);
        }

        kfree(node);
        return NULL;
    }

    return node;
}


static bool ramfs_remove(
    vfs_node_t *node
)
{
    if (node == NULL)
    {
        return false;
    }

    if (
        node->type == VFS_NODE_FILE &&
        node->filesystem_data != NULL
    )
    {
        ramfs_file_data_t *file_data =
            node->filesystem_data;

        kfree(file_data->data);
        kfree(file_data);
        node->filesystem_data = NULL;
    }

    return true;
}

static bool ramfs_move(
    vfs_node_t *node,
    vfs_node_t *new_parent,
    const char *new_name
)
{
    return (
        node != NULL &&
        new_parent != NULL &&
        new_name != NULL
    );
}

bool ramfs_init(void)
{
    vfs_node_t *root =
        allocate_node(
            "",
            VFS_NODE_DIRECTORY
        );

    if (
        root == NULL ||
        !vfs_mount_root(root)
    )
    {
        return false;
    }

    if (
        !vfs_make_directory("/etc") ||
        !vfs_make_directory("/tmp") ||
        !vfs_make_directory("/bin")
    )
    {
        return false;
    }


    if (!vfs_create_file("/bin/hello"))
    {
        return false;
    }

    vfs_node_t *hello_program =
        vfs_open("/bin/hello");

    size_t hello_size =
        (size_t)(
            user_hello_end -
            user_hello_start
        );

    if (
        hello_program == NULL ||
        vfs_write(
            hello_program,
            0,
            user_hello_start,
            hello_size
        ) != hello_size
    )
    {
        return false;
    }

    if (!vfs_create_file("/bin/spin"))
    {
        return false;
    }

    vfs_node_t *spin_program =
        vfs_open("/bin/spin");

    size_t spin_size =
        (size_t)(
            user_spin_end -
            user_spin_start
        );

    if (
        spin_program == NULL ||
        vfs_write(
            spin_program,
            0,
            user_spin_start,
            spin_size
        ) != spin_size
    )
    {
        return false;
    }

    if (!vfs_create_file("/bin/apitest"))
    {
        return false;
    }

    vfs_node_t *apitest_program =
        vfs_open("/bin/apitest");

    size_t apitest_size =
        (size_t)(
            user_apitest_end -
            user_apitest_start
        );

    if (
        apitest_program == NULL ||
        vfs_write(
            apitest_program,
            0,
            user_apitest_start,
            apitest_size
        ) != apitest_size
    )
    {
        return false;
    }

    if (
        !vfs_write_text(
            "/README",
            "Welcome to the LatterOS RAM filesystem.\n"
            "Use ls, cd, pwd, cat, mkdir, and write.\n"
        )
    )
    {
        return false;
    }

    if (
        !vfs_write_text(
            "/etc/system.conf",
            "name=LatterOS\nversion=0.1\nfilesystem=ramfs\n"
        )
    )
    {
        return false;
    }

    return true;
}
