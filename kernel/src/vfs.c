#include "vfs.h"

#include <stddef.h>
#include <stdint.h>

#define VFS_MAX_COMPONENT_DEPTH 32

static vfs_node_t *root_node;
static vfs_node_t *current_directory;

static bool strings_equal(
    const char *first,
    const char *second
)
{
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

static bool valid_name(const char *name)
{
    if (
        name == NULL ||
        name[0] == '\0'
    )
    {
        return false;
    }

    if (
        strings_equal(name, ".") ||
        strings_equal(name, "..")
    )
    {
        return false;
    }

    size_t length = 0;

    while (name[length] != '\0')
    {
        if (
            name[length] == '/' ||
            length >= VFS_NAME_MAX
        )
        {
            return false;
        }

        length++;
    }

    return length > 0;
}

void vfs_init(void)
{
    root_node = NULL;
    current_directory = NULL;
}

bool vfs_mount_root(vfs_node_t *root)
{
    if (
        root == NULL ||
        root->type != VFS_NODE_DIRECTORY
    )
    {
        return false;
    }

    root->parent = NULL;
    root_node = root;
    current_directory = root;

    return true;
}

vfs_node_t *vfs_root(void)
{
    return root_node;
}

vfs_node_t *vfs_current_directory(void)
{
    return current_directory;
}

vfs_node_t *vfs_find_child(
    const vfs_node_t *parent,
    const char *name
)
{
    if (
        parent == NULL ||
        parent->type != VFS_NODE_DIRECTORY ||
        name == NULL
    )
    {
        return NULL;
    }

    vfs_node_t *child =
        parent->first_child;

    while (child != NULL)
    {
        if (
            strings_equal(
                child->name,
                name
            )
        )
        {
            return child;
        }

        child = child->next_sibling;
    }

    return NULL;
}

bool vfs_add_child(
    vfs_node_t *parent,
    vfs_node_t *child
)
{
    if (
        parent == NULL ||
        child == NULL ||
        parent->type != VFS_NODE_DIRECTORY ||
        !valid_name(child->name) ||
        vfs_find_child(parent, child->name) != NULL
    )
    {
        return false;
    }

    child->parent = parent;
    child->next_sibling = NULL;

    if (parent->first_child == NULL)
    {
        parent->first_child = child;
        return true;
    }

    vfs_node_t *last =
        parent->first_child;

    while (last->next_sibling != NULL)
    {
        last = last->next_sibling;
    }

    last->next_sibling = child;
    return true;
}

vfs_node_t *vfs_open(const char *path)
{
    if (
        root_node == NULL ||
        current_directory == NULL ||
        path == NULL
    )
    {
        return NULL;
    }

    if (path[0] == '\0')
    {
        return current_directory;
    }

    vfs_node_t *node =
        path[0] == '/' ?
        root_node :
        current_directory;

    const char *cursor = path;

    while (*cursor != '\0')
    {
        while (*cursor == '/')
        {
            cursor++;
        }

        if (*cursor == '\0')
        {
            break;
        }

        char component[VFS_NAME_MAX + 1];
        size_t length = 0;

        while (
            *cursor != '\0' &&
            *cursor != '/'
        )
        {
            if (length >= VFS_NAME_MAX)
            {
                return NULL;
            }

            component[length] = *cursor;
            length++;
            cursor++;
        }

        component[length] = '\0';

        if (strings_equal(component, "."))
        {
            continue;
        }

        if (strings_equal(component, ".."))
        {
            if (node->parent != NULL)
            {
                node = node->parent;
            }

            continue;
        }

        if (node->type != VFS_NODE_DIRECTORY)
        {
            return NULL;
        }

        node = vfs_find_child(
            node,
            component
        );

        if (node == NULL)
        {
            return NULL;
        }
    }

    return node;
}

static bool resolve_parent(
    const char *path,
    vfs_node_t **parent,
    char *name
)
{
    if (
        path == NULL ||
        parent == NULL ||
        name == NULL ||
        path[0] == '\0'
    )
    {
        return false;
    }

    size_t length = string_length(path);

    if (length >= VFS_PATH_MAX)
    {
        return false;
    }

    char normalized[VFS_PATH_MAX];

    for (
        size_t index = 0;
        index <= length;
        index++
    )
    {
        normalized[index] = path[index];
    }

    while (
        length > 1 &&
        normalized[length - 1] == '/'
    )
    {
        length--;
        normalized[length] = '\0';
    }

    size_t last_slash = length;

    while (
        last_slash > 0 &&
        normalized[last_slash - 1] != '/'
    )
    {
        last_slash--;
    }

    const char *source_name =
        &normalized[last_slash];

    if (!valid_name(source_name))
    {
        return false;
    }

    size_t name_length =
        string_length(source_name);

    for (
        size_t index = 0;
        index <= name_length;
        index++
    )
    {
        name[index] = source_name[index];
    }

    if (last_slash == 0)
    {
        *parent = current_directory;
    }
    else if (last_slash == 1)
    {
        *parent = root_node;
    }
    else
    {
        normalized[last_slash - 1] = '\0';
        *parent = vfs_open(normalized);
    }

    return (
        *parent != NULL &&
        (*parent)->type ==
            VFS_NODE_DIRECTORY
    );
}

bool vfs_change_directory(const char *path)
{
    vfs_node_t *node =
        vfs_open(path);

    if (
        node == NULL ||
        node->type != VFS_NODE_DIRECTORY
    )
    {
        return false;
    }

    current_directory = node;
    return true;
}

static bool create_node(
    const char *path,
    vfs_node_type_t type
)
{
    if (
        root_node == NULL ||
        current_directory == NULL
    )
    {
        return false;
    }

    vfs_node_t *parent;
    char name[VFS_NAME_MAX + 1];

    if (
        !resolve_parent(
            path,
            &parent,
            name
        )
    )
    {
        return false;
    }

    if (
        vfs_find_child(parent, name) != NULL ||
        parent->operations == NULL ||
        parent->operations->create == NULL
    )
    {
        return false;
    }

    return (
        parent->operations->create(
            parent,
            name,
            type
        ) != NULL
    );
}

bool vfs_create_file(const char *path)
{
    return create_node(
        path,
        VFS_NODE_FILE
    );
}

bool vfs_make_directory(const char *path)
{
    return create_node(
        path,
        VFS_NODE_DIRECTORY
    );
}

size_t vfs_read(
    vfs_node_t *node,
    size_t offset,
    void *buffer,
    size_t count
)
{
    if (
        node == NULL ||
        node->type != VFS_NODE_FILE ||
        node->operations == NULL ||
        node->operations->read == NULL
    )
    {
        return 0;
    }

    return node->operations->read(
        node,
        offset,
        buffer,
        count
    );
}

size_t vfs_write(
    vfs_node_t *node,
    size_t offset,
    const void *buffer,
    size_t count
)
{
    if (
        node == NULL ||
        node->type != VFS_NODE_FILE ||
        node->operations == NULL ||
        node->operations->write == NULL
    )
    {
        return 0;
    }

    return node->operations->write(
        node,
        offset,
        buffer,
        count
    );
}

bool vfs_write_text(
    const char *path,
    const char *text
)
{
    if (
        path == NULL ||
        text == NULL
    )
    {
        return false;
    }

    vfs_node_t *node =
        vfs_open(path);

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
        node->operations == NULL ||
        node->operations->truncate == NULL
    )
    {
        return false;
    }

    if (!node->operations->truncate(node))
    {
        return false;
    }

    size_t length =
        string_length(text);

    if (length == 0)
    {
        return true;
    }

    return (
        vfs_write(
            node,
            0,
            text,
            length
        ) == length
    );
}

bool vfs_get_working_directory(
    char *buffer,
    size_t capacity
)
{
    if (
        buffer == NULL ||
        capacity < 2 ||
        current_directory == NULL
    )
    {
        return false;
    }

    if (current_directory == root_node)
    {
        buffer[0] = '/';
        buffer[1] = '\0';
        return true;
    }

    const vfs_node_t *parts[
        VFS_MAX_COMPONENT_DEPTH
    ];

    size_t depth = 0;
    const vfs_node_t *node =
        current_directory;

    while (
        node != NULL &&
        node != root_node
    )
    {
        if (
            depth >=
            VFS_MAX_COMPONENT_DEPTH
        )
        {
            return false;
        }

        parts[depth] = node;
        depth++;
        node = node->parent;
    }

    size_t output = 0;
    buffer[output] = '/';
    output++;

    while (depth > 0)
    {
        depth--;

        const char *name =
            parts[depth]->name;

        size_t index = 0;

        while (name[index] != '\0')
        {
            if (output + 1 >= capacity)
            {
                return false;
            }

            buffer[output] = name[index];
            output++;
            index++;
        }

        if (depth > 0)
        {
            if (output + 1 >= capacity)
            {
                return false;
            }

            buffer[output] = '/';
            output++;
        }
    }

    buffer[output] = '\0';
    return true;
}
