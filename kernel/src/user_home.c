#include "user_home.h"

#include "security.h"
#include "vfs.h"

#include <stddef.h>
#include <stdint.h>

#define USER_HOME_MODE 0700U
#define USER_FILE_MODE 0644U

static vfs_node_t *ensure_child(
    vfs_node_t *parent,
    const char *name,
    vfs_node_type_t type
)
{
    if (
        parent == NULL ||
        name == NULL ||
        parent->type != VFS_NODE_DIRECTORY
    )
    {
        return NULL;
    }

    vfs_node_t *node = vfs_find_child(parent, name);

    if (node != NULL)
    {
        return node->type == type ? node : NULL;
    }

    if (
        parent->operations == NULL ||
        parent->operations->create == NULL
    )
    {
        return NULL;
    }

    return parent->operations->create(parent, name, type);
}

static void set_identity(
    vfs_node_t *node,
    uint32_t uid,
    uint32_t gid,
    uint16_t mode
)
{
    if (node == NULL)
    {
        return;
    }

    node->owner_uid = uid;
    node->owner_gid = gid;
    node->mode = mode;

    if (
        node->operations != NULL &&
        node->operations->metadata != NULL
    )
    {
        (void)node->operations->metadata(node);
    }
}

static bool ensure_welcome_file(vfs_node_t *user_directory)
{
    static const char welcome_text[] =
        "Welcome to your writable LatterOS home directory.\n"
        "Documents saved here can be edited by the user account.\n";

    vfs_node_t *welcome = ensure_child(
        user_directory,
        "Welcome.txt",
        VFS_NODE_FILE
    );

    if (welcome == NULL)
    {
        return false;
    }

    set_identity(
        welcome,
        SECURITY_UID_USER,
        SECURITY_GID_USER,
        USER_FILE_MODE
    );

    if (welcome->size != 0)
    {
        return true;
    }

    size_t length = sizeof(welcome_text) - 1U;

    return vfs_write(
        welcome,
        0,
        welcome_text,
        length
    ) == length;
}

bool user_home_ensure(void)
{
    vfs_node_t *root = vfs_root();

    if (root == NULL)
    {
        return false;
    }

    vfs_node_t *home = ensure_child(
        root,
        "home",
        VFS_NODE_DIRECTORY
    );

    if (home == NULL)
    {
        return false;
    }

    set_identity(
        home,
        SECURITY_UID_ROOT,
        SECURITY_GID_ROOT,
        0755U
    );

    vfs_node_t *user = ensure_child(
        home,
        "user",
        VFS_NODE_DIRECTORY
    );

    vfs_node_t *guest = ensure_child(
        home,
        "guest",
        VFS_NODE_DIRECTORY
    );

    if (user == NULL || guest == NULL)
    {
        return false;
    }

    set_identity(
        user,
        SECURITY_UID_USER,
        SECURITY_GID_USER,
        USER_HOME_MODE
    );

    set_identity(
        guest,
        SECURITY_UID_GUEST,
        SECURITY_GID_GUEST,
        USER_HOME_MODE
    );

    vfs_node_t *documents = ensure_child(
        user,
        "Documents",
        VFS_NODE_DIRECTORY
    );

    if (documents == NULL)
    {
        return false;
    }

    set_identity(
        documents,
        SECURITY_UID_USER,
        SECURITY_GID_USER,
        USER_HOME_MODE
    );

    return ensure_welcome_file(user);
}
