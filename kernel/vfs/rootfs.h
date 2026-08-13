/*
 * rootfs.h - mounting the initial ram filesystem
 *
 * The root filesystem image (rootfs.img, a ustar tar archive) is
 * loaded into memory by Limine as a module and handed to the kernel.
 * vfs_mount_rootfs() parses it and populates the ramfs tree, so the
 * contents of rootfs.img (user programs under /boot, the boot logo,
 * config files...) appear exactly like files on disk.
 */

#ifndef TUS_VFS_ROOTFS_H
#define TUS_VFS_ROOTFS_H

#include <stddef.h>

/* Parse a ustar tar image in memory and merge its contents into the
 * VFS tree. Regular files and directories are supported; every other
 * entry type is skipped. Returns 0 on success, -1 on a malformed
 * image. */
int vfs_mount_rootfs(const void *image, size_t size);

#endif /* TUS_VFS_ROOTFS_H */
