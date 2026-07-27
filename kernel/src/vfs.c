#include "vfs.h"

#include "heap.h"
#include "security.h"

#include <stddef.h>
#include <stdint.h>

#define VFS_MAX_COMPONENT_DEPTH 32
#define VFS_COPY_BUFFER_SIZE 512

static vfs_node_t *root_node;
static vfs_node_t *current_directory;

static uint16_t access_bits_for_identity(
    const vfs_node_t *node,
    uint32_t uid,
    uint32_t gid
)
{
    if (node == NULL)
    {
        return 0;
    }

    if (uid == node->owner_uid)
    {
        return (uint16_t)(
            (node->mode >> 6) & 0x7U
        );
    }

    if (gid == node->owner_gid)
    {
        return (uint16_t)(
            (node->mode >> 3) & 0x7U
        );
    }

    return (uint16_t)(node->mode & 0x7U);
}

void vfs_initialize_metadata(
    vfs_node_t *node,
    vfs_node_type_t type
)
{
    if (node == NULL)
    {
        return;
    }

    node->owner_uid = security_effective_uid();
    node->owner_gid = security_effective_gid();
    node->mode = type == VFS_NODE_DIRECTORY ?
        VFS_MODE_DIRECTORY_DEFAULT :
        VFS_MODE_FILE_DEFAULT;
}

bool vfs_check_access(
    const vfs_node_t *node,
    uint8_t access
)
{
    if (node == NULL)
    {
        return false;
    }

    if (!security_is_ready())
    {
        return true;
    }

    if (
        security_effective_uid() ==
            SECURITY_UID_ROOT ||
        security_effective_has_capability(
            SECURITY_CAP_FILE_ADMIN
        )
    )
    {
        return true;
    }

    uint16_t bits = access_bits_for_identity(
        node,
        security_effective_uid(),
        security_effective_gid()
    );

    if (
        (access & VFS_ACCESS_READ) != 0 &&
        (bits & 0x4U) == 0
    )
    {
        return false;
    }

    if (
        (access & VFS_ACCESS_WRITE) != 0 &&
        (bits & 0x2U) == 0
    )
    {
        return false;
    }

    if (
        (access & VFS_ACCESS_EXECUTE) != 0 &&
        (bits & 0x1U) == 0
    )
    {
        return false;
    }

    return true;
}

static bool sync_metadata(vfs_node_t *node)
{
    if (node == NULL)
    {
        return false;
    }

    if (
        node->operations == NULL ||
        node->operations->metadata == NULL
    )
    {
        return true;
    }

    return node->operations->metadata(node);
}

bool vfs_chmod(
    const char *path,
    uint16_t mode
)
{
    vfs_node_t *node = vfs_open(path);

    if (
        node == NULL ||
        (mode & ~0777U) != 0
    )
    {
        return false;
    }

    if (
        security_is_ready() &&
        security_effective_uid() !=
            node->owner_uid &&
        !security_effective_has_capability(
            SECURITY_CAP_FILE_ADMIN
        )
    )
    {
        return false;
    }

    uint16_t previous = node->mode;
    node->mode = mode;

    if (!sync_metadata(node))
    {
        node->mode = previous;
        return false;
    }

    return true;
}

bool vfs_chown(
    const char *path,
    uint32_t uid,
    uint32_t gid
)
{
    vfs_node_t *node = vfs_open(path);

    if (
        node == NULL ||
        !security_effective_has_capability(
            SECURITY_CAP_FILE_ADMIN
        )
    )
    {
        return false;
    }

    uint32_t previous_uid = node->owner_uid;
    uint32_t previous_gid = node->owner_gid;

    node->owner_uid = uid;
    node->owner_gid = gid;

    if (!sync_metadata(node))
    {
        node->owner_uid = previous_uid;
        node->owner_gid = previous_gid;
        return false;
    }

    return true;
}

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

static bool node_is_ancestor(
    const vfs_node_t *ancestor,
    const vfs_node_t *node
)
{
    const vfs_node_t *cursor = node;

    while (cursor != NULL)
    {
        if (cursor == ancestor)
        {
            return true;
        }

        cursor = cursor->parent;
    }

    return false;
}

static bool mounted_root(
    const vfs_node_t *node
)
{
    return (
        node != NULL &&
        node->parent != NULL &&
        node->operations !=
            node->parent->operations
    );
}

static bool detach_child(
    vfs_node_t *parent,
    vfs_node_t *child
)
{
    if (
        parent == NULL ||
        child == NULL
    )
    {
        return false;
    }

    vfs_node_t *previous = NULL;
    vfs_node_t *cursor =
        parent->first_child;

    while (cursor != NULL)
    {
        if (cursor == child)
        {
            if (previous == NULL)
            {
                parent->first_child =
                    cursor->next_sibling;
            }
            else
            {
                previous->next_sibling =
                    cursor->next_sibling;
            }

            cursor->next_sibling = NULL;
            return true;
        }

        previous = cursor;
        cursor = cursor->next_sibling;
    }

    return false;
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

    if (!copy_name(name, source_name))
    {
        return false;
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

bool vfs_mount_at(
    const char *path,
    vfs_node_t *filesystem_root
)
{
    if (
        root_node == NULL ||
        current_directory == NULL ||
        path == NULL ||
        filesystem_root == NULL ||
        filesystem_root->type !=
            VFS_NODE_DIRECTORY
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
        ) ||
        vfs_find_child(parent, name) != NULL
    )
    {
        return false;
    }

    if (!copy_name(filesystem_root->name, name))
    {
        return false;
    }

    return vfs_add_child(
        parent,
        filesystem_root
    );
}

vfs_node_t *vfs_detach_mount(const char *path)
{
    vfs_node_t *node = vfs_open(path);

    if (
        node == NULL ||
        node->parent == NULL ||
        !mounted_root(node)
    )
    {
        return NULL;
    }

    if (node_is_ancestor(node, current_directory))
    {
        current_directory = node->parent;
    }

    vfs_node_t *parent = node->parent;

    if (!detach_child(parent, node))
    {
        return NULL;
    }

    node->parent = NULL;
    return node;
}

bool vfs_change_directory(const char *path)
{
    vfs_node_t *node =
        vfs_open(path);

    if (
        node == NULL ||
        node->type != VFS_NODE_DIRECTORY ||
        !vfs_check_access(
            node,
            VFS_ACCESS_EXECUTE
        )
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
        !vfs_check_access(
            parent,
            VFS_ACCESS_WRITE |
                VFS_ACCESS_EXECUTE
        ) ||
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

static bool remove_node_recursive(
    vfs_node_t *node,
    bool recursive
)
{
    if (
        node == NULL ||
        node == root_node ||
        mounted_root(node) ||
        node_is_ancestor(
            node,
            current_directory
        )
    )
    {
        return false;
    }

    if (
        node->type == VFS_NODE_DIRECTORY &&
        node->first_child != NULL &&
        !recursive
    )
    {
        return false;
    }

    while (node->first_child != NULL)
    {
        if (
            !remove_node_recursive(
                node->first_child,
                true
            )
        )
        {
            return false;
        }
    }

    if (
        node->operations == NULL ||
        node->operations->remove == NULL ||
        !node->operations->remove(node)
    )
    {
        return false;
    }

    vfs_node_t *parent = node->parent;

    if (
        parent == NULL ||
        !detach_child(parent, node)
    )
    {
        return false;
    }

    kfree(node);
    return true;
}

bool vfs_remove(
    const char *path,
    bool recursive
)
{
    vfs_node_t *node = vfs_open(path);

    if (
        node == NULL ||
        node->parent == NULL ||
        !vfs_check_access(
            node->parent,
            VFS_ACCESS_WRITE |
                VFS_ACCESS_EXECUTE
        )
    )
    {
        return false;
    }

    return remove_node_recursive(
        node,
        recursive
    );
}

static bool move_node_same_filesystem(
    vfs_node_t *node,
    vfs_node_t *new_parent,
    const char *new_name
)
{
    if (
        node == NULL ||
        new_parent == NULL ||
        !valid_name(new_name) ||
        node == root_node ||
        mounted_root(node) ||
        node_is_ancestor(
            node,
            current_directory
        ) ||
        new_parent->type !=
            VFS_NODE_DIRECTORY ||
        node_is_ancestor(node, new_parent) ||
        vfs_find_child(
            new_parent,
            new_name
        ) != NULL ||
        node->operations == NULL ||
        node->operations !=
            new_parent->operations ||
        node->operations->move == NULL
    )
    {
        return false;
    }

    if (
        !node->operations->move(
            node,
            new_parent,
            new_name
        )
    )
    {
        return false;
    }

    vfs_node_t *old_parent = node->parent;

    if (
        old_parent == NULL ||
        !detach_child(old_parent, node)
    )
    {
        return false;
    }

    if (!copy_name(node->name, new_name))
    {
        return false;
    }

    return vfs_add_child(
        new_parent,
        node
    );
}

bool vfs_rename(
    const char *path,
    const char *new_name
)
{
    vfs_node_t *node = vfs_open(path);

    if (
        node == NULL ||
        node->parent == NULL ||
        !valid_name(new_name) ||
        !vfs_check_access(
            node->parent,
            VFS_ACCESS_WRITE |
                VFS_ACCESS_EXECUTE
        )
    )
    {
        return false;
    }

    if (strings_equal(node->name, new_name))
    {
        return true;
    }

    return move_node_same_filesystem(
        node,
        node->parent,
        new_name
    );
}

static bool copy_file_contents(
    vfs_node_t *source,
    vfs_node_t *destination
)
{
    if (
        source == NULL ||
        destination == NULL ||
        source->type != VFS_NODE_FILE ||
        destination->type != VFS_NODE_FILE ||
        destination->operations == NULL ||
        destination->operations->truncate == NULL ||
        !destination->operations->truncate(destination)
    )
    {
        return false;
    }

    uint8_t buffer[VFS_COPY_BUFFER_SIZE];
    size_t offset = 0;

    while (offset < source->size)
    {
        size_t remaining =
            source->size - offset;

        size_t requested =
            remaining < sizeof(buffer) ?
            remaining : sizeof(buffer);

        size_t count = vfs_read(
            source,
            offset,
            buffer,
            requested
        );

        if (count == 0)
        {
            return false;
        }

        if (
            vfs_write(
                destination,
                offset,
                buffer,
                count
            ) != count
        )
        {
            return false;
        }

        offset += count;
    }

    return true;
}

static bool copy_node_into(
    vfs_node_t *source,
    vfs_node_t *destination_parent,
    const char *destination_name
)
{
    if (
        source == NULL ||
        destination_parent == NULL ||
        !valid_name(destination_name) ||
        destination_parent->type !=
            VFS_NODE_DIRECTORY ||
        destination_parent->operations == NULL ||
        destination_parent->operations->create == NULL ||
        vfs_find_child(
            destination_parent,
            destination_name
        ) != NULL ||
        (
            source->type == VFS_NODE_DIRECTORY &&
            node_is_ancestor(
                source,
                destination_parent
            )
        )
    )
    {
        return false;
    }

    vfs_node_t *destination =
        destination_parent->operations->create(
            destination_parent,
            destination_name,
            source->type
        );

    if (destination == NULL)
    {
        return false;
    }

    bool success = true;

    if (source->type == VFS_NODE_FILE)
    {
        success = copy_file_contents(
            source,
            destination
        );
    }
    else
    {
        vfs_node_t *child =
            source->first_child;

        while (
            success &&
            child != NULL
        )
        {
            success = copy_node_into(
                child,
                destination,
                child->name
            );

            child = child->next_sibling;
        }
    }

    if (!success)
    {
        (void)remove_node_recursive(
            destination,
            true
        );
    }

    return success;
}

static bool destination_target(
    vfs_node_t *source,
    const char *destination_path,
    vfs_node_t **parent,
    char *name,
    vfs_node_t **existing
)
{
    if (
        source == NULL ||
        destination_path == NULL ||
        parent == NULL ||
        name == NULL ||
        existing == NULL
    )
    {
        return false;
    }

    *existing = vfs_open(destination_path);

    if (*existing != NULL)
    {
        if (
            (*existing)->type ==
                VFS_NODE_DIRECTORY
        )
        {
            *parent = *existing;
            return copy_name(
                name,
                source->name
            );
        }

        *parent = (*existing)->parent;
        return copy_name(
            name,
            (*existing)->name
        );
    }

    return resolve_parent(
        destination_path,
        parent,
        name
    );
}

bool vfs_copy(
    const char *source_path,
    const char *destination_path
)
{
    vfs_node_t *source =
        vfs_open(source_path);

    if (
        source == NULL ||
        source == root_node ||
        destination_path == NULL ||
        (
            source->type == VFS_NODE_FILE &&
            !vfs_check_access(
                source,
                VFS_ACCESS_READ
            )
        )
    )
    {
        return false;
    }

    vfs_node_t *parent;
    vfs_node_t *existing;
    char name[VFS_NAME_MAX + 1];

    if (
        !destination_target(
            source,
            destination_path,
            &parent,
            name,
            &existing
        ) ||
        !vfs_check_access(
            parent,
            VFS_ACCESS_WRITE |
                VFS_ACCESS_EXECUTE
        )
    )
    {
        return false;
    }

    if (
        existing != NULL &&
        existing->type == VFS_NODE_FILE
    )
    {
        return (
            source->type == VFS_NODE_FILE &&
            existing != source &&
            copy_file_contents(
                source,
                existing
            )
        );
    }

    return copy_node_into(
        source,
        parent,
        name
    );
}

bool vfs_move(
    const char *source_path,
    const char *destination_path
)
{
    vfs_node_t *source =
        vfs_open(source_path);

    if (
        source == NULL ||
        source == root_node ||
        mounted_root(source) ||
        node_is_ancestor(
            source,
            current_directory
        ) ||
        destination_path == NULL ||
        source->parent == NULL ||
        !vfs_check_access(
            source->parent,
            VFS_ACCESS_WRITE |
                VFS_ACCESS_EXECUTE
        )
    )
    {
        return false;
    }

    vfs_node_t *parent;
    vfs_node_t *existing;
    char name[VFS_NAME_MAX + 1];

    if (
        !destination_target(
            source,
            destination_path,
            &parent,
            name,
            &existing
        ) ||
        !vfs_check_access(
            parent,
            VFS_ACCESS_WRITE |
                VFS_ACCESS_EXECUTE
        ) ||
        (
            existing != NULL &&
            existing->type == VFS_NODE_FILE
        )
    )
    {
        return false;
    }

    if (
        parent == source->parent &&
        strings_equal(name, source->name)
    )
    {
        return true;
    }

    if (
        source->operations ==
            parent->operations &&
        source->operations != NULL &&
        source->operations->move != NULL
    )
    {
        return move_node_same_filesystem(
            source,
            parent,
            name
        );
    }

    if (
        !copy_node_into(
            source,
            parent,
            name
        )
    )
    {
        return false;
    }

    return remove_node_recursive(
        source,
        true
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
        !vfs_check_access(
            node,
            VFS_ACCESS_READ
        ) ||
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
        !vfs_check_access(
            node,
            VFS_ACCESS_WRITE
        ) ||
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

bool vfs_truncate(vfs_node_t *node)
{
    if (
        node == NULL ||
        node->type != VFS_NODE_FILE ||
        !vfs_check_access(
            node,
            VFS_ACCESS_WRITE
        ) ||
        node->operations == NULL ||
        node->operations->truncate == NULL
    )
    {
        return false;
    }

    return node->operations->truncate(node);
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
        node->type != VFS_NODE_FILE
    )
    {
        return false;
    }

    if (!vfs_truncate(node))
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

    const vfs_node_t *components[
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
        if (depth >= VFS_MAX_COMPONENT_DEPTH)
        {
            return false;
        }

        components[depth] = node;
        depth++;
        node = node->parent;
    }

    size_t output = 0;
    buffer[output++] = '/';

    for (
        size_t index = depth;
        index > 0;
        index--
    )
    {
        const char *name =
            components[index - 1]->name;

        size_t length =
            string_length(name);

        if (
            output + length + 1 >
            capacity
        )
        {
            return false;
        }

        for (
            size_t character = 0;
            character < length;
            character++
        )
        {
            buffer[output++] =
                name[character];
        }

        if (index > 1)
        {
            buffer[output++] = '/';
        }
    }

    buffer[output] = '\0';
    return true;
}
