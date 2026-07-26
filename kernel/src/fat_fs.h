#ifndef FAT_FS_H
#define FAT_FS_H

#include <stdbool.h>

bool fat_fs_mount_first_usb(void);
bool fat_fs_mounted(void);
void fat_fs_print_status(void);

#endif
