/*
 * rootfs.c - mounting the initial ram filesystem (rootfs.img)
 *
 * rootfs.img is a ustar (POSIX) tar archive produced by the Makefile
 * from the rootfs/ staging directory:
 *
 *     tar --format=ustar -C rootfs -cf rootfs.img .
 *
 * ustar layout (every field ASCII, headers padded to 512 bytes):
 *     offset 0   name[100]
 *     offset 100 mode[8]      (octal)
 *     offset 124 size[12]     (octal, NUL/space padded)
 *     offset 136 mtime[12]    (octal, ignored)
 *     offset 148 chksum[8]    (ignored - we do not verify)
 *     offset 156 typeflag     ('0' or NUL = file, '5' = directory)
 *     offset 157 linkname[100]
 *     offset 257 magic "ustar"
 *     offset 512 file data, padded up to the next 512 boundary
 *
 * The archive ends with two consecutive zero blocks. Only regular
 * files and directories are extracted; symlinks, hardlinks and device
 * entries are skipped. Paths inside the archive are relative ("boot/
 * kilo.elf") and are mounted at the VFS root ("/boot/kilo.elf").
 */

#include "rootfs.h"

#include <stdbool.h>

#include "vfs.h"
#include "../core/klib.h"
#include "../mm/kmalloc.h"

#define TAR_BLOCK 512

/* Parse an octal number stored as ASCII in [field, field+len).
 * ustar fields are NUL/space padded; tolerate both. */
static uint64_t tar_octal(const char *field, size_t len) {
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        char c = field[i];
        if (c >= '0' && c <= '7') {
            v = (v << 3) | (uint64_t)(c - '0');
        } else if (c == ' ' || c == '\0') {
            continue;
        } else {
            break; /* non-octal garbage ends the number */
        }
    }
    return v;
}

/* Normalise an archive name into `buf`: strip the leading "./" (tar
 * emits it for the archive root), drop any trailing slashes (tar
 * writes directory entries as "boot/") and NUL-terminate. Returns
 * NULL for the archive root itself, otherwise a pointer to buf. */
static const char *tar_normalise(const char *name, char *buf, size_t buf_size) {
    if (name[0] == '.' && name[1] == '/') {
        name += 2;
    }
    size_t len = 0;
    while (name[len] != '\0' && len + 1 < buf_size) {
        len++;
    }
    while (len > 0 && name[len - 1] == '/') {
        len--; /* directory entries carry a trailing slash */
    }
    if (len == 0) {
        return NULL; /* the archive root is the VFS root */
    }
    memcpy(buf, name, len);
    buf[len] = '\0';
    return buf;
}

int vfs_mount_rootfs(const void *image, size_t size) {
    if (image == NULL || size < TAR_BLOCK * 2) {
        return -1;
    }

    const uint8_t *p = (const uint8_t *)image;
    const uint8_t *end = p + size;

    while (p + TAR_BLOCK <= end) {
        /* Two zero blocks mark the end of the archive. */
        bool zero = true;
        for (int i = 0; i < TAR_BLOCK; i++) {
            if (p[i] != 0) {
                zero = false;
                break;
            }
        }
        if (zero) {
            break;
        }

        const char *name = (const char *)p;
        char type = (char)p[156];
        uint64_t fsize = tar_octal((const char *)p + 124, 12);
        uint32_t mode = (uint32_t)tar_octal((const char *)p + 100, 8);

        char name_buf[101];
        const char *path = tar_normalise(name, name_buf, sizeof(name_buf));
        if (path != NULL) {
            if (type == '5') {
                /* Directory entry. */
                struct vfs_node *dir = vfs_create_dir(path);
                if (dir != NULL && mode != 0) {
                    dir->mode = mode;
                }
            } else if (type == '0' || type == '\0') {
                /* Regular file. */
                struct vfs_node *node = vfs_create_file(path);
                if (node == NULL) {
                    return -1; /* path collision or out of memory */
                }
                if (mode != 0) {
                    node->mode = mode;
                }
                if (fsize > 0) {
                    node->data = kmalloc((size_t)fsize);
                    if (node->data == NULL) {
                        node->size = 0;
                        return -1;
                    }
                    memcpy(node->data, p + TAR_BLOCK, (size_t)fsize);
                    node->size = (size_t)fsize;
                    node->capacity = (size_t)fsize;
                }
            }
            /* Other types (symlink '2', etc.) are skipped. */
        }

        /* Advance past the header and the padded file data. */
        uint64_t padded = (fsize + TAR_BLOCK - 1) & ~(uint64_t)(TAR_BLOCK - 1);
        p += TAR_BLOCK + padded;
        if (p > end) {
            return -1; /* truncated archive */
        }
    }

    return 0;
}
