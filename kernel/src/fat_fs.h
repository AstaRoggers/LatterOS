#ifndef FAT_FS_H
#define FAT_FS_H

#include <stdbool.h>
#include <stdint.h>

bool fat_fs_mount_first_usb(void);
bool fat_fs_mount_first_sata(void);
bool fat_fs_mount_device_index(
    uint32_t device_index,
    const char *mount_path
);
bool fat_fs_unmount(void);
bool fat_fs_sync(void);
bool fat_fs_mounted(void);
const char *fat_fs_mount_path(void);
void fat_fs_print_status(void);
bool fat_fs_run_write_test(void);

#endif
