/*
 * tus_elf.c - running static ELF images on TUS
 *
 * Wires the portable elfload library into TUS:
 *   - reads the binary from the VFS (positioned reads via vfs_pread)
 *   - allocates each PT_LOAD segment by mapping fresh frames with the
 *     VMM at the segment's virtual address
 *   - jumps to the entry point
 *
 * Only ET_EXEC images are accepted (binaries linked with `-static`).
 * There are no user processes yet, so the image runs on the kernel
 * stack in ring 0; a future scheduler will give images their own
 * address space, stack and privilege level.
 */

#include "elfload.h"

#include "../core/console.h"
#include "../core/errno.h"
#include "../core/klib.h"
#include "../mm/kmalloc.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../vfs/vfs.h"
#include "tus_elf.h"

/* File descriptor of the binary being loaded (single-threaded). */
static long g_elf_fd = -1;

/* ---- embedded test program ---- */

/* tests/hello.elf linked into the kernel as a binary blob (see
 * Makefile: tests/hello_blob.o). Exposed to the VFS at /boot/hello.elf
 * so `exec /boot/hello.elf` works out of the box. */
extern char _binary_tests_hello_elf_start[];
extern char _binary_tests_hello_elf_end[];

void elf_install_test_program(void) {
    size_t len = (size_t)(_binary_tests_hello_elf_end -
                          _binary_tests_hello_elf_start);
    struct vfs_node *node = vfs_create_file("/boot/hello.elf");
    if (node == NULL) {
        return;
    }
    node->data = kmalloc(len);
    if (node->data == NULL) {
        return;
    }
    memcpy(node->data, _binary_tests_hello_elf_start, len);
    node->size = len;
    node->capacity = len;
}

static bool tus_pread(el_ctx *ctx, void *dest, size_t nb, size_t offset) {
    (void)ctx;
    return vfs_pread(g_elf_fd, dest, nb, offset) == (long)nb;
}

static void *tus_alloc(el_ctx *ctx, Elf_Addr phys, Elf_Addr virt,
                       Elf_Addr size) {
    (void)ctx;
    (void)phys;

    /* Map every 4 KiB page the segment touches with a fresh frame. */
    uint64_t first = virt & ~0xFFFull;
    uint64_t last = (virt + size + 0xFFF) & ~0xFFFull;
    for (uint64_t page = first; page < last; page += 0x1000) {
        /* Segments may overlap (e.g. a page shared by two PT_LOADs);
         * keep the first mapping in that case. */
        if (vmm_translate(page) != 0) {
            continue;
        }
        uint64_t frame = pmm_alloc_frame();
        if (frame == 0) {
            return NULL;
        }
        if (vmm_map_page(page, frame, VMM_PRESENT | VMM_WRITE) != 0) {
            return NULL;
        }
    }
    return (void *)(uintptr_t)virt;
}

/* Print an elfload error as a readable message. */
static void elf_error(const char *what, el_status st) {
    static const char *const names[] = {
        [EL_OK]         = "ok",
        [EL_EIO]        = "I/O error",
        [EL_ENOMEM]     = "out of memory",
        [EL_NOTELF]     = "not an ELF file",
        [EL_WRONGBITS]  = "wrong ELF class (need 64-bit)",
        [EL_WRONGENDIAN] = "wrong endianness",
        [EL_WRONGARCH]  = "wrong architecture (need x86-64)",
        [EL_WRONGOS]    = "wrong OS ABI",
        [EL_NOTEXEC]    = "not a static executable (link with -static)",
        [EL_NODYN]      = "no dynamic segment",
        [EL_BADREL]     = "unsupported relocation",
    };
    const char *name = (st >= 0 && (size_t)st < sizeof(names) / sizeof(names[0]))
        ? names[st] : "unknown error";
    kprintf("%s: %s (%d)\n", what, name, (int)st);
}

/*
 * Load the static ELF at `path` and jump to its entry point.
 * Returns 0 on success, a negative errno otherwise.
 */
long elf_exec(const char *path) {
    if (path == NULL) {
        return -EINVAL;
    }

    g_elf_fd = vfs_open(path, O_RDONLY);
    if (g_elf_fd < 0) {
        kprintf("exec: cannot open %s\n", path);
        return g_elf_fd;
    }

    el_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.pread = tus_pread;

    el_status st = el_init(&ctx);
    if (st != EL_OK) {
        elf_error("exec", st);
        vfs_close(g_elf_fd);
        return -ENOEXEC;
    }

    st = el_load(&ctx, tus_alloc);
    if (st != EL_OK) {
        elf_error("exec", st);
        vfs_close(g_elf_fd);
        return -ENOMEM;
    }

    /* ET_EXEC images need no relocations: skip el_relocate(). */
    void (*entry)(void) = (void (*)(void))(uintptr_t)ctx.ehdr.e_entry;
    kprintf("exec: running %s, entry 0x%llx\n", path,
            (unsigned long long)ctx.ehdr.e_entry);

    entry();

    kprintf("exec: %s returned\n", path);
    vfs_close(g_elf_fd);
    return 0;
}
