/*
 * tus_elf.c - running static ELF images on TUS
 *
 * Wires the portable elfload library into TUS:
 *   - reads the binary from the VFS (positioned reads via vfs_pread)
 *   - creates a private address space for the new task
 *     (vmm_space_clone) and maps each PT_LOAD segment there
 *   - spawns a ring-3 task in that space via the scheduler
 *
 * Only ET_EXEC images are accepted (binaries linked with `-static`).
 * Each task gets its own user half, so several images can use the
 * same link addresses without colliding. The kernel half (and with
 * it the file data, console and heap) is shared with the root space.
 */

#include "elfload.h"

#include "../core/console.h"
#include "../core/errno.h"
#include "../core/klib.h"
#include "../mm/kmalloc.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "../vfs/vfs.h"
#include "tus_elf.h"

/* File descriptor of the binary being loaded (single-threaded; the
 * whole load runs with preemption disabled). */
static long g_elf_fd = -1;

/* ---- embedded test programs ---- */

/* tests/hello.elf, tests/enforce.elf and tests/musl_hello.elf linked
 * into the kernel as binary blobs (see Makefile: *_blob.o). Exposed
 * to the VFS at /boot/ so `exec /boot/hello.elf` works out of the
 * box. */
extern char _binary_tests_hello_elf_start[];
extern char _binary_tests_hello_elf_end[];
extern char _binary_tests_enforce_elf_start[];
extern char _binary_tests_enforce_elf_end[];
extern char _binary_tests_musl_hello_elf_start[];
extern char _binary_tests_musl_hello_elf_end[];

static void elf_install_blob(const char *path, char *start, char *end) {
    size_t len = (size_t)(end - start);
    struct vfs_node *node = vfs_create_file(path);
    if (node == NULL) {
        return;
    }
    node->data = kmalloc(len);
    if (node->data == NULL) {
        return;
    }
    memcpy(node->data, start, len);
    node->size = len;
    node->capacity = len;
}

void elf_install_test_program(void) {
    elf_install_blob("/boot/hello.elf",
                     _binary_tests_hello_elf_start,
                     _binary_tests_hello_elf_end);
    elf_install_blob("/boot/enforce.elf",
                     _binary_tests_enforce_elf_start,
                     _binary_tests_enforce_elf_end);
    elf_install_blob("/boot/musl_hello.elf",
                     _binary_tests_musl_hello_elf_start,
                     _binary_tests_musl_hello_elf_end);
}

static bool tus_pread(el_ctx *ctx, void *dest, size_t nb, size_t offset) {
    (void)ctx;
    return vfs_pread(g_elf_fd, dest, nb, offset) == (long)nb;
}

/* The address space being loaded is carried in ctx->userdata. */
static uint64_t ctx_cr3(el_ctx *ctx) {
    return (uint64_t)(uintptr_t)ctx->userdata;
}

static void *tus_alloc(el_ctx *ctx, Elf_Addr phys, Elf_Addr virt,
                       Elf_Addr size) {
    (void)ctx;
    (void)phys;

    uint64_t cr3 = ctx_cr3(ctx);

    /* Map every 4 KiB page the segment touches with a fresh frame,
     * inside the new task's private address space. Pages are
     * user-accessible (VMM_USER) and the upper levels are marked USER
     * by the VMM so ring 3 can actually execute them. */
    uint64_t first = virt & ~0xFFFull;
    uint64_t last = (virt + size + 0xFFF) & ~0xFFFull;
    for (uint64_t page = first; page < last; page += 0x1000) {
        uint64_t frame = vmm_translate_in(cr3, page);
        if (frame == 0) {
            frame = pmm_alloc_frame();
            if (frame == 0) {
                return NULL;
            }
        }
        if (vmm_map_page_in(cr3, page, frame & ~0xFFFull,
                            VMM_PRESENT | VMM_WRITE | VMM_USER) != 0) {
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
 * Load the static ELF at `path` and start it as a ring-3 task in its
 * own address space. Returns 0 on success, a negative errno otherwise.
 *
 * The task's space is created first, then CR3 is switched to it for
 * the duration of the load so el_load's segment writes land in the
 * right page tables. Preemption is disabled throughout so no tick can
 * switch tasks while we are inside a half-initialised space; the
 * kernel half is shared, so running kernel code there is safe.
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

    /* Private address space for the new task. */
    uint64_t cr3 = vmm_space_clone();
    if (cr3 == 0) {
        kprintf("exec: cannot allocate address space\n");
        vfs_close(g_elf_fd);
        return -ENOMEM;
    }
    ctx.userdata = (void *)(uintptr_t)cr3;

    preempt_disable();
    vmm_space_switch(cr3);

    st = el_load(&ctx, tus_alloc);
    if (st != EL_OK) {
        elf_error("exec", st);
        vmm_space_switch(vmm_root_cr3());
        preempt_enable();
        vfs_close(g_elf_fd);
        return -ENOMEM;
    }

    /* ET_EXEC images need no relocations: skip el_relocate().
     * Spawn a ring-3 task that starts at the entry point inside this
     * address space; the scheduler runs it alongside the shell. */
    int pid = task_create_user(ctx.ehdr.e_entry, path, cr3);

    /* Back to the shell's space; the new task's space is only entered
     * by the scheduler (CR3 switch on its first time slice). */
    vmm_space_switch(vmm_root_cr3());
    preempt_enable();

    if (pid < 0) {
        kprintf("exec: cannot create task for %s\n", path);
        vfs_close(g_elf_fd);
        return -ENOMEM;
    }
    kprintf("exec: %s started as pid %d (entry 0x%llx, ring 3, cr3 0x%llx)\n",
            path, pid, (unsigned long long)ctx.ehdr.e_entry,
            (unsigned long long)cr3);
    return 0;
}
