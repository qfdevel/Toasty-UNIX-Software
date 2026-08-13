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
#include "../arch/x86_64/io.h"
#include "../core/errno.h"
#include "../core/klib.h"
#include "../mm/kmalloc.h"
#include "../sched/sched.h"

/* An open file description: a reference to either a filesystem node
 * or a pipe, plus the current position. Multiple fd slots may point
 * at the same vfs_file (dup/dup2 share the position, like POSIX), so
 * it is refcounted; the last close frees it. */
struct vfs_file {
    struct vfs_node *node;
    struct vfs_pipe *pipe;
    size_t pos;
    int flags;
    int refs;
};

/* Pipe: a fixed 4 KiB ring buffer. Readers and writers block in a
 * hlt() loop while the buffer is empty/full; the 100 Hz PIT tick
 * preempts the waiter so the peer task gets CPU time. EOF is simply
 * "empty and no write end open"; -EPIPE is "write end open but no
 * read end". refs_r/refs_w count the open descriptors per end (one
 * per vfs_file referencing this pipe). */
#define PIPE_BUF_SIZE 4096

struct vfs_pipe {
    uint8_t buf[PIPE_BUF_SIZE];
    size_t head;  /* read position */
    size_t count; /* bytes buffered */
    int refs_r;
    int refs_w;
};

static struct vfs_node *g_root;

/* The fd table lives in the CURRENT task (struct task::fds). This
 * indirection is what makes redirection and pipelines possible: the
 * shell sets up its own slots, spawns a child (which inherits a
 * refcounted copy), then restores its own slots - the child keeps
 * its copy, so `cmd > file` and `a | b` just work. */
static struct vfs_file **fd_table(void) {
    struct task *cur = sched_current();
    return cur != NULL ? cur->fds : NULL;
}

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
    node->mode = 0755;
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
    node->mode = 0644;
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
    node->mode = 0600;
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

static void file_ref(struct vfs_file *f) {
    f->refs++;
    if (f->pipe != NULL) {
        if (f->flags & O_WRONLY) {
            f->pipe->refs_w++;
        } else {
            f->pipe->refs_r++;
        }
    }
}

static void file_unref(struct vfs_file *f) {
    if (f->pipe != NULL) {
        if (f->flags & O_WRONLY) {
            f->pipe->refs_w--;
        } else {
            f->pipe->refs_r--;
        }
    }
    if (--f->refs == 0) {
        /* A pipe lives as long as either end is referenced. */
        if (f->pipe != NULL && f->pipe->refs_r == 0 && f->pipe->refs_w == 0) {
            kfree(f->pipe);
        }
        kfree(f);
    }
}

/* Allocate a fresh slot (3..15; 0..2 are the standard descriptors,
 * replaced only via dup2, never by open) and take ownership. */
static long fd_alloc(struct vfs_file *f) {
    struct vfs_file **tbl = fd_table();
    if (tbl == NULL) {
        return -EBADF;
    }
    for (int i = 3; i < VFS_MAX_FDS; i++) {
        if (tbl[i] == NULL) {
            tbl[i] = f;
            return i;
        }
    }
    return -ENOMEM; /* handled by caller mapping */
}

static struct vfs_file *fd_get(long fd) {
    struct vfs_file **tbl = fd_table();
    if (tbl == NULL || fd < 0 || fd >= VFS_MAX_FDS) {
        return NULL;
    }
    return tbl[fd];
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

    struct vfs_file *f = kmalloc(sizeof(*f));
    if (f == NULL) {
        return -ENOMEM;
    }
    f->node = node;
    f->pipe = NULL;
    f->pos = 0;
    f->flags = flags;
    f->refs = 1;
    if (flags & O_APPEND) {
        f->pos = node->size; /* append: start at the end */
    }

    return fd_alloc(f);
}

long vfs_close(long fd) {
    struct vfs_file **tbl = fd_table();
    if (tbl == NULL || fd < 0 || fd >= VFS_MAX_FDS || tbl[fd] == NULL) {
        return -EBADF;
    }
    struct vfs_file *f = tbl[fd];
    tbl[fd] = NULL;
    file_unref(f);
    return 0;
}

long vfs_dup(long oldfd) {
    struct vfs_file **tbl = fd_table();
    if (tbl == NULL || oldfd < 0 || oldfd >= VFS_MAX_FDS ||
        tbl[oldfd] == NULL) {
        return -EBADF;
    }
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (tbl[i] == NULL) {
            tbl[i] = tbl[oldfd];
            file_ref(tbl[oldfd]);
            return i;
        }
    }
    return -ENOMEM;
}

long vfs_dup2(long oldfd, long newfd) {
    struct vfs_file **tbl = fd_table();
    if (tbl == NULL || oldfd < 0 || oldfd >= VFS_MAX_FDS ||
        tbl[oldfd] == NULL) {
        return -EBADF;
    }
    if (newfd < 0 || newfd >= VFS_MAX_FDS) {
        return -EBADF;
    }
    if (oldfd == newfd) {
        return newfd;
    }
    if (tbl[newfd] != NULL) {
        file_unref(tbl[newfd]); /* close the target first */
    }
    tbl[newfd] = tbl[oldfd];
    file_ref(tbl[oldfd]);
    return newfd;
}

long vfs_pipe(int fds[2]) {
    struct vfs_file **tbl = fd_table();
    if (tbl == NULL) {
        return -EBADF;
    }
    struct vfs_pipe *p = kmalloc(sizeof(*p));
    if (p == NULL) {
        return -ENOMEM;
    }
    memset(p, 0, sizeof(*p));
    p->refs_r = 1;
    p->refs_w = 1;

    struct vfs_file *r = kmalloc(sizeof(*r));
    struct vfs_file *w = kmalloc(sizeof(*w));
    if (r == NULL || w == NULL) {
        kfree(r);
        kfree(w);
        kfree(p);
        return -ENOMEM;
    }
    r->node = NULL;
    r->pipe = p;
    r->pos = 0;
    r->flags = O_RDONLY;
    r->refs = 1;
    w->node = NULL;
    w->pipe = p;
    w->pos = 0;
    w->flags = O_WRONLY;
    w->refs = 1;

    long a = fd_alloc(r);
    long b = fd_alloc(w);
    if (a < 0 || b < 0) {
        if (a >= 0) {
            tbl[a] = NULL;
        }
        if (b >= 0) {
            tbl[b] = NULL;
        }
        kfree(r);
        kfree(w);
        kfree(p);
        return -ENOMEM;
    }
    fds[0] = (int)a;
    fds[1] = (int)b;
    return 0;
}

/* Close every fd of the current task (called from task_exit; the
 * exiting task's whole table goes away, which is what lets a pipe
 * reader observe EOF once its writer exits). */
void vfs_close_all(void) {
    struct vfs_file **tbl = fd_table();
    if (tbl == NULL) {
        return;
    }
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (tbl[i] != NULL) {
            file_unref(tbl[i]);
            tbl[i] = NULL;
        }
    }
}

/* Inherit a parent's fd table at task creation (refcounted copy). */
void vfs_fd_inherit(struct vfs_file **dst, struct vfs_file **src) {
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        dst[i] = NULL;
        if (src[i] != NULL) {
            dst[i] = src[i];
            file_ref(src[i]);
        }
    }
}

/* ---- pipes ---- */

static long pipe_read(struct vfs_pipe *p, void *buf, size_t count) {
    while (p->count == 0) {
        if (p->refs_w == 0) {
            return 0; /* EOF: every write end is closed */
        }
        hlt(); /* empty: wait for the writer (the tick preempts us) */
    }
    size_t n = count < p->count ? count : p->count;
    size_t first = PIPE_BUF_SIZE - p->head;
    if (n > first) {
        memcpy(buf, p->buf + p->head, first);
        memcpy((uint8_t *)buf + first, p->buf, n - first);
    } else {
        memcpy(buf, p->buf + p->head, n);
    }
    p->head = (p->head + n) % PIPE_BUF_SIZE;
    p->count -= n;
    return (long)n;
}

static long pipe_write(struct vfs_pipe *p, const void *buf, size_t count) {
    size_t total = 0;
    while (total < count) {
        if (p->refs_r == 0) {
            return total > 0 ? (long)total : -EPIPE;
        }
        if (p->count == PIPE_BUF_SIZE) {
            hlt(); /* full: wait for the reader to drain */
            continue;
        }
        size_t free = PIPE_BUF_SIZE - p->count;
        size_t n = count - total < free ? count - total : free;
        size_t wpos = (p->head + p->count) % PIPE_BUF_SIZE;
        size_t first = PIPE_BUF_SIZE - wpos;
        if (n > first) {
            memcpy(p->buf + wpos, (const uint8_t *)buf + total, first);
            memcpy(p->buf, (const uint8_t *)buf + total + first, n - first);
        } else {
            memcpy(p->buf + wpos, (const uint8_t *)buf + total, n);
        }
        p->count += n;
        total += n;
    }
    return (long)total;
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
    if (f == NULL || f->pipe != NULL || f->node->type != VFS_FILE) {
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
    if (f->pipe != NULL) {
        return pipe_read(f->pipe, buf, count);
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
    if (f->pipe != NULL || f->node->type != VFS_FILE) {
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
    if (f->pipe != NULL) {
        return pipe_write(f->pipe, buf, count);
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
    if (f->pipe != NULL || f->node->type != VFS_DEVICE ||
        f->node->ops == NULL || f->node->ops->ioctl == NULL) {
        return -ENOTTY;
    }
    return f->node->ops->ioctl(f->node->priv, request, arg);
}

long vfs_readdir(long fd, void *buf, size_t count) {
    struct vfs_file *f = fd_get(fd);
    if (f == NULL) {
        return -EBADF;
    }
    if (f->pipe != NULL || f->node->type != VFS_DIR) {
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
        d->mode = n->mode;
        written += sizeof(struct vfs_dirent);
        f->pos++;
        n = n->sibling;
    }
    return (long)written;
}

long vfs_mkdir(const char *path, uint32_t mode) {
    if (path == NULL) {
        return -EINVAL;
    }
    struct vfs_node *existing = vfs_lookup(path);
    if (existing != NULL) {
        return -EEXIST;
    }
    struct vfs_node *dir = vfs_create_dir(path);
    if (dir == NULL) {
        return -ENOENT;
    }
    if (mode != 0) {
        dir->mode = mode;
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

long vfs_chmod(const char *path, uint32_t mode) {
    if (path == NULL) {
        return -EINVAL;
    }
    struct vfs_node *node = vfs_lookup(path);
    if (node == NULL) {
        return -ENOENT;
    }
    node->mode = mode;
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
            struct vfs_file *f = kmalloc(sizeof(*f));
            if (f == NULL) {
                continue;
            }
            f->node = tty0;
            f->pipe = NULL;
            f->pos = 0;
            f->flags = O_RDWR;
            f->refs = 1;
            struct vfs_file **tbl = fd_table();
            if (tbl == NULL) {
                kfree(f);
                continue;
            }
            tbl[i] = f;
        }
    }
}
