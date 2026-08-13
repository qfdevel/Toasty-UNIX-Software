/*
 * vfs.h - Virtual File System
 *
 * A small in-memory UNIX-like filesystem ("ramfs"): a tree of nodes
 * rooted at '/'. Nodes are directories, regular files (data held in
 * kernel memory) or device nodes (backed by a driver's file_ops).
 *
 * Access goes through file descriptors, exactly like a real UNIX
 * kernel: open() returns an fd, read/write/ioctl operate on it.
 * fds 0, 1 and 2 are pre-opened on /dev/tty0 (stdin/stdout/stderr).
 *
 * NOTE: paths are absolute ("/dev/fb0"); the shell (cmd_fs.c)
 * resolves relative paths against its working directory before
 * calling into the VFS.
 */

#ifndef TUS_VFS_VFS_H
#define TUS_VFS_VFS_H

#include <stddef.h>
#include <stdint.h>

/* Node types. */
#define VFS_DIR    1
#define VFS_FILE   2
#define VFS_DEVICE 3

/* open() flags (POSIX-ish subset). */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40
#define O_TRUNC  0x200

#define VFS_NAME_MAX 64
#define VFS_MAX_FDS  16

/* Directory entry as returned by readdir(). */
struct vfs_dirent {
    char name[VFS_NAME_MAX];
    uint32_t type;
    uint32_t size;
};

/* Device driver interface. `pos` is the fd's current position. */
struct file_ops {
    long (*read)(void *priv, void *buf, size_t count, size_t pos);
    long (*write)(void *priv, const void *buf, size_t count, size_t pos);
    int  (*ioctl)(void *priv, uint64_t request, void *arg);
};

struct vfs_node {
    char name[VFS_NAME_MAX];
    uint32_t type;

    /* Regular file contents. */
    uint8_t *data;
    size_t size;
    size_t capacity;

    /* Device backing. */
    const struct file_ops *ops;
    void *priv;

    struct vfs_node *parent;
    struct vfs_node *sibling; /* next entry in the parent */
    struct vfs_node *child;   /* first entry inside a directory */
};

/* Build the root node. The directory tree is mounted afterwards
 * from rootfs.img (vfs_mount_rootfs). */
void vfs_init(void);

/* Register the built-in device nodes and wire stdin/stdout/stderr.
 * Must run after the rootfs mount (the base directories come from
 * the image; they are recreated here only as a fallback). */
void vfs_devices_init(void);

/* ---- node creation ---- */

struct vfs_node *vfs_create_dir(const char *path);
struct vfs_node *vfs_create_file(const char *path);
struct vfs_node *vfs_create_device(const char *path,
                                   const struct file_ops *ops, void *priv);
int vfs_remove(const char *path);

/* ---- fd-based API (what the syscalls call) ---- */

long vfs_open(const char *path, int flags);
long vfs_close(long fd);
long vfs_read(long fd, void *buf, size_t count);
long vfs_write(long fd, const void *buf, size_t count);
long vfs_ioctl(long fd, uint64_t request, void *arg);
long vfs_ftruncate(long fd, long length);
long vfs_readdir(long fd, void *buf, size_t count);
long vfs_mkdir(const char *path);
long vfs_unlink(const char *path);

/* Read from a file at an explicit offset, without moving the fd's
 * position (used by the ELF loader). Returns bytes read or -errno. */
long vfs_pread(long fd, void *buf, size_t count, size_t offset);

/* Return the node for a path, or NULL. */
struct vfs_node *vfs_lookup(const char *path);

#endif /* TUS_VFS_VFS_H */
