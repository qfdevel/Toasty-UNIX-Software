/*
 * vfs.c - Virtual File System implementation
 *
 * The tree lives entirely in kernel memory (ramfs). Path resolution
 * splits on '/' and walks child lists by name. File writes grow the
 * backing buffer with krealloc(). Device reads/writes are forwarded to
 * the node's file_ops.
 *
 * The fd table is a fixed array; entries 0..2 are the standard
 * descriptors pre-opened on /dev/tty0.
 */

#include "vfs.h"

#include <stdbool.h>

#include "devices.h"
#include "../core/errno.h"
#include "../core/klib.h"
#include "../mm/kmalloc.h"

struct vfs_file {
    struct vfs_node *node;
    size_t pos;
    int flags;
};

static struct vfs_node *g_root;
static struct vfs_file g_fds[VFS_MAX_FDS];
static bool g_fd_used[VFS_MAX_FDS];

/* ---- path helpers ---- */

/* Split "a/b/c" into parent path and final name. Returns 0 on success. */
static int path_split(const char *path, char *dir_out, size_t dir_size,
                      char *name_out, size_t name_size) {
    const char *slash = NULL;
    const char *p;
    for (p = path; *p != '\0'; p++) {
        if (*p == '/') {
            slash = p;
        }
    }
    if (slash == NULL) {
        /* No directory part: dir is "/", name is the whole path. */
        size_t nlen = strlen(path);
        if (nlen >= name_size) {
            return -1;
        }
        memcpy(name_out, path, nlen + 1);
        dir_out[0] = '/';
        dir_out[1] = '\0';
        return 0;
    }

    size_t dlen = (size_t)(slash - path);
    if (dlen == 0) {
        /* Leading slash only (e.g. "/dev"): dir is root, name is
         * everything after the slash. Do NOT copy the slash into
         * the name - lookup splits on '/' and compares components. */
        dir_out[0] = '/';
        dir_out[1] = '\0';
    } else {
        if (dlen >= dir_size) {
            return -1;
        }
        memcpy(dir_out, path, dlen);
        dir_out[dlen] = '\0';
    }

    size_t nlen = strlen(slash + 1);
    if (nlen == 0 || nlen >= name_size) {
        return -1;
    }
    memcpy(name_out, slash + 1, nlen + 1);
    return 0;
}

/* Look up a single name inside a directory. */
static struct vfs_node *dir_find(struct vfs_node *dir, const char *name) {
    for (struct vfs_node *n = dir->child; n != NULL; n = n->sibling) {
        if (strcmp(n->name, name) == 0) {
            return n;
        }
    }
    return NULL;
}

/* Attach a freshly allocated node to a directory. */
static struct vfs_node *dir_attach(struct vfs_node *dir, struct vfs_node *node) {
    node->parent = dir;
    node->sibling = dir->child;
    dir->child = node;
    return node;
}

struct vfs_node *vfs_lookup(const char *path) {
    struct vfs_node *cur = g_root;

    const char *p = path;
    while (*p == '/') {
        p++;
    }
    if (*p == '\0') {
        return g_root;
    }

    while (*p != '\0') {
        const char *end = p;
        while (*end != '\0' && *end != '/') {
            end++;
        }
        size_t len = (size_t)(end - p);
        if (len >= VFS_NAME_MAX) {
            return NULL;
        }

        if (cur->type != VFS_DIR) {
            return NULL; /* path component through a non-directory */
        }
        char name[VFS_NAME_MAX];
        memcpy(name, p, len);
        name[len] = '\0';
        cur = dir_find(cur, name);
        if (cur == NULL) {
            return NULL;
        }

        p = end;
        while (*p == '/') {
            p++;
        }
    }
    return cur;
}

/* ---- node creation ---- */

struct vfs_node *vfs_create_dir(const char *path) {
    char dir_path[256];
    char name[VFS_NAME_MAX];
    if (path_split(path, dir_path, sizeof(dir_path), name, sizeof(name)) != 0) {
        return NULL;
    }
    struct vfs_node *parent = vfs_lookup(dir_path);
    if (parent == NULL || parent->type != VFS_DIR || dir_find(parent, name) != NULL) {
        return NULL;
    }

    struct vfs_node *node = kmalloc(sizeof(*node));
    if (node == NULL) {
        return NULL;
    }
    memset(node, 0, sizeof(*node));
    node->type = VFS_DIR;
    memcpy(node->name, name, strlen(name) + 1);
    return dir_attach(parent, node);
}

struct vfs_node *vfs_create_file(const char *path) {
    char dir_path[256];
    char name[VFS_NAME_MAX];
    if (path_split(path, dir_path, sizeof(dir_path), name, sizeof(name)) != 0) {
        return NULL;
    }
    struct vfs_node *parent = vfs_lookup(dir_path);
    if (parent == NULL || parent->type != VFS_DIR || dir_find(parent, name) != NULL) {
        return NULL;
    }

    struct vfs_node *node = kmalloc(sizeof(*node));
    if (node == NULL) {
        return NULL;
    }
    memset(node, 0, sizeof(*node));
    node->type = VFS_FILE;
    memcpy(node->name, name, strlen(name) + 1);
    return dir_attach(parent, node);
}

struct vfs_node *vfs_create_device(const char *path,
                                   const struct file_ops *ops, void *priv) {
    char dir_path[256];
    char name[VFS_NAME_MAX];
    if (path_split(path, dir_path, sizeof(dir_path), name, sizeof(name)) != 0) {
        return NULL;
    }
    struct vfs_node *parent = vfs_lookup(dir_path);
    if (parent == NULL || parent->type != VFS_DIR || dir_find(parent, name) != NULL) {
        return NULL;
    }

    struct vfs_node *node = kmalloc(sizeof(*node));
    if (node == NULL) {
        return NULL;
    }
    memset(node, 0, sizeof(*node));
    node->type = VFS_DEVICE;
    node->ops = ops;
    node->priv = priv;
    memcpy(node->name, name, strlen(name) + 1);
    return dir_attach(parent, node);
}

int vfs_remove(const char *path) {
    char dir_path[256];
    char name[VFS_NAME_MAX];
    if (path_split(path, dir_path, sizeof(dir_path), name, sizeof(name)) != 0) {
        return -1;
    }
    struct vfs_node *parent = vfs_lookup(dir_path);
    if (parent == NULL || parent->type != VFS_DIR) {
        return -1;
    }

    struct vfs_node **link = &parent->child;
    while (*link != NULL) {
        if (strcmp((*link)->name, name) == 0) {
            struct vfs_node *victim = *link;
            if (victim->type == VFS_DIR && victim->child != NULL) {
                return -1; /* directory not empty */
            }
            *link = victim->sibling;
            if (victim->type == VFS_FILE) {
                kfree(victim->data);
            }
            kfree(victim);
            return 0;
        }
        link = &(*link)->sibling;
    }
    return -1;
}

/* ---- fd table ---- */

static long fd_alloc(struct vfs_node *node, int flags) {
    for (int i = 3; i < VFS_MAX_FDS; i++) { /* 0..2 are standard fds */
        if (!g_fd_used[i]) {
            g_fd_used[i] = true;
            g_fds[i].node = node;
            g_fds[i].pos = 0;
            g_fds[i].flags = flags;
            return i;
        }
    }
    return -ENOMEM; /* handled by caller mapping */
}

static struct vfs_file *fd_get(long fd) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !g_fd_used[fd]) {
        return NULL;
    }
    return &g_fds[fd];
}

/* ---- fd-based API ---- */

long vfs_open(const char *path, int flags) {
    if (path == NULL) {
        return -EINVAL;
    }
    struct vfs_node *node = vfs_lookup(path);

    if (node == NULL) {
        if (flags & O_CREAT) {
            node = vfs_create_file(path);
            if (node == NULL) {
                return -ENOENT;
            }
        } else {
            return -ENOENT;
        }
    }

    if (node->type == VFS_DIR && (flags & (O_WRONLY | O_RDWR))) {
        return -EISDIR;
    }
    if (node->type == VFS_FILE && (flags & O_TRUNC)) {
        node->size = 0;
    }

    return fd_alloc(node, flags);
}

long vfs_close(long fd) {
    if (fd_get(fd) == NULL) {
        return -EBADF;
    }
    g_fd_used[fd] = false;
    return 0;
}

static long file_read(struct vfs_file *f, void *buf, size_t count) {
    if (f->pos >= f->node->size) {
        return 0; /* EOF */
    }
    size_t avail = f->node->size - f->pos;
    if (count > avail) {
        count = avail;
    }
    memcpy(buf, f->node->data + f->pos, count);
    f->pos += count;
    return (long)count;
}

static long file_write(struct vfs_file *f, const void *buf, size_t count) {
    size_t need = f->pos + count;
    if (need > f->node->capacity) {
        size_t newcap = f->node->capacity ? f->node->capacity : 64;
        while (newcap < need) {
            newcap *= 2;
        }
        uint8_t *fresh = krealloc(f->node->data, newcap);
        if (fresh == NULL) {
            return -ENOMEM;
        }
        f->node->data = fresh;
        f->node->capacity = newcap;
    }
    memcpy(f->node->data + f->pos, buf, count);
    f->pos += count;
    if (f->pos > f->node->size) {
        f->node->size = f->pos;
    }
    return (long)count;
}

long vfs_ftruncate(long fd, long length) {
    struct vfs_file *f = fd_get(fd);
    if (f == NULL || f->node->type != VFS_FILE) {
        return -EBADF;
    }
    if (length < 0) {
        return -EINVAL;
    }
    struct vfs_node *node = f->node;
    if ((size_t)length < node->size) {
        node->size = (size_t)length;
        return 0;
    }
    /* Growing: extend with zero bytes (kilo truncates to the new
     * length, then rewrites the whole file from offset 0). */
    size_t need = (size_t)length;
    if (need > node->capacity) {
        size_t newcap = node->capacity ? node->capacity : 64;
        while (newcap < need) {
            newcap *= 2;
        }
        uint8_t *fresh = krealloc(node->data, newcap);
        if (fresh == NULL) {
            return -ENOMEM;
        }
        node->data = fresh;
        node->capacity = newcap;
    }
    if (need > node->size) {
        memset(node->data + node->size, 0, need - node->size);
        node->size = need;
    }
    return 0;
}

long vfs_read(long fd, void *buf, size_t count) {
    struct vfs_file *f = fd_get(fd);
    if (f == NULL) {
        return -EBADF;
    }
    if (buf == NULL) {
        return -EINVAL;
    }
    if ((f->flags & 3) == O_WRONLY) {
        return -EBADF;
    }

    struct vfs_node *node = f->node;
    switch (node->type) {
    case VFS_FILE:
        return file_read(f, buf, count);
    case VFS_DEVICE:
        if (node->ops != NULL && node->ops->read != NULL) {
            long n = node->ops->read(node->priv, buf, count, f->pos);
            if (n > 0) {
                f->pos += (size_t)n;
            }
            return n;
        }
        return -EIO;
    default:
        return -EISDIR;
    }
}

long vfs_pread(long fd, void *buf, size_t count, size_t offset) {
    struct vfs_file *f = fd_get(fd);
    if (f == NULL) {
        return -EBADF;
    }
    if (buf == NULL) {
        return -EINVAL;
    }
    if (f->node->type != VFS_FILE) {
        return -EISDIR; /* positioned reads only make sense for files */
    }
    if (offset >= f->node->size) {
        return 0; /* EOF */
    }
    size_t avail = f->node->size - offset;
    if (count > avail) {
        count = avail;
    }
    memcpy(buf, f->node->data + offset, count);
    return (long)count;
}

long vfs_write(long fd, const void *buf, size_t count) {
    struct vfs_file *f = fd_get(fd);
    if (f == NULL) {
        return -EBADF;
    }
    if (buf == NULL) {
        return -EINVAL;
    }
    if ((f->flags & 3) == O_RDONLY) {
        return -EBADF;
    }

    struct vfs_node *node = f->node;
    switch (node->type) {
    case VFS_FILE:
        return file_write(f, buf, count);
    case VFS_DEVICE:
        if (node->ops != NULL && node->ops->write != NULL) {
            long n = node->ops->write(node->priv, buf, count, f->pos);
            if (n > 0) {
                f->pos += (size_t)n;
            }
            return n;
        }
        return -EIO;
    default:
        return -EISDIR;
    }
}

long vfs_ioctl(long fd, uint64_t request, void *arg) {
    struct vfs_file *f = fd_get(fd);
    if (f == NULL) {
        return -EBADF;
    }
    if (f->node->type == VFS_DEVICE && f->node->ops != NULL &&
        f->node->ops->ioctl != NULL) {
        return f->node->ops->ioctl(f->node->priv, request, arg);
    }
    return -ENOTTY;
}

long vfs_readdir(long fd, void *buf, size_t count) {
    struct vfs_file *f = fd_get(fd);
    if (f == NULL) {
        return -EBADF;
    }
    if (f->node->type != VFS_DIR) {
        return -ENOTDIR;
    }
    if (buf == NULL) {
        return -EINVAL;
    }

    size_t written = 0;
    struct vfs_node *n = f->node->child;
    size_t index = 0;
    while (n != NULL && index < f->pos) {
        n = n->sibling;
        index++;
    }

    while (n != NULL && written + sizeof(struct vfs_dirent) <= count) {
        struct vfs_dirent *d = (struct vfs_dirent *)((uint8_t *)buf + written);
        memset(d, 0, sizeof(*d));
        memcpy(d->name, n->name, strlen(n->name) + 1);
        d->type = n->type;
        d->size = (uint32_t)n->size;
        written += sizeof(struct vfs_dirent);
        f->pos++;
        n = n->sibling;
    }
    return (long)written;
}

long vfs_mkdir(const char *path) {
    if (path == NULL) {
        return -EINVAL;
    }
    struct vfs_node *existing = vfs_lookup(path);
    if (existing != NULL) {
        return -EEXIST;
    }
    if (vfs_create_dir(path) == NULL) {
        return -ENOENT;
    }
    return 0;
}

long vfs_unlink(const char *path) {
    if (path == NULL) {
        return -EINVAL;
    }
    if (vfs_lookup(path) == NULL) {
        return -ENOENT;
    }
    if (vfs_remove(path) != 0) {
        return -EISDIR; /* non-empty directory or removal failure */
    }
    return 0;
}

/* ---- tree construction ---- */

/* Ensure a base directory exists. This is the safety net for a boot
 * without a rootfs module: normally /dev, /tmp, /etc and /boot all
 * come from rootfs.img (see kernel/vfs/rootfs.c), and this does
 * nothing. Only when the module is missing are the directories
 * recreated here so the system can still boot (serial-only). */
static void ensure_dir(const char *path) {
    if (vfs_lookup(path) == NULL) {
        vfs_create_dir(path);
    }
}

void vfs_init(void) {
    g_root = kmalloc(sizeof(*g_root));
    if (g_root == NULL) {
        return; /* out of memory at boot: VFS stays unusable */
    }
    memset(g_root, 0, sizeof(*g_root));
    g_root->type = VFS_DIR;
    memcpy(g_root->name, "/", 2);
}

/* Second init stage, run AFTER the rootfs module is mounted: the
 * directory tree (/dev, /tmp, /etc, /boot) comes from rootfs.img,
 * then the built-in device nodes are registered and the standard
 * descriptors are wired to the console. */
void vfs_devices_init(void) {
    /* Fallback only: normally these directories already exist
     * because rootfs.img provided them. */
    ensure_dir("/dev");
    ensure_dir("/tmp");
    ensure_dir("/etc");
    ensure_dir("/boot");

    /* Populate /dev with the built-in devices. */
    devices_init();

    /* Standard descriptors: stdin/stdout/stderr on the console. */
    struct vfs_node *tty0 = vfs_lookup("/dev/tty0");
    if (tty0 != NULL) {
        for (int i = 0; i < 3; i++) {
            g_fd_used[i] = true;
            g_fds[i].node = tty0;
            g_fds[i].pos = 0;
            g_fds[i].flags = O_RDWR;
        }
    }
}
