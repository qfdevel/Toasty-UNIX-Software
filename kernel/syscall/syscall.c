/*
 * syscall.c - system call dispatch
 *
 * The IDT gate at vector 0x80 enters syscall_entry(), a tiny naked
 * stub that pushes the seven argument registers and calls
 * syscall_dispatch() with a pointer to them. The dispatch table maps
 * POSIX-style numbers onto the VFS, timer and process APIs.
 *
 * Ring-3 enforcement: the stub also records the caller's CS (pushed
 * by the CPU as part of the interrupt frame), so the dispatcher knows
 * whether the call came from user mode. User callers may only pass
 * pointers into the canonical user half; anything in the kernel half
 * is rejected with -EFAULT. (The kernel shell itself calls the same
 * ABI from ring 0 and is exempt.)
 */

#include "syscall.h"

#include <stdbool.h>

#include "../arch/x86_64/io.h"
#include "../core/bootinfo.h"
#include "../core/errno.h"
#include "../drivers/pit.h"
#include "../elf/tus_elf.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "../vfs/vfs.h"
#include "../core/klib.h"

/* Register image as pushed by syscall_entry() (lowest address first).
 * The caller's CS is passed as a separate argument to the dispatcher
 * (it lives in the CPU-pushed frame, not among the registers). */
struct syscall_regs {
    uint64_t rax; /* syscall number */
    uint64_t rdi; /* arg 1 */
    uint64_t rsi; /* arg 2 */
    uint64_t rdx; /* arg 3 */
    uint64_t r10; /* arg 4 */
    uint64_t r8;  /* arg 5 */
    uint64_t r9;  /* arg 6 (unused for now) */
};

/* Vector 0x80 gate: save the registers, dispatch, restore, iretq.
 * After the seven pushes, the CPU-pushed frame starts at %rsp+56:
 * RIP at +56, CS at +64 (valid for both ring-3 and ring-0 callers).
 * CS is handed to the dispatcher in %rsi. */
__attribute__((naked)) void syscall_entry(void) {
    __asm__ volatile(
        "push %r9\n\t"
        "push %r8\n\t"
        "push %r10\n\t"
        "push %rdx\n\t"
        "push %rsi\n\t"
        "push %rdi\n\t"
        "push %rax\n\t"
        "mov %rsp, %rdi\n\t"
        "mov 64(%rsp), %rsi\n\t"   /* CS from the interrupt frame */
        "lea 56(%rsp), %rdx\n\t"   /* pointer to the interrupt frame */
        "call syscall_dispatch\n\t"
        "add $56, %rsp\n\t"
        "iretq\n");
}

/* Upper bound of the canonical user half (0x00007fffffffffff). The
 * kernel half starts at 0xffff800000000000; non-canonical addresses
 * sit in between and are rejected too. */
#define USER_HALF_MAX 0x00007fffffffffffull

/* True when a user-mode caller may reference [ptr, ptr+len). Ring-0
 * callers (the shell) pass kernel pointers freely. */
static bool access_ok(bool from_user, const void *ptr, size_t len) {
    if (!from_user) {
        return true;
    }
    uint64_t a = (uint64_t)(uintptr_t)ptr;
    uint64_t e = a + len;
    return e >= a && e <= USER_HALF_MAX;
}

/* arch_prctl operations (Linux-compatible subset). */
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

/* IA32_FS_BASE model-specific register. */
#define MSR_FS_BASE 0xC0000100

/* mmap flags (Linux subset understood by userspace). */
#define MAP_ANONYMOUS 0x20

/* Initial address for anonymous mmap allocations. Every task has its
 * own address space, so all tasks can use the same cursor; it grows
 * upward from here (ELF images live at 0x10000000, the user stack at
 * 0x60000000, so this range is free). */
#define MMAP_CURSOR_START 0x40000000ull

/* ------------------------------------------------------------------ */

static long sys_munmap(long addr, long len);

/* execve(path, argv, envp): replace the calling task with the ELF at
 * `path`. The path and the argv strings are copied out of user memory
 * first (we are running in the caller's address space, so a bounded
 * direct read is fine after access_ok). Only user-mode callers may
 * exec; the shell spawns tasks with elf_exec() instead. On success
 * the rewritten IRETQ frame lands in the new program. */
static long sys_execve(struct syscall_regs *r, bool from_user,
                       uint64_t frame_rsp) {
    if (!from_user) {
        return -EPERM;
    }
    if (!access_ok(from_user, (const void *)r->rdi, 1)) {
        return -EFAULT;
    }

    char path[256];
    size_t i = 0;
    const char *upath = (const char *)r->rdi;
    while (i + 1 < sizeof(path)) {
        char c = upath[i];
        if (c == '\0') {
            break;
        }
        path[i++] = c;
    }
    path[i] = '\0';
    if (i == 0) {
        return -ENOENT;
    }

    char *argv[17];
    char arg_data[16][128];
    int argc = 0;
    char **uargv = (char **)r->rsi;
    if (uargv != NULL) {
        while (argc < 16) {
            if (!access_ok(from_user, (void *)(uargv + argc),
                           sizeof(char *))) {
                return -EFAULT;
            }
            const char *s = uargv[argc];
            if (s == NULL) {
                break;
            }
            if (!access_ok(from_user, s, 1)) {
                return -EFAULT;
            }
            size_t j = 0;
            while (j + 1 < sizeof(arg_data[argc])) {
                char c = s[j];
                if (c == '\0') {
                    break;
                }
                arg_data[argc][j++] = c;
            }
            arg_data[argc][j] = '\0';
            argv[argc] = arg_data[argc];
            argc++;
        }
    }
    argv[argc] = NULL;

    /* execve's argv includes the program name; elf_exec_current
     * prepends `path` as argv[0] itself, so skip it. */
    if (argc > 0) {
        return elf_exec_current(path, argc - 1, &argv[1], frame_rsp);
    }
    return elf_exec_current(path, 0, argv, frame_rsp);
}

/* exit(status): terminate the current task and switch to the next.
 * Never returns. */
__attribute__((noreturn)) static long sys_exit(int status) {
    task_exit(status);
}

/* mmap(addr, len, prot, flags): anonymous private mapping of zeroed
 * pages inside the calling task's address space. If addr is 0 a free
 * region is chosen (per-task cursor); file-backed mappings are not
 * supported yet and return -ENODEV. Returns the mapping address or a
 * negative errno. */
static long sys_mmap(long addr, long len, long prot, long flags) {
    (void)prot;
    struct task *cur = sched_current();
    if (cur == NULL || len <= 0) {
        return -EINVAL;
    }
    if ((flags & MAP_ANONYMOUS) == 0) {
        return -EINVAL; /* file-backed mmap not implemented yet */
    }

    uint64_t start = (uint64_t)addr;
    uint64_t bytes = (uint64_t)len;
    if (start == 0) {
        start = cur->mmap_cur;
    }
    if ((start & 0xFFF) != 0) {
        return -EINVAL;
    }
    bytes = (bytes + 0xFFF) & ~0xFFFull;
    if (start + bytes > USER_HALF_MAX || start + bytes < start) {
        return -ENOMEM;
    }

    for (uint64_t page = start; page < start + bytes; page += 0x1000) {
        uint64_t frame = pmm_alloc_frame();
        if (frame == 0) {
            /* Roll back what we mapped so far. */
            sys_munmap(start, (long)(page - start));
            return -ENOMEM;
        }
        /* Fresh anonymous pages must read as zeros. */
        memset((void *)pmm_phys_to_virt(frame), 0, 4096);
        if (vmm_map_page_in(cur->cr3, page, frame,
                            VMM_PRESENT | VMM_WRITE | VMM_USER) != 0) {
            pmm_free_frame(frame);
            sys_munmap(start, (long)(page - start));
            return -ENOMEM;
        }
    }
    cur->mmap_cur = start + bytes;
    return (long)start;
}

/* munmap(addr, len): unmap anonymous pages in the calling task's
 * space and return their frames to the PMM. */
static long sys_munmap(long addr, long len) {
    struct task *cur = sched_current();
    if (cur == NULL || len <= 0 || (addr & 0xFFF) != 0) {
        return -EINVAL;
    }
    uint64_t start = (uint64_t)addr;
    uint64_t bytes = ((uint64_t)len + 0xFFF) & ~0xFFFull;
    if (start + bytes > USER_HALF_MAX || start + bytes < start) {
        return -EINVAL;
    }
    for (uint64_t page = start; page < start + bytes; page += 0x1000) {
        uint64_t phys = vmm_translate_in(cur->cr3, page);
        if (phys == 0) {
            continue; /* not mapped: nothing to do */
        }
        vmm_unmap_page_in(cur->cr3, page);
        pmm_free_frame(phys & ~0xFFFull);
    }
    return 0;
}

/* arch_prctl(op, addr): thread-control operations. Only the FS base
 * (the thread pointer that the C library stores its TLS in) is
 * supported: ARCH_SET_FS writes the MSR immediately (the scheduler
 * reloads it on every task switch), ARCH_GET_FS reads it back. */
static long sys_arch_prctl(long op, long addr, bool from_user) {
    struct task *cur = sched_current();
    if (cur == NULL) {
        return -EINVAL;
    }
    if (op == ARCH_SET_FS) {
        cur->fs_base = (uint64_t)addr;
        wrmsr(MSR_FS_BASE, (uint64_t)addr);
        return 0;
    }
    if (op == ARCH_GET_FS) {
        if (!access_ok(from_user, (void *)addr, 8)) {
            return -EFAULT;
        }
        *(uint64_t *)(uintptr_t)addr = cur->fs_base;
        return 0;
    }
    return -EINVAL;
}

/* writev(fd, iov, count): scatter write. The iovec array lives in
 * user memory and each segment is validated before it is written. */
static long sys_writev(long fd, void *iov, long count, bool from_user) {
    if (count < 0 || count > 256) {
        return -EINVAL;
    }
    size_t bytes = (size_t)count * 16; /* struct iovec { base; len; } */
    if (!access_ok(from_user, iov, bytes)) {
        return -EFAULT;
    }
    long total = 0;
    for (long i = 0; i < count; i++) {
        uint64_t base = ((uint64_t *)iov)[2 * i];
        uint64_t len = ((uint64_t *)iov)[2 * i + 1];
        if (!access_ok(from_user, (void *)base, (size_t)len)) {
            return total > 0 ? total : -EFAULT;
        }
        long n = vfs_write(fd, (const void *)base, (size_t)len);
        if (n < 0) {
            return total > 0 ? total : n;
        }
        total += n;
    }
    return total;
}

long syscall_dispatch(struct syscall_regs *r, uint64_t cs, uint64_t frame_rsp) {
    bool from_user = (cs & 3) == 3;

    switch (r->rax) {
    case SYS_EXIT:
        return sys_exit((int)r->rdi);
    case SYS_EXECVE:
        return sys_execve(r, from_user, frame_rsp);
    case SYS_PIPE: {
        int fds[2];
        long ret = vfs_pipe(fds);
        if (ret < 0) {
            return ret;
        }
        if (!access_ok(from_user, (void *)r->rdi, 2 * sizeof(int))) {
            return -EFAULT;
        }
        ((int *)(uintptr_t)r->rdi)[0] = fds[0];
        ((int *)(uintptr_t)r->rdi)[1] = fds[1];
        return 0;
    }
    case SYS_DUP2:
        return vfs_dup2(r->rdi, r->rsi);
    case SYS_DUP:
        return vfs_dup(r->rdi);    case SYS_READ:
        if (!access_ok(from_user, (void *)r->rsi, (size_t)r->rdx)) {
            return -EFAULT;
        }
        return vfs_read((int)r->rdi, (void *)r->rsi, (size_t)r->rdx);
    case SYS_WRITE:
        if (!access_ok(from_user, (const void *)r->rsi, (size_t)r->rdx)) {
            return -EFAULT;
        }
        return vfs_write((int)r->rdi, (const void *)r->rsi, (size_t)r->rdx);
    case SYS_OPEN:
        if (!access_ok(from_user, (const void *)r->rdi, 1)) {
            return -EFAULT;
        }
        return vfs_open((const char *)r->rdi, (int)r->rsi);
    case SYS_CLOSE:
        return vfs_close((int)r->rdi);
    case SYS_IOCTL:
        /* The ioctl arg blob may be a termios (57 bytes), a winsize
         * (8 bytes) or a device info struct; check the worst case. */
        if (!access_ok(from_user, (void *)r->rdx, 64)) {
            return -EFAULT;
        }
        return vfs_ioctl((int)r->rdi, r->rsi, (void *)r->rdx);
    case SYS_GETPID: {
        struct task *cur = sched_current();
        return cur != NULL ? (long)cur->pid : 1;
    }
    case SYS_UPTIME:
        return (long)pit_uptime_ms();
    case SYS_SLEEP:
        timer_sleep_ms((uint32_t)r->rdi);
        return 0;
    case SYS_MKDIR:
        if (!access_ok(from_user, (const void *)r->rdi, 1)) {
            return -EFAULT;
        }
        return vfs_mkdir((const char *)r->rdi, (uint32_t)r->rsi);
    case SYS_CHMOD:
        if (!access_ok(from_user, (const void *)r->rdi, 1)) {
            return -EFAULT;
        }
        return vfs_chmod((const char *)r->rdi, (uint32_t)r->rsi);
    case SYS_UNLINK:
        if (!access_ok(from_user, (const void *)r->rdi, 1)) {
            return -EFAULT;
        }
        return vfs_unlink((const char *)r->rdi);
    case SYS_READDIR:
        if (!access_ok(from_user, (void *)r->rsi, (size_t)r->rdx)) {
            return -EFAULT;
        }
        return vfs_readdir((int)r->rdi, (void *)r->rsi, (size_t)r->rdx);
    case SYS_MMAP:
        return sys_mmap(r->rdi, r->rsi, r->rdx, r->r10);
    case SYS_MUNMAP:
        return sys_munmap(r->rdi, r->rsi);
    case SYS_ARCH_PRCTL:
        return sys_arch_prctl(r->rdi, r->rsi, from_user);
    case SYS_WRITEV:
        return sys_writev(r->rdi, (void *)r->rsi, r->rdx, from_user);
    case SYS_TIME:
        return (long)(pit_uptime_ms() / 1000);
    case SYS_FTRUNCATE:
        return vfs_ftruncate((int)r->rdi, (long)r->rsi);
    case SYS_GETUID: {
        struct task *cur = sched_current();
        return cur != NULL ? (long)cur->uid : 0;
    }
    case SYS_GETEUID: {
        struct task *cur = sched_current();
        return cur != NULL ? (long)cur->euid : 0;
    }
    case SYS_SETUID: {
        struct task *cur = sched_current();
        if (cur == NULL) {
            return -EPERM;
        }
        /* TUS has no privilege separation: any task may change its
         * ids (everything runs as root by default). */
        cur->uid = (uint32_t)r->rdi;
        cur->euid = (uint32_t)r->rdi;
        return 0;
    }
    case SYS_GETGID: {
        struct task *cur = sched_current();
        return cur != NULL ? (long)cur->gid : 0;
    }
    case SYS_SETGID: {
        struct task *cur = sched_current();
        if (cur == NULL) {
            return -EPERM;
        }
        cur->gid = (uint32_t)r->rdi;
        cur->egid = (uint32_t)r->rdi;
        return 0;
    }
    default:
        return -ENOSYS;
    }
}
