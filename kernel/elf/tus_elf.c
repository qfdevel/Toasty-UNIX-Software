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
 * own address space. `argc`/`argv` are forwarded to the program
 * (argv[0] is set to the path). Returns 0 on success, a negative
 * errno otherwise.
 *
 * The task's space is created first, then CR3 is switched to it for
 * the duration of the load so el_load's segment writes land in the
 * right page tables. Preemption is disabled throughout so no tick can
 * switch tasks while we are inside a half-initialised space; the
 * kernel half is shared, so running kernel code there is safe.
 */
long elf_exec(const char *path, int argc, char **argv) {
    if (path == NULL) {
        return -EINVAL;
    }

    g_elf_fd = vfs_open(path, O_RDONLY);
    if (g_elf_fd < 0) {
        kprintf("exec: cannot open %s\n", path);
        return g_elf_fd;
    }

    /* UNIX rule: executability comes from the x permission bit, not
     * from a file extension. */
    struct vfs_node *node = vfs_lookup(path);
    if (node == NULL || (node->mode & 0111) == 0) {
        kprintf("exec: permission denied: %s\n", path);
        vfs_close(g_elf_fd);
        return -EACCES;
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
    int pid = task_create_user(ctx.ehdr.e_entry, path, cr3, argc, argv);

    /* Back to the shell's space; the new task's space is only entered
     * by the scheduler (CR3 switch on its first time slice). */
    vmm_space_switch(vmm_root_cr3());
    preempt_enable();

    if (pid < 0) {
        kprintf("exec: cannot create task for %s\n", path);
        vfs_close(g_elf_fd);
        return -ENOMEM;
    }
    /* The binary's fd is no longer needed once the task is spawned -
     * without this close every exec leaked one fd (the table is only
     * 16 slots) and later programs ran out of descriptors. */
    vfs_close(g_elf_fd);
    kprintf("exec: %s started as pid %d (entry 0x%llx, ring 3, cr3 0x%llx)\n",
            path, pid, (unsigned long long)ctx.ehdr.e_entry,
            (unsigned long long)cr3);
    return 0;
}

/* ---- execve: replace the current task's image ---- */

/* User stack layout constants (mirror kernel/sched/sched.c). */
#define EXEC_STACK_SIZE   65536
#define EXEC_USER_STACK   0x60000000ull
#define EXEC_AT_PAGESZ    6
#define EXEC_MAX_ARGS     16

/* Map a fresh user stack in `cr3` and lay out the SysV argument
 * image (argc, argv pointers, envp, auxv) the way musl's crt1
 * expects. Returns the initial RSP or 0 on failure. All writes go
 * through the HHDM mapping (the caller may be in a different space). */
static uint64_t exec_build_stack(uint64_t cr3, const char *name,
                                 int argc, char **argv) {
    uint64_t ustack = EXEC_USER_STACK;
    for (uint64_t i = 0; i < EXEC_STACK_SIZE / 4096; i++) {
        uint64_t frame = pmm_alloc_frame();
        if (frame == 0) {
            return 0;
        }
        memset((void *)pmm_phys_to_virt(frame), 0, 4096);
        if (vmm_map_page_in(cr3, ustack + i * 4096, frame,
                            VMM_PRESENT | VMM_WRITE | VMM_USER) != 0) {
            return 0;
        }
    }

    int nargs = argc + 1;
    int nwords = nargs + 6;
    if (nargs > EXEC_MAX_ARGS) {
        nargs = EXEC_MAX_ARGS;
        nwords = nargs + 6;
    }
    uint64_t top_frame = vmm_translate_in(cr3, ustack + EXEC_STACK_SIZE - 4096);
    if (top_frame == 0) {
        return 0;
    }
    char *frame_base = (char *)pmm_phys_to_virt(top_frame);

    /* Argument strings at the VERY TOP of the top page, the pointer
     * array below them, the initial RSP below that: the downward
     * growing stack can never reach the strings (see sched.c). */
    const char *args[EXEC_MAX_ARGS];
    args[0] = name != NULL ? name : "";
    for (int i = 1; i < nargs; i++) {
        args[i] = (argv != NULL && argv[i - 1] != NULL) ? argv[i - 1] : "";
    }
    size_t need = 8 + (size_t)nwords * 8;
    for (int i = 0; i < nargs; i++) {
        need += strlen(args[i]) + 1;
    }
    if (need > 4096) {
        return 0;
    }

    char *sptr = frame_base + 4096;
    uint64_t arg_ptrs[EXEC_MAX_ARGS];
    for (int i = 0; i < nargs; i++) {
        size_t slen = strlen(args[i]);
        sptr -= slen + 1;
        memcpy(sptr, args[i], slen + 1);
        arg_ptrs[i] = (uint64_t)(ustack + EXEC_STACK_SIZE - 4096) +
                      (uint64_t)(sptr - frame_base);
    }

    uint64_t *init = (uint64_t *)((uintptr_t)sptr & ~(uintptr_t)7);
    init -= nwords;
    init[0] = (uint64_t)nargs;
    for (int i = 0; i < nargs; i++) {
        init[1 + i] = arg_ptrs[i];
    }
    init[1 + nargs] = 0;        /* argv terminator */
    init[2 + nargs] = 0;        /* envp[0] */
    init[3 + nargs] = EXEC_AT_PAGESZ;
    init[4 + nargs] = 4096;     /* page size */
    init[5 + nargs] = 0;        /* auxv terminator */
    return (uint64_t)(ustack + EXEC_STACK_SIZE - 4096) +
           (uint64_t)((char *)init - frame_base);
}

/* execve(path, argv): replace the calling task's program image.
 * `path` and the `argv` strings must be kernel pointers (the syscall
 * layer copies them out of user memory first). On success this never
 * returns to the old image: the task's address space, user stack and
 * the interrupt frame on its kernel stack are rewritten, so the
 * syscall's IRETQ lands at the new entry point in ring 3.
 *
 * The old address space is intentionally leaked (the VMM has no
 * space-free primitive yet); exec is not a hot path. */
long elf_exec_current(const char *path, int argc, char **argv,
                      uint64_t frame_rsp) {
    struct task *cur = sched_current();
    if (cur == NULL || path == NULL) {
        return -EINVAL;
    }

    g_elf_fd = vfs_open(path, O_RDONLY);
    if (g_elf_fd < 0) {
        return g_elf_fd;
    }
    struct vfs_node *node = vfs_lookup(path);
    if (node == NULL || (node->mode & 0111) == 0) {
        vfs_close(g_elf_fd);
        return -EACCES; /* executability comes from the x bit */
    }

    el_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.pread = tus_pread;
    if (el_init(&ctx) != EL_OK) {
        vfs_close(g_elf_fd);
        return -ENOEXEC;
    }

    uint64_t cr3 = vmm_space_clone();
    if (cr3 == 0) {
        vfs_close(g_elf_fd);
        return -ENOMEM;
    }
    ctx.userdata = (void *)(uintptr_t)cr3;

    preempt_disable();
    vmm_space_switch(cr3);
    el_status st = el_load(&ctx, tus_alloc);
    if (st != EL_OK) {
        vmm_space_switch(vmm_root_cr3());
        preempt_enable();
        vfs_close(g_elf_fd);
        return -ENOEXEC;
    }

    uint64_t ustack_rsp = exec_build_stack(cr3, path, argc, argv);
    if (ustack_rsp == 0) {
        vmm_space_switch(vmm_root_cr3());
        preempt_enable();
        vfs_close(g_elf_fd);
        return -ENOMEM;
    }

    /* The new image is fully loaded: switch the task to it. The
     * kernel half is shared, so the remaining kernel code (and the
     * syscall epilogue) runs fine in the new space. */
    uint64_t entry = ctx.ehdr.e_entry;
    vfs_close(g_elf_fd);

    vmm_space_switch(cr3);
    cur->cr3 = cr3;
    cur->ustack = EXEC_USER_STACK;
    cur->ustack_top = EXEC_USER_STACK + EXEC_STACK_SIZE;
    cur->fs_base = 0;          /* new program sets up its own TLS */
    cur->mmap_cur = 0x40000000ull;
    cur->uid = 0;              /* execve resets to root (no model yet) */
    cur->euid = 0;
    cur->gid = 0;
    cur->egid = 0;
    const char *base = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/') {
            base = p + 1;
        }
    }
    strncpy(cur->name, base, TASK_NAME_MAX - 1);
    cur->name[TASK_NAME_MAX - 1] = '\0';

    /* Rewrite the live interrupt frame on this kernel stack (layout:
     * [.. 15 regs ..][rip cs rflags rsp ss], see syscall_entry). The
     * iretq of the syscall epilogue will land in the new program. */
    uint64_t *f = (uint64_t *)(uintptr_t)frame_rsp;
    f[0] = entry;            /* rip */
    f[1] = 0x1B;             /* cs  (0x18 | RPL 3) */
    f[2] = f[2] | 0x200;     /* rflags: keep IF */
    f[3] = ustack_rsp;       /* rsp */
    f[4] = 0x23;             /* ss  (0x20 | RPL 3) */

    preempt_enable();
    return 0;
}
