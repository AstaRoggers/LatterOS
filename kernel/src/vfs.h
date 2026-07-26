#ifndef VFS_H
#define VFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VFS_NAME_MAX 31
#define VFS_PATH_MAX 256

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

typedef struct
{
    vfs_read_function_t read;
    vfs_write_function_t write;
    vfs_truncate_function_t truncate;
    vfs_create_function_t create;
    vfs_remove_function_t remove;
    vfs_move_function_t move;
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
};

void vfs_init(void);

bool vfs_mount_root(vfs_node_t *root);
bool vfs_mount_at(
    const char *path,
    vfs_node_t *filesystem_root
);

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

bool vfs_write_text(
    const char *path,
    const char *text
);

bool vfs_get_working_directory(
    char *buffer,
    size_t capacity
);

#endif
