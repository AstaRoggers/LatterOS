#ifndef VFS_H
#define VFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VFS_NAME_MAX 31
#define VFS_PATH_MAX 256

#define VFS_MODE_OWNER_READ    0400U
#define VFS_MODE_OWNER_WRITE   0200U
#define VFS_MODE_OWNER_EXECUTE 0100U
#define VFS_MODE_GROUP_READ    0040U
#define VFS_MODE_GROUP_WRITE   0020U
#define VFS_MODE_GROUP_EXECUTE 0010U
#define VFS_MODE_OTHER_READ    0004U
#define VFS_MODE_OTHER_WRITE   0002U
#define VFS_MODE_OTHER_EXECUTE 0001U

#define VFS_MODE_FILE_DEFAULT      0644U
#define VFS_MODE_DIRECTORY_DEFAULT 0755U
#define VFS_MODE_EXECUTABLE_DEFAULT 0755U

#define VFS_ACCESS_READ    0x01U
#define VFS_ACCESS_WRITE   0x02U
#define VFS_ACCESS_EXECUTE 0x04U

typedef enum
{
    VFS_NODE_FILE,
    VFS_NODE_DIRECTORY
} vfs_node_type_t;

typedef struct vfs_node vfs_node_t;

typedef size_t (*vfs_read_function_t)(
    vfs_node_t *node,
    size_t offset,
    void *buffer,
    size_t count
);

typedef size_t (*vfs_write_function_t)(
    vfs_node_t *node,
    size_t offset,
    const void *buffer,
    size_t count
);

typedef bool (*vfs_truncate_function_t)(
    vfs_node_t *node
);

typedef vfs_node_t *(*vfs_create_function_t)(
    vfs_node_t *parent,
    const char *name,
    vfs_node_type_t type
);

typedef bool (*vfs_remove_function_t)(
    vfs_node_t *node
);

typedef bool (*vfs_move_function_t)(
    vfs_node_t *node,
    vfs_node_t *new_parent,
    const char *new_name
);

typedef bool (*vfs_metadata_function_t)(
    vfs_node_t *node
);

typedef struct
{
    vfs_read_function_t read;
    vfs_write_function_t write;
    vfs_truncate_function_t truncate;
    vfs_create_function_t create;
    vfs_remove_function_t remove;
    vfs_move_function_t move;
    vfs_metadata_function_t metadata;
} vfs_operations_t;

struct vfs_node
{
    char name[VFS_NAME_MAX + 1];
    vfs_node_type_t type;
    size_t size;

    vfs_node_t *parent;
    vfs_node_t *first_child;
    vfs_node_t *next_sibling;

    const vfs_operations_t *operations;
    void *filesystem_data;

    uint32_t owner_uid;
    uint32_t owner_gid;
    uint16_t mode;
};

void vfs_init(void);

bool vfs_mount_root(vfs_node_t *root);
bool vfs_mount_at(
    const char *path,
    vfs_node_t *filesystem_root
);

vfs_node_t *vfs_detach_mount(const char *path);

vfs_node_t *vfs_root(void);
vfs_node_t *vfs_current_directory(void);

bool vfs_add_child(
    vfs_node_t *parent,
    vfs_node_t *child
);

vfs_node_t *vfs_find_child(
    const vfs_node_t *parent,
    const char *name
);

vfs_node_t *vfs_open(const char *path);

bool vfs_change_directory(const char *path);

void vfs_initialize_metadata(
    vfs_node_t *node,
    vfs_node_type_t type
);

bool vfs_check_access(
    const vfs_node_t *node,
    uint8_t access
);

bool vfs_chmod(
    const char *path,
    uint16_t mode
);

bool vfs_chown(
    const char *path,
    uint32_t uid,
    uint32_t gid
);

bool vfs_create_file(const char *path);
bool vfs_make_directory(const char *path);

bool vfs_remove(
    const char *path,
    bool recursive
);

bool vfs_rename(
    const char *path,
    const char *new_name
);

bool vfs_copy(
    const char *source_path,
    const char *destination_path
);

bool vfs_move(
    const char *source_path,
    const char *destination_path
);

size_t vfs_read(
    vfs_node_t *node,
    size_t offset,
    void *buffer,
    size_t count
);

size_t vfs_write(
    vfs_node_t *node,
    size_t offset,
    const void *buffer,
    size_t count
);

bool vfs_truncate(vfs_node_t *node);

bool vfs_write_text(
    const char *path,
    const char *text
);

bool vfs_get_working_directory(
    char *buffer,
    size_t capacity
);

#endif
